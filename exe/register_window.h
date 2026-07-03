// exe/register_window.h — 注册进度窗口
// 逐步异步展示注册过程, 与卸载窗口风格一致
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include "../shared/pinyin_settings.h"
#include "registration.h"

// ==================== 自定义消息 ====================
#define WM_APP_REG_STEP_COMPLETE (WM_APP + 20)
#define WM_APP_REG_START_STEP    (WM_APP + 21)

// ==================== 控件 ID ====================
#define IDC_REG_CLOSE     998
#define IDC_REG_ELEVATE   2001
#define IDC_REG_SETTINGS  2002

// Windows 11 检测 (build >= 22000)
// 使用 RtlGetVersion 直接从内核获取版本号, 不依赖注册表
inline bool IsWindows11OrLater() {
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) return false;

    typedef LONG (WINAPI* FnRtlGetVersion)(OSVERSIONINFOW*);
    auto pfn = (FnRtlGetVersion)GetProcAddress(hNtdll, "RtlGetVersion");
    if (!pfn) return false;

    OSVERSIONINFOW osvi = {};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    if (pfn(&osvi) == 0) {
        return osvi.dwBuildNumber >= 22000;
    }
    return false;
}

// ==================== 步骤状态 ====================
enum class RegStepState : int {
    PENDING = 0, RUNNING = 1, SUCCESS = 2, FAILURE = 3, WAITING = 4,
};

// ==================== 步骤类型 ====================
enum class RegStepType : int {
    CheckAdmin   = 0,
    ComRegister  = 1,
    TsfRegister  = 2,
    StartupReg   = 3,
    Count        = 4,
};

// ==================== 每个步骤的信息 ====================
struct RegStepInfo {
    std::wstring label;
    RegStepState state = RegStepState::PENDING;
    std::wstring detail;
    RECT rowRect = {};
};

// ==================== 工作线程结果 ====================
struct RegStepResult {
    bool        success = false;
    std::wstring detail;
};

// ==================== RegisterWindow ====================
struct RegisterWindow {
    HWND  m_hDlg      = nullptr;
    float m_dpiScale  = 1.0f;
    int   m_roundR    = 10;
    RECT  m_closeBtnRect = {};

    COLORREF m_bgColor     = RGB(0xF0, 0xF5, 0xF0);
    COLORREF m_borderColor = RGB(0x96, 0xC6, 0x96);
    COLORREF m_textColor   = RGB(0x28, 0x3D, 0x28);

    HFONT  m_hFont    = nullptr;
    HBRUSH m_hBgBrush = nullptr;

    std::vector<RegStepInfo> m_steps;
    int     m_currentStepIdx = -1;
    bool    m_isWorking      = false;
    HANDLE  m_workerThread   = nullptr;
    RegStepResult m_pendingResult;

    std::wstring m_dllPath;
    HWND m_hCloseBtn   = nullptr;
    HWND m_hElevateBtn = nullptr;

    // 完成后的提示区域
    bool m_showTips     = false;
    RECT m_tipsRect     = {};
    bool m_isWin11      = false;
    HWND m_hSettingsBtn = nullptr;

    int S(int v) const { return (int)(v * m_dpiScale + 0.5f); }

    // ==================== 窗口过程 ====================
    static LRESULT CALLBACK dlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        RegisterWindow* self = (RegisterWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        switch (msg) {
        case WM_CREATE: {
            self = (RegisterWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
            self->m_hDlg = hwnd;
            self->initUI();
            PostMessageW(hwnd, WM_APP_REG_START_STEP, 0, 0);
            return 0;
        }
        case WM_ERASEBKGND:
            return TRUE;
        case WM_NCHITTEST: {
            if (!self) break;
            POINT pt = { LOWORD(lp), HIWORD(lp) };
            ScreenToClient(hwnd, &pt);
            if (PtInRect(&self->m_closeBtnRect, pt)) return HTCLIENT;
            RECT titleRc = { 0, 0, self->S(530), self->S(32) };
            if (PtInRect(&titleRc, pt)) return HTCAPTION;
            break;
        }
        case WM_LBUTTONDOWN: {
            if (!self) break;
            POINT pt = { LOWORD(lp), HIWORD(lp) };
            if (PtInRect(&self->m_closeBtnRect, pt)) { DestroyWindow(hwnd); return 0; }
            break;
        }
        case WM_PAINT: {
            if (!self) break;
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);

            // 4 层渐变背景
            {
                Gdiplus::Graphics graphics(hdc);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                int w = rc.right, h = rc.bottom, cr = self->m_roundR;
                auto makeRR = [](Gdiplus::GraphicsPath& p, int x, int y, int rw, int rh, int rad) {
                    p.Reset(); p.StartFigure();
                    int dia = rad * 2;
                    p.AddArc(x, y, dia, dia, 180, 90);
                    p.AddArc(x + rw - dia, y, dia, dia, 270, 90);
                    p.AddArc(x + rw - dia, y + rh - dia, dia, dia, 0, 90);
                    p.AddArc(x, y + rh - dia, dia, dia, 90, 90);
                    p.CloseFigure();
                };
                COLORREF bc = self->m_borderColor, bgc = self->m_bgColor;
                int rr2 = GetRValue(bc), rg2 = GetGValue(bc), rb2 = GetBValue(bc);
                int bgBright = (GetRValue(bgc) * 299 + GetGValue(bgc) * 587 + GetBValue(bgc) * 114) / 1000;
                int dir2 = (bgBright < 128) ? 1 : -1;
                auto clampC = [](int v) -> int { return v < 0 ? 0 : (v > 255 ? 255 : v); };
                for (int layer = 0; layer < 4; layer++) {
                    int off = layer, lw = w - off * 2, lh = h - off * 2, lcr = cr - off; if (lcr < 2) lcr = 2;
                    int delta = (layer == 0) ? 40 : (layer == 1) ? 18 : 0;
                    COLORREF col = (layer < 3) ? RGB(clampC(rr2 + delta * dir2), clampC(rg2 + delta * dir2), clampC(rb2 + delta * dir2)) : bgc;
                    Gdiplus::SolidBrush br(Gdiplus::Color(255, GetRValue(col), GetGValue(col), GetBValue(col)));
                    Gdiplus::GraphicsPath pth;
                    makeRR(pth, off, off, lw, lh, lcr);
                    graphics.FillPath(&br, &pth);
                }
            }

            // 标题栏
            {
                SelectObject(hdc, self->m_hFont);
                SetBkMode(hdc, TRANSPARENT);
                int titleH = self->S(32);
                SetTextColor(hdc, self->m_textColor);
                RECT titleRc = { self->S(15), 0, rc.right - self->S(40), titleH };
                DrawTextW(hdc, L"PinyinIME 注册进度", -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                int btnSize = self->S(22), btnX = rc.right - self->S(30), btnY = (titleH - btnSize) / 2;
                self->m_closeBtnRect = { btnX, btnY, btnX + btnSize, btnY + btnSize };
                {
                    Gdiplus::Graphics g(hdc);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                    RECT br2 = self->m_closeBtnRect; int r2 = 3;
                    COLORREF bg = self->m_bgColor;
                    int rr = GetRValue(bg), gg = GetGValue(bg), bb = GetBValue(bg);
                    int brBright = (rr * 299 + gg * 587 + bb * 114) / 1000;
                    int delta = brBright > 128 ? -18 : 18;
                    auto clmp = [](int v) -> int { return v < 0 ? 0 : (v > 255 ? 255 : v); };
                    Gdiplus::SolidBrush fb(Gdiplus::Color(255, clmp(rr + delta), clmp(gg + delta), clmp(bb + delta)));
                    Gdiplus::GraphicsPath cp; cp.StartFigure();
                    int dia = r2 * 2;
                    cp.AddArc(br2.left, br2.top, dia, dia, 180, 90);
                    cp.AddArc(br2.right - dia, br2.top, dia, dia, 270, 90);
                    cp.AddArc(br2.right - dia, br2.bottom - dia, dia, dia, 0, 90);
                    cp.AddArc(br2.left, br2.bottom - dia, dia, dia, 90, 90);
                    cp.CloseFigure();
                    g.FillPath(&fb, &cp);
                }
                SetTextColor(hdc, self->m_textColor);
                RECT cr2 = self->m_closeBtnRect;
                DrawTextW(hdc, L"✕", -1, &cr2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                {
                    Gdiplus::Graphics g(hdc);
                    Gdiplus::Color sepColor(60, GetRValue(self->m_borderColor), GetGValue(self->m_borderColor), GetBValue(self->m_borderColor));
                    Gdiplus::Pen sepPen(sepColor, 1.0f);
                    g.DrawLine(&sepPen, self->S(10), titleH, self->S(520), titleH);
                }
            }

            // 步骤列表
            {
                SelectObject(hdc, self->m_hFont);
                SetBkMode(hdc, TRANSPARENT);
                for (size_t si = 0; si < self->m_steps.size(); si++) {
                    auto& step = self->m_steps[si];
                    RECT& sr = step.rowRect;

                    const wchar_t* icon = L"○";
                    COLORREF iconColor = RGB(0x88, 0x88, 0x88);
                    switch (step.state) {
                    case RegStepState::PENDING: icon = L"○"; iconColor = RGB(0x88, 0x88, 0x88); break;
                    case RegStepState::RUNNING: icon = L"●"; iconColor = RGB(0x33, 0x99, 0xFF); break;
                    case RegStepState::SUCCESS: icon = L"✓"; iconColor = RGB(0x22, 0xAA, 0x22); break;
                    case RegStepState::FAILURE: icon = L"✗"; iconColor = RGB(0xDD, 0x33, 0x33); break;
                    case RegStepState::WAITING: icon = L"▶"; iconColor = RGB(0xCC, 0x88, 0x00); break;
                    }

                    RECT iconRc = sr; iconRc.right = iconRc.left + self->S(22);
                    SetTextColor(hdc, iconColor);
                    DrawTextW(hdc, icon, -1, &iconRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                    RECT labelRc = sr; labelRc.left += self->S(26);
                    SetTextColor(hdc, self->m_textColor);
                    DrawTextW(hdc, step.label.c_str(), -1, &labelRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                    if (!step.detail.empty()) {
                        RECT detailRc = sr;
                        detailRc.left += self->S(26) + self->S(170);
                        SetTextColor(hdc, RGB(0x88, 0x88, 0x88));
                        DrawTextW(hdc, step.detail.c_str(), -1, &detailRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    }
                }
            }

            // ── 完成提示文本 ──
            if (self->m_showTips && !IsRectEmpty(&self->m_tipsRect)) {
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, self->m_textColor);
                std::wstring tips = L"💡 注册成功后:\n"
                    L"• 打开 设置 → 时间和语言 → 语言和区域 → 语言和区域\n"
                    L"• 语言 → 简体中文(中国大陆) 右边 ... → 语言选项\n"
                    L"• 键盘 → 增加键盘 → 在列表中找到 \"PinyinIME\" 并添加\n"
                    L"• 使用 Win+Space 切换输入法\n"
                    L"• 托盘图标由 PinyinIME.exe 提供 (已开机自启)\n";
                if (self->m_isWin11) {
                    tips += L"• 点击下方按钮可直接跳转到语言设置页面";
                }
                DrawTextW(hdc, tips.c_str(), -1, &self->m_tipsRect,
                    DT_LEFT | DT_TOP | DT_WORDBREAK);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE: {
            if (!self) break;
            RECT wrc; GetWindowRect(hwnd, &wrc);
            int ww = wrc.right - wrc.left, wh = wrc.bottom - wrc.top;
            HRGN rgn = CreateRoundRectRgn(0, 0, ww + 1, wh + 1, self->m_roundR * 2, self->m_roundR * 2);
            SetWindowRgn(hwnd, rgn, TRUE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            if (!self) break;
            HDC hdc = (HDC)wp;
            SetTextColor(hdc, self->m_textColor);
            SetBkColor(hdc, self->m_bgColor);
            return (LRESULT)self->m_hBgBrush;
        }
        case WM_CTLCOLORBTN: {
            if (!self) break;
            HDC hdc = (HDC)wp;
            SetTextColor(hdc, self->m_textColor);
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)self->m_hBgBrush;
        }
        case WM_COMMAND: {
            if (!self) break;
            int id = LOWORD(wp);
            if (id == IDC_REG_CLOSE) { DestroyWindow(hwnd); return 0; }
            if (id == IDC_REG_ELEVATE) { self->handleElevate(); return 0; }
            if (id == IDC_REG_SETTINGS) { self->handleOpenSettings(); return 0; }
            break;
        }
        case WM_TIMER: {
            if (!self) break;
            if (wp == 2) { KillTimer(hwnd, 2); DestroyWindow(hwnd); return 0; }
            break;
        }
        case WM_APP_REG_START_STEP: {
            if (!self) break;
            self->startStep((RegStepType)(int)wp);
            return 0;
        }
        case WM_APP_REG_STEP_COMPLETE: {
            if (!self) break;
            self->onStepComplete((int)wp, lp != 0);
            return 0;
        }
        case WM_DESTROY: {
            if (!self) break;
            if (self->m_workerThread) {
                WaitForSingleObject(self->m_workerThread, 1000);
                CloseHandle(self->m_workerThread);
                self->m_workerThread = nullptr;
            }
            if (self->m_hSettingsBtn) { DestroyWindow(self->m_hSettingsBtn); self->m_hSettingsBtn = nullptr; }
            if (self->m_hFont) DeleteObject(self->m_hFont);
            if (self->m_hBgBrush) DeleteObject(self->m_hBgBrush);
            return 0;
        }
        case WM_NCDESTROY: {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    // ==================== UI 初始化 ====================
    void initUI() {
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(m_hDlg, GWLP_HINSTANCE);

        m_hFont = CreateFontW(-S(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
        m_hBgBrush = CreateSolidBrush(m_bgColor);

        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        m_dllPath = exePath;
        size_t pos = m_dllPath.find_last_of(L"\\/");
        if (pos != std::wstring::npos) m_dllPath = m_dllPath.substr(0, pos + 1);
        m_dllPath += L"PinyinIMETSF.dll";

        m_steps.resize((int)RegStepType::Count);
        m_steps[(int)RegStepType::CheckAdmin].label  = L"管理员权限检查";
        m_steps[(int)RegStepType::ComRegister].label  = L"COM 组件注册";
        m_steps[(int)RegStepType::TsfRegister].label  = L"TSF 框架注册";
        m_steps[(int)RegStepType::StartupReg].label   = L"开机自启动注册";

        m_hCloseBtn = CreateWindowExW(0, L"BUTTON", L"关闭",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, S(80), S(28),
            m_hDlg, (HMENU)(UINT_PTR)IDC_REG_CLOSE, hInst, nullptr);
        if (m_hFont) SendMessageW(m_hCloseBtn, WM_SETFONT, (WPARAM)m_hFont, TRUE);

        {
            HDC hdc = GetDC(m_hDlg);
            HFONT hOld = (HFONT)SelectObject(hdc, m_hFont);
            TEXTMETRICW tm; GetTextMetricsW(hdc, &tm);
            SelectObject(hdc, hOld);
            ReleaseDC(m_hDlg, hdc);
            m_roundR = (std::max)(6, (std::min)(12, (int)(tm.tmHeight * 2 / 3)));
        }

        m_isWin11 = IsWindows11OrLater();

        recalcLayout();
    }

    // ==================== 布局 ====================
    void recalcLayout() {
        int y = S(32) + S(12);
        int stepH = S(28);
        int gap = S(4);

        for (auto& step : m_steps) {
            step.rowRect = { S(15), y, S(510), y + stepH };
            y += stepH + gap;
        }

        y += S(10);

        // 完成后显示提示区域
        if (m_showTips) {
            // 使用 DT_CALCRECT 动态测算文本所需高度 (适配不同 DPI)
            HDC hdc = GetDC(m_hDlg);
            HFONT hOld = (HFONT)SelectObject(hdc, m_hFont);

            std::wstring tips = L"💡 注册成功后:\n"
                L"• 打开 设置 → 时间和语言 → 语言和区域 → 语言和区域\n"
                L"• 语言 → 简体中文(中国大陆) 右边 ... → 语言选项\n"
                L"• 键盘 → 增加键盘 → 在列表中找到 \"PinyinIME\" 并添加\n"
                L"• 使用 Win+Space 切换输入法\n"
                L"• 托盘图标由 PinyinIME.exe 提供 (已开机自启)\n";
            if (m_isWin11) {
                tips += L"• 点击下方按钮可直接跳转到语言设置页面";
            }

            RECT measureRect = { 0, 0, S(510) - S(15), 0 };
            DrawTextW(hdc, tips.c_str(), -1, &measureRect,
                DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
            int tipsH = measureRect.bottom - measureRect.top + S(4);

            SelectObject(hdc, hOld);
            ReleaseDC(m_hDlg, hdc);

            m_tipsRect = { S(15), y, S(510), y + tipsH };
            y += tipsH + S(8);

            // Win11: 添加"打开设置"按钮
            if (m_isWin11) {
                if (m_hSettingsBtn) {
                    int sbtnW = S(220), sbtnH = S(28);
                    SetWindowPos(m_hSettingsBtn, nullptr,
                        (S(530) - sbtnW) / 2, y, sbtnW, sbtnH, SWP_NOZORDER);
                }
                y += S(36);
            }
        }

        int btnW = S(80), btnH = S(28);
        SetWindowPos(m_hCloseBtn, nullptr, (S(530) - btnW) / 2, y, btnW, btnH, SWP_NOZORDER);

        y += btnH + S(15);
        SetWindowPos(m_hDlg, nullptr, 0, 0, S(530), y, SWP_NOMOVE | SWP_NOZORDER);
    }

    // ==================== 状态机 ====================
    void startStep(RegStepType type) {
        if (m_isWorking) return;
        m_currentStepIdx = (int)type;
        m_isWorking = true;

        auto& step = m_steps[(int)type];
        step.state = RegStepState::RUNNING;
        InvalidateRect(m_hDlg, nullptr, TRUE);

        if (type == RegStepType::CheckAdmin) {
            runCheckAdmin();
            return;
        }

        m_workerThread = CreateThread(nullptr, 0, regWorkerThread, this, 0, nullptr);
        if (!m_workerThread) {
            step.state = RegStepState::FAILURE;
            step.detail = L"无法创建工作线程";
            m_isWorking = false;
            InvalidateRect(m_hDlg, nullptr, TRUE);
            startNextStep();
        }
    }

    void onStepComplete(int stepIdx, bool success) {
        m_isWorking = false;
        if (m_workerThread) {
            CloseHandle(m_workerThread);
            m_workerThread = nullptr;
        }

        auto& step = m_steps[stepIdx];
        step.detail = m_pendingResult.detail;
        step.state = success ? RegStepState::SUCCESS : RegStepState::FAILURE;
        recalcLayout();
        InvalidateRect(m_hDlg, nullptr, TRUE);
        enableCloseButton(true);
        startNextStep();
    }

    void startNextStep() {
        int next = m_currentStepIdx + 1;
        if (next >= (int)RegStepType::Count) {
            // 全部完成, 显示提示
            m_showTips = true;
            if (m_isWin11) {
                createSettingsButton();
            }
            recalcLayout();
            InvalidateRect(m_hDlg, nullptr, TRUE);
            return;
        }
        PostMessageW(m_hDlg, WM_APP_REG_START_STEP, (WPARAM)next, 0);
    }

    void enableCloseButton(bool enable) {
        if (m_hCloseBtn) EnableWindow(m_hCloseBtn, enable ? TRUE : FALSE);
    }

    // ==================== 步骤 0: 权限检查 ====================
    void runCheckAdmin() {
        auto& step = m_steps[(int)RegStepType::CheckAdmin];

        if (IsRunningAsAdmin()) {
            step.state = RegStepState::SUCCESS;
            step.detail = L"已具有管理员权限";
            m_isWorking = false;
            InvalidateRect(m_hDlg, nullptr, TRUE);
            startNextStep();
        } else {
            step.state = RegStepState::WAITING;
            step.detail = L"需要管理员权限才能注册 TSF 输入法";
            m_isWorking = false;
            InvalidateRect(m_hDlg, nullptr, TRUE);
            createElevateButton();
        }
    }

    void createElevateButton() {
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(m_hDlg, GWLP_HINSTANCE);
        m_hElevateBtn = CreateWindowExW(0, L"BUTTON", L"以管理员身份运行",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            S(360), S(34), S(145), S(22),
            m_hDlg, (HMENU)(UINT_PTR)IDC_REG_ELEVATE, hInst, nullptr);
        if (m_hFont) SendMessageW(m_hElevateBtn, WM_SETFONT, (WPARAM)m_hFont, TRUE);
    }

    void handleElevate() {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"runas";
        sei.lpFile = exePath;
        sei.lpParameters = L"--register-with-ui";
        sei.nShow = SW_SHOWNORMAL;

        if (ShellExecuteExW(&sei)) {
            auto& step = m_steps[(int)RegStepType::CheckAdmin];
            step.detail = L"已在管理员权限下打开新窗口，请在新窗口中完成注册。";
            step.state = RegStepState::SUCCESS;
            if (m_hElevateBtn) { DestroyWindow(m_hElevateBtn); m_hElevateBtn = nullptr; }
            InvalidateRect(m_hDlg, nullptr, TRUE);
            SetTimer(m_hDlg, 2, 2000, nullptr);
        } else {
            auto& step = m_steps[(int)RegStepType::CheckAdmin];
            step.detail = L"提权失败，请右键以管理员身份运行 PinyinIME.exe 后重试。";
            step.state = RegStepState::FAILURE;
            if (m_hElevateBtn) { DestroyWindow(m_hElevateBtn); m_hElevateBtn = nullptr; }
            InvalidateRect(m_hDlg, nullptr, TRUE);
            enableCloseButton(true);
        }
    }

    void createSettingsButton() {
        if (m_hSettingsBtn) return;  // 已创建
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(m_hDlg, GWLP_HINSTANCE);
        m_hSettingsBtn = CreateWindowExW(0, L"BUTTON", L"📂 打开设置 → 语言和区域",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, S(220), S(28),
            m_hDlg, (HMENU)(UINT_PTR)IDC_REG_SETTINGS, hInst, nullptr);
        if (m_hFont) SendMessageW(m_hSettingsBtn, WM_SETFONT, (WPARAM)m_hFont, TRUE);
    }

    void handleOpenSettings() {
        // 打开 Windows 11 设置 → 时间和语言 → 语言和区域
        ShellExecuteW(m_hDlg, L"open",
            L"ms-settings:regionlanguage",
            nullptr, nullptr, SW_SHOWNORMAL);
    }

    // ==================== 工作线程 ====================
    static DWORD WINAPI regWorkerThread(LPVOID param) {
        RegisterWindow* self = (RegisterWindow*)param;
        int stepIdx = self->m_currentStepIdx;

        // TSF 注册步骤需要 COM
        bool needCom = (stepIdx == (int)RegStepType::TsfRegister);
        if (needCom) CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        RegStepResult result;
        switch ((RegStepType)stepIdx) {
        case RegStepType::ComRegister:
            result = executeComRegister(self->m_dllPath);
            break;
        case RegStepType::TsfRegister:
            result = executeTsfRegister(self->m_dllPath);
            break;
        case RegStepType::StartupReg:
            result = executeStartupRegister();
            break;
        default:
            break;
        }

        if (needCom) CoUninitialize();

        self->m_pendingResult = result;
        PostMessageW(self->m_hDlg, WM_APP_REG_STEP_COMPLETE,
            (WPARAM)stepIdx, (LPARAM)(result.success ? 1 : 0));
        return 0;
    }

    // ==================== 步骤 1: COM 组件注册 ====================
    static RegStepResult executeComRegister(const std::wstring& dllPath) {
        RegStepResult r;

        if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            r.detail = L"找不到 PinyinIMETSF.dll";
            r.success = false;
            return r;
        }

        HMODULE hDll = LoadLibraryW(dllPath.c_str());
        if (hDll) {
            typedef HRESULT(STDAPICALLTYPE* DllRegisterServerFn)();
            auto pfn = (DllRegisterServerFn)GetProcAddress(hDll, "DllRegisterServer");
            if (pfn) {
                HRESULT hr = pfn();
                if (SUCCEEDED(hr)) {
                    r.detail = L"COM 组件已注册 (DllRegisterServer)";
                    r.success = true;
                } else {
                    wchar_t buf[16]; swprintf(buf, 16, L"%08X", hr);
                    r.detail = L"COM 注册失败 (0x" + std::wstring(buf) + L")";
                    r.success = false;
                }
            } else {
                r.detail = L"DLL 缺少 DllRegisterServer 导出";
                r.success = false;
            }
            FreeLibrary(hDll);
        } else {
            wchar_t buf[16]; swprintf(buf, 16, L"%08X", GetLastError());
            r.detail = L"无法加载 DLL (0x" + std::wstring(buf) + L")";
            r.success = false;
        }
        return r;
    }

    // ==================== 步骤 2: TSF 框架注册 ====================
    static RegStepResult executeTsfRegister(const std::wstring& dllPath) {
        RegStepResult r;
        HRESULT hr = RegisterTSFProfile(dllPath.c_str());
        if (SUCCEEDED(hr)) {
            r.detail = L"TSF 输入法框架 已注册";
            r.success = true;
        } else {
            wchar_t buf[16]; swprintf(buf, 16, L"%08X", hr);
            r.detail = L"TSF 框架注册失败 (0x" + std::wstring(buf) + L")";
            r.success = false;
        }
        return r;
    }

    // ==================== 步骤 3: 开机自启动注册 ====================
    static RegStepResult executeStartupRegister() {
        RegStepResult r;
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        HKEY hKey = nullptr;
        LONG res = RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
        if (res == ERROR_SUCCESS) {
            DWORD len = (DWORD)((wcslen(exePath) + 1) * sizeof(wchar_t));
            res = RegSetValueExW(hKey, L"PinyinIME", 0, REG_SZ, (const BYTE*)exePath, len);
            RegCloseKey(hKey);
            if (res == ERROR_SUCCESS) {
                r.detail = L"开机自启动 已注册";
                r.success = true;
            } else {
                wchar_t buf[16]; swprintf(buf, 16, L"%08X", res);
                r.detail = L"开机自启动 注册失败 (0x" + std::wstring(buf) + L")";
                r.success = false;
            }
        } else {
            wchar_t buf[16]; swprintf(buf, 16, L"%08X", res);
            r.detail = L"开机自启动 无法打开注册表项 (0x" + std::wstring(buf) + L")";
            r.success = false;
        }
        return r;
    }

    // ==================== show() 静态入口 ====================
    static void show(HINSTANCE hInst, HWND hParent, const PinyinSettings& settings) {
        RegisterWindow* rw = new RegisterWindow();
        rw->m_bgColor     = settings.bgColor;
        rw->m_borderColor = settings.borderColor;
        rw->m_textColor   = settings.textColor;

        {
            HDC hdc = GetDC(nullptr);
            int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(nullptr, hdc);
            rw->m_dpiScale = (float)dpi / 96.0f;
        }

        auto S = [rw](int v) -> int { return (int)(v * rw->m_dpiScale + 0.5f); };

        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = dlgProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"PinyinIMERegister";
        RegisterClassExW(&wc);

        HWND hDlg = CreateWindowExW(WS_EX_TOPMOST,
            L"PinyinIMERegister", L"PinyinIME 注册进度",
            WS_POPUP,
            CW_USEDEFAULT, CW_USEDEFAULT, S(530), S(280),
            hParent, nullptr, hInst, rw);

        if (hDlg) {
            RECT prc;
            if (hParent && IsWindow(hParent)) {
                GetWindowRect(hParent, &prc);
            } else {
                prc.left = 0; prc.top = 0;
                prc.right  = GetSystemMetrics(SM_CXSCREEN);
                prc.bottom = GetSystemMetrics(SM_CYSCREEN);
            }
            RECT rc2;
            GetWindowRect(hDlg, &rc2);
            int w = rc2.right - rc2.left, h = rc2.bottom - rc2.top;
            int cx = prc.left + (prc.right - prc.left - w) / 2;
            int cy = prc.top  + (prc.bottom  - prc.top  - h) / 2;
            SetWindowPos(hDlg, nullptr, cx, cy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            ShowWindow(hDlg, SW_SHOWNORMAL);
            UpdateWindow(hDlg);
        }

        MSG msg;
        while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        delete rw;
    }
};
