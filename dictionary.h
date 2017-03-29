// dictionary.h — 极简拼音输入法外部词库加载器
// 从同目录下的文本文件中动态加载单字、词组和简拼数据
// 格式: 拼音 \t 汉字 \t 词频权重

#ifndef DICTIONARY_H
#define DICTIONARY_H

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <fstream>
#include <sstream>

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

struct PinyinDict {
    std::unordered_map<std::string, std::vector<std::pair<std::string, int>>> entries;

    // 从 TSV 文本文件加载词库条目
    bool loadFromFile(const std::string& filename) {
        std::ifstream fin(filename);
        if (!fin.is_open()) return false;

        std::string line;
        while (std::getline(fin, line)) {
            // 过滤空行和注释行
            if (line.empty() || line[0] == '#') continue;

            size_t t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            size_t t2 = line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;

            std::string py = line.substr(0, t1);
            std::string txt = line.substr(t1 + 1, t2 - t1 - 1);
            int freq = 0;
            try {
                freq = std::stoi(line.substr(t2 + 1));
            } catch (...) {
                continue;
            }

            if (!py.empty() && !txt.empty()) {
                entries[py].push_back({txt, freq});
            }
        }
        return true;
    }

    void init() {
        entries.clear();
        std::string exeDir = getExeDirectory();

        loadFromFile(exeDir + "single_chars.txt");
        loadFromFile(exeDir + "words.txt");
        loadFromFile(exeDir + "short_pinyin.txt");

        // 对每个拼音的候选词列表按词频从大到小进行排序
        for (auto& kv : entries) {
            auto& vec = kv.second;
            std::sort(vec.begin(), vec.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
        }
    }
};

#endif // DICTIONARY_H
