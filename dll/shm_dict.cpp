// dll/shm_dict.cpp — FlatTrie query implementation + TrieDict→shared-memory serializer
#include "shm_dict.h"
#include <algorithm>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════════════
//  FlatTrie — query interface
// ═══════════════════════════════════════════════════════════════════════════

void FlatTrie::setBase(void* base) {
    m_base   = reinterpret_cast<char*>(base);
    m_header = reinterpret_cast<ShmHeader*>(base);
    m_cache.clear();
    m_cachedState = m_header ? m_header->state : DICT_STATE_EMPTY;
}

bool FlatTrie::isReady() const {
    return m_base != nullptr
        && m_header != nullptr
        && m_header->magic == SHM_DICT_MAGIC
        && m_header->state >= DICT_STATE_CHARS_ONLY;
}

LONG FlatTrie::refreshState() {
    if (!m_header) return DICT_STATE_EMPTY;
    LONG current = m_header->state;
    if (current != m_cachedState) {
        m_cache.clear();
        m_cachedState = current;
    }
    return current;
}

ShmNode* FlatTrie::walkToNode(const std::string& key) const {
    if (!isReady()) return nullptr;
    ShmNode* cur = nodeAt(m_header->root_node_offset);
    if (!cur) return nullptr;

    for (char ch : key) {
        int idx = ch - 'a';
        if (idx < 0 || idx >= 26) return nullptr;
        uint32_t childOff = static_cast<uint32_t>(cur->children[idx]);
        if (childOff == 0) return nullptr;
        cur = nodeAt(childOff);
        if (!cur) return nullptr;
    }
    return cur;
}

void FlatTrie::materialize(const std::string& key, ShmNode* node) {
    std::vector<TrieDict::Entry> vec;
    if (node && node->entries_count > 0 && node->entries_offset > 0) {
        ShmEntry* entries = reinterpret_cast<ShmEntry*>(m_base + node->entries_offset);
        vec.reserve(static_cast<size_t>(node->entries_count));
        for (int32_t i = 0; i < node->entries_count; ++i) {
            const char* word = stringAt(static_cast<uint32_t>(entries[i].word_offset));
            if (word) {
                vec.emplace_back(std::string(word), entries[i].freq);
            }
        }
    }
    m_cache[key] = std::move(vec);
}

const std::vector<TrieDict::Entry>* FlatTrie::find(const std::string& key) {
    refreshState();
    if (!isReady() || key.empty()) return nullptr;

    // Check cache first
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        return &it->second;
    }

    ShmNode* node = walkToNode(key);
    if (!node || node->entries_count == 0) return nullptr;

    materialize(key, node);
    return &m_cache[key];
}

void FlatTrie::collectPrefix(ShmNode* node, int depthLeft, int maxPerKey,
                             std::unordered_map<std::string, int>& out) const {
    if (!node) return;
    if (depthLeft == 0) return;  // reached depth limit

    // Collect entries at this node (if terminal)
    if (node->entries_count > 0 && node->entries_offset > 0) {
        ShmEntry* entries = reinterpret_cast<ShmEntry*>(m_base + node->entries_offset);
        int take = (std::min)(maxPerKey, static_cast<int>(node->entries_count));
        for (int i = 0; i < take; ++i) {
            const char* word = stringAt(static_cast<uint32_t>(entries[i].word_offset));
            if (!word) continue;
            std::string w(word);
            auto it = out.find(w);
            if (it == out.end() || entries[i].freq > it->second) {
                out[w] = entries[i].freq;
            }
        }
    }

    // Recurse into children, decrementing depth limit
    int nextDepth = (depthLeft > 0) ? depthLeft - 1 : depthLeft;
    if (nextDepth == 0 && depthLeft > 0) return;
    for (int i = 0; i < 26; ++i) {
        uint32_t childOff = static_cast<uint32_t>(node->children[i]);
        if (childOff != 0) {
            collectPrefix(nodeAt(childOff), nextDepth, maxPerKey, out);
        }
    }
}

void FlatTrie::prefixSearch(const std::string& prefix,
                            std::unordered_map<std::string, int>& out,
                            int maxPerKey,
                            int maxDepth) const {
    if (!isReady() || prefix.empty()) return;

    // Walk to the prefix node
    ShmNode* cur = nodeAt(m_header->root_node_offset);
    if (!cur) return;

    for (char ch : prefix) {
        int idx = ch - 'a';
        if (idx < 0 || idx >= 26) return;
        uint32_t childOff = static_cast<uint32_t>(cur->children[idx]);
        if (childOff == 0) return;  // prefix doesn't exist
        cur = nodeAt(childOff);
        if (!cur) return;
    }

    // Collect entries at the prefix node itself
    if (cur->entries_count > 0 && cur->entries_offset > 0) {
        ShmEntry* entries = reinterpret_cast<ShmEntry*>(m_base + cur->entries_offset);
        int take = (std::min)(maxPerKey, static_cast<int>(cur->entries_count));
        for (int i = 0; i < take; ++i) {
            const char* word = stringAt(static_cast<uint32_t>(entries[i].word_offset));
            if (!word) continue;
            std::string w(word);
            auto it = out.find(w);
            if (it == out.end() || entries[i].freq > it->second) {
                out[w] = entries[i].freq;
            }
        }
    }

    // Recurse into children
    int depthLeft = (maxDepth <= 0) ? -1 : maxDepth;
    for (int i = 0; i < 26; ++i) {
        uint32_t childOff = static_cast<uint32_t>(cur->children[i]);
        if (childOff != 0) {
            collectPrefix(nodeAt(childOff), depthLeft, maxPerKey, out);
        }
    }
}

size_t FlatTrie::keyCount() const {
    return (m_header && isReady()) ? static_cast<size_t>(m_header->key_count) : 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Serialization: TrieDict → shared-memory flat format
// ═══════════════════════════════════════════════════════════════════════════

// Internal builder structures used during serialization
namespace {

struct BuildNode {
    int32_t offset = 0;               // byte offset of this node in the mapping
    int32_t children[26] = {};        // byte offsets of children (0 = none)
    std::vector<TrieDict::Entry> entries; // accumulated entries
};

// Helper: allocate a new node at the next free position in the node array.
// Returns its byte offset from mapping base.
inline int32_t allocNode(std::vector<BuildNode>& nodes, int32_t nodeBase) {
    int32_t offset = nodeBase + static_cast<int32_t>(nodes.size()) * SHM_NODE_SIZE;
    nodes.emplace_back();
    nodes.back().offset = offset;
    return offset;
}

// Helper: append a string to the string table, return its byte offset.
// Deduplicates — if the string already exists, returns the existing offset.
inline int32_t internString(std::vector<char>& strTab,
                            std::unordered_map<std::string, int32_t>& strMap,
                            const std::string& s) {
    auto it = strMap.find(s);
    if (it != strMap.end()) return it->second;
    int32_t off = static_cast<int32_t>(strTab.size());
    strTab.insert(strTab.end(), s.begin(), s.end());
    strTab.push_back('\0');
    strMap[s] = off;
    return off;
}

} // anonymous namespace

bool serializeTrieToSharedMemory(void* mappedBase, TrieDict& source) {
    if (!mappedBase) return false;

    char* base = reinterpret_cast<char*>(mappedBase);

    // ── Layout offsets ──────────────────────────────────────────────────
    const int32_t headerOff    = 0;
    const int32_t nodeBaseOff  = SHM_HEADER_SIZE;  // nodes start right after header
    const int32_t maxNodeOff   = nodeBaseOff;       // will be updated
    int32_t       entryBaseOff = 0;                 // filled after nodes are built
    int32_t       strTabOff    = 0;                 // filled after entries are built

    // ── Phase 1: Build flat node tree from TrieDict ─────────────────────
    std::vector<BuildNode> nodes;
    // Allocate root node at offset nodeBaseOff
    allocNode(nodes, nodeBaseOff);  // root is nodes[0]

    // Map from original TrieDict Node* to flat node index for dedup of
    // the pointer-based tree structure (not strictly needed for correctness
    // but useful for debugging; we rebuild the tree from the key strings).
    //
    // Strategy: iterate every (key, entries) via forEach, walk the flat node
    // tree character-by-character, allocating new nodes as needed.
    source.forEach([&](const std::string& key,
                       std::vector<TrieDict::Entry>& srcEntries) {
        if (key.empty()) return;

        int32_t curOff = nodeBaseOff;  // start at root
        for (size_t ci = 0; ci < key.size(); ++ci) {
            int idx = key[ci] - 'a';
            if (idx < 0 || idx >= 26) return;  // skip non-[a-z] keys

            // Find the current node in the nodes vector
            BuildNode* curNode = nullptr;
            for (auto& n : nodes) {
                if (n.offset == curOff) { curNode = &n; break; }
            }
            if (!curNode) return;  // should never happen

            int32_t childOff = curNode->children[idx];
            if (childOff == 0) {
                // Need to create a new child node
                childOff = allocNode(nodes, nodeBaseOff);
                curNode->children[idx] = childOff;
            }
            curOff = childOff;
        }

        // curOff is now the terminal node for this key
        BuildNode* termNode = nullptr;
        for (auto& n : nodes) {
            if (n.offset == curOff) { termNode = &n; break; }
        }
        if (!termNode) return;

        // Append srcEntries to terminal node (dedup by word, keep highest freq)
        for (auto& e : srcEntries) {
            bool found = false;
            for (auto& existing : termNode->entries) {
                if (existing.word == e.word) {
                    if (e.freq > existing.freq) existing.freq = e.freq;
                    found = true;
                    break;
                }
            }
            if (!found) {
                termNode->entries.push_back(e);
            }
        }
    }); // end forEach

    // ── Phase 2: Sort entries within each node (descending by freq) ──────
    for (auto& node : nodes) {
        std::sort(node.entries.begin(), node.entries.end(),
            [](const TrieDict::Entry& a, const TrieDict::Entry& b) {
                return a.freq > b.freq;
            });
    }

    // ── Phase 3: Calculate layout ───────────────────────────────────────
    int32_t nodeArrayEnd = nodeBaseOff
        + static_cast<int32_t>(nodes.size()) * SHM_NODE_SIZE;

    // Entry arrays go right after the last node
    entryBaseOff = nodeArrayEnd;

    // String table goes after entries — we'll calculate exact offset after
    // we know the total entry count.
    // For now, lay out entries sequentially and collect strings.
    std::vector<char>       strTab;
    std::unordered_map<std::string, int32_t> strMap;
    std::vector<ShmEntry>   allEntries;  // concatenated entry arrays per node

    for (auto& bn : nodes) {
        if (bn.entries.empty()) {
            bn.offset = 0; // will be overwritten — keep existing node offset
            continue;
        }
        // bn.entries_offset will be set relative to mapping base
        // bn.entries_count = number of entries
        // We'll write the actual offset after computing all entry positions
    }

    // Actually, let's do the layout properly. We need to:
    // 1. Write ShmNodes to base + nodeBaseOff (fixed position)
    // 2. Write ShmEntry arrays sequentially after nodes
    // 3. Write string table after entries

    int32_t entryCursor = entryBaseOff;
    for (auto& bn : nodes) {
        if (bn.entries.empty()) continue;
        // Intern all entry words into string table
        for (auto& e : bn.entries) {
            int32_t wordOff = internString(strTab, strMap, e.word);
            allEntries.push_back({wordOff, e.freq});
        }
    }

    strTabOff = entryBaseOff
        + static_cast<int32_t>(allEntries.size()) * SHM_ENTRY_SIZE;

    // ── Phase 4: Write to shared memory ──────────────────────────────────
    // Zero out the region first
    uint32_t totalUsed = static_cast<uint32_t>(strTabOff + strTab.size());
    if (totalUsed > SHM_DICT_MAX_SIZE) {
        OutputDebugStringA("[PinyinIME] ERROR: dict serialization exceeds max size\n");
        return false;
    }
    std::memset(mappedBase, 0, totalUsed);

    // ── Write nodes ──────────────────────────────────────────────────────
    // Need to go back and assign correct entry offsets for each node
    entryCursor = entryBaseOff;
    int32_t nodeIdx = 0;
    int32_t entryIdxGlobal = 0;
    uint32_t totalEntryCount = 0;
    uint32_t totalKeyCount = 0;

    for (auto& bn : nodes) {
        ShmNode* shm = reinterpret_cast<ShmNode*>(base + bn.offset);
        for (int i = 0; i < 26; ++i) {
            shm->children[i] = bn.children[i];  // already byte offsets, 0 = null
        }
        if (!bn.entries.empty()) {
            shm->entries_offset = entryCursor;
            shm->entries_count  = static_cast<int32_t>(bn.entries.size());
            entryCursor += shm->entries_count * SHM_ENTRY_SIZE;
            totalEntryCount += static_cast<uint32_t>(bn.entries.size());
            totalKeyCount++;
        } else {
            shm->entries_offset = 0;
            shm->entries_count  = 0;
        }
        nodeIdx++;
    }

    // ── Write entries ────────────────────────────────────────────────────
    ShmEntry* shmEntries = reinterpret_cast<ShmEntry*>(base + entryBaseOff);
    for (size_t i = 0; i < allEntries.size(); ++i) {
        shmEntries[i] = allEntries[i];
    }

    // ── Write string table ───────────────────────────────────────────────
    std::memcpy(base + strTabOff, strTab.data(), strTab.size());

    // ── Write header ─────────────────────────────────────────────────────
    ShmHeader* hdr = reinterpret_cast<ShmHeader*>(base + headerOff);
    hdr->magic              = SHM_DICT_MAGIC;
    hdr->version            = SHM_DICT_VERSION;
    hdr->total_size         = totalUsed;
    hdr->root_node_offset   = nodeBaseOff;  // root is always the first node
    hdr->key_count          = totalKeyCount;
    hdr->state              = DICT_STATE_CHARS_ONLY;  // caller will upgrade
    hdr->node_count         = static_cast<uint32_t>(nodes.size());
    hdr->entry_count        = totalEntryCount;
    hdr->string_table_offset = static_cast<uint32_t>(strTabOff);
    hdr->string_table_size  = static_cast<uint32_t>(strTab.size());

    char buf[256];
    snprintf(buf, sizeof(buf),
        "[PinyinIME] Serialized dict: %zu nodes, %u keys, %u entries, %u str bytes → %u total\n",
        nodes.size(), totalKeyCount, totalEntryCount,
        static_cast<uint32_t>(strTab.size()), totalUsed);
    OutputDebugStringA(buf);

    return true;
}
