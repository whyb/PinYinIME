// dll/text_service.cpp — CPinyinTextService 实现
// TSF 文本服务核心: 激活/停用, 按键处理, 组合管理, 候选窗口
#include "text_service.h"
#include "class_factory.h"
#include "../shared/utf_utils.h"

// ==================== CPinyinTextService 构造 / 析构 ====================
CPinyinTextService::CPinyinTextService()
    : m_cRef(1), m_pThreadMgr(nullptr), m_clientId(0), m_activateFlags(0),
      m_dwKeyEventCookie(0), m_dwCompositionCookie(0), m_dwDisplayAttrCookie(0),
      m_dwCompartmentKeyboardCookie(0), m_dwCompartmentConversionCookie(0),
      m_gdiplusToken(0), m_chineseMode(true),
      m_pendingAction(ACT_NONE), m_pComposition(nullptr), m_bComposing(false),
      m_pendingStripQuote(false)
{
}

CPinyinTextService::~CPinyinTextService() {
    if (m_pThreadMgr) {
        m_pThreadMgr->Release();
        m_pThreadMgr = nullptr;
    }
}

// ==================== IUnknown ====================
STDMETHODIMP CPinyinTextService::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_ITfTextInputProcessorEx) {
        *ppv = static_cast<ITfTextInputProcessorEx*>(this);
    } else if (riid == IID_ITfTextInputProcessor) {
        *ppv = static_cast<ITfTextInputProcessor*>(this);
    } else if (riid == IID_ITfKeyEventSink) {
        *ppv = static_cast<ITfKeyEventSink*>(this);
    } else if (riid == IID_ITfCompositionSink) {
        *ppv = static_cast<ITfCompositionSink*>(this);
    } else if (riid == IID_ITfDisplayAttributeProvider) {
        *ppv = static_cast<ITfDisplayAttributeProvider*>(this);
    } else if (riid == IID_ITfCompartmentEventSink) {
        *ppv = static_cast<ITfCompartmentEventSink*>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) CPinyinTextService::AddRef() {
    return InterlockedIncrement(&m_cRef);
}

STDMETHODIMP_(ULONG) CPinyinTextService::Release() {
    LONG c = InterlockedDecrement(&m_cRef);
    if (c == 0) delete this;
    return c;
}

// ==================== ITfTextInputProcessorEx ====================
STDMETHODIMP CPinyinTextService::ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) {
    m_pThreadMgr = ptim;
    m_pThreadMgr->AddRef();
    m_clientId = tid;
    m_activateFlags = dwFlags;

    // 加载设置
    loadSettings();

    // 初始化拼音引擎
    m_engine.setSettings(&m_settings);
    m_engine.init();
    g_pSharedEngine = &m_engine;  // 候选窗口需要通过全局指针访问引擎

    // 初始化 GDI+
    Gdiplus::GdiplusStartupInput gdiSI;
    Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiSI, nullptr);

    // 创建候选窗口 (在 TSF 激活线程上, 由应用程序消息泵驱动)
    // 设置指针以便候选窗口使用
    m_candidateWin.m_pSettings = &m_settings;
    m_candidateWin.create(g_hDllInst);
    OutputDebugStringA("[PinyinIME] Candidate window created\n");

    // ── 1. 注册 KeyEventSink (通过 ITfKeystrokeMgr 替代键盘钩子) ──
    ITfKeystrokeMgr* pKeystrokeMgr = nullptr;
    if (SUCCEEDED(m_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKeystrokeMgr))) {
        pKeystrokeMgr->AdviseKeyEventSink(m_clientId,
            static_cast<ITfKeyEventSink*>(this), TRUE);
        pKeystrokeMgr->Release();
    }

    // ── 2. 注册 CompartmentEventSink (监听键盘开关/转换模式) ──
    // 键盘开关状态舱室 (IME 打开/关闭)
    ITfCompartmentMgr* pCompMgr = nullptr;
    if (SUCCEEDED(m_pThreadMgr->QueryInterface(IID_ITfCompartmentMgr, (void**)&pCompMgr))) {
        ITfCompartment* pComp = nullptr;
        // 键盘开关
        if (SUCCEEDED(pCompMgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &pComp))) {
            ITfSource* pCompSource = nullptr;
            if (SUCCEEDED(pComp->QueryInterface(IID_ITfSource, (void**)&pCompSource))) {
                pCompSource->AdviseSink(IID_ITfCompartmentEventSink,
                    static_cast<ITfCompartmentEventSink*>(this),
                    &m_dwCompartmentKeyboardCookie);
                pCompSource->Release();
            }
            pComp->Release();
        }
        // 输入模式转换
        if (SUCCEEDED(pCompMgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &pComp))) {
            ITfSource* pCompSource = nullptr;
            if (SUCCEEDED(pComp->QueryInterface(IID_ITfSource, (void**)&pCompSource))) {
                pCompSource->AdviseSink(IID_ITfCompartmentEventSink,
                    static_cast<ITfCompartmentEventSink*>(this),
                    &m_dwCompartmentConversionCookie);
                pCompSource->Release();
            }
            pComp->Release();
        }
        pCompMgr->Release();
    }

    // ── 3. 注册 DisplayAttributeProvider (组合文本显示属性) ──
    ITfSourceSingle* pSourceSingle = nullptr;
    if (SUCCEEDED(m_pThreadMgr->QueryInterface(IID_ITfSourceSingle, (void**)&pSourceSingle))) {
        pSourceSingle->AdviseSingleSink(m_clientId, IID_ITfDisplayAttributeProvider,
            static_cast<ITfDisplayAttributeProvider*>(this));
        m_dwDisplayAttrCookie = 1;  // ITfSourceSingle 不使用 cookie, 标记已安装
        pSourceSingle->Release();
    }

    return S_OK;
}

STDMETHODIMP CPinyinTextService::Deactivate() {
    // 如果正在组合, 提交残留文本
    if (m_bComposing) {
        cancelComposition();
    }

    // ── 1. 注销 KeyEventSink ──
    ITfKeystrokeMgr* pKeystrokeMgr = nullptr;
    if (m_pThreadMgr && SUCCEEDED(m_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKeystrokeMgr))) {
        pKeystrokeMgr->UnadviseKeyEventSink(m_clientId);
        pKeystrokeMgr->Release();
    }

    // ── 2. 注销 CompartmentEventSinks ──
    ITfCompartmentMgr* pCompMgr = nullptr;
    if (m_pThreadMgr && SUCCEEDED(m_pThreadMgr->QueryInterface(IID_ITfCompartmentMgr, (void**)&pCompMgr))) {
        ITfCompartment* pComp = nullptr;
        // 键盘开关
        if (m_dwCompartmentKeyboardCookie &&
            SUCCEEDED(pCompMgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &pComp))) {
            ITfSource* pCompSource = nullptr;
            if (SUCCEEDED(pComp->QueryInterface(IID_ITfSource, (void**)&pCompSource))) {
                pCompSource->UnadviseSink(m_dwCompartmentKeyboardCookie);
                pCompSource->Release();
            }
            pComp->Release();
        }
        // 输入模式转换
        if (m_dwCompartmentConversionCookie &&
            SUCCEEDED(pCompMgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &pComp))) {
            ITfSource* pCompSource = nullptr;
            if (SUCCEEDED(pComp->QueryInterface(IID_ITfSource, (void**)&pCompSource))) {
                pCompSource->UnadviseSink(m_dwCompartmentConversionCookie);
                pCompSource->Release();
            }
            pComp->Release();
        }
        pCompMgr->Release();
    }
    m_dwCompartmentKeyboardCookie = 0;
    m_dwCompartmentConversionCookie = 0;

    // ── 3. 注销 DisplayAttributeProvider ──
    if (m_pThreadMgr && m_dwDisplayAttrCookie) {
        ITfSourceSingle* pSourceSingle = nullptr;
        if (SUCCEEDED(m_pThreadMgr->QueryInterface(IID_ITfSourceSingle, (void**)&pSourceSingle))) {
            pSourceSingle->UnadviseSingleSink(m_clientId, IID_ITfDisplayAttributeProvider);
            pSourceSingle->Release();
        }
        m_dwDisplayAttrCookie = 0;
    }

    // 销毁候选窗口
    m_candidateWin.hide();
    m_candidateWin.destroy();
    OutputDebugStringA("[PinyinIME] Deactivate complete\n");

    // 保存用户词典
    m_engine.saveUserDict();

    // 关闭 GDI+
    if (m_gdiplusToken) {
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
    }

    return S_OK;
}

// ==================== ITfKeyEventSink (从 keyboardProc 移植) ====================
STDMETHODIMP CPinyinTextService::OnTestKeyDown(ITfContext*, WPARAM wParam, LPARAM, BOOL* pfEaten) {
    *pfEaten = FALSE;

    DWORD vk = (DWORD)wParam;
    bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool altDown  = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
    bool winDown  = (GetAsyncKeyState(VK_LWIN)    & 0x8000) != 0
                 || (GetAsyncKeyState(VK_RWIN)    & 0x8000) != 0;

    // 中文模式下, 以下按键需要 IME 处理:
    if (m_chineseMode && !ctrlDown && !altDown && !winDown) {
        // 字母键 A-Z (拼音输入)
        if (vk >= 'A' && vk <= 'Z') {
            bool capsLock = (GetKeyState(VK_CAPITAL) & 1) != 0;
            if (!capsLock) {
                *pfEaten = TRUE;
                return S_OK;
            }
        }
        // 数字键 1-9 (候选选择, 仅当有候选时)
        if (vk >= '1' && vk <= '9' && !m_engine.m_candidates.empty()) {
            *pfEaten = TRUE;
            return S_OK;
        }
        // 空格 (确认候选)
        if (vk == VK_SPACE && !m_engine.m_candidates.empty()) {
            *pfEaten = TRUE;
            return S_OK;
        }
        // 退格 (拼音回退)
        if (vk == VK_BACK && !m_engine.m_buffer.empty()) {
            *pfEaten = TRUE;
            return S_OK;
        }
        // Enter (提交拼音)
        if (vk == VK_RETURN && !m_engine.m_buffer.empty()) {
            *pfEaten = TRUE;
            return S_OK;
        }
        // Escape (取消)
        if (vk == VK_ESCAPE && !m_engine.m_buffer.empty()) {
            *pfEaten = TRUE;
            return S_OK;
        }
        // Shift (提交拼音 + 切换)
        if ((vk == VK_LSHIFT || vk == VK_RSHIFT) && !m_engine.m_buffer.empty()) {
            *pfEaten = TRUE;
            return S_OK;
        }
        // - / PageUp (上一页)
        if ((vk == VK_OEM_MINUS || vk == VK_PRIOR) && !m_engine.m_candidates.empty()) {
            *pfEaten = TRUE;
            return S_OK;
        }
        // = / PageDown (下一页)
        if ((vk == VK_OEM_PLUS || vk == VK_NEXT) && !m_engine.m_candidates.empty()) {
            *pfEaten = TRUE;
            return S_OK;
        }
        // 撇号 (分词符)
        if (vk == VK_OEM_7 && !m_engine.m_buffer.empty()) {
            *pfEaten = TRUE;
            return S_OK;
        }
    }

    // 切换热键 (始终监听)
    {
        bool isToggleKey = false;
        if (m_settings.toggleModifier == 0) {
            isToggleKey = (vk == m_settings.toggleHotkey);
        } else {
            if (m_settings.toggleHotkey == VK_SHIFT)
                isToggleKey = (vk == VK_LSHIFT || vk == VK_RSHIFT);
            else
                isToggleKey = (vk == m_settings.toggleHotkey);
        }
        bool modifierOk = false;
        if (m_settings.toggleModifier == 0)
            modifierOk = !ctrlDown && !altDown && !winDown;
        else if (m_settings.toggleModifier == VK_MENU)
            modifierOk = altDown && !ctrlDown && !winDown;
        else if (m_settings.toggleModifier == VK_CONTROL)
            modifierOk = ctrlDown && !altDown && !winDown;

        if (isToggleKey && modifierOk) {
            *pfEaten = TRUE;
        }
    }

    return S_OK;
}
STDMETHODIMP CPinyinTextService::OnTestKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* pfEaten) {
    *pfEaten = FALSE;
    return S_OK;
}
STDMETHODIMP CPinyinTextService::OnKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* pfEaten) {
    *pfEaten = FALSE;
    return S_OK;
}

STDMETHODIMP CPinyinTextService::OnKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    DWORD vk = (DWORD)wParam;
    *pfEaten = FALSE;

    // 读取修饰键状态
    bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool altDown  = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
    bool winDown  = (GetAsyncKeyState(VK_LWIN)    & 0x8000) != 0
                 || (GetAsyncKeyState(VK_RWIN)    & 0x8000) != 0;

    // ── 切换热键 ──
    bool isToggleKey = false;
    if (m_settings.toggleModifier == 0) {
        isToggleKey = (vk == m_settings.toggleHotkey);
    } else {
        if (m_settings.toggleHotkey == VK_SHIFT)
            isToggleKey = (vk == VK_LSHIFT || vk == VK_RSHIFT);
        else
            isToggleKey = (vk == m_settings.toggleHotkey);
    }

    bool modifierOk = false;
    if (m_settings.toggleModifier == 0)
        modifierOk = !ctrlDown && !altDown && !winDown;
    else if (m_settings.toggleModifier == VK_MENU)
        modifierOk = altDown && !ctrlDown && !winDown;
    else if (m_settings.toggleModifier == VK_CONTROL)
        modifierOk = ctrlDown && !altDown && !winDown;

    if (isToggleKey && modifierOk) {
        if (m_chineseMode && !m_engine.m_buffer.empty()) {
            cancelComposition();
        }
        m_chineseMode = !m_chineseMode;
        *pfEaten = TRUE;
        return S_OK;
    }

    // 透传含 Ctrl/Win/Alt 的组合键
    if (ctrlDown || winDown || altDown) {
        return S_OK;
    }

    if (!m_chineseMode) {
        return S_OK;  // 英文模式: 全部放行
    }

    // ── Enter: 剥离 ' 后提交拼音 ──
    if (vk == VK_RETURN && !m_engine.m_buffer.empty()) {
        std::string rawText = m_engine.m_buffer;
        rawText.erase(std::remove(rawText.begin(), rawText.end(), '\''), rawText.end());
        m_engine.clear();
        hideCandidateWindow();
        commitComposition(utf8ToWide(rawText));
        *pfEaten = TRUE;
        return S_OK;
    }

    // ── Shift: 提交拼音 + 切换模式 ──
    if ((vk == VK_LSHIFT || vk == VK_RSHIFT) && !m_engine.m_buffer.empty()) {
        std::string rawText = m_engine.m_buffer;
        rawText.erase(std::remove(rawText.begin(), rawText.end(), '\''), rawText.end());
        m_engine.clear();
        hideCandidateWindow();
        commitComposition(utf8ToWide(rawText));
        m_chineseMode = !m_chineseMode;
        *pfEaten = TRUE;
        return S_OK;
    }

    // ── Escape: 取消组合 ──
    if (vk == VK_ESCAPE) {
        if (!m_engine.m_buffer.empty()) {
            cancelComposition();
            m_engine.clear();
            hideCandidateWindow();
            *pfEaten = TRUE;
        }
        return S_OK;
    }

    // ── Backspace ──
    if (vk == VK_BACK) {
        if (!m_engine.m_buffer.empty()) {
            m_engine.backspace();
            if (m_engine.m_buffer.empty()) {
                cancelComposition();
                hideCandidateWindow();
            } else {
                showCandidateWindow();
                // 更新组合文本
                if (m_bComposing && pContext) {
                    ITfDocumentMgr* pDocMgr = nullptr;
                    if (SUCCEEDED(m_pThreadMgr->GetFocus(&pDocMgr)) && pDocMgr) {
                        ITfContext* pCtx = nullptr;
                        if (SUCCEEDED(pDocMgr->GetTop(&pCtx)) && pCtx) {
                            HRESULT hrSession = S_OK;
                            if (SUCCEEDED(pCtx->RequestEditSession(m_clientId,
                                    new CPinyinEditSession(this),
                                    TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hrSession))) {
                                // ec comes from DoEditSession callback, not here
                                updateComposition(pCtx, (TfEditCookie)0);
                            }
                            pCtx->Release();
                        }
                        pDocMgr->Release();
                    }
                }
            }
        }
        *pfEaten = TRUE;
        return S_OK;
    }

    // ── 数字键 1-9: 选择候选 ──
    if (vk >= '1' && vk <= '9' && !m_engine.m_candidates.empty()) {
        int idx = (int)(vk - '1');
        handleKeyCommit(idx);
        *pfEaten = TRUE;
        return S_OK;
    }

    // ── 空格: 确认第一个候选 ──
    if (vk == VK_SPACE && !m_engine.m_candidates.empty()) {
        handleKeyCommit(0);
        *pfEaten = TRUE;
        return S_OK;
    }

    // 空格无候选且 buffer 空: 放行
    if (vk == VK_SPACE && m_engine.m_buffer.empty()) {
        return S_OK;
    }

    // ── 撇号: 分词符 ──
    if (vk == VK_OEM_7 && !m_engine.m_buffer.empty()) {
        m_engine.addChar('\'');
        showCandidateWindow();
        // 扩展组合文本
        if (m_bComposing && pContext) {
            ITfDocumentMgr* pDocMgr = nullptr;
            if (SUCCEEDED(m_pThreadMgr->GetFocus(&pDocMgr)) && pDocMgr) {
                ITfContext* pCtx = nullptr;
                if (SUCCEEDED(pDocMgr->GetTop(&pCtx)) && pCtx) {
                    HRESULT hrSession = S_OK;
                    pCtx->RequestEditSession(m_clientId,
                            new CPinyinEditSession(this),
                            TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hrSession);
                    pCtx->Release();
                }
                pDocMgr->Release();
            }
        }
        *pfEaten = FALSE;  // 放行到应用显示
        return S_OK;
    }

    // ── - / PageUp: 上一页 ──
    if (vk == VK_OEM_MINUS || vk == VK_PRIOR) {
        if (!m_engine.m_candidates.empty()) {
            m_engine.prevPage();
            showCandidateWindow();
            *pfEaten = TRUE;
        }
        return S_OK;
    }

    // ── = / PageDown: 下一页 ──
    if (vk == VK_OEM_PLUS || vk == VK_NEXT) {
        if (!m_engine.m_candidates.empty()) {
            m_engine.nextPage();
            showCandidateWindow();
            *pfEaten = TRUE;
        }
        return S_OK;
    }

    // ── 字母键 A-Z: 加入拼音缓冲 ──
    if (vk >= 'A' && vk <= 'Z') {
        bool capsLock = (GetKeyState(VK_CAPITAL) & 1) != 0;
        bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

        if (capsLock) return S_OK;  // Caps Lock: 放行英文

        char c = shiftDown ? (char)vk : (char)(vk + 32);
        m_engine.addChar(c);
        showCandidateWindow();

        // 开始或扩展 TSF 组合 (内联显示拼音)
        if (pContext) {
            ITfDocumentMgr* pDocMgr = nullptr;
            if (SUCCEEDED(m_pThreadMgr->GetFocus(&pDocMgr)) && pDocMgr) {
                ITfContext* pCtx = nullptr;
                if (SUCCEEDED(pDocMgr->GetTop(&pCtx)) && pCtx) {
                    HRESULT hrSession = S_OK;
                    m_pendingAction = m_bComposing ? ACT_UPDATE : ACT_START;
                    pCtx->RequestEditSession(m_clientId,
                            new CPinyinEditSession(this),
                            TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hrSession);
                    pCtx->Release();
                }
                pDocMgr->Release();
            }
        }

        *pfEaten = TRUE;   // 吃掉字母键, 仅通过 TSF composition 显示内联拼音
        return S_OK;
    }

    // ── 其他键: 自动提交首候选 ──
    if (!m_engine.m_buffer.empty() && !m_engine.m_candidates.empty()) {
        handleKeyCommit(0);
        return S_OK;
    }

    // ── 其他键: 无候选则取消 ──
    if (!m_engine.m_buffer.empty()) {
        cancelComposition();
        m_engine.clear();
        hideCandidateWindow();
        return S_OK;
    }

    return S_OK;
}

// ITfKeyEventSink::OnSetFocus — 焦点丢失时清理候选窗口
STDMETHODIMP CPinyinTextService::OnSetFocus(BOOL fForeground) {
    if (!fForeground) {
        // 失去焦点: 取消组合 + 隐藏候选窗口
        if (!m_engine.m_buffer.empty()) {
            m_engine.clear();
        }
        if (m_bComposing) {
            cancelComposition();
        }
        hideCandidateWindow();
    }
    return S_OK;
}

// ==================== ITfCompositionSink ====================
STDMETHODIMP CPinyinTextService::OnCompositionTerminated(TfEditCookie, ITfComposition* pComposition) {
    if (m_pComposition == pComposition) {
        m_pComposition->Release();
        m_pComposition = nullptr;
        m_bComposing = false;
    }
    return S_OK;
}

// ==================== ITfDisplayAttributeProvider ====================
STDMETHODIMP CPinyinTextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) {
    if (!ppEnum) return E_POINTER;
    auto* pInfo = new (std::nothrow) CPinyinDisplayAttributeInfo(&m_settings);
    if (!pInfo) return E_OUTOFMEMORY;
    *ppEnum = new (std::nothrow) CPinyinDisplayAttributeEnum(pInfo);
    pInfo->Release();
    return (*ppEnum) ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CPinyinTextService::GetDisplayAttributeInfo(REFGUID guidInfo,
        ITfDisplayAttributeInfo** ppInfo) {
    if (!ppInfo) return E_POINTER;
    *ppInfo = nullptr;

    if (guidInfo == GUID_PinyinDisplayAttribute) {
        *ppInfo = new (std::nothrow) CPinyinDisplayAttributeInfo(&m_settings);
        return (*ppInfo) ? S_OK : E_OUTOFMEMORY;
    }
    return E_FAIL;
}

// ==================== ITfCompartmentEventSink ====================
STDMETHODIMP CPinyinTextService::OnChange(REFGUID rguid) {
    // 键盘开关状态变化 (系统级 IME 开关)
    if (rguid == GUID_COMPARTMENT_KEYBOARD_OPENCLOSE) {
        ITfCompartmentMgr* pCompMgr = nullptr;
        if (SUCCEEDED(m_pThreadMgr->QueryInterface(IID_ITfCompartmentMgr, (void**)&pCompMgr))) {
            ITfCompartment* pComp = nullptr;
            if (SUCCEEDED(pCompMgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &pComp))) {
                VARIANT var;
                VariantInit(&var);
                if (SUCCEEDED(pComp->GetValue(&var)) && var.vt == VT_I4) {
                    // 如果系统关闭了 IME 而我们正在组合中, 提交残留文本
                    if (!var.lVal && m_bComposing) {
                        if (!m_engine.m_buffer.empty()) {
                            std::string rawText = m_engine.m_buffer;
                            rawText.erase(std::remove(rawText.begin(), rawText.end(), '\''), rawText.end());
                            m_engine.clear();
                            hideCandidateWindow();
                            // 延迟提交 (Async)
                            m_pendingCommit = utf8ToWide(rawText);
                            m_pendingAction = ACT_COMMIT;
                            ITfDocumentMgr* pDocMgr = nullptr;
                            if (SUCCEEDED(m_pThreadMgr->GetFocus(&pDocMgr)) && pDocMgr) {
                                ITfContext* pCtx = nullptr;
                                if (SUCCEEDED(pDocMgr->GetTop(&pCtx)) && pCtx) {
                                    HRESULT hrSession = S_OK;
                                    pCtx->RequestEditSession(m_clientId,
                                        new CPinyinEditSession(this),
                                        TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hrSession);
                                    pCtx->Release();
                                }
                                pDocMgr->Release();
                            }
                        }
                    }
                }
                VariantClear(&var);
                pComp->Release();
            }
            pCompMgr->Release();
        }
    }

    // 输入模式转换变化 (例如日文输入法在全角/半角间切换)
    if (rguid == GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION) {
        // 中文拼音输入法通常不需要处理此事件, 但提供钩子供未来扩展
    }

    return S_OK;
}

// ==================== 组合管理 ====================
void CPinyinTextService::startComposition(ITfContext* pContext, TfEditCookie ec) {
    if (m_bComposing) return;

    // 获取当前选区
    TF_SELECTION tfSel;
    ULONG cFetched = 0;
    if (FAILED(pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSel, &cFetched)) || cFetched == 0) {
        return;
    }

    // StartComposition 在 ITfContextComposition 接口上
    ITfContextComposition* pCompCtx = nullptr;
    if (SUCCEEDED(pContext->QueryInterface(IID_ITfContextComposition, (void**)&pCompCtx)) && pCompCtx) {
        ITfComposition* pComp = nullptr;
        if (SUCCEEDED(pCompCtx->StartComposition(ec, tfSel.range,
                static_cast<ITfCompositionSink*>(this), &pComp)) && pComp) {
            m_pComposition = pComp;
            m_bComposing = true;

            // 将拼音缓冲区写入组合范围
            std::wstring wBuf = utf8ToWide(m_engine.m_buffer);
            tfSel.range->SetText(ec, 0, wBuf.c_str(), (LONG)wBuf.size());

            // 移动选区到末尾
            tfSel.range->Collapse(ec, TF_ANCHOR_END);
            pContext->SetSelection(ec, 1, &tfSel);
        }
        pCompCtx->Release();
    }

    if (tfSel.range) tfSel.range->Release();
}

void CPinyinTextService::updateComposition(ITfContext* pContext, TfEditCookie ec) {
    if (!m_bComposing || !m_pComposition) {
        // 可能组合被意外终止, 重新开始
        startComposition(pContext, ec);
        return;
    }

    ITfRange* pRange = nullptr;
    if (SUCCEEDED(m_pComposition->GetRange(&pRange)) && pRange) {
        std::wstring wBuf = utf8ToWide(m_engine.m_buffer);
        pRange->SetText(ec, 0, wBuf.c_str(), (LONG)wBuf.size());

        // 移动选区到末尾
        pRange->Collapse(ec, TF_ANCHOR_END);
        TF_SELECTION tfSel;
        tfSel.range = pRange;
        tfSel.style.ase = TF_AE_END;
        tfSel.style.fInterimChar = FALSE;
        pContext->SetSelection(ec, 1, &tfSel);

        pRange->Release();
    }
}

void CPinyinTextService::commitComposition(const std::wstring& text) {
    m_pendingCommit = text;
    m_pendingAction = ACT_COMMIT;

    ITfDocumentMgr* pDocMgr = nullptr;
    if (SUCCEEDED(m_pThreadMgr->GetFocus(&pDocMgr)) && pDocMgr) {
        ITfContext* pContext = nullptr;
        if (SUCCEEDED(pDocMgr->GetTop(&pContext)) && pContext) {
            HRESULT hrSession = S_OK;
            pContext->RequestEditSession(m_clientId,
                new CPinyinEditSession(this),
                TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hrSession);
            pContext->Release();
        }
        pDocMgr->Release();
    }
}

void CPinyinTextService::cancelComposition() {
    m_pendingCommit = L"";
    m_pendingAction = ACT_CANCEL;

    ITfDocumentMgr* pDocMgr = nullptr;
    if (SUCCEEDED(m_pThreadMgr->GetFocus(&pDocMgr)) && pDocMgr) {
        ITfContext* pContext = nullptr;
        if (SUCCEEDED(pDocMgr->GetTop(&pContext)) && pContext) {
            HRESULT hrSession = S_OK;
            pContext->RequestEditSession(m_clientId,
                new CPinyinEditSession(this),
                TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hrSession);
            pContext->Release();
        }
        pDocMgr->Release();
    }
}

// ==================== ITfEditSession (在 text_service.h 中声明) ====================
STDMETHODIMP CPinyinEditSession::DoEditSession(TfEditCookie ec) {
    if (!m_pService) return E_FAIL;

    CPinyinTextService* svc = m_pService;

    // 获取当前聚焦的上下文 (标准 TSF 模式: 在 edit session 内通过 thread manager 获取)
    ITfDocumentMgr* pDocMgr = nullptr;
    ITfContext* pContext = nullptr;
    if (SUCCEEDED(svc->m_pThreadMgr->GetFocus(&pDocMgr)) && pDocMgr) {
        pDocMgr->GetTop(&pContext);
        pDocMgr->Release();
    }

    // 处理 COMMIT / CANCEL
    if (svc->m_pendingAction == CPinyinTextService::ACT_COMMIT ||
        svc->m_pendingAction == CPinyinTextService::ACT_CANCEL) {
        if (svc->m_bComposing && svc->m_pComposition) {
            ITfRange* pRange = nullptr;
            if (SUCCEEDED(svc->m_pComposition->GetRange(&pRange)) && pRange) {
                if (!svc->m_pendingCommit.empty()) {
                    pRange->SetText(ec, 0,
                        svc->m_pendingCommit.c_str(),
                        (LONG)svc->m_pendingCommit.size());
                } else {
                    pRange->SetText(ec, 0, L"", 0);
                }
                pRange->Release();
            }
            svc->m_pComposition->EndComposition(ec);
            svc->m_pComposition->Release();
            svc->m_pComposition = nullptr;
            svc->m_bComposing = false;
        }
        svc->m_pendingCommit.clear();
    }

    // 处理 START / UPDATE composition (使用 edit session 内获取的 context)
    if (svc->m_pendingAction == CPinyinTextService::ACT_START) {
        if (pContext) {
            svc->startComposition(pContext, ec);
        }
    } else if (svc->m_pendingAction == CPinyinTextService::ACT_UPDATE) {
        if (pContext) {
            svc->updateComposition(pContext, ec);
        }
    }

    // 清理 context
    if (pContext) pContext->Release();

    svc->m_pendingAction = CPinyinTextService::ACT_NONE;
    return S_OK;
}

// ==================== 辅助方法 ====================
void CPinyinTextService::loadSettings() {
    std::string dir = getModuleDirectory(g_hDllInst);
    if (!m_settings.loadFromFile(dir + "pinyin_config.ini")) {
        // 首次运行, 使用默认值 (已初始化)
    }
}

void CPinyinTextService::showCandidateWindow() {
    if (m_candidateWin.m_hwnd) {
        m_candidateWin.show(
            m_engine.getPageCandidates(),
            m_engine.m_pageIndex);
    }
}

void CPinyinTextService::hideCandidateWindow() {
    if (m_candidateWin.m_hwnd) {
        m_candidateWin.hide();
    }
}

void CPinyinTextService::handleKeyCommit(int candidateIndex) {
    std::string text = m_engine.selectCandidate(candidateIndex);
    if (!text.empty()) {
        hideCandidateWindow();
        commitComposition(utf8ToWide(text));
    }
}
