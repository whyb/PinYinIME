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
    RECT m_prevBtnRect = {};
    RECT m_nextBtnRect = {};
    bool m_prevHovered = false;
    bool m_nextHovered = false;
    bool m_trackingMouse = false;
    int m_selectedIndex = 0;  // 当前选中的候选词索引 (相对当前页)
    int m_textY = 6;
    int m_rowH = 24;
    HRGN m_roundRgn = nullptr;
    int m_roundR = 10;
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
        HDC hdc = GetDC(m_hwnd);
        SelectObject(hdc, m_font);
        TEXTMETRICW tm; GetTextMetrics(hdc, &tm);

        int width = 12; SIZE sz;
        for (int i = 0; i < (int)candidates.size(); i++) {
            std::wstring wtext = utf8ToWide(std::to_string(i + 1) + "." + candidates[i].first + " ");
            GetTextExtentPoint32W(hdc, wtext.c_str(), (int)wtext.size(), &sz); width += sz.cx;
        }
        bool showBtns = m_pSettings ? m_pSettings->showPageButtons : true;
        bool showGear = m_pSettings ? m_pSettings->showSettingsGear : true;
        SIZE tmpSz;
        if (showBtns || showGear) {
            if (showBtns) {
                GetTextExtentPoint32W(hdc, L"◀", 1, &tmpSz);
                int btnW = tmpSz.cx + 12;
                width += btnW * 2 + 6;  // ◀ + gap + ▶
            }
            if (showGear) {
                GetTextExtentPoint32W(hdc, L"⚙", 1, &tmpSz);
                width += tmpSz.cx + 12;  // gear width + padding
            }
            if (showBtns && showGear) width += 8;  // gap between buttons and gear
            width += 12;  // end padding
        }
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

                {   // GDI+ 背景 + 渐变边框 (FillPath 同心层叠, 边框粗细均匀)
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
                    COLORREF bc=self->getBorderColor(), bgc=self->getBgColor();
                    int rr2=GetRValue(bc),rg2=GetGValue(bc),rb2=GetBValue(bc);
                    int bgb=(GetRValue(bgc)*299+GetGValue(bgc)*587+GetBValue(bgc)*114)/1000;
                    int dir=(bgb<128)?1:-1;
                    auto clmp=[](int v)->int{return v<0?0:(v>255?255:v);};
                    // 4层同心FillPath: 3层边框(每层1px) + 1层背景
                    for(int layer=0;layer<4;layer++){
                        int off=layer, lw=w-off*2, lh=h-off*2, lcr=cr-off;
                        if(lcr<2)lcr=2;
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
                int x=8, y=self->m_textY;
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
                        int selPadX=4, selPadY=1;
                        RECT selRc={x-selPadX, cy-selPadY, x+totalW+selPadX, cy+self->m_rowH+selPadY-2};
                        int selR=(std::max)(2,(std::min)(4,self->m_roundR/3));
                        Gdiplus::Graphics g(hdc);
                        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                        COLORREF tc=self->getTextColor();
                        Gdiplus::SolidBrush selBr(Gdiplus::Color(255, GetRValue(tc), GetGValue(tc), GetBValue(tc)));
                        Gdiplus::GraphicsPath sp;
                        sp.StartFigure();
                        int dia = selR * 2;
                        sp.AddArc(selRc.left, selRc.top, dia, dia, 180, 90);
                        sp.AddArc(selRc.right - dia, selRc.top, dia, dia, 270, 90);
                        sp.AddArc(selRc.right - dia, selRc.bottom - dia, dia, dia, 0, 90);
                        sp.AddArc(selRc.left, selRc.bottom - dia, dia, dia, 90, 90);
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
                    if(!vert) x+=totalW+8;
                }
                // ── 翻页按钮 + 齿轮 (根据设置条件显示) ──
                bool showBtns = self->m_pSettings ? self->m_pSettings->showPageButtons : true;
                bool showGear = self->m_pSettings ? self->m_pSettings->showSettingsGear : true;

                if (showBtns || showGear) {
                int pageY=y,pageX=x;
                if(vert){pageY=candBaseY+(int)candidates.size()*self->m_rowH+2;pageX=8;}

                SIZE btnSz; GetTextExtentPoint32W(hdc,L"◀",1,&btnSz);
                int btnPadX=6,btnPadY=2;
                int btnW=btnSz.cx+btnPadX*2, btnH=btnSz.cy+btnPadY*2;
                int btnRad=(std::max)(3,(std::min)(5,(int)(self->m_roundR/2)));

                // 计算总页数 & 启用状态
                int totalPages=1;
                if(g_pSharedEngine&&!g_pSharedEngine->m_candidates.empty()){
                    int ps=g_pSharedEngine->getPageSize();
                    totalPages=((int)g_pSharedEngine->m_candidates.size()+ps-1)/ps;
                }
                bool hasPrev=(g_pSharedEngine&&g_pSharedEngine->m_pageIndex>0);
                bool hasNext=(g_pSharedEngine&&g_pSharedEngine->m_pageIndex<totalPages-1);

                if (showBtns) {
                // 绘制单个翻页按钮 (圆角矩形 + 箭头文字), hovered 参数控制 hover 高亮
                auto drawBtn=[&](int bx,int by,const wchar_t* label,bool enabled,bool hovered)->RECT{
                    RECT br={bx,by,bx+btnW,by+btnH};
                    {   Gdiplus::Graphics g(hdc);
                        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                        COLORREF bg=self->getBgColor();
                        int r=GetRValue(bg),gr=GetGValue(bg),b=GetBValue(bg);
                        int bgBright=(r*299+gr*587+b*114)/1000;
                        int d1=bgBright>128?-24:24;
                        int d2=hovered?(bgBright>128?-20:20):0;
                        int delta=d1+d2;
                        auto clmp=[](int v)->int{return v<0?0:(v>255?255:v);};
                        COLORREF btnBg=RGB(clmp(r+delta),clmp(gr+delta),clmp(b+delta));
                        COLORREF bc=self->getBorderColor();
                        int bcr=GetRValue(bc),bcg=GetGValue(bc),bcb=GetBValue(bc);
                        if(!enabled){bcr=(bcr+192)/2;bcg=(bcg+192)/2;bcb=(bcb+192)/2;}
                        // FillPath 两层同心: 边框层 + 按钮底色
                        for(int lay=0;lay<2;lay++){
                            int off=lay, bx=br.left+off, by=br.top+off;
                            int bw=br.right-br.left-off*2, bh=br.bottom-br.top-off*2;
                            int rad=btnRad-off; if(rad<1)rad=1;
                            COLORREF col=(lay==0)?RGB(bcr,bcg,bcb):btnBg;
                            Gdiplus::SolidBrush br2(Gdiplus::Color(255, GetRValue(col), GetGValue(col), GetBValue(col)));
                            Gdiplus::GraphicsPath pt;
                            pt.StartFigure();
                            int dia = rad * 2;
                            pt.AddArc(bx, by, dia, dia, 180, 90);
                            pt.AddArc(bx + bw - dia, by, dia, dia, 270, 90);
                            pt.AddArc(bx + bw - dia, by + bh - dia, dia, dia, 0, 90);
                            pt.AddArc(bx, by + bh - dia, dia, dia, 90, 90);
                            pt.CloseFigure();
                            g.FillPath(&br2,&pt);
                        }
                    }
                    SetTextColor(hdc,enabled?self->getTextColor():RGB(180,180,180));
                    SetBkMode(hdc,TRANSPARENT);
                    DrawTextW(hdc,label,-1,&br,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                    return br;
                };

                self->m_prevBtnRect=drawBtn(pageX,pageY,L"◀",hasPrev,self->m_prevHovered); pageX+=btnW+6;
                self->m_nextBtnRect=drawBtn(pageX,pageY,L"▶",hasNext,self->m_nextHovered); pageX+=btnW+8;
                } else {
                    self->m_prevBtnRect={};
                    self->m_nextBtnRect={};
                }
                if (showGear) {
                SetTextColor(hdc,RGB(80,80,200)); std::wstring wgear=L"⚙";
                SIZE gearSz; GetTextExtentPoint32W(hdc,wgear.c_str(),1,&gearSz);
                int gearCY=pageY+(btnH-gearSz.cy)/2;
                TextOutW(hdc,pageX,gearCY,wgear.c_str(),1);
                self->m_settingsBtnRect={pageX,gearCY,pageX+gearSz.cx+4,gearCY+gearSz.cy};
                } else {
                    self->m_settingsBtnRect={};
                }
                } // end if (showBtns || showGear)
                EndPaint(hwnd,&ps);
            }
            return 0;
        }
        if(msg==WM_LBUTTONDOWN){
            CandidateWindow* self=(CandidateWindow*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
            if(self){POINT pt={LOWORD(lp),HIWORD(lp)};
                bool showBtns = self->m_pSettings ? self->m_pSettings->showPageButtons : true;
                bool showGear = self->m_pSettings ? self->m_pSettings->showSettingsGear : true;
                // ◀ 上一页按钮
                if(showBtns && PtInRect(&self->m_prevBtnRect,pt)){
                    if(g_pSharedEngine&&g_pSharedEngine->m_pageIndex>0){
                        g_pSharedEngine->prevPage();
                        self->show(g_pSharedEngine->getPageCandidates(),g_pSharedEngine->m_pageIndex);
                    }
                }
                // ▶ 下一页按钮
                else if(showBtns && PtInRect(&self->m_nextBtnRect,pt)){
                    if(g_pSharedEngine){
                        int ps=g_pSharedEngine->getPageSize();
                        int total=((int)g_pSharedEngine->m_candidates.size()+ps-1)/ps;
                        if(total<1)total=1;
                        if(g_pSharedEngine->m_pageIndex<total-1){
                            g_pSharedEngine->nextPage();
                            self->show(g_pSharedEngine->getPageCandidates(),g_pSharedEngine->m_pageIndex);
                        }
                    }
                }
                // ⚙ 齿轮按钮: 打开 PinyinIME.exe 设置窗口 (IPC)
                else if(showGear && PtInRect(&self->m_settingsBtnRect,pt)){
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
                bool prevHov=PtInRect(&self->m_prevBtnRect,pt);
                bool nextHov=PtInRect(&self->m_nextBtnRect,pt);
                if(prevHov!=self->m_prevHovered||nextHov!=self->m_nextHovered){
                    self->m_prevHovered=prevHov;
                    self->m_nextHovered=nextHov;
                    InvalidateRect(hwnd,nullptr,TRUE);
                }
                // 注册 WM_MOUSELEAVE 通知
                if((prevHov||nextHov)&&!self->m_trackingMouse){
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
                self->m_prevHovered=false;
                self->m_nextHovered=false;
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
