// exe/uninstall_window.h — 卸载进度窗口
// 逐步异步展示卸载过程, 支持手动结束占用进程, UAC 提权
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include "../shared/pinyin_settings.h"
#include "registration.h"

// ==================== 自定义消息 ====================
#define WM_APP_STEP_COMPLETE (WM_APP + 10)  // wParam=stepIdx, lParam=success(bool)
#define WM_APP_START_STEP    (WM_APP + 11)  // wParam=stepIdx

// ==================== 控件 ID ====================
#define IDC_CLOSE      999
#define IDC_KILL_BASE  1000   // +i 对应第 i 个进程
#define IDC_ELEVATE    2000

// ==================== 步骤状态 ====================
enum class StepState : int {
    PENDING = 0,   // 等待中
    RUNNING = 1,   // 执行中
    SUCCESS = 2,   // 成功
    FAILURE = 3,   // 失败
    WARNING = 4,   // 有残留进程
    WAITING = 5,   // 等待用户操作
};

// ==================== 步骤类型 (按执行顺序) ====================
enum class StepType : int {
    CheckAdmin      = 0,
    TsfUnregister   = 1,
    ComCleanup      = 2,
    AutostartRemove = 3,
    DllLockCheck    = 4,
    FinalCleanup    = 5,
    Count           = 6,
};

// ==================== 每个步骤的信息 ====================
struct StepInfo {
    std::wstring label;
    StepState    state = StepState::PENDING;
    std::wstring detail;

    // DLL 占用检查步骤专用
    std::vector<LockedProcess> processes;
    std::vector<HWND>          killButtons;
    std::vector<bool>           killed;

    RECT rowRect = {};
    std::vector<RECT> procRects;
};

// ==================== 工作线程 → UI 线程 结果传递 ====================
struct StepResultData {
    bool        success = false;
    std::wstring detail;
    std::vector<LockedProcess> lockedProcesses;
};

// ==================== UninstallWindow ====================
struct UninstallWindow {
    // ── 窗口 ──
    HWND  m_hDlg      = nullptr;
    float m_dpiScale  = 1.0f;
    int   m_roundR    = 10;
    RECT  m_closeBtnRect = {};

    // ── 配色 (从 PinyinSettings 复制) ──
    COLORREF m_bgColor     = RGB(0xF0, 0xF5, 0xF0);
    COLORREF m_borderColor = RGB(0x96, 0xC6, 0x96);
    COLORREF m_textColor   = RGB(0x28, 0x3D, 0x28);

    // ── GDI 资源 ──
    HFONT  m_hFont    = nullptr;
    HBRUSH m_hBgBrush = nullptr;

    // ── 步骤数据 ──
    std::vector<StepInfo> m_steps;
    int     m_currentStepIdx = -1;
    bool    m_isWorking      = false;
    HANDLE  m_workerThread   = nullptr;
    StepResultData m_pendingResult;

    // ── DLL 路径 ──
    std::wstring m_dllPath;

    // ── 子控件 ──
    HWND m_hCloseBtn   = nullptr;
    HWND m_hElevateBtn = nullptr;

    // DPI 缩放
    int S(int v) const { return (int)(v * m_dpiScale + 0.5f); }

    // ==================== 窗口过程 ====================
    static LRESULT CALLBACK dlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        UninstallWindow* self = (UninstallWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        switch (msg) {
        case WM_CREATE: {
            self = (UninstallWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
            self->m_hDlg = hwnd;
            self->initUI();
            PostMessageW(hwnd, WM_APP_START_STEP, 0, 0);
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

            // ── GDI+ 4 层渐变背景 ──
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

            // ── 标题栏 ──
            {
                SelectObject(hdc, self->m_hFont);
                SetBkMode(hdc, TRANSPARENT);
                int titleH = self->S(32);
                SetTextColor(hdc, self->m_textColor);
                RECT titleRc = { self->S(15), 0, rc.right - self->S(40), titleH };
                DrawTextW(hdc, L"PinyinIME 卸载进度", -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                // 关闭按钮 ✕
                int btnSize = self->S(22), btnX = rc.right - self->S(30), btnY = (titleH - btnSize) / 2;
                self->m_closeBtnRect = { btnX, btnY, btnX + btnSize, btnY + btnSize };
                {
                    Gdiplus::Graphics g(hdc);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                    RECT br2 = self->m_closeBtnRect;
                    int r2 = 3;
                    COLORREF bg = self->m_bgColor;
                    int rr = GetRValue(bg), gg = GetGValue(bg), bb = GetBValue(bg);
                    int brBright = (rr * 299 + gg * 587 + bb * 114) / 1000;
                    int delta = brBright > 128 ? -18 : 18;
                    auto clmp = [](int v) -> int { return v < 0 ? 0 : (v > 255 ? 255 : v); };
                    Gdiplus::SolidBrush fb(Gdiplus::Color(255, clmp(rr + delta), clmp(gg + delta), clmp(bb + delta)));
                    Gdiplus::GraphicsPath cp;
                    cp.StartFigure();
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

                // 分隔线
                {
                    Gdiplus::Graphics g(hdc);
                    int alpha = 60;
                    Gdiplus::Color sepColor(alpha, GetRValue(self->m_borderColor), GetGValue(self->m_borderColor), GetBValue(self->m_borderColor));
                    Gdiplus::Pen sepPen(sepColor, 1.0f);
                    g.DrawLine(&sepPen, self->S(10), titleH, self->S(520), titleH);
                }
            }

            // ── 步骤列表 ──
            {
                SelectObject(hdc, self->m_hFont);
                SetBkMode(hdc, TRANSPARENT);
                for (size_t si = 0; si < self->m_steps.size(); si++) {
                    auto& step = self->m_steps[si];
                    RECT& sr = step.rowRect;

                    // 状态图标
                    const wchar_t* icon = L"○";
                    COLORREF iconColor = RGB(0x88, 0x88, 0x88);
                    switch (step.state) {
                    case StepState::PENDING: icon = L"○"; iconColor = RGB(0x88, 0x88, 0x88); break;
                    case StepState::RUNNING: icon = L"●"; iconColor = RGB(0x33, 0x99, 0xFF); break;
                    case StepState::SUCCESS: icon = L"✓"; iconColor = RGB(0x22, 0xAA, 0x22); break;
                    case StepState::FAILURE: icon = L"✗"; iconColor = RGB(0xDD, 0x33, 0x33); break;
                    case StepState::WARNING: icon = L"⚠"; iconColor = RGB(0xDD, 0xAA, 0x00); break;
                    case StepState::WAITING: icon = L"▶"; iconColor = RGB(0xCC, 0x88, 0x00); break;
                    }

                    RECT iconRc = sr; iconRc.right = iconRc.left + self->S(22);
                    SetTextColor(hdc, iconColor);
                    DrawTextW(hdc, icon, -1, &iconRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                    // 步骤名称
                    RECT labelRc = sr; labelRc.left += self->S(26);
                    SetTextColor(hdc, self->m_textColor);
                    DrawTextW(hdc, step.label.c_str(), -1, &labelRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                    // 详情 (多行支持)
                    if (!step.detail.empty()) {
                        RECT detailRc = sr;
                        detailRc.left += self->S(26) + self->S(170);
                        SetTextColor(hdc, RGB(0x88, 0x88, 0x88));
                        DrawTextW(hdc, step.detail.c_str(), -1, &detailRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    }

                    // 进程子行
                    if (!step.processes.empty()) {
                        for (size_t pi = 0; pi < step.processes.size(); pi++) {
                            if (pi >= step.procRects.size()) break;
                            RECT& pr = step.procRects[pi];
                            std::wstring plbl = step.processes[pi].name
                                + L" [PID:" + std::to_wstring(step.processes[pi].pid) + L"]";
                            if (pi < step.killed.size() && step.killed[pi]) {
                                SetTextColor(hdc, RGB(0x22, 0xAA, 0x22));
                                plbl = L"✓ " + plbl;
                            } else {
                                SetTextColor(hdc, self->m_textColor);
                                plbl = L"  " + plbl;
                            }
                            DrawTextW(hdc, plbl.c_str(), -1, &pr,
                                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                        }
                    }
                }
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
            if (id == IDC_CLOSE) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDC_ELEVATE) {
                self->handleElevate();
                return 0;
            }
            if (id >= IDC_KILL_BASE && id < IDC_KILL_BASE + 256) {
                int procIdx = id - IDC_KILL_BASE;
                self->killSingleProcess(procIdx);
                return 0;
            }
            break;
        }
        case WM_TIMER: {
            if (!self) break;
            if (wp == 1) {  // UAC 启动后自动关闭计时器
                KillTimer(hwnd, 1);
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_APP_START_STEP: {
            if (!self) break;
            self->startStep((StepType)(int)wp);
            return 0;
        }
        case WM_APP_STEP_COMPLETE: {
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
            if (self->m_hFont) DeleteObject(self->m_hFont);
            if (self->m_hBgBrush) DeleteObject(self->m_hBgBrush);
            PostQuitMessage(0);
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

        // 计算 DLL 路径
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        m_dllPath = exePath;
        size_t pos = m_dllPath.find_last_of(L"\\/");
        if (pos != std::wstring::npos) m_dllPath = m_dllPath.substr(0, pos + 1);
        m_dllPath += L"PinyinIMETSF.dll";

        // 初始化步骤标签
        m_steps.resize((int)StepType::Count);
        m_steps[(int)StepType::CheckAdmin].label      = L"管理员权限检查";
        m_steps[(int)StepType::TsfUnregister].label    = L"TSF 框架卸载";
        m_steps[(int)StepType::ComCleanup].label       = L"COM 注册表清理";
        m_steps[(int)StepType::AutostartRemove].label  = L"开机自启动移除";
        m_steps[(int)StepType::DllLockCheck].label     = L"DLL 占用检查";
        m_steps[(int)StepType::FinalCleanup].label     = L"最终清理";

        // 关闭按钮
        m_hCloseBtn = CreateWindowExW(0, L"BUTTON", L"关闭",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, S(80), S(28),
            m_hDlg, (HMENU)(UINT_PTR)IDC_CLOSE, hInst, nullptr);
        if (m_hFont) SendMessageW(m_hCloseBtn, WM_SETFONT, (WPARAM)m_hFont, TRUE);

        // 计算圆角半径
        {
            HDC hdc = GetDC(m_hDlg);
            HFONT hOld = (HFONT)SelectObject(hdc, m_hFont);
            TEXTMETRICW tm; GetTextMetricsW(hdc, &tm);
            SelectObject(hdc, hOld);
            ReleaseDC(m_hDlg, hdc);
            m_roundR = (std::max)(6, (std::min)(12, (int)(tm.tmHeight * 2 / 3)));
        }

        recalcLayout();
    }

    // ==================== 布局计算 ====================
    void recalcLayout() {
        int y = S(32) + S(12);  // 标题栏下方
        int stepH = S(28);
        int gap = S(4);
        int procH = S(24);
        int procGap = S(3);

        for (auto& step : m_steps) {
            step.rowRect = { S(15), y, S(510), y + stepH };
            y += stepH + gap;

            if (!step.processes.empty()) {
                y += S(2);
                step.procRects.clear();
                for (size_t i = 0; i < step.processes.size(); i++) {
                    RECT pr = { S(45), y, S(400), y + procH };
                    step.procRects.push_back(pr);
                    y += procH + procGap;
                }
            }
        }

        y += S(10);

        // 关闭按钮 (居中)
        int btnW = S(80), btnH = S(28);
        SetWindowPos(m_hCloseBtn, nullptr, (S(530) - btnW) / 2, y, btnW, btnH, SWP_NOZORDER);

        y += btnH + S(15);

        // 更新窗口高度
        SetWindowPos(m_hDlg, nullptr, 0, 0, S(530), y, SWP_NOMOVE | SWP_NOZORDER);

        // 重新定位进程结束按钮
        positionKillButtons();
    }

    void positionKillButtons() {
        auto& step = m_steps[(int)StepType::DllLockCheck];
        for (size_t i = 0; i < step.killButtons.size(); i++) {
            if (step.killButtons[i] && i < step.procRects.size()) {
                RECT& pr = step.procRects[i];
                int btnW = S(80), btnH = S(22);
                int btnX = S(510) - btnW;
                int btnY = pr.top + (pr.bottom - pr.top - btnH) / 2;
                SetWindowPos(step.killButtons[i], nullptr, btnX, btnY, btnW, btnH, SWP_NOZORDER);
            }
        }
    }

    // ==================== 步骤状态机 ====================
    void startStep(StepType type) {
        if (m_isWorking) return;
        m_currentStepIdx = (int)type;
        m_isWorking = true;

        auto& step = m_steps[(int)type];
        step.state = StepState::RUNNING;
        InvalidateRect(m_hDlg, nullptr, TRUE);

        // 步骤 0: 权限检查在主线程同步执行
        if (type == StepType::CheckAdmin) {
            runCheckAdmin();
            return;
        }

        // 步骤 1-5: 工作线程异步执行
        m_workerThread = CreateThread(nullptr, 0, stepWorkerThread, this, 0, nullptr);
        if (!m_workerThread) {
            // 线程创建失败 → 直接标记失败
            step.state = StepState::FAILURE;
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
        auto& result = m_pendingResult;
        step.detail = result.detail;

        // DLL 占用检查步骤: 有残留进程 → 进入 WAITING 状态
        if (stepIdx == (int)StepType::DllLockCheck && !result.lockedProcesses.empty()) {
            step.state = StepState::WARNING;
            step.processes = std::move(result.lockedProcesses);
            step.killed.assign(step.processes.size(), false);
            createKillButtons();
            recalcLayout();
            InvalidateRect(m_hDlg, nullptr, TRUE);
            // 不自动推进, 等待用户操作
            enableCloseButton(true);
            return;
        }

        // 正常完成
        step.state = success ? StepState::SUCCESS : StepState::FAILURE;
        recalcLayout();
        InvalidateRect(m_hDlg, nullptr, TRUE);
        enableCloseButton(true);
        startNextStep();
    }

    void startNextStep() {
        int next = m_currentStepIdx + 1;
        if (next >= (int)StepType::Count) {
            // 全部完成
            InvalidateRect(m_hDlg, nullptr, TRUE);
            return;
        }
        PostMessageW(m_hDlg, WM_APP_START_STEP, (WPARAM)next, 0);
    }

    void enableCloseButton(bool enable) {
        if (m_hCloseBtn) {
            EnableWindow(m_hCloseBtn, enable ? TRUE : FALSE);
        }
    }

    // ==================== 步骤 0: 权限检查 (主线程) ====================
    void runCheckAdmin() {
        auto& step = m_steps[(int)StepType::CheckAdmin];

        if (IsRunningAsAdmin()) {
            step.state = StepState::SUCCESS;
            step.detail = L"已具有管理员权限";
            m_isWorking = false;
            InvalidateRect(m_hDlg, nullptr, TRUE);
            startNextStep();
        } else {
            step.state = StepState::WAITING;
            step.detail = L"需要管理员权限才能完整卸载 TSF 输入法";
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
            m_hDlg, (HMENU)(UINT_PTR)IDC_ELEVATE, hInst, nullptr);
        if (m_hFont) SendMessageW(m_hElevateBtn, WM_SETFONT, (WPARAM)m_hFont, TRUE);
    }

    void handleElevate() {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"runas";
        sei.lpFile = exePath;
        sei.lpParameters = L"--uninstall-with-ui";
        sei.nShow = SW_SHOWNORMAL;

        if (ShellExecuteExW(&sei)) {
            auto& step = m_steps[(int)StepType::CheckAdmin];
            step.detail = L"已在管理员权限下打开新窗口，请在新窗口中完成卸载。";
            step.state = StepState::SUCCESS;
            if (m_hElevateBtn) { DestroyWindow(m_hElevateBtn); m_hElevateBtn = nullptr; }
            InvalidateRect(m_hDlg, nullptr, TRUE);
            SetTimer(m_hDlg, 1, 2000, nullptr);
        } else {
            auto& step = m_steps[(int)StepType::CheckAdmin];
            step.detail = L"提权失败，请右键以管理员身份运行 PinyinIME.exe 后重试。";
            step.state = StepState::FAILURE;
            if (m_hElevateBtn) { DestroyWindow(m_hElevateBtn); m_hElevateBtn = nullptr; }
            InvalidateRect(m_hDlg, nullptr, TRUE);
            enableCloseButton(true);
        }
    }

    // ==================== 进程终止交互 ====================
    void createKillButtons() {
        auto& step = m_steps[(int)StepType::DllLockCheck];
        // 先销毁旧的
        for (auto& hBtn : step.killButtons) {
            if (hBtn) DestroyWindow(hBtn);
        }
        step.killButtons.clear();

        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(m_hDlg, GWLP_HINSTANCE);
        for (size_t i = 0; i < step.processes.size(); i++) {
            HWND hBtn = CreateWindowExW(0, L"BUTTON", L"结束进程",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0, 0, S(80), S(22),
                m_hDlg, (HMENU)(UINT_PTR)(IDC_KILL_BASE + i), hInst, nullptr);
            if (m_hFont) SendMessageW(hBtn, WM_SETFONT, (WPARAM)m_hFont, TRUE);
            step.killButtons.push_back(hBtn);
        }
    }

    void killSingleProcess(int procIdx) {
        auto& step = m_steps[(int)StepType::DllLockCheck];
        if (procIdx >= (int)step.processes.size()) return;
        if (procIdx < (int)step.killed.size() && step.killed[procIdx]) return;

        HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, step.processes[procIdx].pid);
        if (hProc) {
            TerminateProcess(hProc, 0);
            CloseHandle(hProc);
        }
        step.killed[procIdx] = true;
        if (procIdx < (int)step.killButtons.size() && step.killButtons[procIdx]) {
            DestroyWindow(step.killButtons[procIdx]);
            step.killButtons[procIdx] = nullptr;
        }
        InvalidateRect(m_hDlg, nullptr, TRUE);

        // 检查是否全部结束
        bool allDone = true;
        for (size_t i = 0; i < step.killed.size(); i++) {
            if (!step.killed[i]) { allDone = false; break; }
        }
        if (allDone) {
            // 复查 DLL 占用
            m_isWorking = true;
            enableCloseButton(false);
            auto& st = m_steps[(int)StepType::DllLockCheck];
            st.state = StepState::RUNNING;
            st.detail = L"复查 DLL 占用...";
            InvalidateRect(m_hDlg, nullptr, TRUE);
            CreateThread(nullptr, 0, dllRecheckThread, this, 0, nullptr);
        }
    }

    static DWORD WINAPI dllRecheckThread(LPVOID param) {
        UninstallWindow* self = (UninstallWindow*)param;
        auto remaining = findAndReleaseDllLocks(self->m_dllPath.c_str());
        self->m_pendingResult = {};
        if (remaining.empty()) {
            self->m_pendingResult.success = true;
            self->m_pendingResult.detail = L"所有进程已释放 DLL";
        } else {
            self->m_pendingResult.success = false;
            self->m_pendingResult.detail = L"仍有进程占用 DLL";
            self->m_pendingResult.lockedProcesses = std::move(remaining);
        }
        PostMessageW(self->m_hDlg, WM_APP_STEP_COMPLETE,
            (WPARAM)(int)StepType::DllLockCheck,
            (LPARAM)self->m_pendingResult.success);
        return 0;
    }

    // ==================== 工作线程 ====================
    static DWORD WINAPI stepWorkerThread(LPVOID param) {
        UninstallWindow* self = (UninstallWindow*)param;
        int stepIdx = self->m_currentStepIdx;

        // TSF 和最终清理步骤需要 COM
        bool needCom = (stepIdx == (int)StepType::TsfUnregister ||
                        stepIdx == (int)StepType::FinalCleanup);
        if (needCom) CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        StepResultData result;

        switch ((StepType)stepIdx) {
        case StepType::TsfUnregister:
            result = executeTsfUnregister(self->m_dllPath);
            break;
        case StepType::ComCleanup:
            result = executeComCleanup(self->m_dllPath);
            break;
        case StepType::AutostartRemove:
            result = executeAutostartRemove();
            break;
        case StepType::DllLockCheck:
            result = executeDllLockCheck(self->m_dllPath);
            break;
        case StepType::FinalCleanup:
            result = executeFinalCleanup(self->m_dllPath);
            break;
        default:
            break;
        }

        if (needCom) CoUninitialize();

        self->m_pendingResult = result;
        PostMessageW(self->m_hDlg, WM_APP_STEP_COMPLETE,
            (WPARAM)stepIdx, (LPARAM)(result.success ? 1 : 0));
        return 0;
    }

    // ==================== 步骤 1: TSF 框架卸载 ====================
    static StepResultData executeTsfUnregister(const std::wstring& dllPath) {
        (void)dllPath;
        StepResultData r;
        std::wstring tmp;
        int ok = 0, fail = 0;

        // 1a. ITfInputProcessorProfileMgr::UnregisterProfile (Win8+)
        ITfInputProcessorProfileMgr* pProfileMgr = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfInputProcessorProfileMgr, (void**)&pProfileMgr);
        if (SUCCEEDED(hr) && pProfileMgr) {
            hr = pProfileMgr->UnregisterProfile(CLSID_PinyinIME, 0x0804, GUID_PinyinProfile, 0);
            if (SUCCEEDED(hr)) { ok++; tmp += L"TSF Profile 已停用; "; }
            else { fail++; wchar_t buf[16]; swprintf(buf, 16, L"%08X", hr);
                   tmp += L"TSF Profile 停用失败(0x" + std::wstring(buf) + L"); "; }
            pProfileMgr->Release();
        }

        // 1b. 传统 API
        ITfInputProcessorProfiles* pProfiles = nullptr;
        hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfInputProcessorProfiles, (void**)&pProfiles);
        if (SUCCEEDED(hr) && pProfiles) {
            hr = pProfiles->RemoveLanguageProfile(CLSID_PinyinIME, 0x0804, GUID_PinyinProfile);
            if (SUCCEEDED(hr)) { ok++; tmp += L"语言配置文件 已移除; "; }
            else { fail++; tmp += L"语言配置文件 移除失败; "; }

            hr = pProfiles->Unregister(CLSID_PinyinIME);
            if (SUCCEEDED(hr)) { ok++; tmp += L"文本服务 已取消注册; "; }
            else { fail++; tmp += L"文本服务 取消注册失败; "; }

            pProfiles->Release();
        }

        // 1c. COM 垃圾回收
        for (int i = 0; i < 5; i++) {
            CoFreeUnusedLibrariesEx(0, 0);
            Sleep(100);
        }

        r.detail = tmp;
        r.success = (fail == 0);
        return r;
    }

    // ==================== 步骤 2: COM 注册表清理 ====================
    static StepResultData executeComCleanup(const std::wstring& dllPath) {
        StepResultData r;

        HMODULE hDll = LoadLibraryW(dllPath.c_str());
        if (!hDll) {
            hDll = LoadLibraryExW(dllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        }

        if (hDll) {
            typedef HRESULT(STDAPICALLTYPE* DllUnregisterServerFn)();
            auto pfn = (DllUnregisterServerFn)GetProcAddress(hDll, "DllUnregisterServer");
            if (pfn) {
                HRESULT hr = pfn();
                if (SUCCEEDED(hr)) {
                    r.detail = L"COM 注册表项 已清理";
                    r.success = true;
                } else {
                    wchar_t buf[16]; swprintf(buf, 16, L"%08X", hr);
                    r.detail = L"COM 注册表项 清理失败 (0x" + std::wstring(buf) + L")";
                    r.success = false;
                }
            } else {
                r.detail = L"DLL 缺少 DllUnregisterServer 导出";
                r.success = false;
            }
            FreeLibrary(hDll);
        } else {
            r.detail = L"跳过 COM 注册表清理 (DLL 被占用)";
            r.success = true;
        }
        return r;
    }

    // ==================== 步骤 3: 开机自启动移除 ====================
    static StepResultData executeAutostartRemove() {
        StepResultData r;
        HKEY hKey = nullptr;
        LONG res = RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &hKey);
        if (res == ERROR_SUCCESS) {
            res = RegDeleteValueW(hKey, L"PinyinIME");
            RegCloseKey(hKey);
            if (res == ERROR_SUCCESS) {
                r.detail = L"开机自启动 已移除";
                r.success = true;
            } else if (res == ERROR_FILE_NOT_FOUND) {
                r.detail = L"开机自启动 无需移除 (不存在)";
                r.success = true;
            } else {
                wchar_t buf[16]; swprintf(buf, 16, L"%08X", res);
                r.detail = L"开机自启动 移除失败 (0x" + std::wstring(buf) + L")";
                r.success = false;
            }
        } else {
            wchar_t buf[16]; swprintf(buf, 16, L"%08X", res);
            r.detail = L"开机自启动 无法打开注册表项 (0x" + std::wstring(buf) + L")";
            r.success = false;
        }
        return r;
    }

    // ==================== 步骤 4: DLL 占用检查 ====================
    static StepResultData executeDllLockCheck(const std::wstring& dllPath) {
        StepResultData r;
        auto locked = findAndReleaseDllLocks(dllPath.c_str());
        if (locked.empty()) {
            r.detail = L"无其他进程占用 DLL";
            r.success = true;
        } else {
            int cnt = (int)locked.size();
            r.detail = L"发现 " + std::to_wstring(cnt) + L" 个进程占用 DLL，请手动结束";
            r.lockedProcesses = std::move(locked);
            r.success = false;
        }
        return r;
    }

    // ==================== 步骤 5: 最终清理 ====================
    static StepResultData executeFinalCleanup(const std::wstring& dllPath) {
        StepResultData r;

        for (int i = 0; i < 3; i++) {
            CoFreeUnusedLibrariesEx(0, 0);
            Sleep(100);
        }

        HANDLE hFile = CreateFileW(dllPath.c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile);
            r.detail = L"DLL 文件锁已释放 — 可安全替换";
            r.success = true;
        } else {
            wchar_t buf[16]; swprintf(buf, 16, L"%08X", GetLastError());
            r.detail = L"DLL 文件仍被占用 (需注销/重启后替换) (0x"
                + std::wstring(buf) + L")";
            r.success = false;
        }
        return r;
    }

    // ==================== show() 静态入口 ====================
    static void show(HINSTANCE hInst, HWND hParent, const PinyinSettings& settings) {
        UninstallWindow* uw = new UninstallWindow();
        uw->m_bgColor     = settings.bgColor;
        uw->m_borderColor = settings.borderColor;
        uw->m_textColor   = settings.textColor;

        // DPI
        {
            HDC hdc = GetDC(nullptr);
            int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(nullptr, hdc);
            uw->m_dpiScale = (float)dpi / 96.0f;
        }

        auto S = [uw](int v) -> int { return (int)(v * uw->m_dpiScale + 0.5f); };

        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = dlgProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"PinyinIMEUninstall";
        RegisterClassExW(&wc);

        HWND hDlg = CreateWindowExW(WS_EX_TOPMOST,
            L"PinyinIMEUninstall", L"PinyinIME 卸载进度",
            WS_POPUP,
            CW_USEDEFAULT, CW_USEDEFAULT, S(530), S(300),
            hParent, nullptr, hInst, uw);

        if (hDlg) {
            // 居中于父窗口 (或屏幕)
            RECT prc;
            if (hParent && IsWindow(hParent)) {
                GetWindowRect(hParent, &prc);
            } else {
                prc.left = 0; prc.top = 0;
                prc.right  = GetSystemMetrics(SM_CXSCREEN);
                prc.bottom = GetSystemMetrics(SM_CYSCREEN);
            }
            RECT rc;
            GetWindowRect(hDlg, &rc);
            int w = rc.right - rc.left, h = rc.bottom - rc.top;
            int cx = prc.left + (prc.right - prc.left - w) / 2;
            int cy = prc.top  + (prc.bottom  - prc.top  - h) / 2;
            SetWindowPos(hDlg, nullptr, cx, cy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            ShowWindow(hDlg, SW_SHOWNORMAL);
            UpdateWindow(hDlg);
        }

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            if (!IsWindow(hDlg)) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        delete uw;
    }
};
