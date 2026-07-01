// exe/main.cpp — PinyinIME.exe 精简入口
// 只负责: 托盘图标, 设置窗口, 系统注册
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <gdiplus.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")
#include <string>
#include "../shared/pinyin_settings.h"
#include "../shared/utf_utils.h"
#include "settings_window.h"
#include "registration.h"

// ==================== 全局变量 ====================
static HINSTANCE g_hInst = nullptr;
static HWND g_hWnd = nullptr;
static NOTIFYICONDATAW g_trayIcon = {};
PinyinSettings g_settings;

// ==================== 自定义消息 ====================
#define WM_TRAYICON      (WM_USER + 2)
#define WM_OPEN_SETTINGS (WM_USER + 3)

// ==================== 主窗口过程 ====================
static LRESULT CALLBACK mainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAYICON:
        if (wp == 1 && lp == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, 1, L"⚙ 设置");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, 2, L"❌ 退出");
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
                pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);
            if (cmd == 1) PostMessage(hwnd, WM_OPEN_SETTINGS, 0, 0);
            else if (cmd == 2) PostQuitMessage(0);
        } else if (wp == 1 && lp == WM_LBUTTONDBLCLK) {
            PostMessage(hwnd, WM_OPEN_SETTINGS, 0, 0);
        }
        return 0;
    case WM_OPEN_SETTINGS:
        SettingsWindow::show(g_hInst, hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ==================== WinMain ====================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    // ── 命令行: --register-system / --unregister-system ──
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(lpCmdLine, &argc);
        if (argv) {
            for (int i = 0; i < argc; i++) {
                if (wcscmp(argv[i], L"--register-system") == 0) {
                    bool ok = doFullRegistration();
                    LocalFree(argv);
                    return ok ? 0 : 1;
                }
                if (wcscmp(argv[i], L"--unregister-system") == 0) {
                    bool ok = doFullUnregistration();
                    LocalFree(argv);
                    return ok ? 0 : 1;
                }
            }
            LocalFree(argv);
        }
    }

    g_hInst = hInstance;
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // GDI+
    Gdiplus::GdiplusStartupInput gdiSI;
    ULONG_PTR gdiToken;
    Gdiplus::GdiplusStartup(&gdiToken, &gdiSI, nullptr);

    // Common Controls
    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icex);

    // 加载设置
    std::string configPath = getModuleDirectory(nullptr) + "pinyin_config.ini";
    if (!g_settings.loadFromFile(configPath)) {
        g_settings.toggleModifier = VK_CONTROL;
        g_settings.toggleHotkey = VK_SHIFT;
    } else if (!g_settings.hasToggleModifierInFile) {
        g_settings.toggleModifier = VK_CONTROL;
        g_settings.toggleHotkey = VK_SHIFT;
        g_settings.saveToFile(configPath);
    }

    // 创建消息窗口
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = mainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"PinyinIMEMain";
    RegisterClassExW(&wc);
    g_hWnd = CreateWindowExW(0, L"PinyinIMEMain", L"PinyinIME",
        0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);

    // 托盘图标
    g_trayIcon.cbSize = sizeof(NOTIFYICONDATAW);
    g_trayIcon.hWnd = g_hWnd;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = WM_TRAYICON;
    g_trayIcon.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_trayIcon.szTip, L"PinyinIME 拼音输入法");
    Shell_NotifyIconW(NIM_ADD, &g_trayIcon);

    // 消息循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 清理
    Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
    Gdiplus::GdiplusShutdown(gdiToken);
    CoUninitialize();
    return 0;
}
