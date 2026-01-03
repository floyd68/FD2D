#include "Core.h"

namespace FD2D
{
    bool Core::s_initialized = false;
    bool Core::s_coInitialized = false;
    HINSTANCE Core::s_instance = nullptr;
    ComPtr<ID2D1Factory> Core::s_d2dFactory {};
    ComPtr<IDWriteFactory> Core::s_dwriteFactory {};
    ComPtr<IWICImagingFactory> Core::s_wicFactory {};

    HRESULT Core::Initialize(const InitContext& context)
    {
        if (s_initialized)
        {
            return S_FALSE;
        }

        s_instance = context.instance;

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr))
        {
            s_coInitialized = true;
        }
        else if (hr == RPC_E_CHANGED_MODE)
        {
            s_coInitialized = false;
        }
        else
        {
            return hr;
        }

        hr = D2D1CreateFactory(context.factoryType, IID_PPV_ARGS(&s_d2dFactory));
        if (FAILED(hr))
        {
            Shutdown();
            return hr;
        }

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &s_dwriteFactory);
        if (FAILED(hr))
        {
            Shutdown();
            return hr;
        }

        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s_wicFactory));
        if (FAILED(hr))
        {
            Shutdown();
            return hr;
        }

        s_initialized = true;
        return S_OK;
    }

    void Core::Shutdown()
    {
        s_wicFactory.Reset();
        s_dwriteFactory.Reset();
        s_d2dFactory.Reset();

        if (s_coInitialized)
        {
            CoUninitialize();
            s_coInitialized = false;
        }

        s_instance = nullptr;
        s_initialized = false;
    }

    ID2D1Factory* Core::D2DFactory()
    {
        return s_d2dFactory.Get();
    }

    IDWriteFactory* Core::DWriteFactory()
    {
        return s_dwriteFactory.Get();
    }

    IWICImagingFactory* Core::WicFactory()
    {
        return s_wicFactory.Get();
    }

    HINSTANCE Core::Instance()
    {
        return s_instance;
    }
}

