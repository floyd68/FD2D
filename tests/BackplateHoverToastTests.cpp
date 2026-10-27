#include "TestHarness.h"
#include "BackplateTestAccess.h"
#include "WindowlessTestSupport.h"

#include "Backplate.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <string>

// Backplate's hover tooltip and toast (drawn by Backplate::DrawHoverAndToast,
// not Text controls).
//
// Resources: as in GraphicsGenerationTests, two WIC render targets from
// different D2D factories stand in for "before" and "after" a target
// recreation; a brush from the first fails EndDraw on the second.

using Microsoft::WRL::ComPtr;
using FD2D::BackplateTestAccess;

namespace
{
    constexpr UINT kCanvasW = 160;
    constexpr UINT kCanvasH = 96;

    // A software render target on its own D2D factory (= its own resource domain).
    struct Canvas
    {
        ComPtr<ID2D1Factory> d2d;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<ID2D1RenderTarget> target;

        bool Create(IWICImagingFactory* wic)
        {
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
                FAILED(wic->CreateBitmap(kCanvasW, kCanvasH, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)))
            {
                return false;
            }
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            return SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target));
        }

        HRESULT DrawHoverAndToast(FD2D::Backplate& backplate)
        {
            target->BeginDraw();
            target->Clear(D2D1::ColorF(0, 0, 0, 1));
            BackplateTestAccess::DrawHoverAndToast(backplate, target.Get());
            return target->EndDraw();
        }

        // Pixels of the tooltip box fill, the toast box fill and the (near
        // white) text.
        struct Counts
        {
            int tooltip { 0 };
            int toast { 0 };
            int text { 0 };
        };

        Counts Count() const
        {
            Counts counts;
            WICRect rect { 0, 0, static_cast<INT>(kCanvasW), static_cast<INT>(kCanvasH) };
            ComPtr<IWICBitmapLock> lock;
            if (FAILED(bitmap->Lock(&rect, WICBitmapLockRead, &lock)))
            {
                return counts;
            }
            UINT stride = 0;
            UINT size = 0;
            BYTE* data = nullptr;
            lock->GetStride(&stride);
            lock->GetDataPointer(&size, &data);
            for (UINT y = 0; y < kCanvasH; ++y)
            {
                for (UINT x = 0; x < kCanvasW; ++x)
                {
                    const BYTE* p = data + y * stride + x * 4u;
                    const int b = p[0];
                    const int g = p[1];
                    const int r = p[2];
                    // Tooltip fill (0.12, 0.12, 0.14) at 97%: about (30, 30, 35).
                    if (r >= 22 && r <= 38 && g >= 22 && g <= 38 && b >= 27 && b <= 43)
                    {
                        ++counts.tooltip;
                    }
                    // Toast fill (0.16, 0.34, 0.58) at 97%: about (40, 84, 143).
                    if (r >= 32 && r <= 48 && g >= 76 && g <= 92 && b >= 135 && b <= 151)
                    {
                        ++counts.toast;
                    }
                    if (r >= 200 && g >= 200 && b >= 200)
                    {
                        ++counts.text;
                    }
                }
            }
            return counts;
        }
    };

    struct Fixture
    {
        FD2DWindowlessTest::ComScope com;
        FD2DWindowlessTest::CoreScope core; // DirectWrite factory
        ComPtr<IWICImagingFactory> wic;
        Canvas a;
        Canvas b;

        bool Create()
        {
            return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                a.Create(wic.Get()) && b.Create(wic.Get());
        }
    };

    void PrintCounts(const char* label, HRESULT hr, const Canvas::Counts& counts)
    {
        std::printf("  %-34s hr 0x%08X, tooltip fill %4d px, toast fill %4d px, text %3d px\n",
            label, static_cast<unsigned>(hr), counts.tooltip, counts.toast, counts.text);
    }

    bool Visible(const Canvas::Counts& counts)
    {
        // Both boxes (each well over 20 x 20 px) with text in them.
        return counts.tooltip > 400 && counts.toast > 400 && counts.text > 20;
    }
}

FD2D_TEST(BackplateHoverToast_ReusesBrushAndLayoutsAcrossFrames)
{
    Fixture fx;
    FD2D_REQUIRE(fx.Create());
    FD2D::Backplate backplate(L"hover-toast-test");
    BackplateTestAccess::SetClientSize(backplate, kCanvasW, kCanvasH);
    // Tooltip box at (12, 8) (anchor + (12, 20)); toast centered near the bottom.
    BackplateTestAccess::ShowTooltipNow(backplate, L"Tooltip text", POINT { 0, -12 });
    backplate.ShowToast(L"Toast text");

    HRESULT hr = fx.a.DrawHoverAndToast(backplate);
    Canvas::Counts counts = fx.a.Count();
    PrintCounts("first frame", hr, counts);
    FD2D_CHECK(SUCCEEDED(hr));
    FD2D_CHECK(Visible(counts));

    // Held here, so a rebuilt layout cannot reuse the address.
    const ComPtr<IDWriteTextLayout> tooltipLayout = BackplateTestAccess::TooltipLayout(backplate);
    const ComPtr<IDWriteTextLayout> toastLayout = BackplateTestAccess::ToastLayout(backplate);
    const ComPtr<ID2D1SolidColorBrush> brush = BackplateTestAccess::TipBrush(backplate);
    FD2D_REQUIRE(tooltipLayout && toastLayout && brush);

    // Later frames create neither a brush nor a layout.
    for (int frame = 0; frame < 3; ++frame)
    {
        FD2D_CHECK(SUCCEEDED(fx.a.DrawHoverAndToast(backplate)));
        FD2D_CHECK(BackplateTestAccess::TipBrush(backplate) == brush.Get());
        FD2D_CHECK(BackplateTestAccess::TooltipLayout(backplate) == tooltipLayout.Get());
        FD2D_CHECK(BackplateTestAccess::ToastLayout(backplate) == toastLayout.Get());
    }

    // Same generation, other resource domain: the cached brush fails EndDraw.
    hr = fx.b.DrawHoverAndToast(backplate);
    std::printf("  other domain, same generation: hr 0x%08X\n", static_cast<unsigned>(hr));
    FD2D_CHECK(FAILED(hr));
}

FD2D_TEST(BackplateHoverToast_TargetRecreationRecreatesBrushKeepsLayouts)
{
    Fixture fx;
    FD2D_REQUIRE(fx.Create());
    FD2D::Backplate backplate(L"hover-toast-test");
    BackplateTestAccess::SetClientSize(backplate, kCanvasW, kCanvasH);
    BackplateTestAccess::ShowTooltipNow(backplate, L"Tooltip text", POINT { 0, -12 });
    backplate.ShowToast(L"Toast text");
    FD2D_REQUIRE(SUCCEEDED(fx.a.DrawHoverAndToast(backplate)));
    const ComPtr<IDWriteTextLayout> tooltipLayout = BackplateTestAccess::TooltipLayout(backplate);
    const ComPtr<IDWriteTextLayout> toastLayout = BackplateTestAccess::ToastLayout(backplate);
    FD2D_REQUIRE(tooltipLayout && toastLayout);

    // A forced target recreation drops the brush (Backplate's own graphics
    // invalidation), keeps the layouts, and the next frame - on the new
    // target - draws both boxes and their text again.
    BackplateTestAccess::TargetRecreated(backplate);
    FD2D_CHECK(BackplateTestAccess::TipBrush(backplate) == nullptr);
    HRESULT hr = fx.b.DrawHoverAndToast(backplate);
    Canvas::Counts counts = fx.b.Count();
    PrintCounts("after target recreation", hr, counts);
    FD2D_CHECK(SUCCEEDED(hr));
    FD2D_CHECK(Visible(counts));
    FD2D_CHECK(BackplateTestAccess::TipBrush(backplate) != nullptr);
    FD2D_CHECK(BackplateTestAccess::TooltipLayout(backplate) == tooltipLayout.Get());
    FD2D_CHECK(BackplateTestAccess::ToastLayout(backplate) == toastLayout.Get());

    // The same after a device loss (device and target generations).
    BackplateTestAccess::DeviceLost(backplate);
    FD2D_CHECK(BackplateTestAccess::TipBrush(backplate) == nullptr);
    hr = fx.a.DrawHoverAndToast(backplate);
    counts = fx.a.Count();
    PrintCounts("after device loss", hr, counts);
    FD2D_CHECK(SUCCEEDED(hr));
    FD2D_CHECK(Visible(counts));
    FD2D_CHECK(BackplateTestAccess::TooltipLayout(backplate) == tooltipLayout.Get());
    FD2D_CHECK(BackplateTestAccess::ToastLayout(backplate) == toastLayout.Get());

    // New text rebuilds only that layout.
    backplate.ShowToast(L"Another toast");
    FD2D_CHECK(SUCCEEDED(fx.a.DrawHoverAndToast(backplate)));
    FD2D_CHECK(BackplateTestAccess::ToastLayout(backplate) != toastLayout.Get());
    FD2D_CHECK(BackplateTestAccess::TooltipLayout(backplate) == tooltipLayout.Get());
}
