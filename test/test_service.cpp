// test/test_service.cpp — Standalone test dict service
//
// Starts a lightweight dictionary service with TEST IPC names (isolated
// from the production PinyinIME service). Loads dict.bin and serves
// queries via the same IPC protocol as the production service.
//
// Usage: test_service.exe [path\to\dict.bin]
//   If no path given, looks for dict.bin next to the executable.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdarg>
#include <unordered_map>

#include "../shared/dict_binary.h"
#include "../shared/hot_dict.h"
#include <unordered_set>
#include "../shared/ipc_protocol.h"
#include "../shared/unique_handle.h"
#include "test_common.h"

// ═══════════════════════════════════════════════════════════════════════════

static void LOG(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

// ═══════════════════════════════════════════════════════════════════════════

class TestDictService {
public:
    bool initialize(const wchar_t* dictPath) {
        LOG("[TestService] Loading dict.bin: %ls\n", dictPath);

        // Open dict.bin
        HANDLE hFile = CreateFileW(dictPath, GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            LOG("[TestService] FAIL: Cannot open dict.bin (err=%lu)\n", GetLastError());
            return false;
        }

        // Read header
        DictFileHeader diskHeader;
        DWORD bytesRead = 0;
        if (!ReadFile(hFile, &diskHeader, sizeof(diskHeader), &bytesRead, nullptr) ||
            bytesRead != sizeof(diskHeader)) {
            LOG("[TestService] FAIL: Cannot read header\n");
            CloseHandle(hFile);
            return false;
        }
        LOG("[TestService] Header: magic=0x%08X ver=%u size=%u entries=%u\n",
            diskHeader.magic, diskHeader.version, diskHeader.total_file_size, diskHeader.entry_count);

        if (diskHeader.magic != DICT_BIN_MAGIC) {
            LOG("[TestService] FAIL: Bad magic\n");
            CloseHandle(hFile);
            return false;
        }

        // Memory-map dict.bin
        HANDLE hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        CloseHandle(hFile);
        if (!hMapping) {
            LOG("[TestService] FAIL: CreateFileMapping (err=%lu)\n", GetLastError());
            return false;
        }

        void* view = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
        if (!view) {
            LOG("[TestService] FAIL: MapViewOfFile (err=%lu)\n", GetLastError());
            CloseHandle(hMapping);
            return false;
        }

        if (!m_dictReader.attach(view)) {
            LOG("[TestService] FAIL: BinaryDictReader::attach() failed\n");
            UnmapViewOfFile(view);
            CloseHandle(hMapping);
            return false;
        }

        m_dictView.reset(view);
        m_dictMapping.reset(hMapping);
        // Quick smoke test: verify dict lookups work
        uint32_t tc = 0;
        m_dictReader.find("ni", tc);
        if (tc == 0) {
            LOG("[TestService] FAIL: dict lookup broken (find('ni') returned 0)\n");
            return false;
        }
        LOG("[TestService] Dict loaded OK: %zu keys, %zu entries (find('ni')=%u)\n",
            m_dictReader.stateCount(), m_dictReader.entryCount(), tc);

        // Load hot word cache
        {
            char hotPath[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, dictPath, -1, hotPath, MAX_PATH, nullptr, nullptr);
            char* d = strrchr(hotPath, '.');
            if (d) *d = '\0';
            strcat_s(hotPath, "_hotwords.bin");
            if (m_hotCache.load(hotPath)) {
                LOG("[TestService] Hot cache: %zu entries\n", m_hotCache.count());
            }
        }
        return true;
    }

    bool createIpcChannel() {
        SandboxSecurityAttributes sd;
        LOG("[TestService] Creating IPC objects... (DACL: %s)\n",
            sd.hasDacl() ? "OK" : "NULL");

        // Create events (manual-reset to prevent signal loss)
        m_hEvtQuery.reset(CreateEventW(&sd, TRUE, FALSE, TEST_IPC_EVT_QUERY));
        if (!m_hEvtQuery.valid()) {
            LOG("[TestService] FAIL: EvtQuery (err=%lu)\n", GetLastError());
            return false;
        }

        m_hEvtReply.reset(CreateEventW(&sd, TRUE, FALSE, TEST_IPC_EVT_REPLY));
        if (!m_hEvtReply.valid()) {
            LOG("[TestService] FAIL: EvtReply (err=%lu)\n", GetLastError());
            return false;
        }

        m_hEvtStop.reset(CreateEventW(&sd, TRUE, FALSE, TEST_IPC_EVT_STOP));
        if (!m_hEvtStop.valid()) {
            LOG("[TestService] FAIL: EvtStop (err=%lu)\n", GetLastError());
            return false;
        }

        // Reset events that may be in signaled state from a previous crashed run
        ResetEvent(m_hEvtQuery.get());
        ResetEvent(m_hEvtReply.get());
        ResetEvent(m_hEvtStop.get());

        // Create shared memory
        m_hIpcMapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, &sd,
            PAGE_READWRITE, 0, IPC_MAPPING_SIZE, TEST_IPC_MAPPING));
        if (!m_hIpcMapping.valid()) {
            LOG("[TestService] FAIL: Mapping (err=%lu)\n", GetLastError());
            return false;
        }
        bool isCreator = (GetLastError() != ERROR_ALREADY_EXISTS);

        void* view = MapViewOfFile(m_hIpcMapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!view) {
            LOG("[TestService] FAIL: MapViewOfFile (err=%lu)\n", GetLastError());
            return false;
        }

        if (isCreator) memset(view, 0, IPC_MAPPING_SIZE);
        IpcHeader* hdr = ipcHeader(view);
        hdr->query_id = 0;
        hdr->status = IPC_STATUS_IDLE;

        m_ipcView.reset(view);

        // Create mutex
        m_hIpcMutex.reset(CreateMutexW(&sd, FALSE, TEST_IPC_MUTEX));
        LOG("[TestService] IPC mutex: %s\n", m_hIpcMutex.valid() ? "OK" : "FAIL");

        // Signal ready
        m_hServiceReady.reset(CreateEventW(&sd, TRUE, FALSE, TEST_SERVICE_READY));
        if (m_hServiceReady.valid()) SetEvent(m_hServiceReady.get());

        LOG("[TestService] IPC channel ready (isCreator=%d)\n", isCreator);
        return true;
    }

    void run() {
        LOG("[TestService] IPC loop starting...\n");
        HANDLE waitHandles[2] = { m_hEvtQuery.get(), m_hEvtStop.get() };
        uint64_t queryCount = 0;

        while (true) {
            DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

            if (waitResult == WAIT_OBJECT_0) {
                // Query event
                ResetEvent(m_hEvtQuery.get());  // Manual-reset: consume
                queryCount++;
                processQuery(queryCount);
            } else if (waitResult == WAIT_OBJECT_0 + 1) {
                // Stop event
                LOG("[TestService] Stop signaled (processed %llu queries)\n", queryCount);
                break;
            } else {
                LOG("[TestService] Wait error (err=%lu)\n", GetLastError());
                break;
            }
        }
    }

    void shutdown() {
        if (m_hEvtQuery.valid()) SetEvent(m_hEvtQuery.get());  // Wake loop
        m_dictView.reset();
        m_dictMapping.reset();
        m_ipcView.reset();
        m_hIpcMapping.reset();
        m_hEvtQuery.reset();
        m_hEvtReply.reset();
        m_hEvtStop.reset();
        m_hIpcMutex.reset();
        m_hServiceReady.reset();
    }

private:
    void processQuery(uint64_t queryId) {
        void* base = m_ipcView.get();
        if (!base) return;

        IpcHeader* hdr = ipcHeader(base);

        // Check status — must be QUERY_PENDING
        uint32_t status = InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(&hdr->status),
            IPC_STATUS_IDLE, IPC_STATUS_QUERY_PENDING);
        if (status != IPC_STATUS_QUERY_PENDING) {
            LOG("  [%llu] Spurious wakeup (status=%u)\n", queryId, status);
            return;
        }

        // Read input
        uint32_t inputLen = hdr->input_len;
        if (inputLen == 0 || inputLen > IPC_MAX_INPUT_LEN) {
            hdr->error_code = IPC_ERROR_QUERY_TOO_LONG;
            hdr->status = IPC_STATUS_ERROR;
            SetEvent(m_hEvtReply.get());
            return;
        }

        char* input = ipcInputBuf(base);
        input[IPC_MAX_INPUT_LEN - 1] = '\0';
        std::string query(input, strnlen(input, inputLen));

        LOG("  [%llu] Query: \"%s\"", queryId, query.c_str());

        auto results = m_dictReader.query(query, IPC_MAX_CANDIDATES);

        // Augment with hot cache
        if (m_hotCache.isLoaded()) {
            auto hotResults = m_hotCache.query(query, 32);
            std::unordered_set<std::string> seen;
            for (auto& r : results) seen.insert(r.first);
            for (auto& h : hotResults) {
                if (seen.insert(h.first).second) results.push_back(h);
            }
        }

        LOG(" -> %zu candidates\n", results.size());

        // Write output
        IpcCandidate* output = ipcOutputBuf(base);
        char* strArea = ipcStringArea(base);
        uint32_t strOff = 0;
        uint32_t count = 0;

        for (auto& r : results) {
            if (count >= IPC_MAX_CANDIDATES) break;
            size_t len = r.first.size() + 1;
            if (strOff + len > IPC_STRING_AREA_SIZE) break;
            memcpy(strArea + strOff, r.first.c_str(), len);
            output[count].word_offset = IPC_STRING_AREA_OFFSET + strOff;
            output[count].frequency = r.second;
            strOff += (uint32_t)len;
            count++;
        }

        hdr->output_count = count;
        hdr->output_total = (uint32_t)results.size();
        hdr->error_code = IPC_ERROR_NONE;

        // Signal reply
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&hdr->status),
                            IPC_STATUS_REPLY_READY);
        SetEvent(m_hEvtReply.get());
    }

    // ── Members ──────────────────────────────────────────────────────────
    unique_handle    m_dictMapping;
    scoped_unmap     m_dictView;
    BinaryDictReader m_dictReader;
    HotWordCache     m_hotCache;
    unique_handle    m_hIpcMapping;
    scoped_unmap     m_ipcView;
    unique_handle    m_hEvtQuery;
    unique_handle    m_hEvtReply;
    unique_handle    m_hEvtStop;
    unique_handle    m_hIpcMutex;
    unique_handle    m_hServiceReady;
};

// ═══════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    LOG("=== Test PinyinIME Dict Service ===\n");

    std::wstring dictPathStr;

    if (argc >= 2) {
        // User-provided path: convert from UTF-8
        int wlen = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
        wchar_t* buf = new wchar_t[wlen];
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, buf, wlen);
        dictPathStr = buf;
        delete[] buf;
    } else {
        dictPathStr = defaultDictPath();
    }
    const wchar_t* dictPath = dictPathStr.c_str();

    LOG("[TestService] dict.bin path: %ls\n", dictPath);

    TestDictService service;
    if (!service.initialize(dictPath)) {
        LOG("[TestService] FAIL: Initialization failed\n");
        return 1;
    }

    if (!service.createIpcChannel()) {
        LOG("[TestService] FAIL: IPC channel creation failed\n");
        return 1;
    }

    LOG("[TestService] Service is ready. Press Ctrl+C or send stop signal to exit.\n");
    LOG("[TestService] ========================================\n");

    service.run();
    service.shutdown();

    LOG("[TestService] Shutdown complete.\n");
    return 0;
}
