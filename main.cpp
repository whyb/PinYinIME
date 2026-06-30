// main.cpp — 极简拼音输入法（Win32）
// 功能: Ctrl+Shift切换中英文, 全局键盘钩子, 候选框GDI绘制, 剪贴板注入, 自学习词库, 设置窗口(纯Win32)
// 编译: 运行 build.bat 或在 VS Developer Command Prompt 中执行上面的 cl.exe 命令

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <imm.h>
#include <oleacc.h>
#include <UIAutomationClient.h>
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "UIAutomationCore.lib")
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <fstream>
#include "dictionary.h"
#include "settings.h"

// ==================== 自定义消息 ====================
#define WM_INJECT_TEXT  (WM_USER + 1)
#define WM_TRAYICON     (WM_USER + 2)
#define WM_OPEN_SETTINGS (WM_USER + 3)

// ==================== 全局变量 ====================
static HINSTANCE g_hInst = nullptr;
static HWND g_hWnd = nullptr;          // 消息窗口
static HHOOK g_hHook = nullptr;        // 键盘钩子
static bool g_chineseMode = false;     // 中英文模式
static NOTIFYICONDATAW g_trayIcon = {}; // 托盘图标

// 延迟注入队列（钩子回调中不能直接 SendInput）
static std::wstring g_pendingText;
static CRITICAL_SECTION g_pendingLock;

// ==================== 全局设置 ====================
PinyinSettings g_settings;

// ==================== 前向声明 ====================
class PinyinEngine;
class CandidateWindow;

static PinyinEngine* g_engine = nullptr;
static CandidateWindow* g_candidateWin = nullptr;

// ==================== 延迟注入 ====================
// 通过剪贴板注入文本（必须在主线程消息循环中调用，不能在钩子回调中调用）
static void doInjectText(const std::wstring& text) {
    if (text.empty()) return;

    std::vector<INPUT> inputs;
    for (wchar_t ch : text) {
        INPUT inputDown = {};
        inputDown.type = INPUT_KEYBOARD;
        inputDown.ki.wVk = 0;
        inputDown.ki.wScan = ch;
        inputDown.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(inputDown);

        INPUT inputUp = {};
        inputUp.type = INPUT_KEYBOARD;
        inputUp.ki.wVk = 0;
        inputUp.ki.wScan = ch;
        inputUp.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(inputUp);
    }
    SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
}

// 请求延迟注入（钩子回调中调用此函数）
static void requestInject(const std::wstring& text) {
    EnterCriticalSection(&g_pendingLock);
    g_pendingText = text;
    LeaveCriticalSection(&g_pendingLock);
    PostMessage(g_hWnd, WM_INJECT_TEXT, 0, 0);
}

// ==================== PinyinEngine ====================
class PinyinEngine {
public:
    PinyinDict m_dict;
    std::unordered_map<std::string, std::vector<std::pair<std::string,int>>> m_userDict;
    std::string m_buffer;                // 当前拼音缓冲区
    std::vector<std::pair<std::string,int>> m_candidates; // 当前候选列表
    int m_pageIndex = 0;                 // 当前页码

    int getPageSize() const {
        int cnt = g_settings.candidateCount;
        return (cnt >= 3 && cnt <= 9) ? cnt : 5;
    }

    void init() {
        m_dict.init();
        loadUserDict();
    }

    // 加载用户自学习词库
    void loadUserDict() {
        std::ifstream fin(getExeDirectory() + "user.dict");
        if (!fin.is_open()) return;
        std::string line;
        while (std::getline(fin, line)) {
            if (line.empty() || line[0] == '#') continue;
            size_t t1 = line.find('\t');
            size_t t2 = line.find('\t', t1 + 1);
            if (t1 == std::string::npos || t2 == std::string::npos) continue;
            std::string py = line.substr(0, t1);
            std::string txt = line.substr(t1 + 1, t2 - t1 - 1);
            int freq = 0;
            try { freq = std::stoi(line.substr(t2 + 1)); } catch (...) { continue; }
            m_userDict[py].push_back({txt, freq});
        }
        for (auto& kv : m_userDict) {
            std::sort(kv.second.begin(), kv.second.end(),
                [](const auto& a, const auto& b){ return a.second > b.second; });
        }
    }

    // 保存用户自学习词库
    void saveUserDict() {
        std::ofstream fout(getExeDirectory() + "user.dict");
        if (!fout.is_open()) return;
        fout << "# PinyinIME user dictionary - auto-generated\n";
        for (auto& kv : m_userDict) {
            for (auto& p : kv.second) {
                fout << kv.first << "\t" << p.first << "\t" << p.second << "\n";
            }
        }
    }

    void clear() {
        m_buffer.clear();
        m_candidates.clear();
        m_pageIndex = 0;
    }

    void addChar(char c) {
        m_buffer += c;
        updateCandidates();
    }

    void backspace() {
        if (m_buffer.empty()) return;
        m_buffer.pop_back();
        if (m_buffer.empty()) {
            m_candidates.clear();
            m_pageIndex = 0;
        } else {
            updateCandidates();
        }
    }

    // 获取常用候选词 (sys+user merged, sorted)
    std::vector<std::pair<std::string,int>> getTopCandidates(const std::string& syl, int n) {
        std::vector<std::pair<std::string,int>> result;
        auto* entries = m_dict.m_trie.find(syl);
        if (entries) {
            for (auto& p : *entries) result.push_back({p.word, p.freq});
        }
        auto uit = m_userDict.find(syl);
        if (uit != m_userDict.end()) {
            for (auto& p : uit->second) result.push_back(p);
        }
        // 去重取最高频
        std::unordered_map<std::string, int> dedup;
        for (auto& p : result) dedup[p.first] = (std::max)(dedup[p.first], p.second);
        result.clear();
        for (auto& kv : dedup) result.push_back({kv.first, kv.second});
        std::sort(result.begin(), result.end(),
            [](const auto& a, const auto& b){ return a.second > b.second; });
        if ((int)result.size() > n) result.resize(n);
        return result;
    }

    // 有效拼音音节集合 (约410个, 含所有标准拼音)
    static const std::unordered_set<std::string>& validSyllables() {
        static std::unordered_set<std::string> s;
        if (s.empty()) {
            const char* raw[] = {
                "a","ai","an","ang","ao",
                "ba","bai","ban","bang","bao","bei","ben","beng","bi","bian","biao","bie","bin","bing","bo","bu",
                "ca","cai","can","cang","cao","ce","cen","ceng","cha","chai","chan","chang","chao","che","chen",
                "cheng","chi","chong","chou","chu","chuai","chuan","chuang","chui","chun","chuo","ci","cong",
                "cou","cu","cuan","cui","cun","cuo",
                "da","dai","dan","dang","dao","de","dei","den","deng","di","dian","diao","die","ding","diu",
                "dong","dou","du","duan","dui","dun","duo",
                "e","ei","en","eng","er",
                "fa","fan","fang","fei","fen","feng","fo","fou","fu",
                "ga","gai","gan","gang","gao","ge","gei","gen","geng","gong","gou","gu","gua","guai","guan",
                "guang","gui","gun","guo",
                "ha","hai","han","hang","hao","he","hei","hen","heng","hong","hou","hu","hua","huai","huan",
                "huang","hui","hun","huo",
                "ji","jia","jian","jiang","jiao","jie","jin","jing","jiong","jiu","ju","juan","jue","jun",
                "ka","kai","kan","kang","kao","ke","kei","ken","keng","kong","kou","ku","kua","kuai","kuan",
                "kuang","kui","kun","kuo",
                "la","lai","lan","lang","lao","le","lei","leng","li","lia","lian","liang","liao","lie","lin",
                "ling","liu","long","lou","lu","luan","lun","luo","lv","lve",
                "ma","mai","man","mang","mao","me","mei","men","meng","mi","mian","miao","mie","min","ming",
                "miu","mo","mou","mu",
                "na","nai","nan","nang","nao","ne","nei","nen","neng","ni","nian","niang","niao","nie","nin",
                "ning","niu","nong","nou","nu","nuan","nuo","nv","nve",
                "o","ou",
                "pa","pai","pan","pang","pao","pei","pen","peng","pi","pian","piao","pie","pin","ping","po",
                "pou","pu",
                "qi","qia","qian","qiang","qiao","qie","qin","qing","qiong","qiu","qu","quan","que","qun",
                "ran","rang","rao","re","ren","reng","ri","rong","rou","ru","ruan","rui","run","ruo",
                "sa","sai","san","sang","sao","se","sen","seng","sha","shai","shan","shang","shao","she",
                "shei","shen","sheng","shi","shou","shu","shua","shuai","shuan","shuang","shui","shun","shuo",
                "si","song","sou","su","suan","sui","sun","suo",
                "ta","tai","tan","tang","tao","te","tei","teng","ti","tian","tiao","tie","ting","tong","tou",
                "tu","tuan","tui","tun","tuo",
                "wa","wai","wan","wang","wei","wen","weng","wo","wu",
                "xi","xia","xian","xiang","xiao","xie","xin","xing","xiong","xiu","xu","xuan","xue","xun",
                "ya","yan","yang","yao","ye","yi","yin","ying","yo","yong","you","yu","yuan","yue","yun",
                "za","zai","zan","zang","zao","ze","zei","zen","zeng","zha","zhai","zhan","zhang","zhao",
                "zhe","zhei","zhen","zheng","zhi","zhong","zhou","zhu","zhua","zhuai","zhuan","zhuang",
                "zhui","zhun","zhuo","zi","zong","zou","zu","zuan","zui","zun","zuo"
            };
            int n = sizeof(raw)/sizeof(raw[0]);
            for (int i = 0; i < n; i++) s.insert(raw[i]);
        }
        return s;
    }

    // 检查字符串是否都是有效拼音 (用于判断是否需要分词)
    bool isAllValidPinyin(const std::string& str) {
        const auto& vs = validSyllables();
        // 用 DP 检查整个字符串是否可被完整分割为有效拼音
        int n = (int)str.size();
        std::vector<bool> dp(n + 1, false);
        dp[0] = true;
        for (int i = 0; i < n; i++) {
            if (!dp[i]) continue;
            for (int len = 1; len <= 6 && i + len <= n; len++) {
                if (vs.count(str.substr(i, len))) dp[i + len] = true;
            }
        }
        return dp[n];
    }

    struct Segmentation {
        std::vector<std::string> sylls; // 拼音切分
        int totalFreq = 0;
        int minFreq = 0;
        int count = 0;
    };

    // DP + 束搜索: 找到拼音串的最佳切分方案
    std::vector<Segmentation> segmentPinyin(const std::string& buf) {
        int n = (int)buf.size();
        const auto& vs = validSyllables();
        std::vector<std::vector<Segmentation>> dp(n + 1);
        dp[0].push_back({{}, 0, 99999, 0});

        for (int i = 0; i < n; i++) {
            if (dp[i].empty()) continue;
            for (int len = 1; len <= 6 && i + len <= n; len++) {
                std::string syl = buf.substr(i, len);
                if (!vs.count(syl)) continue;
                int topFreq = 0;
                auto cands = getTopCandidates(syl, 1);
                if (!cands.empty()) topFreq = cands[0].second;
                else topFreq = 1; // 有音节无词条, 给最低分

                for (auto& seg : dp[i]) {
                    Segmentation ns = seg;
                    ns.sylls.push_back(syl);
                    ns.totalFreq += topFreq;
                    ns.minFreq = (std::min)(seg.minFreq, topFreq);
                    ns.count = seg.count + 1;
                    dp[i + len].push_back(ns);
                }
            }
            // 束剪枝: 每个位置保留 top 8
            if (dp[i].size() > 8) {
                std::sort(dp[i].begin(), dp[i].end(),
                    [](const Segmentation& a, const Segmentation& b) {
                        // 优先: minFreq 高, 其次 totalFreq 高, 再次音节数少
                        if (a.minFreq != b.minFreq) return a.minFreq > b.minFreq;
                        if (a.totalFreq != b.totalFreq) return a.totalFreq > b.totalFreq;
                        return a.count < b.count;
                    });
                dp[i].resize(8);
            }
        }

        auto& full = dp[n];
        std::sort(full.begin(), full.end(),
            [](const Segmentation& a, const Segmentation& b) {
                if (a.minFreq != b.minFreq) return a.minFreq > b.minFreq;
                if (a.totalFreq != b.totalFreq) return a.totalFreq > b.totalFreq;
                return a.count < b.count;
            });
        if (full.size() > 12) full.resize(12);
        return full;
    }

    // 从切分方案生成组合候选词
    std::vector<std::pair<std::string,int>> genCombinedCandidates(
            const std::vector<Segmentation>& segs) {
        std::vector<std::pair<std::string,int>> result;
        std::unordered_set<std::string> seen;

        for (auto& seg : segs) {
            if (seg.sylls.empty()) continue;
            // 获取每个音节的候选词
            std::vector<std::vector<std::pair<std::string,int>>> perSyl;
            perSyl.reserve(seg.sylls.size());
            for (auto& syl : seg.sylls) {
                auto cands = getTopCandidates(syl, 5);
                if (cands.empty()) cands.push_back({syl, 1});
                perSyl.push_back(cands);
            }

            // 策略1: 每个音节取 top-1 — 最可能的组合
            {
                std::string combined;
                int sumFreq = 0;
                for (size_t k = 0; k < perSyl.size(); k++) {
                    combined += perSyl[k][0].first;
                    sumFreq += perSyl[k][0].second;
                }
                if (seen.insert(combined).second)
                    result.push_back({combined, sumFreq / (int)perSyl.size()});
            }

            // 策略2: 轮流替换其中一个音节为第2/3候选
            for (size_t v = 0; v < perSyl.size() && result.size() < 40; v++) {
                for (int ci = 1; ci < (int)perSyl[v].size() && ci <= 3; ci++) {
                    std::string combined;
                    int sumFreq = 0;
                    for (size_t k = 0; k < perSyl.size(); k++) {
                        int pick = (k == v) ? ci : 0;
                        combined += perSyl[k][pick].first;
                        sumFreq += perSyl[k][pick].second;
                    }
                    if (seen.insert(combined).second)
                        result.push_back({combined, sumFreq / (int)perSyl.size()});
                }
            }
        }
        // 按平均频率排序
        std::sort(result.begin(), result.end(),
            [](const auto& a, const auto& b){ return a.second > b.second; });
        if (result.size() > 30) result.resize(30);
        return result;
    }

    // 更新候选列表
    void updateCandidates() {
        m_candidates.clear();
        m_pageIndex = 0;
        if (m_buffer.empty()) return;

        // 词典查询统一用小写 (buffer 可能含 Shift+字母 输入的大写)
        std::string lookup = m_buffer;
        for (char& ch : lookup) {
            if (ch >= 'A' && ch <= 'Z') ch += 32;
        }

        // 第 1 层：词典匹配（精确 + 前缀，保持原始 YAML 词频）
        //          底层使用 TrieDict 前缀树, O(L+M) 非 O(N) 全表扫描
        std::unordered_map<std::string, int> dictMatches;

        // 1a. 精确匹配: O(L) 沿 trie 走到终端节点
        auto* exactEntries = m_dict.m_trie.find(lookup);
        if (exactEntries) {
            for (auto& p : *exactEntries) {
                dictMatches[p.word] = (std::max)(dictMatches[p.word], p.freq);
            }
        }

        // 1b. 前缀匹配: O(L+M) 走到前缀节点后 DFS 收集子树词条
        //     查询用小写 key (buffer 中的大写来自 Shift+字母)
        {
            int maxDepth = 0;  // 0 = 不限制
            if (lookup.size() == 1)      maxDepth = 6;
            else if (lookup.size() == 2) maxDepth = 5;
            m_dict.m_trie.prefixSearch(lookup, dictMatches, 3, maxDepth);
        }

        // 用户词库精确匹配 (也用小写 key)
        auto uit = m_userDict.find(lookup);
        if (uit != m_userDict.end()) {
            for (auto& p : uit->second) {
                dictMatches[p.first] = (std::max)(dictMatches[p.first], p.second);
            }
        }

        // 将词典匹配按词频降序排列（精确匹配和前缀匹配统一按权重排序）
        std::vector<std::pair<std::string,int>> dictVec;
        for (auto& kv : dictMatches) {
            dictVec.push_back(kv);
        }
        std::sort(dictVec.begin(), dictVec.end(),
            [](const auto& a, const auto& b){ return a.second > b.second; });

        // 第 2 层：DP 拼音分词组合
        //          例如 "haiyoumeiyou" → hai+you+meiyou → "还有没有"
        std::vector<std::pair<std::string,int>> dpVec;
        if (lookup.size() >= 3) {
            auto segs = segmentPinyin(lookup);
            if (!segs.empty()) {
                auto combined = genCombinedCandidates(segs);
                std::unordered_map<std::string, int> dpUniq;
                for (auto& c : combined) {
                    // 跳过已在词典匹配中出现的词
                    if (dictMatches.find(c.first) != dictMatches.end()) continue;
                    dpUniq[c.first] = (std::max)(dpUniq[c.first], c.second);
                }
                for (auto& kv : dpUniq) {
                    dpVec.push_back(kv);
                }
                std::sort(dpVec.begin(), dpVec.end(),
                    [](const auto& a, const auto& b){ return a.second > b.second; });
            }
        }

        // 合并：词典匹配在前，DP 分词组合在后
        m_candidates = dictVec;
        m_candidates.insert(m_candidates.end(), dpVec.begin(), dpVec.end());
    }

    std::vector<std::pair<std::string,int>> getPageCandidates() {
        std::vector<std::pair<std::string,int>> result;
        int pageSize = getPageSize();
        int start = m_pageIndex * pageSize;
        for (int i = start; i < start + pageSize && i < (int)m_candidates.size(); i++) {
            result.push_back(m_candidates[i]);
        }
        return result;
    }

    void nextPage() {
        int pageSize = getPageSize();
        int maxPage = ((int)m_candidates.size() + pageSize - 1) / pageSize - 1;
        if (maxPage < 0) maxPage = 0;
        if (m_pageIndex < maxPage) m_pageIndex++;
    }

    void prevPage() {
        if (m_pageIndex > 0) m_pageIndex--;
    }

    // 选择候选（自学习）
    std::string selectCandidate(int index) {
        int globalIdx = m_pageIndex * getPageSize() + index;
        if (globalIdx < 0 || globalIdx >= (int)m_candidates.size()) return "";
        auto& selected = m_candidates[globalIdx];

        // 自学习：提升频率 (user dict key 统一用小写)
        std::string lowerKey = m_buffer;
        for (char& ch : lowerKey) {
            if (ch >= 'A' && ch <= 'Z') ch += 32;
        }
        m_userDict[lowerKey].push_back({selected.first, selected.second + 1});
        auto& vec = m_userDict[lowerKey];
        std::unordered_map<std::string, int> dedup;
        for (auto& p : vec) dedup[p.first] = (std::max)(dedup[p.first], p.second);
        vec.clear();
        for (auto& kv : dedup) vec.push_back({kv.first, kv.second});
        std::sort(vec.begin(), vec.end(),
            [](const auto& a, const auto& b){ return a.second > b.second; });

        saveUserDict();

        std::string result = selected.first;
        clear();
        return result;
    }
};

// 前向声明 settings 相关 (在 settings.h include 之前)
struct PinyinSettings;
extern PinyinSettings g_settings;

// ==================== CandidateWindow ====================
class CandidateWindow {
public:
    HWND m_hwnd = nullptr;
    HFONT m_font = nullptr;
    bool m_visible = false;
    RECT m_settingsBtnRect = {}; // 设置按钮点击区域
    int m_textY = 6;            // 文本 Y 坐标 (根据字体度量动态计算)
    int m_rowH = 24;            // 竖排模式每行高度 (根据字体度量动态计算)
    HRGN m_roundRgn = nullptr;  // 圆角窗口区域
    int m_roundR = 10;          // 圆角半径

    COLORREF getBgColor()     { return g_settings.bgColor; }
    COLORREF getBorderColor() { return g_settings.borderColor; }
    COLORREF getTextColor()   { return g_settings.textColor; }
    COLORREF getIndexColor()  { return g_settings.indexColor; }
    COLORREF getInputColor()  { return g_settings.inputColor; }

    void create(HINSTANCE hInst) {
        m_font = CreateFontW(
            -g_settings.fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
            g_settings.fontName.c_str());

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(getBgColor());
        wc.lpszClassName = L"PinyinIMECandidate";
        RegisterClassExW(&wc);

        m_hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"PinyinIMECandidate", L"",
            WS_POPUP,
            0, 0, 400, 30,
            nullptr, nullptr, hInst, nullptr);

        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);
    }

    void destroy() {
        if (m_roundRgn) { DeleteObject(m_roundRgn); m_roundRgn = nullptr; }
        if (m_font) { DeleteObject(m_font); m_font = nullptr; }
        if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    }

    // 获取文本光标位置（多级降级 — UI Automation → MSAA → Win32 → IME → 窗口 → 屏幕）
    POINT getCaretPosition() {
        POINT pt = {0, 0};
        bool found = false;

        // === 1. UI Automation — 现代应用首选 (Chrome/Edge/VSCode/Office/UWP 等) ===
        IUIAutomation* pUIA = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pUIA));
        if (SUCCEEDED(hr) && pUIA) {
            IUIAutomationElement* pFocused = nullptr;
            hr = pUIA->GetFocusedElement(&pFocused);
            if (SUCCEEDED(hr) && pFocused) {
                // 1a. TextPattern: 获取精确文本光标
                IUIAutomationTextPattern2* pText2 = nullptr;
                hr = pFocused->GetCurrentPatternAs(UIA_TextPattern2Id,
                    IID_PPV_ARGS(&pText2));
                if (SUCCEEDED(hr) && pText2) {
                    BOOL isActive = FALSE;
                    IUIAutomationTextRange* pCaret = nullptr;
                    if (SUCCEEDED(pText2->GetCaretRange(&isActive, &pCaret)) && pCaret) {
                        SAFEARRAY* pRectArray = nullptr;
                        if (SUCCEEDED(pCaret->GetBoundingRectangles(&pRectArray)) && pRectArray) {
                            double* pData = nullptr;
                            if (SUCCEEDED(SafeArrayAccessData(pRectArray, (void**)&pData))) {
                                long ub;
                                SafeArrayGetUBound(pRectArray, 1, &ub);
                                if (ub >= 3) { // [x, y, w, h]
                                    pt.x = (LONG)pData[0];
                                    pt.y = (LONG)(pData[1] + pData[3]); // 光标底部
                                    found = true;
                                }
                                SafeArrayUnaccessData(pRectArray);
                            }
                            SafeArrayDestroy(pRectArray);
                        }
                        pCaret->Release();
                    }
                    pText2->Release();
                }

                // 1b. 备选: TextPattern GetSelection
                if (!found) {
                    IUIAutomationTextPattern* pText = nullptr;
                    hr = pFocused->GetCurrentPatternAs(UIA_TextPatternId,
                        IID_PPV_ARGS(&pText));
                    if (SUCCEEDED(hr) && pText) {
                        IUIAutomationTextRangeArray* pSelection = nullptr;
                        if (SUCCEEDED(pText->GetSelection(&pSelection)) && pSelection) {
                            IUIAutomationTextRange* pRange = nullptr;
                            if (SUCCEEDED(pSelection->GetElement(0, &pRange)) && pRange) {
                                SAFEARRAY* pRectArray = nullptr;
                                if (SUCCEEDED(pRange->GetBoundingRectangles(&pRectArray)) && pRectArray) {
                                    double* pData = nullptr;
                                    if (SUCCEEDED(SafeArrayAccessData(pRectArray, (void**)&pData))) {
                                        long ub;
                                        SafeArrayGetUBound(pRectArray, 1, &ub);
                                        if (ub >= 3) {
                                            pt.x = (LONG)pData[0];
                                            pt.y = (LONG)(pData[1] + pData[3]);
                                            found = true;
                                        }
                                        SafeArrayUnaccessData(pRectArray);
                                    }
                                    SafeArrayDestroy(pRectArray);
                                }
                                pRange->Release();
                            }
                            pSelection->Release();
                        }
                        pText->Release();
                    }
                }

                // 1c. 兜底: 焦点控件自身边界框
                if (!found) {
                    RECT rc = {};
                    if (SUCCEEDED(pFocused->get_CurrentBoundingRectangle(&rc)) &&
                        (rc.right > rc.left || rc.bottom > rc.top)) {
                        pt.x = rc.left;
                        pt.y = rc.bottom + 4;
                        found = true;
                    }
                }
                pFocused->Release();
            }
            pUIA->Release();
        }

        // === 2. Win32 / MSAA / IME / 窗口 降级链 ===
        if (!found) {
            HWND hForeground = GetForegroundWindow();
            if (hForeground) {
                DWORD tid = GetWindowThreadProcessId(hForeground, nullptr);
                GUITHREADINFO gti = {};
                gti.cbSize = sizeof(gti);

                if (GetGUIThreadInfo(tid, &gti)) {
                    // 2a. Win32 标准光标
                    if (gti.hwndCaret && (gti.rcCaret.right > 0 || gti.rcCaret.bottom > 0)) {
                        POINT caretPt = {gti.rcCaret.left, gti.rcCaret.bottom};
                        if (ClientToScreen(gti.hwndCaret, &caretPt)) {
                            pt = caretPt;
                            found = true;
                        }
                    }

                    // 2b. MSAA 文本光标
                    if (!found && gti.hwndFocus) {
                        IAccessible* pAcc = nullptr;
                        HRESULT hr2 = AccessibleObjectFromWindow(gti.hwndFocus,
                            OBJID_CARET, IID_IAccessible, (void**)&pAcc);
                        if (SUCCEEDED(hr2) && pAcc) {
                            long x = 0, y = 0, cx = 0, cy = 0;
                            VARIANT vt; vt.vt = VT_I4; vt.lVal = CHILDID_SELF;
                            if (pAcc->accLocation(&x, &y, &cx, &cy, vt) == S_OK && (cx > 0 || cy > 0)) {
                                pt.x = x;
                                pt.y = y + cy;
                                found = true;
                            }
                            pAcc->Release();
                        }
                    }

                    // 2c. MSAA 焦点控件位置
                    if (!found && gti.hwndFocus) {
                        IAccessible* pAcc = nullptr;
                        HRESULT hr2 = AccessibleObjectFromWindow(gti.hwndFocus,
                            OBJID_CLIENT, IID_IAccessible, (void**)&pAcc);
                        if (FAILED(hr2) || !pAcc) {
                            hr2 = AccessibleObjectFromWindow(gti.hwndFocus,
                                OBJID_WINDOW, IID_IAccessible, (void**)&pAcc);
                        }
                        if (SUCCEEDED(hr2) && pAcc) {
                            long x = 0, y = 0, cx = 0, cy = 0;
                            VARIANT vt; vt.vt = VT_I4; vt.lVal = CHILDID_SELF;
                            if (pAcc->accLocation(&x, &y, &cx, &cy, vt) == S_OK && (cx > 0 || cy > 0)) {
                                pt.x = x;
                                pt.y = y + cy + 4;
                                found = true;
                            }
                            pAcc->Release();
                        }
                    }
                }

                // 3. IME 组合窗口
                if (!found) {
                    HIMC hIMC = ImmGetContext(hForeground);
                    if (hIMC) {
                        COMPOSITIONFORM cf = {};
                        if (ImmGetCompositionWindow(hIMC, &cf)) {
                            POINT imePt = {cf.ptCurrentPos.x, cf.ptCurrentPos.y};
                            if (ClientToScreen(hForeground, &imePt)) {
                                pt = imePt;
                                pt.y += 20;
                                found = true;
                            }
                        }
                        ImmReleaseContext(hForeground, hIMC);
                    }
                }

                // 4. 前台窗口左下区域 (离焦点最近的位置决不是鼠标)
                if (!found) {
                    RECT fgRect;
                    if (GetWindowRect(hForeground, &fgRect)) {
                        pt.x = fgRect.left + 40;
                        pt.y = fgRect.bottom - 60;
                        found = true;
                    }
                }
            }
        }

        // 5. 屏幕工作区 (最终保底)
        if (!found) {
            RECT screen;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &screen, 0);
            pt.x = screen.left + 200;
            pt.y = screen.bottom - 120;
        }

        return pt;
    }

    // 显示候选框
    void show(const std::string& pinyinBuffer,
              const std::vector<std::pair<std::string,int>>& candidates,
              int pageIndex) {
        if (!m_hwnd) return;

        POINT pt = getCaretPosition();

        // 计算窗口宽度
        HDC hdc = GetDC(m_hwnd);
        SelectObject(hdc, m_font);

        // 从字体度量计算动态窗口高度和文本 Y 坐标
        TEXTMETRICW tm;
        GetTextMetrics(hdc, &tm);

        int width = 20;
        SIZE sz;

        std::wstring wpinyin = utf8ToWide("[" + pinyinBuffer + "] ");
        GetTextExtentPoint32W(hdc, wpinyin.c_str(), (int)wpinyin.size(), &sz);
        width += sz.cx + 10;

        for (int i = 0; i < (int)candidates.size(); i++) {
            std::wstring wtext = utf8ToWide(
                std::to_string(i + 1) + "." + candidates[i].first + " ");
            GetTextExtentPoint32W(hdc, wtext.c_str(), (int)wtext.size(), &sz);
            width += sz.cx;
        }

        std::wstring wpage = utf8ToWide(" -/=/PgUp/PgDn翻页  ⚙");
        GetTextExtentPoint32W(hdc, wpage.c_str(), (int)wpage.size(), &sz);
        width += sz.cx + 20;
        ReleaseDC(m_hwnd, hdc);

        // 获取屏幕工作区 (用于限制宽度不超过屏幕)
        RECT screen;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &screen, 0);
        int maxWidth = (screen.right - screen.left) * 85 / 100;  // 不超过屏幕 85%
        if (width > maxWidth) width = maxWidth;

        // 竖排模式: 拼音行 + 每个候选一行 + 翻页/齿轮行
        int height;
        int borderW = 3;  // 渐变边框宽度 (3 层 RoundRect → 软边缘效果)
        m_roundR = (std::max)(6, (std::min)(16, (int)(tm.tmHeight * 2 / 3)));  // 圆角半径

        if (g_settings.verticalLayout && !candidates.empty()) {
            if (250 > maxWidth) width = maxWidth;  // 竖排也不超出屏幕
            m_rowH = tm.tmHeight + 6;   // 字符高度 + 行间距
            m_textY = borderW + 4;      // 顶部边距
            // 布局: [拼音] / 候选1 / 候选2 / ... / 翻页⚙
            // 行数 = 1(拼音) + candidates + 1(翻页)
            height = m_textY + ((int)candidates.size() + 2) * m_rowH + borderW + 6;
        } else {
            // 横排模式: 根据字体大小动态计算高度
            int pad = (std::max)(4, (int)(tm.tmHeight / 8));
            m_textY = borderW + pad;    // 边框 + 内边距 = 字符顶部
            height = m_textY + tm.tmHeight + pad + borderW;
            m_rowH = tm.tmHeight;
        }

        // 创建圆角窗口区域 (裁剪尖锐边角)
        if (m_roundRgn) { DeleteObject(m_roundRgn); m_roundRgn = nullptr; }
        m_roundRgn = CreateRoundRectRgn(0, 0, width + 1, height + 1, m_roundR * 2, m_roundR * 2);
        SetWindowRgn(m_hwnd, m_roundRgn, TRUE);

        // 定位：光标下方
        int x = pt.x;
        int y = pt.y + 5;
        if (x + width > screen.right) x = screen.right - width;
        if (x < screen.left) x = screen.left;
        if (y + height > screen.bottom) y = pt.y - height - 5;

        SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        m_visible = true;
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }

    void hide() {
        if (m_hwnd && m_visible) {
            ShowWindow(m_hwnd, SW_HIDE);
            m_visible = false;
        }
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_PAINT) {
            CandidateWindow* self = (CandidateWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (self && self->m_visible && g_engine) {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                RECT rc;
                GetClientRect(hwnd, &rc);

                // 背景
                HBRUSH hBrush = CreateSolidBrush(self->getBgColor());
                FillRect(hdc, &rc, hBrush);
                DeleteObject(hBrush);

                // 渐变圆角边框 (3 层 → 软边缘效果)
                {
                    COLORREF bc = self->getBorderColor();
                    int rr = GetRValue(bc), rg = GetGValue(bc), rb = GetBValue(bc);
                    int r = rc.right, b = rc.bottom;
                    int cr = self->m_roundR;  // 圆角半径
                    HBRUSH nullBr = (HBRUSH)GetStockObject(NULL_BRUSH);

                    // 确定渐变方向: 暗色背景→外浅内深, 亮色背景→外深内浅
                    COLORREF bg = self->getBgColor();
                    int bgBright = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000;
                    int dir = (bgBright < 128) ? 1 : -1;  // 1=外浅, -1=外深

                    auto clampC = [](int v) -> int { return v < 0 ? 0 : (v > 255 ? 255 : v); };

                    // 第 1 层: 最外层 (最浅/最深)
                    HPEN p1 = CreatePen(PS_SOLID, 1, RGB(clampC(rr + 40 * dir), clampC(rg + 40 * dir), clampC(rb + 40 * dir)));
                    SelectObject(hdc, p1); SelectObject(hdc, nullBr);
                    RoundRect(hdc, 0, 0, r, b, cr * 2, cr * 2);

                    // 第 2 层: 中间层
                    HPEN p2 = CreatePen(PS_SOLID, 1, RGB(clampC(rr + 18 * dir), clampC(rg + 18 * dir), clampC(rb + 18 * dir)));
                    SelectObject(hdc, p2); SelectObject(hdc, nullBr);
                    RoundRect(hdc, 1, 1, r - 1, b - 1, (cr - 1) * 2, (cr - 1) * 2);

                    // 第 3 层: 内层 (原始边框颜色)
                    HPEN p3 = CreatePen(PS_SOLID, 1, bc);
                    SelectObject(hdc, p3); SelectObject(hdc, nullBr);
                    RoundRect(hdc, 2, 2, r - 2, b - 2, (cr - 2) * 2, (cr - 2) * 2);

                    DeleteObject(p3); DeleteObject(p2); DeleteObject(p1);
                }

                SelectObject(hdc, self->m_font);
                SetBkMode(hdc, TRANSPARENT);
                int x = 8;
                int y = self->m_textY;

                // 拼音缓冲区
                SetTextColor(hdc, self->getInputColor());
                std::wstring wpinyin = utf8ToWide("[" + g_engine->m_buffer + "] ");
                TextOutW(hdc, x, y, wpinyin.c_str(), (int)wpinyin.size());
                SIZE sz;
                GetTextExtentPoint32W(hdc, wpinyin.c_str(), (int)wpinyin.size(), &sz);
                x += sz.cx + 5;

                // 候选列表 (竖排模式从拼音行下一行开始)
                auto candidates = g_engine->getPageCandidates();
                int candBaseY = g_settings.verticalLayout ? (y + self->m_rowH) : y;
                for (int i = 0; i < (int)candidates.size(); i++) {
                    int cy = g_settings.verticalLayout ? (candBaseY + i * self->m_rowH) : y;

                    SetTextColor(hdc, self->getIndexColor());
                    std::wstring widx = std::to_wstring(i + 1) + L".";
                    TextOutW(hdc, x, cy, widx.c_str(), (int)widx.size());
                    GetTextExtentPoint32W(hdc, widx.c_str(), (int)widx.size(), &sz);
                    int cw = sz.cx;

                    SetTextColor(hdc, self->getTextColor());
                    std::wstring wtext = utf8ToWide(candidates[i].first);
                    TextOutW(hdc, x + cw, cy, wtext.c_str(), (int)wtext.size());
                    GetTextExtentPoint32W(hdc, wtext.c_str(), (int)wtext.size(), &sz);

                    if (!g_settings.verticalLayout) {
                        x += cw + sz.cx + 8;
                    }
                }

                // 翻页提示 + 齿轮
                SetTextColor(hdc, RGB(150, 150, 150));
                std::wstring wpage = L" -/=/PgUp/PgDn翻页";
                int pageY = y;
                int pageX = x;
                if (g_settings.verticalLayout) {
                    // 竖排模式: 在所有候选下方单独一行 (拼音占用首行)
                    pageY = candBaseY + (int)candidates.size() * self->m_rowH + 2;
                    pageX = 8;
                }
                TextOutW(hdc, pageX, pageY, wpage.c_str(), (int)wpage.size());
                GetTextExtentPoint32W(hdc, wpage.c_str(), (int)wpage.size(), &sz);

                // 齿轮紧跟翻页提示
                SetTextColor(hdc, RGB(80, 80, 200));
                std::wstring wgear = L"⚙";
                int gearX = pageX + sz.cx + 4;
                TextOutW(hdc, gearX, pageY, wgear.c_str(), (int)wgear.size());
                SIZE gearSz;
                GetTextExtentPoint32W(hdc, wgear.c_str(), (int)wgear.size(), &gearSz);
                self->m_settingsBtnRect = {gearX, pageY, gearX + gearSz.cx + 4, pageY + gearSz.cy};

                EndPaint(hwnd, &ps);
            }
            return 0;
        }
        if (msg == WM_LBUTTONDOWN) {
            CandidateWindow* self = (CandidateWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (self) {
                POINT pt = {LOWORD(lp), HIWORD(lp)};
                if (PtInRect(&self->m_settingsBtnRect, pt)) {
                    PostMessage(g_hWnd, WM_OPEN_SETTINGS, 0, 0);
                }
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

// ==================== 设置系统接口实现 ====================
// (这些函数在 PinyinEngine/CandidateWindow 完整定义之后实现)

void openSettingsWindow(HINSTANCE hInst, HWND hParent) {
    SettingsWindow::show(hInst, hParent);
}

// 用户词典操作 (由 settings.h 中的 UserDictDialog 调用)
void userDictAddEntry(const std::string& pinyin, const std::string& word, int freq) {
    if (g_engine) {
        g_engine->m_userDict[pinyin].push_back({word, freq});
        g_engine->saveUserDict();
    }
}

void userDictRemoveEntry(const std::string& pinyin, const std::string& word) {
    if (!g_engine) return;
    auto it = g_engine->m_userDict.find(pinyin);
    if (it != g_engine->m_userDict.end()) {
        auto& vec = it->second;
        vec.erase(std::remove_if(vec.begin(), vec.end(),
            [&word](const auto& p){ return p.first == word; }), vec.end());
        if (vec.empty()) g_engine->m_userDict.erase(it);
    }
    g_engine->saveUserDict();
}

void userDictClearAll() {
    if (g_engine) {
        g_engine->m_userDict.clear();
        g_engine->saveUserDict();
    }
}

std::string userDictGetAllAsString() {
    if (!g_engine) return "";
    std::string result;
    for (auto& kv : g_engine->m_userDict) {
        for (auto& p : kv.second) {
            result += kv.first + "\t" + p.first + "\t" + std::to_string(p.second) + "\n";
        }
    }
    return result;
}

void userDictSaveToFile() {
    if (g_engine) g_engine->saveUserDict();
}

void applySettingsToEngine(const PinyinSettings& s) {
    if (g_candidateWin) {
        // 1. 更新字体（字号可能改变）
        if (g_candidateWin->m_font) DeleteObject(g_candidateWin->m_font);
        g_candidateWin->m_font = CreateFontW(
            -s.fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
            s.fontName.c_str());

        // 2. 如果候选框正在显示，立即刷新以反映新设置
        //    (颜色/字体/竖排布局/候选数量等)
        if (g_candidateWin->m_visible && g_engine && !g_engine->m_buffer.empty()) {
            g_candidateWin->show(g_engine->m_buffer,
                g_engine->getPageCandidates(), g_engine->m_pageIndex);
        }
    }
}

// ==================== 键盘钩子 ====================
static LRESULT CALLBACK keyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        DWORD vk = p->vkCode;

        // 忽略注入的按键
        if (p->flags & LLKHF_INJECTED) {
            return CallNextHookEx(g_hHook, nCode, wParam, lParam);
        }

        // --- 读取修饰键状态 ---
        bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool altDown  = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
        bool winDown  = (GetAsyncKeyState(VK_LWIN)    & 0x8000) != 0
                     || (GetAsyncKeyState(VK_RWIN)    & 0x8000) != 0;

        // --- 切换中英文热键 (支持配置修饰键) ---
        bool isToggleKey = false;
        if (g_settings.toggleModifier == 0) {
            // 无修饰键模式: 精确匹配 vk
            isToggleKey = (vk == g_settings.toggleHotkey);
        } else {
            // 有修饰键模式: Shift 键匹配任一 Shift; 其他键精确匹配
            if (g_settings.toggleHotkey == VK_SHIFT) {
                isToggleKey = (vk == VK_LSHIFT || vk == VK_RSHIFT);
            } else {
                isToggleKey = (vk == g_settings.toggleHotkey);
            }
        }

        bool modifierOk = false;
        if (g_settings.toggleModifier == 0) {
            modifierOk = !ctrlDown && !altDown && !winDown;
        } else if (g_settings.toggleModifier == VK_MENU) {
            modifierOk = altDown && !ctrlDown && !winDown;
        } else if (g_settings.toggleModifier == VK_CONTROL) {
            modifierOk = ctrlDown && !altDown && !winDown;
        }

        if (isToggleKey && modifierOk) {
            g_chineseMode = !g_chineseMode;
            if (!g_chineseMode) {
                g_engine->clear();
                g_candidateWin->hide();
            }
            return 1;
        }

        // --- 透传含 Ctrl/Win 的组合键 ---
        if (ctrlDown || winDown) {
            return CallNextHookEx(g_hHook, nCode, wParam, lParam);
        }

        // --- 透传含 Alt 但非切换热键的组合 ---
        if (altDown) {
            return CallNextHookEx(g_hHook, nCode, wParam, lParam);
        }

        if (g_chineseMode) {
            // Enter: 上屏原始拼音字母
            if (vk == VK_RETURN && !g_engine->m_buffer.empty()) {
                requestInject(utf8ToWide(g_engine->m_buffer));
                g_engine->clear();
                g_candidateWin->hide();
                return 1;
            }

            // Shift（无修饰键）: 上屏原始拼音字母
            if ((vk == VK_LSHIFT || vk == VK_RSHIFT)
                && !g_engine->m_buffer.empty()) {
                requestInject(utf8ToWide(g_engine->m_buffer));
                g_engine->clear();
                g_candidateWin->hide();
                return 1;
            }

            // Escape: 清空缓冲区
            if (vk == VK_ESCAPE) {
                if (!g_engine->m_buffer.empty()) {
                    g_engine->clear();
                    g_candidateWin->hide();
                    return 1;
                }
                return CallNextHookEx(g_hHook, nCode, wParam, lParam);
            }

            // Backspace: 删除最后一个拼音字母
            if (vk == VK_BACK) {
                if (!g_engine->m_buffer.empty()) {
                    g_engine->backspace();
                    if (g_engine->m_buffer.empty()) {
                        g_candidateWin->hide();
                    } else {
                        g_candidateWin->show(g_engine->m_buffer,
                            g_engine->getPageCandidates(), g_engine->m_pageIndex);
                    }
                    return 1;
                }
                return CallNextHookEx(g_hHook, nCode, wParam, lParam);
            }

            // 数字键 1-9: 选择候选
            if (vk >= '1' && vk <= '9' && !g_engine->m_candidates.empty()) {
                int idx = (int)(vk - '1');
                std::string text = g_engine->selectCandidate(idx);
                if (!text.empty()) {
                    g_candidateWin->hide();
                    requestInject(utf8ToWide(text));
                }
                return 1;
            }

            // 空格: 确认第一个候选
            if (vk == VK_SPACE && !g_engine->m_candidates.empty()) {
                std::string text = g_engine->selectCandidate(0);
                if (!text.empty()) {
                    g_candidateWin->hide();
                    requestInject(utf8ToWide(text));
                }
                return 1;
            }

            // 空格但无候选且缓冲区为空: 传递空格
            if (vk == VK_SPACE && g_engine->m_buffer.empty()) {
                return CallNextHookEx(g_hHook, nCode, wParam, lParam);
            }

            // 减号 / PageUp: 上一页
            if (vk == VK_OEM_MINUS || vk == VK_PRIOR) {
                if (!g_engine->m_candidates.empty()) {
                    g_engine->prevPage();
                    g_candidateWin->show(g_engine->m_buffer,
                        g_engine->getPageCandidates(), g_engine->m_pageIndex);
                    return 1;
                }
            }

            // 等号 / PageDown: 下一页
            if (vk == VK_OEM_PLUS || vk == VK_NEXT) {
                if (!g_engine->m_candidates.empty()) {
                    g_engine->nextPage();
                    g_candidateWin->show(g_engine->m_buffer,
                        g_engine->getPageCandidates(), g_engine->m_pageIndex);
                    return 1;
                }
            }

            // 字母键 A-Z: 添加到拼音缓冲区
            if (vk >= 'A' && vk <= 'Z') {
                bool capsLock = (GetKeyState(VK_CAPITAL) & 1) != 0;
                bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

                // Caps Lock 模式下: 直接透传英文大写, 不弹出候选窗
                if (capsLock) {
                    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
                }

                // Shift+字母: 保留大写显示在缓冲区, 查词时忽略大小写
                // 普通字母: 小写
                char c = shiftDown ? (char)vk : (char)(vk + 32);
                g_engine->addChar(c);
                g_candidateWin->show(g_engine->m_buffer,
                    g_engine->getPageCandidates(), g_engine->m_pageIndex);
                return 1;
            }

            // 其他键: 如果有候选，先确认第一个，再传递该键
            if (!g_engine->m_buffer.empty() && !g_engine->m_candidates.empty()) {
                std::string text = g_engine->selectCandidate(0);
                if (!text.empty()) {
                    g_candidateWin->hide();
                    requestInject(utf8ToWide(text));
                }
                // 延迟传递当前键（等注入完成后再发）
                // 简化：直接传递，注入通过 PostMessage 异步执行
                return CallNextHookEx(g_hHook, nCode, wParam, lParam);
            }

            // 有缓冲区但无候选
            if (!g_engine->m_buffer.empty()) {
                g_engine->clear();
                g_candidateWin->hide();
                return CallNextHookEx(g_hHook, nCode, wParam, lParam);
            }
        }
    }

    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}

// ==================== 主窗口过程 ====================
static LRESULT CALLBACK mainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INJECT_TEXT: {
        // 延迟注入：钩子已返回，现在可以安全地操作剪贴板和 SendInput
        EnterCriticalSection(&g_pendingLock);
        std::wstring text = g_pendingText;
        g_pendingText.clear();
        LeaveCriticalSection(&g_pendingLock);
        if (!text.empty()) {
            doInjectText(text);
        }
        return 0;
    }
    case WM_TRAYICON: {
        if (wp == 1 && lp == WM_RBUTTONUP) {
            // 托盘右键菜单
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, 1, L"⚙ 设置");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, 2, L"❌ 退出");
            SetForegroundWindow(hwnd); // 确保菜单能正确关闭
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
                pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);
            if (cmd == 1) {
                PostMessage(hwnd, WM_OPEN_SETTINGS, 0, 0);
            } else if (cmd == 2) {
                PostQuitMessage(0);
            }
        } else if (wp == 1 && lp == WM_LBUTTONDBLCLK) {
            // 双击托盘图标打开设置
            PostMessage(hwnd, WM_OPEN_SETTINGS, 0, 0);
        }
        return 0;
    }
    case WM_OPEN_SETTINGS: {
        openSettingsWindow(g_hInst, hwnd);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ==================== WinMain ====================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    // ── 命令行模式: --register-system (UAC 提权后静默注册) ──
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(lpCmdLine, &argc);
        if (argv) {
            for (int i = 0; i < argc; i++) {
                if (wcscmp(argv[i], L"--register-system") == 0) {
                    // 静默注册 + 退出
                    bool ok = registerIMEToSystem(true);
                    LocalFree(argv);
                    return ok ? 0 : 1;
                }
            }
            LocalFree(argv);
        }
    }

    g_hInst = hInstance;

    SetProcessDPIAware();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitializeCriticalSection(&g_pendingLock);

    // 初始化 Common Controls (用于设置窗口)
    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icex);

    // 加载设置 (使用 exe 所在目录，保证设置文件始终与程序在同一位置)
    if (!g_settings.loadFromFile(getExeDirectory() + "pinyin_config.ini")) {
        // 首次运行: 默认 Ctrl+Shift 切换中英文
        g_settings.toggleModifier = VK_CONTROL;
        g_settings.toggleHotkey = VK_SHIFT;
    } else if (!g_settings.hasToggleModifierInFile) {
        // 旧版本配置文件迁移: 没有 toggleModifier 字段 → 升级为 Ctrl+Shift
        g_settings.toggleModifier = VK_CONTROL;
        g_settings.toggleHotkey = VK_SHIFT;
        g_settings.saveToFile(getExeDirectory() + "pinyin_config.ini");
    }

    // 初始化拼音引擎
    g_engine = new PinyinEngine();
    g_engine->init();

    // 初始化候选窗口
    g_candidateWin = new CandidateWindow();
    g_candidateWin->create(hInstance);

    // 创建消息窗口
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = mainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"PinyinIMEMain";
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(0, L"PinyinIMEMain", L"PinyinIME",
        0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);

    // 创建托盘图标
    g_trayIcon.cbSize = sizeof(NOTIFYICONDATAW);
    g_trayIcon.hWnd = g_hWnd;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = WM_TRAYICON;
    g_trayIcon.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_trayIcon.szTip, L"PinyinIME 拼音输入法");
    Shell_NotifyIconW(NIM_ADD, &g_trayIcon);

    // 安装全局键盘钩子
    g_hHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardProc, hInstance, 0);
    if (!g_hHook) {
        MessageBoxW(nullptr, L"无法安装键盘钩子！", L"错误", MB_ICONERROR);
        Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
        CoUninitialize();
        return 1;
    }

    // 消息循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 清理
    if (g_hHook) UnhookWindowsHookEx(g_hHook);
    Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
    g_engine->saveUserDict();
    g_candidateWin->destroy();
    delete g_candidateWin;
    delete g_engine;
    DeleteCriticalSection(&g_pendingLock);
    CoUninitialize();

    return 0;
}
