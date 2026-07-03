// shared/ipc_protocol.h — IPC protocol between DLL (TSF text service) and Service (dict server)
//
// ARCHITECTURE:
//   PinyinIMETSF.dll (injected into every app process)
//       │
//       │  Named Shared Memory + Named Events
//       │  Synchronous blocking query with 50ms timeout
//       ▼
//   PinyinIMEDictService.exe (single instance, runs as background service)
//       │
//       │  MapViewOfFile (zero-copy, demand-paged)
//       ▼
//   dict.bin (pre-compiled binary dictionary, ~87 MB on disk, ~50 MB RSS)
//
// SANDBOX PENETRATION (TWO CRITICAL MECHANISMS):
//
//   1. Global\ Namespace Prefix:
//      AppContainer processes have ISOLATED kernel object namespaces.
//      A Local\Foo object created by the service is invisible to a
//      sandboxed DLL. Using Global\ puts the object in the global
//      namespace where ALL integrity levels and AppContainer sandboxes
//      can see it. Without this:
//        - Chrome/Edge tabs: IME dead
//        - UWP/Store apps: IME dead
//        - Games with anti-cheat (Vanguard, EAC): IME dead
//
//   2. Low-Integrity DACL:
//      Even with Global\, the default DACL rejects Low Integrity and
//      AppContainer callers with ERROR_ACCESS_DENIED (5).
//      We add explicit ACEs:
//        WD = Everyone (all integrity levels)
//        AC = ALL APPLICATION PACKAGES (AppContainer SID)
//      SDDL: "D:(A;;GA;;;WD)(A;;GA;;;AC)"
//
//   Combined, these two measures ensure the IME works in:
//     - Chrome/Edge (AppContainer + Low Integrity tabs)
//     - UWP / Windows Store apps
//     - Games with anti-cheat (Vanguard, EasyAntiCheat, etc.)
//     - Windows Sandbox
//     - Standard Win32 apps (Notepad, Word, etc.)
//
//   FALLBACK: If the service lacks SeCreateGlobalPrivilege (rare on
//   locked-down enterprise systems), it falls back to Local\ namespace
//   with a warning. Sandboxed apps won't work in that case, but
//   standard apps will.
//
//
// SHARED MEMORY LAYOUT:
//   The mapping is divided into two regions:
//
//   ┌──────────────────────────────────────────────────────────────┐
//   │  IpcHeader (64 bytes)                                        │
//   │    query_id     — incrementing request counter               │
//   │    input_len    — bytes in input buffer (incl. null term)    │
//   │    output_count — number of valid IpcCandidate in output[]   │
//   │    output_total — total matches found (may exceed output_count)│
//   │    status       — 0=idle, 1=query_pending, 2=reply_ready    │
//   │    error_code   — 0=OK, nonzero=error                        │
//   │    reserved[...]                                             │
//   ├──────────────────────────────────────────────────────────────┤
//   │  Input Buffer  (256 bytes)                                   │
//   │    Null-terminated UTF-8 pinyin string (e.g. "nihao")        │
//   ├──────────────────────────────────────────────────────────────┤
//   │  Output Buffer (MAX_CANDIDATES × sizeof(IpcCandidate))       │
//   │    Candidate array filled by service, read by DLL             │
//   └──────────────────────────────────────────────────────────────┘
//
// PROTOCOL FLOW:
//   1. DLL writes pinyin to input buffer
//   2. DLL sets status = QUERY_PENDING (InterlockedExchange)
//   3. DLL signals Evt_Query (SetEvent)
//   4. DLL waits on Evt_Reply with 50ms timeout (WaitForSingleObject)
//   5. Service wakes on Evt_Query, reads input buffer
//   6. Service queries dict.bin, writes candidates to output buffer
//   7. Service sets status = REPLY_READY (InterlockedExchange)
//   8. Service signals Evt_Reply (SetEvent)
//   9. DLL wakes, reads output buffer, returns candidates to engine
//
// TIMEOUT BEHAVIOR:
//   If DLL times out (50ms), it returns empty results — the IME shows no
//   candidates for this keystroke. The next keystroke retries. This prevents
//   a dead/hung service from freezing the user's application.
//
#pragma once
#include <windows.h>
#include <sddl.h>
#include <cstdint>
#include <string>

// ── Named object names (must match between DLL and Service) ──────────────
//
// PRIMARY NAMES (Global\ namespace — visible to ALL processes including
// AppContainer sandboxes). Required for IME to work in Chrome/Edge/UWP.
//
#define PinyinIME_IPC_MAPPING    L"Global\\PinyinIME_IpcChannel"
#define PinyinIME_IPC_EVT_QUERY  L"Global\\PinyinIME_EvtQuery"
#define PinyinIME_IPC_EVT_REPLY  L"Global\\PinyinIME_EvtReply"
#define PinyinIME_IPC_MUTEX      L"Global\\PinyinIME_IpcMutex"

// FALLBACK NAMES (Local\ namespace — only visible to non-sandboxed processes).
// Used when the service process lacks SeCreateGlobalPrivilege (rare).
// Sandboxed apps (Chrome/Edge/UWP) will NOT work with these fallback names.
//
#define PinyinIME_IPC_MAPPING_FALLBACK    L"Local\\PinyinIME_IpcChannel"
#define PinyinIME_IPC_EVT_QUERY_FALLBACK  L"Local\\PinyinIME_EvtQuery"
#define PinyinIME_IPC_EVT_REPLY_FALLBACK  L"Local\\PinyinIME_EvtReply"

// ── Status codes (written atomically to IpcHeader::status) ──────────────

enum IpcStatus : uint32_t {
    IPC_STATUS_IDLE          = 0,  // Channel is free
    IPC_STATUS_QUERY_PENDING = 1,  // DLL has written a query, waiting for reply
    IPC_STATUS_REPLY_READY   = 2,  // Service has written results
    IPC_STATUS_ERROR         = 3,  // Service encountered an error
};

// ── Error codes (written to IpcHeader::error_code when status==ERROR) ───

enum IpcError : uint32_t {
    IPC_ERROR_NONE            = 0,
    IPC_ERROR_DICT_NOT_READY  = 1,  // dict.bin not loaded yet
    IPC_ERROR_QUERY_TOO_LONG  = 2,  // Input exceeds IPC_MAX_INPUT_LEN
    IPC_ERROR_INTERNAL        = 3,  // Internal service error
};

// ── Packed IPC structures ───────────────────────────────────────────────

#pragma pack(push, 1)

struct IpcHeader {
    uint32_t query_id;           // Monotonically incrementing request ID
    uint32_t input_len;          // Length of data in input buffer (bytes, incl. null)
    uint32_t output_count;       // Number of valid candidates written by service
    uint32_t output_total;       // Total matches (may exceed output_count)
    volatile uint32_t status;    // IpcStatus — atomic read/write across processes
    uint32_t error_code;         // IpcError — valid when status==IPC_STATUS_ERROR
    uint32_t reserved[10];       // Pad to 64 bytes
};
static_assert(sizeof(IpcHeader) == 64, "IpcHeader must be 64 bytes");

struct IpcCandidate {
    uint32_t word_offset;        // Byte offset from mapping base to UTF-8 word text
    int32_t  frequency;          // Frequency score
    // Note: word text lives in the output buffer immediately after the
    // IpcCandidate array. We use a contiguous layout:
    //
    //   [IpcCandidate[output_count]] [string data...]
    //
    // The string data area starts at:
    //   IPC_OUTPUT_OFFSET + IPC_MAX_CANDIDATES * sizeof(IpcCandidate)
    //
    // Each word_offset is relative to the mapping base and points into
    // this string data area. The service writes words consecutively.
};
static_assert(sizeof(IpcCandidate) == 8, "IpcCandidate must be 8 bytes");

#pragma pack(pop)

// ── Sizing constants (must follow IpcCandidate definition) ──────────────

constexpr uint32_t IPC_MAX_INPUT_LEN     = 256;    // Max pinyin query length (UTF-8 bytes)
constexpr uint32_t IPC_MAX_CANDIDATES    = 64;     // Max candidates per query
constexpr uint32_t IPC_MAPPING_SIZE      = 65536;  // 64 KB total mapping
constexpr uint32_t IPC_HEADER_SIZE       = 64;
constexpr uint32_t IPC_INPUT_OFFSET      = IPC_HEADER_SIZE;
constexpr uint32_t IPC_INPUT_SIZE        = IPC_MAX_INPUT_LEN;
constexpr uint32_t IPC_OUTPUT_OFFSET     = IPC_INPUT_OFFSET + IPC_INPUT_SIZE;
constexpr uint32_t IPC_OUTPUT_SIZE       = IPC_MAX_CANDIDATES * sizeof(IpcCandidate);

// ── Query timeout ───────────────────────────────────────────────────────
// If the service doesn't reply within this window, the DLL returns empty
// results to avoid freezing the application. 50ms is ~3 frames at 60fps.

constexpr DWORD IPC_QUERY_TIMEOUT_MS = 50;

// ── String data area ────────────────────────────────────────────────────
// After the IpcCandidate array, words are written as null-terminated UTF-8.
// word_offset in IpcCandidate points to the start of each word.
constexpr uint32_t IPC_STRING_AREA_OFFSET = IPC_OUTPUT_OFFSET
    + IPC_MAX_CANDIDATES * sizeof(IpcCandidate);
constexpr uint32_t IPC_STRING_AREA_SIZE   = IPC_MAPPING_SIZE - IPC_STRING_AREA_OFFSET;

// ── Inline helpers for accessing mapped regions ─────────────────────────

inline IpcHeader*      ipcHeader(void* base)      { return static_cast<IpcHeader*>(base); }
inline char*           ipcInputBuf(void* base)     { return static_cast<char*>(base) + IPC_INPUT_OFFSET; }
inline IpcCandidate*   ipcOutputBuf(void* base)    { return reinterpret_cast<IpcCandidate*>(static_cast<char*>(base) + IPC_OUTPUT_OFFSET); }
inline char*           ipcStringArea(void* base)   { return static_cast<char*>(base) + IPC_STRING_AREA_OFFSET; }

// ── Sandbox-penetrating security descriptor (RAII) ───────────────────────
//
// Creates a SECURITY_ATTRIBUTES with DACL:
//   (A;;GA;;;WD) — Generic All for Everyone (covers Low Integrity processes)
//   (A;;GA;;;AC) — Generic All for ALL APPLICATION PACKAGES (AppContainer)
//
// This is CRITICAL for sandbox penetration. Without it:
//   - DLL in Chrome/Edge sandbox → OpenEventW → ERROR_ACCESS_DENIED (5)
//   - DLL in UWP app → OpenFileMappingW → ERROR_ACCESS_DENIED (5)
//   - Result: IME dead in any sandboxed process
//
// RAII: the security descriptor is freed on destruction via LocalFree.
//
class SandboxSecurityAttributes {
public:
    SandboxSecurityAttributes() {
        m_sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        m_sa.bInheritHandle = FALSE;
        m_sa.lpSecurityDescriptor = nullptr;

        // Why we need BOTH ACEs:
        //   WD (Everyone)          — covers standard AND Low Integrity processes
        //   AC (AppContainer)      — covers modern UWP/Store app sandboxes
        //
        // Low Integrity processes are already covered by WD (Everyone)
        // because Low IL does not remove you from the Everyone group.
        // AppContainer processes need an explicit AC ACE because they
        // are NOT in the Everyone group by default.
        const wchar_t* sddl = L"D:(A;;GA;;;WD)(A;;GA;;;AC)";

        BOOL ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl,
            SDDL_REVISION_1,
            &m_pSD,
            nullptr);

        if (ok && m_pSD) {
            m_sa.lpSecurityDescriptor = m_pSD;
        } else {
            // SDDL conversion failure — very rare, indicates system corruption.
            // Fall back to nullptr (default DACL). Non-sandboxed apps will work;
            // sandboxed apps will get ACCESS_DENIED.
            wchar_t buf[128];
            swprintf(buf, 128, L"[PinyinIME] SDDL conversion failed (err=%lu) — "
                           L"sandboxed apps may not be able to type\n",
                     GetLastError());
            OutputDebugStringW(buf);
            m_sa.lpSecurityDescriptor = nullptr;
        }
    }

    ~SandboxSecurityAttributes() {
        if (m_pSD) {
            LocalFree(m_pSD);
        }
    }

    // Non-copyable
    SandboxSecurityAttributes(const SandboxSecurityAttributes&) = delete;
    SandboxSecurityAttributes& operator=(const SandboxSecurityAttributes&) = delete;

    LPSECURITY_ATTRIBUTES get() { return &m_sa; }
    SECURITY_ATTRIBUTES* operator&() { return &m_sa; }
    bool hasDacl() const { return m_sa.lpSecurityDescriptor != nullptr; }

    // Transfer ownership of the security descriptor to the caller.
    // After this call, the SandboxSecurityAttributes no longer owns the SD.
    // Caller must free with LocalFree(). Returns nullptr if no SD was created.
    PSECURITY_DESCRIPTOR releaseSD() {
        PSECURITY_DESCRIPTOR sd = m_pSD;
        m_pSD = nullptr;
        m_sa.lpSecurityDescriptor = nullptr;
        return sd;
    }

private:
    SECURITY_ATTRIBUTES  m_sa = {};
    PSECURITY_DESCRIPTOR m_pSD = nullptr;
};

// ── Namespace strategy: try Global\ first, fall back to Local\ ──────────
//
// Global\ makes objects visible to AppContainer sandboxes but requires
// SeCreateGlobalPrivilege (granted by default on Win10/11 consumer SKUs).
// If creation fails with ERROR_ACCESS_DENIED (missing privilege), we fall
// back to Local\ — sandboxed apps won't work, but standard apps will.
//
// Returns the actual namespace prefix used (L"Global\\" or L"Local\\").
// Caller should check the return value and log appropriately.

enum class IpcNamespace {
    Global,  // Full sandbox compatibility
    Local    // Fallback — no sandbox support
};

// Helper: determine whether an error code means "need to fall back from Global\ to Local\"
inline bool isGlobalNamespaceError(DWORD err) {
    return err == ERROR_ACCESS_DENIED     // Missing SeCreateGlobalPrivilege
        || err == ERROR_PRIVILEGE_NOT_HELD;
}

// Diagnostic: log when fallback mode is active
inline void LogIpcNamespaceWarning(IpcNamespace ns) {
    if (ns == IpcNamespace::Local) {
        OutputDebugStringW(
            L"[PinyinIME] WARNING: Using Local\\ namespace (sandboxed apps may not type).\n"
            L"[PinyinIME] The service process lacks SeCreateGlobalPrivilege.\n"
            L"[PinyinIME] Chrome/Edge/UWP apps will NOT work with this IME.\n");
    }
}

// Legacy helper — kept for backward compatibility in simple callers.
// Prefer SandboxSecurityAttributes for new code.
// Caller must free sa.lpSecurityDescriptor with LocalFree().
inline SECURITY_ATTRIBUTES BuildLowIntegritySecurityDescriptor() {
    SandboxSecurityAttributes sd;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = sd.releaseSD();  // transfer ownership
    return sa;
}
