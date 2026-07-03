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
        m_version = m_header->version;
        if (m_version >= 3) {
            // v3: sorted array format
            m_keyCount   = m_header->root_base;
            m_keyOffsets = reinterpret_cast<uint32_t*>(m_base + m_header->base_array_offset);
            m_keyStarts  = reinterpret_cast<uint32_t*>(m_base + m_header->check_array_offset);
            m_keyCounts  = reinterpret_cast<uint32_t*>(m_base + m_header->entry_offset_array_offset);
            m_keyPool    = m_base + m_header->reserved[0];
        } else {
            // v2: DAT format
            m_base_array    = reinterpret_cast<uint32_t*>(m_base + m_header->base_array_offset);
            m_check_array   = reinterpret_cast<uint32_t*>(m_base + m_header->check_array_offset);
            m_entry_offsets = reinterpret_cast<uint32_t*>(m_base + m_header->entry_offset_array_offset);
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
