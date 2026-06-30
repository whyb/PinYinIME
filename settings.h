// settings.h — PinyinIME 设置系统
// 功能: 繁体/简体切换, 皮肤配色, 用户词典管理, 模糊音, 智能纠错等
// 使用纯 Win32 API (comctl32), 无额外依赖
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "Advapi32.lib")
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <algorithm>

// ==================== UTF 转换工具函数 ====================
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

// ==================== 简繁转换 ====================
// 数据与转换函数统一在 s2t_data.h 中维护, 支持:
//   - 单字级映射 (2000+ 对, 涵盖《简化字总表》全部)
//   - 词汇级消歧映射 (一简对多繁, 如 发→發/髮)
//   - 两岸 IT 词汇差异 (如 软件→軟體)
//   - 最长匹配优先策略 (词级优先于字级)
#include "s2t_data.h"

// ==================== 设置数据结构 ====================
struct PinyinSettings {
    // === 语言 ===
    bool useTraditional = false;

    // === 外观 (默认 Dark 墨黑主题) ===
    COLORREF bgColor     = RGB(0x15, 0x15, 0x15);  // #151515
    COLORREF borderColor = RGB(0x33, 0x33, 0x33);  // #333333
    COLORREF textColor   = RGB(0xBD, 0xBD, 0xBD);  // #BDBDBD
    COLORREF indexColor  = RGB(0x76, 0x76, 0x76);  // #767676
    COLORREF inputColor  = RGB(0x5E, 0x97, 0xF6);  // #5E97F6 蓝色强调
    std::wstring fontName = L"Microsoft YaHei"; // 字体名称
    int fontSize         = 18;   // 字体高度 (负值=像素)
    int candidateCount   = 5;    // 候选词数量 5-9
    bool verticalLayout  = false; // 候选框竖排

    // === 模糊音 ===
    bool fuzzyZ_Zh  = false;
    bool fuzzyC_Ch  = false;
    bool fuzzyS_Sh  = false;
    bool fuzzyN_L   = false;
    bool fuzzyF_H   = false;
    bool fuzzyEn_Eng = false;
    bool fuzzyIn_Ing = false;

    // === 智能功能 ===
    bool smartCorrection  = true;
    bool autoWordCreate   = true;
    bool autoFreqAdjust   = true;
    bool chinesePunctuation = true;

    // === 快捷键 ===
    DWORD toggleHotkey = VK_RSHIFT;
    DWORD toggleModifier = 0;   // 修饰键: 0=无, VK_MENU=Alt, VK_CONTROL=Ctrl
    bool hasToggleModifierInFile = false;  // 配置文件是否显式设置了修饰键

    // 预设皮肤
    static const struct Skin {
        const wchar_t* name;
        COLORREF bg, border, text, index, input;
    } skins[];

    static const int SKIN_COUNT = 8;

    void applySkin(int idx) {
        if (idx < 0 || idx >= SKIN_COUNT) return;
        bgColor     = skins[idx].bg;
        borderColor = skins[idx].border;
        textColor   = skins[idx].text;
        indexColor  = skins[idx].index;
        inputColor  = skins[idx].input;
    }

    bool loadFromFile(const std::string& path) {
        std::ifstream fin(path);
        if (!fin.is_open()) return false;
        std::string line;
        while (std::getline(fin, line)) {
            if (line.empty() || line[0] == '#') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            // trim
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            val.erase(0, val.find_first_not_of(" \t"));
            val.erase(val.find_last_not_of(" \t") + 1);

            if (key == "useTraditional") useTraditional = (val == "1");
            else if (key == "bgColor") bgColor = (COLORREF)std::stoul(val);
            else if (key == "borderColor") borderColor = (COLORREF)std::stoul(val);
            else if (key == "textColor") textColor = (COLORREF)std::stoul(val);
            else if (key == "indexColor") indexColor = (COLORREF)std::stoul(val);
            else if (key == "inputColor") inputColor = (COLORREF)std::stoul(val);
            else if (key == "fontName") fontName = utf8ToWide(val);
            else if (key == "fontSize") fontSize = std::stoi(val);
            else if (key == "candidateCount") candidateCount = std::stoi(val);
            else if (key == "verticalLayout") verticalLayout = (val == "1");
            else if (key == "fuzzyZ_Zh") fuzzyZ_Zh = (val == "1");
            else if (key == "fuzzyC_Ch") fuzzyC_Ch = (val == "1");
            else if (key == "fuzzyS_Sh") fuzzyS_Sh = (val == "1");
            else if (key == "fuzzyN_L") fuzzyN_L = (val == "1");
            else if (key == "fuzzyF_H") fuzzyF_H = (val == "1");
            else if (key == "fuzzyEn_Eng") fuzzyEn_Eng = (val == "1");
            else if (key == "fuzzyIn_Ing") fuzzyIn_Ing = (val == "1");
            else if (key == "smartCorrection") smartCorrection = (val == "1");
            else if (key == "autoWordCreate") autoWordCreate = (val == "1");
            else if (key == "autoFreqAdjust") autoFreqAdjust = (val == "1");
            else if (key == "chinesePunctuation") chinesePunctuation = (val == "1");
            else if (key == "toggleHotkey") toggleHotkey = (DWORD)std::stoul(val);
            else if (key == "toggleModifier") { toggleModifier = (DWORD)std::stoul(val); hasToggleModifierInFile = true; }
        }
        return true;
    }

    bool saveToFile(const std::string& path) {
        std::ofstream fout(path);
        if (!fout.is_open()) return false;
        fout << "# PinyinIME Settings\n";
        fout << "useTraditional=" << useTraditional << "\n";
        fout << "bgColor=" << (unsigned long)bgColor << "\n";
        fout << "borderColor=" << (unsigned long)borderColor << "\n";
        fout << "textColor=" << (unsigned long)textColor << "\n";
        fout << "indexColor=" << (unsigned long)indexColor << "\n";
        fout << "inputColor=" << (unsigned long)inputColor << "\n";
        fout << "fontName=" << wideToUtf8(fontName) << "\n";
        fout << "fontSize=" << fontSize << "\n";
        fout << "candidateCount=" << candidateCount << "\n";
        fout << "verticalLayout=" << verticalLayout << "\n";
        fout << "fuzzyZ_Zh=" << fuzzyZ_Zh << "\n";
        fout << "fuzzyC_Ch=" << fuzzyC_Ch << "\n";
        fout << "fuzzyS_Sh=" << fuzzyS_Sh << "\n";
        fout << "fuzzyN_L=" << fuzzyN_L << "\n";
        fout << "fuzzyF_H=" << fuzzyF_H << "\n";
        fout << "fuzzyEn_Eng=" << fuzzyEn_Eng << "\n";
        fout << "fuzzyIn_Ing=" << fuzzyIn_Ing << "\n";
        fout << "smartCorrection=" << smartCorrection << "\n";
        fout << "autoWordCreate=" << autoWordCreate << "\n";
        fout << "autoFreqAdjust=" << autoFreqAdjust << "\n";
        fout << "chinesePunctuation=" << chinesePunctuation << "\n";
        fout << "toggleHotkey=" << (unsigned long)toggleHotkey << "\n";
        fout << "toggleModifier=" << (unsigned long)toggleModifier << "\n";
        return true;
    }
};

// 预设皮肤定义 (5 款暗色 + 3 款亮色)
inline const PinyinSettings::Skin PinyinSettings::skins[] = {
    // ── Dark 系列 ──
    {L"Dark 墨黑",   RGB(0x15,0x15,0x15), RGB(0x33,0x33,0x33), RGB(0xBD,0xBD,0xBD), RGB(0x76,0x76,0x76), RGB(0x5E,0x97,0xF6)},
    {L"Dark 炭灰",   RGB(0x21,0x21,0x21), RGB(0x3C,0x3C,0x3C), RGB(0xC8,0xC8,0xC8), RGB(0x82,0x82,0x82), RGB(0x6D,0xA0,0xF8)},
    {L"Dark 暖咖",   RGB(0x1C,0x18,0x16), RGB(0x3D,0x34,0x30), RGB(0xC8,0xB8,0xA0), RGB(0x88,0x78,0x60), RGB(0xD4,0xA0,0x50)},
    {L"Dark 墨绿",   RGB(0x14,0x1A,0x14), RGB(0x2A,0x3A,0x2A), RGB(0xA0,0xC0,0xA0), RGB(0x60,0x80,0x60), RGB(0x50,0xB8,0x70)},
    {L"Dark 藏蓝",   RGB(0x14,0x16,0x20), RGB(0x2A,0x30,0x40), RGB(0xA0,0xB0,0xD0), RGB(0x68,0x78,0xA0), RGB(0x60,0x78,0xE0)},
    // ── 亮色系列 ──
    {L"亮色 浅灰",   RGB(0xF0,0xF0,0xF0), RGB(0xC0,0xC0,0xC0), RGB(0x33,0x33,0x33), RGB(0x70,0x70,0xC0), RGB(0x00,0x60,0xC0)},
    {L"亮色 纯白",   RGB(0xFF,0xFF,0xFF), RGB(0xD0,0xD0,0xD0), RGB(0x1A,0x1A,0x1A), RGB(0x58,0x58,0xB8), RGB(0x00,0x48,0xB8)},
    {L"亮色 暖米",   RGB(0xFF,0xF8,0xF0), RGB(0xD8,0xC8,0xB0), RGB(0x4A,0x3A,0x2A), RGB(0xB0,0x70,0x50), RGB(0xA0,0x50,0x30)},
};

// ==================== 系统注册功能 ====================
// 将 PinyinIME 注册到 Windows 输入法系统中

// 检查当前进程是否以管理员权限运行
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

// 通过 UAC 提权重启自身进程 (用于执行需管理员权限的操作)
inline bool RelaunchWithUAC(const wchar_t* extraArgs) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";        // 触发 UAC 提权对话框
    sei.lpFile = exePath;
    sei.lpParameters = extraArgs;
    sei.nShow = SW_SHOWNORMAL;

    if (ShellExecuteExW(&sei)) {
        if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 15000); // 等 15 秒
            DWORD exitCode = 0;
            GetExitCodeProcess(sei.hProcess, &exitCode);
            CloseHandle(sei.hProcess);
            return exitCode == 0;
        }
        return true;
    }
    // 用户取消 UAC 弹窗 (ERROR_CANCELLED)
    return false;
}

// 将 PinyinIME 注册到系统 (写入 4 项注册表)
// silent=true: 静默模式 (用于 --register-system 命令行), 不弹总结框
// 返回 true 表示所有步骤均成功
inline bool registerIMEToSystem(bool silent) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    int okCount = 0, failCount = 0, skipCount = 0;
    std::wstring details;

    auto logOk   = [&](const wchar_t* m) { okCount++;   details += L"  ✅ "; details += m; details += L"\n"; };
    auto logFail = [&](const wchar_t* m, LONG ec=0) { failCount++; details += L"  ❌ "; details += m;
        if (ec) { details += L" (代码 "; details += std::to_wstring(ec); details += L")"; }
        details += L"\n"; };
    auto logSkip = [&](const wchar_t* m) { skipCount++; details += L"  ⏭️ "; details += m; details += L"\n"; };

    // ── 1. 开机自启动 (HKCU) ────────────────────────────
    {
        HKEY hKey = nullptr;
        LONG res = RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_WRITE | KEY_SET_VALUE, nullptr, &hKey, nullptr);
        if (res == ERROR_SUCCESS) {
            DWORD dataLen = (DWORD)(wcslen(exePath) + 1) * sizeof(wchar_t);
            res = RegSetValueExW(hKey, L"PinyinIME", 0, REG_SZ,
                (const BYTE*)exePath, dataLen);
            RegCloseKey(hKey);
            (res == ERROR_SUCCESS) ? logOk(L"开机自启动 已注册")
                                   : logFail(L"开机自启动 写入失败", res);
        } else {
            logFail(L"开机自启动 无法打开注册表项", res);
        }
    }

    // ── 2. 键盘布局预加载 (HKCU) ───────────────────────
    {
        HKEY hKey = nullptr;
        LONG res = RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Keyboard Layout\\Preload",
            0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_READ | KEY_SET_VALUE, nullptr, &hKey, nullptr);
        if (res == ERROR_SUCCESS) {
            // 检查是否已注册过，避免重复添加
            std::wstring targetId = L"E0800804";
            bool alreadyExists = false;
            for (int slot = 1; slot <= 20; slot++) {
                std::wstring vn = std::to_wstring(slot);
                wchar_t data[64] = {};
                DWORD ds = sizeof(data);
                if (RegQueryValueExW(hKey, vn.c_str(), nullptr, nullptr,
                        (LPBYTE)data, &ds) == ERROR_SUCCESS && targetId == data) {
                    alreadyExists = true; break;
                }
            }
            if (alreadyExists) {
                logOk(L"键盘布局 已存在, 跳过");
            } else {
                int maxSlot = 0;
                for (int s = 1; s <= 20; s++) {
                    std::wstring vn = std::to_wstring(s);
                    DWORD d = 0, ds = sizeof(d);
                    if (RegQueryValueExW(hKey, vn.c_str(), nullptr, nullptr,
                            (LPBYTE)&d, &ds) == ERROR_SUCCESS) { if (s > maxSlot) maxSlot = s; }
                }
                std::wstring sn = std::to_wstring(maxSlot + 1);
                res = RegSetValueExW(hKey, sn.c_str(), 0, REG_SZ,
                    (const BYTE*)targetId.c_str(),
                    (DWORD)((targetId.size() + 1) * sizeof(wchar_t)));
                (res == ERROR_SUCCESS) ? logOk(L"键盘布局 已注册")
                                       : logFail(L"键盘布局 写入失败", res);
            }
            RegCloseKey(hKey);
        } else {
            logFail(L"键盘布局 无法打开注册表项", res);
        }
    }

    // ── 3. TSF 配置文件桩 (HKCU) ───────────────────────
    {
        const wchar_t* clsid = L"{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}";
        std::wstring tipPath = L"Software\\Microsoft\\CTF\\TIP\\";
        tipPath += clsid;

        HKEY hKey = nullptr;
        LONG res = RegCreateKeyExW(HKEY_CURRENT_USER, tipPath.c_str(),
            0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_WRITE | KEY_SET_VALUE, nullptr, &hKey, nullptr);
        if (res == ERROR_SUCCESS) {
            RegSetValueExW(hKey, L"Display Description", 0, REG_SZ,
                (const BYTE*)L"PinyinIME", 20);
            RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                (const BYTE*)L"PinyinIME 拼音输入法", 36);

            std::wstring catPath = tipPath;
            catPath += L"\\Category\\Item\\{34745C63-B2F0-4784-8B67-5E12C8701A31}";
            HKEY hCat = nullptr;
            LONG catRes = RegCreateKeyExW(HKEY_CURRENT_USER, catPath.c_str(),
                0, nullptr, REG_OPTION_NON_VOLATILE,
                KEY_WRITE | KEY_SET_VALUE, nullptr, &hCat, nullptr);
            if (hCat) RegCloseKey(hCat);
            RegCloseKey(hKey);
            (catRes == ERROR_SUCCESS) ? logOk(L"TSF 输入法配置文件 已注册")
                                       : logFail(L"TSF 输入法配置文件 写入失败", catRes);
        } else {
            logFail(L"TSF 输入法配置文件 无法打开注册表项", res);
        }
    }

    // ── 4. 系统级键盘布局 (HKLM, 需管理员) ────────────
    {
        LONG hklmErr = 0;
        {
            HKEY hKey = nullptr;
            LONG res = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\E0800804",
                0, nullptr, REG_OPTION_NON_VOLATILE,
                KEY_WRITE | KEY_SET_VALUE, nullptr, &hKey, nullptr);
            if (res == ERROR_SUCCESS) {
                RegSetValueExW(hKey, L"Layout File", 0, REG_SZ,
                    (const BYTE*)L"kbdus.dll", 20);
                RegSetValueExW(hKey, L"Layout Text", 0, REG_SZ,
                    (const BYTE*)L"PinyinIME Chinese", 36);
                RegSetValueExW(hKey, L"Layout Display Name", 0, REG_SZ,
                    (const BYTE*)L"PinyinIME 拼音输入法", 36);
                RegCloseKey(hKey);
                logOk(L"系统级键盘布局 已注册 (HKLM)");
            } else {
                hklmErr = res;
            }
        }

        if (hklmErr == ERROR_ACCESS_DENIED && hklmErr != 0) {
            // 权限不足 → 询问用户是否 UAC 提权 (静默模式直接提权)
            bool elevate = silent;
            if (!silent) {
                int mb = MessageBoxW(nullptr,
                    L"注册系统级键盘布局需要管理员权限。\n\n"
                    L"是否通过 UAC 提权继续？\n"
                    L"(选择「否」将跳过此步骤，输入法核心功能不受影响)",
                    L"PinyinIME — 需要管理员权限",
                    MB_YESNO | MB_ICONQUESTION);
                elevate = (mb == IDYES);
            }
            if (elevate) {
                if (RelaunchWithUAC(L"--register-system")) {
                    logOk(L"系统级键盘布局 已注册 (UAC 提权)");
                } else {
                    logSkip(L"系统级键盘布局 用户取消或提权失败");
                }
            } else {
                logSkip(L"系统级键盘布局 已跳过 (无管理员权限)");
            }
        } else if (hklmErr != 0) {
            logFail(L"系统级键盘布局 注册失败", hklmErr);
        }
    }

    // ── 显示结果 (非静默模式) ──────────────────────────
    if (!silent) {
        std::wstring summary;
        summary += L"成功: " + std::to_wstring(okCount) + L" 项";
        if (failCount > 0) summary += L", 失败: " + std::to_wstring(failCount) + L" 项";
        if (skipCount > 0) summary += L", 跳过: " + std::to_wstring(skipCount) + L" 项";

        std::wstring msg = L"PinyinIME 系统注册结果\n\n" + summary + L"\n\n" + details;
        msg += L"\n━━━━━━━━━━━━━━━━━━━━\n";
        msg += L"💡 提示:\n";
        msg += L"• HKCU 注册项无需管理员权限\n";
        msg += L"• 系统级键盘布局 (HKLM) 需要管理员权限\n";
        msg += L"• 即使跳过 HKLM，输入法仍可正常使用\n";
        msg += L"• 系统托盘可找到 PinyinIME 图标\n";

        MessageBoxW(nullptr, msg.c_str(),
            L"PinyinIME 系统注册", MB_OK | MB_ICONINFORMATION);
    }

    return failCount == 0;
}

// ==================== 前向声明 ====================
// 这些函数在 main.cpp 中实现 (在完整的类定义之后)
extern void userDictAddEntry(const std::string& pinyin, const std::string& word, int freq);
extern void userDictRemoveEntry(const std::string& pinyin, const std::string& word);
extern void userDictClearAll();
extern std::string userDictGetAllAsString();
extern void userDictSaveToFile();
extern void applySettingsToEngine(const PinyinSettings& s);

// 全局设置对象 (定义在 main.cpp)
extern PinyinSettings g_settings;

// ==================== 用户词典管理对话框 ====================
struct UserDictDialog {
    static std::vector<std::pair<std::string, std::pair<std::string, int>>> g_entries;
    static HWND g_hList;
    static HWND g_hParent;

    static void refreshList(HWND hList) {
        SendMessageW(hList, LB_RESETCONTENT, 0, 0);
        std::string all = userDictGetAllAsString();
        if (all.empty()) return;
        std::istringstream iss(all);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            std::wstring wline = utf8ToWide(line);
            SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)wline.c_str());
        }
    }

    static INT_PTR CALLBACK dlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_INITDIALOG: {
            g_hParent = hDlg;
            g_hList = GetDlgItem(hDlg, 101);
            refreshList(g_hList);
            return TRUE;
        }
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
            case 201: { // 添加
                HWND hPy = GetDlgItem(hDlg, 301);
                HWND hWd = GetDlgItem(hDlg, 302);
                wchar_t py[256] = {0}, wd[256] = {0};
                GetWindowTextW(hPy, py, 256);
                GetWindowTextW(hWd, wd, 256);
                if (py[0] && wd[0]) {
                    std::string spy = wideToUtf8(py);
                    std::string swd = wideToUtf8(wd);
                    userDictAddEntry(spy, swd, 8500);
                    refreshList(g_hList);
                    SetWindowTextW(hPy, L"");
                    SetWindowTextW(hWd, L"");
                }
                return TRUE;
            }
            case 202: { // 删除选中
                int sel = (int)SendMessageW(g_hList, LB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    wchar_t buf[512];
                    SendMessageW(g_hList, LB_GETTEXT, sel, (LPARAM)buf);
                    std::string line = wideToUtf8(buf);
                    size_t t1 = line.find('\t');
                    size_t t2 = line.find('\t', t1 + 1);
                    if (t1 != std::string::npos && t2 != std::string::npos) {
                        std::string py = line.substr(0, t1);
                        std::string wd = line.substr(t1 + 1, t2 - t1 - 1);
                        userDictRemoveEntry(py, wd);
                    }
                    refreshList(g_hList);
                }
                return TRUE;
            }
            case 203: { // 清空全部
                if (MessageBoxW(hDlg, L"确定清空所有用户词典吗？", L"确认", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    userDictClearAll();
                    refreshList(g_hList);
                }
                return TRUE;
            }
            case 204: EndDialog(hDlg, 0); return TRUE; // 关闭
            }
            break;
        }
        }
        return FALSE;
    }

    static void show(HINSTANCE hInst, HWND hParent) {
        // 使用 DialogBox 会阻塞，这里用 CreateDialog 实现非阻塞
        HWND hDlg = CreateDialogParamW(hInst, MAKEINTRESOURCEW(200), hParent, dlgProc, 0);
        ShowWindow(hDlg, SW_SHOW);
    }
};

inline HWND UserDictDialog::g_hList = nullptr;
inline HWND UserDictDialog::g_hParent = nullptr;
inline std::vector<std::pair<std::string, std::pair<std::string, int>>> UserDictDialog::g_entries;

// ==================== 主设置对话框 ====================
struct SettingsDialog {
    static PinyinSettings g_tempSettings;
    static HWND g_hSkinPreview;
    static int g_selectedSkin;

    // 自定义皮肤颜色预览
    static LRESULT CALLBACK previewWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);

            HBRUSH hBg = CreateSolidBrush(g_tempSettings.bgColor);
            FillRect(hdc, &rc, hBg);
            DeleteObject(hBg);

            HPEN hPen = CreatePen(PS_SOLID, 1, g_tempSettings.borderColor);
            HPEN oldPen = (HPEN)SelectObject(hdc, hPen);
            Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(hdc, oldPen);
            DeleteObject(hPen);

            HFONT hFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, g_tempSettings.fontName.c_str());
            SelectObject(hdc, hFont);
            SetBkMode(hdc, TRANSPARENT);

            SetTextColor(hdc, g_tempSettings.inputColor);
            TextOutW(hdc, 5, 5, L"[nihao] ", 8);

            SetTextColor(hdc, g_tempSettings.indexColor);
            TextOutW(hdc, 75, 5, L"1.", 2);
            SetTextColor(hdc, g_tempSettings.textColor);
            TextOutW(hdc, 90, 5, L"你好", 2);
            SetTextColor(hdc, g_tempSettings.indexColor);
            TextOutW(hdc, 125, 5, L"2.", 2);
            SetTextColor(hdc, g_tempSettings.textColor);
            TextOutW(hdc, 140, 5, L"泥嚎", 2);

            DeleteObject(hFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static void applySettingsToDialog(HWND hDlg) {
        // 基本设置
        CheckDlgButton(hDlg, 401, g_tempSettings.useTraditional ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(hDlg, 402, g_tempSettings.candidateCount, FALSE);
        SetDlgItemInt(hDlg, 403, g_tempSettings.fontSize, TRUE);
        CheckDlgButton(hDlg, 404, g_tempSettings.verticalLayout ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, 405, g_tempSettings.chinesePunctuation ? BST_CHECKED : BST_UNCHECKED);

        // 模糊音
        CheckDlgButton(hDlg, 501, g_tempSettings.fuzzyZ_Zh ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, 502, g_tempSettings.fuzzyC_Ch ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, 503, g_tempSettings.fuzzyS_Sh ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, 504, g_tempSettings.fuzzyN_L ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, 505, g_tempSettings.fuzzyF_H ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, 506, g_tempSettings.fuzzyEn_Eng ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, 507, g_tempSettings.fuzzyIn_Ing ? BST_CHECKED : BST_UNCHECKED);

        // 智能功能
        CheckDlgButton(hDlg, 601, g_tempSettings.smartCorrection ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, 602, g_tempSettings.autoWordCreate ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, 603, g_tempSettings.autoFreqAdjust ? BST_CHECKED : BST_UNCHECKED);

        // 皮肤下拉
        HWND hSkin = GetDlgItem(hDlg, 701);
        SendMessageW(hSkin, CB_RESETCONTENT, 0, 0);
        for (int i = 0; i < PinyinSettings::SKIN_COUNT; i++) {
            SendMessageW(hSkin, CB_ADDSTRING, 0, (LPARAM)PinyinSettings::skins[i].name);
        }
        SendMessageW(hSkin, CB_SETCURSEL, g_selectedSkin, 0);

        InvalidateRect(g_hSkinPreview, nullptr, TRUE);
    }

    static INT_PTR CALLBACK dlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_INITDIALOG: {
            g_tempSettings = g_settings;
            g_selectedSkin = 0;

            // 创建皮肤预览窗口
            g_hSkinPreview = CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                390, 15, 260, 35, hDlg, nullptr, nullptr, nullptr);
            SetWindowLongPtrW(g_hSkinPreview, GWLP_WNDPROC, (LONG_PTR)previewWndProc);

            applySettingsToDialog(hDlg);
            // 限制候选数量范围
            SendMessageW(GetDlgItem(hDlg, 402), EM_LIMITTEXT, 1, 0);
            return TRUE;
        }
        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT lpdis = (LPDRAWITEMSTRUCT)lp;
            if (lpdis->hwndItem == g_hSkinPreview) {
                // handled by previewWndProc via WM_PAINT
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            // 确保预览窗口背景刷新
            if ((HWND)lp == g_hSkinPreview) {
                InvalidateRect(g_hSkinPreview, nullptr, TRUE);
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);

            switch (id) {
            case 701: // 皮肤选择
                if (code == CBN_SELCHANGE) {
                    int sel = (int)SendMessageW(GetDlgItem(hDlg, 701), CB_GETCURSEL, 0, 0);
                    if (sel >= 0) {
                        g_selectedSkin = sel;
                        g_tempSettings.applySkin(sel);
                        InvalidateRect(g_hSkinPreview, nullptr, TRUE);
                    }
                }
                break;

            case 702: { // 自定义主色调
                CHOOSECOLORW cc = {};
                static COLORREF acr[16];
                cc.lStructSize = sizeof(cc);
                cc.hwndOwner = hDlg;
                cc.rgbResult = g_tempSettings.bgColor;
                cc.lpCustColors = acr;
                cc.Flags = CC_FULLOPEN | CC_RGBINIT;
                if (ChooseColorW(&cc)) {
                    // 基于选色生成一套配色
                    COLORREF base = cc.rgbResult;
                    int r = GetRValue(base), g = GetGValue(base), b = GetBValue(base);
                    g_tempSettings.bgColor = base;
                    g_tempSettings.borderColor = RGB((r*3)/4, (g*3)/4, (b*3)/4);
                    // 根据亮度决定文字颜色
                    int brightness = (r*299 + g*587 + b*114) / 1000;
                    if (brightness > 128) {
                        g_tempSettings.textColor = RGB((r*1)/5, (g*1)/5, (b*1)/5);
                        g_tempSettings.indexColor = RGB((r*1)/3, (g*2)/5, (b+100>255?255:b+100));
                        g_tempSettings.inputColor = RGB(0, (g*1)/4, (b*4)/5);
                    } else {
                        g_tempSettings.textColor = RGB(220, 220, 220);
                        g_tempSettings.indexColor = RGB((r+100>255?255:r+100), (g+80>255?255:g+80), 255);
                        g_tempSettings.inputColor = RGB(150, 200, 255);
                    }
                    g_selectedSkin = -1; // 自定义
                    SendMessageW(GetDlgItem(hDlg, 701), CB_SETCURSEL, -1, 0);
                    InvalidateRect(g_hSkinPreview, nullptr, TRUE);
                }
                break;
            }

            case 801: // 打开用户词典管理
                UserDictDialog::show((HINSTANCE)GetWindowLongPtrW(hDlg, GWLP_HINSTANCE), hDlg);
                break;

            case 901: { // 保存
                // 从控件读取设置
                g_tempSettings.useTraditional = (IsDlgButtonChecked(hDlg, 401) == BST_CHECKED);
                BOOL ok;
                int cnt = GetDlgItemInt(hDlg, 402, &ok, FALSE);
                if (ok && cnt >= 3 && cnt <= 9) g_tempSettings.candidateCount = cnt;
                int fs = GetDlgItemInt(hDlg, 403, &ok, TRUE);
                if (ok && fs >= 12 && fs <= 36) g_tempSettings.fontSize = fs;
                g_tempSettings.verticalLayout = (IsDlgButtonChecked(hDlg, 404) == BST_CHECKED);
                g_tempSettings.chinesePunctuation = (IsDlgButtonChecked(hDlg, 405) == BST_CHECKED);

                g_tempSettings.fuzzyZ_Zh = (IsDlgButtonChecked(hDlg, 501) == BST_CHECKED);
                g_tempSettings.fuzzyC_Ch = (IsDlgButtonChecked(hDlg, 502) == BST_CHECKED);
                g_tempSettings.fuzzyS_Sh = (IsDlgButtonChecked(hDlg, 503) == BST_CHECKED);
                g_tempSettings.fuzzyN_L = (IsDlgButtonChecked(hDlg, 504) == BST_CHECKED);
                g_tempSettings.fuzzyF_H = (IsDlgButtonChecked(hDlg, 505) == BST_CHECKED);
                g_tempSettings.fuzzyEn_Eng = (IsDlgButtonChecked(hDlg, 506) == BST_CHECKED);
                g_tempSettings.fuzzyIn_Ing = (IsDlgButtonChecked(hDlg, 507) == BST_CHECKED);

                g_tempSettings.smartCorrection = (IsDlgButtonChecked(hDlg, 601) == BST_CHECKED);
                g_tempSettings.autoWordCreate = (IsDlgButtonChecked(hDlg, 602) == BST_CHECKED);
                g_tempSettings.autoFreqAdjust = (IsDlgButtonChecked(hDlg, 603) == BST_CHECKED);

                // 应用设置
                g_settings = g_tempSettings;
                g_settings.saveToFile(getExeDirectory() + "pinyin_config.ini");
                applySettingsToEngine(g_settings);

                EndDialog(hDlg, 1);
                return TRUE;
            }
            case 902: // 取消
                EndDialog(hDlg, 0);
                return TRUE;
            }
            break;
        }
        }
        return FALSE;
    }

    static bool show(HINSTANCE hInst, HWND hParent) {
        // 需要用 DialogBox 创建的资源对话框
        // 由于资源文件不存在，这里用 CreateWindow 手动构建
        // 详见 createSettingsWindow 函数
        INT_PTR result = DialogBoxParamW(hInst, MAKEINTRESOURCEW(100), hParent, dlgProc, 0);
        return result == 1;
    }
};

inline PinyinSettings SettingsDialog::g_tempSettings;
inline HWND SettingsDialog::g_hSkinPreview = nullptr;
inline int SettingsDialog::g_selectedSkin = 0;

// ==================== 系统字体枚举 ====================
// 使用 EnumFontFamiliesExW 遍历系统已安装的 TrueType/OpenType 字体
// 返回去重并排序的字体名称列表，可直接用于 CreateFontW
inline std::vector<std::wstring> enumSystemFonts() {
    std::vector<std::wstring> fonts;

    HDC hdc = GetDC(nullptr);
    if (!hdc) return fonts;

    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfFaceName[0] = L'\0';
    lf.lfPitchAndFamily = 0;

    EnumFontFamiliesExW(hdc, &lf,
        [](const LOGFONTW* pLf, const TEXTMETRICW*, DWORD fontType, LPARAM lParam) -> int {
            auto* pFonts = reinterpret_cast<std::vector<std::wstring>*>(lParam);
            // 只收录 TrueType / OpenType 字体 (排除点阵字体和设备字体)
            if (fontType & TRUETYPE_FONTTYPE) {
                std::wstring name = pLf->lfFaceName;
                // 去重 (同一字体家族的不同样式只保留一个)
                if (std::find(pFonts->begin(), pFonts->end(), name) == pFonts->end()) {
                    pFonts->push_back(name);
                }
            }
            return 1; // 继续枚举
        },
        (LPARAM)&fonts, 0);

    ReleaseDC(nullptr, hdc);

    // 按 Unicode 排序 (拉丁字母在前, 中文在后)
    std::sort(fonts.begin(), fonts.end());
    return fonts;
}

// ==================== 无资源文件的设置窗口构建 ====================
// 因为不使用 .rc 资源文件，纯代码创建所有控件
struct SettingsWindow {
    HWND m_hDlg = nullptr;
    HWND m_hSkinPreview = nullptr;
    int m_selectedSkin = 0;
    PinyinSettings m_temp;
    HFONT m_hFont = nullptr; // 所有控件共用字体
    float m_dpiScale = 1.0f; // DPI 缩放因子 (96 DPI=1.0, 192 DPI=2.0)

    // 皮肤预览子窗口过程
    static LRESULT CALLBACK skinPreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        SettingsWindow* self = (SettingsWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (!self && msg == WM_CREATE) {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
            self = (SettingsWindow*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (msg == WM_PAINT && self) {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);

            HBRUSH hBg = CreateSolidBrush(self->m_temp.bgColor);
            FillRect(hdc, &rc, hBg);
            DeleteObject(hBg);

            HPEN hPen = CreatePen(PS_SOLID, 1, self->m_temp.borderColor);
            HPEN oldPen = (HPEN)SelectObject(hdc, hPen);
            Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(hdc, oldPen);
            DeleteObject(hPen);

            auto S = [self](int v) -> int { return (int)(v * self->m_dpiScale + 0.5f); };
            HFONT hFont = CreateFontW(-S(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, self->m_temp.fontName.c_str());
            SelectObject(hdc, hFont);
            SetBkMode(hdc, TRANSPARENT);

            int py = S(5);
            SetTextColor(hdc, self->m_temp.inputColor);
            TextOutW(hdc, py, py, L"[nihao] ", 8);
            SIZE sz; GetTextExtentPoint32W(hdc, L"[nihao] ", 8, &sz);
            int x = py + sz.cx;

            SetTextColor(hdc, self->m_temp.indexColor);
            TextOutW(hdc, x, py, L"1.", 2);
            GetTextExtentPoint32W(hdc, L"1.", 2, &sz); x += sz.cx;
            SetTextColor(hdc, self->m_temp.textColor);
            TextOutW(hdc, x, py, L"你好", 2);
            GetTextExtentPoint32W(hdc, L"你好", 2, &sz); x += sz.cx + S(8);

            SetTextColor(hdc, self->m_temp.indexColor);
            TextOutW(hdc, x, py, L"2.", 2);
            GetTextExtentPoint32W(hdc, L"2.", 2, &sz); x += sz.cx;
            SetTextColor(hdc, self->m_temp.textColor);
            TextOutW(hdc, x, py, L"泥嚎", 2);
            GetTextExtentPoint32W(hdc, L"泥嚎", 2, &sz); x += sz.cx + S(8);

            SetTextColor(hdc, self->m_temp.indexColor);
            TextOutW(hdc, x, py, L"3.", 2);
            GetTextExtentPoint32W(hdc, L"3.", 2, &sz); x += sz.cx;
            SetTextColor(hdc, self->m_temp.textColor);
            TextOutW(hdc, x, py, L"你号", 2);

            DeleteObject(hFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    // 创建控件辅助函数 (统一使用微软雅黑字体)
    HWND addLabel(const wchar_t* text, int x, int y, int w, int h) {
        HWND ctrl = CreateWindowExW(0, L"STATIC", text,
            WS_CHILD | WS_VISIBLE,
            x, y, w, h, m_hDlg, nullptr,
            (HINSTANCE)GetWindowLongPtrW(m_hDlg, GWLP_HINSTANCE), nullptr);
        if (ctrl && m_hFont) SendMessageW(ctrl, WM_SETFONT, (WPARAM)m_hFont, TRUE);
        return ctrl;
    }

    HWND addCheck(const wchar_t* text, int id, int x, int y, int w, int h, bool checked) {
        HWND ctrl = CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            x, y, w, h, m_hDlg, (HMENU)(UINT_PTR)id,
            (HINSTANCE)GetWindowLongPtrW(m_hDlg, GWLP_HINSTANCE), nullptr);
        if (ctrl) {
            if (m_hFont) SendMessageW(ctrl, WM_SETFONT, (WPARAM)m_hFont, TRUE);
            SendMessageW(ctrl, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        return ctrl;
    }

    HWND addButton(const wchar_t* text, int id, int x, int y, int w, int h) {
        HWND ctrl = CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            x, y, w, h, m_hDlg, (HMENU)(UINT_PTR)id,
            (HINSTANCE)GetWindowLongPtrW(m_hDlg, GWLP_HINSTANCE), nullptr);
        if (ctrl && m_hFont) SendMessageW(ctrl, WM_SETFONT, (WPARAM)m_hFont, TRUE);
        return ctrl;
    }

    HWND addEdit(int id, int x, int y, int w, int h, const wchar_t* text) {
        HWND ctrl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
            x, y, w, h, m_hDlg, (HMENU)(UINT_PTR)id,
            (HINSTANCE)GetWindowLongPtrW(m_hDlg, GWLP_HINSTANCE), nullptr);
        if (ctrl && m_hFont) SendMessageW(ctrl, WM_SETFONT, (WPARAM)m_hFont, TRUE);
        return ctrl;
    }

    HWND addCombo(int id, int x, int y, int w, int h) {
        HWND ctrl = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            x, y, w, h, m_hDlg, (HMENU)(UINT_PTR)id,
            (HINSTANCE)GetWindowLongPtrW(m_hDlg, GWLP_HINSTANCE), nullptr);
        if (ctrl && m_hFont) SendMessageW(ctrl, WM_SETFONT, (WPARAM)m_hFont, TRUE);
        return ctrl;
    }

    void buildUI() {
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(m_hDlg, GWLP_HINSTANCE);

        // DPI 缩放辅助: 将 96 DPI 基准像素值转换为当前屏幕物理像素
        auto S = [this](int v) -> int { return (int)(v * m_dpiScale + 0.5f); };

        // 创建字体 (所有控件共用, 字号也随 DPI 缩放)
        if (!m_hFont) {
            m_hFont = CreateFontW(-S(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, m_temp.fontName.c_str());
        }

        // === 标题 ===
        addLabel(L"PinyinIME 输入法设置", S(15), S(10), S(300), S(24));

        // === 基本设置组 ===
        int gy = S(40);
        addLabel(L"━━ 基本设置 ━━", S(15), gy, S(200), S(18));
        gy += S(22);

        addCheck(L"繁体中文模式", 401, S(20), gy, S(140), S(20), m_temp.useTraditional);
        addCheck(L"竖排候选框", 404, S(170), gy, S(120), S(20), m_temp.verticalLayout);
        addCheck(L"中文标点符号（，。）", 405, S(300), gy, S(180), S(20), m_temp.chinesePunctuation);
        gy += S(24);

        addLabel(L"候选词数量 (3-9):", S(20), gy, S(130), S(20));
        addEdit(402, S(155), gy - S(2), S(30), S(20), std::to_wstring(m_temp.candidateCount).c_str());
        addLabel(L"字体大小 (12-36):", S(200), gy, S(130), S(20));
        addEdit(403, S(335), gy - S(2), S(30), S(20), std::to_wstring(m_temp.fontSize).c_str());
        gy += S(28);

        // 字体选择
        addLabel(L"选择字体:", S(20), gy, S(80), S(20));
        HWND hFontCombo = addCombo(408, S(105), gy - S(2), S(280), S(300));
        {
            auto fonts = enumSystemFonts();
            for (const auto& f : fonts) {
                SendMessageW(hFontCombo, CB_ADDSTRING, 0, (LPARAM)f.c_str());
            }
            int fontIdx = (int)SendMessageW(hFontCombo, CB_FINDSTRINGEXACT, -1, (LPARAM)m_temp.fontName.c_str());
            if (fontIdx >= 0) SendMessageW(hFontCombo, CB_SETCURSEL, fontIdx, 0);
        }
        gy += S(28);

        // === 外观设置组 ===
        addLabel(L"━━ 外观配色 ━━", S(15), gy, S(200), S(18));
        gy += S(22);

        addLabel(L"预设皮肤:", S(20), gy, S(80), S(20));
        HWND hSkin = addCombo(701, S(100), gy - S(2), S(140), S(200));
        for (int i = 0; i < PinyinSettings::SKIN_COUNT; i++) {
            SendMessageW(hSkin, CB_ADDSTRING, 0, (LPARAM)PinyinSettings::skins[i].name);
        }
        SendMessageW(hSkin, CB_SETCURSEL, 0, 0);

        addButton(L"🎨 自定义主色调", 702, S(255), gy - S(2), S(140), S(22));
        gy += S(28);

        // 皮肤预览
        addLabel(L"候选框预览:", S(20), gy, S(80), S(20));

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = skinPreviewProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"PinyinSkinPreview";
        RegisterClassExW(&wc);

        m_hSkinPreview = CreateWindowExW(0, L"PinyinSkinPreview", L"",
            WS_CHILD | WS_VISIBLE,
            S(20), gy, S(460), S(35), m_hDlg, nullptr, hInst, this);
        gy += S(42);

        addButton(L"📝 管理用户词典...", 801, S(20), gy, S(170), S(26));
        gy += S(32);

        // === 模糊音设置组 ===
        addLabel(L"━━ 模糊音设置 ━━", S(15), gy, S(200), S(18));
        gy += S(22);

        addCheck(L"z/zh 不分", 501, S(20), gy, S(95), S(20), m_temp.fuzzyZ_Zh);
        addCheck(L"c/ch 不分", 502, S(125), gy, S(95), S(20), m_temp.fuzzyC_Ch);
        addCheck(L"s/sh 不分", 503, S(230), gy, S(95), S(20), m_temp.fuzzyS_Sh);
        addCheck(L"n/l 不分", 504, S(335), gy, S(90), S(20), m_temp.fuzzyN_L);
        gy += S(22);
        addCheck(L"f/h 不分", 505, S(20), gy, S(95), S(20), m_temp.fuzzyF_H);
        addCheck(L"en/eng 不分", 506, S(125), gy, S(110), S(20), m_temp.fuzzyEn_Eng);
        addCheck(L"in/ing 不分", 507, S(240), gy, S(110), S(20), m_temp.fuzzyIn_Ing);
        gy += S(28);

        // === 智能功能组 ===
        addLabel(L"━━ 智能功能 ━━", S(15), gy, S(200), S(18));
        gy += S(22);

        addCheck(L"智能拼音纠错", 601, S(20), gy, S(140), S(20), m_temp.smartCorrection);
        addCheck(L"自动加入新词到用户词典", 602, S(170), gy, S(190), S(20), m_temp.autoWordCreate);
        addCheck(L"词频自动调整", 603, S(370), gy, S(140), S(20), m_temp.autoFreqAdjust);
        gy += S(30);

        // === 系统设置组 ===
        addLabel(L"━━ 系统 ━━", S(15), gy, S(200), S(18));
        gy += S(22);

        addLabel(L"切换热键:", S(20), gy, S(80), S(20));
        HWND hHotkey = addCombo(406, S(100), gy - S(2), S(160), S(200));
        SendMessageW(hHotkey, CB_ADDSTRING, 0, (LPARAM)L"Ctrl + Shift");
        SendMessageW(hHotkey, CB_ADDSTRING, 0, (LPARAM)L"Alt + Shift");
        SendMessageW(hHotkey, CB_ADDSTRING, 0, (LPARAM)L"Right Shift (单独)");
        SendMessageW(hHotkey, CB_ADDSTRING, 0, (LPARAM)L"Ctrl + Space");
        // 根据当前设置选中正确的项
        {
            int hotkeySel = 1; // 默认: Ctrl+Shift
            if (m_temp.toggleModifier == VK_MENU && m_temp.toggleHotkey == VK_SHIFT)
                hotkeySel = 0;
            else if (m_temp.toggleModifier == 0 && m_temp.toggleHotkey == VK_RSHIFT)
                hotkeySel = 2;
            else if (m_temp.toggleModifier == VK_CONTROL && m_temp.toggleHotkey == VK_SPACE)
                hotkeySel = 3;
            SendMessageW(hHotkey, CB_SETCURSEL, hotkeySel, 0);
        }
        gy += S(28);

        addButton(L"📌 注册输入法到系统", 910, S(20), gy, S(180), S(26));
        addLabel(L"将 PinyinIME 注册为系统输入法并设置开机自启", S(210), gy + S(3), S(300), S(20));
        gy += S(32);

        // === 底部按钮 ===
        addButton(L"✅ 保存设置", 901, S(250), gy, S(100), S(28));
        addButton(L"❌ 取消", 902, S(360), gy, S(80), S(28));

        // 设置窗口总尺寸 (使用 AdjustWindowRect 计算包含标题栏的窗口大小)
        RECT clientRC = {0, 0, S(530), gy + S(52)};
        AdjustWindowRect(&clientRC, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
        int winW = clientRC.right - clientRC.left;
        int winH = clientRC.bottom - clientRC.top;
        SetWindowPos(m_hDlg, nullptr, 0, 0, winW, winH, SWP_NOMOVE | SWP_NOZORDER);
    }

    static LRESULT CALLBACK dlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        SettingsWindow* self = (SettingsWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

        switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
            self = (SettingsWindow*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
            self->m_hDlg = hwnd;
            self->buildUI();
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);

            if (!self) break;

            if (id == 701 && code == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(GetDlgItem(hwnd, 701), CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < PinyinSettings::SKIN_COUNT) {
                    self->m_selectedSkin = sel;
                    self->m_temp.applySkin(sel);
                    InvalidateRect(self->m_hSkinPreview, nullptr, TRUE);
                }
                return 0;
            }

            if (id == 408 && code == CBN_SELCHANGE) { // 字体选择
                HWND hFontCombo = GetDlgItem(hwnd, 408);
                int sel = (int)SendMessageW(hFontCombo, CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    wchar_t buf[64] = {};
                    SendMessageW(hFontCombo, CB_GETLBTEXT, sel, (LPARAM)buf);
                    self->m_temp.fontName = buf;
                    InvalidateRect(self->m_hSkinPreview, nullptr, TRUE);
                }
                return 0;
            }

            if (id == 702) { // 自定义颜色
                CHOOSECOLORW cc = {};
                static COLORREF acr[16];
                cc.lStructSize = sizeof(cc);
                cc.hwndOwner = hwnd;
                cc.rgbResult = self->m_temp.bgColor;
                cc.lpCustColors = acr;
                cc.Flags = CC_FULLOPEN | CC_RGBINIT;
                if (ChooseColorW(&cc)) {
                    COLORREF base = cc.rgbResult;
                    int r = GetRValue(base), g = GetGValue(base), b = GetBValue(base);
                    self->m_temp.bgColor = base;
                    self->m_temp.borderColor = RGB((r*2)/3, (g*2)/3, (b*2)/3);
                    int brightness = (r*299 + g*587 + b*114) / 1000;
                    if (brightness > 128) {
                        self->m_temp.textColor = RGB(std::max(0,r/4), std::max(0,g/4), std::max(0,b/4));
                        self->m_temp.indexColor = RGB(std::min(255,r/2+40), std::min(255,g/2+40), std::min(255,b/2+100));
                        self->m_temp.inputColor = RGB(0, std::min(255,g/3), std::min(255,b));
                    } else {
                        self->m_temp.textColor = RGB(220, 220, 220);
                        self->m_temp.indexColor = RGB(std::min(255,r+100), std::min(255,g+80), 255);
                        self->m_temp.inputColor = RGB(150, 200, 255);
                    }
                    self->m_selectedSkin = -1;
                    SendMessageW(GetDlgItem(hwnd, 701), CB_SETCURSEL, -1, 0);
                    InvalidateRect(self->m_hSkinPreview, nullptr, TRUE);
                }
                return 0;
            }

            if (id == 801) { // 用户词典管理
                UserDictDialog::show(
                    (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), hwnd);
                return 0;
            }

            if (id == 910) { // 注册输入法到系统
                registerIMEToSystem(false);
                return 0;
            }

            if (id == 901) { // 保存
                self->m_temp.useTraditional = (IsDlgButtonChecked(hwnd, 401) == BST_CHECKED);
                self->m_temp.verticalLayout = (IsDlgButtonChecked(hwnd, 404) == BST_CHECKED);
                self->m_temp.chinesePunctuation = (IsDlgButtonChecked(hwnd, 405) == BST_CHECKED);

                BOOL ok;
                int cnt = GetDlgItemInt(hwnd, 402, &ok, FALSE);
                if (ok && cnt >= 3 && cnt <= 9) self->m_temp.candidateCount = cnt;
                int fs = GetDlgItemInt(hwnd, 403, &ok, TRUE);
                if (ok && fs >= 12 && fs <= 36) self->m_temp.fontSize = fs;

                // 读取字体选择
                {
                    HWND hFontCombo = GetDlgItem(hwnd, 408);
                    int fontSel = (int)SendMessageW(hFontCombo, CB_GETCURSEL, 0, 0);
                    if (fontSel >= 0) {
                        wchar_t buf[64] = {};
                        SendMessageW(hFontCombo, CB_GETLBTEXT, fontSel, (LPARAM)buf);
                        if (buf[0]) self->m_temp.fontName = buf;
                    }
                }

                self->m_temp.fuzzyZ_Zh = (IsDlgButtonChecked(hwnd, 501) == BST_CHECKED);
                self->m_temp.fuzzyC_Ch = (IsDlgButtonChecked(hwnd, 502) == BST_CHECKED);
                self->m_temp.fuzzyS_Sh = (IsDlgButtonChecked(hwnd, 503) == BST_CHECKED);
                self->m_temp.fuzzyN_L = (IsDlgButtonChecked(hwnd, 504) == BST_CHECKED);
                self->m_temp.fuzzyF_H = (IsDlgButtonChecked(hwnd, 505) == BST_CHECKED);
                self->m_temp.fuzzyEn_Eng = (IsDlgButtonChecked(hwnd, 506) == BST_CHECKED);
                self->m_temp.fuzzyIn_Ing = (IsDlgButtonChecked(hwnd, 507) == BST_CHECKED);

                self->m_temp.smartCorrection = (IsDlgButtonChecked(hwnd, 601) == BST_CHECKED);
                self->m_temp.autoWordCreate = (IsDlgButtonChecked(hwnd, 602) == BST_CHECKED);
                self->m_temp.autoFreqAdjust = (IsDlgButtonChecked(hwnd, 603) == BST_CHECKED);

                // 读取切换热键
                {
                    int hotkeySel = (int)SendMessageW(GetDlgItem(hwnd, 406), CB_GETCURSEL, 0, 0);
                    switch (hotkeySel) {
                        case 0: // Ctrl+Shift
                            self->m_temp.toggleModifier = VK_CONTROL;
                            self->m_temp.toggleHotkey = VK_SHIFT;
                            break;
                        case 1: // Alt+Shift
                            self->m_temp.toggleModifier = VK_MENU;
                            self->m_temp.toggleHotkey = VK_SHIFT;
                            break;
                        case 2: // Right Shift (单独)
                            self->m_temp.toggleModifier = 0;
                            self->m_temp.toggleHotkey = VK_RSHIFT;
                            break;
                        case 3: // Ctrl+Space
                            self->m_temp.toggleModifier = VK_CONTROL;
                            self->m_temp.toggleHotkey = VK_SPACE;
                            break;
                    }
                }

                // 应用到全局设置
                g_settings = self->m_temp;
                g_settings.saveToFile(getExeDirectory() + "pinyin_config.ini");
                applySettingsToEngine(g_settings);

                DestroyWindow(hwnd);
                return 0;
            }

            if (id == 902) { // 取消
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_DESTROY:
            // 子控件还未销毁, 不在此时 delete self
            // 最终清理在 WM_NCDESTROY 中完成
            break;
        case WM_NCDESTROY:
            if (self) {
                if (self->m_hFont) { DeleteObject(self->m_hFont); self->m_hFont = nullptr; }
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            // 注意: 不在此处 delete self
            // show() 函数在消息循环结束后负责 delete sw
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static void show(HINSTANCE hInst, HWND hParent) {
        SettingsWindow* sw = new SettingsWindow();
        sw->m_temp = g_settings;

        // 获取系统 DPI 缩放因子 (用于适配 4K/1080p 等不同分辨率屏幕)
        {
            HDC hdc = GetDC(nullptr);
            int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(nullptr, hdc);
            sw->m_dpiScale = (float)dpi / 96.0f;
        }
        auto S = [sw](int v) -> int { return (int)(v * sw->m_dpiScale + 0.5f); };

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = dlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"PinyinIMESettings";
        RegisterClassExW(&wc);

        HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
            L"PinyinIMESettings", L"PinyinIME 设置",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, S(530), S(580),
            hParent, nullptr, hInst, sw);

        if (hDlg) {
            // 屏幕居中
            RECT rc; GetWindowRect(hDlg, &rc);
            int w = rc.right - rc.left, h = rc.bottom - rc.top;
            int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
            SetWindowPos(hDlg, nullptr, (sx - w) / 2, (sy - h) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            ShowWindow(hDlg, SW_SHOWNORMAL);
            UpdateWindow(hDlg);
        }

        // 消息循环 (模态)
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            if (!IsWindow(hDlg)) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        delete sw;
    }
};
