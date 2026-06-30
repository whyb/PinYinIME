// trie_dict.h — 极简拼音输入法 高性能前缀树词库
//
// 针对拼音输入法的查询模式优化:
//   - 键空间: 只有小写字母 a-z (26 个分支)
//   - 操作比例: 前缀查找 >> 精确查找
//   - 数据特点: 写入集中在初始化, 运行时只读
//
// 设计:
//   - 固定 26 子节点数组, 无哈希开销, 缓存友好
//   - 终端节点持有词条向量 (已排序), 非终端节点零分配
//   - 前缀查找沿着树走到前缀节点, 然后 DFS 收集子树中所有终端节点的词条
//
// 复杂度:
//   - 精确查找: O(L)     L = key 长度
//   - 前缀查找: O(L + M) M = 匹配的终端节点数
//   - 插入:     O(L * K) K = 每 key 平均词条数

#ifndef TRIE_DICT_H
#define TRIE_DICT_H

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

class TrieDict {
public:
    struct Entry {
        std::string word;
        int freq;

        Entry() : freq(0) {}
        Entry(const std::string& w, int f) : word(w), freq(f) {}
    };

    TrieDict();
    ~TrieDict();

    // 禁止拷贝
    TrieDict(const TrieDict&) = delete;
    TrieDict& operator=(const TrieDict&) = delete;

    // 移动
    TrieDict(TrieDict&& other) noexcept;
    TrieDict& operator=(TrieDict&& other) noexcept;

    // ── 写入操作 (仅在初始化时调用) ──────────────────
    //
    // 向 key 对应的词条列表追加一条 (word, freq)
    // 若 key 不存在则自动创建节点链
    void insert(const std::string& key, const std::string& word, int freq);

    // ── 读取操作 (运行时热路径) ──────────────────────
    //
    // 精确查找: 返回 key 对应的词条列表指针, 不存在返回 nullptr
    // 词条列表已按 freq 降序排列
    const std::vector<Entry>* find(const std::string& key) const;

    // 前缀查找: 收集所有以 prefix 开头的 key 的词条
    //   - 结果写入 out (word → max freq, 自动去重取最高频)
    //   - 每个终端 key 最多取 maxPerKey 个词条
    //   - maxDepth: 从 prefix 节点起向下搜索的最大深度 (0=不限制)
    void prefixSearch(const std::string& prefix,
                      std::unordered_map<std::string, int>& out,
                      int maxPerKey = 3,
                      int maxDepth = 0) const;

    // ── 批量操作 ─────────────────────────────────────
    //
    // 遍历所有 key → entries
    void forEach(std::function<void(const std::string& key,
                                    std::vector<Entry>& entries)> fn);

    // 获取 key 总数
    size_t keyCount() const { return m_keyCount; }

    // 清空
    void clear();

private:
    struct Node {
        Node* children[26];   // 固定 26 分支 (a-z), 无哈希
        std::vector<Entry>* entries; // 仅终端节点非空 (heap allocated)

        Node();
        ~Node();

        Node(const Node&) = delete;
        Node& operator=(const Node&) = delete;
    };

    Node* m_root;
    size_t m_keyCount;

    // 递归遍历子树, 收集所有终端节点的词条
    // depthLeft: 剩余可向下搜索的深度 (-1=不限制)
    static void collectEntries(Node* node,
                               std::unordered_map<std::string, int>& out,
                               int maxPerKey,
                               int depthLeft,
                               int& collected);

    // 递归遍历所有终端节点
    static void traverse(Node* node, std::string& prefix,
                         std::function<void(const std::string&,
                                            std::vector<Entry>&)> fn);

    // 递归删除
    static void destroy(Node* node);
};

#endif // TRIE_DICT_H
