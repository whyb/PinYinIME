// shared/ime_ipc.h — IPC constants shared between DLL (TSF text service) and EXE (tray/settings)
// Both projects include this header to agree on named kernel object names and window messages.
#pragma once
#include <windows.h>

// ── Single-instance EXE mutex ──────────────────────────────────────────
// Created by PinyinIME.exe at startup. DLL checks it to decide whether to
// PostMessage (EXE already running) or ShellExecute (launch new instance).
#define PinyinIME_SINGLE_INSTANCE_MUTEX  L"Local\\PinyinIME_SingleInstance"

// ── Window class for EXE's hidden main window ──────────────────────────
// Must be a top-level window (not HWND_MESSAGE) so FindWindowW locates it
// cross-process. Uses WS_EX_TOOLWINDOW so it stays off the taskbar.
#define PinyinIME_MAIN_WINDOW_CLASS      L"PinyinIMEMain"

// ── Registered window message for "open settings" ──────────────────────
// Both DLL and EXE call RegisterWindowMessageW with this string to get a
// consistent message ID across process boundaries.
#define PinyinIME_MSG_OPEN_SETTINGS      L"PinyinIME_OpenSettings"

// ── Shared dictionary file mapping ─────────────────────────────────────
// Session-local file mapping containing the serialized FlatTrie dictionary.
// Created by the first process to finish full background loading.
#define PinyinIME_DICT_MAPPING           L"Local\\PinyinIME_DictData"

// ── Dictionary initialization mutex ─────────────────────────────────────
// Guards the creation of the shared dictionary file mapping.
// The first process to acquire this becomes the designated creator.
#define PinyinIME_DICT_MUTEX             L"Local\\PinyinIME_DictMutex"

// ── Dict service process management ─────────────────────────────────────
// The EXE (PinyinIME.exe) uses these to manage the background dict service.
#define PinyinIME_DICT_SERVICE_EXE       L"PinyinIMEDictService.exe"
#define PinyinIME_DICT_SERVICE_MUTEX     L"Local\\PinyinIME_DictService_SingleInstance"

// ── Dict service stop event ─────────────────────────────────────────────
// The EXE signals this event to request the service to gracefully shut down.
// The service uses WaitForMultipleObjects on both Evt_Query and Evt_Stop.
// Must be Global\ so the EXE (which may run elevated) can signal it.
#define PinyinIME_SERVICE_STOP_EVENT     L"Global\\PinyinIME_EvtStop"

// ── Dict service readiness event ────────────────────────────────────────
// The service signals this global event once dict.bin is loaded and the
// IPC channel is ready. DLLs and EXE poll/wait on it before querying.
#define PinyinIME_SERVICE_READY_EVENT    L"Global\\PinyinIME_ServiceReady"

// ── IPC channel (DLL ↔ Service) ─────────────────────────────────────────
// These are defined in shared/ipc_protocol.h for the protocol layer.
// Primary names use Global\ for AppContainer sandbox compatibility.
// Fallback names use Local\ when SeCreateGlobalPrivilege is unavailable.
