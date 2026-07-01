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

    // 皮肤预览子窗口过程
    static LRESULT CALLBACK skinPreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        SettingsWindow* self = (SettingsWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (!self && msg == WM_CREATE) {
            self = (SettingsWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
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
            {   // GDI+ 背景 + 渐变边框
                Gdiplus::Graphics graphics(hdc);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                int w=rc.right,h=rc.bottom,cr=(std::max)(6,(std::min)(12,(int)(tm.tmHeight*2/3)));
                auto makeRR=[](Gdiplus::GraphicsPath& p,int x,int y,int rw,int rh,int rad){
                    p.Reset();p.StartFigure();
                    p.AddArc(x,y,rad*2,rad*2,180,90);p.AddArc(x+rw-rad*2,y,rad*2,rad*2,270,90);
                    p.AddArc(x+rw-rad*2,y+rh-rad*2,rad*2,rad*2,0,90);p.AddArc(x,y+rh-rad*2,rad*2,rad*2,90,90);
                    p.CloseFigure();
                };
                Gdiplus::GraphicsPath bgPath;
                {COLORREF c=self->m_temp.bgColor;Gdiplus::SolidBrush bgBrush(Gdiplus::Color(GetRValue(c),GetGValue(c),GetBValue(c)));
                 makeRR(bgPath,0,0,w,h,cr);graphics.FillPath(&bgBrush,&bgPath);}
                COLORREF bc=self->m_temp.borderColor,bg=self->m_temp.bgColor;
                int rr=GetRValue(bc),rg=GetGValue(bc),rb=GetBValue(bc);
                int bgBright=(GetRValue(bg)*299+GetGValue(bg)*587+GetBValue(bg)*114)/1000;
                int dir=(bgBright<128)?1:-1;
                auto clampC=[](int v)->int{return v<0?0:(v>255?255:v);};
                Gdiplus::GraphicsPath borderPath;
                for(int layer=0;layer<3;layer++){
                    int off=layer,delta=(layer==0)?40:(layer==1)?18:0;
                    int lw=w-off*2,lh=h-off*2,lcr=cr-off;if(lcr<2)lcr=2;
                    Gdiplus::Color penColor(clampC(rr+delta*dir),clampC(rg+delta*dir),clampC(rb+delta*dir));
                    Gdiplus::Pen pen(penColor,1.0f);
                    makeRR(borderPath,off,off,lw,lh,lcr);graphics.DrawPath(&pen,&borderPath);
                }
            }
            int borderW=3,pad=(std::max)(4,(int)(tm.tmHeight/8)),textY=borderW+pad,x=S(8);
            SIZE sz;
            // 候选词 (从左边距开始, 无拼音前缀)
            SetTextColor(hdc,self->m_temp.indexColor);TextOutW(hdc,x,textY,L"1.",2);
            GetTextExtentPoint32W(hdc,L"1.",2,&sz);x+=sz.cx;
            SetTextColor(hdc,self->m_temp.textColor);TextOutW(hdc,x,textY,L"你好",2);
            GetTextExtentPoint32W(hdc,L"你好",2,&sz);x+=sz.cx+S(8);
            SetTextColor(hdc,self->m_temp.indexColor);TextOutW(hdc,x,textY,L"2.",2);
            GetTextExtentPoint32W(hdc,L"2.",2,&sz);x+=sz.cx;
            SetTextColor(hdc,self->m_temp.textColor);TextOutW(hdc,x,textY,L"泥嚎",2);
            GetTextExtentPoint32W(hdc,L"泥嚎",2,&sz);x+=sz.cx+S(8);
            SetTextColor(hdc,self->m_temp.indexColor);TextOutW(hdc,x,textY,L"3.",2);
            GetTextExtentPoint32W(hdc,L"3.",2,&sz);x+=sz.cx;
            SetTextColor(hdc,self->m_temp.textColor);TextOutW(hdc,x,textY,L"你号",2);
            GetTextExtentPoint32W(hdc,L"你号",2,&sz);x+=sz.cx+S(4);
            SetTextColor(hdc,RGB(150,150,150));TextOutW(hdc,x,textY,L" -/=/翻页",8);
            GetTextExtentPoint32W(hdc,L" -/=/翻页",8,&sz);x+=sz.cx+S(4);
            SetTextColor(hdc,RGB(80,80,200));TextOutW(hdc,x,textY,L"⚙",1);
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

    void buildUI(){
        HINSTANCE hInst=(HINSTANCE)GetWindowLongPtrW(m_hDlg,GWLP_HINSTANCE);
        auto S=[this](int v)->int{return (int)(v*m_dpiScale+0.5f);};
        if(!m_hFont){m_hFont=CreateFontW(-S(16),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,m_temp.fontName.c_str());}

        addLabel(L"PinyinIME 输入法设置",S(15),S(10),S(300),S(24));
        int gy=S(40);
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
            S(20),gy,S(460),S(48),m_hDlg,nullptr,hInst,this);gy+=S(55);

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

        addLabel(L"━━ 系统 ━━",S(15),gy,S(200),S(18));gy+=S(22);
        addLabel(L"切换热键:",S(20),gy,S(80),S(20));
        HWND hHotkey=addCombo(406,S(100),gy-S(2),S(160),S(200));
        SendMessageW(hHotkey,CB_ADDSTRING,0,(LPARAM)L"Ctrl + Shift");
        SendMessageW(hHotkey,CB_ADDSTRING,0,(LPARAM)L"Alt + Shift");
        SendMessageW(hHotkey,CB_ADDSTRING,0,(LPARAM)L"Right Shift (单独)");
        SendMessageW(hHotkey,CB_ADDSTRING,0,(LPARAM)L"Ctrl + Space");
        {int hs=1;
         if(m_temp.toggleModifier==VK_MENU&&m_temp.toggleHotkey==VK_SHIFT)hs=0;
         else if(m_temp.toggleModifier==0&&m_temp.toggleHotkey==VK_RSHIFT)hs=2;
         else if(m_temp.toggleModifier==VK_CONTROL&&m_temp.toggleHotkey==VK_SPACE)hs=3;
         SendMessageW(hHotkey,CB_SETCURSEL,hs,0);}gy+=S(28);

        // TSF 注册 / 卸载按钮
        addButton(L"📌 注册输入法到系统 (TSF)",910,S(20),gy,S(215),S(26));
        addButton(L"🗑 从系统卸载此输入法",911,S(245),gy,S(215),S(26));gy+=S(32);

        addButton(L"✅ 保存设置",901,S(250),gy,S(100),S(28));
        addButton(L"❌ 取消",902,S(360),gy,S(80),S(28));

        RECT clientRC={0,0,S(530),gy+S(52)};
        AdjustWindowRect(&clientRC,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE);
        int winW=clientRC.right-clientRC.left,winH=clientRC.bottom-clientRC.top;
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
        case WM_ERASEBKGND:{
            return TRUE;}  // WM_PAINT 负责全部背景绘制
        case WM_PAINT:{
            if(!self)break;
            PAINTSTRUCT ps;HDC hdc=BeginPaint(hwnd,&ps);
            RECT rc;GetClientRect(hwnd,&rc);
            {
                Gdiplus::Graphics graphics(hdc);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                int w=rc.right,h=rc.bottom,cr=self->m_roundR;
                auto makeRR=[](Gdiplus::GraphicsPath& p,int x,int y,int rw,int rh,int rad){
                    p.Reset();p.StartFigure();
                    p.AddArc(x,y,rad*2,rad*2,180,90);p.AddArc(x+rw-rad*2,y,rad*2,rad*2,270,90);
                    p.AddArc(x+rw-rad*2,y+rh-rad*2,rad*2,rad*2,0,90);p.AddArc(x,y+rh-rad*2,rad*2,rad*2,90,90);
                    p.CloseFigure();
                };
                Gdiplus::GraphicsPath bgPath;
                {COLORREF c=self->m_temp.bgColor;Gdiplus::SolidBrush bgBrush(Gdiplus::Color(GetRValue(c),GetGValue(c),GetBValue(c)));
                 makeRR(bgPath,0,0,w,h,cr);graphics.FillPath(&bgBrush,&bgPath);}
                COLORREF bc=self->m_temp.borderColor,bg=self->m_temp.bgColor;
                int rr=GetRValue(bc),rg=GetGValue(bc),rb=GetBValue(bc);
                int bgBright=(GetRValue(bg)*299+GetGValue(bg)*587+GetBValue(bg)*114)/1000;
                int dir=(bgBright<128)?1:-1;
                auto clampC=[](int v)->int{return v<0?0:(v>255?255:v);};
                Gdiplus::GraphicsPath borderPath;
                for(int layer=0;layer<3;layer++){
                    int off=layer,delta=(layer==0)?40:(layer==1)?18:0;
                    int lw=w-off*2,lh=h-off*2,lcr=cr-off;if(lcr<2)lcr=2;
                    Gdiplus::Color penColor(clampC(rr+delta*dir),clampC(rg+delta*dir),clampC(rb+delta*dir));
                    Gdiplus::Pen pen(penColor,1.0f);
                    makeRR(borderPath,off,off,lw,lh,lcr);graphics.DrawPath(&pen,&borderPath);
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
                    self->updateBgBrush();InvalidateRect(self->m_hSkinPreview,nullptr,TRUE);
                    InvalidateRect(hwnd,nullptr,TRUE);}return 0;}
            if(id==408&&code==CBN_SELCHANGE){
                HWND hfc=GetDlgItem(hwnd,408);int sel=(int)SendMessageW(hfc,CB_GETCURSEL,0,0);
                if(sel>=0){wchar_t buf[64]={};SendMessageW(hfc,CB_GETLBTEXT,sel,(LPARAM)buf);
                    self->m_temp.fontName=buf;InvalidateRect(self->m_hSkinPreview,nullptr,TRUE);}return 0;}
            if(id==402&&code==CBN_SELCHANGE){
                int sel=(int)SendMessageW(GetDlgItem(hwnd,402),CB_GETCURSEL,0,0);
                if(sel>=0){self->m_temp.candidateCount=sel+3;InvalidateRect(self->m_hSkinPreview,nullptr,TRUE);}return 0;}
            if(id==403&&code==CBN_SELCHANGE){
                int sel=(int)SendMessageW(GetDlgItem(hwnd,403),CB_GETCURSEL,0,0);
                if(sel>=0){self->m_temp.fontSize=sel+12;InvalidateRect(self->m_hSkinPreview,nullptr,TRUE);}return 0;}
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
                {int hs=(int)SendMessageW(GetDlgItem(hwnd,406),CB_GETCURSEL,0,0);
                    switch(hs){case 0:self->m_temp.toggleModifier=VK_CONTROL;self->m_temp.toggleHotkey=VK_SHIFT;break;
                        case 1:self->m_temp.toggleModifier=VK_MENU;self->m_temp.toggleHotkey=VK_SHIFT;break;
                        case 2:self->m_temp.toggleModifier=0;self->m_temp.toggleHotkey=VK_RSHIFT;break;
                        case 3:self->m_temp.toggleModifier=VK_CONTROL;self->m_temp.toggleHotkey=VK_SPACE;break;}}
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
        HWND hDlg=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_TOPMOST,
            L"PinyinIMESettings2",L"PinyinIME 设置",
            WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
            CW_USEDEFAULT,CW_USEDEFAULT,S(530),S(580),hParent,nullptr,hInst,sw);
        if(hDlg){RECT rc;GetWindowRect(hDlg,&rc);int w=rc.right-rc.left,h=rc.bottom-rc.top;
            int sx=GetSystemMetrics(SM_CXSCREEN),sy=GetSystemMetrics(SM_CYSCREEN);
            SetWindowPos(hDlg,nullptr,(sx-w)/2,(sy-h)/2,0,0,SWP_NOSIZE|SWP_NOZORDER);
            ShowWindow(hDlg,SW_SHOWNORMAL);UpdateWindow(hDlg);}
        MSG msg;while(GetMessageW(&msg,nullptr,0,0)){
            if(!IsWindow(hDlg))break;TranslateMessage(&msg);DispatchMessageW(&msg);}
        delete sw;
    }
};
