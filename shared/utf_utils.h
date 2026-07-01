// shared/utf_utils.h — UTF 编码转换工具函数
// 纯函数, 无外部依赖 (仅需 Windows.h 中的 MultiByteToWideChar / WideCharToMultiByte)
#pragma once
#include <windows.h>
#include <string>

inline std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, nullptr, 0);
    UINT codePage = CP_UTF8;
    if (len <= 0) {
        codePage = CP_ACP;
        len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    }
    if (len <= 0) return L"";
    std::wstring result(len - 1, 0);
    MultiByteToWideChar(codePage, 0, s.c_str(), -1, &result[0], len);
    return result;
}

inline std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &result[0], len, nullptr, nullptr);
    return result;
}

// 获取模块所在目录 (用于定位配置文件/词典)
// 传入 GetModuleHandle(nullptr) 或 g_hDllInst 均可
inline std::string getModuleDirectory(HMODULE hModule) {
    char path[MAX_PATH];
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::string s(path);
    size_t pos = s.find_last_of("\\/");
    if (pos != std::string::npos) s = s.substr(0, pos + 1);
    return s;
}
