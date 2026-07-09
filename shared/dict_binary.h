// shared/dict_binary.h — Binary dictionary format + query engine
// Supports v2 (DAT) and v3 (sorted array) formats.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

constexpr uint32_t DICT_BIN_MAGIC       = 0x32424450;
constexpr uint32_t DICT_BIN_VERSION     = 2;
constexpr uint32_t DICT_BIN_HEADER_SIZE = 64;

#pragma pack(push, 1)
struct DictFileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t total_file_size;
    uint32_t root_base;
    uint32_t root_check;
    uint32_t state_count;
    uint32_t entry_count;
    uint32_t string_pool_offset;
    uint32_t string_pool_size;
    uint32_t base_array_offset;
    uint32_t check_array_offset;
    uint32_t entry_offset_array_offset;
    uint32_t entry_index_offset;
    uint32_t reserved[3];
};

struct DictFileEntry {
    uint32_t word_offset;
    int32_t  frequency;
};
#pragma pack(pop)

class BinaryDictReader {
public:
    BinaryDictReader() = default;

    bool attach(void* mappingBase) {
        m_base = static_cast<char*>(mappingBase);
        m_header = static_cast<DictFileHeader*>(mappingBase);
        if (!m_header || m_header->magic != DICT_BIN_MAGIC) { m_base = nullptr; m_header = nullptr; return false; }

        // Get actual mapping size via VirtualQuery for bounds validation
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(mappingBase, &mbi, sizeof(mbi)) || mbi.RegionSize < DICT_BIN_HEADER_SIZE) {
            m_base = nullptr; m_header = nullptr; return false;
        }
        size_t mapSize = mbi.RegionSize;

        // Validate total_file_size against actual mapping
        if (m_header->total_file_size > mapSize) {
            m_base = nullptr; m_header = nullptr; return false;
        }

        m_version = m_header->version;

        // Helper: validate an offset + size fits within the mapping
        auto inBounds = [mapSize](uint32_t offset, uint32_t size) -> bool {
            return offset < mapSize && size <= mapSize - offset;
        };

        if (m_version >= 3) {
            // v3: sorted array format — validate all sections
            m_keyCount   = m_header->root_base;
            uint32_t kiSize = m_keyCount * sizeof(uint32_t);   // keyOffsets[]
            uint32_t ksSize = m_keyCount * sizeof(uint32_t);   // keyStarts[]
            uint32_t kcSize = m_keyCount * sizeof(uint32_t);   // keyCounts[]
            uint32_t kpSize = m_header->reserved[1];           // keyPool size

            if (!inBounds(m_header->base_array_offset, kiSize) ||
                !inBounds(m_header->check_array_offset, ksSize) ||
                !inBounds(m_header->entry_offset_array_offset, kcSize)) {
                m_base = nullptr; m_header = nullptr; return false;
            }

            m_keyOffsets = reinterpret_cast<uint32_t*>(m_base + m_header->base_array_offset);
            m_keyStarts  = reinterpret_cast<uint32_t*>(m_base + m_header->check_array_offset);
            m_keyCounts  = reinterpret_cast<uint32_t*>(m_base + m_header->entry_offset_array_offset);

            // Key pool
            uint32_t kpOff = m_header->reserved[0];
            if (kpOff == 0) {
                // Old compiler didn't write reserved[0]; compute from layout:
                // layout: header(64) + keyOffsets(K*4) + keyStarts(K*4) + keyCounts(K*4) + entries(E*8)
                kpOff = 64 + m_keyCount * 12 + m_header->entry_count * 8;
            }
            if (kpSize == 0) {
                // reserved[1] not set by older compiler — compute from mapping bounds
                kpSize = (uint32_t)(mapSize - kpOff);
            }
            if (!inBounds(kpOff, kpSize)) { m_base = nullptr; m_header = nullptr; return false; }
            m_keyPool = m_base + kpOff;
            m_keyPoolSize = kpSize;
        } else {
            // v2: DAT format — validate arrays
            uint32_t sc = m_header->state_count;
            uint32_t baSize = sc * sizeof(uint32_t);
            uint32_t caSize = sc * sizeof(uint32_t);
            uint32_t eoSize = sc * 2 * sizeof(uint32_t);

            if (!inBounds(m_header->base_array_offset, baSize) ||
                !inBounds(m_header->check_array_offset, caSize) ||
                !inBounds(m_header->entry_offset_array_offset, eoSize)) {
                m_base = nullptr; m_header = nullptr; return false;
            }
            m_base_array    = reinterpret_cast<uint32_t*>(m_base + m_header->base_array_offset);
            m_check_array   = reinterpret_cast<uint32_t*>(m_base + m_header->check_array_offset);
            m_entry_offsets = reinterpret_cast<uint32_t*>(m_base + m_header->entry_offset_array_offset);
        }

        // Common: entry index + string pool
        uint32_t eiSize = m_header->entry_count * sizeof(DictFileEntry);
        if (!inBounds(m_header->entry_index_offset, eiSize) ||
            !inBounds(m_header->string_pool_offset, m_header->string_pool_size)) {
            m_base = nullptr; m_header = nullptr; return false;
        }
        m_entry_index = reinterpret_cast<DictFileEntry*>(m_base + m_header->entry_index_offset);
        m_string_pool = m_base + m_header->string_pool_offset;
        return true;
    }

    bool isReady() const { return m_base != nullptr && m_header != nullptr; }

    const DictFileEntry* find(const std::string& key, uint32_t& outCount) const {
        if (!isReady() || key.empty()) { outCount = 0; return nullptr; }
        if (m_version >= 3) {
            // Binary search on sorted keys
            uint32_t lo = 0, hi = m_keyCount;
            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2;
                if (m_keyPoolSize > 0 && m_keyOffsets[mid] >= m_keyPoolSize) { outCount = 0; return nullptr; }
                const char* k = m_keyPool + m_keyOffsets[mid];
                int cmp = key.compare(k);
                if (cmp == 0) {
                    outCount = m_keyCounts[mid];
                    if (outCount == 0) return nullptr;
                    return &m_entry_index[m_keyStarts[mid]];
                } else if (cmp < 0) {
                    hi = mid;
                } else {
                    lo = mid + 1;
                }
            }
            outCount = 0; return nullptr;
        }
        // v2: DAT traversal
        uint32_t s = m_header->root_base;
        for (char ch : key) {
            int c = ch - 'a'; if (c < 0 || c >= 26) { outCount = 0; return nullptr; }
            uint32_t t = m_base_array[s] + (uint32_t)c;
            if (t >= m_header->state_count || m_check_array[t] != s) { outCount = 0; return nullptr; }
            s = t;
        }
        outCount = m_entry_offsets[2 * s + 1];
        if (outCount == 0) return nullptr;
        return &m_entry_index[m_entry_offsets[2 * s]];
    }

    void prefixSearch(const std::string& prefix, std::unordered_map<std::string, int>& out,
                      int maxPerKey = 3, int maxDepth = 0) const {
        if (!isReady() || prefix.empty()) return;
        if (m_version >= 3) {
            // Find first key >= prefix, iterate while key starts with prefix
            uint32_t lo = 0, hi = m_keyCount;
            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2;
                const char* k = m_keyPool + m_keyOffsets[mid];
                if (prefix.compare(0, prefix.size(), k, prefix.size()) <= 0) {
                    hi = mid;
                } else {
                    lo = mid + 1;
                }
            }
            for (uint32_t i = lo; i < m_keyCount; ++i) {
                const char* k = m_keyPool + m_keyOffsets[i];
                if (strncmp(k, prefix.c_str(), prefix.size()) != 0) break;
                uint32_t cnt = m_keyCounts[i];
                uint32_t take = cnt < (uint32_t)maxPerKey ? cnt : (uint32_t)maxPerKey;
                for (uint32_t j = 0; j < take; ++j) {
                    const auto& e = m_entry_index[m_keyStarts[i] + j];
                    const char* w = getWord(e.word_offset);
                    if (!w) continue;
                    std::string ws(w);
                    auto it = out.find(ws);
                    if (it == out.end() || e.frequency > it->second) out[ws] = e.frequency;
                }
            }
            return;
        }
        // v2: DAT prefix search
        uint32_t prefixState = m_header->root_base;
        for (char ch : prefix) {
            int c = ch - 'a'; if (c < 0 || c >= 26) return;
            uint32_t t = m_base_array[prefixState] + (uint32_t)c;
            if (t >= m_header->state_count || m_check_array[t] != prefixState) return;
            prefixState = t;
        }
        struct WorkItem { uint32_t state; int depth; };
        std::vector<WorkItem> worklist; worklist.reserve(1024);
        worklist.push_back({prefixState, 0});
        while (!worklist.empty()) {
            WorkItem item = worklist.back(); worklist.pop_back();
            uint32_t start = m_entry_offsets[2 * item.state];
            uint32_t count = m_entry_offsets[2 * item.state + 1];
            if (count > 0) {
                uint32_t take = count < (uint32_t)maxPerKey ? count : (uint32_t)maxPerKey;
                for (uint32_t i = 0; i < take; ++i) {
                    const char* w = getWord(m_entry_index[start + i].word_offset);
                    if (!w) continue;
                    std::string ws(w);
                    auto it = out.find(ws);
                    if (it == out.end() || m_entry_index[start + i].frequency > it->second)
                        out[ws] = m_entry_index[start + i].frequency;
                }
            }
            if (maxDepth > 0 && item.depth >= maxDepth) continue;
            int nd = maxDepth > 0 ? item.depth + 1 : 0;
            uint32_t b = m_base_array[item.state];
            for (int c = 0; c < 26; ++c) {
                uint32_t t = b + (uint32_t)c;
                if (t < m_header->state_count && m_check_array[t] == item.state)
                    worklist.push_back({t, nd});
            }
        }
    }

    const char* getWord(uint32_t offset) const {
        if (!isReady() || offset >= m_header->string_pool_size) return nullptr;
        return m_string_pool + offset;
    }

    // ── Combined query: exact find + prefix search ──────────────────
    //
    // The canonical dict lookup used by both the production service and
    // test tools. Does exact key lookup first, then prefix search for
    // partial pinyin completion (e.g. "xianz" → "xianzai" → 现在).
    //
    // Exact matches always rank ABOVE prefix matches, regardless of
    // frequency. Within each tier, results are sorted by frequency desc.
    //
    // maxResults: clamp output to this size (0 = no limit)
    std::vector<std::pair<std::string, int>> query(const std::string& pinyin,
                                                    size_t maxResults = 0) const {
        std::vector<std::pair<std::string, int>> out;
        if (!isReady() || pinyin.empty()) return out;

        std::unordered_map<std::string, int> exactMap;
        std::unordered_map<std::string, int> prefixMap;

        // 1. Exact match
        uint32_t exactCount = 0;
        const DictFileEntry* exact = find(pinyin, exactCount);
        if (exact && exactCount > 0) {
            for (uint32_t i = 0; i < exactCount; ++i) {
                const char* w = getWord(exact[i].word_offset);
                if (!w) continue;
                auto& v = exactMap[std::string(w)];
                v = (std::max)(v, exact[i].frequency);
            }
        }

        // 2. Prefix search — skip words already in exactMap
        {
            int maxDepth = 0;
            if (pinyin.size() == 1) maxDepth = 6;
            else if (pinyin.size() == 2) maxDepth = 5;
            int maxPerKey = (pinyin.size() >= 4) ? 5 : 3;
            prefixSearch(pinyin, prefixMap, maxPerKey, maxDepth);
        }
        // Remove prefix results that also appear in exact results (exact wins)
        for (auto it = prefixMap.begin(); it != prefixMap.end(); ) {
            if (exactMap.count(it->first)) it = prefixMap.erase(it);
            else ++it;
        }

        // 3. Output: exact matches first (by freq), then prefix matches (by freq)
        out.reserve(exactMap.size() + prefixMap.size());
        for (auto& kv : exactMap) out.push_back(kv);
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        size_t exactSize = out.size();
        for (auto& kv : prefixMap) out.push_back(kv);
        std::sort(out.begin() + exactSize, out.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        if (maxResults > 0 && out.size() > maxResults) out.resize(maxResults);
        return out;
    }

    size_t stateCount() const { return m_version >= 3 ? m_keyCount : (m_header ? m_header->state_count : 0); }
    size_t entryCount() const { return m_header ? m_header->entry_count : 0; }

private:
    char*            m_base = nullptr;
    DictFileHeader*  m_header = nullptr;
    uint32_t         m_version = 0;

    // v2 DAT
    uint32_t*        m_base_array = nullptr;
    uint32_t*        m_check_array = nullptr;
    uint32_t*        m_entry_offsets = nullptr;

    // v3 sorted array
    uint32_t         m_keyCount = 0;
    uint32_t*        m_keyOffsets = nullptr;
    uint32_t*        m_keyStarts = nullptr;
    uint32_t*        m_keyCounts = nullptr;
    const char*      m_keyPool = nullptr;
    uint32_t         m_keyPoolSize = 0;

    // Common
    DictFileEntry*   m_entry_index = nullptr;
    const char*      m_string_pool = nullptr;
};
