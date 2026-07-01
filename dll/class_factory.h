// dll/class_factory.h — IClassFactory 实现
// 由 DllGetClassObject 创建, 用于实例化 CPinyinTextService
#pragma once
#include <windows.h>
#include <new>
#include "text_service.h"  // CPinyinTextService 完整定义

extern HINSTANCE g_hDllInst;
extern LONG g_cDllRef;

class CPinyinClassFactory : public IClassFactory {
private:
    LONG m_cRef;

public:
    CPinyinClassFactory() : m_cRef(1) {
        InterlockedIncrement(&g_cDllRef);
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG) Release() override {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) {
            InterlockedDecrement(&g_cDllRef);
            delete this;
        }
        return c;
    }

    // IClassFactory
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (pUnkOuter != nullptr) return CLASS_E_NOAGGREGATION;

        CPinyinTextService* pService = new (std::nothrow) CPinyinTextService();
        if (!pService) return E_OUTOFMEMORY;

        HRESULT hr = pService->QueryInterface(riid, ppv);
        pService->Release();  // QueryInterface 已经 AddRef
        return hr;
    }

    STDMETHODIMP LockServer(BOOL fLock) override {
        if (fLock)
            InterlockedIncrement(&g_cDllRef);
        else
            InterlockedDecrement(&g_cDllRef);
        return S_OK;
    }
};
