// trie_dict.cpp — TrieDict 实现
//
// 针对拼音输入法的前缀查找优化:
//   - 键仅包含小写字母 a-z (26 分支)
//   - 终端节点持有已排序词条列表
//   - 前缀查找 O(L + M), 非 O(N) 全表扫描

#include "trie_dict.h"
#include <algorithm>
#include <cstring>

// ==================== Node ====================

TrieDict::Node::Node() {
    std::memset(children, 0, sizeof(children));
    entries = nullptr;
}

TrieDict::Node::~Node() {
    delete entries;
    for (int i = 0; i < 26; i++) {
        delete children[i];
    }
}

// ==================== TrieDict ====================

TrieDict::TrieDict() : m_root(new Node()), m_keyCount(0) {}

TrieDict::~TrieDict() {
    destroy(m_root);
}

TrieDict::TrieDict(TrieDict&& other) noexcept
    : m_root(other.m_root), m_keyCount(other.m_keyCount) {
    other.m_root = nullptr;
    other.m_keyCount = 0;
}

TrieDict& TrieDict::operator=(TrieDict&& other) noexcept {
    if (this != &other) {
        destroy(m_root);
        m_root = other.m_root;
        m_keyCount = other.m_keyCount;
        other.m_root = nullptr;
        other.m_keyCount = 0;
    }
    return *this;
}

void TrieDict::destroy(Node* node) {
    if (!node) return;
    delete node;  // destructor recurses into children
}

// ==================== 插入 ====================

void TrieDict::insert(const std::string& key,
                      const std::string& word, int freq) {
    if (key.empty()) return;

    Node* cur = m_root;
    for (char ch : key) {
        int idx = ch - 'a';
        if (idx < 0 || idx >= 26) return;  // 非法字符, 安全忽略
        if (!cur->children[idx]) {
            cur->children[idx] = new Node();
        }
        cur = cur->children[idx];
    }

    // cur 现在是终端节点
    if (!cur->entries) {
        cur->entries = new std::vector<Entry>();
        m_keyCount++;
    }
    cur->entries->push_back(Entry(word, freq));
}

// ==================== 精确查找 ====================

const std::vector<TrieDict::Entry>* TrieDict::find(const std::string& key) const {
    Node* cur = m_root;
    for (char ch : key) {
        int idx = ch - 'a';
        if (idx < 0 || idx >= 26) return nullptr;
        if (!cur->children[idx]) return nullptr;
        cur = cur->children[idx];
    }
    return cur->entries;  // 可能为 nullptr (前缀存在但不是终端)
}

// ==================== 前缀查找 ====================

void TrieDict::collectEntries(Node* node,
                              std::unordered_map<std::string, int>& out,
                              int maxPerKey,
                              int depthLeft,
                              int& collected) {
    if (!node) return;
    if (depthLeft == 0) return;  // 达到深度限制, 不再向下

    // 收集当前节点的词条 (如果是终端)
    if (node->entries && !node->entries->empty()) {
        int take = (std::min)(maxPerKey, (int)node->entries->size());
        for (int i = 0; i < take; i++) {
            const Entry& e = (*node->entries)[i];
            auto it = out.find(e.word);
            if (it == out.end() || e.freq > it->second) {
                out[e.word] = e.freq;
            }
            collected++;
        }
    }

    // 递归搜索所有子节点, depthLeft 递减
    int nextDepth = (depthLeft > 0) ? depthLeft - 1 : depthLeft;
    if (nextDepth == 0 && depthLeft > 0) return;  // 不再深入下一层
    for (int i = 0; i < 26; i++) {
        if (node->children[i]) {
            collectEntries(node->children[i], out, maxPerKey, nextDepth, collected);
        }
    }
}

void TrieDict::prefixSearch(const std::string& prefix,
                            std::unordered_map<std::string, int>& out,
                            int maxPerKey,
                            int maxDepth) const {
    if (prefix.empty()) return;

    // 1. 走到前缀对应的节点
    Node* cur = m_root;
    for (char ch : prefix) {
        int idx = ch - 'a';
        if (idx < 0 || idx >= 26) return;
        if (!cur->children[idx]) return;  // 前缀不存在
        cur = cur->children[idx];
    }

    // 2. 从该节点开始 DFS 收集终端词条
    //    depthLeft: 从 cur 的子节点开始算, 还需向下搜索的层数
    //               maxDepth=0 表示不限制, 传 -1 表示无限制
    int depthLeft = (maxDepth <= 0) ? -1 : maxDepth;
    int collected = 0;

    // 先收集当前节点 (prefix 本身是终端 key 的情况)
    if (cur->entries && !cur->entries->empty()) {
        int take = (std::min)(maxPerKey, (int)cur->entries->size());
        for (int i = 0; i < take; i++) {
            const Entry& e = (*cur->entries)[i];
            auto it = out.find(e.word);
            if (it == out.end() || e.freq > it->second) {
                out[e.word] = e.freq;
            }
            collected++;
        }
    }

    // 再递归子节点 (带深度限制)
    for (int i = 0; i < 26; i++) {
        if (cur->children[i]) {
            collectEntries(cur->children[i], out, maxPerKey, depthLeft, collected);
        }
    }
}

// ==================== 遍历 ====================

void TrieDict::traverse(Node* node, std::string& prefix,
                        std::function<void(const std::string&,
                                           std::vector<Entry>&)> fn) {
    if (!node) return;

    if (node->entries && !node->entries->empty()) {
        fn(prefix, *node->entries);
    }

    for (int i = 0; i < 26; i++) {
        if (node->children[i]) {
            prefix.push_back('a' + (char)i);
            traverse(node->children[i], prefix, fn);
            prefix.pop_back();
        }
    }
}

void TrieDict::forEach(std::function<void(const std::string&,
                                          std::vector<Entry>&)> fn) {
    std::string prefix;
    traverse(m_root, prefix, fn);
}

// ==================== 清空 ====================

void TrieDict::clear() {
    destroy(m_root);
    m_root = new Node();
    m_keyCount = 0;
}
