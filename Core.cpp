#include "Core.h"
#include <initguid.h>  // For DEFINE_GUID

// Define IIDs for newer Direct2D factory interfaces for runtime detection
// These are used only for QueryInterface, avoiding compile-time dependencies
DEFINE_GUID(IID_ID2D1Factory2_, 0x94f81a73, 0x9212, 0x4376, 0x9c, 0x58, 0xb1, 0x6a, 0x3a, 0x0d, 0x39, 0x92);
DEFINE_GUID(IID_ID2D1Factory3_, 0x0869759f, 0x4f00, 0x413f, 0xb0, 0x3e, 0x2b, 0xda, 0x45, 0x40, 0x4d, 0x0f);
DEFINE_GUID(IID_ID2D1Factory4_, 0xbd4ec2d2, 0x8892, 0x4c88, 0xbb, 0x7a, 0x9b, 0x3f, 0x0a, 0x31, 0xc9, 0x71);
DEFINE_GUID(IID_ID2D1Factory5_, 0xc4349994, 0x838e, 0x4b0f, 0x8c, 0xab, 0x44, 0x97, 0xd9, 0xee, 0xcc, 0xb1);

namespace FD2D
{
    bool Core::s_initialized = false;
    HINSTANCE Core::s_instance = nullptr;
    D2DVersion Core::s_d2dVersion = D2DVersion::D2D1_0;
    ComPtr<ID2D1Factory> Core::s_d2dFactory {};
    ComPtr<ID2D1Factory1> Core::s_d2dFactory1 {};
    ComPtr<IDWriteFactory> Core::s_dwriteFactory {};

    HRESULT Core::DetectD2DVersion()
    {
        if (!s_d2dFactory1)
        {
            if (s_d2dFactory)
            {
                s_d2dVersion = D2DVersion::D2D1_0;
                return S_OK;
            }
            return E_FAIL;
        }

        // Try to query for the highest available Direct2D version at runtime
        // Query interfaces from newest to oldest to find the highest supported version
        // Use QueryInterface with IID GUIDs directly to avoid compile-time dependencies
        
        // Try Direct2D 1.5 (Windows 10 October 2018 Update+)
        {
            void* factory5 = nullptr;
            HRESULT hr = s_d2dFactory1->QueryInterface(IID_ID2D1Factory5_, &factory5);
            if (SUCCEEDED(hr) && factory5)
            {
                reinterpret_cast<IUnknown*>(factory5)->Release();
                s_d2dVersion = D2DVersion::D2D1_5;
                return S_OK;
            }
        }
        
        // Try Direct2D 1.4 (Windows 10 Creators Update+)
        {
            void* factory4 = nullptr;
            HRESULT hr = s_d2dFactory1->QueryInterface(IID_ID2D1Factory4_, &factory4);
            if (SUCCEEDED(hr) && factory4)
            {
                reinterpret_cast<IUnknown*>(factory4)->Release();
                s_d2dVersion = D2DVersion::D2D1_4;
                return S_OK;
            }
        }
        
        // Try Direct2D 1.3 (Windows 10+)
        {
            void* factory3 = nullptr;
            HRESULT hr = s_d2dFactory1->QueryInterface(IID_ID2D1Factory3_, &factory3);
            if (SUCCEEDED(hr) && factory3)
            {
                reinterpret_cast<IUnknown*>(factory3)->Release();
                s_d2dVersion = D2DVersion::D2D1_3;
                return S_OK;
            }
        }
        
        // Try Direct2D 1.2 (Windows 8.1+)
        {
            void* factory2 = nullptr;
            HRESULT hr = s_d2dFactory1->QueryInterface(IID_ID2D1Factory2_, &factory2);
            if (SUCCEEDED(hr) && factory2)
            {
                reinterpret_cast<IUnknown*>(factory2)->Release();
                s_d2dVersion = D2DVersion::D2D1_2;
                return S_OK;
            }
        }
        
        // Direct2D 1.1 (Windows 8+) - we already have ID2D1Factory1
        s_d2dVersion = D2DVersion::D2D1_1;
        return S_OK;
    }

    HRESULT Core::Initialize(const InitContext& context)
    {
        if (s_initialized)
        {
            return S_FALSE;
        }

        s_instance = context.instance;
        // COM lifetime is owned by the application (FICture2).
        // FD2D assumes COM is already initialized on the calling thread.
        
        // D2D1CreateFactory returns the highest available version at runtime
        // We create with IID_ID2D1Factory1 first to get 1.1+ features if available
        HRESULT hr = D2D1CreateFactory(context.factoryType, IID_PPV_ARGS(&s_d2dFactory1));
        if (FAILED(hr))
        {
            // Fallback to Direct2D 1.0 if 1.1 is not available
            hr = D2D1CreateFactory(context.factoryType, IID_PPV_ARGS(&s_d2dFactory));
            if (FAILED(hr))
            {
                Shutdown();
                return hr;
            }
            s_d2dVersion = D2DVersion::D2D1_0;
        }
        else
        {
            // Query for base factory interface
            hr = s_d2dFactory1.As(&s_d2dFactory);
            if (FAILED(hr))
            {
                Shutdown();
                return hr;
            }
            
            // Detect the highest available version at runtime
            hr = DetectD2DVersion();
            if (FAILED(hr))
            {
                // Default to 1.1 if detection fails
                s_d2dVersion = D2DVersion::D2D1_1;
            }
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
        s_d2dFactory1.Reset();
        s_d2dFactory.Reset();

        s_instance = nullptr;
        s_initialized = false;
    }

    ID2D1Factory* Core::D2DFactory()
    {
        return s_d2dFactory.Get();
    }

    ID2D1Factory1* Core::D2DFactory1()
    {
        return s_d2dFactory1.Get();
    }

    IDWriteFactory* Core::DWriteFactory()
    {
        return s_dwriteFactory.Get();
    }

    HINSTANCE Core::Instance()
    {
        return s_instance;
    }

    D2DVersion Core::GetSupportedD2DVersion()
    {
        return s_d2dVersion;
    }

    const char* Core::GetD2DVersionString()
    {
        switch (s_d2dVersion)
        {
        case D2DVersion::D2D1_0:
            return "Direct2D 1.0 (Windows 7+)";
        case D2DVersion::D2D1_1:
            return "Direct2D 1.1 (Windows 8+)";
        case D2DVersion::D2D1_2:
            return "Direct2D 1.2 (Windows 8.1+)";
        case D2DVersion::D2D1_3:
            return "Direct2D 1.3 (Windows 10+)";
        case D2DVersion::D2D1_4:
            return "Direct2D 1.4 (Windows 10 Creators Update+)";
        case D2DVersion::D2D1_5:
            return "Direct2D 1.5 (Windows 10 October 2018 Update+)";
        default:
            return "Unknown Direct2D Version";
        }
    }
}


