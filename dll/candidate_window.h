// dll/candidate_window.h — CandidateWindow (从 main.cpp 提取, 适配 DLL)
// GDI+ 候选词弹出窗口, 放在独立线程中运行
#pragma once
#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <oleacc.h>
#include <UIAutomationClient.h>
#include <string>
#include <vector>
#include "../shared/pinyin_settings.h"
#include "../shared/utf_utils.h"
#include "../shared/ime_ipc.h"
#include "pinyin_engine.h"

extern HINSTANCE g_hDllInst;
extern PinyinEngine* g_pSharedEngine;  // 指向 CPinyinTextService::m_engine

class CandidateWindow {
public:
    HWND m_hwnd = nullptr;
    HFONT m_font = nullptr;
    bool m_visible = false;
    RECT m_settingsBtnRect = {};
    bool m_trackingMouse = false;
    int m_selectedIndex = 0;  // 当前选中的候选词索引 (相对当前页)
    int m_textY = 6;
    int m_rowH = 24;
    HRGN m_roundRgn = nullptr;
    int m_roundR = 10;
    float m_dpiScale = 1.0f;
    PinyinSettings* m_pSettings = nullptr;
    // TSF 标准: 在 edit session 内通过 ITfContextView::GetTextExt 拿到准确坐标,
    // 缓存后供 getCaretPosition 优先使用 (解决 Firefox 等不使用 Win32 caret 的应用)
    POINT m_tsfCaretPos = {0, 0};
    bool m_hasTsfCaretPos = false;
    // 预组合光标: 在 startComposition 插入文本前捕获的原始光标位置,
    // 此时应用程序的原生选区仍然有效, GetTextExt 返回的坐标通常更可靠
    POINT m_preCompCaretPos = {0, 0};
    bool m_hasPreCompCaretPos = false;

    COLORREF getBgColor()     { return m_pSettings ? m_pSettings->bgColor     : RGB(0xF0,0xF5,0xF0); }
    COLORREF getBorderColor() { return m_pSettings ? m_pSettings->borderColor : RGB(0x96,0xC6,0x96); }
    COLORREF getTextColor()   { return m_pSettings ? m_pSettings->textColor   : RGB(0x28,0x3D,0x28); }
    COLORREF getIndexColor()  { return m_pSettings ? m_pSettings->indexColor  : RGB(0x3C,0x81,0x3C); }
    COLORREF getInputColor()  { return m_pSettings ? m_pSettings->inputColor  : RGB(0x32,0x64,0x32); }

    void create(HINSTANCE hInst) {
        // 获取主显示器 DPI 缩放比例
        HDC hdcScreen = GetDC(nullptr);
        int dpi = GetDeviceCaps(hdcScreen, LOGPIXELSY);
        ReleaseDC(nullptr, hdcScreen);
        m_dpiScale = dpi / 96.0f;

        int fontSize = m_pSettings ? m_pSettings->fontSize : 20;
        m_font = CreateFontW(-(int)(fontSize * m_dpiScale + 0.5f), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
            m_pSettings ? m_pSettings->fontName.c_str() : L"Microsoft YaHei");

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(getBgColor());
        wc.lpszClassName = L"PinyinIMECandidateDLL";
        RegisterClassExW(&wc);

        m_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"PinyinIMECandidateDLL", L"", WS_POPUP,
            0, 0, 400, 30, nullptr, nullptr, hInst, nullptr);
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);
    }

    void destroy() {
        if (m_roundRgn) { DeleteObject(m_roundRgn); m_roundRgn = nullptr; }
        if (m_font) { DeleteObject(m_font); m_font = nullptr; }
        if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    }

    POINT getCaretPosition() {
        POINT pt = {0, 0};
        bool found = false;

        // ── 先查询 Win32 GUI 线程信息 (用于交叉验证 TSF 坐标) ──
        POINT guiCaretPt = {0, 0};
        bool hasGuiCaret = false;
        POINT guiFocusPt = {0, 0};
        bool hasGuiFocus = false;
        {
            GUITHREADINFO gti = {};
            gti.cbSize = sizeof(gti);
            if (GetGUIThreadInfo(GetCurrentThreadId(), &gti)) {
                // Win32 光标 (System Caret) — 系统管理, 在所有原生 Win32 应用中可靠
                if (gti.hwndCaret && (gti.rcCaret.right > 0 || gti.rcCaret.bottom > 0)) {
                    POINT caretPt = {gti.rcCaret.left, gti.rcCaret.bottom};
                    if (ClientToScreen(gti.hwndCaret, &caretPt)) {
                        guiCaretPt = caretPt;
                        hasGuiCaret = true;
                    }
                }
                // 焦点窗口矩形 — 较粗粒度的后备
                if (gti.hwndFocus) {
                    RECT rc;
                    if (GetWindowRect(gti.hwndFocus, &rc)) {
                        guiFocusPt.x = rc.left + 4;
                        guiFocusPt.y = rc.bottom + 4;
                        hasGuiFocus = true;
                    }
                }
            }
        }

        // 辅助: 判断两个屏幕坐标是否 "接近" (60px 阈值内认为一致)
        auto isCloseEnough = [](POINT a, POINT b) -> bool {
            return abs(a.x - b.x) < 60 && abs(a.y - b.y) < 60;
        };

        // 辅助: 判断 TSF 坐标是否看起来合理 (在屏幕范围内, 非零)
        auto isValidTsfPos = [](POINT p) -> bool {
            if (p.x == 0 && p.y == 0) return false;
            // 排除明显出界的坐标 (屏幕坐标不应为负值过大, 这里简单检查)
            if (p.x < -10000 || p.y < -10000) return false;
            if (p.x > 50000 || p.y > 50000) return false;
            return true;
        };

        // ── 方法 0: 预组合 TSF 光标 (在组合文本插入前捕获, 最接近应用原生光标) ──
        if (m_hasPreCompCaretPos && isValidTsfPos(m_preCompCaretPos)) {
            // 交叉验证: 如果 Win32 光标存在且与 TSF 坐标不一致, 优先信任 Win32 光标
            // (Firefox 等应用对 TSF GetTextExt 实现不完整, 但可能暴露了正确的 Win32 caret)
            if (hasGuiCaret && !isCloseEnough(m_preCompCaretPos, guiCaretPt)) {
                pt = guiCaretPt;
            } else {
                pt = m_preCompCaretPos;
            }
            found = true;
        }

        // ── 方法 1: TSF GetTextExt 组合时缓存 (带交叉验证) ──
        if (!found && m_hasTsfCaretPos && isValidTsfPos(m_tsfCaretPos)) {
            if (hasGuiCaret && !isCloseEnough(m_tsfCaretPos, guiCaretPt)) {
                // TSF 坐标与系统光标不一致: 优先信任系统光标
                // 典型场景: Firefox 地址栏 / Chrome 等应用 GetTextExt 返回偏移坐标
                pt = guiCaretPt;
            } else {
                pt = m_tsfCaretPos;
            }
            found = true;
        }

        // ── 方法 2: Win32 系统光标 (GetGUIThreadInfo) ──
        if (!found && hasGuiCaret) {
            pt = guiCaretPt;
            found = true;
        }

        // ── 方法 3: 焦点窗口矩形 ──
        if (!found && hasGuiFocus) {
            pt = guiFocusPt;
            found = true;
        }

        // ── 方法 4: UI Automation (现代应用: Chrome/Edge/VSCode/UWP 等) ──
        if (!found) {
            IUIAutomation* pUIA = nullptr;
            HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&pUIA));
            if (SUCCEEDED(hr) && pUIA) {
                IUIAutomationElement* pFocused = nullptr;
                hr = pUIA->GetFocusedElement(&pFocused);
                if (SUCCEEDED(hr) && pFocused) {
                    IUIAutomationTextPattern2* pText2 = nullptr;
                    hr = pFocused->GetCurrentPatternAs(UIA_TextPattern2Id, IID_PPV_ARGS(&pText2));
                    if (SUCCEEDED(hr) && pText2) {
                        BOOL isActive = FALSE;
                        IUIAutomationTextRange* pCaret = nullptr;
                        if (SUCCEEDED(pText2->GetCaretRange(&isActive, &pCaret)) && pCaret) {
                            SAFEARRAY* pRectArray = nullptr;
                            if (SUCCEEDED(pCaret->GetBoundingRectangles(&pRectArray)) && pRectArray) {
                                double* pData = nullptr;
                                if (SUCCEEDED(SafeArrayAccessData(pRectArray, (void**)&pData))) {
                                    long ub; SafeArrayGetUBound(pRectArray, 1, &ub);
                                    if (ub >= 3) {
                                        pt.x = (LONG)pData[0];
                                        pt.y = (LONG)(pData[1] + pData[3]);
                                        found = true;
                                    }
                                    SafeArrayUnaccessData(pRectArray);
                                }
                                SafeArrayDestroy(pRectArray);
                            }
                            pCaret->Release();
                        }
                        pText2->Release();
                    }
                    if (!found) {
                        RECT rc = {};
                        if (SUCCEEDED(pFocused->get_CurrentBoundingRectangle(&rc)) &&
                            (rc.right > rc.left || rc.bottom > rc.top)) {
                            pt.x = rc.left;
                            pt.y = rc.bottom + 4;
                            found = true;
                        }
                    }
                    pFocused->Release();
                }
                pUIA->Release();
            }
        }

        // ── 方法 5: 前台窗口 ──
        if (!found) {
            HWND hForeground = GetForegroundWindow();
            if (hForeground) {
                RECT fgRect;
                if (GetWindowRect(hForeground, &fgRect)) {
                    pt.x = fgRect.left + 40;
                    pt.y = fgRect.bottom - 60;
                    found = true;
                }
            }
        }

        // ── 方法 6: 屏幕保底 ──
        if (!found) {
            RECT screen;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &screen, 0);
            pt.x = screen.left + 200;
            pt.y = screen.bottom - 120;
        }
        return pt;
    }

    void show(const std::vector<std::pair<std::string,int>>& candidates, int pageIndex) {
        if (!m_hwnd) return;
        m_selectedIndex = 0;  // 新候选列表默认选中第一个
        POINT pt = getCaretPosition();

        // ── 根据目标显示器更新 DPI 缩放比例 ──
        {
            HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            UINT dpiY = 96;
            HMODULE hShcore = LoadLibraryW(L"shcore.dll");
            if (hShcore) {
                auto pfn = (HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*))
                    GetProcAddress(hShcore, "GetDpiForMonitor");
                if (pfn) { UINT dpiX; pfn(hMon, 0, &dpiX, &dpiY); }
                FreeLibrary(hShcore);
            } else {
                HDC hdcScreen = GetDC(nullptr);
                dpiY = GetDeviceCaps(hdcScreen, LOGPIXELSY);
                ReleaseDC(nullptr, hdcScreen);
            }
            m_dpiScale = dpiY / 96.0f;
        }
        auto S = [this](int v) -> int { return (int)(v * m_dpiScale + 0.5f); };

        HDC hdc = GetDC(m_hwnd);
        SelectObject(hdc, m_font);
        TEXTMETRICW tm; GetTextMetrics(hdc, &tm);

        bool vert = m_pSettings ? m_pSettings->verticalLayout : false;
        bool showGear = m_pSettings ? m_pSettings->showSettingsGear : true;
        int leftPad = S(8), rightPad = S(12), candSpacing = S(8);

        int width; SIZE sz;
        if (vert && !candidates.empty()) {
            // 竖排: 宽度取最宽候选词 + 内边距
            width = leftPad + rightPad;
            for (int i = 0; i < (int)candidates.size(); i++) {
                std::wstring wtext = utf8ToWide(std::to_string(i + 1) + "." + candidates[i].first);
                GetTextExtentPoint32W(hdc, wtext.c_str(), (int)wtext.size(), &sz);
                int candW = leftPad + sz.cx + rightPad;
                if (candW > width) width = candW;
            }
            if (showGear) {
                SIZE gearSz;
                GetTextExtentPoint32W(hdc, L"⚙", 1, &gearSz);
                int gearW = leftPad + gearSz.cx + rightPad;
                if (gearW > width) width = gearW;
            }
        } else {
            // 横排: 所有候选词宽度之和 + 间距 (与 WM_PAINT 的 x+=totalW+8 保持一致)
            width = leftPad;
            for (int i = 0; i < (int)candidates.size(); i++) {
                std::wstring wtext = utf8ToWide(std::to_string(i + 1) + "." + candidates[i].first);
                GetTextExtentPoint32W(hdc, wtext.c_str(), (int)wtext.size(), &sz);
                width += sz.cx + candSpacing;
            }
            if (showGear) {
                SIZE gearSz;
                GetTextExtentPoint32W(hdc, L"⚙", 1, &gearSz);
                width += gearSz.cx;
            }
            width += rightPad;
        }
        ReleaseDC(m_hwnd, hdc);

        RECT screen; SystemParametersInfoW(SPI_GETWORKAREA, 0, &screen, 0);
        int maxWidth = (screen.right - screen.left) * 85 / 100;
        if (width > maxWidth) width = maxWidth;

        int borderW = S(3);
        m_roundR = (std::max)(6, (std::min)(16, (int)(tm.tmHeight * 2 / 3)));
        int height;
        if (vert && !candidates.empty()) {
            m_rowH = tm.tmHeight + S(6);
            m_textY = borderW + S(4);
            height = m_textY + ((int)candidates.size() + 1) * m_rowH + borderW + S(6);
        } else {
            int pad = (std::max)(4, (int)(tm.tmHeight / 8));
            m_textY = borderW + pad;
            height = m_textY + tm.tmHeight + pad + borderW;
            m_rowH = tm.tmHeight;
        }

        if (m_roundRgn) { DeleteObject(m_roundRgn); m_roundRgn = nullptr; }
        if (m_pSettings && m_pSettings->roundedCorner) {
            m_roundRgn = CreateRoundRectRgn(0, 0, width + 1, height + 1, m_roundR * 2, m_roundR * 2);
            SetWindowRgn(m_hwnd, m_roundRgn, TRUE);
        } else {
            SetWindowRgn(m_hwnd, nullptr, TRUE);
        }

        int x = pt.x, y = pt.y + 5;
        if (x + width > screen.right) x = screen.right - width;
        if (x < screen.left) x = screen.left;
        if (y + height > screen.bottom) y = pt.y - height - 5;

        SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        m_visible = true;
        // 触发 light dismiss 可访问性事件 (ime.md 要求)
        NotifyWinEvent(EVENT_OBJECT_IME_SHOW, m_hwnd, OBJID_CLIENT, CHILDID_SELF);
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }

    void hide() {
        if (m_hwnd && m_visible) {
            // 触发 light dismiss 可访问性事件 (ime.md 要求)
            NotifyWinEvent(EVENT_OBJECT_IME_HIDE, m_hwnd, OBJID_CLIENT, CHILDID_SELF);
            ShowWindow(m_hwnd, SW_HIDE);
            m_visible = false;
        }
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_PAINT) {
            CandidateWindow* self = (CandidateWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (self && self->m_visible && g_pSharedEngine) {
                PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
                RECT rc; GetClientRect(hwnd, &rc);

                {   // GDI+ 背景 + 渐变边框 (FillPath 同心层叠, 边框粗细均匀)
                    Gdiplus::Graphics graphics(hdc);
                    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                    int w = rc.right, h = rc.bottom, cr = self->m_roundR;
                    if(self->m_pSettings && !self->m_pSettings->roundedCorner) cr = 0;
                    auto makeRR = [](Gdiplus::GraphicsPath& p, int x, int y, int rw, int rh, int rad) {
                        p.Reset(); p.StartFigure();
                        if(rad<=0){
                            p.AddLine(Gdiplus::Point(x,y),Gdiplus::Point(x+rw,y));
                            p.AddLine(Gdiplus::Point(x+rw,y),Gdiplus::Point(x+rw,y+rh));
                            p.AddLine(Gdiplus::Point(x+rw,y+rh),Gdiplus::Point(x,y+rh));
                            p.AddLine(Gdiplus::Point(x,y+rh),Gdiplus::Point(x,y));
                        }else{
                            int dia = rad * 2;
                            p.AddArc(x, y, dia, dia, 180, 90);
                            p.AddArc(x + rw - dia, y, dia, dia, 270, 90);
                            p.AddArc(x + rw - dia, y + rh - dia, dia, dia, 0, 90);
                            p.AddArc(x, y + rh - dia, dia, dia, 90, 90);
                        }
                        p.CloseFigure();
                    };
                    COLORREF bc=self->getBorderColor(), bgc=self->getBgColor();
                    int rr2=GetRValue(bc),rg2=GetGValue(bc),rb2=GetBValue(bc);
                    int bgb=(GetRValue(bgc)*299+GetGValue(bgc)*587+GetBValue(bgc)*114)/1000;
                    int dir=(bgb<128)?1:-1;
                    auto clmp=[](int v)->int{return v<0?0:(v>255?255:v);};
                    // 4层同心FillPath: 3层边框(每层1px) + 1层背景
                    for(int layer=0;layer<4;layer++){
                        int off=layer, lw=w-off*2, lh=h-off*2, lcr=cr-off;
                        if(cr>0 && lcr<2) lcr=2;
                        int delta=(layer==0)?40:(layer==1)?18:0;
                        COLORREF col=(layer<3)?RGB(clmp(rr2+delta*dir),clmp(rg2+delta*dir),clmp(rb2+delta*dir)):bgc;
                        Gdiplus::SolidBrush br(Gdiplus::Color(255, GetRValue(col), GetGValue(col), GetBValue(col)));
                        Gdiplus::GraphicsPath pth;
                        makeRR(pth,off,off,lw,lh,lcr);
                        graphics.FillPath(&br,&pth);
                    }
                }

                SelectObject(hdc, self->m_font);
                SetBkMode(hdc, TRANSPARENT);
                auto S = [self](int v) -> int { return (int)(v * self->m_dpiScale + 0.5f); };
                int x = S(8), y = self->m_textY;
                auto candidates = g_pSharedEngine->getPageCandidates();
                int candBaseY=y; SIZE sz;
                bool vert = self->m_pSettings ? self->m_pSettings->verticalLayout : false;
                for(int i=0;i<(int)candidates.size();i++){
                    int cy = vert ? (candBaseY+i*self->m_rowH) : y;
                    bool selected = (i == self->m_selectedIndex);
                    std::wstring widx=std::to_wstring(i+1)+L".";
                    std::wstring wtext=utf8ToWide(candidates[i].first);
                    SIZE szIdx,szTxt;
                    GetTextExtentPoint32W(hdc,widx.c_str(),(int)widx.size(),&szIdx);
                    GetTextExtentPoint32W(hdc,wtext.c_str(),(int)wtext.size(),&szTxt);
                    int totalW=szIdx.cx+szTxt.cx;
                    // 选中项: 反色圆角矩形背景
                    if(selected){
                        int selPadX = S(4), selPadY = S(1);
                        RECT selRc = {x - selPadX, cy - selPadY, x + totalW + selPadX, cy + self->m_rowH + selPadY - S(2)};
                        int selR=(self->m_pSettings && !self->m_pSettings->roundedCorner)?0:(std::max)(2,(std::min)(4,self->m_roundR/3));
                        Gdiplus::Graphics g(hdc);
                        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                        COLORREF tc=self->getTextColor();
                        Gdiplus::SolidBrush selBr(Gdiplus::Color(255, GetRValue(tc), GetGValue(tc), GetBValue(tc)));
                        Gdiplus::GraphicsPath sp;
                        sp.StartFigure();
                        if(selR<=0){
                            sp.AddLine(Gdiplus::Point(selRc.left,selRc.top),Gdiplus::Point(selRc.right,selRc.top));
                            sp.AddLine(Gdiplus::Point(selRc.right,selRc.top),Gdiplus::Point(selRc.right,selRc.bottom));
                            sp.AddLine(Gdiplus::Point(selRc.right,selRc.bottom),Gdiplus::Point(selRc.left,selRc.bottom));
                            sp.AddLine(Gdiplus::Point(selRc.left,selRc.bottom),Gdiplus::Point(selRc.left,selRc.top));
                        }else{
                            int dia = selR * 2;
                            sp.AddArc(selRc.left, selRc.top, dia, dia, 180, 90);
                            sp.AddArc(selRc.right - dia, selRc.top, dia, dia, 270, 90);
                            sp.AddArc(selRc.right - dia, selRc.bottom - dia, dia, dia, 0, 90);
                            sp.AddArc(selRc.left, selRc.bottom - dia, dia, dia, 90, 90);
                        }
                        sp.CloseFigure();
                        g.FillPath(&selBr,&sp);
                        SetTextColor(hdc,self->getBgColor());
                        TextOutW(hdc,x,cy,widx.c_str(),(int)widx.size());
                        TextOutW(hdc,x+szIdx.cx,cy,wtext.c_str(),(int)wtext.size());
                    }else{
                        SetTextColor(hdc,self->getIndexColor());
                        TextOutW(hdc,x,cy,widx.c_str(),(int)widx.size());
                        SetTextColor(hdc,self->getTextColor());
                        TextOutW(hdc,x+szIdx.cx,cy,wtext.c_str(),(int)wtext.size());
                    }
                    if(!vert) x += totalW + S(8);
                }
                // ── 齿轮按钮 ──
                bool showGear = self->m_pSettings ? self->m_pSettings->showSettingsGear : true;
                if (showGear) {
                    int pageY=y,pageX=x;
                    if(vert){pageY = candBaseY + (int)candidates.size() * self->m_rowH + S(2); pageX = S(8);}
                    SetTextColor(hdc,RGB(80,80,200)); std::wstring wgear=L"⚙";
                    SIZE gearSz; GetTextExtentPoint32W(hdc,wgear.c_str(),1,&gearSz);
                    int gearCY=pageY+(self->m_rowH-gearSz.cy)/2;
                    TextOutW(hdc,pageX,gearCY,wgear.c_str(),1);
                    self->m_settingsBtnRect = {pageX, gearCY, pageX + gearSz.cx + S(4), gearCY + gearSz.cy};
                } else {
                    self->m_settingsBtnRect={};
                }
                EndPaint(hwnd,&ps);
            }
            return 0;
        }
        if(msg==WM_LBUTTONDOWN){
            CandidateWindow* self=(CandidateWindow*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
            if(self){POINT pt={LOWORD(lp),HIWORD(lp)};
                bool showGear = self->m_pSettings ? self->m_pSettings->showSettingsGear : true;
                // ⚙ 齿轮按钮: 打开 PinyinIME.exe 设置窗口 (IPC)
                if(showGear && PtInRect(&self->m_settingsBtnRect,pt)){
                // DLL 加载到其他进程后工作目录可能不对, 必须用完整路径
                wchar_t exePath[MAX_PATH];
                GetModuleFileNameW(g_hDllInst, exePath, MAX_PATH);
                std::wstring dir(exePath);
                size_t pos = dir.find_last_of(L"\\/");
                if (pos == std::wstring::npos) return 0;
                dir = dir.substr(0, pos + 1);

                // 检查 EXE 是否已在运行
                HANDLE hMutex = OpenMutexW(SYNCHRONIZE, FALSE, PinyinIME_SINGLE_INSTANCE_MUTEX);
                if (hMutex) {
                    CloseHandle(hMutex);
                    // EXE 已在运行: 通过跨进程消息唤起设置窗口
                    static UINT s_msgOS = RegisterWindowMessageW(PinyinIME_MSG_OPEN_SETTINGS);
                    HWND hExe = FindWindowW(PinyinIME_MAIN_WINDOW_CLASS, L"PinyinIME");
                    if (hExe && s_msgOS) {
                        PostMessageW(hExe, s_msgOS, 0, 0);
                    }
                } else {
                    // EXE 未运行: 启动 (默认行为即打开设置窗口)
                    std::wstring settingsExe = dir + L"PinyinIME.exe";
                    ShellExecuteW(nullptr, L"open", settingsExe.c_str(),
                        nullptr, dir.c_str(), SW_SHOWNORMAL);
                }
            }}
            return 0;
        }
        if(msg==WM_MOUSEMOVE){
            CandidateWindow* self=(CandidateWindow*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
            if(self){
                POINT pt={LOWORD(lp),HIWORD(lp)};
                bool gearHov = PtInRect(&self->m_settingsBtnRect,pt);
                // 注册 WM_MOUSELEAVE 通知
                if(gearHov && !self->m_trackingMouse){
                    TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,hwnd,0};
                    TrackMouseEvent(&tme);
                    self->m_trackingMouse=true;
                }
            }
            return 0;
        }
        if(msg==WM_MOUSELEAVE){
            CandidateWindow* self=(CandidateWindow*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
            if(self){
                self->m_trackingMouse=false;
                InvalidateRect(hwnd,nullptr,TRUE);
            }
            return 0;
        }
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
};

// 全局指针, 在 ActivateEx 中设置 (定义在 dll_main.cpp)
extern PinyinEngine* g_pSharedEngine;
