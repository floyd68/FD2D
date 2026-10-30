#include "TestHarness.h"

#include "BackplateTestAccess.h"
#include "WindowlessTestSupport.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

// The swap-chain renderer of a real (hidden) window: an ordinary frame goes
// straight to the back buffer, without the full-frame offscreen copy, and
// ReadComposedPixels() still returns the current composed D3D + D2D frame -
// after a scene change, a resize, a target recreation and a device loss -
// composing it offscreen on demand when the last frame went to the back
// buffer. Size/move and the D2D-only renderer keep their errors.
//
// Scene (logical units, as in WindowlessRenderTests): clear color opaque
// black; a D3D child clears [0, 32) x [0, 48) with ClearRectD3D; a D2D child
// fills [16, 48) x [0, 48) with 60% blue. Columns: D3D | blue over D3D | blue
// over black | black.

using FD2DWindowlessTest::Bgra;
using FD2DWindowlessTest::ClearRectD3DWnd;
using FD2DWindowlessTest::FillRectD2DWnd;
using FD2DWindowlessTest::Frame;

namespace
{
    constexpr Bgra kRed { 0, 0, 255, 255 };
    constexpr Bgra kBlueOverRed { 153, 0, 102, 255 };
    constexpr Bgra kGreen { 0, 255, 0, 255 };
    constexpr Bgra kBlueOverGreen { 153, 102, 0, 255 };
    constexpr Bgra kBlueOverBlack { 153, 0, 0, 255 };
    constexpr Bgra kBlack { 0, 0, 0, 255 };

    const D2D1_COLOR_F kRedColor = D2D1::ColorF(1.0f, 0.0f, 0.0f, 1.0f);
    const D2D1_COLOR_F kGreenColor = D2D1::ColorF(0.0f, 1.0f, 0.0f, 1.0f);
    const D2D1_COLOR_F kTranslucentBlue = D2D1::ColorF(0.0f, 0.0f, 1.0f, 0.6f);

    struct WindowScene
    {
        FD2DWindowlessTest::ComScope com;
        FD2DWindowlessTest::CoreScope core;
        FD2D::Backplate backplate { L"swapchain-readback-test" };
        std::shared_ptr<ClearRectD3DWnd> d3d {
            std::make_shared<ClearRectD3DWnd>(L"d3d", D2D1::RectF(0.0f, 0.0f, 32.0f, 48.0f), kRedColor) };
        std::shared_ptr<FillRectD2DWnd> d2d {
            std::make_shared<FillRectD2DWnd>(L"d2d", D2D1::RectF(16.0f, 0.0f, 48.0f, 48.0f), kTranslucentBlue) };

        // A hidden window (never shown) with the given renderer.
        bool Create(UINT width, UINT height, const wchar_t* rendererId = nullptr)
        {
            FD2D::WindowOptions options {};
            options.instance = GetModuleHandleW(nullptr);
            options.title = L"FD2D swap-chain readback test";
            options.x = 0;
            options.y = 0;
            options.width = width;
            options.height = height;
            options.chrome = FD2D::ChromeStyle::Borderless;
            options.style = WS_POPUP;
            options.exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
            options.className = L"FD2DSwapChainReadbackTest";
            options.rendererId = rendererId;
            backplate.SetClearColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
            return SUCCEEDED(backplate.CreateWindowed(options)) && backplate.AddWnd(d3d) && backplate.AddWnd(d2d);
        }

        ~WindowScene()
        {
            if (backplate.Window() != nullptr)
            {
                DestroyWindow(backplate.Window());
            }
        }
    };

    // Reads [0, 64) x [0, 48) and checks the four columns at row 24, with the
    // D3D child's color `d3dColor` (and blue over it).
    bool ReadsColumns(FD2D::Backplate& backplate, const Bgra& d3dColor, const Bgra& blueOverD3d)
    {
        Frame frame;
        const HRESULT hr = frame.Read(backplate, D2D1::RectF(0.0f, 0.0f, 64.0f, 48.0f));
        if (FAILED(hr))
        {
            std::printf("    ReadComposedPixels failed: 0x%08X\n", static_cast<unsigned>(hr));
            return false;
        }
        const bool ok = frame.width == 64 && frame.height == 48 &&
            frame.Near(8, 24, d3dColor) && frame.Near(24, 24, blueOverD3d) &&
            frame.Near(40, 24, kBlueOverBlack) && frame.Near(56, 24, kBlack);
        if (!ok)
        {
            frame.Print("    read row", 24);
        }
        return ok;
    }

    // GPU time (ms, median of `runs`) of the copy the D3D path used to do every
    // frame: a D2D DrawImage (SOURCE_COPY) of a full-frame BGRA target into
    // another, on `device`. Negative when it cannot be measured.
    double MeasureFullFrameCopyMs(ID3D11Device* device, ID3D11DeviceContext* context, UINT width, UINT height, int runs)
    {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice;
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
        if (FD2D::Core::D2DFactory1() == nullptr || FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
            FAILED(FD2D::Core::D2DFactory1()->CreateDevice(dxgi.Get(), &d2dDevice)) ||
            FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)))
        {
            return -1.0;
        }
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> source, target;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &source)) || FAILED(device->CreateTexture2D(&desc, nullptr, &target)))
        {
            return -1.0;
        }
        const auto bitmapOf = [&](ID3D11Texture2D* texture)
        {
            Microsoft::WRL::ComPtr<IDXGISurface> surface;
            Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
            D2D1_BITMAP_PROPERTIES1 props {};
            props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
            props.dpiX = 96.0f;
            props.dpiY = 96.0f;
            props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            if (SUCCEEDED(texture->QueryInterface(IID_PPV_ARGS(&surface))))
            {
                (void)dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &bitmap);
            }
            return bitmap;
        };
        const auto sourceBitmap = bitmapOf(source.Get());
        const auto targetBitmap = bitmapOf(target.Get());
        if (!sourceBitmap || !targetBitmap)
        {
            return -1.0;
        }

        std::vector<double> ms;
        for (int i = 0; i < runs; ++i)
        {
            D3D11_QUERY_DESC disjointDesc { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            D3D11_QUERY_DESC stampDesc { D3D11_QUERY_TIMESTAMP, 0 };
            Microsoft::WRL::ComPtr<ID3D11Query> disjoint, begin, end;
            if (FAILED(device->CreateQuery(&disjointDesc, &disjoint)) || FAILED(device->CreateQuery(&stampDesc, &begin)) ||
                FAILED(device->CreateQuery(&stampDesc, &end)))
            {
                return -1.0;
            }
            context->Begin(disjoint.Get());
            context->End(begin.Get());
            dc->SetTarget(targetBitmap.Get());
            dc->BeginDraw();
            const D2D1_POINT_2F origin = D2D1::Point2F(0.0f, 0.0f);
            dc->DrawImage(sourceBitmap.Get(), &origin, nullptr, D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
            if (FAILED(dc->EndDraw()))
            {
                return -1.0;
            }
            dc->SetTarget(nullptr);
            context->End(end.Get());
            context->End(disjoint.Get());
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT freq {};
            UINT64 t0 = 0;
            UINT64 t1 = 0;
            while (context->GetData(disjoint.Get(), &freq, sizeof(freq), 0) == S_FALSE) { Sleep(0); }
            while (context->GetData(begin.Get(), &t0, sizeof(t0), 0) == S_FALSE) { Sleep(0); }
            while (context->GetData(end.Get(), &t1, sizeof(t1), 0) == S_FALSE) { Sleep(0); }
            if (!freq.Disjoint && freq.Frequency != 0)
            {
                ms.push_back(static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(freq.Frequency));
            }
        }
        if (ms.empty())
        {
            return -1.0;
        }
        std::sort(ms.begin(), ms.end());
        return ms[ms.size() / 2];
    }
}

FD2D_TEST(SwapChainReadback_OrdinaryFramesSkipTheOffscreenCopy)
{
    WindowScene scene;
    FD2D_REQUIRE(scene.Create(2560, 1440));
    FD2D::Backplate& backplate = scene.backplate;
    FD2D_REQUIRE(backplate.D3DDevice() != nullptr);

    backplate.Render();   // device, swap chain and first frame
    const std::size_t copiesBefore = FD2D::BackplateTestAccess::OffscreenCopies(backplate);
    constexpr int kFrames = 200;
    for (int i = 0; i < kFrames; ++i)
    {
        backplate.Render();
    }
    const std::size_t copies = FD2D::BackplateTestAccess::OffscreenCopies(backplate) - copiesBefore;

    // What each of those frames saves: the GPU time of the full-frame copy, on
    // this device at this size (frame times themselves are vsync-bound here).
    const D2D1_SIZE_U size = backplate.ClientSize();
    const double copyMs = MeasureFullFrameCopyMs(backplate.D3DDevice(), backplate.D3DContext(), size.width, size.height, 50);
    std::printf("  %d frames at %ux%u: offscreen copies %zu; one full-frame copy costs %.3f ms of GPU time\n",
        kFrames, size.width, size.height, copies, copyMs);
    FD2D_CHECK(copies == 0);
    FD2D_CHECK(scene.d3d->LastClearSucceeded());
}

FD2D_TEST(SwapChainReadback_ReadsTheCurrentComposedFrame)
{
    WindowScene scene;
    FD2D_REQUIRE(scene.Create(320, 200));
    FD2D::Backplate& backplate = scene.backplate;

    // Render() then ReadComposedPixels(), as FICture2's screenshot does.
    backplate.Render();
    FD2D_CHECK(ReadsColumns(backplate, kRed, kBlueOverRed));

    // A scene change, rendered straight to the back buffer: the next read
    // shows it, not the frame composed for the previous read.
    scene.d3d->SetColor(kGreenColor);
    backplate.Render();
    backplate.Render();
    FD2D_CHECK(ReadsColumns(backplate, kGreen, kBlueOverGreen));

    // Reading again with no frame in between returns the same frame.
    FD2D_CHECK(ReadsColumns(backplate, kGreen, kBlueOverGreen));

    // After a capture, ordinary frames skip the copy again.
    const std::size_t copies = FD2D::BackplateTestAccess::OffscreenCopies(backplate);
    for (int i = 0; i < 5; ++i)
    {
        backplate.Render();
    }
    FD2D_CHECK(FD2D::BackplateTestAccess::OffscreenCopies(backplate) == copies);
}

FD2D_TEST(SwapChainReadback_ReadsAfterResizeAndRecreation)
{
    WindowScene scene;
    FD2D_REQUIRE(scene.Create(320, 200));
    FD2D::Backplate& backplate = scene.backplate;
    backplate.Render();
    FD2D_CHECK(ReadsColumns(backplate, kRed, kBlueOverRed));

    // Resize: the next capture is composed at the new size.
    SetWindowPos(backplate.Window(), nullptr, 0, 0, 400, 240, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    backplate.Resize(400, 240);
    backplate.Render();
    scene.d3d->SetColor(kGreenColor);
    backplate.Render();
    FD2D_CHECK(ReadsColumns(backplate, kGreen, kBlueOverGreen));

    // A real target recreation (what a recoverable EndDraw failure does): the
    // swap-chain targets and the offscreen resources are discarded, the next
    // frame recreates the swap-chain targets, and the first capture composes
    // into a new offscreen texture. Ordinary frames then go direct again.
    FD2D_REQUIRE(FD2D::BackplateTestAccess::OffscreenTexture(backplate) != nullptr);   // from the last read
    FD2D::BackplateTestAccess::RecreateTargetsAfterEndDrawFailure(backplate);
    FD2D_CHECK(FD2D::BackplateTestAccess::SwapChainTargetBitmap(backplate) == nullptr);
    FD2D_CHECK(FD2D::BackplateTestAccess::OffscreenTexture(backplate) == nullptr);
    scene.d3d->SetColor(kRedColor);
    backplate.Render();
    FD2D_CHECK(FD2D::BackplateTestAccess::SwapChainTargetBitmap(backplate) != nullptr);
    FD2D_CHECK(FD2D::BackplateTestAccess::OffscreenTexture(backplate) == nullptr);   // a direct frame
    FD2D_CHECK(ReadsColumns(backplate, kRed, kBlueOverRed));
    FD2D_CHECK(FD2D::BackplateTestAccess::OffscreenTexture(backplate) != nullptr);
    const std::size_t copies = FD2D::BackplateTestAccess::OffscreenCopies(backplate);
    scene.d3d->SetColor(kGreenColor);
    backplate.Render();
    backplate.Render();
    FD2D_CHECK(FD2D::BackplateTestAccess::OffscreenCopies(backplate) == copies);
    FD2D_CHECK(ReadsColumns(backplate, kGreen, kBlueOverGreen));

    // A device loss: the first capture after it shows the current frame.
    FD2D::BackplateTestAccess::DeviceLost(backplate);
    scene.d3d->SetColor(kRedColor);
    backplate.Render();
    FD2D_REQUIRE(backplate.D3DDevice() != nullptr);
    FD2D_CHECK(ReadsColumns(backplate, kRed, kBlueOverRed));
}

FD2D_TEST(SwapChainReadback_CaptureSurvivesARenderRequestedByARenderCallback)
{
    // A render callback that calls Render() makes the outer Render run one
    // more pass; the capture keeps composing offscreen through it, so the
    // read succeeds with the current frame.
    WindowScene scene;
    FD2D_REQUIRE(scene.Create(320, 200));
    FD2D::Backplate& backplate = scene.backplate;
    backplate.Render();

    int passes = 0;
    backplate.SetBetweenPassesHook([&]
    {
        ++passes;
        if (passes == 1)
        {
            backplate.Render();   // a nested request: one more pass of this Render
        }
    });
    scene.d3d->SetColor(kGreenColor);
    const std::size_t copies = FD2D::BackplateTestAccess::OffscreenCopies(backplate);
    FD2D_CHECK(ReadsColumns(backplate, kGreen, kBlueOverGreen));
    backplate.SetBetweenPassesHook({});
    std::printf("  capture with a nested render request: %d passes, %zu offscreen copies\n", passes,
        FD2D::BackplateTestAccess::OffscreenCopies(backplate) - copies);
    FD2D_CHECK(passes == 2);
    FD2D_CHECK(FD2D::BackplateTestAccess::OffscreenCopies(backplate) - copies == 2);

    // The next ordinary frames go direct again.
    const std::size_t after = FD2D::BackplateTestAccess::OffscreenCopies(backplate);
    backplate.Render();
    FD2D_CHECK(FD2D::BackplateTestAccess::OffscreenCopies(backplate) == after);
}

FD2D_TEST(SwapChainReadback_SizeMoveAndD2DOnlyKeepTheirErrors)
{
    {
        WindowScene scene;
        FD2D_REQUIRE(scene.Create(320, 200));
        FD2D::Backplate& backplate = scene.backplate;
        backplate.Render();
        FD2D::BackplateTestAccess::SetInSizeMove(backplate, true);
        backplate.Render();
        Frame frame;
        FD2D_CHECK(frame.Read(backplate, D2D1::RectF(0.0f, 0.0f, 64.0f, 48.0f)) == HRESULT_FROM_WIN32(ERROR_NOT_READY));
        FD2D::BackplateTestAccess::SetInSizeMove(backplate, false);
        backplate.Render();
        FD2D_CHECK(ReadsColumns(backplate, kRed, kBlueOverRed));
    }
    {
        WindowScene scene;
        FD2D_REQUIRE(scene.Create(320, 200, L"d2d_hwndrt"));
        FD2D::Backplate& backplate = scene.backplate;
        backplate.Render();
        Frame frame;
        FD2D_CHECK(frame.Read(backplate, D2D1::RectF(0.0f, 0.0f, 64.0f, 48.0f)) == E_NOINTERFACE);
    }
}
