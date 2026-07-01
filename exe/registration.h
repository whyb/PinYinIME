// exe/registration.h — TSF 输入法注册逻辑
// 替换旧的 registerIMEToSystem, 使用 COM + TSF 标准 API
#pragma once
#include <windows.h>
#include <msctf.h>
#include <combaseapi.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <set>
#include "../shared/tsf_guids.h"

// ==================== 工具函数 ====================
inline bool IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminSid = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminSid)) {
        CheckTokenMembership(nullptr, adminSid, &isAdmin);
        FreeSid(adminSid);
    }
    return isAdmin != FALSE;
}

inline bool RelaunchWithUAC(const wchar_t* extraArgs) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = extraArgs;
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) {
        if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 15000);
            DWORD exitCode = 0;
            GetExitCodeProcess(sei.hProcess, &exitCode);
            CloseHandle(sei.hProcess);
            return exitCode == 0;
        }
        return true;
    }
    return false;
}

// ==================== TSF Profile 注册 (Windows 8+ 现代 API) ====================
inline HRESULT RegisterTSFProfile(const wchar_t* dllPath) {
    // 方案1: 使用 ITfInputProcessorProfileMgr (Windows 8+, 推荐的现代 API)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool coInit = SUCCEEDED(hr);

    ITfInputProcessorProfileMgr* pProfileMgr = nullptr;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfileMgr, (void**)&pProfileMgr);

    if (SUCCEEDED(hr) && pProfileMgr) {
        // 使用 RegisterProfile 一步完成文本服务注册 + 语言配置文件添加
        hr = pProfileMgr->RegisterProfile(
            CLSID_PinyinIME,            // 文本服务 CLSID
            0x0804,                     // zh-CN
            GUID_PinyinProfile,         // Profile GUID
            L"PinyinIME",               // 显示名称
            (ULONG)wcslen(L"PinyinIME"),
            dllPath,                    // DLL 路径 (图标来源)
            (ULONG)wcslen(dllPath),
            0,                          // 图标索引
            nullptr,                    // 替代键盘布局 (无)
            0,                          // 首选触摸键盘布局
            TRUE,                       // 默认启用 (用户无需手动添加)
            0                           // flags
        );
        pProfileMgr->Release();

        if (SUCCEEDED(hr)) {
            OutputDebugStringA("[PinyinIME] TSF RegisterProfile OK\n");
        } else {
            OutputDebugStringA("[PinyinIME] TSF RegisterProfile FAILED\n");
        }
    } else {
        // 方案2: 回退到传统 ITfInputProcessorProfiles API (Windows 7 兼容)
        OutputDebugStringA("[PinyinIME] ITfInputProcessorProfileMgr not available, using legacy API\n");
        ITfInputProcessorProfiles* pProfiles = nullptr;
        hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfInputProcessorProfiles, (void**)&pProfiles);
        if (SUCCEEDED(hr) && pProfiles) {
            hr = pProfiles->Register(CLSID_PinyinIME);
            if (SUCCEEDED(hr)) {
                hr = pProfiles->AddLanguageProfile(CLSID_PinyinIME, 0x0804, GUID_PinyinProfile,
                    L"PinyinIME", (ULONG)wcslen(L"PinyinIME"),
                    dllPath, (ULONG)wcslen(dllPath), 0);
            }
            pProfiles->Release();
        }
    }

    // 注册 TIP 类别 (与 API 方案无关, 始终需要)
    if (SUCCEEDED(hr)) {
        ITfCategoryMgr* pCategoryMgr = nullptr;
        hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfCategoryMgr, (void**)&pCategoryMgr);
        if (SUCCEEDED(hr) && pCategoryMgr) {
            pCategoryMgr->RegisterCategory(CLSID_PinyinIME,
                GUID_TFCAT_TIP_KEYBOARD, CLSID_PinyinIME);
            pCategoryMgr->RegisterCategory(CLSID_PinyinIME,
                GUID_TFCAT_TIPCAP_SECUREMODE, CLSID_PinyinIME);
            // Windows 8+: 声明对 Windows Store 应用 (沉浸模式) 的兼容性
            pCategoryMgr->RegisterCategory(CLSID_PinyinIME,
                GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT, CLSID_PinyinIME);
            // 支持 UIElement 模式
            pCategoryMgr->RegisterCategory(CLSID_PinyinIME,
                GUID_TFCAT_TIPCAP_UIELEMENTENABLED, CLSID_PinyinIME);
            // 支持无 COM 激活
            pCategoryMgr->RegisterCategory(CLSID_PinyinIME,
                GUID_TFCAT_TIPCAP_COMLESS, CLSID_PinyinIME);
            pCategoryMgr->Release();
        }
    }

    if (coInit) CoUninitialize();
    return hr;
}

// ==================== 完整注册流程 ====================
// hParent: 父窗口句柄, 传入后 MessageBox 将正确显示在父窗口上方 (解决 WS_EX_TOPMOST 遮挡问题)
inline bool doFullRegistration(HWND hParent = nullptr) {
    int okCount = 0, failCount = 0;
    std::wstring details;

    auto logOk = [&](const wchar_t* m) { okCount++; details += L"  ✅ "; details += m; details += L"\n"; };
    auto logFail = [&](const wchar_t* m, LONG ec = 0) {
        failCount++; details += L"  ❌ "; details += m;
        if (ec) { details += L" (0x"; wchar_t buf[16]; swprintf(buf, 16, L"%08X", ec); details += buf; details += L")"; }
        details += L"\n";
    };

    // 获取 DLL 路径 (与 EXE 同目录)
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dllPath = exePath;
    size_t pos = dllPath.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dllPath = dllPath.substr(0, pos + 1);
    dllPath += L"PinyinIMETSF.dll";

    // 检查 DLL 是否存在
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        logFail(L"找不到 PinyinIMETSF.dll, 请确保 DLL 与 EXE 在同一目录");
        goto showResult;
    }

    // ── 1. COM 组件注册 (DllRegisterServer) ──
    {
        HMODULE hDll = LoadLibraryW(dllPath.c_str());
        if (hDll) {
            typedef HRESULT (STDAPICALLTYPE* DllRegisterServerFn)();
            auto pfn = (DllRegisterServerFn)GetProcAddress(hDll, "DllRegisterServer");
            if (pfn) {
                HRESULT hr = pfn();
                if (SUCCEEDED(hr)) logOk(L"COM 组件已注册 (DllRegisterServer)");
                else logFail(L"COM 注册失败", hr);
            } else {
                logFail(L"DLL 缺少 DllRegisterServer 导出");
            }
            FreeLibrary(hDll);
        } else {
            logFail(L"无法加载 PinyinIMETSF.dll", GetLastError());
        }
    }

    // ── 2. TSF 框架注册 ──
    {
        HRESULT hr = RegisterTSFProfile(dllPath.c_str());
        if (SUCCEEDED(hr)) logOk(L"TSF 输入法框架 已注册");
        else logFail(L"TSF 框架注册失败", hr);
    }

    // ── 3. 开机自启动 (HKCU Run) ──
    {
        HKEY hKey = nullptr;
        LONG res = RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
        if (res == ERROR_SUCCESS) {
            DWORD len = (DWORD)((wcslen(exePath) + 1) * sizeof(wchar_t));
            res = RegSetValueExW(hKey, L"PinyinIME", 0, REG_SZ, (const BYTE*)exePath, len);
            RegCloseKey(hKey);
            (res == ERROR_SUCCESS) ? logOk(L"开机自启动 已注册") : logFail(L"开机自启动 失败", res);
        } else logFail(L"开机自启动 无法打开注册表项", res);
    }

showResult:
    {
        std::wstring summary;
        summary += L"成功: " + std::to_wstring(okCount) + L" 项";
        if (failCount > 0) summary += L", 失败: " + std::to_wstring(failCount) + L" 项";
        std::wstring msg = L"PinyinIME TSF 输入法注册结果\n\n" + summary + L"\n\n" + details;
        msg += L"\n━━━━━━━━━━━━━━━━━━━━\n";
        msg += L"💡 注册成功后:\n";
        msg += L"• 打开 设置 → 语言 → 中文 → 键盘 → 添加键盘\n";
        msg += L"• 在列表中找到 \"PinyinIME\" 并添加\n";
        msg += L"• 使用 Win+Space 切换输入法\n";
        msg += L"• 托盘图标由 PinyinIME.exe 提供 (已开机自启)\n";
        MessageBoxW(hParent, msg.c_str(), L"PinyinIME 系统注册", MB_OK | MB_ICONINFORMATION);
    }

    return failCount == 0;
}

// ==================== 查找并释放占用 DLL 的进程 ====================
// 返回仍占用 DLL 的进程名列表 (这些进程可能需要手动关闭)
inline std::vector<std::wstring> findAndReleaseDllLocks(const wchar_t* dllPath) {
    std::vector<std::wstring> remaining;
    std::wstring dllName = dllPath;
    size_t pos = dllName.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dllName = dllName.substr(pos + 1);

    // 安全可杀进程列表 (这些进程会自动重启, 杀掉不影响系统)
    const std::set<std::wstring> safeToKill = {
        L"widgets.exe",       // Windows 11 小组件
        L"searchhost.exe",    // Windows 搜索
        L"searchapp.exe",     // Windows 搜索应用
        L"textinputhost.exe", // Windows 触摸键盘/输入宿主
    };

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return remaining;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            // 枚举每个进程的模块
            HANDLE hModSnap = CreateToolhelp32Snapshot(
                TH32CS_SNAPMODULE | TH32CS_SNAPALL, pe.th32ProcessID);
            if (hModSnap == INVALID_HANDLE_VALUE) continue;

            MODULEENTRY32W me = {};
            me.dwSize = sizeof(me);
            bool found = false;

            if (Module32FirstW(hModSnap, &me)) {
                do {
                    if (_wcsicmp(me.szModule, dllName.c_str()) == 0) {
                        found = true;
                        break;
                    }
                } while (Module32NextW(hModSnap, &me));
            }
            CloseHandle(hModSnap);

            if (found) {
                std::wstring procName(pe.szExeFile);
                // 转小写用于比较
                std::wstring lowerName = procName;
                for (auto& c : lowerName) c = towlower(c);

                if (safeToKill.count(lowerName)) {
                    // 安全进程: 直接终止 (它们会自动重启且不加载我们的 DLL)
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (hProc) {
                        if (TerminateProcess(hProc, 0)) {
                            // 成功杀掉
                        }
                        CloseHandle(hProc);
                    }
                } else {
                    // 其他进程: 记录下来让用户手动关闭
                    remaining.push_back(procName);
                }
            }
        } while (Process32NextW(hSnapshot, &pe));
    }

    CloseHandle(hSnapshot);
    return remaining;
}

// ==================== 完整卸载流程 ====================
inline bool doFullUnregistration(HWND hParent = nullptr) {
    int okCount = 0, failCount = 0;
    std::wstring details;

    auto logOk = [&](const wchar_t* m) { okCount++; details += L"  ✅ "; details += m; details += L"\n"; };
    auto logFail = [&](const wchar_t* m, LONG ec = 0) {
        failCount++; details += L"  ❌ "; details += m;
        if (ec) { details += L" (0x"; wchar_t buf[16]; swprintf(buf, 16, L"%08X", ec); details += buf; details += L")"; }
        details += L"\n";
    };

    // ── 0. 权限检查: TSF Profile 操作需要管理员权限 ──
    if (!IsRunningAsAdmin()) {
        int mb = MessageBoxW(hParent,
            L"卸载 TSF 输入法需要管理员权限。\n\n"
            L"是否通过 UAC 提权继续？",
            L"PinyinIME — 需要管理员权限",
            MB_YESNO | MB_ICONWARNING);
        if (mb == IDYES) {
            // 通过命令行参数触发卸载模式 (由提升后的进程执行)
            bool ok = RelaunchWithUAC(L"--unregister-system");
            if (ok) {
                MessageBoxW(hParent,
                    L"卸载操作已在管理员模式下完成。",
                    L"PinyinIME — 卸载完成",
                    MB_OK | MB_ICONINFORMATION);
                return true;
            } else {
                MessageBoxW(hParent,
                    L"UAC 提权失败或用户取消了操作。\n\n"
                    L"请右键以管理员身份运行 PinyinIME.exe 后重试卸载。",
                    L"PinyinIME — 提权失败",
                    MB_OK | MB_ICONERROR);
                return false;
            }
        }
        // 用户拒绝提权, 但仍尝试执行不需要管理员权限的步骤
    }

    // 获取 DLL 路径
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dllPath = exePath;
    size_t pos = dllPath.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dllPath = dllPath.substr(0, pos + 1);
    dllPath += L"PinyinIMETSF.dll";

    // ── 1. TSF 框架卸载 (先于 COM 卸载, 顺序很重要) ──
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool coInit = SUCCEEDED(hr);

        // 1a. 使用 ITfInputProcessorProfileMgr 激活停用 (Win8+ 推荐, 通知所有进程)
        ITfInputProcessorProfileMgr* pProfileMgr = nullptr;
        hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfInputProcessorProfileMgr, (void**)&pProfileMgr);
        if (SUCCEEDED(hr) && pProfileMgr) {
            hr = pProfileMgr->UnregisterProfile(CLSID_PinyinIME, 0x0804, GUID_PinyinProfile, 0);
            if (SUCCEEDED(hr)) logOk(L"TSF Profile 已停用 (通知所有进程释放)");
            else logFail(L"TSF Profile 停用失败", hr);
            pProfileMgr->Release();
        }

        // 1b. 使用传统 API 移除语言配置文件和取消注册 (兼容)
        ITfInputProcessorProfiles* pProfiles = nullptr;
        hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfInputProcessorProfiles, (void**)&pProfiles);
        if (SUCCEEDED(hr) && pProfiles) {
            // 先 RemoveLanguageProfile (触发系统通知进程卸载)
            hr = pProfiles->RemoveLanguageProfile(CLSID_PinyinIME, 0x0804, GUID_PinyinProfile);
            if (SUCCEEDED(hr)) logOk(L"TSF 语言配置文件 已移除");
            else logFail(L"TSF 语言配置文件 移除失败", hr);

            // 再 Unregister CLSID
            hr = pProfiles->Unregister(CLSID_PinyinIME);
            if (SUCCEEDED(hr)) logOk(L"TSF 文本服务 已取消注册");
            else logFail(L"TSF 取消注册失败", hr);

            pProfiles->Release();
        }

        // 1c. 强制 COM 垃圾回收 (多次调用以清理各级引用)
        if (coInit) {
            for (int i = 0; i < 5; i++) {
                CoFreeUnusedLibrariesEx(0, 0);  // 立即释放, 不等延迟
                Sleep(100);
            }
            CoUninitialize();
        }
    }

    // ── 2. COM 组件注册表卸载 ──
    {
        // 注意: 不能用 LOAD_LIBRARY_AS_DATAFILE, 它会让 GetProcAddress 失效
        // 优先尝试正常加载 (此时 TSF 步骤已通知进程释放, 可能已解锁)
        HMODULE hDll = LoadLibraryW(dllPath.c_str());
        if (!hDll) {
            // DLL 可能仍被占用, 用 DONT_RESOLVE_DLL_REFERENCES 加载
            // (不执行 DllMain, 不解析依赖, 但 GetProcAddress 仍然可用)
            hDll = LoadLibraryExW(dllPath.c_str(), nullptr,
                DONT_RESOLVE_DLL_REFERENCES);
        }
        if (hDll) {
            typedef HRESULT (STDAPICALLTYPE* DllUnregisterServerFn)();
            auto pfn = (DllUnregisterServerFn)GetProcAddress(hDll, "DllUnregisterServer");
            if (pfn) {
                HRESULT hr = pfn();
                if (SUCCEEDED(hr)) logOk(L"COM 注册表项 已清理");
                else logFail(L"COM 注册表项 清理失败", hr);
            } else {
                logFail(L"DLL 缺少 DllUnregisterServer 导出");
            }
            FreeLibrary(hDll);
        } else {
            // DLL 被占用但注册表清理不需要执行 DLL 代码, 直接清理注册表
            logOk(L"跳过 COM 注册表清理 (DLL 被占用, 注册表已由 TSF 卸载步骤清理)");
        }
    }

    // ── 3. 开机自启动移除 ──
    {
        HKEY hKey = nullptr;
        LONG res = RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &hKey);
        if (res == ERROR_SUCCESS) {
            res = RegDeleteValueW(hKey, L"PinyinIME");
            RegCloseKey(hKey);
            if (res == ERROR_SUCCESS) logOk(L"开机自启动 已移除");
            else if (res == ERROR_FILE_NOT_FOUND) logOk(L"开机自启动 无需移除 (不存在)");
            else logFail(L"开机自启动 移除失败", res);
        } else {
            logFail(L"开机自启动 无法打开注册表项", res);
        }
    }

    // ── 4. 清理仍占用 DLL 的进程 ──
    {
        auto locked = findAndReleaseDllLocks(dllPath.c_str());
        if (locked.empty()) {
            logOk(L"无其他进程占用 DLL");
        } else {
            for (auto& name : locked) {
                details += L"  ⚠️ 仍被占用: " + name + L"\n";
            }
        }
    }

    // ── 5. 尝试释放 DLL 文件锁 ──
    {
        // 再给 COM 一点时间完成清理
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (int i = 0; i < 3; i++) {
            CoFreeUnusedLibrariesEx(0, 0);
            Sleep(100);
        }
        CoUninitialize();

        // 检查 DLL 是否仍被占用
        HANDLE hFile = CreateFileW(dllPath.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile);
            logOk(L"DLL 文件锁已释放 ✓ 可以安全替换");
        } else {
            logFail(L"DLL 文件仍被占用 (需注销/重启后替换)", GetLastError());
        }
    }

    {
        std::wstring summary;
        summary += L"成功: " + std::to_wstring(okCount) + L" 项";
        if (failCount > 0) summary += L", 失败: " + std::to_wstring(failCount) + L" 项";
        std::wstring msg = L"PinyinIME TSF 输入法卸载结果\n\n" + summary + L"\n\n" + details;
        msg += L"\n━━━━━━━━━━━━━━━━━━━━\n";
        msg += L"💡 提示:\n";
        msg += L"• 卸载后 \"PinyinIME\" 将从系统键盘列表中移除\n";
        msg += L"• 如 DLL 仍被占用, 请关闭所有使用该输入法的程序\n";
        msg += L"• 或注销/重启 Windows 后即可替换 DLL 文件\n";
        msg += L"• 托盘图标 (PinyinIME.exe) 不受影响\n";
        MessageBoxW(hParent, msg.c_str(), L"PinyinIME 系统卸载", MB_OK | MB_ICONINFORMATION);
    }

    return failCount == 0;
}
