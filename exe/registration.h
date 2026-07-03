// exe/registration.h — TSF 输入法注册逻辑
// 替换旧的 registerIMEToSystem, 使用 COM + TSF 标准 API
#pragma once
#include <windows.h>
#include <commctrl.h>
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

// ==================== 占用 DLL 的进程信息 ====================
struct LockedProcess {
    std::wstring name;
    DWORD pid;
};

// 解析逗号分隔的 PID 列表并强制终止 (供 UAC 提权后的进程使用)
inline void forceKillByPidString(const std::wstring& pidList) {
    if (pidList.empty()) return;
    std::wstring remaining = pidList;
    size_t comma = 0;
    while ((comma = remaining.find(L',')) != std::wstring::npos || !remaining.empty()) {
        std::wstring token;
        if (comma != std::wstring::npos) {
            token = remaining.substr(0, comma);
            remaining = remaining.substr(comma + 1);
        } else {
            token = remaining;
            remaining.clear();
        }
        if (token.empty()) continue;
        DWORD pid = (DWORD)_wtoi(token.c_str());
        if (pid == 0) continue;
        HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (hProc) {
            TerminateProcess(hProc, 0);
            CloseHandle(hProc);
        }
    }
}

// ==================== 查找并释放占用 DLL 的进程 ====================
// 返回仍占用 DLL 的进程列表 (含 PID, 供后续强制终止)
inline std::vector<LockedProcess> findAndReleaseDllLocks(const wchar_t* dllPath) {
    std::vector<LockedProcess> remaining;
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
                std::wstring lowerName = procName;
                for (auto& c : lowerName) c = towlower(c);

                if (safeToKill.count(lowerName)) {
                    // 安全进程: 直接终止 (它们会自动重启且不加载我们的 DLL)
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (hProc) {
                        TerminateProcess(hProc, 0);
                        CloseHandle(hProc);
                    }
                } else {
                    // 其他进程: 记录下来 (含 PID, 供强制终止功能使用)
                    remaining.push_back({procName, pe.th32ProcessID});
                }
            }
        } while (Process32NextW(hSnapshot, &pe));
    }

    CloseHandle(hSnapshot);
    return remaining;
}
