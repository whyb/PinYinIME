// exe/service_manager.h — PinyinIME.exe 管理后台词库服务的逻辑
//
// PinyinIMEDictService.exe 是独立的词库服务进程, 负责:
//   - 开机自启 (通过 Windows Task Scheduler ONLOGON 触发器)
//   - 加载 dict.bin 共享内存 (内存占用在服务进程中)
//   - 通过 IPC 与 TSF DLL 通信, 提供拼音查询
//
// PinyinIME.exe (配置工具) 仅在注册/卸载时管理服务:
//   1. 注册时: 创建 Task Scheduler 任务 → 启动服务 → 等待就绪
//   2. 卸载时: 停止服务 → 删除 Task Scheduler 任务
//   3. 手动调用 ensureServiceRunning() 可强制重启服务
//
// 服务启动方式:
//   a) Windows Task Scheduler 触发器 (用户登录时自动启动, 主要方式)
//   b) 注册/安装过程中由 PinyinIME.exe 启动
//   c) 手动命令行: PinyinIMEDictService.exe
//
#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <taskschd.h>
#include <comdef.h>
#include <string>
#include <vector>
#include "../shared/ime_ipc.h"
#include "../shared/unique_handle.h"

#pragma comment(lib, "taskschd.lib")

// ═══════════════════════════════════════════════════════════════════════════
//  Service lifecycle management
// ═══════════════════════════════════════════════════════════════════════════

class ServiceManager {
public:
    // ── 检查服务是否已在运行 ────────────────────────────────────────────
    //
    // 通过尝试打开服务单实例互斥量来判断。
    // 服务在启动时会创建 PinyinIME_DICT_SERVICE_MUTEX。

    static bool isServiceRunning() {
        unique_handle hMutex(OpenMutexW(SYNCHRONIZE, FALSE,
                                         PinyinIME_DICT_SERVICE_MUTEX));
        return hMutex.valid();
    }

    // ── 等待服务就绪 ────────────────────────────────────────────────────
    //
    // 服务在 dict.bin 加载完成 + IPC channel 创建完毕后会
    // 设置 PinyinIME_SERVICE_READY_EVENT 全局事件。
    //
    // timeoutMs: 最长等待毫秒数 (默认 30 秒, 足够加载 dict.bin)
    // 返回 true 表示服务已就绪

    static bool waitForServiceReady(DWORD timeoutMs = 30000) {
        // Try Local\ first (matches service's Local-first strategy)
        auto tryOpen = []() -> unique_handle {
            unique_handle h(OpenEventW(SYNCHRONIZE, FALSE, L"Local\\PinyinIME_ServiceReady"));
            if (!h.valid()) {
                h.reset(OpenEventW(SYNCHRONIZE, FALSE, PinyinIME_SERVICE_READY_EVENT));
            }
            return h;
        };

        unique_handle hReady = tryOpen();
        if (!hReady.valid()) {
            // 事件不存在 — 服务可能还没创建它
            // 轮询等待
            for (DWORD elapsed = 0; elapsed < timeoutMs; elapsed += 500) {
                Sleep(500);
                hReady = tryOpen();
                if (hReady.valid()) break;
            }
            if (!hReady.valid()) return false;
        }

        DWORD result = WaitForSingleObject(hReady.get(), timeoutMs);
        return (result == WAIT_OBJECT_0);
    }

    // ── 启动服务进程 ────────────────────────────────────────────────────
    //
    // 启动 PinyinIMEDictService.exe。
    // exeDir: PinyinIME.exe 所在目录 (服务 exe 也在同一目录)
    //
    // 返回服务进程的 HANDLE (调用方负责 CloseHandle)
    // 或 nullptr 表示启动失败

    static HANDLE startService(const std::wstring& exeDir) {
        // 规范化路径: 去掉末尾多余的反斜杠
        std::wstring dir = exeDir;
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) {
            dir.pop_back();
        }
        std::wstring servicePath = dir + L"\\" + PinyinIME_DICT_SERVICE_EXE;

        // 检查服务 exe 是否存在
        if (GetFileAttributesW(servicePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            OutputDebugStringW(L"[PinyinIME] Dict service exe not found\n");
            return nullptr;
        }

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;  // 后台运行, 无窗口

        PROCESS_INFORMATION pi = {};

        // 以低优先级启动 — 词库查询对延迟敏感, 但服务本身不需要高优先级
        BOOL ok = CreateProcessW(
            servicePath.c_str(),
            nullptr,                  // 命令行 (使用默认)
            nullptr,                  // 进程安全属性 (默认)
            nullptr,                  // 线程安全属性 (默认)
            FALSE,                    // 不继承句柄
            CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
            nullptr,                  // 环境 (继承)
            exeDir.c_str(),          // 工作目录
            &si,
            &pi);

        if (!ok) {
            OutputDebugStringW(L"[PinyinIME] Failed to start dict service\n");
            return nullptr;
        }

        CloseHandle(pi.hThread);  // 不需要线程句柄
        return pi.hProcess;       // 调用方负责 CloseHandle
    }

    // ── 确保服务正在运行 (强制重启模式) ─────────────────────────────────
    //
    // 不管当前状态如何: 先停止旧实例, 再启动新实例。
    // 这消除了僵尸互斥量 / 残留进程带来的各种边界情况。

    static bool ensureServiceRunning(const std::wstring& exeDir) {
        // 先停掉可能存在的旧实例 (含僵尸进程)
        if (isServiceRunning()) {
            OutputDebugStringW(L"[PinyinIME] Stopping existing dict service instance...\n");
            stopService(3000);
            Sleep(500);  // 给 Windows 一点时间清理内核句柄
        }

        // 再启动新实例
        OutputDebugStringW(L"[PinyinIME] Starting dict service...\n");

        HANDLE hProcess = startService(exeDir);
        if (!hProcess) {
            return false;
        }

        bool ready = waitForServiceReady();
        if (ready) {
            OutputDebugStringW(L"[PinyinIME] Dict service started and ready\n");
        } else {
            OutputDebugStringW(L"[PinyinIME] Dict service started but not ready in time\n");
        }

        CloseHandle(hProcess);
        return ready;
    }

    // ── 注册开机自启 (Task Scheduler) ───────────────────────────────────
    //
    // 通过 Windows Task Scheduler 2.0 COM API 注册登录时自动启动服务的任务。
    //
    // 触发器: TASK_TRIGGER_LOGON (用户登录时触发)
    // 执行动作: 启动 PinyinIMEDictService.exe
    // WorkingDirectory: 设置为 exe 所在目录 (避免 CWD 默认变成 System32)
    //
    // 服务通过 GetModuleFileNameW 找到自身路径并加载 dict.bin,
    // 但设置 WorkingDirectory 可以确保子进程 (dict_compiler.exe)
    // 和相对路径依赖在正确的目录下运行。
    //
    // 返回 true 表示注册成功

    static bool installAutoStart(const std::wstring& exeDir) {
        // 规范化路径: 去掉末尾多余的反斜杠
        std::wstring dir = exeDir;
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) {
            dir.pop_back();
        }
        std::wstring servicePath = dir + L"\\" + PinyinIME_DICT_SERVICE_EXE;

        // 初始化 COM (可能已被调用方初始化, S_FALSE 表示已初始化)
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool needUninit = (hr == S_OK);

        bool result = false;
        ITaskService* pService = nullptr;
        ITaskFolder* pRootFolder = nullptr;
        ITaskDefinition* pTask = nullptr;
        IActionCollection* pActionCollection = nullptr;
        IAction* pAction = nullptr;
        IExecAction* pExecAction = nullptr;
        ITriggerCollection* pTriggerCollection = nullptr;
        ITrigger* pTrigger = nullptr;
        ILogonTrigger* pLogonTrigger = nullptr;
        IRegisteredTask* pRegisteredTask = nullptr;
        ITaskSettings* pSettings = nullptr;

        hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                              IID_ITaskService, (void**)&pService);
        if (FAILED(hr)) {
            OutputDebugStringW(L"[PinyinIME] Failed to create TaskScheduler instance\n");
            goto cleanup;
        }

        hr = pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
        if (FAILED(hr)) {
            OutputDebugStringW(L"[PinyinIME] Failed to connect to Task Scheduler\n");
            goto cleanup;
        }

        hr = pService->GetFolder(_bstr_t(L"\\"), &pRootFolder);
        if (FAILED(hr)) {
            OutputDebugStringW(L"[PinyinIME] Failed to get root task folder\n");
            goto cleanup;
        }

        // 如果同名任务已存在, 先删除
        pRootFolder->DeleteTask(_bstr_t(L"PinyinIME Dict Service"), 0);

        hr = pService->NewTask(0, &pTask);
        if (FAILED(hr)) goto cleanup;

        // ── 设置触发器: ONLOGON ──────────────────────────────────────────
        hr = pTask->get_Triggers(&pTriggerCollection);
        if (FAILED(hr)) goto cleanup;

        hr = pTriggerCollection->Create(TASK_TRIGGER_LOGON, &pTrigger);
        if (FAILED(hr)) goto cleanup;

        hr = pTrigger->QueryInterface(IID_ILogonTrigger, (void**)&pLogonTrigger);
        if (FAILED(hr)) goto cleanup;

        pLogonTrigger->put_Id(_bstr_t(L"LogonTrigger"));

        // ── 设置执行动作 ──────────────────────────────────────────────────
        hr = pTask->get_Actions(&pActionCollection);
        if (FAILED(hr)) goto cleanup;

        hr = pActionCollection->Create(TASK_ACTION_EXEC, &pAction);
        if (FAILED(hr)) goto cleanup;

        hr = pAction->QueryInterface(IID_IExecAction, (void**)&pExecAction);
        if (FAILED(hr)) goto cleanup;

        // 程序路径 (对应界面上的 "程序或脚本")
        hr = pExecAction->put_Path(_bstr_t(servicePath.c_str()));
        if (FAILED(hr)) goto cleanup;

        // ★ 工作目录 (对应界面上的 "起始于(可选)")
        // 不设置此项时, CWD 默认为 C:\Windows\System32,
        // 可能导致相对路径依赖 (如 dict_compiler.exe 子进程) 找不到文件
        hr = pExecAction->put_WorkingDirectory(_bstr_t(dir.c_str()));
        if (FAILED(hr)) {
            OutputDebugStringW(L"[PinyinIME] Failed to set WorkingDirectory\n");
            goto cleanup;
        }

        // ── 设置任务属性 ──────────────────────────────────────────────────
        hr = pTask->get_Settings(&pSettings);
        if (SUCCEEDED(hr)) {
            pSettings->put_StartWhenAvailable(VARIANT_TRUE);
            pSettings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
            pSettings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
            pSettings->put_ExecutionTimeLimit(_bstr_t(L"PT0S"));  // 无时间限制
            pSettings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        }

        // ── 注册任务 ──────────────────────────────────────────────────────
        hr = pRootFolder->RegisterTaskDefinition(
            _bstr_t(L"PinyinIME Dict Service"),
            pTask,
            TASK_CREATE_OR_UPDATE,
            _variant_t(),                             // user (当前用户)
            _variant_t(),                             // password
            TASK_LOGON_INTERACTIVE_TOKEN,             // 仅交互式登录
            _variant_t(L""),
            &pRegisteredTask);

        if (SUCCEEDED(hr)) {
            OutputDebugStringW(L"[PinyinIME] Auto-start scheduled task created "
                               L"(with WorkingDirectory)\n");
            result = true;
        } else {
            wchar_t buf[64];
            swprintf(buf, 64, L"[PinyinIME] RegisterTaskDefinition failed: 0x%08X\n", hr);
            OutputDebugStringW(buf);
        }

    cleanup:
        if (pRegisteredTask) pRegisteredTask->Release();
        if (pSettings) pSettings->Release();
        if (pLogonTrigger) pLogonTrigger->Release();
        if (pTrigger) pTrigger->Release();
        if (pTriggerCollection) pTriggerCollection->Release();
        if (pExecAction) pExecAction->Release();
        if (pAction) pAction->Release();
        if (pActionCollection) pActionCollection->Release();
        if (pTask) pTask->Release();
        if (pRootFolder) pRootFolder->Release();
        if (pService) pService->Release();
        if (needUninit) CoUninitialize();

        return result;
    }

    // ── 卸载开机自启 ────────────────────────────────────────────────────

    static bool uninstallAutoStart() {
        std::wstring cmd = L"schtasks.exe /Delete /TN \"PinyinIME Dict Service\" /F";

        // CreateProcessW may modify lpCommandLine, so copy to mutable buffer
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {};

        BOOL ok = CreateProcessW(
            nullptr,
            cmdBuf.data(),
            nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW,
            nullptr, nullptr,
            &si, &pi);

        if (!ok) return false;

        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }

    // ── 停止服务进程 ────────────────────────────────────────────────────
    //
    // Two-phase shutdown:
    //   1. Signal the named stop event → service exits gracefully via
    //      its WaitForMultipleObjects loop.
    //   2. If the service doesn't exit within timeout, enumerate processes
    //      and terminate PinyinIMEDictService.exe directly.
    //
    // Returns true if the service is confirmed stopped.

    static bool stopService(DWORD timeoutMs = 5000) {
        if (!isServiceRunning()) {
            return true;  // Already stopped
        }

        // ── Phase 1: Graceful shutdown via named event ──────────────────
        {
            unique_handle hStopEvent(
                OpenEventW(EVENT_MODIFY_STATE, FALSE, PinyinIME_SERVICE_STOP_EVENT));

            if (hStopEvent.valid()) {
                SetEvent(hStopEvent.get());
                OutputDebugStringW(L"[ServiceManager] Signaled stop event\n");
            } else {
                OutputDebugStringW(L"[ServiceManager] Stop event not found "
                                   L"(service may be old version without stop support)\n");
            }
        }

        // ── Phase 2: Wait for the service to exit ───────────────────────
        DWORD elapsed = 0;
        const DWORD pollInterval = 200;

        while (elapsed < timeoutMs) {
            Sleep(pollInterval);
            elapsed += pollInterval;

            if (!isServiceRunning()) {
                OutputDebugStringW(L"[ServiceManager] Service stopped gracefully\n");
                return true;
            }
        }

        // ── Phase 3: Force terminate ────────────────────────────────────
        OutputDebugStringW(L"[ServiceManager] Graceful stop timed out, force terminating\n");

        DWORD svcPid = findServiceProcessId();
        if (svcPid == 0) {
            OutputDebugStringW(L"[ServiceManager] Could not find service process\n");
            return !isServiceRunning();  // May have exited between checks
        }

        unique_handle hProc(OpenProcess(
            PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION,
            FALSE, svcPid));

        if (!hProc.valid()) {
            OutputDebugStringW(L"[ServiceManager] Cannot open service process for termination\n");
            return false;
        }

        if (!TerminateProcess(hProc.get(), 0)) {
            OutputDebugStringW(L"[ServiceManager] TerminateProcess failed\n");
            return false;
        }

        // Wait for process to actually exit
        WaitForSingleObject(hProc.get(), 3000);
        CloseHandle(hProc.release());

        bool stopped = !isServiceRunning();
        if (stopped) {
            OutputDebugStringW(L"[ServiceManager] Service force-terminated\n");
        }
        return stopped;
    }

private:
    // Find the PID of PinyinIMEDictService.exe by enumerating processes.
    static DWORD findServiceProcessId() {
        DWORD pid = 0;
        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnapshot == INVALID_HANDLE_VALUE) return 0;

        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);

        if (Process32FirstW(hSnapshot, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, PinyinIME_DICT_SERVICE_EXE) == 0) {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(hSnapshot, &pe));
        }

        CloseHandle(hSnapshot);
        return pid;
    }

public:
};
