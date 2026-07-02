// exe/settings_window.h — SettingsWindow + 辅助类 (从 settings.h 提取到 EXE)
// 修改: "注册到系统" 按钮改为调用 doFullRegistration (TSF 方式)
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>
#pragma comment(lib, "comctl32.lib")
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include "../shared/pinyin_settings.h"
#include "../shared/utf_utils.h"
#include "registration.h"

// ==================== 前向声明 ====================
extern PinyinSettings g_settings;

// ==================== 系统字体枚举 ====================
inline std::vector<std::wstring> enumSystemFonts() {
    std::vector<std::wstring> fonts;
    HDC hdc = GetDC(nullptr);
    if (!hdc) return fonts;
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(hdc, &lf,
        [](const LOGFONTW* pLf, const TEXTMETRICW*, DWORD fontType, LPARAM lParam) -> int {
            auto* pFonts = reinterpret_cast<std::vector<std::wstring>*>(lParam);
            if (fontType & TRUETYPE_FONTTYPE) {
                std::wstring name = pLf->lfFaceName;
                if (std::find(pFonts->begin(), pFonts->end(), name) == pFonts->end())
                    pFonts->push_back(name);
            }
            return 1;
        }, (LPARAM)&fonts, 0);
    ReleaseDC(nullptr, hdc);
    std::sort(fonts.begin(), fonts.end());
    return fonts;
}

// ==================== 设置窗口 ====================
struct SettingsWindow {
    HWND m_hDlg = nullptr;
    HWND m_hSkinPreview = nullptr;
    int m_selectedSkin = 8;  // 默认 亮色 墨绿
    PinyinSettings m_temp;
    HFONT m_hFont = nullptr;
    HBRUSH m_hBgBrush = nullptr;
    float m_dpiScale = 1.0f;
    int m_roundR = 10;          // 窗口圆角半径 (从字体度量计算)
    RECT m_closeBtnRect = {};   // 自绘关闭按钮区域

    // 皮肤预览子窗口过程
    static LRESULT CALLBACK skinPreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        SettingsWindow* self = (SettingsWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (!self && msg == WM_CREATE) {
            self = (SettingsWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        }
        // 当窗口大小改变或创建时，实时裁剪窗口区域为圆角，从根本上杜绝白边
        if ((msg == WM_SIZE || msg == WM_CREATE) && self) {
            RECT rc; GetClientRect(hwnd, &rc);
            auto S = [self](int v) -> int { return (int)(v * self->m_dpiScale + 0.5f); };
            int fs = self->m_temp.fontSize; if (fs < 12) fs = 12; if (fs > 36) fs = 36;
            HFONT hFont = CreateFontW(-(int)(fs * self->m_dpiScale + 0.5f), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, self->m_temp.fontName.c_str());
            HDC hdc = GetDC(hwnd);
            HGDIOBJ old = SelectObject(hdc, hFont);
            TEXTMETRICW tm; GetTextMetrics(hdc, &tm);
            SelectObject(hdc, old); DeleteObject(hFont);
            ReleaseDC(hwnd, hdc);
            int cr = (std::max)(6, (std::min)(12, (int)(tm.tmHeight * 2 / 3)));
            if (self->m_temp.roundedCorner) {
                HRGN hRgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, cr * 2, cr * 2);
                SetWindowRgn(hwnd, hRgn, TRUE);
            } else {
                SetWindowRgn(hwnd, nullptr, TRUE);
            }
        }

        if (msg == WM_PAINT && self) {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            auto S = [self](int v) -> int { return (int)(v * self->m_dpiScale + 0.5f); };
            int fs=self->m_temp.fontSize;if(fs<12)fs=12;if(fs>36)fs=36;
            HFONT hFont = CreateFontW(-(int)(fs*self->m_dpiScale+0.5f),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
                DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,self->m_temp.fontName.c_str());
            SelectObject(hdc,hFont); SetBkMode(hdc,TRANSPARENT);
            TEXTMETRICW tm; GetTextMetrics(hdc,&tm);
            {   // GDI+ 背景 + 渐变边框 (FillPath 同心层叠, 窗口已被 SetWindowRgn 裁剪干净)
                Gdiplus::Graphics graphics(hdc);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                int w=rc.right,h=rc.bottom,cr=(std::max)(6,(std::min)(12,(int)(tm.tmHeight*2/3)));
                if(!self->m_temp.roundedCorner) cr=0;
                auto makeRR=[](Gdiplus::GraphicsPath& p,int x,int y,int rw,int rh,int rad){
                    p.Reset();p.StartFigure();
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
                COLORREF bc=self->m_temp.borderColor,bgc=self->m_temp.bgColor;
                int rr=GetRValue(bc),rg=GetGValue(bc),rb=GetBValue(bc);
                int bgBright=(GetRValue(bgc)*299+GetGValue(bgc)*587+GetBValue(bgc)*114)/1000;
                int dir=(bgBright<128)?1:-1;
                auto clampC=[](int v)->int{return v<0?0:(v>255?255:v);};
                for(int layer=0;layer<4;layer++){
                    int off=layer,lw=w-off*2,lh=h-off*2,lcr=cr-off;
                    if(cr>0 && lcr<2) lcr=2;
                    int delta=(layer==0)?40:(layer==1)?18:0;
                    COLORREF col=(layer<3)?RGB(clampC(rr+delta*dir),clampC(rg+delta*dir),clampC(rb+delta*dir)):bgc;
                    Gdiplus::SolidBrush br(Gdiplus::Color(255, GetRValue(col), GetGValue(col), GetBValue(col)));
                    Gdiplus::GraphicsPath pth;
                    makeRR(pth,off,off,lw,lh,lcr);
                    graphics.FillPath(&br,&pth);
                }
            }
            int borderW=3,pad=(std::max)(4,(int)(tm.tmHeight/8)),textY=borderW+pad,x=S(8);
            SIZE sz;
            // ── 候选词 (候选框只展示候选, 拼音通过 TSF composition 内联到光标处) ──
            // 第一个候选默认选中, 反色显示
            {
                SIZE sz1,sz2;
                GetTextExtentPoint32W(hdc,L"1.",2,&sz1);
                GetTextExtentPoint32W(hdc,L"你好",2,&sz2);
                int totalW=sz1.cx+sz2.cx, selPad=4;
                int selR=self->m_temp.roundedCorner?(std::max)(2,(std::min)(4,(int)(tm.tmHeight/5))):0;
                RECT selRc={x-selPad, textY-1, x+totalW+selPad, textY+tm.tmHeight-1};
                {
                    Gdiplus::Graphics g(hdc);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                    COLORREF tc=self->m_temp.textColor;
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
                }
                SetTextColor(hdc,self->m_temp.bgColor);
                TextOutW(hdc,x,textY,L"1.",2);
                TextOutW(hdc,x+sz1.cx,textY,L"你好",2);
                x+=totalW+S(8);
            }
            SetTextColor(hdc,self->m_temp.indexColor);TextOutW(hdc,x,textY,L"2.",2);
            GetTextExtentPoint32W(hdc,L"2.",2,&sz);x+=sz.cx;
            SetTextColor(hdc,self->m_temp.textColor);TextOutW(hdc,x,textY,L"世界",2);
            GetTextExtentPoint32W(hdc,L"世界",2,&sz);x+=sz.cx+S(8);
            SetTextColor(hdc,self->m_temp.indexColor);TextOutW(hdc,x,textY,L"3.",2);
            GetTextExtentPoint32W(hdc,L"3.",2,&sz);x+=sz.cx;
            SetTextColor(hdc,self->m_temp.textColor);TextOutW(hdc,x,textY,L"你好",2);
            GetTextExtentPoint32W(hdc,L"你好",2,&sz);x+=sz.cx+S(4);
            // 齿轮图标 (受显示设置控制)
            if (self->m_temp.showSettingsGear) {
            SetTextColor(hdc,RGB(80,80,200));
            TextOutW(hdc,x,textY,L"⚙",1);
            }
            DeleteObject(hFont);EndPaint(hwnd,&ps);
            return 0;
        }
        return DefWindowProcW(hwnd,msg,wp,lp);
    }

    void updateBgBrush() {
        if(m_hBgBrush){DeleteObject(m_hBgBrush);m_hBgBrush=nullptr;}
        m_hBgBrush=CreateSolidBrush(m_temp.bgColor);
    }

    HWND addLabel(const wchar_t* text,int x,int y,int w,int h){
        HWND ctrl=CreateWindowExW(0,L"STATIC",text,WS_CHILD|WS_VISIBLE,x,y,w,h,m_hDlg,nullptr,
            (HINSTANCE)GetWindowLongPtrW(m_hDlg,GWLP_HINSTANCE),nullptr);
        if(ctrl&&m_hFont)SendMessageW(ctrl,WM_SETFONT,(WPARAM)m_hFont,TRUE);
        return ctrl;
    }
    HWND addCheck(const wchar_t* text,int id,int x,int y,int w,int h,bool checked){
        HWND ctrl=CreateWindowExW(0,L"BUTTON",text,WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,x,y,w,h,m_hDlg,
            (HMENU)(UINT_PTR)id,(HINSTANCE)GetWindowLongPtrW(m_hDlg,GWLP_HINSTANCE),nullptr);
        if(ctrl){if(m_hFont)SendMessageW(ctrl,WM_SETFONT,(WPARAM)m_hFont,TRUE);
            SendMessageW(ctrl,BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);}
        return ctrl;
    }
    HWND addButton(const wchar_t* text,int id,int x,int y,int w,int h){
        HWND ctrl=CreateWindowExW(0,L"BUTTON",text,WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,x,y,w,h,m_hDlg,
            (HMENU)(UINT_PTR)id,(HINSTANCE)GetWindowLongPtrW(m_hDlg,GWLP_HINSTANCE),nullptr);
        if(ctrl&&m_hFont)SendMessageW(ctrl,WM_SETFONT,(WPARAM)m_hFont,TRUE);
        return ctrl;
    }
    HWND addEdit(int id,int x,int y,int w,int h,const wchar_t* text){
        HWND ctrl=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",text,WS_CHILD|WS_VISIBLE|ES_LEFT|ES_AUTOHSCROLL,
            x,y,w,h,m_hDlg,(HMENU)(UINT_PTR)id,(HINSTANCE)GetWindowLongPtrW(m_hDlg,GWLP_HINSTANCE),nullptr);
        if(ctrl&&m_hFont)SendMessageW(ctrl,WM_SETFONT,(WPARAM)m_hFont,TRUE);
        return ctrl;
    }
    HWND addCombo(int id,int x,int y,int w,int h){
        HWND ctrl=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            x,y,w,h,m_hDlg,(HMENU)(UINT_PTR)id,(HINSTANCE)GetWindowLongPtrW(m_hDlg,GWLP_HINSTANCE),nullptr);
        if(ctrl&&m_hFont)SendMessageW(ctrl,WM_SETFONT,(WPARAM)m_hFont,TRUE);
        return ctrl;
    }

    // ==================== 预览自动大小调整 ====================
    // 用与 CandidateWindow::show() 相同的逻辑计算预览宽高
    void resizePreview() {
        if (!m_hSkinPreview) return;
        HDC hdc = GetDC(m_hSkinPreview);
        auto S = [this](int v) -> int { return (int)(v * m_dpiScale + 0.5f); };
        int fs = m_temp.fontSize; if (fs < 12) fs = 12; if (fs > 36) fs = 36;
        HFONT hFont = CreateFontW(-(int)(fs * m_dpiScale + 0.5f), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
            m_temp.fontName.c_str());
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
        TEXTMETRICW tm; GetTextMetrics(hdc, &tm);
        int borderW = 3, pad = (std::max)(4, (int)(tm.tmHeight / 8));
        int textY = borderW + pad, height = textY + tm.tmHeight + pad + borderW;
        int x = S(8); SIZE sz;
        struct { const wchar_t* idx; const wchar_t* text; } cands[] = {
            {L"1.", L"你好"}, {L"2.", L"世界"}, {L"3.", L"你好"}
        };
        for (auto& c : cands) {
            SIZE szi, szt;
            GetTextExtentPoint32W(hdc, c.idx, (int)wcslen(c.idx), &szi);
            GetTextExtentPoint32W(hdc, c.text, (int)wcslen(c.text), &szt);
            x += szi.cx + szt.cx + S(8);
        }
        if (m_temp.showSettingsGear) {
            SIZE gearSz; GetTextExtentPoint32W(hdc, L"⚙", 1, &gearSz);
            x += gearSz.cx + S(12);
        }
        x += S(8);
        int width = x, maxW = S(510);
        if (width > maxW) width = maxW;
        SelectObject(hdc, hOldFont); DeleteObject(hFont); ReleaseDC(m_hSkinPreview, hdc);
        SetWindowPos(m_hSkinPreview, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);
        // 更新预览窗口圆角区域
        if (m_temp.roundedCorner) {
            int cr = (std::max)(6, (std::min)(12, (int)(tm.tmHeight * 2 / 3)));
            HRGN hRgn = CreateRoundRectRgn(0, 0, width + 1, height + 1, cr * 2, cr * 2);
            SetWindowRgn(m_hSkinPreview, hRgn, TRUE);
        } else {
            SetWindowRgn(m_hSkinPreview, nullptr, TRUE);
        }
        InvalidateRect(m_hSkinPreview, nullptr, TRUE);
    }

    void buildUI(){
        HINSTANCE hInst=(HINSTANCE)GetWindowLongPtrW(m_hDlg,GWLP_HINSTANCE);
        auto S=[this](int v)->int{return (int)(v*m_dpiScale+0.5f);};
        if(!m_hFont){m_hFont=CreateFontW(-S(16),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,m_temp.fontName.c_str());}

        // 标题在 WM_PAINT 中自绘, 内容向下偏移让出标题栏空间
        int gy=S(36);
        addLabel(L"━━ 基本设置 ━━",S(15),gy,S(200),S(18));gy+=S(22);
        addCheck(L"繁体中文模式",401,S(20),gy,S(140),S(20),m_temp.useTraditional);
        addCheck(L"竖排候选框",404,S(170),gy,S(120),S(20),m_temp.verticalLayout);
        addCheck(L"中文标点符号（，。）",405,S(300),gy,S(180),S(20),m_temp.chinesePunctuation);gy+=S(24);
        addLabel(L"候选词数量:",S(20),gy,S(100),S(20));
        HWND hCandCombo=addCombo(402,S(115),gy-S(2),S(55),S(200));
        for(int i=3;i<=9;i++){std::wstring s=std::to_wstring(i);SendMessageW(hCandCombo,CB_ADDSTRING,0,(LPARAM)s.c_str());}
        int ccIdx=m_temp.candidateCount-3;if(ccIdx<0)ccIdx=0;if(ccIdx>6)ccIdx=6;
        SendMessageW(hCandCombo,CB_SETCURSEL,ccIdx,0);
        addLabel(L"字体大小:",S(200),gy,S(80),S(20));
        HWND hFsCombo=addCombo(403,S(280),gy-S(2),S(55),S(300));
        for(int i=12;i<=36;i++){std::wstring s=std::to_wstring(i);SendMessageW(hFsCombo,CB_ADDSTRING,0,(LPARAM)s.c_str());}
        int fsIdx=m_temp.fontSize-12;if(fsIdx<0)fsIdx=0;if(fsIdx>24)fsIdx=24;
        SendMessageW(hFsCombo,CB_SETCURSEL,fsIdx,0);gy+=S(28);
        addLabel(L"选择字体:",S(20),gy,S(80),S(20));
        HWND hFontCombo=addCombo(408,S(105),gy-S(2),S(280),S(300));
        {auto fonts=enumSystemFonts();for(const auto& f:fonts)SendMessageW(hFontCombo,CB_ADDSTRING,0,(LPARAM)f.c_str());
         int fi=(int)SendMessageW(hFontCombo,CB_FINDSTRINGEXACT,-1,(LPARAM)m_temp.fontName.c_str());
         if(fi>=0)SendMessageW(hFontCombo,CB_SETCURSEL,fi,0);}gy+=S(28);

        addLabel(L"━━ 外观配色 ━━",S(15),gy,S(200),S(18));gy+=S(22);
        addLabel(L"预设皮肤:",S(20),gy,S(80),S(20));
        HWND hSkin=addCombo(701,S(100),gy-S(2),S(140),S(200));
        for(int i=0;i<PinyinSettings::SKIN_COUNT;i++)SendMessageW(hSkin,CB_ADDSTRING,0,(LPARAM)PinyinSettings::skins[i].name);
        SendMessageW(hSkin,CB_SETCURSEL,m_selectedSkin,0);
        addButton(L"🎨 自定义主色调",702,S(255),gy-S(2),S(140),S(22));gy+=S(28);

        addLabel(L"候选框预览:",S(20),gy,S(80),S(20));
        WNDCLASSEXW wc={};wc.cbSize=sizeof(wc);wc.lpfnWndProc=skinPreviewProc;wc.hInstance=hInst;
        wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
        wc.lpszClassName=L"PinyinSkinPreview2";RegisterClassExW(&wc);
        m_hSkinPreview=CreateWindowExW(0,L"PinyinSkinPreview2",L"",WS_CHILD|WS_VISIBLE,
            S(20),gy,S(460),S(48),m_hDlg,nullptr,hInst,this);
        resizePreview();
        RECT prc; GetWindowRect(m_hSkinPreview,&prc);
        int prevH=prc.bottom-prc.top;
        gy+=(std::max)(S(48),prevH)+S(6);

        addCheck(L"显示设置齿轮",924,S(20),gy,S(140),S(20),m_temp.showSettingsGear);
        addCheck(L"圆角候选框",926,S(175),gy,S(140),S(20),m_temp.roundedCorner);gy+=S(24);

        addButton(L"📝 管理用户词典...",801,S(20),gy,S(170),S(26));gy+=S(32);

        addLabel(L"━━ 模糊音设置 ━━",S(15),gy,S(200),S(18));gy+=S(22);
        addCheck(L"z/zh 不分",501,S(20),gy,S(95),S(20),m_temp.fuzzyZ_Zh);
        addCheck(L"c/ch 不分",502,S(125),gy,S(95),S(20),m_temp.fuzzyC_Ch);
        addCheck(L"s/sh 不分",503,S(230),gy,S(95),S(20),m_temp.fuzzyS_Sh);
        addCheck(L"n/l 不分",504,S(335),gy,S(90),S(20),m_temp.fuzzyN_L);gy+=S(22);
        addCheck(L"f/h 不分",505,S(20),gy,S(95),S(20),m_temp.fuzzyF_H);
        addCheck(L"en/eng 不分",506,S(125),gy,S(110),S(20),m_temp.fuzzyEn_Eng);
        addCheck(L"in/ing 不分",507,S(240),gy,S(110),S(20),m_temp.fuzzyIn_Ing);gy+=S(28);

        addLabel(L"━━ 智能功能 ━━",S(15),gy,S(200),S(18));gy+=S(22);
        addCheck(L"智能拼音纠错",601,S(20),gy,S(140),S(20),m_temp.smartCorrection);
        addCheck(L"自动加入新词到用户词典",602,S(170),gy,S(190),S(20),m_temp.autoWordCreate);
        addCheck(L"词频自动调整",603,S(370),gy,S(140),S(20),m_temp.autoFreqAdjust);gy+=S(30);

        addLabel(L"━━ 候选翻页 ━━",S(15),gy,S(200),S(18));gy+=S(22);
        addCheck(L"启用 -/= 翻页",920,S(20),gy,S(145),S(20),m_temp.enableMinusEqualsPage);
        addCheck(L"启用 ,/. 翻页",921,S(175),gy,S(145),S(20),m_temp.enableCommaPeriodPage);
        addCheck(L"启用 Tab 翻页",922,S(330),gy,S(155),S(20),m_temp.enableTabPage);gy+=S(22);
        addCheck(L"启用 [/] 翻页",923,S(20),gy,S(145),S(20),m_temp.enableBracketPage);
        addLabel(L"切换中/英热键: 左Shift",S(175),gy,S(350),S(20));gy+=S(28);

        // TSF 注册 / 卸载按钮
        addButton(L"📌 注册输入法到系统 (TSF)",910,S(20),gy,S(215),S(26));
        addButton(L"🗑 从系统卸载此输入法",911,S(245),gy,S(215),S(26));gy+=S(32);

        addButton(L"✅ 保存设置",901,S(250),gy,S(100),S(28));
        addButton(L"❌ 取消",902,S(360),gy,S(80),S(28));

        int winW=S(530),winH=gy+S(52);
        SetWindowPos(m_hDlg,nullptr,0,0,winW,winH,SWP_NOMOVE|SWP_NOZORDER);

        // 计算圆角半径并设置窗口裁剪区域
        {
            HDC hdc=GetDC(m_hDlg);
            HFONT hOld=(HFONT)SelectObject(hdc,m_hFont);
            TEXTMETRICW tm;GetTextMetrics(hdc,&tm);
            SelectObject(hdc,hOld);
            ReleaseDC(m_hDlg,hdc);
            m_roundR=(std::max)(6,(std::min)(12,(int)(tm.tmHeight*2/3)));
        }
        HRGN rgn=CreateRoundRectRgn(0,0,winW+1,winH+1,m_roundR*2,m_roundR*2);
        SetWindowRgn(m_hDlg,rgn,TRUE);
        // rgn 所有权已转移给系统, 不可再删除
    }

    static LRESULT CALLBACK dlgProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
        SettingsWindow* self=(SettingsWindow*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
        switch(msg){
        case WM_CREATE:{
            self=(SettingsWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)self);
            self->m_hDlg=hwnd;self->buildUI();return 0;}
        case WM_NCHITTEST:{
            // 自绘标题栏拖拽支持 (关闭按钮区域除外, 否则 WM_LBUTTONDOWN 收不到点击)
            if(!self)break;
            POINT pt={LOWORD(lp),HIWORD(lp)};
            ScreenToClient(hwnd,&pt);
            // 关闭按钮优先 — 返回 HTCLIENT 让 WM_LBUTTONDOWN 正常触发
            if(PtInRect(&self->m_closeBtnRect,pt))return HTCLIENT;
            auto S=[self](int v)->int{return (int)(v*self->m_dpiScale+0.5f);};
            RECT titleRc={0,0,S(530),S(32)};
            if(PtInRect(&titleRc,pt))return HTCAPTION;
            break;}
        case WM_LBUTTONDOWN:{
            if(!self)break;
            POINT pt={LOWORD(lp),HIWORD(lp)};
            if(PtInRect(&self->m_closeBtnRect,pt)){DestroyWindow(hwnd);return 0;}
            break;}
        case WM_ERASEBKGND:{
            return TRUE;}  // WM_PAINT 负责全部背景绘制
        case WM_PAINT:{
            if(!self)break;
            PAINTSTRUCT ps;HDC hdc=BeginPaint(hwnd,&ps);
            RECT rc;GetClientRect(hwnd,&rc);
            {   // GDI+ 背景 + 渐变边框 (FillPath 同心层叠, 窗口已被 SetWindowRgn 裁剪干净)
                Gdiplus::Graphics graphics(hdc);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                int w=rc.right,h=rc.bottom,cr=self->m_roundR;
                auto makeRR=[](Gdiplus::GraphicsPath& p,int x,int y,int rw,int rh,int rad){
                    p.Reset();p.StartFigure();
                    int dia = rad * 2;
                    p.AddArc(x, y, dia, dia, 180, 90);
                    p.AddArc(x + rw - dia, y, dia, dia, 270, 90);
                    p.AddArc(x + rw - dia, y + rh - dia, dia, dia, 0, 90);
                    p.AddArc(x, y + rh - dia, dia, dia, 90, 90);
                    p.CloseFigure();
                };
                COLORREF bc=self->m_temp.borderColor,bgc=self->m_temp.bgColor;
                int rr=GetRValue(bc),rg=GetGValue(bc),rb=GetBValue(bc);
                int bgBright=(GetRValue(bgc)*299+GetGValue(bgc)*587+GetBValue(bgc)*114)/1000;
                int dir=(bgBright<128)?1:-1;
                auto clampC=[](int v)->int{return v<0?0:(v>255?255:v);};
                for(int layer=0;layer<4;layer++){
                    int off=layer,lw=w-off*2,lh=h-off*2,lcr=cr-off;if(lcr<2)lcr=2;
                    int delta=(layer==0)?40:(layer==1)?18:0;
                    COLORREF col=(layer<3)?RGB(clampC(rr+delta*dir),clampC(rg+delta*dir),clampC(rb+delta*dir)):bgc;
                    Gdiplus::SolidBrush br(Gdiplus::Color(255, GetRValue(col), GetGValue(col), GetBValue(col)));
                    Gdiplus::GraphicsPath pth;
                    makeRR(pth,off,off,lw,lh,lcr);
                    graphics.FillPath(&br,&pth);
                }
            }
            // ── 自绘标题栏 ──
            {
                auto S=[self](int v)->int{return (int)(v*self->m_dpiScale+0.5f);};
                int titleH=S(32), sepY=titleH;
                // 标题文字
                SelectObject(hdc,self->m_hFont);
                SetBkMode(hdc,TRANSPARENT);
                SetTextColor(hdc,self->m_temp.textColor);
                RECT titleRc={S(15),0,rc.right-S(40),titleH};
                DrawTextW(hdc,L"PinyinIME 设置",-1,&titleRc,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                // 关闭按钮 ✕
                int btnSize=S(22), btnX=rc.right-S(30), btnY=(titleH-btnSize)/2;
                self->m_closeBtnRect={btnX,btnY,btnX+btnSize,btnY+btnSize};
                {
                    Gdiplus::Graphics g(hdc);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                    RECT br=self->m_closeBtnRect;
                    int r2=3;
                    COLORREF bg=self->m_temp.bgColor;
                    int rr=GetRValue(bg),gg=GetGValue(bg),bb=GetBValue(bg);
                    int br2=(rr*299+gg*587+bb*114)/1000;
                    int delta=br2>128?-18:18;
                    auto clmp=[](int v)->int{return v<0?0:(v>255?255:v);};
                    Gdiplus::SolidBrush fb(Gdiplus::Color(255, clmp(rr+delta), clmp(gg+delta), clmp(bb+delta)));
                    Gdiplus::GraphicsPath cp;
                    cp.StartFigure();
                    int dia = r2 * 2;
                    cp.AddArc(br.left, br.top, dia, dia, 180, 90);
                    cp.AddArc(br.right - dia, br.top, dia, dia, 270, 90);
                    cp.AddArc(br.right - dia, br.bottom - dia, dia, dia, 0, 90);
                    cp.AddArc(br.left, br.bottom - dia, dia, dia, 90, 90);
                    cp.CloseFigure();
                    g.FillPath(&fb,&cp);
                }
                SetTextColor(hdc,self->m_temp.textColor);
                RECT cr=self->m_closeBtnRect;
                DrawTextW(hdc,L"✕",-1,&cr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                // 分隔线
                SetTextColor(hdc,self->m_temp.borderColor);
                for(int i=0;i<S(530);i+=2){
                    int bright=(GetRValue(self->m_temp.bgColor)*299+GetGValue(self->m_temp.bgColor)*587+GetBValue(self->m_temp.bgColor)*114)/1000;
                    int alpha=bright>128?40:40;
                    // simplified: draw dotted line with border color
                }
                // 用 GDI+ 画分隔线
                {
                    Gdiplus::Graphics g(hdc);
                    int br2=(GetRValue(self->m_temp.bgColor)*299+GetGValue(self->m_temp.bgColor)*587+GetBValue(self->m_temp.bgColor)*114)/1000;
                    int alpha=br2>128?60:60;
                    Gdiplus::Color sepColor(alpha,GetRValue(self->m_temp.borderColor),GetGValue(self->m_temp.borderColor),GetBValue(self->m_temp.borderColor));
                    Gdiplus::Pen sepPen(sepColor,1.0f);
                    g.DrawLine(&sepPen,S(10),sepY,S(520),sepY);
                }
            }
            EndPaint(hwnd,&ps);
            return 0;}
        case WM_SIZE:{
            if(!self)break;
            RECT wrc;GetWindowRect(hwnd,&wrc);
            int ww=wrc.right-wrc.left,wh=wrc.bottom-wrc.top;
            HRGN rgn=CreateRoundRectRgn(0,0,ww+1,wh+1,self->m_roundR*2,self->m_roundR*2);
            SetWindowRgn(hwnd,rgn,TRUE);
            InvalidateRect(hwnd,nullptr,TRUE);
            return 0;}
        case WM_CTLCOLORSTATIC:{
            if(!self)break;HDC hdc=(HDC)wp;
            SetTextColor(hdc,self->m_temp.textColor);SetBkColor(hdc,self->m_temp.bgColor);
            if(!self->m_hBgBrush)self->updateBgBrush();return(LRESULT)self->m_hBgBrush;}
        case WM_CTLCOLORBTN:{
            if(!self)break;HDC hdc=(HDC)wp;
            SetTextColor(hdc,self->m_temp.textColor);SetBkMode(hdc,TRANSPARENT);
            if(!self->m_hBgBrush)self->updateBgBrush();return(LRESULT)self->m_hBgBrush;}
        case WM_CTLCOLOREDIT:{
            if(!self)break;HDC hdc=(HDC)wp;
            int bright=(GetRValue(self->m_temp.bgColor)*299+GetGValue(self->m_temp.bgColor)*587+GetBValue(self->m_temp.bgColor)*114)/1000;
            if(bright>128){SetTextColor(hdc,self->m_temp.textColor);SetBkColor(hdc,RGB(0xFF,0xFF,0xFF));
                static HBRUSH eb=CreateSolidBrush(RGB(0xFF,0xFF,0xFF));return(LRESULT)eb;}
            else{SetTextColor(hdc,self->m_temp.textColor);SetBkColor(hdc,RGB(0x2A,0x2A,0x2A));
                static HBRUSH ed=CreateSolidBrush(RGB(0x2A,0x2A,0x2A));return(LRESULT)ed;}}
        case WM_CTLCOLORLISTBOX:{
            if(!self)break;HDC hdc=(HDC)wp;
            SetTextColor(hdc,self->m_temp.textColor);SetBkColor(hdc,self->m_temp.bgColor);
            if(!self->m_hBgBrush)self->updateBgBrush();return(LRESULT)self->m_hBgBrush;}
        case WM_COMMAND:{
            int id=LOWORD(wp),code=HIWORD(wp);
            if(!self)break;
            if(id==701&&code==CBN_SELCHANGE){
                int sel=(int)SendMessageW(GetDlgItem(hwnd,701),CB_GETCURSEL,0,0);
                if(sel>=0&&sel<PinyinSettings::SKIN_COUNT){self->m_selectedSkin=sel;self->m_temp.applySkin(sel);
                    self->updateBgBrush();self->resizePreview();
                    InvalidateRect(hwnd,nullptr,TRUE);}return 0;}
            if(id==408&&code==CBN_SELCHANGE){
                HWND hfc=GetDlgItem(hwnd,408);int sel=(int)SendMessageW(hfc,CB_GETCURSEL,0,0);
                if(sel>=0){wchar_t buf[64]={};SendMessageW(hfc,CB_GETLBTEXT,sel,(LPARAM)buf);
                    self->m_temp.fontName=buf;self->resizePreview();}return 0;}
            if(id==402&&code==CBN_SELCHANGE){
                int sel=(int)SendMessageW(GetDlgItem(hwnd,402),CB_GETCURSEL,0,0);
                if(sel>=0){self->m_temp.candidateCount=sel+3;self->resizePreview();}return 0;}
            if(id==403&&code==CBN_SELCHANGE){
                int sel=(int)SendMessageW(GetDlgItem(hwnd,403),CB_GETCURSEL,0,0);
                if(sel>=0){self->m_temp.fontSize=sel+12;self->resizePreview();}return 0;}
            if(id==702){CHOOSECOLORW cc={};static COLORREF acr[16];
                cc.lStructSize=sizeof(cc);cc.hwndOwner=hwnd;cc.rgbResult=self->m_temp.bgColor;
                cc.lpCustColors=acr;cc.Flags=CC_FULLOPEN|CC_RGBINIT;
                if(ChooseColorW(&cc)){COLORREF base=cc.rgbResult;
                    int r=GetRValue(base),g=GetGValue(base),b=GetBValue(base);
                    self->m_temp.bgColor=base;self->m_temp.borderColor=RGB((r*2)/3,(g*2)/3,(b*2)/3);
                    int bright=(r*299+g*587+b*114)/1000;
                    if(bright>128){self->m_temp.textColor=RGB(std::max(0,r/4),std::max(0,g/4),std::max(0,b/4));
                        self->m_temp.indexColor=RGB(std::min(255,r/2+40),std::min(255,g/2+40),std::min(255,b/2+100));
                        self->m_temp.inputColor=RGB(0,std::min(255,g/3),std::min(255,b));}
                    else{self->m_temp.textColor=RGB(220,220,220);self->m_temp.indexColor=RGB(std::min(255,r+100),std::min(255,g+80),255);
                        self->m_temp.inputColor=RGB(150,200,255);}
                    self->m_selectedSkin=-1;SendMessageW(GetDlgItem(hwnd,701),CB_SETCURSEL,-1,0);
                    self->updateBgBrush();InvalidateRect(self->m_hSkinPreview,nullptr,TRUE);
                    InvalidateRect(hwnd,nullptr,TRUE);}return 0;}
            // ── 候选翻页 checkbox ──
            if(id==920){self->m_temp.enableMinusEqualsPage=(IsDlgButtonChecked(hwnd,920)==BST_CHECKED);return 0;}
            if(id==921){self->m_temp.enableCommaPeriodPage=(IsDlgButtonChecked(hwnd,921)==BST_CHECKED);return 0;}
            if(id==922){self->m_temp.enableTabPage=(IsDlgButtonChecked(hwnd,922)==BST_CHECKED);return 0;}
            if(id==923){self->m_temp.enableBracketPage=(IsDlgButtonChecked(hwnd,923)==BST_CHECKED);return 0;}
            // ── 候选窗显示 checkbox (影响预览) ──
            if(id==924){self->m_temp.showSettingsGear=(IsDlgButtonChecked(hwnd,924)==BST_CHECKED);self->resizePreview();return 0;}
            if(id==926){self->m_temp.roundedCorner=(IsDlgButtonChecked(hwnd,926)==BST_CHECKED);self->resizePreview();return 0;}
            if(id==910){/* 注册到系统 — TSF 方式 */doFullRegistration(hwnd);return 0;}
            if(id==911){/* 从系统卸载 */doFullUnregistration(hwnd);return 0;}
            if(id==901){/* 保存 */
                self->m_temp.useTraditional=(IsDlgButtonChecked(hwnd,401)==BST_CHECKED);
                self->m_temp.verticalLayout=(IsDlgButtonChecked(hwnd,404)==BST_CHECKED);
                self->m_temp.chinesePunctuation=(IsDlgButtonChecked(hwnd,405)==BST_CHECKED);
                {int cs=(int)SendMessageW(GetDlgItem(hwnd,402),CB_GETCURSEL,0,0);
                 if(cs>=0&&cs<=6)self->m_temp.candidateCount=cs+3;}
                {int fs=(int)SendMessageW(GetDlgItem(hwnd,403),CB_GETCURSEL,0,0);
                 if(fs>=0&&fs<=24)self->m_temp.fontSize=fs+12;}
                {HWND hfc=GetDlgItem(hwnd,408);int fs2=(int)SendMessageW(hfc,CB_GETCURSEL,0,0);
                    if(fs2>=0){wchar_t buf[64]={};SendMessageW(hfc,CB_GETLBTEXT,fs2,(LPARAM)buf);
                        if(buf[0])self->m_temp.fontName=buf;}}
                self->m_temp.fuzzyZ_Zh=(IsDlgButtonChecked(hwnd,501)==BST_CHECKED);
                self->m_temp.fuzzyC_Ch=(IsDlgButtonChecked(hwnd,502)==BST_CHECKED);
                self->m_temp.fuzzyS_Sh=(IsDlgButtonChecked(hwnd,503)==BST_CHECKED);
                self->m_temp.fuzzyN_L=(IsDlgButtonChecked(hwnd,504)==BST_CHECKED);
                self->m_temp.fuzzyF_H=(IsDlgButtonChecked(hwnd,505)==BST_CHECKED);
                self->m_temp.fuzzyEn_Eng=(IsDlgButtonChecked(hwnd,506)==BST_CHECKED);
                self->m_temp.fuzzyIn_Ing=(IsDlgButtonChecked(hwnd,507)==BST_CHECKED);
                self->m_temp.smartCorrection=(IsDlgButtonChecked(hwnd,601)==BST_CHECKED);
                self->m_temp.autoWordCreate=(IsDlgButtonChecked(hwnd,602)==BST_CHECKED);
                self->m_temp.autoFreqAdjust=(IsDlgButtonChecked(hwnd,603)==BST_CHECKED);
                self->m_temp.enableMinusEqualsPage=(IsDlgButtonChecked(hwnd,920)==BST_CHECKED);
                self->m_temp.enableCommaPeriodPage=(IsDlgButtonChecked(hwnd,921)==BST_CHECKED);
                self->m_temp.enableTabPage=(IsDlgButtonChecked(hwnd,922)==BST_CHECKED);
                self->m_temp.enableBracketPage=(IsDlgButtonChecked(hwnd,923)==BST_CHECKED);
                self->m_temp.showSettingsGear=(IsDlgButtonChecked(hwnd,924)==BST_CHECKED);
                self->m_temp.roundedCorner=(IsDlgButtonChecked(hwnd,926)==BST_CHECKED);
                g_settings=self->m_temp;
                g_settings.saveToFile(getModuleDirectory(nullptr)+"pinyin_config.ini");
                DestroyWindow(hwnd);return 0;}
            if(id==902){DestroyWindow(hwnd);return 0;}
            break;}
        case WM_NCDESTROY:
            if(self){if(self->m_hFont){DeleteObject(self->m_hFont);self->m_hFont=nullptr;}
                if(self->m_hBgBrush){DeleteObject(self->m_hBgBrush);self->m_hBgBrush=nullptr;}
                SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);}break;
        }
        return DefWindowProcW(hwnd,msg,wp,lp);
    }

    static void show(HINSTANCE hInst,HWND hParent){
        SettingsWindow* sw=new SettingsWindow();
        sw->m_temp=g_settings;
        {HDC hdc=GetDC(nullptr);int dpi=GetDeviceCaps(hdc,LOGPIXELSY);ReleaseDC(nullptr,hdc);
            sw->m_dpiScale=(float)dpi/96.0f;}
        auto S=[sw](int v)->int{return (int)(v*sw->m_dpiScale+0.5f);};
        WNDCLASSEXW wc={};wc.cbSize=sizeof(wc);wc.lpfnWndProc=dlgProc;wc.hInstance=hInst;
        wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=nullptr;
        wc.lpszClassName=L"PinyinIMESettings2";RegisterClassExW(&wc);
        HWND hDlg=CreateWindowExW(WS_EX_TOPMOST,
            L"PinyinIMESettings2",L"PinyinIME 设置",
            WS_POPUP,
            CW_USEDEFAULT,CW_USEDEFAULT,S(530),S(610),hParent,nullptr,hInst,sw);
        if(hDlg){RECT rc;GetWindowRect(hDlg,&rc);int w=rc.right-rc.left,h=rc.bottom-rc.top;
            int sx=GetSystemMetrics(SM_CXSCREEN),sy=GetSystemMetrics(SM_CYSCREEN);
            SetWindowPos(hDlg,nullptr,(sx-w)/2,(sy-h)/2,0,0,SWP_NOSIZE|SWP_NOZORDER);
            ShowWindow(hDlg,SW_SHOWNORMAL);UpdateWindow(hDlg);}
        MSG msg;while(GetMessageW(&msg,nullptr,0,0)){
            if(!IsWindow(hDlg))break;TranslateMessage(&msg);DispatchMessageW(&msg);}
        delete sw;
    }
};
