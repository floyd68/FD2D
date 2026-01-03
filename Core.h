#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace FD2D
{
    using Microsoft::WRL::ComPtr;

    struct InitContext
    {
        HINSTANCE instance { nullptr };
        D2D1_FACTORY_TYPE factoryType { D2D1_FACTORY_TYPE_MULTI_THREADED };
    };

    class Core
    {
    public:
        static HRESULT Initialize(const InitContext& context);
        static void Shutdown();

        static ID2D1Factory* D2DFactory();
        static IDWriteFactory* DWriteFactory();
        static IWICImagingFactory* WicFactory();
        static HINSTANCE Instance();

    private:
        static bool s_initialized;
        static bool s_coInitialized;
        static HINSTANCE s_instance;
        static ComPtr<ID2D1Factory> s_d2dFactory;
        static ComPtr<IDWriteFactory> s_dwriteFactory;
        static ComPtr<IWICImagingFactory> s_wicFactory;
    };
}

