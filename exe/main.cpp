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
#include "../shared/ime_ipc.h"
#include "settings_window.h"
#include "registration.h"

// ==================== 全局变量 ====================
static HINSTANCE g_hInst = nullptr;
static HWND g_hWnd = nullptr;
static NOTIFYICONDATAW g_trayIcon = {};
static UINT g_uMsgOpenSettings = 0;  // registered message for cross-process settings open
static UINT g_uTaskbarCreated = 0;   // explorer 重启后重建托盘图标
PinyinSettings g_settings;

// ==================== 自定义消息 ====================
#define WM_TRAYICON      (WM_USER + 2)
#define WM_OPEN_SETTINGS (WM_USER + 3)
#define WM_TRAY_RESTORE  (WM_USER + 4)  // 定时重试恢复托盘图标

// 创建/恢复托盘图标
static void ensureTrayIcon() {
    if (!g_trayIcon.hWnd) return;
    // 先尝试删除旧的 (忽略失败)
    Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
    // 重新添加
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &g_trayIcon);
    if (!ok) {
        // 首次添加失败, 1 秒后重试
        SetTimer(g_hWnd, 2, 1000, nullptr);
    }
}

// ==================== 主窗口过程 ====================
static LRESULT CALLBACK mainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // 跨进程打开设置 (来自 DLL 齿轮按钮 或 --open-settings 转发)
    if (g_uMsgOpenSettings && msg == g_uMsgOpenSettings) {
        SettingsWindow::show(g_hInst, hwnd);
        return 0;
    }
    // explorer 重启后重建托盘图标
    if (g_uTaskbarCreated && msg == g_uTaskbarCreated) {
        ensureTrayIcon();
        return 0;
    }
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
    case WM_TIMER:
        if (wp == 2) {  // 托盘图标重试
            if (Shell_NotifyIconW(NIM_ADD, &g_trayIcon)) {
                KillTimer(hwnd, 2);
            }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ==================== WinMain ====================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    // ── 命令行: --register-system / --unregister-system / --force-kill ──
    // 这些是管理命令, 不进入正常的设置/托盘流程
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(lpCmdLine, &argc);
        if (argv) {
            for (int i = 0; i < argc; i++) {
                if (wcscmp(argv[i], L"--register-system") == 0 ||
                    wcscmp(argv[i], L"--register-with-ui") == 0) {
                    // 注册进度窗口 (--register-system 和 --register-with-ui 都使用)
                    LocalFree(argv);
                    SetProcessDPIAware();
                    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                    {
                        Gdiplus::GdiplusStartupInput gdiSI3;
                        ULONG_PTR gdiToken3;
                        Gdiplus::GdiplusStartup(&gdiToken3, &gdiSI3, nullptr);
                        INITCOMMONCONTROLSEX icex3 = { sizeof(icex3), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES };
                        InitCommonControlsEx(&icex3);
                        std::string configPath3 = getModuleDirectory(nullptr) + "pinyin_config.ini";
                        g_settings.loadFromFile(configPath3);
                        RegisterWindow::show(hInstance, nullptr, g_settings);
                        Gdiplus::GdiplusShutdown(gdiToken3);
                    }
                    CoUninitialize();
                    return 0;
                }
                if (wcscmp(argv[i], L"--unregister-system") == 0 ||
                    wcscmp(argv[i], L"--uninstall-with-ui") == 0) {
                    // 卸载进度窗口 (--unregister-system 和 --uninstall-with-ui 都使用)
                    LocalFree(argv);
                    SetProcessDPIAware();
                    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                    {
                        Gdiplus::GdiplusStartupInput gdiSI2;
                        ULONG_PTR gdiToken2;
                        Gdiplus::GdiplusStartup(&gdiToken2, &gdiSI2, nullptr);
                        INITCOMMONCONTROLSEX icex2 = { sizeof(icex2), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES };
                        InitCommonControlsEx(&icex2);
                        std::string configPath2 = getModuleDirectory(nullptr) + "pinyin_config.ini";
                        g_settings.loadFromFile(configPath2);
                        UninstallWindow::show(hInstance, nullptr, g_settings);
                        Gdiplus::GdiplusShutdown(gdiToken2);
                    }
                    CoUninitialize();
                    return 0;
                }
                if (wcscmp(argv[i], L"--force-kill") == 0 && i + 1 < argc) {
                    // UAC 提权后: 强制终止占用 DLL 的进程 (参数为逗号分隔的 PID 列表)
                    forceKillByPidString(argv[i + 1]);
                    LocalFree(argv);
                    return 0;
                }
            }
            LocalFree(argv);
        }
    }

    // ── 单实例检查 ──
    // 双击 EXE 默认行为: 打开设置窗口
    // 如果已有实例在运行, 转发请求到已有实例后退出
    HANDLE hSingleMutex = CreateMutexW(nullptr, FALSE, PinyinIME_SINGLE_INSTANCE_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        UINT msgOS = RegisterWindowMessageW(PinyinIME_MSG_OPEN_SETTINGS);
        HWND hExisting = FindWindowW(PinyinIME_MAIN_WINDOW_CLASS, L"PinyinIME");
        if (hExisting && msgOS) {
            PostMessageW(hExisting, msgOS, 0, 0);
        }
        return 0;
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
    g_settings.loadFromFile(configPath);  // 首次运行使用默认值, 忽略返回值

    // 注册跨进程消息
    g_uMsgOpenSettings = RegisterWindowMessageW(PinyinIME_MSG_OPEN_SETTINGS);
    g_uTaskbarCreated  = RegisterWindowMessageW(L"TaskbarCreated");

    // 创建消息窗口 (改为 WS_POPUP 而非 HWND_MESSAGE, 以便 FindWindowW 跨进程查找)
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = mainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = PinyinIME_MAIN_WINDOW_CLASS;
    RegisterClassExW(&wc);
    g_hWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        PinyinIME_MAIN_WINDOW_CLASS, L"PinyinIME",
        WS_POPUP,
        -32000, -32000, 0, 0,
        nullptr, nullptr, hInstance, nullptr);

    // 托盘图标
    g_trayIcon.cbSize = sizeof(NOTIFYICONDATAW);
    g_trayIcon.hWnd = g_hWnd;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = WM_TRAYICON;
    g_trayIcon.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_trayIcon.szTip, L"PinyinIME 拼音输入法");
    ensureTrayIcon();

    // 启动默认打开设置窗口
    PostMessageW(g_hWnd, g_uMsgOpenSettings, 0, 0);

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
