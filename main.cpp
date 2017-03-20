// main.cpp — 极简拼音输入法（Win32）
// 功能: 右Shift切换中英文，全局键盘钩子拦截，候选框GDI绘制，剪贴板注入，自学习词库
// 编译: cl.exe /utf-8 /O2 /MT /EHsc /DUNICODE /D_UNICODE main.cpp /Fe:PinyinIME.exe user32.lib gdi32.lib kernel32.lib

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <imm.h>
#include <oleacc.h>
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "Ole32.lib")
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <fstream>
#include <sstream>
#include "dictionary.h"

// ==================== 自定义消息 ====================
#define WM_INJECT_TEXT  (WM_USER + 1)

// ==================== 全局变量 ====================
static HINSTANCE g_hInst = nullptr;
static HWND g_hWnd = nullptr;          // 消息窗口
static HHOOK g_hHook = nullptr;        // 键盘钩子
static bool g_chineseMode = false;     // 中英文模式

// 延迟注入队列（钩子回调中不能直接 SendInput）
static std::wstring g_pendingText;
static CRITICAL_SECTION g_pendingLock;

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
    static const int PAGE_SIZE = 5;      // 每页候选数

    void init() {
        m_dict.init();
        loadUserDict();
    }

    // 加载用户自学习词库
    void loadUserDict() {
        std::ifstream fin("user.dict");
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
        std::ofstream fout("user.dict");
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

    // 更新候选列表
    void updateCandidates() {
        m_candidates.clear();
        m_pageIndex = 0;
        if (m_buffer.empty()) return;

        std::unordered_map<std::string, int> merged;

        // 1. 精确匹配（完整拼音）
        auto it = m_dict.entries.find(m_buffer);
        if (it != m_dict.entries.end()) {
            for (auto& p : it->second) {
                merged[p.first] = (std::max)(merged[p.first], p.second);
            }
        }

        // 用户词库精确匹配
        auto uit = m_userDict.find(m_buffer);
        if (uit != m_userDict.end()) {
            for (auto& p : uit->second) {
                merged[p.first] = (std::max)(merged[p.first], p.second);
            }
        }

        // 2. 前缀匹配（buffer 是 key 的前缀）
        for (auto& kv : m_dict.entries) {
            if (kv.first == m_buffer) continue;
            if (kv.first.size() >= m_buffer.size() &&
                kv.first.substr(0, m_buffer.size()) == m_buffer) {
                for (auto& p : kv.second) {
                    if (merged.find(p.first) == merged.end()) {
                        merged[p.first] = p.second - 100;
                    }
                }
            }
        }

        // 3. 首字母简拼匹配
        for (auto& kv : m_dict.entries) {
            if (kv.first == m_buffer) continue;
            if (kv.first.size() <= m_buffer.size()) continue;
            std::string initials = extractInitials(kv.first);
            if (initials == m_buffer) {
                for (auto& p : kv.second) {
                    if (merged.find(p.first) == merged.end()) {
                        merged[p.first] = p.second - 200;
                    }
                }
            }
        }

        // 转换为候选列表并排序
        for (auto& kv : merged) {
            m_candidates.push_back({kv.first, kv.second});
        }
        std::sort(m_candidates.begin(), m_candidates.end(),
            [](const auto& a, const auto& b){ return a.second > b.second; });
    }

    // 提取拼音首字母序列（简化版）
    std::string extractInitials(const std::string& pinyin) {
        std::string result;
        if (pinyin.empty()) return result;
        result += pinyin[0];
        for (size_t i = 1; i < pinyin.size(); i++) {
            char c = pinyin[i];
            char prev = pinyin[i-1];
            bool isConsonant = (c != 'a' && c != 'e' && c != 'i' && c != 'o' && c != 'u' && c != 'v');
            bool prevIsVowel = (prev == 'a' || prev == 'e' || prev == 'i' || prev == 'o' || prev == 'u' || prev == 'v');
            if (isConsonant && prevIsVowel) {
                result += c;
            }
            if ((c == 'h' && (prev == 'z' || prev == 'c' || prev == 's')) && result.size() > 1) {
                result.back() = c;
            }
        }
        return result;
    }

    std::vector<std::pair<std::string,int>> getPageCandidates() {
        std::vector<std::pair<std::string,int>> result;
        int start = m_pageIndex * PAGE_SIZE;
        for (int i = start; i < start + PAGE_SIZE && i < (int)m_candidates.size(); i++) {
            result.push_back(m_candidates[i]);
        }
        return result;
    }

    void nextPage() {
        int maxPage = ((int)m_candidates.size() + PAGE_SIZE - 1) / PAGE_SIZE - 1;
        if (maxPage < 0) maxPage = 0;
        if (m_pageIndex < maxPage) m_pageIndex++;
    }

    void prevPage() {
        if (m_pageIndex > 0) m_pageIndex--;
    }

    // 选择候选（自学习）
    std::string selectCandidate(int index) {
        int globalIdx = m_pageIndex * PAGE_SIZE + index;
        if (globalIdx < 0 || globalIdx >= (int)m_candidates.size()) return "";
        auto& selected = m_candidates[globalIdx];

        // 自学习：提升频率
        m_userDict[m_buffer].push_back({selected.first, selected.second + 1});
        auto& vec = m_userDict[m_buffer];
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

// ==================== CandidateWindow ====================
class CandidateWindow {
public:
    HWND m_hwnd = nullptr;
    HFONT m_font = nullptr;
    bool m_visible = false;

    static const COLORREF BG_COLOR = RGB(245, 245, 245);
    static const COLORREF BORDER_COLOR = RGB(180, 180, 180);
    static const COLORREF TEXT_COLOR = RGB(50, 50, 50);
    static const COLORREF INDEX_COLOR = RGB(100, 100, 200);
    static const COLORREF INPUT_COLOR = RGB(0, 100, 200);

    void create(HINSTANCE hInst) {
        m_font = CreateFontW(
            -18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
            L"Microsoft YaHei");

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wndProc;
        wc.hInstance = hInst;
        wc.hbrBackground = CreateSolidBrush(BG_COLOR);
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
        if (m_font) { DeleteObject(m_font); m_font = nullptr; }
        if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    }

    // 获取光标位置（多级降级策略）
    POINT getCaretPosition() {
        POINT pt = {0, 0};
        bool found = false;

        HWND hForeground = GetForegroundWindow();
        if (hForeground) {
            DWORD tid = GetWindowThreadProcessId(hForeground, nullptr);
            GUITHREADINFO gti = {};
            gti.cbSize = sizeof(gti);
            
            if (GetGUIThreadInfo(tid, &gti)) {
                // 1. 标准 Win32 光标
                if (gti.hwndCaret && (gti.rcCaret.right > 0 || gti.rcCaret.bottom > 0)) {
                    POINT caretPt = {gti.rcCaret.left, gti.rcCaret.bottom};
                    if (ClientToScreen(gti.hwndCaret, &caretPt)) {
                        pt = caretPt;
                        found = true;
                    }
                }
                
                // 2. MSAA 光标 (适用于 Chrome, VS Code, 新版记事本等现代应用)
                if (!found && gti.hwndFocus) {
                    IAccessible* pAcc = nullptr;
                    HRESULT hr = AccessibleObjectFromWindow(gti.hwndFocus, OBJID_CARET, IID_IAccessible, (void**)&pAcc);
                    if (SUCCEEDED(hr) && pAcc) {
                        long x = 0, y = 0, cx = 0, cy = 0;
                        VARIANT vt;
                        vt.vt = VT_I4;
                        vt.lVal = CHILDID_SELF;
                        if (pAcc->accLocation(&x, &y, &cx, &cy, vt) == S_OK) {
                            pt.x = x;
                            pt.y = y + cy; // 定位到光标底部
                            found = true;
                        }
                        pAcc->Release();
                    }
                }
            }
            
            // 3. IME 组合窗口位置 (部分中文应用支持)
            if (!found) {
                HIMC hIMC = ImmGetContext(hForeground);
                if (hIMC) {
                    COMPOSITIONFORM cf = {};
                    if (ImmGetCompositionWindow(hIMC, &cf)) {
                        POINT imePt = {cf.ptCurrentPos.x, cf.ptCurrentPos.y};
                        if (ClientToScreen(hForeground, &imePt)) {
                            pt = imePt;
                            pt.y += 20; // 行高偏移
                            found = true;
                        }
                    }
                    ImmReleaseContext(hForeground, hIMC);
                }
            }
        }

        // 4. 鼠标位置 (最终降级)
        if (!found) {
            GetCursorPos(&pt);
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

        std::wstring wpage = utf8ToWide(" -/=翻页");
        GetTextExtentPoint32W(hdc, wpage.c_str(), (int)wpage.size(), &sz);
        width += sz.cx + 20;
        ReleaseDC(m_hwnd, hdc);

        if (width > 600) width = 600;

        // 定位：光标下方
        int x = pt.x;
        int y = pt.y + 5;
        RECT screen;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &screen, 0);
        if (x + width > screen.right) x = screen.right - width;
        if (x < screen.left) x = screen.left;
        if (y + 30 > screen.bottom) y = pt.y - 35;

        SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, width, 30, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        m_visible = true;
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }

    void hide() {
        if (m_hwnd && m_visible) {
            ShowWindow(m_hwnd, SW_HIDE);
            m_visible = false;
        }
    }

    static std::wstring utf8ToWide(const std::string& s) {
        if (s.empty()) return L"";
        // 尝试用 UTF-8 解码，如果包含无效字符则失败并回退到 ANSI (GBK)
        int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, nullptr, 0);
        UINT codePage = CP_UTF8;
        if (len <= 0) {
            // 回退到系统默认 ANSI 页 (如 GBK)
            codePage = CP_ACP;
            len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
        }
        if (len <= 0) return L"";
        std::wstring result(len - 1, 0);
        MultiByteToWideChar(codePage, 0, s.c_str(), -1, &result[0], len);
        return result;
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
                HBRUSH hBrush = CreateSolidBrush(BG_COLOR);
                FillRect(hdc, &rc, hBrush);
                DeleteObject(hBrush);

                // 边框
                HPEN hPen = CreatePen(PS_SOLID, 1, BORDER_COLOR);
                HPEN oldPen = (HPEN)SelectObject(hdc, hPen);
                Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
                SelectObject(hdc, oldPen);
                DeleteObject(hPen);

                SelectObject(hdc, self->m_font);
                SetBkMode(hdc, TRANSPARENT);
                int x = 8;

                // 拼音缓冲区
                SetTextColor(hdc, INPUT_COLOR);
                std::wstring wpinyin = utf8ToWide("[" + g_engine->m_buffer + "] ");
                TextOutW(hdc, x, 6, wpinyin.c_str(), (int)wpinyin.size());
                SIZE sz;
                GetTextExtentPoint32W(hdc, wpinyin.c_str(), (int)wpinyin.size(), &sz);
                x += sz.cx + 5;

                // 候选列表
                auto candidates = g_engine->getPageCandidates();
                for (int i = 0; i < (int)candidates.size(); i++) {
                    SetTextColor(hdc, INDEX_COLOR);
                    std::wstring widx = std::to_wstring(i + 1) + L".";
                    TextOutW(hdc, x, 6, widx.c_str(), (int)widx.size());
                    GetTextExtentPoint32W(hdc, widx.c_str(), (int)widx.size(), &sz);
                    x += sz.cx;

                    SetTextColor(hdc, TEXT_COLOR);
                    std::wstring wtext = utf8ToWide(candidates[i].first);
                    TextOutW(hdc, x, 6, wtext.c_str(), (int)wtext.size());
                    GetTextExtentPoint32W(hdc, wtext.c_str(), (int)wtext.size(), &sz);
                    x += sz.cx + 8;
                }

                SetTextColor(hdc, RGB(150, 150, 150));
                std::wstring wpage = L" -/=翻页";
                TextOutW(hdc, x, 6, wpage.c_str(), (int)wpage.size());

                EndPaint(hwnd, &ps);
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

// ==================== 键盘钩子 ====================
static LRESULT CALLBACK keyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        DWORD vk = p->vkCode;

        // 忽略注入的按键
        if (p->flags & LLKHF_INJECTED) {
            return CallNextHookEx(g_hHook, nCode, wParam, lParam);
        }

        // 忽略带有控制键的组合键（Ctrl/Alt/Win），允许快捷键正常透传
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
            (GetAsyncKeyState(VK_MENU) & 0x8000) ||
            (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
            (GetAsyncKeyState(VK_RWIN) & 0x8000)) {
            return CallNextHookEx(g_hHook, nCode, wParam, lParam);
        }

        // 右Shift切换中英文
        if (vk == VK_RSHIFT) {
            g_chineseMode = !g_chineseMode;
            if (!g_chineseMode) {
                g_engine->clear();
                g_candidateWin->hide();
            }
            return 1;
        }

        if (g_chineseMode) {
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
                    requestInject(CandidateWindow::utf8ToWide(text));
                }
                return 1;
            }

            // 空格: 确认第一个候选
            if (vk == VK_SPACE && !g_engine->m_candidates.empty()) {
                std::string text = g_engine->selectCandidate(0);
                if (!text.empty()) {
                    g_candidateWin->hide();
                    requestInject(CandidateWindow::utf8ToWide(text));
                }
                return 1;
            }

            // 空格但无候选且缓冲区为空: 传递空格
            if (vk == VK_SPACE && g_engine->m_buffer.empty()) {
                return CallNextHookEx(g_hHook, nCode, wParam, lParam);
            }

            // 减号: 上一页
            if (vk == VK_OEM_MINUS) {
                if (!g_engine->m_candidates.empty()) {
                    g_engine->prevPage();
                    g_candidateWin->show(g_engine->m_buffer,
                        g_engine->getPageCandidates(), g_engine->m_pageIndex);
                    return 1;
                }
            }

            // 等号: 下一页
            if (vk == VK_OEM_PLUS) {
                if (!g_engine->m_candidates.empty()) {
                    g_engine->nextPage();
                    g_candidateWin->show(g_engine->m_buffer,
                        g_engine->getPageCandidates(), g_engine->m_pageIndex);
                    return 1;
                }
            }

            // 字母键 A-Z: 添加到拼音缓冲区
            if (vk >= 'A' && vk <= 'Z') {
                char c = (char)(vk + 32); // 统一转小写（拼音不需要大写）
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
                    requestInject(CandidateWindow::utf8ToWide(text));
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
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ==================== WinMain ====================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    g_hInst = hInstance;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitializeCriticalSection(&g_pendingLock);

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

    // 安装全局键盘钩子
    g_hHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardProc, hInstance, 0);
    if (!g_hHook) {
        MessageBoxW(nullptr, L"无法安装键盘钩子！", L"错误", MB_ICONERROR);
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
    g_engine->saveUserDict();
    g_candidateWin->destroy();
    delete g_candidateWin;
    delete g_engine;
    DeleteCriticalSection(&g_pendingLock);
    CoUninitialize();

    return 0;
}
