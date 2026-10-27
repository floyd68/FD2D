#include "TestHarness.h"
#include "WindowlessTestSupport.h"

#include "Backplate.h"
#include "Button.h"
#include "Core.h"
#include "CheckBox.h"
#include "ComboBox.h"
#include "Image.h"
#include "ScrollView.h"
#include "Slider.h"
#include "Spinner.h"
#include "Splitter.h"
#include "Text.h"

#include <windows.h>
#include <d2d1.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Graphics generations decide resource validity. Windowless: two WIC render
// targets from *different* D2D factories stand in for "before" and "after" a
// target/device recreation - a resource from the first fails on the second
// (D2DERR_WRONG_FACTORY at EndDraw), exactly like stale resources after a
// real recreation. The backplate's generations are changed through
// BackplateTestAccess, as Backplate itself does on recreation.

namespace FD2D
{
    struct BackplateTestAccess
    {
        static void TargetRecreated(Backplate& backplate)
        {
            backplate.InvalidateGraphics(GraphicsInvalidationReason::TargetRecreated, false, true, false);
        }

        static void DeviceLost(Backplate& backplate)
        {
            backplate.DiscardDeviceResources();
            backplate.InvalidateGraphics(GraphicsInvalidationReason::DeviceLost, true, true, false);
        }
    };
}

using Microsoft::WRL::ComPtr;
using FD2D::BackplateTestAccess;

namespace
{
    struct ComScope
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope()
        {
            if (SUCCEEDED(hr))
            {
                CoUninitialize();
            }
        }
    };

    // A software render target on its own D2D factory (= its own resource domain).
    struct Canvas
    {
        ComPtr<ID2D1Factory> d2d;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<ID2D1RenderTarget> target;

        bool Create(IWICImagingFactory* wic)
        {
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
                FAILED(wic->CreateBitmap(48, 48, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)))
            {
                return false;
            }
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            return SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target));
        }

        HRESULT Draw(FD2D::Wnd& wnd)
        {
            target->BeginDraw();
            target->Clear(D2D1::ColorF(0, 0, 0, 1));
            wnd.OnRender(target.Get());
            return target->EndDraw();
        }

        ComPtr<ID2D1Bitmap> SolidBitmap()
        {
            const uint32_t pixels[4] = { 0x80FF0000u, 0x80FF0000u, 0x80FF0000u, 0x80FF0000u };
            ComPtr<ID2D1Bitmap> out;
            const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            (void)target->CreateBitmap(D2D1::SizeU(2, 2), pixels, 8, props, &out);
            return out;
        }
    };

    // Text needs Core's DirectWrite factory.
    struct CoreScope
    {
        HRESULT hr = S_FALSE;
        CoreScope()
        {
            FD2D::InitContext context {};
            context.instance = GetModuleHandleW(nullptr);
            hr = FD2D::Core::Initialize(context);
        }
        ~CoreScope()
        {
            if (hr == S_OK)
            {
                FD2D::Core::Shutdown();
            }
        }
    };

    struct Fixture
    {
        ComScope com;
        CoreScope core;
        ComPtr<IWICImagingFactory> wic;
        Canvas a;
        Canvas b;

        bool Create()
        {
            return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                a.Create(wic.Get()) && b.Create(wic.Get());
        }
    };

    // Content larger than its ScrollView's viewport, so both scrollbars show.
    class OversizeContent : public FD2D::Wnd
    {
    public:
        OversizeContent()
            : FD2D::Wnd(L"content")
        {
        }

        FD2D::Size Measure(FD2D::Size) override
        {
            m_desired = { 400.0f, 400.0f };
            return m_desired;
        }
    };

    std::shared_ptr<FD2D::Wnd> MakeScrollViewWithBars()
    {
        auto scroll = std::make_shared<FD2D::ScrollView>(L"scroll");
        scroll->SetScrollBarsVisible(true);
        scroll->SetContent(std::make_shared<OversizeContent>());
        scroll->Measure({ 48.0f, 48.0f });
        scroll->Arrange({ 0.0f, 0.0f, 48.0f, 48.0f });
        return scroll;
    }

    struct ControlCase
    {
        const char* name;
        std::function<std::shared_ptr<FD2D::Wnd>()> make;
    };

    std::vector<ControlCase> BrushCachingControls()
    {
        return {
            { "Text", []() { auto c = std::make_shared<FD2D::Text>(L"text"); c->SetText(L"Xy"); return std::shared_ptr<FD2D::Wnd>(c); } },
            { "Button", []() { return std::shared_ptr<FD2D::Wnd>(std::make_shared<FD2D::Button>(L"button")); } },
            { "CheckBox", []() { return std::shared_ptr<FD2D::Wnd>(std::make_shared<FD2D::CheckBox>(L"check")); } },
            { "ComboBox", []() { auto c = std::make_shared<FD2D::ComboBox>(L"combo"); c->SetItems({ L"one", L"two" }); return std::shared_ptr<FD2D::Wnd>(c); } },
            { "Slider", []() { return std::shared_ptr<FD2D::Wnd>(std::make_shared<FD2D::Slider>(L"slider")); } },
            { "Spinner", []() { auto c = std::make_shared<FD2D::Spinner>(L"spinner"); c->SetActive(true); return std::shared_ptr<FD2D::Wnd>(c); } },
            { "Splitter", []() { return std::shared_ptr<FD2D::Wnd>(std::make_shared<FD2D::Splitter>(L"splitter")); } },
            // Scrollbars only; the content draws nothing.
            { "ScrollView", []() { return MakeScrollViewWithBars(); } },
        };
    }

    ComPtr<ID3D11ShaderResourceView> MakeWarpSrv(ComPtr<ID3D11Device>& device)
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, nullptr)))
        {
            return nullptr;
        }
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = 4;
        desc.Height = 4;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture)) ||
            FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv)))
        {
            return nullptr;
        }
        return srv;
    }
}

FD2D_TEST(GraphicsGeneration_ControlBrushesFollowTargetGeneration)
{
    Fixture fx;
    FD2D_REQUIRE(fx.Create());

    for (const auto& control : BrushCachingControls())
    {
        FD2D::Backplate backplate(L"generation-test");
        auto wnd = control.make();
        FD2D_REQUIRE(backplate.AddWnd(wnd));
        wnd->SetLayoutRect(D2D1::RectF(0.0f, 0.0f, 48.0f, 48.0f));

        // Two frames: animated controls (Spinner) fade in before they draw.
        (void)fx.a.Draw(*wnd);
        Sleep(150);
        const HRESULT first = fx.a.Draw(*wnd);
        // Same generation: the control keeps its brushes, so drawing onto a
        // target from another resource domain fails (proves they are cached).
        const HRESULT staleWithoutBump = fx.b.Draw(*wnd);
        // After a target recreation the control rebuilds them for the new target.
        BackplateTestAccess::TargetRecreated(backplate);
        const HRESULT afterBump = fx.b.Draw(*wnd);
        // And again after a device loss (device + target generations).
        BackplateTestAccess::DeviceLost(backplate);
        const HRESULT afterDeviceLoss = fx.a.Draw(*wnd);

        std::printf("  %-9s first 0x%08X, other domain same generation 0x%08X, after target bump 0x%08X, after device loss 0x%08X\n",
            control.name, static_cast<unsigned>(first), static_cast<unsigned>(staleWithoutBump),
            static_cast<unsigned>(afterBump), static_cast<unsigned>(afterDeviceLoss));
        FD2D_CHECK(SUCCEEDED(first));
        FD2D_CHECK(FAILED(staleWithoutBump));
        FD2D_CHECK(SUCCEEDED(afterBump));
        FD2D_CHECK(SUCCEEDED(afterDeviceLoss));
    }
}

FD2D_TEST(GraphicsGeneration_ImageBitmapAndCheckerFollowTargetGeneration)
{
    Fixture fx;
    FD2D_REQUIRE(fx.Create());
    FD2D::Backplate backplate(L"generation-test");
    auto image = std::make_shared<FD2D::Image>(L"image");
    FD2D_REQUIRE(backplate.AddWnd(image));
    image->SetLayoutRect(D2D1::RectF(0.0f, 0.0f, 48.0f, 48.0f));
    FD2D::Image::DrawState state = image->GetDrawState();
    state.alphaCheckerboardEnabled = true;
    image->SetDrawState(state);

    image->SetBitmap(fx.a.SolidBitmap());
    FD2D_REQUIRE(image->ContentPixelSize().width == 2);
    FD2D_CHECK(SUCCEEDED(fx.a.Draw(*image)));

    // A target recreation drops the target-bound bitmap.
    BackplateTestAccess::TargetRecreated(backplate);
    FD2D_CHECK(image->ContentPixelSize().width == 0);
    FD2D_CHECK(SUCCEEDED(fx.b.Draw(*image)));

    // New content on the new target: the checkerboard brushes are rebuilt too.
    image->SetBitmap(fx.b.SolidBitmap());
    const HRESULT withChecker = fx.b.Draw(*image);
    std::printf("  checkerboard on the new target: 0x%08X\n", static_cast<unsigned>(withChecker));
    FD2D_CHECK(SUCCEEDED(withChecker));
}

FD2D_TEST(GraphicsGeneration_SrvSurvivesTargetButNotDeviceChange)
{
    ComScope com;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ShaderResourceView> srv = MakeWarpSrv(device);
    FD2D_REQUIRE(srv != nullptr);

    FD2D::Backplate backplate(L"generation-test");
    auto image = std::make_shared<FD2D::Image>(L"image");
    FD2D_REQUIRE(backplate.AddWnd(image));
    image->SetShaderResource(srv);
    FD2D_REQUIRE(image->ContentPixelSize().width == 4);

    BackplateTestAccess::TargetRecreated(backplate);
    FD2D_CHECK(image->ContentPixelSize().width == 4);   // same device: SRV kept

    BackplateTestAccess::DeviceLost(backplate);
    FD2D_CHECK(image->ContentPixelSize().width == 0);   // new device: SRV dropped
}

FD2D_TEST(GraphicsGeneration_DeviceCacheIsPerBackplateAndClearedWithDevice)
{
    FD2D::Backplate first(L"first");
    FD2D::Backplate second(L"second");
    static const char kKey = 0;
    auto entryA = std::make_shared<int>(1);
    auto entryB = std::make_shared<int>(2);
    first.SetDeviceCacheEntry(&kKey, entryA);
    second.SetDeviceCacheEntry(&kKey, entryB);

    // Each backplate keeps its own entry (no shared slot to thrash).
    FD2D_CHECK(first.DeviceCacheEntry(&kKey) == entryA);
    FD2D_CHECK(second.DeviceCacheEntry(&kKey) == entryB);

    // Discarding one backplate's device drops only its entries.
    BackplateTestAccess::DeviceLost(first);
    FD2D_CHECK(first.DeviceCacheEntry(&kKey) == nullptr);
    FD2D_CHECK(second.DeviceCacheEntry(&kKey) == entryB);
    FD2D_CHECK(entryA.use_count() == 1);
}

FD2D_TEST(GraphicsGeneration_WindowlessRenderAfterDeviceLost)
{
    FD2DWindowlessTest::ComScope com;
    FD2DWindowlessTest::CoreScope core;
    FD2D::Backplate backplate(L"generation-test");
    backplate.SetClearColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
    auto d3d = std::make_shared<FD2DWindowlessTest::ClearRectD3DWnd>(
        L"d3d", D2D1::RectF(0.0f, 0.0f, 16.0f, 16.0f), D2D1::ColorF(1.0f, 0.0f, 0.0f, 1.0f));
    auto d2d = std::make_shared<FD2DWindowlessTest::FillRectD2DWnd>(
        L"d2d", D2D1::RectF(8.0f, 0.0f, 24.0f, 16.0f), D2D1::ColorF(0.0f, 0.0f, 1.0f, 0.6f));
    FD2D_REQUIRE(SUCCEEDED(backplate.InitializeWindowless(32, 16)));
    FD2D_REQUIRE(backplate.AddWnd(d3d));
    FD2D_REQUIRE(backplate.AddWnd(d2d));

    const auto checkFrame = [&backplate](const char* label)
    {
        FD2DWindowlessTest::Frame frame;
        const HRESULT hr = frame.Read(backplate, D2D1::RectF(0.0f, 0.0f, 32.0f, 16.0f));
        std::printf("  %s: read 0x%08X\n", label, static_cast<unsigned>(hr));
        FD2D_REQUIRE(SUCCEEDED(hr));
        FD2D_CHECK(frame.Near(4, 8, FD2DWindowlessTest::Bgra { 0, 0, 255, 255 }));
        FD2D_CHECK(frame.Near(12, 8, FD2DWindowlessTest::Bgra { 153, 0, 102, 255 }));
        FD2D_CHECK(frame.Near(20, 8, FD2DWindowlessTest::Bgra { 153, 0, 0, 255 }));
        FD2D_CHECK(frame.Near(28, 8, FD2DWindowlessTest::Bgra { 0, 0, 0, 255 }));
    };

    backplate.Render();
    checkFrame("first frame");
    FD2D_CHECK(d2d->BrushesCreated() == 1);
    const FD2D::GraphicsGeneration before = backplate.GetGraphicsGeneration();

    // Device loss: the device stack and the composed frame are gone.
    BackplateTestAccess::DeviceLost(backplate);
    FD2D_CHECK(backplate.D3DDevice() == nullptr);
    std::vector<std::uint8_t> pixels;
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    FD2D_CHECK(backplate.ReadComposedPixels(D2D1::RectF(0.0f, 0.0f, 32.0f, 16.0f), pixels, width, height, stride) == E_NOINTERFACE);

    // The next render re-creates a WARP device stack under new generations, the
    // D2D child rebuilds its brush for it, and the frame composes as before.
    backplate.Render();
    FD2D_CHECK(backplate.D3DDevice() != nullptr);
    FD2D_CHECK(backplate.IsWindowless());
    FD2D_CHECK(backplate.GetGraphicsGeneration().device > before.device);
    FD2D_CHECK(backplate.GetGraphicsGeneration().target > before.target);
    FD2D_CHECK(d3d->LastClearSucceeded());
    FD2D_CHECK(d2d->BrushesCreated() == 2);
    checkFrame("after device loss");
}
