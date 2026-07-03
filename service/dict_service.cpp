// service/dict_service.cpp — PinyinIMEDictService.exe
//
// Self-contained background dictionary service. Responsibilities:
//   1. Auto-compile dict.bin from YAML sources on first launch (if missing).
//   2. Memory-map dict.bin using MapViewOfFile.
//   3. Create named shared memory + named events for IPC with DLL clients.
//   4. Run IPC loop: wait on Evt_Query, process query against dict, write results.
//
#include <windows.h>
#include <sddl.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <atomic>

#include "../shared/unique_handle.h"
#include "../shared/dict_binary.h"
#include "../shared/ipc_protocol.h"
#include "../shared/ime_ipc.h"

// ── Logging helper (writes to stderr with newline) ──────────────────────
#define LOG(fmt, ...) fprintf(stderr, "[svc] " fmt "\n", ##__VA_ARGS__)

namespace {

struct IpcCreationResult {
    unique_handle   handle;
    IpcNamespace    usedNamespace;
};

IpcCreationResult createEventWithFallback(
    SandboxSecurityAttributes& sd, BOOL manualReset, BOOL initialState,
    const wchar_t* globalName, const wchar_t* fallbackName)
{
    IpcCreationResult result;
    result.handle.reset(CreateEventW(&sd, manualReset, initialState, globalName));
    if (result.handle.valid() && GetLastError() == ERROR_ALREADY_EXISTS) {
        result.usedNamespace = IpcNamespace::Global;
        return result;
    }
    if (result.handle.valid()) {
        result.usedNamespace = IpcNamespace::Global;
        return result;
    }
    DWORD err = GetLastError();
    if (isGlobalNamespaceError(err)) {
        result.handle.reset(CreateEventW(&sd, manualReset, initialState, fallbackName));
        result.usedNamespace = IpcNamespace::Local;
    } else {
        result.handle.reset(CreateEventW(&sd, manualReset, initialState, fallbackName));
        result.usedNamespace = IpcNamespace::Local;
    }
    return result;
}

IpcCreationResult createMappingWithFallback(
    SandboxSecurityAttributes& sd, DWORD flProtect, DWORD maxSize,
    const wchar_t* globalName, const wchar_t* fallbackName)
{
    IpcCreationResult result;
    result.handle.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, &sd, flProtect, 0, maxSize, globalName));
    if (result.handle.valid()) { result.usedNamespace = IpcNamespace::Global; return result; }
    DWORD err = GetLastError();
    result.handle.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, &sd, flProtect, 0, maxSize, fallbackName));
    result.usedNamespace = IpcNamespace::Local;
    return result;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
//  Dictionary service class
// ═══════════════════════════════════════════════════════════════════════════

class DictService {
public:
    DictService() = default;
    ~DictService() { shutdown(); }

    bool initialize(const wchar_t* dictBinPath) {
        LOG("=== initialize: dictPath='%ls' ===", dictBinPath);

        if (!loadDictionary(dictBinPath)) {
            LOG("FAIL: loadDictionary");
            return false;
        }
        LOG("OK: Dictionary loaded (%zu states, %zu entries)",
            m_dictReader.stateCount(), m_dictReader.entryCount());

        if (!createIpcChannel()) {
            LOG("FAIL: createIpcChannel");
            return false;
        }
        LOG("OK: IPC channel created (namespace=%s)",
            m_ipcNamespace == IpcNamespace::Global ? "Global" : "Local");

        // Step 3: Create the stop event
        {
            SandboxSecurityAttributes sd;
            m_hEvtStop.reset(CreateEventW(&sd, TRUE, FALSE, PinyinIME_SERVICE_STOP_EVENT));
            if (m_hEvtStop.valid()) {
                ResetEvent(m_hEvtStop.get());
                LOG("OK: Stop event created");
            } else {
                LOG("WARN: Could not create stop event (err=%lu)", GetLastError());
            }
        }

        // Step 4: Signal readiness
        {
            SandboxSecurityAttributes sd;
            const wchar_t* readyName = (m_ipcNamespace == IpcNamespace::Global)
                ? L"Global\\PinyinIME_ServiceReady"
                : L"Local\\PinyinIME_ServiceReady";
            m_hServiceReady.reset(CreateEventW(&sd, TRUE, FALSE, readyName));
            if (m_hServiceReady.valid()) {
                SetEvent(m_hServiceReady.get());
                LOG("OK: ServiceReady event signaled (%ls)", readyName);
            } else {
                LOG("WARN: Could not create ServiceReady event (err=%lu)", GetLastError());
            }
        }

        m_running.store(true);
        LOG("=== initialize: SUCCESS ===");
        return true;
    }

    void run() {
        LOG("=== IPC loop starting ===");
        HANDLE waitHandles[2] = { m_hEvtQuery.get(), m_hEvtStop.get() };
        uint64_t queryCount = 0;

        while (m_running.load()) {
            DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

            if (waitResult == WAIT_OBJECT_0) {
                queryCount++;
                processQuery(queryCount);
            } else if (waitResult == WAIT_OBJECT_0 + 1) {
                LOG("Stop event signaled, shutting down (processed %llu queries)", queryCount);
                m_running.store(false);
                break;
            } else {
                LOG("WaitForMultipleObjects failed (err=%lu), exiting", GetLastError());
                break;
            }
        }
        LOG("=== IPC loop ended ===");
    }

    void shutdown() {
        m_running.store(false);
        if (m_hEvtQuery.valid()) SetEvent(m_hEvtQuery.get());
        m_dictView.reset();
        m_dictMapping.reset();
        m_ipcView.reset();
        m_ipcMapping.reset();
        m_hEvtQuery.reset();
        m_hEvtReply.reset();
        m_hEvtStop.reset();
        m_hServiceReady.reset();
    }

    void signalStop() {
        m_running.store(false);
        if (m_hEvtStop.valid()) SetEvent(m_hEvtStop.get());
    }

    // ── Direct test: query dict.bin without IPC ─────────────────────────
    void testLookup(const std::string& key) {
        LOG("=== TEST: lookup '%s' ===", key.c_str());
        if (!m_dictReader.isReady()) {
            LOG("  dictReader NOT ready");
            return;
        }
        uint32_t count = 0;
        const DictFileEntry* entries = m_dictReader.find(key, count);
        LOG("  exact find: returned %p, count=%u", (void*)entries, count);
        if (entries && count > 0) {
            uint32_t show = count < 5 ? count : 5;
            LOG("  showing first %u entries", show);
            for (uint32_t i = 0; i < show; ++i) {
                LOG("  [%u] word_offset=%u freq=%d", i, entries[i].word_offset, entries[i].frequency);
                const char* w = m_dictReader.getWord(entries[i].word_offset);
                LOG("  [%u] word='%s'", i, w ? w : "(null)");
            }
        }

        LOG("  starting prefix search...");
        std::unordered_map<std::string, int> prefixResults;
        int maxDepth = (key.size() == 1) ? 6 : (key.size() == 2 ? 5 : 0);
        m_dictReader.prefixSearch(key, prefixResults, 3, maxDepth);
        LOG("  prefix search: %zu results (maxDepth=%d)", prefixResults.size(), maxDepth);
    }

private:
    bool loadDictionary(const wchar_t* path) {
        unique_handle hFile(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!hFile.valid()) {
            LOG("  FAIL: Cannot open file (err=%lu)", GetLastError());
            return false;
        }
        LOG("  File opened OK");

        DictFileHeader diskHeader;
        DWORD bytesRead = 0;
        if (!ReadFile(hFile.get(), &diskHeader, sizeof(diskHeader), &bytesRead, nullptr)
            || bytesRead != sizeof(diskHeader)) {
            LOG("  FAIL: Read header (bytes=%lu, err=%lu)", bytesRead, GetLastError());
            return false;
        }
        if (diskHeader.magic != DICT_BIN_MAGIC) {
            LOG("  FAIL: Bad magic 0x%08X (expected 0x%08X)", diskHeader.magic, DICT_BIN_MAGIC);
            return false;
        }
        LOG("  Header OK: v%u, %u states, %u entries, total=%u bytes",
            diskHeader.version, diskHeader.state_count, diskHeader.entry_count, diskHeader.total_file_size);

        unique_handle hMapping(CreateFileMappingW(hFile.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
        if (!hMapping.valid()) {
            LOG("  FAIL: CreateFileMapping (err=%lu)", GetLastError());
            return false;
        }

        void* view = MapViewOfFile(hMapping.get(), FILE_MAP_READ, 0, 0, 0);
        if (!view) {
            LOG("  FAIL: MapViewOfFile (err=%lu)", GetLastError());
            return false;
        }
        LOG("  Mapped at %p", view);

        if (!m_dictReader.attach(view)) {
            UnmapViewOfFile(view);
            LOG("  FAIL: BinaryDictReader::attach");
            return false;
        }
        m_dictView.reset(view);
        m_dictMapping.reset(hMapping.release());
        LOG("  Dict ready: root_base=%u state_count=%zu entry_count=%zu",
            diskHeader.root_base, m_dictReader.stateCount(), m_dictReader.entryCount());
        return true;
    }

    bool createIpcChannel() {
        SandboxSecurityAttributes sd;
        LOG("  SDDL DACL: %s", sd.hasDacl() ? "OK" : "NULL (sandbox may not work)");

        for (int attempt = 0; attempt < 2; ++attempt) {
            IpcNamespace ns = (attempt == 0) ? IpcNamespace::Local : IpcNamespace::Global;
            const wchar_t* evtQueryName = (ns == IpcNamespace::Global) ? PinyinIME_IPC_EVT_QUERY  : PinyinIME_IPC_EVT_QUERY_FALLBACK;
            const wchar_t* evtReplyName = (ns == IpcNamespace::Global) ? PinyinIME_IPC_EVT_REPLY  : PinyinIME_IPC_EVT_REPLY_FALLBACK;
            const wchar_t* mappingName  = (ns == IpcNamespace::Global) ? PinyinIME_IPC_MAPPING    : PinyinIME_IPC_MAPPING_FALLBACK;
            const char* nsName = (ns == IpcNamespace::Global) ? "Global" : "Local";

            LOG("  Attempt %d: %s\\ namespace", attempt + 1, nsName);

            unique_handle hQuery(CreateEventW(&sd, FALSE, FALSE, evtQueryName));
            if (!hQuery.valid()) {
                DWORD err = GetLastError();
                LOG("    EvtQuery FAIL (err=%lu)", err);
                if (attempt == 0 && isGlobalNamespaceError(err)) continue;
                return false;
            }
            LOG("    EvtQuery OK (isNew=%d)", GetLastError() != ERROR_ALREADY_EXISTS);

            unique_handle hReply(CreateEventW(&sd, FALSE, FALSE, evtReplyName));
            if (!hReply.valid()) {
                LOG("    EvtReply FAIL (err=%lu)", GetLastError());
                return false;
            }
            LOG("    EvtReply OK");

            unique_handle hMapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &sd, PAGE_READWRITE, 0, IPC_MAPPING_SIZE, mappingName));
            if (!hMapping.valid()) {
                DWORD err = GetLastError();
                LOG("    Mapping FAIL (err=%lu)", err);
                if (attempt == 0 && isGlobalNamespaceError(err)) continue;
                return false;
            }
            bool isCreator = (GetLastError() != ERROR_ALREADY_EXISTS);
            LOG("    Mapping OK (isCreator=%d)", isCreator);

            void* view = MapViewOfFile(hMapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, 0);
            if (!view) {
                LOG("    MapViewOfFile FAIL (err=%lu)", GetLastError());
                return false;
            }
            LOG("    Mapped IPC at %p", view);

            if (isCreator) memset(view, 0, IPC_MAPPING_SIZE);
            IpcHeader* hdr = ipcHeader(view);
            hdr->query_id = 0;
            hdr->status = IPC_STATUS_IDLE;

            m_ipcView.reset(view);
            m_ipcMapping.reset(hMapping.release());
            m_hEvtQuery.reset(hQuery.release());
            m_hEvtReply.reset(hReply.release());
            m_ipcNamespace = ns;
            return true;
        }
        return false;
    }

    void processQuery(uint64_t queryId) {
        if (!m_ipcView) return;
        void* base = m_ipcView.get();
        IpcHeader* hdr = ipcHeader(base);

        uint32_t status = InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(&hdr->status),
            IPC_STATUS_IDLE, IPC_STATUS_QUERY_PENDING);

        if (status != IPC_STATUS_QUERY_PENDING) {
            LOG("Q#%llu: spurious wakeup (status=%u)", queryId, status);
            return;
        }

        uint32_t inputLen = hdr->input_len;
        if (inputLen == 0 || inputLen > IPC_MAX_INPUT_LEN) {
            hdr->error_code = IPC_ERROR_QUERY_TOO_LONG;
            hdr->status = IPC_STATUS_ERROR;
            hdr->output_count = 0;
            SetEvent(m_hEvtReply.get());
            LOG("Q#%llu: invalid input_len=%u", queryId, inputLen);
            return;
        }

        char* input = ipcInputBuf(base);
        input[IPC_MAX_INPUT_LEN - 1] = '\0';
        std::string query(input, strnlen(input, inputLen));

        if (!m_dictReader.isReady()) {
            hdr->error_code = IPC_ERROR_DICT_NOT_READY;
            hdr->status = IPC_STATUS_ERROR;
            hdr->output_count = 0;
            SetEvent(m_hEvtReply.get());
            LOG("Q#%llu: dict not ready", queryId);
            return;
        }

        std::unordered_map<std::string, int> results;
        IpcCandidate* candidates = ipcOutputBuf(base);
        char* strArea = ipcStringArea(base);

        // 1. Exact match
        uint32_t exactCount = 0;
        const DictFileEntry* exactEntries = m_dictReader.find(query, exactCount);
        if (exactEntries && exactCount > 0) {
            uint32_t take = (std::min)(exactCount, IPC_MAX_CANDIDATES);
            for (uint32_t i = 0; i < take; ++i) {
                const char* word = m_dictReader.getWord(exactEntries[i].word_offset);
                if (!word) continue;
                results[std::string(word)] = exactEntries[i].frequency;
            }
        }

        // 2. Prefix search
        if (query.size() <= 4) {
            int maxDepth = 0;
            if (query.size() == 1) maxDepth = 6;
            else if (query.size() == 2) maxDepth = 5;
            m_dictReader.prefixSearch(query, results, 3, maxDepth);
        }

        // 3. Sort
        std::vector<std::pair<std::string, int>> sorted;
        sorted.reserve(results.size());
        for (auto& kv : results) sorted.push_back(kv);
        std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

        // 4. Write to output buffer
        uint32_t totalFound = static_cast<uint32_t>(sorted.size());
        uint32_t outCount = (std::min)(totalFound, IPC_MAX_CANDIDATES);
        uint32_t strOffset = 0;
        for (uint32_t i = 0; i < outCount; ++i) {
            const std::string& word = sorted[i].first;
            uint32_t needed = static_cast<uint32_t>(word.size()) + 1;
            if (strOffset + needed > IPC_STRING_AREA_SIZE) { outCount = i; break; }
            memcpy(strArea + strOffset, word.c_str(), needed);
            candidates[i].word_offset = IPC_STRING_AREA_OFFSET + strOffset;
            candidates[i].frequency = sorted[i].second;
            strOffset += needed;
        }

        hdr->output_count = outCount;
        hdr->output_total = totalFound;
        hdr->error_code = IPC_ERROR_NONE;
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&hdr->status), IPC_STATUS_REPLY_READY);
        SetEvent(m_hEvtReply.get());

        // Log query summary (only for non-empty results to reduce noise; always log first few & every 100th)
        if (outCount > 0 || queryId <= 3 || queryId % 100 == 0) {
            LOG("Q#%llu: '%s' -> exact=%u prefix+exact=%u sent=%u/%u",
                queryId, query.c_str(), exactCount, totalFound, outCount, totalFound);
            if (outCount > 0) {
                const char* top = m_dictReader.getWord(candidates[0].word_offset);
                LOG("  top: '%s' (freq=%d)", top ? top : "?", candidates[0].frequency);
            }
        }
    }

    // ── Members ─────────────────────────────────────────────────────────
    unique_handle    m_dictMapping;
    scoped_unmap     m_dictView;
    BinaryDictReader m_dictReader;
    unique_handle    m_ipcMapping;
    scoped_unmap     m_ipcView;
    unique_handle    m_hEvtQuery;
    unique_handle    m_hEvtReply;
    unique_handle    m_hEvtStop;
    unique_handle    m_hServiceReady;
    std::atomic<bool> m_running{false};
    IpcNamespace     m_ipcNamespace = IpcNamespace::Global;
};

// ═══════════════════════════════════════════════════════════════════════════
//  Console control handler
// ═══════════════════════════════════════════════════════════════════════════

static DictService* g_pService = nullptr;

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (g_pService) {
        LOG("Shutdown signal received (ctrlType=%lu)", ctrlType);
        g_pService->signalStop();
    }
    return TRUE;
}

// Helper to get file size
static LONGLONG GetFileSizeEx_impl(const wchar_t* path) {
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &attr)) {
        LARGE_INTEGER sz;
        sz.LowPart = attr.nFileSizeLow;
        sz.HighPart = attr.nFileSizeHigh;
        return sz.QuadPart;
    }
    return -1;
}

// ═══════════════════════════════════════════════════════════════════════════
//  wmain — Service entry point
// ═══════════════════════════════════════════════════════════════════════════

int wmain(int argc, wchar_t* argv[])
{
    LOG("========================================");
    LOG("PinyinIMEDictService starting (pid=%lu)", GetCurrentProcessId());
    LOG("========================================");

    // ── Determine paths ─────────────────────────────────────────────────
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos + 1);
    std::wstring dictPath = dir + L"dict.bin";
    LOG("exe dir: %ls", dir.c_str());
    LOG("dict path: %ls", dictPath.c_str());

    // ── Check command line ──────────────────────────────────────────────
    bool runTest = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--dict") == 0 && i + 1 < argc) {
            dictPath = argv[++i];
        } else if (wcscmp(argv[i], L"--test") == 0) {
            runTest = true;
        }
    }

    // ── Auto-compile dict.bin if missing ─────────────────────────────────
    if (GetFileAttributesW(dictPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOG("dict.bin NOT FOUND, attempting auto-compile...");
        std::wstring compilerPath = dir + L"dict_compiler.exe";
        std::wstring cnDictsPath  = dir + L"cn_dicts";

        if (GetFileAttributesW(compilerPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(cnDictsPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring cmdLine = L"\"" + compilerPath + L"\" \"" + cnDictsPath + L"\" \"" + dictPath + L"\"";
            LOG("Running: %ls", cmdLine.c_str());

            STARTUPINFOW si = {}; si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi = {};
            std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
            cmdBuf.push_back(L'\0');

            if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr,
                               FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
                CloseHandle(pi.hThread);
                LOG("Waiting for dict_compiler (up to 120s)...");
                WaitForSingleObject(pi.hProcess, 120000);
                CloseHandle(pi.hProcess);
                if (GetFileAttributesW(dictPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    LOG("dict.bin compiled successfully");
                } else {
                    LOG("dict.bin compilation FAILED");
                }
            } else {
                LOG("FAILED to launch dict_compiler (err=%lu)", GetLastError());
            }
        } else {
            LOG("dict_compiler.exe or cn_dicts not found — cannot compile");
        }
    } else {
        LOG("dict.bin found, size=%lld bytes",
            (long long)GetFileSizeEx_impl(dictPath.c_str()));
    }

    // ── Single instance check ───────────────────────────────────────────
    unique_handle hSingleInstance(CreateMutexW(nullptr, TRUE, PinyinIME_DICT_SERVICE_MUTEX));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        DWORD waitResult = WaitForSingleObject(hSingleInstance.get(), 0);
        if (waitResult == WAIT_ABANDONED) {
            LOG("Took over abandoned mutex (previous instance crashed)");
        } else if (waitResult == WAIT_OBJECT_0) {
            LOG("Acquired existing mutex");
        } else {
            LOG("Another instance is already running — exiting");
            return 0;
        }
    }
    if (!hSingleInstance.valid()) {
        LOG("Failed to create single-instance mutex");
        return 0;
    }

    // ── Start service ───────────────────────────────────────────────────
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    DictService service;
    g_pService = &service;

    if (!service.initialize(dictPath.c_str())) {
        LOG("=== INITIALIZATION FAILED ===");
        unique_handle hReady(CreateEventW(nullptr, TRUE, TRUE, PinyinIME_SERVICE_READY_EVENT));
        g_pService = nullptr;
        return 1;
    }

    // ── If --test flag: run a few test lookups then exit ────────────────
    if (runTest) {
        LOG("=== RUNNING TEST LOOKUPS ===");
        service.testLookup("n");
        service.testLookup("ni");
        service.testLookup("nihao");
        service.testLookup("zhongguo");
        service.testLookup("beijing");
        service.testLookup("a");
        service.testLookup("sh");
        LOG("=== TEST COMPLETE ===");
        g_pService = nullptr;
        return 0;
    }

    // ── Run the IPC loop ────────────────────────────────────────────────
    LOG("Entering IPC loop (Ctrl+C to stop)...");
    service.run();

    g_pService = nullptr;
    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    LOG("=== Stopped ===");
    return 0;
}
