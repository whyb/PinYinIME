// shared/hot_dict.h — Hot word cache for fast prefix completion
//
// Preloads the top-N most frequent multi-character words from
// rime-ice base.dict.yaml as a compact in-memory array. Each entry
// stores (concatenated pinyin, word, frequency). On query, a linear
// scan over ~150K entries finds all words whose pinyin starts with
// the given prefix — typically in a few hundred microseconds.
//
// Complements dict.bin by providing fast, frequency-sorted completion
// for partial pinyin input.
//
// File format (hotwords.bin):
//   uint32_t  count
//   per entry:
//     uint16_t  pyLen     — concatenated pinyin UTF-8 length
//     char      pinyin[]  — concatenated pinyin (no spaces, no null)
//     uint16_t  wordLen   — word UTF-8 length
//     char      word[]    — word text (no null)
//     int32_t   freq      — frequency
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>

class HotWordCache {
public:
    struct Entry {
        std::string pinyin;  // concatenated, e.g. "nihao"
        std::string word;
        int32_t     freq;
    };

    HotWordCache() = default;

    bool load(const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) return false;

        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size < 4) { fclose(f); return false; }

        m_data.resize(size);
        if (fread(m_data.data(), 1, size, f) != (size_t)size) {
            fclose(f); return false;
        }
        fclose(f);

        const uint8_t* p = m_data.data();
        const uint8_t* end = p + size;
        m_count = *(const uint32_t*)p;
        p += 4;

        m_entries.reserve(m_count);
        for (uint32_t i = 0; i < m_count; ++i) {
            if (p + 2 > end) break;
            uint16_t pyLen = *(const uint16_t*)p; p += 2;
            if (p + pyLen + 2 > end) break;
            std::string py((const char*)p, pyLen); p += pyLen;

            uint16_t wLen = *(const uint16_t*)p; p += 2;
            if (p + wLen + 4 > end) break;
            std::string w((const char*)p, wLen); p += wLen;

            int32_t f = *(const int32_t*)p; p += 4;
            // Only keep multi-character words (skip single chars — dict covers them)
            if (w.size() > 3) {  // UTF-8: 1 Chinese char = 3 bytes
                m_entries.push_back({py, w, f});
            }
        }
        return true;
    }

    bool isLoaded() const { return !m_entries.empty(); }
    size_t count() const { return m_entries.size(); }

    // Find words whose concatenated pinyin starts with `prefix`.
    // Results are frequency-sorted and limited to `maxResults` (0 = no limit).
    std::vector<std::pair<std::string, int>> query(const std::string& prefix,
                                                    size_t maxResults = 32) const {
        std::vector<std::pair<std::string, int>> out;
        if (!isLoaded() || prefix.empty()) return out;

        // Linear scan — ~150K entries × ~10 bytes avg pinyin = fast
        for (auto& e : m_entries) {
            if (e.pinyin.size() >= prefix.size() &&
                memcmp(e.pinyin.data(), prefix.data(), prefix.size()) == 0) {
                out.emplace_back(e.word, e.freq);
            }
        }

        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        if (maxResults > 0 && out.size() > maxResults) out.resize(maxResults);
        return out;
    }

private:
    std::vector<uint8_t> m_data;    // raw file buffer
    std::vector<Entry>   m_entries; // parsed entries (multi-char only)
    uint32_t             m_count = 0;
};
