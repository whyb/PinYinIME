// test/test_client.cpp — IPC test client that simulates user typing
//
// Connects to the test dict service (TestPinyinIME_* IPC names),
// simulates typing pinyin character by character, and prints
// candidates for each keystroke.
//
// Usage: test_client.exe [--verbose]
//   --verbose  Print all candidates with frequencies
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <cstdarg>

#include "../shared/unique_handle.h"
#include "../shared/ipc_protocol.h"
#include "test_common.h"

static bool g_verbose = false;

static void LOG(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

// ═══════════════════════════════════════════════════════════════════════════
// Simplified IPC client (no reconnect, no cache, pure query)
// ═══════════════════════════════════════════════════════════════════════════

class TestIpcClient {
public:
    bool connect() {
        // Open shared memory
        unique_handle hMapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, TEST_IPC_MAPPING));
        if (!hMapping.valid()) {
            LOG("  FAIL: Cannot open mapping (err=%lu) — service not running?\n", GetLastError());
            return false;
        }

        void* view = MapViewOfFile(hMapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!view) {
            LOG("  FAIL: MapViewOfFile (err=%lu)\n", GetLastError());
            return false;
        }

        // Open events
        unique_handle hQuery(OpenEventW(EVENT_MODIFY_STATE, FALSE, TEST_IPC_EVT_QUERY));
        unique_handle hReply(OpenEventW(SYNCHRONIZE, FALSE, TEST_IPC_EVT_REPLY));
        if (!hQuery.valid() || !hReply.valid()) {
            LOG("  FAIL: Cannot open events (err=%lu)\n", GetLastError());
            UnmapViewOfFile(view);
            return false;
        }

        // Open mutex (best-effort)
        unique_handle hMutex(OpenMutexW(SYNCHRONIZE, FALSE, TEST_IPC_MUTEX));

        m_ipcView.reset(view);
        m_ipcMapping.reset(hMapping.release());
        m_hEvtQuery.reset(hQuery.release());
        m_hEvtReply.reset(hReply.release());
        m_hIpcMutex.reset(hMutex.release());

        LOG("  Connected (mutex=%s)\n", m_hIpcMutex.valid() ? "yes" : "no");
        return true;
    }

    void disconnect() {
        if (m_hIpcMutex.valid()) ReleaseMutex(m_hIpcMutex.get());
        m_hIpcMutex.reset();
        m_ipcView.reset();
        m_ipcMapping.reset();
        m_hEvtQuery.reset();
        m_hEvtReply.reset();
    }

    std::vector<std::pair<std::string, int>> query(const std::string& pinyin) {
        std::vector<std::pair<std::string, int>> results;
        if (pinyin.empty()) return results;

        std::string key = pinyin;
        if (key.size() >= IPC_MAX_INPUT_LEN) key.resize(IPC_MAX_INPUT_LEN - 1);

        void* base = m_ipcView.get();
        if (!base) return results;
        IpcHeader* hdr = ipcHeader(base);

        // Acquire mutex
        if (m_hIpcMutex.valid()) {
            DWORD wr = WaitForSingleObject(m_hIpcMutex.get(), IPC_QUERY_TIMEOUT_MS);
            if (wr != WAIT_OBJECT_0 && wr != WAIT_ABANDONED) return results;
        }

        // Check channel
        uint32_t currentStatus = hdr->status;
        if (currentStatus != IPC_STATUS_IDLE && currentStatus != IPC_STATUS_REPLY_READY) {
            if (m_hIpcMutex.valid()) ReleaseMutex(m_hIpcMutex.get());
            return results;
        }

        // Write query
        char* input = ipcInputBuf(base);
        size_t len = key.size() + 1;
        memcpy(input, key.c_str(), len);

        hdr->query_id = static_cast<uint32_t>(
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr->query_id)));
        hdr->input_len = static_cast<uint32_t>(len);
        hdr->output_count = 0;
        hdr->output_total = 0;
        hdr->error_code = IPC_ERROR_NONE;

        MemoryBarrier();

        InterlockedExchange(reinterpret_cast<volatile LONG*>(&hdr->status),
                            IPC_STATUS_QUERY_PENDING);
        SetEvent(m_hEvtQuery.get());

        // Wait for reply
        DWORD waitResult = WaitForSingleObject(m_hEvtReply.get(), IPC_QUERY_TIMEOUT_MS);

        if (waitResult != WAIT_OBJECT_0) {
            // Timeout or error — reset channel and return empty
            if (waitResult == WAIT_TIMEOUT) {
                InterlockedExchange(reinterpret_cast<volatile LONG*>(&hdr->status), IPC_STATUS_IDLE);
            }
            if (m_hIpcMutex.valid()) ReleaseMutex(m_hIpcMutex.get());
            return results;
        }

        // Read results (auto-reset event already consumed by WaitForSingleObject)
        uint32_t replyStatus = hdr->status;
        if (replyStatus != IPC_STATUS_REPLY_READY) {
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&hdr->status), IPC_STATUS_IDLE);
            if (m_hIpcMutex.valid()) ReleaseMutex(m_hIpcMutex.get());
            return results;
        }

        uint32_t count = hdr->output_count;
        if (count > IPC_MAX_CANDIDATES) count = IPC_MAX_CANDIDATES;

        if (count > 0) {
            IpcCandidate* candidates = ipcOutputBuf(base);
            results.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t wordOff = candidates[i].word_offset;
                if (wordOff >= IPC_STRING_AREA_OFFSET &&
                    wordOff < IPC_STRING_AREA_OFFSET + IPC_STRING_AREA_SIZE) {
                    const char* word = static_cast<const char*>(base) + wordOff;
                    results.emplace_back(std::string(word), candidates[i].frequency);
                }
            }
        }

        InterlockedExchange(reinterpret_cast<volatile LONG*>(&hdr->status), IPC_STATUS_IDLE);
        if (m_hIpcMutex.valid()) ReleaseMutex(m_hIpcMutex.get());
        return results;
    }

    static bool waitForService(DWORD timeoutMs = 10000) {
        LOG("Waiting for test service...\n");
        DWORD start = GetTickCount();
        while (GetTickCount() - start < timeoutMs) {
            unique_handle hReady(OpenEventW(SYNCHRONIZE, FALSE, TEST_SERVICE_READY));
            if (hReady.valid()) {
                DWORD r = WaitForSingleObject(hReady.get(), 1000);
                if (r == WAIT_OBJECT_0) {
                    LOG("Service ready!\n");
                    return true;
                }
            }
            Sleep(200);
        }
        LOG("Timeout waiting for service\n");
        return false;
    }

private:
    unique_handle m_ipcMapping;
    scoped_unmap  m_ipcView;
    unique_handle m_hEvtQuery;
    unique_handle m_hEvtReply;
    unique_handle m_hIpcMutex;
};

// ═══════════════════════════════════════════════════════════════════════════
// Test cases
// ═══════════════════════════════════════════════════════════════════════════

struct TestCase {
    const char* name;
    const char* pinyin;      // Full pinyin to type character by character
    const char* description;
};

static TestCase g_tests[] = {
    {"basic",       "nihao",     "ni hao → 你好"},
    {"sh-zh-fuzzy", "shenme",    "shen me → 什么"},
    {"long-pinyin", "zhongguo",  "zhong guo → 中国"},
    {"single-char", "a",         "单字母前缀搜索"},
    {"compound",    "xianshi",   "xian shi → 显示/先是"},
    {"ch-fuzzy",    "chifan",    "chi fan → 吃饭"},
    {"abbrev",      "nh",        "简拼 nihao → 你好"},
    {"multi-syl",   "shijie",    "shi jie → 世界"},
    {"prefix",      "ha",        "ha 前缀 → 哈/还/海..."},
};

void runTypingSimulation(TestIpcClient& client, const TestCase& tc) {
    LOG("\n");
    LOG("═══════════════════════════════════════════\n");
    LOG("  Test: %s — \"%s\" (%s)\n", tc.name, tc.pinyin, tc.description);
    LOG("═══════════════════════════════════════════\n");

    std::string buffer;
    for (size_t i = 0; i < strlen(tc.pinyin); i++) {
        buffer += tc.pinyin[i];
        LOG("\n[%c] buffer=\"%s\"\n", tc.pinyin[i], buffer.c_str());

        // Delay between queries — IPC protocol uses synchronous shared memory
        Sleep(100);

        DWORD t0 = GetTickCount();
        auto candidates = client.query(buffer);
        DWORD elapsed = GetTickCount() - t0;

        if (candidates.empty()) {
            LOG("  → NO CANDIDATES (took %lums) ⚠️\n", elapsed);
        } else {
            LOG("  → %zu candidates (took %lums):\n", candidates.size(), elapsed);
            size_t show = g_verbose ? candidates.size() : (std::min)(candidates.size(), (size_t)12);
            for (size_t j = 0; j < show; j++) {
                LOG("    %2zu. %-12s  freq=%d\n", j + 1, candidates[j].first.c_str(), candidates[j].second);
            }
            if (candidates.size() > show) {
                LOG("    ... and %zu more\n", candidates.size() - show);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    LOG("=== Test PinyinIME IPC Client ===\n");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            g_verbose = true;
        }
    }

    // Wait for service
    if (!TestIpcClient::waitForService(15000)) {
        LOG("FAIL: Test service did not become ready within 15s\n");
        LOG("Make sure test_service.exe is running first.\n");
        return 1;
    }

    // Connect
    TestIpcClient client;
    if (!client.connect()) {
        LOG("FAIL: Cannot connect to test service\n");
        return 1;
    }

    // Run all test cases
    for (auto& tc : g_tests) {
        runTypingSimulation(client, tc);
        Sleep(200);  // Let IPC channel settle between test cases
    }

    // Quick validation with retry (IPC channel may need settling)
    Sleep(2000);
    int passed = 0, failed = 0;

    auto validateQuery = [&](const char* key, int retries = 5) {
        for (int r = 0; r < retries; r++) {
            auto candidates = client.query(key);
            if (!candidates.empty()) {
                LOG("  '%s' → %zu results  ✅ PASS\n", key, candidates.size());
                return true;
            }
            if (r < retries - 1) Sleep(500);
        }
        LOG("  '%s' → 0 results  ❌ FAIL\n", key);
        return false;
    };

    if (validateQuery("nihao")) passed++; else failed++;
    if (validateQuery("zhongguo")) passed++; else failed++;
    if (validateQuery("shijie")) passed++; else failed++;

    // Summary
    LOG("\n");
    LOG("═══════════════════════════════════════════\n");
    LOG("  Test Summary\n");
    LOG("═══════════════════════════════════════════\n");
    if (failed == 0) {
        LOG("  ✅ All validation queries PASSED (%d checks)\n", passed);
    } else {
        LOG("  ❌ %d FAILED, %d passed\n", failed, passed);
    }

    client.disconnect();
    return failed > 0 ? 1 : 0;
}
