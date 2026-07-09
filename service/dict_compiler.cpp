// service/dict_compiler.cpp — Offline dictionary compiler (sorted array version)
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <fstream>
#include <cctype>

#include "../shared/dict_binary.h"

inline int utf8CharCount(const std::string& s) {
    int count = 0;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0x80) == 0) i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else i += 1;
        count++;
    }
    return count;
}
inline std::vector<std::string> utf8SplitChars(const std::string& s) {
    std::vector<std::string> chars;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        if ((c & 0x80) == 0) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        chars.push_back(s.substr(i, len)); i += len;
    }
    return chars;
}
struct RawEntry { std::string pinyin, word; int32_t freq; };
using CharPinyinMap = std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>;
static int g_maxChars = 3;

std::string generatePinyin(const std::string& word, const CharPinyinMap& charMap) {
    auto chars = utf8SplitChars(word); if (chars.empty()) return "";
    std::string result;
    for (auto& ch : chars) {
        auto it = charMap.find(ch);
        if (it == charMap.end() || it->second.empty()) {
            if ((unsigned char)ch[0] < 128) { if (!result.empty()) result += ' '; result += ch; continue; }
            return "";
        }
        std::string bestPy; int bestFreq = -1;
        for (auto& p : it->second) if (p.second > bestFreq) { bestFreq = p.second; bestPy = p.first; }
        if (!result.empty()) result += ' '; result += bestPy;
    }
    return result;
}

int loadRimeFile(const std::string& fp, bool addAbbrev, bool buildCharMap,
                 std::vector<RawEntry>& out, CharPinyinMap& cm) {
    std::ifstream f(fp); if (!f.is_open()) return 0;
    bool inData = false; int cnt = 0; std::string l;
    while (std::getline(f, l)) {
        if (l.empty()) continue; if (l == "...") { inData = true; continue; }
        if (l == "---") { inData = false; continue; } if (!inData || l[0] == '#') continue;
        size_t t1 = l.find('\t'); if (t1 == std::string::npos) continue;
        std::string w = l.substr(0, t1), r = l.substr(t1 + 1); if (w.empty()) continue;
        int cc = utf8CharCount(w); if (cc > g_maxChars) continue;
        size_t t2 = r.find('\t'); std::string ps; int fr = 0;
        if (t2 == std::string::npos) {
            bool isF = true; for (char ch : r) if (!std::isdigit((unsigned char)ch)) { isF = false; break; }
            if (isF) { fr = std::stoi(r); ps = ""; } else { ps = r; fr = 0; }
        } else { ps = r.substr(0, t2); try { fr = std::stoi(r.substr(t2+1)); } catch(...) { fr = 0; } }
        if (ps.empty()) { ps = generatePinyin(w, cm); if (ps.empty()) continue; }
        if (fr <= 0) fr = 1;
        std::string pc; for (char ch : ps) if (ch != ' ' && ch != '\'') pc += ch;
        if (pc.empty()) continue;
        bool v = true; for (char ch : pc) if (ch < 'a' || ch > 'z') { v = false; break; }
        if (!v) continue;
        out.push_back({pc, w, fr}); cnt++;
        if (addAbbrev && cc >= 2) {
            std::string ab; bool sos = true;
            for (char ch : ps) { if (ch == ' ') sos = true; else if (sos) { ab += ch; sos = false; } }
            if (!ab.empty() && ab != pc && ab.size() >= 2) {
                bool av = true; for (char ch : ab) if (ch < 'a' || ch > 'z') { av = false; break; }
                if (av) { out.push_back({ab, w, fr - 200}); cnt++; }
            }
        }
        if (buildCharMap && cc == 1) cm[w].push_back({pc, fr});
    }
    return cnt;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: dict_compiler <indir> <out.bin> [--max-chars N] [--hotwords N]\n");
        return 1;
    }
    std::string id = argv[1], op = argv[2];
    int hotCount = 0;
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--max-chars") == 0 && i + 1 < argc) {
            g_maxChars = atoi(argv[++i]); if (g_maxChars < 1) g_maxChars = 1;
        } else if (strcmp(argv[i], "--hotwords") == 0 && i + 1 < argc) {
            hotCount = atoi(argv[++i]); if (hotCount < 100) hotCount = 100;
        }
    }
    if (!id.empty() && id.back() != '\\' && id.back() != '/') id += '\\';
    printf("Dict Compiler v2 (sorted array, max-chars=%d)\n", g_maxChars);

    // Phase 1: Parse
    printf("[Phase1] Parsing...\n");
    std::vector<RawEntry> all; CharPinyinMap cm;
    int n1 = loadRimeFile(id + "8105.dict.yaml", false, true, all, cm);
    int n2 = loadRimeFile(id + "41448.dict.yaml", false, true, all, cm);
    printf("  chars: %d+%d\n", n1, n2);
    int n3 = loadRimeFile(id + "base.dict.yaml", true, false, all, cm);
    int n4 = loadRimeFile(id + "ext.dict.yaml", true, false, all, cm);
    int n5 = loadRimeFile(id + "others.dict.yaml", true, false, all, cm);
    printf("  words: %d+%d+%d\n", n3, n4, n5);
    int n6 = loadRimeFile(id + "tencent.dict.yaml", true, false, all, cm);
    printf("  tencent: %d  total: %zu\n", n6, all.size());

    // Phase 2: Group
    printf("[Phase2] Grouping...\n");
    std::unordered_map<std::string, std::vector<std::pair<std::string, int32_t>>> grp;
    for (auto& e : all) grp[e.pinyin].push_back({e.word, e.freq});
    for (auto& kv : grp) {
        auto& v = kv.second;
        std::unordered_map<std::string, int32_t> best;
        for (auto& p : v) {
            auto it = best.find(p.first);
            if (it == best.end() || p.second > it->second) best[p.first] = p.second;
        }
        v.clear(); for (auto& p : best) v.push_back(p);
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    }

    // Phase 3: Build sorted key array + entry index + string pool
    printf("[Phase3] Building arrays...\n");
    std::vector<std::string> keys; keys.reserve(grp.size());
    for (auto& kv : grp) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());

    // String pool: dedup all words
    std::unordered_map<std::string, uint32_t> strMap;
    std::vector<char> strPool;
    auto intern = [&](const std::string& w) -> uint32_t {
        auto it = strMap.find(w);
        if (it != strMap.end()) return it->second;
        uint32_t off = (uint32_t)strPool.size();
        strPool.insert(strPool.end(), w.begin(), w.end()); strPool.push_back('\0');
        strMap[w] = off; return off;
    };

    // Build sorted index: for each key, store (start, count) in entry_index
    // Binary search uses a parallel array of pinyin strings (stored as offsets into a key pool)
    std::vector<uint32_t> keyOffsets;  // byte offset of each key in keyPool
    std::vector<uint32_t> keyStarts;   // start index in entry_index
    std::vector<uint32_t> keyCounts;   // count for this key
    std::vector<DictFileEntry> entries;
    std::vector<char> keyPool;

    for (auto& key : keys) {
        keyOffsets.push_back((uint32_t)keyPool.size());
        keyPool.insert(keyPool.end(), key.begin(), key.end());
        keyPool.push_back('\0');

        auto& vec = grp[key];
        keyStarts.push_back((uint32_t)entries.size());
        keyCounts.push_back((uint32_t)vec.size());
        for (auto& p : vec) {
            entries.push_back({intern(p.first), p.second});
        }
    }
    uint32_t keyCount = (uint32_t)keys.size();

    printf("  Keys: %u  Entries: %zu  Pool: %zu bytes\n", keyCount, entries.size(), strPool.size());

    // Phase 4: Write binary
    printf("[Phase4] Writing...\n");
    uint32_t hdrSize = 64;
    uint32_t kiSize  = keyCount * 4;        // keyOffsets[]
    uint32_t ksSize  = keyCount * 4;        // keyStarts[]
    uint32_t kcSize  = keyCount * 4;        // keyCounts[]
    uint32_t eiSize  = (uint32_t)entries.size() * 8;
    uint32_t kpSize  = (uint32_t)keyPool.size();
    uint32_t spSize  = (uint32_t)strPool.size();

    uint32_t kiOff  = hdrSize;
    uint32_t ksOff  = kiOff + kiSize;
    uint32_t kcOff  = ksOff + ksSize;
    uint32_t eiOff  = kcOff + kcSize;
    uint32_t kpOff  = eiOff + eiSize;
    uint32_t spOff  = kpOff + kpSize;
    uint32_t total  = spOff + spSize;

    std::vector<uint8_t> buf(total, 0);
    DictFileHeader* h = (DictFileHeader*)buf.data();
    memset(h, 0, 64);
    h->magic = DICT_BIN_MAGIC; h->version = 3;  // v3 = sorted array format
    h->total_file_size = total;
    h->root_base = keyCount;  // reuse as key_count
    h->state_count = 0;       // unused in v3
    h->entry_count = (uint32_t)entries.size();
    h->string_pool_offset = spOff;
    h->string_pool_size = spSize;
    h->base_array_offset = kiOff;        // key_offsets
    h->check_array_offset = ksOff;       // key_starts
    h->entry_offset_array_offset = kcOff; // key_counts
    h->entry_index_offset = eiOff;       // entry_index
    h->reserved[0] = kpOff;              // key_pool_offset
    h->reserved[1] = kpSize;             // key_pool_size
    h->reserved[2] = 0;

    memcpy(buf.data() + kiOff, keyOffsets.data(), kiSize);
    memcpy(buf.data() + ksOff, keyStarts.data(), ksSize);
    memcpy(buf.data() + kcOff, keyCounts.data(), kcSize);
    memcpy(buf.data() + eiOff, entries.data(), eiSize);
    memcpy(buf.data() + kpOff, keyPool.data(), kpSize);
    memcpy(buf.data() + spOff, strPool.data(), spSize);

    FILE* f = fopen(op.c_str(), "wb");
    if (!f) { fprintf(stderr, "ERROR: open\n"); return false; }
    fwrite(buf.data(), 1, total, f); fclose(f);
    printf("  Total: %.2f MB\n", total / (1024.0 * 1024.0));

    // Phase 5: Hot words compilation (optional)
    if (hotCount > 0) {
        printf("[Phase5] Compiling top-%d hot words...\n", hotCount);

        // Collect multi-character words, dedup by word (keep max freq)
        struct HotEntry { std::string py, word; int32_t freq; };
        std::unordered_map<std::string, HotEntry> hotDedup;  // word → best entry

        for (auto& e : all) {
            if (utf8CharCount(e.word) < 2) continue;
            auto it = hotDedup.find(e.word);
            if (it == hotDedup.end() || e.freq > it->second.freq) {
                hotDedup[e.word] = {e.pinyin, e.word, e.freq};
            }
        }

        std::vector<HotEntry> hotEntries;
        hotEntries.reserve(hotDedup.size());
        for (auto& kv : hotDedup) hotEntries.push_back(kv.second);

        // Sort by frequency descending
        std::sort(hotEntries.begin(), hotEntries.end(),
            [](const HotEntry& a, const HotEntry& b) { return a.freq > b.freq; });

        if ((int)hotEntries.size() > hotCount) hotEntries.resize(hotCount);

        // Write hotwords.bin next to dict.bin
        std::string hotPath = op;
        // Replace extension with _hotwords.bin
        size_t slash = hotPath.find_last_of("/\\");
        size_t dot = hotPath.rfind('.');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            hotPath = hotPath.substr(0, dot);
        }
        hotPath += "_hotwords.bin";

        FILE* hf = fopen(hotPath.c_str(), "wb");
        if (!hf) { fprintf(stderr, "  ERROR: cannot create hotwords file\n"); }
        else {
            uint32_t count = (uint32_t)hotEntries.size();
            fwrite(&count, 4, 1, hf);

            for (auto& he : hotEntries) {
                uint16_t pyLen = (uint16_t)he.py.size();
                uint16_t wLen  = (uint16_t)he.word.size();
                fwrite(&pyLen, 2, 1, hf);
                fwrite(he.py.data(), 1, pyLen, hf);
                fwrite(&wLen, 2, 1, hf);
                fwrite(he.word.data(), 1, wLen, hf);
                fwrite(&he.freq, 4, 1, hf);
            }
            fclose(hf);
            printf("  Hot words: %u entries → %s (%.2f MB)\n",
                   count, hotPath.c_str(), (4.0 + count * 12) / (1024 * 1024));
        }
    }

    printf("Done.\n");
    return 0;
}
