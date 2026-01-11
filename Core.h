#pragma once

#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
// Try to include newer Direct2D headers if available (compile-time check)
#if defined(__has_include)
    #if __has_include(<d2d1_2.h>)
        #include <d2d1_2.h>
    #endif
    #if __has_include(<d2d1_3.h>)
        #include <d2d1_3.h>
    #endif
    #if __has_include(<d2d1_4.h>)
        #include <d2d1_4.h>
    #endif
#else
    // Fallback: only include what we know exists in most SDKs
    // Newer versions will be detected via QueryInterface at runtime
#endif
#include <dwrite.h>
#include <wrl/client.h>

namespace FD2D
{
    using Microsoft::WRL::ComPtr;

    struct InitContext
    {
        HINSTANCE instance { nullptr };
        D2D1_FACTORY_TYPE factoryType { D2D1_FACTORY_TYPE_MULTI_THREADED };
    };

    enum class D2DVersion
    {
        D2D1_0,  // Direct2D 1.0 (Windows 7+)
        D2D1_1,  // Direct2D 1.1 (Windows 8+)
        D2D1_2,  // Direct2D 1.2 (Windows 8.1+)
        D2D1_3,  // Direct2D 1.3 (Windows 10+)
        D2D1_4,  // Direct2D 1.4 (Windows 10 Creators Update+)
        D2D1_5   // Direct2D 1.5 (Windows 10 October 2018 Update+)
    };

    enum class BitmapSamplingMode
    {
        HighQuality,
        PixelPerfect
    };

    class Core
    {
    public:
        static HRESULT Initialize(const InitContext& context);
        static void Shutdown();

        static ID2D1Factory* D2DFactory();
        static ID2D1Factory1* D2DFactory1();
        static IDWriteFactory* DWriteFactory();
        static HINSTANCE Instance();
        
        // Get the highest supported Direct2D version at runtime
        static D2DVersion GetSupportedD2DVersion();
        static const char* GetD2DVersionString();

        // Bitmap sampling/filtering (renderer option)
        static BitmapSamplingMode GetBitmapSamplingMode();
        static void SetBitmapSamplingMode(BitmapSamplingMode mode);
        static BitmapSamplingMode ToggleBitmapSamplingMode();

    private:
        static HRESULT DetectD2DVersion();
        
        static bool s_initialized;
        static HINSTANCE s_instance;
        static D2DVersion s_d2dVersion;
        static BitmapSamplingMode s_bitmapSamplingMode;
        static ComPtr<ID2D1Factory> s_d2dFactory;
        static ComPtr<ID2D1Factory1> s_d2dFactory1;
        static ComPtr<IDWriteFactory> s_dwriteFactory;
    };
}


