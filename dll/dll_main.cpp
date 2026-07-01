// dll/dll_main.cpp — PinyinIMETSF.dll COM 入口
// 实现: DllMain, DllRegisterServer, DllUnregisterServer, DllGetClassObject, DllCanUnloadNow
#include <windows.h>
#include <string>
#include "../shared/tsf_guids.h"
#include "text_service.h"  // CPinyinTextService 完整定义 (class_factory 需要)
#include "class_factory.h"

HINSTANCE g_hDllInst = nullptr;
LONG g_cDllRef = 0;

// 全局引擎指针 (供候选窗口访问)
class PinyinEngine;
PinyinEngine* g_pSharedEngine = nullptr;

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hDllInst = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}

// ── DllRegisterServer: 将 COM CLSID 写入注册表 ──
STDAPI DllRegisterServer() {
    wchar_t dllPath[MAX_PATH];
    GetModuleFileNameW(g_hDllInst, dllPath, MAX_PATH);

    // 构建注册表路径: HKCR\CLSID\{A1B2C3D4-...}
    wchar_t clsidStr[64];
    StringFromGUID2(CLSID_PinyinIME, clsidStr, 64);

    std::wstring basePath = L"CLSID\\";
    basePath += clsidStr;

    HKEY hKeyClsid = nullptr;
    LONG res = RegCreateKeyExW(HKEY_CLASSES_ROOT, basePath.c_str(),
        0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKeyClsid, nullptr);
    if (res != ERROR_SUCCESS) return E_FAIL;
    RegSetValueExW(hKeyClsid, nullptr, 0, REG_SZ,
        (const BYTE*)L"PinyinIME TSF Text Service", 56);

    // InprocServer32 子键
    HKEY hKeyInproc = nullptr;
    res = RegCreateKeyExW(hKeyClsid, L"InprocServer32",
        0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKeyInproc, nullptr);
    if (res == ERROR_SUCCESS) {
        RegSetValueExW(hKeyInproc, nullptr, 0, REG_SZ,
            (const BYTE*)dllPath, (DWORD)((wcslen(dllPath) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKeyInproc, L"ThreadingModel", 0, REG_SZ,
            (const BYTE*)L"Apartment", 20);
        RegCloseKey(hKeyInproc);
    }
    RegCloseKey(hKeyClsid);
    return (res == ERROR_SUCCESS) ? S_OK : E_FAIL;
}

// ── DllUnregisterServer: 从注册表删除 COM CLSID ──
STDAPI DllUnregisterServer() {
    wchar_t clsidStr[64];
    StringFromGUID2(CLSID_PinyinIME, clsidStr, 64);
    std::wstring basePath = L"CLSID\\";
    basePath += clsidStr;
    std::wstring inprocPath = basePath + L"\\InprocServer32";

    RegDeleteKeyW(HKEY_CLASSES_ROOT, inprocPath.c_str());
    RegDeleteKeyW(HKEY_CLASSES_ROOT, basePath.c_str());
    return S_OK;
}

// ── DllCanUnloadNow ──
STDAPI DllCanUnloadNow() {
    return (g_cDllRef == 0) ? S_OK : S_FALSE;
}

// ── DllGetClassObject: 创建类工厂 ──
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;

    if (rclsid != CLSID_PinyinIME)
        return CLASS_E_CLASSNOTAVAILABLE;

    CPinyinClassFactory* pFactory = new (std::nothrow) CPinyinClassFactory();
    if (!pFactory) return E_OUTOFMEMORY;

    HRESULT hr = pFactory->QueryInterface(riid, ppv);
    pFactory->Release();
    return hr;
}
