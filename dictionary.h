// dictionary.h — 极简拼音输入法词库加载器
// 直接读取 rime-ice 原版 YAML 词库文件 (cn_dicts/*.dict.yaml)
// 底层使用 TrieDict 前缀树存储, 支持 O(L+M) 前缀查找
// 无需任何预处理，开箱即用

#ifndef DICTIONARY_H
#define DICTIONARY_H

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <fstream>
#include <cctype>
#include "trie_dict.h"

// 获取 exe 所在的文件夹路径（带末尾反斜杠）
inline std::string getExeDirectory() {
    char path[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string exePath(path);
    size_t pos = exePath.find_last_of("\\/");
    if (pos != std::string::npos) {
        return exePath.substr(0, pos + 1);
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

struct PinyinDict {
    // ── 主词库: 前缀树存储 (替代 unordered_map, O(L+M) 前缀查找) ──
    TrieDict m_trie;

    // ── 字符→拼音映射表 (从字表构建，用于给 tencent 等无拼音词库自动注音) ──
    std::unordered_map<std::string, std::vector<std::pair<std::string, int>>> charToPinyins;

    // ── 加载一个 rime-ice 词库文件 ──
    // addAbbrev: 是否为多字词生成简拼条目
    // buildCharMap: 是否用单字条目构建字符→拼音映射
    int loadRimeFile(const std::string& filepath, bool addAbbrev, bool buildCharMap) {
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
                pinyinSpaced = generatePinyin(word);
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
            m_trie.insert(pinyinCompact, word, freq);
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
                        m_trie.insert(abbrev, word, freq - 200);
                    }
                }
            }

            // 构建字符→拼音映射 (仅单字)
            if (buildCharMap && utf8CharCount(word) == 1) {
                charToPinyins[word].push_back({pinyinCompact, freq});
            }
        }

        return count;
    }

    // ── 用字符映射表为词语生成拼音 ──
    std::string generatePinyin(const std::string& word) {
        auto chars = utf8SplitChars(word);
        if (chars.empty()) return "";

        std::string result;
        for (auto& ch : chars) {
            auto it = charToPinyins.find(ch);
            if (it == charToPinyins.end() || it->second.empty()) {
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

    // ── 初始化: 加载所有词库文件并排序 ──
    void init() {
        m_trie.clear();
        charToPinyins.clear();

        std::string dir = getExeDirectory() + "cn_dicts\\";

        // 第 1 步: 加载字表 (同时构建字符→拼音映射)
        int n1 = loadRimeFile(dir + "8105.dict.yaml", false, true);
        int n2 = loadRimeFile(dir + "41448.dict.yaml", false, true);

        // 第 2 步: 加载词组 (生成简拼)
        int n3 = loadRimeFile(dir + "base.dict.yaml", true, false);
        int n4 = loadRimeFile(dir + "ext.dict.yaml", true, false);
        int n5 = loadRimeFile(dir + "others.dict.yaml", true, false);

        // 第 3 步: 加载腾讯词向量 (无拼音，用字符映射表自动注音)
        int n6 = loadRimeFile(dir + "tencent.dict.yaml", true, false);

        // 第 4 步: 去重 + 排序 (遍历前缀树中所有 key 的词条)
        m_trie.forEach([&](const std::string& key,
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

        // 输出加载统计 (调试用)
        char buf[256];
        snprintf(buf, sizeof(buf),
            "Dict loaded: chars %d+%d, words %d+%d+%d, tencent %d, %zu keys\n",
            n1, n2, n3, n4, n5, n6, m_trie.keyCount());
        OutputDebugStringA(buf);
    }
};

#endif // DICTIONARY_H
