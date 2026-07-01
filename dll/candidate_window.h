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
#include "pinyin_engine.h"

extern HINSTANCE g_hDllInst;
extern PinyinEngine* g_pSharedEngine;  // 指向 CPinyinTextService::m_engine

class CandidateWindow {
public:
    HWND m_hwnd = nullptr;
    HFONT m_font = nullptr;
    bool m_visible = false;
    RECT m_settingsBtnRect = {};
    int m_textY = 6;
    int m_rowH = 24;
    HRGN m_roundRgn = nullptr;
    int m_roundR = 10;
    PinyinSettings* m_pSettings = nullptr;
    // TSF 标准: 在 edit session 内通过 ITfContextView::GetTextExt 拿到准确坐标,
    // 缓存后供 getCaretPosition 优先使用 (解决 Firefox 等不使用 Win32 caret 的应用)
    POINT m_tsfCaretPos = {0, 0};
    bool m_hasTsfCaretPos = false;

    COLORREF getBgColor()     { return m_pSettings ? m_pSettings->bgColor     : RGB(0xF0,0xF5,0xF0); }
    COLORREF getBorderColor() { return m_pSettings ? m_pSettings->borderColor : RGB(0x96,0xC6,0x96); }
    COLORREF getTextColor()   { return m_pSettings ? m_pSettings->textColor   : RGB(0x28,0x3D,0x28); }
    COLORREF getIndexColor()  { return m_pSettings ? m_pSettings->indexColor  : RGB(0x3C,0x81,0x3C); }
    COLORREF getInputColor()  { return m_pSettings ? m_pSettings->inputColor  : RGB(0x32,0x64,0x32); }

    void create(HINSTANCE hInst) {
        int fontSize = m_pSettings ? m_pSettings->fontSize : 20;
        m_font = CreateFontW(-fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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

        // 方法 0: TSF GetTextExt 缓存 (最符合 TSF 规范, 解决 Firefox/Chrome 等非 Win32 caret 应用)
        if (m_hasTsfCaretPos && (m_tsfCaretPos.x != 0 || m_tsfCaretPos.y != 0)) {
            pt = m_tsfCaretPos;
            found = true;
        }

        // 方法 1: Win32 标准光标 (同一线程, 最可靠)
        if (!found) {
            GUITHREADINFO gti = {};
            gti.cbSize = sizeof(gti);
            if (GetGUIThreadInfo(GetCurrentThreadId(), &gti)) {
                if (gti.hwndCaret && (gti.rcCaret.right > 0 || gti.rcCaret.bottom > 0)) {
                    POINT caretPt = {gti.rcCaret.left, gti.rcCaret.bottom};
                    if (ClientToScreen(gti.hwndCaret, &caretPt)) {
                        pt = caretPt;
                        found = true;
                    }
                }
                if (!found && gti.hwndFocus) {
                    // 备选: 获取焦点控件的屏幕位置
                    RECT rc;
                    if (GetWindowRect(gti.hwndFocus, &rc)) {
                        pt.x = rc.left + 4;
                        pt.y = rc.bottom + 4;
                        found = true;
                    }
                }
            }
        }

        // 方法 2: UI Automation (现代应用: Chrome/Edge/VSCode/UWP 等)
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

        // 方法 3: 前台窗口
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

        // 方法 4: 屏幕保底
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
        POINT pt = getCaretPosition();
        HDC hdc = GetDC(m_hwnd);
        SelectObject(hdc, m_font);
        TEXTMETRICW tm; GetTextMetrics(hdc, &tm);

        int width = 12; SIZE sz;
        for (int i = 0; i < (int)candidates.size(); i++) {
            std::wstring wtext = utf8ToWide(std::to_string(i + 1) + "." + candidates[i].first + " ");
            GetTextExtentPoint32W(hdc, wtext.c_str(), (int)wtext.size(), &sz); width += sz.cx;
        }
        std::wstring wpage = utf8ToWide(" -/=/PgUp/PgDn翻页  ⚙");
        GetTextExtentPoint32W(hdc, wpage.c_str(), (int)wpage.size(), &sz);
        width += sz.cx + 20;
        ReleaseDC(m_hwnd, hdc);

        RECT screen; SystemParametersInfoW(SPI_GETWORKAREA, 0, &screen, 0);
        int maxWidth = (screen.right - screen.left) * 85 / 100;
        if (width > maxWidth) width = maxWidth;

        int borderW = 3;
        m_roundR = (std::max)(6, (std::min)(16, (int)(tm.tmHeight * 2 / 3)));
        bool vert = m_pSettings ? m_pSettings->verticalLayout : false;
        int height;
        if (vert && !candidates.empty()) {
            if (250 > maxWidth) width = maxWidth;
            m_rowH = tm.tmHeight + 6;
            m_textY = borderW + 4;
            height = m_textY + ((int)candidates.size() + 1) * m_rowH + borderW + 6;
        } else {
            int pad = (std::max)(4, (int)(tm.tmHeight / 8));
            m_textY = borderW + pad;
            height = m_textY + tm.tmHeight + pad + borderW;
            m_rowH = tm.tmHeight;
        }

        if (m_roundRgn) { DeleteObject(m_roundRgn); m_roundRgn = nullptr; }
        m_roundRgn = CreateRoundRectRgn(0, 0, width + 1, height + 1, m_roundR * 2, m_roundR * 2);
        SetWindowRgn(m_hwnd, m_roundRgn, TRUE);

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

                {   // GDI+ 背景 + 渐变边框
                    Gdiplus::Graphics graphics(hdc);
                    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    int w = rc.right, h = rc.bottom, cr = self->m_roundR;
                    auto makeRR = [](Gdiplus::GraphicsPath& p, int x, int y, int rw, int rh, int rad) {
                        p.Reset(); p.StartFigure();
                        p.AddArc(x,y,rad*2,rad*2,180,90); p.AddArc(x+rw-rad*2,y,rad*2,rad*2,270,90);
                        p.AddArc(x+rw-rad*2,y+rh-rad*2,rad*2,rad*2,0,90); p.AddArc(x,y+rh-rad*2,rad*2,rad*2,90,90);
                        p.CloseFigure();
                    };
                    Gdiplus::GraphicsPath bgPath;
                    { COLORREF c = self->getBgColor(); Gdiplus::SolidBrush bgBrush(Gdiplus::Color(GetRValue(c),GetGValue(c),GetBValue(c)));
                      makeRR(bgPath,0,0,w,h,cr); graphics.FillPath(&bgBrush,&bgPath); }
                    COLORREF bc=self->getBorderColor(), bgc=self->getBgColor();
                    int rr=GetRValue(bc),rg=GetGValue(bc),rb=GetBValue(bc);
                    int bgBright=(GetRValue(bgc)*299+GetGValue(bgc)*587+GetBValue(bgc)*114)/1000;
                    int dir=(bgBright<128)?1:-1;
                    auto clampC=[](int v)->int{return v<0?0:(v>255?255:v);};
                    Gdiplus::GraphicsPath borderPath;
                    for(int layer=0;layer<3;layer++){
                        int off=layer,delta=(layer==0)?40:(layer==1)?18:0;
                        int lw=w-off*2,lh=h-off*2,lcr=cr-off; if(lcr<2)lcr=2;
                        Gdiplus::Color penColor(clampC(rr+delta*dir),clampC(rg+delta*dir),clampC(rb+delta*dir));
                        Gdiplus::Pen pen(penColor,1.0f);
                        makeRR(borderPath,off,off,lw,lh,lcr); graphics.DrawPath(&pen,&borderPath);
                    }
                }

                SelectObject(hdc, self->m_font);
                SetBkMode(hdc, TRANSPARENT);
                int x=8, y=self->m_textY;
                auto candidates = g_pSharedEngine->getPageCandidates();
                int candBaseY=y; SIZE sz;
                bool vert = self->m_pSettings ? self->m_pSettings->verticalLayout : false;
                for(int i=0;i<(int)candidates.size();i++){
                    int cy = vert ? (candBaseY+i*self->m_rowH) : y;
                    SetTextColor(hdc,self->getIndexColor());
                    std::wstring widx=std::to_wstring(i+1)+L".";
                    TextOutW(hdc,x,cy,widx.c_str(),(int)widx.size());
                    GetTextExtentPoint32W(hdc,widx.c_str(),(int)widx.size(),&sz); int cw=sz.cx;
                    SetTextColor(hdc,self->getTextColor());
                    std::wstring wtext=utf8ToWide(candidates[i].first);
                    TextOutW(hdc,x+cw,cy,wtext.c_str(),(int)wtext.size());
                    GetTextExtentPoint32W(hdc,wtext.c_str(),(int)wtext.size(),&sz);
                    if(!vert) x+=cw+sz.cx+8;
                }
                SetTextColor(hdc,RGB(150,150,150));
                std::wstring wpage=L" -/=/PgUp/PgDn翻页";
                int pageY=y,pageX=x;
                if(vert){pageY=candBaseY+(int)candidates.size()*self->m_rowH+2;pageX=8;}
                TextOutW(hdc,pageX,pageY,wpage.c_str(),(int)wpage.size());
                GetTextExtentPoint32W(hdc,wpage.c_str(),(int)wpage.size(),&sz);
                SetTextColor(hdc,RGB(80,80,200)); std::wstring wgear=L"⚙";
                int gearX=pageX+sz.cx+4; TextOutW(hdc,gearX,pageY,wgear.c_str(),(int)wgear.size());
                SIZE gearSz; GetTextExtentPoint32W(hdc,wgear.c_str(),(int)wgear.size(),&gearSz);
                self->m_settingsBtnRect={gearX,pageY,gearX+gearSz.cx+4,pageY+gearSz.cy};
                EndPaint(hwnd,&ps);
            }
            return 0;
        }
        if(msg==WM_LBUTTONDOWN){
            CandidateWindow* self=(CandidateWindow*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
            if(self){POINT pt={LOWORD(lp),HIWORD(lp)}; if(PtInRect(&self->m_settingsBtnRect,pt)){
                // 齿轮按钮: 启动 PinyinIME.exe 设置窗口
                // DLL 加载到其他进程后工作目录可能不对, 必须用完整路径
                wchar_t exePath[MAX_PATH];
                GetModuleFileNameW(g_hDllInst, exePath, MAX_PATH);
                std::wstring dir(exePath);
                size_t pos = dir.find_last_of(L"\\/");
                if (pos != std::wstring::npos) {
                    dir = dir.substr(0, pos + 1);
                    std::wstring settingsExe = dir + L"PinyinIME.exe";
                    ShellExecuteW(nullptr, L"open", settingsExe.c_str(),
                        nullptr, dir.c_str(), SW_SHOWNORMAL);
                }
            }}
            return 0;
        }
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
};

// 全局指针, 在 ActivateEx 中设置 (定义在 dll_main.cpp)
extern PinyinEngine* g_pSharedEngine;
