#include "Core.h"

namespace FD2D
{
    bool Core::s_initialized = false;
    HINSTANCE Core::s_instance = nullptr;
    ComPtr<ID2D1Factory> Core::s_d2dFactory {};
    ComPtr<IDWriteFactory> Core::s_dwriteFactory {};

    HRESULT Core::Initialize(const InitContext& context)
    {
        if (s_initialized)
        {
            return S_FALSE;
        }

        s_instance = context.instance;
        // COM lifetime is owned by the application (FICture2).
        // FD2D assumes COM is already initialized on the calling thread.
        HRESULT hr = D2D1CreateFactory(context.factoryType, IID_PPV_ARGS(&s_d2dFactory));
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

        s_initialized = true;
        return S_OK;
    }

    void Core::Shutdown()
    {
        s_dwriteFactory.Reset();
        s_d2dFactory.Reset();

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

    HINSTANCE Core::Instance()
    {
        return s_instance;
    }
}


