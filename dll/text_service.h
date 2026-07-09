// dll/text_service.h — CPinyinTextService 声明
// 实现 TSF 文本服务接口
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <msctf.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include "../shared/pinyin_settings.h"
#include "../shared/tsf_guids.h"
#include "pinyin_engine.h"
#include "candidate_window.h"

// ==================== 前向声明 ====================
class CPinyinTextService;

// ==================== Display Attribute GUID ====================
// {C3D4E5F6-A1B2-8901-CDEF-1234567890AB}
static const GUID GUID_PinyinDisplayAttribute =
    { 0xC3D4E5F6, 0xA1B2, 0x8901, { 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB } };

// ==================== Display Attribute Info ====================
class CPinyinDisplayAttributeInfo : public ITfDisplayAttributeInfo {
private:
    LONG m_cRef;
    PinyinSettings* m_pSettings;  // 指向 text service 的设置

public:
    CPinyinDisplayAttributeInfo(PinyinSettings* pSettings)
        : m_cRef(1), m_pSettings(pSettings) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ITfDisplayAttributeInfo) {
            *ppv = static_cast<ITfDisplayAttributeInfo*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return c;
    }

    // ITfDisplayAttributeInfo
    STDMETHODIMP GetGUID(GUID* pGuid) override {
        if (!pGuid) return E_POINTER;
        *pGuid = GUID_PinyinDisplayAttribute;
        return S_OK;
    }
    STDMETHODIMP GetDescription(BSTR* pbstrDesc) override {
        if (!pbstrDesc) return E_POINTER;
        *pbstrDesc = SysAllocString(L"PinyinIME Composition");
        return S_OK;
    }
    STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE* pAttr) override {
        if (!pAttr) return E_POINTER;
        COLORREF inputC = m_pSettings ? m_pSettings->inputColor : RGB(0x32, 0x64, 0x32);
        pAttr->crText.type = TF_CT_COLORREF;
        pAttr->crText.cr = inputC;
        pAttr->crBk.type = TF_CT_NONE;
        pAttr->crBk.cr = 0;
        pAttr->lsStyle = TF_LS_DASH;      // 虚线下划线
        pAttr->fBoldLine = FALSE;
        pAttr->crLine.type = TF_CT_COLORREF;
        pAttr->crLine.cr = inputC;
        pAttr->bAttr = TF_ATTR_INPUT;
        return S_OK;
    }
    STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE* /*pAttr*/) override { return S_OK; }
    STDMETHODIMP Reset() override { return S_OK; }
};

// ==================== Display Attribute Enumerator ====================
class CPinyinDisplayAttributeEnum : public IEnumTfDisplayAttributeInfo {
private:
    LONG m_cRef;
    ULONG m_index;
    CPinyinDisplayAttributeInfo* m_pInfo;

public:
    CPinyinDisplayAttributeEnum(CPinyinDisplayAttributeInfo* pInfo)
        : m_cRef(1), m_index(0), m_pInfo(pInfo) {
        if (m_pInfo) m_pInfo->AddRef();
    }
    ~CPinyinDisplayAttributeEnum() {
        if (m_pInfo) m_pInfo->Release();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IEnumTfDisplayAttributeInfo) {
            *ppv = static_cast<IEnumTfDisplayAttributeInfo*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return c;
    }

    STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        auto* pClone = new (std::nothrow) CPinyinDisplayAttributeEnum(m_pInfo);
        if (!pClone) return E_OUTOFMEMORY;
        pClone->m_index = m_index;
        *ppEnum = pClone;
        return S_OK;
    }
    STDMETHODIMP Next(ULONG ulCount, ITfDisplayAttributeInfo** rgInfo, ULONG* pcFetched) override {
        if (!rgInfo) return E_POINTER;
        if (pcFetched) *pcFetched = 0;
        if (ulCount == 0) return S_OK;
        if (m_index >= 1) return S_FALSE;  // 只有一项
        rgInfo[0] = m_pInfo;
        m_pInfo->AddRef();
        m_index++;
        if (pcFetched) *pcFetched = 1;
        return (ulCount == 1) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override { m_index = 0; return S_OK; }
    STDMETHODIMP Skip(ULONG ulCount) override {
        m_index += ulCount;
        return S_OK;
    }
};

// ==================== Edit Session (文本提交) ====================
// 注意: ITfEditSession 作为独立对象使用 RequestEditSession
class CPinyinEditSession : public ITfEditSession {
private:
    LONG m_cRef;
    CPinyinTextService* m_pService;

public:
    CPinyinEditSession(CPinyinTextService* pService)
        : m_cRef(1), m_pService(pService) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return c;
    }
    STDMETHODIMP DoEditSession(TfEditCookie ec) override;
};

// ==================== 主文本服务类 ====================
class CPinyinTextService : public ITfTextInputProcessorEx,
                           public ITfKeyEventSink,
                           public ITfCompositionSink,
                           public ITfDisplayAttributeProvider,
                           public ITfCompartmentEventSink {
    friend class CPinyinEditSession;

private:
    LONG m_cRef;
    ITfThreadMgr* m_pThreadMgr;
    TfClientId m_clientId;
    DWORD m_activateFlags;
    DWORD m_dwKeyEventCookie;
    DWORD m_dwCompositionCookie;
    DWORD m_dwDisplayAttrCookie;
    DWORD m_dwCompartmentKeyboardCookie;     // GUID_COMPARTMENT_KEYBOARD_OPENCLOSE
    DWORD m_dwCompartmentConversionCookie;   // GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION

    // 引擎和 UI
    PinyinEngine m_engine;
    CandidateWindow m_candidateWin;
    PinyinSettings m_settings;
    ULONG_PTR m_gdiplusToken;
    bool m_chineseMode;

    // 组合 (Composition) 状态
    enum PendingAction { ACT_NONE, ACT_START, ACT_UPDATE, ACT_COMMIT, ACT_CANCEL };
    PendingAction m_pendingAction;
    ITfComposition* m_pComposition;
    bool m_bComposing;
    std::wstring m_pendingCommit;
    bool m_pendingStripQuote;

    // 候选窗口 (在 TSF 激活线程上创建, 由应用程序消息泵驱动)

public:
    CPinyinTextService();
    ~CPinyinTextService();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfTextInputProcessor (基类)
    STDMETHODIMP Activate(ITfThreadMgr* ptim, TfClientId tid) override {
        return ActivateEx(ptim, tid, 0);
    }
    STDMETHODIMP Deactivate() override;

    // ITfTextInputProcessorEx
    STDMETHODIMP ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) override;

    // ITfKeyEventSink — 替代 keyboardProc
    STDMETHODIMP OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnSetFocus(BOOL fForeground) override;
    STDMETHODIMP OnPreservedKey(ITfContext*, const GUID&, BOOL*) override { return S_OK; }

    // ITfCompositionSink — 组合终止通知
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) override;

    // ITfDisplayAttributeProvider
    STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) override;
    STDMETHODIMP GetDisplayAttributeInfo(REFGUID guidInfo, ITfDisplayAttributeInfo** ppInfo) override;

    // ITfCompartmentEventSink — 监听键盘开关/转换模式变化
    STDMETHODIMP OnChange(REFGUID rguid) override;

    // 组合管理
    void startComposition(ITfContext* pContext, TfEditCookie ec);
    void updateComposition(ITfContext* pContext, TfEditCookie ec);
    void commitComposition(const std::wstring& text);
    void cancelComposition();

    // 辅助
    void loadSettings();
    void showCandidateWindow();
    void hideCandidateWindow();
    void handleKeyCommit(int candidateIndex);

    // Check if a key event matches the configured toggle hotkey
    bool isToggleHotkey(DWORD vk, bool ctrl, bool alt, bool win) const {
        DWORD mod = m_settings.toggleModifier;
        DWORD key = m_settings.toggleHotkey;
        if ((ctrl && mod != VK_CONTROL) || (!ctrl && mod == VK_CONTROL)) return false;
        if ((alt  && mod != VK_MENU)    || (!alt  && mod == VK_MENU))    return false;
        if (win) return false;
        // Accept VK_SHIFT, VK_LSHIFT, VK_RSHIFT interchangeably
        if (key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT)
            return (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT);
        return vk == key;
    }
};

// QueryInterface 实现在 text_service.cpp (需要完整类型)
inline STDMETHODIMP CPinyinEditSession::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfEditSession) {
        *ppv = static_cast<ITfEditSession*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}
