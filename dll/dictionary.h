// dictionary.h — 极简拼音输入法词库加载器
// 直接读取 rime-ice 原版 YAML 词库文件 (cn_dicts/*.dict.yaml)
// 底层使用 TrieDict 前缀树存储, 支持 O(L+M) 前缀查找
// 无需任何预处理，开箱即用
//
// 启动优化: 同步加载字表 (~550KB, 毫秒级), 后台线程异步加载完整词库 (~48MB)
// 输入法启动后立即可用, 词库在后台逐步扩充

#ifndef DICTIONARY_H
#define DICTIONARY_H

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <fstream>
#include <cctype>
#include <atomic>
#include "trie_dict.h"
#include "shm_dict.h"
#include "../shared/ime_ipc.h"

// 获取 DLL 所在的文件夹路径（带末尾反斜杠）
// 用 g_hDllInst 而非 nullptr，因为 DLL 加载到其他进程时 nullptr 返回宿主 EXE 的路径
extern HINSTANCE g_hDllInst;

inline std::string getDictDirectory() {
    char path[MAX_PATH] = { 0 };
    GetModuleFileNameA(g_hDllInst, path, MAX_PATH);
    std::string dllPath(path);
    size_t pos = dllPath.find_last_of("\\/");
    if (pos != std::string::npos) {
        return dllPath.substr(0, pos + 1);
    }
    return "";
}

// UTF-8 辅助: 获取字符串的 Unicode 字符数
inline int utf8CharCount(const std::string& s) {
    int count = 0;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0x80) == 0)      i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else i += 1;
        count++;
    }
    return count;
}

// UTF-8 辅助: 将字符串按 Unicode 字符拆分为 vector
inline std::vector<std::string> utf8SplitChars(const std::string& s) {
    std::vector<std::string> chars;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        if ((c & 0x80) == 0)      len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        chars.push_back(s.substr(i, len));
        i += len;
    }
    return chars;
}

// ── 字符→拼音映射类型 ──
using CharPinyinMap = std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>;

struct PinyinDict {
    // ── 主词库: 前缀树存储 (替代 unordered_map, O(L+M) 前缀查找) ──
    TrieDict m_trie;

    // ── 后台加载中的完整词库 ──
    TrieDict m_trieBackground;

    // ── 字符→拼音映射表 (从字表构建，用于给 tencent 等无拼音词库自动注音) ──
    CharPinyinMap charToPinyins;

    // ── 后台加载状态 ──
    std::atomic<bool> m_backgroundReady{false};
    HANDLE m_hLoadThread = nullptr;

    // ── 共享内存模式 ──
    bool     m_sharedMode       = false;   // true = 使用共享内存 FlatTrie
    FlatTrie m_shmTrie;                    // 共享内存查询接口
    HANDLE   m_hSharedMapping   = nullptr; // 文件映射句柄
    void*    m_pSharedBase       = nullptr; // MapViewOfFile 基址
    HANDLE   m_hDictMutex       = nullptr; // 词库初始化互斥锁 (仅创建者持有)
    LONG     m_shmStateLastCheck = DICT_STATE_EMPTY;

    // ── 用字符映射表为词语生成拼音 ──
    static std::string generatePinyin(const std::string& word, const CharPinyinMap& charMap) {
        auto chars = utf8SplitChars(word);
        if (chars.empty()) return "";

        std::string result;
        for (auto& ch : chars) {
            auto it = charMap.find(ch);
            if (it == charMap.end() || it->second.empty()) {
                if ((unsigned char)ch[0] < 128) {
                    if (!result.empty()) result += ' ';
                    result += ch;
                    continue;
                }
                return "";
            }
            std::string bestPy;
            int bestFreq = -1;
            for (auto& p : it->second) {
                if (p.second > bestFreq) {
                    bestFreq = p.second;
                    bestPy = p.first;
                }
            }
            if (!result.empty()) result += ' ';
            result += bestPy;
        }
        return result;
    }

    // ── 加载一个 rime-ice 词库文件 ──
    // targetTrie:     写入目标前缀树
    // targetCharMap:  写入目标字符→拼音映射 (仅当 buildCharMap=true)
    // addAbbrev:      是否为多字词生成简拼条目
    // buildCharMap:   是否用单字条目构建字符→拼音映射
    static int loadRimeFile(const std::string& filepath, bool addAbbrev, bool buildCharMap,
                            TrieDict& targetTrie,
                            CharPinyinMap& targetCharMap)
    {
        std::ifstream fin(filepath);
        if (!fin.is_open()) return 0;

        bool inData = false;
        int count = 0;
        std::string line;

        while (std::getline(fin, line)) {
            if (line.empty()) continue;

            // YAML 文档标记
            if (line == "...") { inData = true; continue; }
            if (line == "---") { inData = false; continue; }
            if (!inData) continue;

            // 跳过注释行 (# 开头)
            if (line[0] == '#') continue;

            // 解析 Tab 分隔的数据
            size_t t1 = line.find('\t');
            if (t1 == std::string::npos) continue;

            std::string word = line.substr(0, t1);
            std::string rest = line.substr(t1 + 1);
            if (word.empty()) continue;

            size_t t2 = rest.find('\t');

            std::string pinyinSpaced;
            int freq = 0;

            if (t2 == std::string::npos) {
                // 只有 2 列: 可能是「字+拼音」或「词+词频」
                std::string col2 = rest;
                bool isFreq = !col2.empty();
                for (char ch : col2) {
                    if (!std::isdigit((unsigned char)ch)) { isFreq = false; break; }
                }

                if (isFreq) {
                    // 格式: 词\t词频 (tencent 风格, 无拼音)
                    freq = std::stoi(col2);
                    pinyinSpaced = "";
                } else {
                    // 格式: 字\t拼音 (41448 风格, 无词频)
                    pinyinSpaced = col2;
                    freq = 0;
                }
            } else {
                // 3 列: 词\t拼音\t词频
                pinyinSpaced = rest.substr(0, t2);
                try {
                    freq = std::stoi(rest.substr(t2 + 1));
                } catch (...) {
                    freq = 0;
                }
            }

            if (word.empty()) continue;

            // 如果是无拼音条目，尝试从字符映射表生成拼音
            if (pinyinSpaced.empty()) {
                pinyinSpaced = generatePinyin(word, targetCharMap);
                if (pinyinSpaced.empty()) continue;
            }

            // 给无词频的字表条目一个基础频率
            if (freq <= 0) freq = 1;

            // 空格拼音 → 紧凑拼音 (ban fa → banfa)
            std::string pinyinCompact;
            for (char ch : pinyinSpaced) {
                if (ch != ' ' && ch != '\'') pinyinCompact += ch;
            }
            if (pinyinCompact.empty()) continue;

            // 加入前缀树
            targetTrie.insert(pinyinCompact, word, freq);
            count++;

            // 为多字词生成简拼条目
            if (addAbbrev) {
                int charCount = utf8CharCount(word);
                if (charCount >= 2) {
                    std::string abbrev;
                    bool startOfSyl = true;
                    for (char ch : pinyinSpaced) {
                        if (ch == ' ') {
                            startOfSyl = true;
                        } else if (startOfSyl) {
                            abbrev += ch;
                            startOfSyl = false;
                        }
                    }
                    if (!abbrev.empty() && abbrev != pinyinCompact) {
                        targetTrie.insert(abbrev, word, freq - 200);
                    }
                }
            }

            // 构建字符→拼音映射 (仅单字)
            if (buildCharMap && utf8CharCount(word) == 1) {
                targetCharMap[word].push_back({pinyinCompact, freq});
            }
        }

        return count;
    }

    // ── 对 trie 中所有 key 的词条去重并排序 (降序按频率) ──
    static void dedupAndSort(TrieDict& trie) {
        trie.forEach([&](const std::string& /*key*/,
                         std::vector<TrieDict::Entry>& vec) {
            // 去重: 同一词条保留最高频率
            std::unordered_map<std::string, int> best;
            for (auto& p : vec) {
                auto it = best.find(p.word);
                if (it == best.end() || p.freq > it->second) {
                    best[p.word] = p.freq;
                }
            }

            // 重建向量并排序 (降序)
            vec.clear();
            for (auto& p : best) {
                vec.push_back(TrieDict::Entry(p.first, p.second));
            }
            std::sort(vec.begin(), vec.end(),
                [](const TrieDict::Entry& a, const TrieDict::Entry& b) {
                    return a.freq > b.freq;
                });
        });
    }

    // ── 后台加载线程: 从零构建完整词库 ──
    static DWORD WINAPI backgroundLoadProc(LPVOID param) {
        PinyinDict* self = (PinyinDict*)param;

        std::string dir = getDictDirectory() + "cn_dicts\\";

        // 局部字符→拼音映射 (独立于主线程的 charToPinyins)
        CharPinyinMap localCharMap;

        // 第 1 步: 加载字表 (同时构建字符→拼音映射)
        int n1 = loadRimeFile(dir + "8105.dict.yaml", false, true,
                              self->m_trieBackground, localCharMap);
        int n2 = loadRimeFile(dir + "41448.dict.yaml", false, true,
                              self->m_trieBackground, localCharMap);

        // 第 2 步: 加载词组 (生成简拼)
        int n3 = loadRimeFile(dir + "base.dict.yaml", true, false,
                              self->m_trieBackground, localCharMap);
        int n4 = loadRimeFile(dir + "ext.dict.yaml", true, false,
                              self->m_trieBackground, localCharMap);
        int n5 = loadRimeFile(dir + "others.dict.yaml", true, false,
                              self->m_trieBackground, localCharMap);

        // 第 3 步: 加载腾讯词向量 (无拼音，用字符映射表自动注音)
        int n6 = loadRimeFile(dir + "tencent.dict.yaml", true, false,
                              self->m_trieBackground, localCharMap);

        // 第 4 步: 去重 + 排序
        dedupAndSort(self->m_trieBackground);

        // 标记加载完成
        self->m_backgroundReady.store(true, std::memory_order_release);

        // 输出加载统计
        char buf[256];
        snprintf(buf, sizeof(buf),
            "[PinyinIME] Background dict loaded: chars %d+%d, words %d+%d+%d, tencent %d, %zu keys\n",
            n1, n2, n3, n4, n5, n6, self->m_trieBackground.keyCount());
        OutputDebugStringA(buf);

        return 0;
    }

    // ── 尝试将后台加载完成的词库切换到前台 ──
    // 调用时机: 每次查询前 (find/prefixSearch)
    // 线程安全: 后台线程已完成写入, 仅主线程调用此方法
    void trySwapBackground() {
        // ── 共享模式: 仅检查状态升级 ──
        if (m_sharedMode && m_pSharedBase) {
            ShmHeader* hdr = (ShmHeader*)m_pSharedBase;
            LONG currentState = hdr->state;
            if (currentState != m_shmStateLastCheck) {
                m_shmStateLastCheck = currentState;
                m_shmTrie.refreshState();
                OutputDebugStringA("[PinyinIME] Shared dict state changed\n");
            }
            return;
        }

        // ── 本地模式 ──
        if (m_backgroundReady.load(std::memory_order_acquire)) {
            // ── 如果是共享词库创建者, 序列化后台词库到共享内存 ──
            if (m_hDictMutex) {
                WaitForSingleObject(m_hDictMutex, INFINITE);

                HANDLE hMapping = CreateFileMappingW(
                    INVALID_HANDLE_VALUE,     // 页面文件支持
                    nullptr,                   // 默认安全属性
                    PAGE_READWRITE,
                    0,                         // 高 32 位大小
                    SHM_DICT_MAX_SIZE,         // 低 32 位大小 (80 MB)
                    PinyinIME_DICT_MAPPING);

                if (hMapping && GetLastError() != ERROR_ALREADY_EXISTS) {
                    void* base = MapViewOfFile(hMapping, FILE_MAP_WRITE, 0, 0, 0);
                    if (base) {
                        bool ok = serializeTrieToSharedMemory(base, m_trieBackground);
                        if (ok) {
                            ShmHeader* hdr = (ShmHeader*)base;
                            InterlockedExchange(&hdr->state, DICT_STATE_FULL_READY);

                            // 自身切换到共享模式
                            m_pSharedBase = base;
                            m_hSharedMapping = hMapping;
                            m_shmTrie.setBase(m_pSharedBase);
                            m_shmStateLastCheck = DICT_STATE_FULL_READY;
                            m_sharedMode = true;

                            // 释放本地内存
                            m_trie.clear();
                            m_trieBackground.clear();

                            OutputDebugStringA("[PinyinIME] Shared dict created, switched to shared mode\n");
                        } else {
                            UnmapViewOfFile(base);
                            CloseHandle(hMapping);
                            OutputDebugStringA("[PinyinIME] Shared dict serialization failed\n");
                        }
                    }
                }
                ReleaseMutex(m_hDictMutex);
            }

            // ── 如果还没切换到共享模式, 本地交换 ──
            if (!m_sharedMode) {
                m_trie = std::move(m_trieBackground);
                m_backgroundReady.store(false, std::memory_order_release);

                if (m_hLoadThread) {
                    WaitForSingleObject(m_hLoadThread, INFINITE);
                    CloseHandle(m_hLoadThread);
                    m_hLoadThread = nullptr;
                }

                OutputDebugStringA("[PinyinIME] Swapped to full dictionary (local)\n");
            }
        }
    }

    // ── 统一查询接口 (根据 m_sharedMode 分发到本地或共享内存) ──
    const std::vector<TrieDict::Entry>* find(const std::string& key) {
        if (m_sharedMode) return m_shmTrie.find(key);
        return m_trie.find(key);
    }

    void prefixSearch(const std::string& prefix,
                      std::unordered_map<std::string, int>& out,
                      int maxPerKey = 3,
                      int maxDepth = 0) {
        if (m_sharedMode) {
            m_shmTrie.prefixSearch(prefix, out, maxPerKey, maxDepth);
        } else {
            m_trie.prefixSearch(prefix, out, maxPerKey, maxDepth);
        }
    }

    // ── 初始化: 同步加载字表 + 启动后台加载完整词库 ──
    void init() {
        m_trie.clear();
        m_trieBackground.clear();
        charToPinyins.clear();
        m_backgroundReady.store(false, std::memory_order_release);
        m_sharedMode = false;

        // ── 尝试打开已有的共享词库 ──
        m_hSharedMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, PinyinIME_DICT_MAPPING);
        if (m_hSharedMapping) {
            m_pSharedBase = MapViewOfFile(m_hSharedMapping, FILE_MAP_READ, 0, 0, 0);
            if (m_pSharedBase) {
                ShmHeader* hdr = (ShmHeader*)m_pSharedBase;
                if (hdr->magic == SHM_DICT_MAGIC && hdr->state >= DICT_STATE_FULL_READY) {
                    m_shmTrie.setBase(m_pSharedBase);
                    m_shmStateLastCheck = hdr->state;
                    m_sharedMode = true;
                    OutputDebugStringA("[PinyinIME] Using shared memory dictionary (full)\n");
                    return;  // 无需加载 YAML!
                }
                OutputDebugStringA("[PinyinIME] Shared dict not ready, loading locally\n");
                UnmapViewOfFile(m_pSharedBase);
                m_pSharedBase = nullptr;
            }
            CloseHandle(m_hSharedMapping);
            m_hSharedMapping = nullptr;
        }

        // ── 无可用的共享词库, 本地加载 ──
        std::string dir = getDictDirectory() + "cn_dicts\\";

        // 同步加载: 字表 (8105 + 41448, ~550KB, 毫秒级完成)
        int n1 = loadRimeFile(dir + "8105.dict.yaml", false, true,
                              m_trie, charToPinyins);
        int n2 = loadRimeFile(dir + "41448.dict.yaml", false, true,
                              m_trie, charToPinyins);

        dedupAndSort(m_trie);

        char buf[256];
        snprintf(buf, sizeof(buf),
            "[PinyinIME] Startup dict loaded: chars %d+%d, %zu keys (background loading...)\n",
            n1, n2, m_trie.keyCount());
        OutputDebugStringA(buf);

        // ── 尝试成为共享词库的创建者 ──
        m_hDictMutex = CreateMutexW(nullptr, FALSE, PinyinIME_DICT_MUTEX);
        if (m_hDictMutex && GetLastError() != ERROR_ALREADY_EXISTS) {
            // 我们是创建者 — 后台加载完成后会序列化到共享内存
            OutputDebugStringA("[PinyinIME] Will create shared dict after background load\n");
        } else {
            // 已有其他进程在创建, 释放互斥锁
            if (m_hDictMutex) { CloseHandle(m_hDictMutex); m_hDictMutex = nullptr; }
        }

        // ── 异步加载: 启动后台线程加载完整词库 (~48MB, 数秒完成) ──
        m_hLoadThread = CreateThread(
            nullptr,                // 默认安全属性
            0,                      // 默认栈大小
            backgroundLoadProc,     // 线程函数
            this,                   // 参数
            0,                      // 立即执行
            nullptr                 // 不需要线程 ID
        );
    }

    // ── 清理 (停用时调用) ──
    void shutdown() {
        // 等待后台加载线程结束
        if (m_hLoadThread) {
            WaitForSingleObject(m_hLoadThread, INFINITE);
            CloseHandle(m_hLoadThread);
            m_hLoadThread = nullptr;
        }
        // 清理共享内存
        if (m_pSharedBase) {
            UnmapViewOfFile(m_pSharedBase);
            m_pSharedBase = nullptr;
        }
        if (m_hSharedMapping) {
            CloseHandle(m_hSharedMapping);
            m_hSharedMapping = nullptr;
        }
        if (m_hDictMutex) {
            CloseHandle(m_hDictMutex);
            m_hDictMutex = nullptr;
        }
        m_sharedMode = false;
        m_trie.clear();
        m_trieBackground.clear();
        charToPinyins.clear();
    }
};

#endif // DICTIONARY_H
