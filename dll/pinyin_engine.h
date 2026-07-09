// dll/pinyin_engine.h — PinyinEngine (从 main.cpp 提取, 适配 DLL)
// 核心拼音搜索引擎: 字典查询 + DP分词 + 自学习
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <algorithm>
#include "dict_client.h"
#include "../shared/pinyin_settings.h"
#include "../shared/utf_utils.h"

extern HINSTANCE g_hDllInst;

class PinyinEngine {
public:
    DictClient m_dict;
    std::unordered_map<std::string, std::vector<std::pair<std::string,int>>> m_userDict;
    std::string m_buffer;
    std::vector<std::pair<std::string,int>> m_candidates;
    int m_pageIndex = 0;
    bool m_userDictDirty = false;
    int m_selectionsSinceSave = 0;   // Auto-save after N selections
    static constexpr int AUTO_SAVE_INTERVAL = 20;
    PinyinSettings* m_pSettings = nullptr;
    // Per-keystroke cache: avoids redundant IPC queries for same syllable
    std::unordered_map<std::string, std::vector<DictClient::Entry>> m_sylCache;

    void setSettings(PinyinSettings* p) { m_pSettings = p; }

    int getPageSize() const {
        if (m_pSettings) {
            int cnt = m_pSettings->candidateCount;
            return (cnt >= 3 && cnt <= 9) ? cnt : 5;
        }
        return 5;
    }

    void init() {
        m_dict.init();
        loadUserDict();
    }

    void shutdown() {
        m_dict.shutdown();
    }

    void loadUserDict() {
        std::string dir = getModuleDirectory(g_hDllInst);
        std::string mainPath = dir + "user.dict";
        std::string backupPath = dir + "user.dict.tmp";

        // Try main file first, fall back to temp backup
        auto tryLoad = [this](const std::string& path) -> bool {
            std::ifstream fin(path);
            if (!fin.is_open()) return false;
            std::string line;
            int loaded = 0;
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
                loaded++;
            }
            return loaded > 0;  // Empty file = not valid
        };

        if (!tryLoad(mainPath)) {
            // Main file missing/corrupt — try backup
            tryLoad(backupPath);
        }
        for (auto& kv : m_userDict) {
            std::sort(kv.second.begin(), kv.second.end(),
                [](const auto& a, const auto& b){ return a.second > b.second; });
        }
    }

    void saveUserDict() {
        if (!m_userDictDirty) return;
        std::string dir = getModuleDirectory(g_hDllInst);
        std::string mainPath = dir + "user.dict";
        std::string tempPath = dir + "user.dict.tmp";

        // Write to temp file first (atomic on same filesystem)
        {
            std::ofstream fout(tempPath, std::ios::trunc);
            if (!fout.is_open()) return;
            fout << "# PinyinIME user dictionary - auto-generated\n";
            for (auto& kv : m_userDict) {
                for (auto& p : kv.second) {
                    fout << kv.first << "\t" << p.first << "\t" << p.second << "\n";
                }
            }
            if (!fout.good()) return;  // Write failed (disk full, etc.)
        }

        // Atomic rename: temp → main
        if (!MoveFileExA(tempPath.c_str(), mainPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            // Rename failed — temp file remains as backup for next load
            return;
        }
        m_userDictDirty = false;
        m_selectionsSinceSave = 0;
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
        if (m_buffer.empty()) { m_candidates.clear(); m_pageIndex = 0; }
        else updateCandidates();
    }

    std::vector<std::pair<std::string,int>> getTopCandidates(const std::string& syl, int n) {
        m_dict.trySwapBackground();  // no-op in IPC mode (service always has full dict)
        std::vector<std::pair<std::string,int>> result;

        // Per-keystroke cache: avoid redundant IPC queries for same syllable
        auto cacheIt = m_sylCache.find(syl);
        if (cacheIt != m_sylCache.end()) {
            for (auto& p : cacheIt->second)
                result.push_back({p.word, p.freq});
        } else {
            auto entries = m_dict.find(syl);
            m_sylCache[syl] = entries;  // Cache for reuse within this keystroke
            for (auto& p : entries) result.push_back({p.word, p.freq});
        }
        auto uit = m_userDict.find(syl);
        if (uit != m_userDict.end()) for (auto& p : uit->second) result.push_back(p);
        std::unordered_map<std::string, int> dedup;
        for (auto& p : result) dedup[p.first] = (std::max)(dedup[p.first], p.second);
        result.clear();
        for (auto& kv : dedup) result.push_back({kv.first, kv.second});
        std::sort(result.begin(), result.end(),
            [](const auto& a, const auto& b){ return a.second > b.second; });
        if ((int)result.size() > n) result.resize(n);
        return result;
    }

    static const std::unordered_set<std::string>& validSyllables() {
        static std::unordered_set<std::string> s;
        if (s.empty()) {
            const char* raw[] = {
                "a","ai","an","ang","ao","ba","bai","ban","bang","bao","bei","ben","beng","bi","bian","biao","bie","bin","bing","bo","bu",
                "ca","cai","can","cang","cao","ce","cen","ceng","cha","chai","chan","chang","chao","che","chen",
                "cheng","chi","chong","chou","chu","chuai","chuan","chuang","chui","chun","chuo","ci","cong","cou","cu","cuan","cui","cun","cuo",
                "da","dai","dan","dang","dao","de","dei","den","deng","di","dian","diao","die","ding","diu","dong","dou","du","duan","dui","dun","duo",
                "e","ei","en","eng","er","fa","fan","fang","fei","fen","feng","fo","fou","fu",
                "ga","gai","gan","gang","gao","ge","gei","gen","geng","gong","gou","gu","gua","guai","guan","guang","gui","gun","guo",
                "ha","hai","han","hang","hao","he","hei","hen","heng","hong","hou","hu","hua","huai","huan","huang","hui","hun","huo",
                "ji","jia","jian","jiang","jiao","jie","jin","jing","jiong","jiu","ju","juan","jue","jun",
                "ka","kai","kan","kang","kao","ke","kei","ken","keng","kong","kou","ku","kua","kuai","kuan","kuang","kui","kun","kuo",
                "la","lai","lan","lang","lao","le","lei","leng","li","lia","lian","liang","liao","lie","lin","ling","liu","long","lou","lu","luan","lun","luo","lv","lve",
                "ma","mai","man","mang","mao","me","mei","men","meng","mi","mian","miao","mie","min","ming","miu","mo","mou","mu",
                "na","nai","nan","nang","nao","ne","nei","nen","neng","ni","nian","niang","niao","nie","nin","ning","niu","nong","nou","nu","nuan","nuo","nv","nve",
                "o","ou","pa","pai","pan","pang","pao","pei","pen","peng","pi","pian","piao","pie","pin","ping","po","pou","pu",
                "qi","qia","qian","qiang","qiao","qie","qin","qing","qiong","qiu","qu","quan","que","qun",
                "ran","rang","rao","re","ren","reng","ri","rong","rou","ru","ruan","rui","run","ruo",
                "sa","sai","san","sang","sao","se","sen","seng","sha","shai","shan","shang","shao","she","shei","shen","sheng","shi","shou","shu","shua","shuai","shuan","shuang","shui","shun","shuo",
                "si","song","sou","su","suan","sui","sun","suo",
                "ta","tai","tan","tang","tao","te","tei","teng","ti","tian","tiao","tie","ting","tong","tou","tu","tuan","tui","tun","tuo",
                "wa","wai","wan","wang","wei","wen","weng","wo","wu",
                "xi","xia","xian","xiang","xiao","xie","xin","xing","xiong","xiu","xu","xuan","xue","xun",
                "ya","yan","yang","yao","ye","yi","yin","ying","yo","yong","you","yu","yuan","yue","yun",
                "za","zai","zan","zang","zao","ze","zei","zen","zeng","zha","zhai","zhan","zhang","zhao","zhe","zhei","zhen","zheng","zhi","zhong","zhou","zhu","zhua","zhuai","zhuan","zhuang","zhui","zhun","zhuo",
                "zi","zong","zou","zu","zuan","zui","zun","zuo"
            };
            for (int i = 0; i < sizeof(raw)/sizeof(raw[0]); i++) s.insert(raw[i]);
        }
        return s;
    }

    struct Segmentation {
        std::vector<std::string> sylls;
        int totalFreq = 0, minFreq = 0, count = 0;
    };

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
                topFreq = (!cands.empty()) ? cands[0].second : 1;
                for (auto& seg : dp[i]) {
                    Segmentation ns = seg;
                    ns.sylls.push_back(syl);
                    ns.totalFreq += topFreq;
                    ns.minFreq = (std::min)(seg.minFreq, topFreq);
                    ns.count = seg.count + 1;
                    dp[i + len].push_back(ns);
                }
            }
            if (dp[i].size() > 20) {
                std::sort(dp[i].begin(), dp[i].end(), [](const Segmentation& a, const Segmentation& b) {
                    // Primary: total frequency (prefer commonly-used syllables overall)
                    if (a.totalFreq != b.totalFreq) return a.totalFreq > b.totalFreq;
                    // Secondary: worst syllable frequency (break ties)
                    if (a.minFreq != b.minFreq) return a.minFreq > b.minFreq;
                    return a.count < b.count;
                });
                dp[i].resize(20);
            }
        }
        auto& full = dp[n];
        std::sort(full.begin(), full.end(), [](const Segmentation& a, const Segmentation& b) {
            if (a.totalFreq != b.totalFreq) return a.totalFreq > b.totalFreq;
            if (a.minFreq != b.minFreq) return a.minFreq > b.minFreq;
            return a.count < b.count;
        });
        if (full.size() > 24) full.resize(24);
        return full;
    }

    std::vector<std::pair<std::string,int>> genCombinedCandidates(const std::vector<Segmentation>& segs) {
        std::vector<std::pair<std::string,int>> result;
        std::unordered_set<std::string> seen;
        for (auto& seg : segs) {
            if (seg.sylls.empty()) continue;
            std::vector<std::vector<std::pair<std::string,int>>> perSyl;
            for (auto& syl : seg.sylls) {
                auto cands = getTopCandidates(syl, 5);
                if (cands.empty()) cands.push_back({syl, 1});
                perSyl.push_back(cands);
            }
            {   // Top-1 for each syllable
                std::string combined; int sumFreq = 0;
                for (size_t k = 0; k < perSyl.size(); k++) { combined += perSyl[k][0].first; sumFreq += perSyl[k][0].second; }
                // Use totalFreq directly — don't divide by count (penalizes longer correct results)
                if (seen.insert(combined).second) result.push_back({combined, sumFreq});
            }
            for (size_t v = 0; v < perSyl.size() && result.size() < 60; v++) {
                for (int ci = 1; ci < (int)perSyl[v].size() && ci <= 3; ci++) {
                    std::string combined; int sumFreq = 0;
                    for (size_t k = 0; k < perSyl.size(); k++) { int pick = (k == v) ? ci : 0; combined += perSyl[k][pick].first; sumFreq += perSyl[k][pick].second; }
                    if (seen.insert(combined).second) result.push_back({combined, sumFreq});
                }
            }
            // Two-syllable variants: try alternative for TWO syllables at once
            // characters need non-top-1 candidates to form the desired word.
            for (size_t v1 = 0; v1 < perSyl.size(); v1++) {
                for (size_t v2 = v1 + 1; v2 < perSyl.size(); v2++) {
                    for (int c1 = 1; c1 < (int)perSyl[v1].size() && c1 <= 3; c1++) {
                        for (int c2 = 1; c2 < (int)perSyl[v2].size() && c2 <= 3; c2++) {
                            std::string combined; int sumFreq = 0;
                            for (size_t k = 0; k < perSyl.size(); k++) {
                                int pick = 0;
                                if (k == v1) pick = c1;
                                else if (k == v2) pick = c2;
                                combined += perSyl[k][pick].first;
                                sumFreq += perSyl[k][pick].second;
                            }
                            if (seen.insert(combined).second) result.push_back({combined, sumFreq});
                        }
                    }
                }
            }
            // Boost: if first 2-3 syllables' concatenated pinyin matches
            // a known dict entry, boost ONLY combinations starting with
            if (seg.sylls.size() >= 2) {
                std::string pyPrefix;
                for (size_t si = 0; si < seg.sylls.size() && si < 3; ++si) {
                    pyPrefix += seg.sylls[si];
                    if (si >= 1) {
                        auto de = m_dict.find(pyPrefix);
                        if (!de.empty() && !de[0].word.empty()) {
                            const std::string& dictWord = de[0].word;
                            int boost = de[0].freq;
                            for (auto& c : result) {
                                if (c.first.compare(0, dictWord.size(), dictWord) == 0) {
                                    c.second += boost;
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b){ return a.second > b.second; });
        if (result.size() > 50) result.resize(50);
        return result;
    }

    // Generate fuzzy pinyin variants for a lookup key.
    // Returns at most one variant per enabled fuzzy group (max 7).
    std::vector<std::string> getFuzzyVariants(const std::string& key) {
        std::vector<std::string> variants;
        if (!m_pSettings) return variants;

        struct Rule { bool enabled; std::string from, to; };
        Rule rules[] = {
            {m_pSettings->fuzzyZ_Zh,  "zh", "z"},
            {m_pSettings->fuzzyZ_Zh,  "z",  "zh"},
            {m_pSettings->fuzzyC_Ch,  "ch", "c"},
            {m_pSettings->fuzzyC_Ch,  "c",  "ch"},
            {m_pSettings->fuzzyS_Sh,  "sh", "s"},
            {m_pSettings->fuzzyS_Sh,  "s",  "sh"},
            {m_pSettings->fuzzyN_L,   "n",  "l"},
            {m_pSettings->fuzzyF_H,   "f",  "h"},
            {m_pSettings->fuzzyEn_Eng,"en", "eng"},
            {m_pSettings->fuzzyIn_Ing,"in", "ing"},
        };

        for (auto& r : rules) {
            if (!r.enabled) continue;
            size_t pos = key.find(r.from);
            // For consonant pairs (n/l, f/h, z/zh, etc.), only match at syllable-start.
            // Syllable-start = position 0 or after a vowel.
            while (pos != std::string::npos) {
                bool isInitial = (pos == 0 ||
                    key[pos-1] == 'a' || key[pos-1] == 'e' || key[pos-1] == 'i' ||
                    key[pos-1] == 'o' || key[pos-1] == 'u' || key[pos-1] == 'v');
                // For eng/en and ing/in, match anywhere (they are finals)
                bool isFinal = (r.from == "en" || r.from == "eng" ||
                                r.from == "in" || r.from == "ing");
                if (isInitial || isFinal) {
                    std::string v = key;
                    v.replace(pos, r.from.size(), r.to);
                    if (v != key) variants.push_back(v);
                    break;  // One variant per rule
                }
                pos = key.find(r.from, pos + 1);
            }
        }
        // Deduplicate
        std::sort(variants.begin(), variants.end());
        variants.erase(std::unique(variants.begin(), variants.end()), variants.end());
        return variants;
    }

    void updateCandidates() {
        m_dict.trySwapBackground();  // 检查后台词库是否加载完成
        m_candidates.clear();
        m_sylCache.clear();  // Reset per-keystroke syllable cache
        m_pageIndex = 0;
        if (m_buffer.empty()) return;
        std::string lookup = m_buffer;
        for (char& ch : lookup) if (ch >= 'A' && ch <= 'Z') ch += 32;

        std::unordered_map<std::string, int> dictMatches;
        auto exactEntries = m_dict.find(lookup);
        for (auto& p : exactEntries) dictMatches[p.word] = (std::max)(dictMatches[p.word], p.freq);
        // Prefix search: for single-char queries, do deep prefix expansion;
        // for longer partial pinyin (e.g. "zhey" → "zheyang"), take more
        // entries per key to surface rich multi-character word candidates.
        { int maxDepth = 0; if (lookup.size() == 1) maxDepth = 6; else if (lookup.size() == 2) maxDepth = 5;
          int maxPerKey = (lookup.size() >= 4) ? 5 : 3;
          m_dict.prefixSearch(lookup, dictMatches, maxPerKey, maxDepth); }

        // ── Fuzzy pinyin: query variants with frequency penalty ────────
        auto fuzzyVariants = getFuzzyVariants(lookup);
        for (auto& fv : fuzzyVariants) {
            auto fe = m_dict.find(fv);
            // Apply penalty so exact matches always rank above fuzzy matches
            for (auto& p : fe) {
                int penalizedFreq = (std::max)(1, p.freq / 3);
                dictMatches[p.word] = (std::max)(dictMatches[p.word], penalizedFreq);
            }
        }

        auto uit = m_userDict.find(lookup);
        if (uit != m_userDict.end()) for (auto& p : uit->second) dictMatches[p.first] = (std::max)(dictMatches[p.first], p.second);

        std::vector<std::pair<std::string,int>> dictVec;
        for (auto& kv : dictMatches) dictVec.push_back(kv);
        std::sort(dictVec.begin(), dictVec.end(), [](const auto& a, const auto& b){ return a.second > b.second; });

        std::vector<std::pair<std::string,int>> dpVec;
        if (lookup.size() >= 3) {
            auto segs = segmentPinyin(lookup);
            if (!segs.empty()) {
                auto combined = genCombinedCandidates(segs);
                std::unordered_map<std::string, int> dpUniq;
                for (auto& c : combined) if (dictMatches.find(c.first) == dictMatches.end()) dpUniq[c.first] = (std::max)(dpUniq[c.first], c.second);
                for (auto& kv : dpUniq) dpVec.push_back(kv);
                std::sort(dpVec.begin(), dpVec.end(), [](const auto& a, const auto& b){ return a.second > b.second; });
            }
        }
        m_candidates = dictVec;
        m_candidates.insert(m_candidates.end(), dpVec.begin(), dpVec.end());
    }

    std::vector<std::pair<std::string,int>> getPageCandidates() {
        std::vector<std::pair<std::string,int>> result;
        int ps = getPageSize(), start = m_pageIndex * ps;
        for (int i = start; i < start + ps && i < (int)m_candidates.size(); i++) result.push_back(m_candidates[i]);
        return result;
    }

    void nextPage() {
        int ps = getPageSize(), maxPage = ((int)m_candidates.size() + ps - 1) / ps - 1;
        if (maxPage < 0) maxPage = 0;
        if (m_pageIndex < maxPage) m_pageIndex++;
    }

    void prevPage() {
        if (m_pageIndex > 0) m_pageIndex--;
    }

    std::string selectCandidate(int index) {
        int globalIdx = m_pageIndex * getPageSize() + index;
        if (globalIdx < 0 || globalIdx >= (int)m_candidates.size()) return "";
        auto& selected = m_candidates[globalIdx];
        std::string lowerKey = m_buffer;
        for (char& ch : lowerKey) if (ch >= 'A' && ch <= 'Z') ch += 32;
        m_userDict[lowerKey].push_back({selected.first, selected.second + 1});
        auto& vec = m_userDict[lowerKey];
        std::unordered_map<std::string, int> dedup;
        for (auto& p : vec) dedup[p.first] = (std::max)(dedup[p.first], p.second);
        vec.clear();
        for (auto& kv : dedup) vec.push_back({kv.first, kv.second});
        std::sort(vec.begin(), vec.end(), [](const auto& a, const auto& b){ return a.second > b.second; });
        m_userDictDirty = true;
        m_selectionsSinceSave++;

        // Auto-save periodically to avoid losing hours of learning on crash
        if (m_selectionsSinceSave >= AUTO_SAVE_INTERVAL) {
            saveUserDict();
        }

        std::string result = selected.first;
        clear();
        return result;
    }
};
