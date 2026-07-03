// dll/dict_client.h — IPC-based dictionary client (drop-in replacement for PinyinDict)
//
// This class provides the same `find()` and `prefixSearch()` interface as
// PinyinDict, but routes all queries through the IPC channel to the background
// dictionary service. This eliminates in-process dictionary loading entirely.
//
// MIGRATION PATH FROM OLD PinyinDict:
//   旧代码:
//     PinyinDict m_dict;
//     m_dict.init();
//     auto* entries = m_dict.find("nihao");
//
//   新代码:
//     DictClient m_dict;
//     m_dict.init();              // connects to IPC, waits for service
//     auto entries = m_dict.find("nihao");  // returns vector<pair<string,int>>
//
// IMPORTANT DIFFERENCES:
//   1. find() returns std::vector<Entry> by value (not pointer to internal state).
//      This is because the data comes from shared memory that may change between calls.
//   2. prefixSearch() sends the prefix to the service, which does both exact
//      lookup AND prefix expansion internally.
//   3. trySwapBackground() is a no-op — the service handles all dictionary updates.
//   4. shutdown() disconnects from the IPC channel.
//
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include <algorithm>

#include "ipc_client.h"
#include "../shared/ime_ipc.h"

class DictClient {
public:
    struct Entry {
        std::string word;
        int freq;
        Entry() : freq(0) {}
        Entry(const std::string& w, int f) : word(w), freq(f) {}
    };

    DictClient() = default;

    // ── Initialize: connect to the dict service ──────────────────────────

    void init() {
        // Wait for service readiness
        m_ipc.connect();

        if (!m_ipc.isConnected()) {
            // Service not ready yet — try waiting
            for (int retry = 0; retry < 60; ++retry) {  // Up to 30s
                if (IpcClient::isServiceReady()) {
                    if (m_ipc.connect()) break;
                }
                Sleep(500);
            }
        }

        if (m_ipc.isConnected()) {
            OutputDebugStringA("[DictClient] Connected to dict service\n");
        } else {
            OutputDebugStringA("[DictClient] WARNING: Could not connect to dict service\n");
        }
    }

    // ── Shutdown ────────────────────────────────────────────────────────

    void shutdown() {
        m_ipc.disconnect();
    }

    // ── trySwapBackground: no-op in IPC mode ────────────────────────────
    //
    // In the old architecture, this checked whether the background loading
    // thread had finished and swapped dictionaries. In IPC mode, the service
    // handles all updates — this is a compatibility no-op.

    void trySwapBackground() {
        // No-op: service manages dictionary lifecycle
    }

    // ── Exact key lookup ────────────────────────────────────────────────
    //
    // Queries the service for the exact pinyin key.
    // Returns entries sorted by frequency (descending).
    // Returns empty vector if key not found, service unreachable, or timeout.

    std::vector<Entry> find(const std::string& key) {
        std::vector<Entry> result;

        if (key.empty() || !m_ipc.isConnected()) return result;

        auto candidates = m_ipc.query(key);
        result.reserve(candidates.size());
        for (auto& c : candidates) {
            result.emplace_back(std::move(c.first), c.second);
        }
        return result;
    }

    // ── Prefix search ───────────────────────────────────────────────────
    //
    // Sends the prefix to the service. The service does both exact lookup
    // and prefix expansion, returning combined results.
    //
    // maxPerKey, maxDepth: hints for the service (prefix expansion params).
    //   These were used by the local trie traversal. In IPC mode, the service
    //   applies its own prefix expansion logic.

    void prefixSearch(const std::string& prefix,
                      std::unordered_map<std::string, int>& out,
                      int maxPerKey = 3,
                      int maxDepth = 0) {
        if (prefix.empty() || !m_ipc.isConnected()) return;

        auto candidates = m_ipc.query(prefix);
        for (auto& c : candidates) {
            auto it = out.find(c.first);
            if (it == out.end() || c.second > it->second) {
                out[c.first] = c.second;
            }
        }
    }

    // ── Check if connected ──────────────────────────────────────────────

    bool isReady() const {
        return m_ipc.isConnected();
    }

private:
    IpcClient m_ipc;
};
