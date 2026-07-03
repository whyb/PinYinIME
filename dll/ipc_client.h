// dll/ipc_client.h — DLL-side IPC client for communicating with DictService
//
// This replaces direct dictionary queries (PinyinDict::find / prefixSearch)
// with synchronous IPC to the background dictionary service.
//
// DESIGN RATIONALE:
//   Previously, every process hosting PinyinIMETSF.dll parsed YAML dictionaries
//   and built in-memory tries (2 GB per process). With this client, the DLL
//   becomes a thin wrapper that sends pinyin strings to the service and receives
//   candidate words. The service does all the heavy lifting, and the dictionary
//   is loaded only once system-wide.
//
// USAGE IN OnKeyDown (text_service.cpp):
//   IpcClient ipc;
//   if (ipc.connect()) {
//       auto candidates = ipc.query("nihao");
//       // candidates is vector<pair<string, int>> — same format as before
//   }
//
// THREADING:
//   This client is designed for use on the TSF key event thread.
//   It performs SYNCHRONOUS BLOCKING queries with a 50ms timeout.
//   If the service doesn't respond in time, it returns empty results.
//
// CONNECTION CACHING:
//   The client caches the shared memory handle and event handles across
//   queries. connect() is fast after the first call (handles remain open).
//   This avoids the ~100μs overhead of OpenFileMappingW + OpenEventW per keystroke.
//
// SANDBOX COMPATIBILITY:
//   The service creates objects with low-integrity DACL (WD + AC).
//   This client uses OpenFileMappingW + OpenEventW (not Create) which
//   succeeds in AppContainer processes against those objects.
//
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <cstring>

#include "../shared/unique_handle.h"
#include "../shared/ipc_protocol.h"
#include "../shared/ime_ipc.h"

class IpcClient {
public:
    IpcClient() = default;

    ~IpcClient() {
        disconnect();
    }

    // Non-copyable, movable
    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;
    IpcClient(IpcClient&&) = default;
    IpcClient& operator=(IpcClient&&) = default;

    // ── Connect to the IPC channel ───────────────────────────────────────
    //
    // Opens the shared memory and events. Tries Global\ first (for sandbox
    // compatibility), then falls back to Local\ (when service lacked
    // SeCreateGlobalPrivilege). Returns true once connected.
    //
    // Safe to call multiple times — subsequent calls are no-ops.

    bool connect() {
        if (m_connected) return true;

        // Step 1: Try Local\ namespace first (most reliable, no privilege needed)
        if (tryConnect(PinyinIME_IPC_MAPPING_FALLBACK,
                       PinyinIME_IPC_EVT_QUERY_FALLBACK,
                       PinyinIME_IPC_EVT_REPLY_FALLBACK)) {
            m_namespaceUsed = IpcNamespace::Local;
            m_connected = true;
            return true;
        }

        // Step 2: Fall back to Global\ namespace (needed for sandboxed apps)
        if (tryConnect(PinyinIME_IPC_MAPPING,
                       PinyinIME_IPC_EVT_QUERY,
                       PinyinIME_IPC_EVT_REPLY)) {
            m_namespaceUsed = IpcNamespace::Global;
            m_connected = true;
            return true;
        }

        // Step 3: Neither namespace available — service not running
        return false;
    }

    // ── Disconnect ──────────────────────────────────────────────────────

    void disconnect() {
        m_ipcView.reset();
        m_ipcMapping.reset();
        m_hEvtQuery.reset();
        m_hEvtReply.reset();
        m_connected = false;
    }

    bool isConnected() const { return m_connected; }

    // ── Query the dictionary service ────────────────────────────────────
    //
    // Sends a pinyin query string to the service and blocks until the
    // service replies or the timeout expires.
    //
    // Returns: vector of (word, frequency) pairs, sorted by frequency desc.
    //          Empty vector on timeout, error, or no results.
    //
    // Performance:
    //   - Best case (service idle, fast query): ~60-230 μs
    //   - Timeout case (service dead): 50 ms (IPC_QUERY_TIMEOUT_MS)
    //   - Worst case (service busy, long queue): 50 ms timeout
    //
    // Thread-safety: NOT thread-safe. Call from a single thread.
    //
    std::vector<std::pair<std::string, int>> query(const std::string& pinyin) {
        std::vector<std::pair<std::string, int>> results;

        if (!m_connected || pinyin.empty()) {
            return results;
        }

        // Truncate if too long
        std::string key = pinyin;
        if (key.size() >= IPC_MAX_INPUT_LEN) {
            key.resize(IPC_MAX_INPUT_LEN - 1);
        }

        void* base = m_ipcView.get();
        IpcHeader* hdr = ipcHeader(base);

        // ── Check if the channel is free ────────────────────────────────
        // If the service is still processing a previous query (unlikely with
        // auto-reset events), spin briefly or return empty.
        uint32_t currentStatus = hdr->status;
        if (currentStatus != IPC_STATUS_IDLE && currentStatus != IPC_STATUS_REPLY_READY) {
            // Channel busy — previous query not yet consumed.
            // In practice this shouldn't happen because we always consume
            // the reply before sending the next query.
            return results;
        }

        // ── Write the query ─────────────────────────────────────────────
        char* input = ipcInputBuf(base);
        size_t len = key.size() + 1;  // include null terminator
        memcpy(input, key.c_str(), len);

        uint32_t newId = static_cast<uint32_t>(
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr->query_id)));

        hdr->input_len = static_cast<uint32_t>(len);
        hdr->output_count = 0;
        hdr->output_total = 0;
        hdr->error_code = IPC_ERROR_NONE;

        // Memory barrier: ensure all writes are visible before setting status
        MemoryBarrier();

        // ── Signal the service ──────────────────────────────────────────
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&hdr->status),
                            IPC_STATUS_QUERY_PENDING);

        SetEvent(m_hEvtQuery.get());

        // ── Wait for reply (with timeout protection) ────────────────────
        DWORD waitResult = WaitForSingleObject(m_hEvtReply.get(),
                                                IPC_QUERY_TIMEOUT_MS);

        if (waitResult != WAIT_OBJECT_0) {
            // Timeout or error — reset the channel and return empty
            if (waitResult == WAIT_TIMEOUT) {
                InterlockedExchange(reinterpret_cast<volatile LONG*>(&hdr->status),
                                    IPC_STATUS_IDLE);
            }
            // WAIT_FAILED or WAIT_ABANDONED — channel may be broken
            return results;
        }

        // ── Read the results ────────────────────────────────────────────
        uint32_t replyStatus = hdr->status;
        if (replyStatus != IPC_STATUS_REPLY_READY) {
            // Protocol error
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&hdr->status),
                                IPC_STATUS_IDLE);
            return results;
        }

        uint32_t count = hdr->output_count;
        if (count > IPC_MAX_CANDIDATES) {
            count = IPC_MAX_CANDIDATES;  // Defensive clamp
        }

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

        // ── Reset channel to idle ───────────────────────────────────────
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&hdr->status),
                            IPC_STATUS_IDLE);

        return results;
    }

    // ── Convenience: exact lookup (matches old PinyinDict::find) ────────
    //
    // This performs a query and returns candidates for the exact pinyin.
    // The results are the same format as the old API.

    // ── Convenience: prefix search (matches old PinyinDict::prefixSearch) ─
    //
    // For the IPC model, the service handles prefix expansion internally.
    // The client just sends the prefix and gets expanded results.

    // ── Check if the service is ready ───────────────────────────────────

    static bool isServiceReady() {
        // Try Local\ first (matches service's Local-first strategy)
        unique_handle hReady(OpenEventW(SYNCHRONIZE, FALSE, L"Local\\PinyinIME_ServiceReady"));
        if (hReady.valid()) {
            DWORD result = WaitForSingleObject(hReady.get(), 0);
            if (result == WAIT_OBJECT_0) return true;
        }
        // Fallback: Global\ (for elevated or privileged service instances)
        hReady.reset(OpenEventW(SYNCHRONIZE, FALSE, PinyinIME_SERVICE_READY_EVENT));
        if (hReady.valid()) {
            DWORD result = WaitForSingleObject(hReady.get(), 0);
            return (result == WAIT_OBJECT_0);
        }
        return false;
    }

    // ── Namespace used for the current connection ───────────────────────

    IpcNamespace getNamespaceUsed() const { return m_namespaceUsed; }

private:
    // Try to connect using specific object names.
    // Returns true if all three objects (mapping, query event, reply event)
    // can be opened successfully.
    bool tryConnect(const wchar_t* mappingName,
                    const wchar_t* queryEventName,
                    const wchar_t* replyEventName)
    {
        // Step 1: Open the shared memory
        unique_handle hMapping(OpenFileMappingW(
            FILE_MAP_ALL_ACCESS,   // Need write access for input buffer
            FALSE,
            mappingName));

        if (!hMapping.valid()) {
            return false;  // Service not running or object not created yet
        }

        void* view = MapViewOfFile(hMapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!view) {
            return false;
        }

        // Step 2: Open the events
        unique_handle hQuery(OpenEventW(EVENT_MODIFY_STATE, FALSE, queryEventName));
        unique_handle hReply(OpenEventW(SYNCHRONIZE, FALSE, replyEventName));

        if (!hQuery.valid() || !hReply.valid()) {
            UnmapViewOfFile(view);
            return false;
        }

        // Step 3: Store handles
        m_ipcView.reset(view);
        m_ipcMapping.reset(hMapping.release());
        m_hEvtQuery.reset(hQuery.release());
        m_hEvtReply.reset(hReply.release());

        return true;
    }

    // ── IPC handles ─────────────────────────────────────────────────────
    unique_handle m_ipcMapping;    // Shared memory mapping handle
    scoped_unmap  m_ipcView;       // Mapped view base address
    unique_handle m_hEvtQuery;     // "Query ready" event (signal to service)
    unique_handle m_hEvtReply;     // "Reply ready" event (wait for service)

    // ── State ───────────────────────────────────────────────────────────
    bool          m_connected     = false;
    IpcNamespace  m_namespaceUsed = IpcNamespace::Global;
};
