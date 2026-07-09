// test/test_common.h — Shared test IPC names (isolated from production)
//
// All names use "TestPinyinIME" prefix to avoid conflicts with the real
// production IME service. This allows the test service and client to run
// side-by-side with a production installation.
#pragma once
#include <windows.h>
#include <string>

// ── Test IPC named object names ──────────────────────────────────────
// Use Local\ namespace — no privilege needed, and avoids conflicts with
// production Global\ objects. Test targets single-machine dev use.
#define TEST_IPC_MAPPING       L"Local\\TestPinyinIME_IpcChannel"
#define TEST_IPC_EVT_QUERY     L"Local\\TestPinyinIME_EvtQuery"
#define TEST_IPC_EVT_REPLY     L"Local\\TestPinyinIME_EvtReply"
#define TEST_IPC_MUTEX         L"Local\\TestPinyinIME_IpcMutex"
#define TEST_IPC_EVT_STOP      L"Local\\TestPinyinIME_EvtStop"
#define TEST_SERVICE_READY     L"Local\\TestPinyinIME_ServiceReady"

// ── Default dict.bin path (next to the test executable) ──────────────
// dict.bin is copied to the test output dir by CMake post-build step.
inline std::wstring defaultDictPath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* lastSep = wcsrchr(path, L'\\');
    if (lastSep) *(lastSep + 1) = L'\0';  // Truncate at last backslash to get dir
    std::wstring result(path);
    result += L"dict.bin";
    return result;
}
