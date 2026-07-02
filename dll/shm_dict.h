// dll/shm_dict.h — Shared-memory flat trie dictionary
//
// Replaces the pointer-based TrieDict with an offset-based layout that lives
// entirely inside a named file mapping (CreateFileMapping).  The first process
// to finish full background loading serializes its TrieDict into this format;
// every subsequent process maps it read-only and queries directly — no YAML
// parsing, no per-process heap copy of the trie.
//
// Binary layout inside the mapping (all offsets relative to mapping base):
//
//   [ShmHeader  64 B]  magic, version, root_node_offset, key_count,
//                       state (atomic LONG), node_count, string_table_*, …
//   [ShmNode[]  node_count × 112 B]
//       int32_t children[26]   // byte offset of child node; 0 = no child
//       int32_t entries_offset // byte offset of ShmEntry array; 0 = no entries
//       int32_t entries_count  // number of entries at this node
//   [ShmEntry[] blocks]        // variable-length, concatenated per-node
//       int32_t word_offset    // byte offset into string table
//       int32_t freq
//   [String table]             // null-terminated UTF-8 words, deduplicated
//
// All multi-byte fields are little-endian (native Windows).

#ifndef SHM_DICT_H
#define SHM_DICT_H

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <atomic>
#include "trie_dict.h"

// ── Constants ───────────────────────────────────────────────────────────

constexpr uint32_t SHM_DICT_MAGIC      = 0x31424450;  // 'PDB1' (Pinyin Dict Binary v1)
constexpr uint32_t SHM_DICT_VERSION    = 1;
constexpr uint32_t SHM_DICT_MAX_SIZE   = 80 * 1024 * 1024;  // 80 MB — enough for ~35 MB serialized trie
constexpr uint32_t SHM_HEADER_SIZE     = 64;
constexpr uint32_t SHM_NODE_SIZE       = 112;   // 26×4 + 4 + 4
constexpr uint32_t SHM_ENTRY_SIZE      = 8;     // word_offset + freq

// ── Dictionary state flags ──────────────────────────────────────────────

enum DictState : LONG {
    DICT_STATE_EMPTY       = 0,  // No data written yet
    DICT_STATE_CHARS_ONLY  = 1,  // Character-level data only (sync load done)
    DICT_STATE_FULL_READY  = 2,  // Full dictionary loaded and ready
};

// ── Shared-memory structures (must be trivially copyable / POD) ─────────

#pragma pack(push, 1)
struct ShmHeader {
    uint32_t magic;               // SHM_DICT_MAGIC
    uint32_t version;             // SHM_DICT_VERSION
    uint32_t total_size;          // Bytes actually used in this mapping
    uint32_t root_node_offset;    // Byte offset of root ShmNode from mapping base
    uint32_t key_count;           // Total number of terminal keys
    volatile LONG state;           // DictState (atomic read across processes)
    uint32_t node_count;          // Number of ShmNode entries
    uint32_t entry_count;         // Total number of ShmEntry records
    uint32_t string_table_offset; // Byte offset of string table from mapping base
    uint32_t string_table_size;   // Bytes used by string table
    uint32_t reserved[6];         // Pad to 64 bytes (10 used + 6 = 16 dwords = 64 bytes)
};

struct ShmNode {
    int32_t children[26];         // Byte offset of child node; 0 = no child
    int32_t entries_offset;       // Byte offset of ShmEntry array; 0 = no entries
    int32_t entries_count;        // Number of entries
};

struct ShmEntry {
    int32_t word_offset;          // Byte offset of null-terminated word in string table
    int32_t freq;                 // Frequency (higher = more common)
};
#pragma pack(pop)

static_assert(sizeof(ShmHeader) == 64, "ShmHeader must be 64 bytes");
static_assert(sizeof(ShmNode)   == 112, "ShmNode must be 112 bytes");
static_assert(sizeof(ShmEntry)  == 8, "ShmEntry must be 8 bytes");

// ── FlatTrie: read-only query interface over shared memory ──────────────

class FlatTrie {
public:
    FlatTrie() = default;

    // Attach to an already-mapped shared memory region.
    // Does NOT take ownership of the mapping — caller must keep it mapped.
    void setBase(void* base);

    // Returns true if the trie is attached and ready for queries.
    bool isReady() const;

    // Re-read the header state. Returns the current DictState.
    LONG refreshState();

    // ── Query interface (matches TrieDict signatures) ───────────────────

    // Exact key lookup. Returns nullptr if not found.
    // The returned pointer is valid until the next find() call that
    // materializes different entries, or until the mapping is unmapped.
    const std::vector<TrieDict::Entry>* find(const std::string& key);

    // Prefix search. Fills 'out' with (word → max_freq) pairs.
    // maxDepth=0 means unlimited depth.
    void prefixSearch(const std::string& prefix,
                      std::unordered_map<std::string, int>& out,
                      int maxPerKey = 3,
                      int maxDepth = 0) const;

    // Number of terminal keys in the trie.
    size_t keyCount() const;

private:
    char*     m_base   = nullptr;  // Mapping base address
    ShmHeader* m_header = nullptr;  // → ShmHeader

    // Per-process materialization cache.
    // Keyed by the full pinyin key string.
    // Invalidated when refreshState() detects a state change.
    mutable std::unordered_map<std::string, std::vector<TrieDict::Entry>> m_cache;
    LONG m_cachedState = DICT_STATE_EMPTY;

    // Walk the trie following 'key', return pointer to the terminal ShmNode
    // (or nullptr if the key path doesn't exist).
    ShmNode* walkToNode(const std::string& key) const;

    // Materialize ShmEntry records for a node into m_cache[key].
    void materialize(const std::string& key, ShmNode* node);

    // Recursive helper for prefixSearch.
    void collectPrefix(ShmNode* node, int depthLeft, int maxPerKey,
                       std::unordered_map<std::string, int>& out) const;

    // Inline helpers
    inline ShmNode* nodeAt(uint32_t offset) const {
        return (offset > 0) ? reinterpret_cast<ShmNode*>(m_base + offset) : nullptr;
    }
    inline const char* stringAt(uint32_t offset) const {
        return (offset > 0) ? (m_base + offset) : nullptr;
    }
};

// ── Serialization: TrieDict → shared memory ────────────────────────────

// Serialize a locally-built (pointer-based) TrieDict into an already-mapped
// shared memory region.  The mapping must be at least SHM_DICT_MAX_SIZE bytes
// and mapped with FILE_MAP_WRITE access.
//
// Returns true on success.  On failure the mapping content is undefined.
bool serializeTrieToSharedMemory(void* mappedBase, TrieDict& source);

#endif // SHM_DICT_H
