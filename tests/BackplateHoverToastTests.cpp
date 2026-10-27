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
#include <memory>
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

// Deadlines: the tooltip dwell (500 ms) and the toast expiry (1800 ms) wait
// on Backplate's per-window timer, not on animation frames. A windowless
// backplate gets a hidden window that forwards its messages to the backplate;
// the pump below runs ProcessAnimationTick and then renders a pending
// request after every wake, as Application::RunMessageLoop does, so an
// animation request would render.

namespace
{
    // Counts frames and offers a tooltip everywhere.
    class TooltipWnd : public FD2DWindowlessTest::FillRectD2DWnd
    {
    public:
        TooltipWnd()
            : FillRectD2DWnd(L"tip", D2D1::RectF(0.0f, 0.0f, 8.0f, 8.0f), D2D1::ColorF(0.0f, 0.5f, 0.0f, 1.0f))
        {
        }

        std::wstring TooltipText() const override { return L"Tooltip text"; }
    };

    LRESULT CALLBACK ForwardingWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* backplate = reinterpret_cast<FD2D::Backplate*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        LRESULT result = 0;
        if (backplate != nullptr && BackplateTestAccess::HandleMessage(*backplate, hwnd, message, wParam, lParam, result))
        {
            return result;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    // Windowless backplate + hidden message window + a frame-counting child.
    struct TimerScene
    {
        FD2DWindowlessTest::ComScope com;
        FD2DWindowlessTest::CoreScope core;
        FD2D::Backplate backplate { L"hover-toast-timer-test" };
        std::shared_ptr<TooltipWnd> tip { std::make_shared<TooltipWnd>() };
        HWND window { nullptr };

        bool Create()
        {
            WNDCLASSEXW wc { sizeof(WNDCLASSEXW) };
            wc.lpfnWndProc = ForwardingWndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"FD2DHoverToastTestWindow";
            (void)RegisterClassExW(&wc); // may already exist from an earlier test
            window = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, kCanvasW, kCanvasH,
                nullptr, nullptr, wc.hInstance, nullptr);
            if (window == nullptr ||
                FAILED(backplate.InitializeWindowless(kCanvasW, kCanvasH, 1.0f)) ||
                !backplate.AddWnd(tip))
            {
                return false;
            }
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&backplate));
            BackplateTestAccess::SetWindow(backplate, window);
            backplate.Render(); // lays out the child over the whole client area
            return tip->Frames() == 1;
        }

        ~TimerScene()
        {
            if (window != nullptr)
            {
                // Detach first: the backplate never sees this window's WM_DESTROY.
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                BackplateTestAccess::SetWindow(backplate, nullptr);
                DestroyWindow(window);
            }
        }

        // Pumps messages (then the animation tick and the pending render, like
        // RunMessageLoop) until done() or timeoutMs. Counts the wakes that saw
        // an active animation.
        template <typename Done>
        bool PumpUntil(Done done, unsigned long long timeoutMs, int& animatedWakes)
        {
            const unsigned long long start = GetTickCount64();
            while (!done())
            {
                if (GetTickCount64() - start >= timeoutMs)
                {
                    return false;
                }
                (void)MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
                MSG msg {};
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    if (msg.message != WM_QUIT)
                    {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                }
                const unsigned long long now = GetTickCount64();
                if (backplate.HasActiveAnimation(now))
                {
                    ++animatedWakes;
                }
                backplate.ProcessAnimationTick(now);
                (void)backplate.RenderIfPending();
            }
            return true;
        }

        void Settle(unsigned long long ms, int& animatedWakes)
        {
            (void)PumpUntil([]() { return false; }, ms, animatedWakes);
        }

        Canvas::Counts ReadCounts()
        {
            Canvas::Counts counts;
            FD2DWindowlessTest::Frame frame;
            if (FAILED(frame.Read(backplate, D2D1::RectF(0.0f, 0.0f, kCanvasW, kCanvasH))))
            {
                return counts;
            }
            for (UINT y = 0; y < frame.height; ++y)
            {
                for (UINT x = 0; x < frame.width; ++x)
                {
                    const auto p = frame.At(x, y);
                    if (p.r >= 22 && p.r <= 38 && p.g >= 22 && p.g <= 38 && p.b >= 27 && p.b <= 43)
                    {
                        ++counts.tooltip;
                    }
                    if (p.r >= 32 && p.r <= 48 && p.g >= 76 && p.g <= 92 && p.b >= 135 && p.b <= 151)
                    {
                        ++counts.toast;
                    }
                    if (p.r >= 200 && p.g >= 200 && p.b >= 200)
                    {
                        ++counts.text;
                    }
                }
            }
            return counts;
        }
    };
}

FD2D_TEST(BackplateHoverToast_TooltipDwellWaitsOnTimer)
{
    TimerScene scene;
    FD2D_REQUIRE(scene.Create());
    FD2D::Backplate& backplate = scene.backplate;

    // Hover over the control: the dwell is a timer deadline, not an animation.
    BackplateTestAccess::HoverAt(backplate, POINT { 20, 20 });
    const unsigned long long since = BackplateTestAccess::HoverSinceMs(backplate);
    FD2D_CHECK(!BackplateTestAccess::TooltipShown(backplate));
    FD2D_CHECK(!backplate.HasActiveAnimation(GetTickCount64()));
    FD2D_CHECK(BackplateTestAccess::HoverToastTimerDueMs(backplate) == since + 500);

    // Nothing renders while waiting; the timer at the deadline shows the
    // tooltip and renders exactly one frame.
    int animatedWakes = 0;
    const bool shown = scene.PumpUntil([&]() { return BackplateTestAccess::TooltipShown(backplate); }, 3000, animatedWakes);
    const unsigned long long waitedMs = GetTickCount64() - since;
    std::printf("  tooltip shown %d after %llu ms, frames %d (1 before), animated wakes %d\n",
        shown ? 1 : 0, waitedMs, scene.tip->Frames(), animatedWakes);
    FD2D_REQUIRE(shown);
    FD2D_CHECK(waitedMs >= 450); // timer granularity
    FD2D_CHECK(scene.tip->Frames() == 2);
    FD2D_CHECK(animatedWakes == 0);
    const Canvas::Counts counts = scene.ReadCounts();
    std::printf("  frame: tooltip fill %d px, text %d px\n", counts.tooltip, counts.text);
    FD2D_CHECK(counts.tooltip > 400 && counts.text > 10);

    // Then idle: no timer, no frames.
    FD2D_CHECK(BackplateTestAccess::HoverToastTimerDueMs(backplate) == 0);
    scene.Settle(300, animatedWakes);
    FD2D_CHECK(scene.tip->Frames() == 2);
    FD2D_CHECK(animatedWakes == 0);

    // Leaving the control (no tooltip there) drops a pending dwell.
    BackplateTestAccess::HoverAt(backplate, POINT { 500, 500 });
    BackplateTestAccess::HoverAt(backplate, POINT { 30, 30 });
    FD2D_CHECK(BackplateTestAccess::HoverToastTimerDueMs(backplate) != 0);
    BackplateTestAccess::HoverAt(backplate, POINT { 500, 500 });
    FD2D_CHECK(BackplateTestAccess::HoverToastTimerDueMs(backplate) == 0);
}

FD2D_TEST(BackplateHoverToast_ToastExpiryWaitsOnTimer)
{
    TimerScene scene;
    FD2D_REQUIRE(scene.Create());
    FD2D::Backplate& backplate = scene.backplate;

    backplate.ShowToast(L"Toast text");
    const unsigned long long expire = BackplateTestAccess::ToastExpireMs(backplate);
    FD2D_CHECK(!backplate.HasActiveAnimation(GetTickCount64()));
    FD2D_CHECK(BackplateTestAccess::HoverToastTimerDueMs(backplate) == expire);
    // ShowToast requests a render; paint that frame here.
    backplate.Render();
    FD2D_CHECK(scene.tip->Frames() == 2);
    Canvas::Counts counts = scene.ReadCounts();
    std::printf("  toast frame: toast fill %d px, text %d px\n", counts.toast, counts.text);
    FD2D_CHECK(counts.toast > 400 && counts.text > 10);

    // Nothing renders until the expiry; then one frame erases the toast.
    int animatedWakes = 0;
    const bool expired = scene.PumpUntil([&]() { return !BackplateTestAccess::ToastShown(backplate); }, 5000, animatedWakes);
    const long long lateMs = static_cast<long long>(GetTickCount64()) - static_cast<long long>(expire);
    std::printf("  toast expired %d (%lld ms after its deadline), frames %d (2 before), animated wakes %d\n",
        expired ? 1 : 0, lateMs, scene.tip->Frames(), animatedWakes);
    FD2D_REQUIRE(expired);
    FD2D_CHECK(lateMs >= -50); // timer granularity
    FD2D_CHECK(scene.tip->Frames() == 3);
    FD2D_CHECK(animatedWakes == 0);
    counts = scene.ReadCounts();
    FD2D_CHECK(counts.toast == 0);

    FD2D_CHECK(BackplateTestAccess::HoverToastTimerDueMs(backplate) == 0);
    scene.Settle(300, animatedWakes);
    FD2D_CHECK(scene.tip->Frames() == 3);
}

FD2D_TEST(BackplateHoverToast_TooltipAndToastShareTheTimer)
{
    TimerScene scene;
    FD2D_REQUIRE(scene.Create());
    FD2D::Backplate& backplate = scene.backplate;

    // Toast first, then a hover: the timer is armed for the earlier deadline
    // (the dwell), then for the toast expiry.
    backplate.ShowToast(L"Toast text");
    backplate.Render();
    BackplateTestAccess::HoverAt(backplate, POINT { 20, 20 });
    const unsigned long long dwellDue = BackplateTestAccess::HoverSinceMs(backplate) + 500;
    const unsigned long long expire = BackplateTestAccess::ToastExpireMs(backplate);
    FD2D_CHECK(BackplateTestAccess::HoverToastTimerDueMs(backplate) == dwellDue);

    int animatedWakes = 0;
    FD2D_REQUIRE(scene.PumpUntil([&]() { return BackplateTestAccess::TooltipShown(backplate); }, 3000, animatedWakes));
    FD2D_CHECK(BackplateTestAccess::ToastShown(backplate));
    FD2D_CHECK(BackplateTestAccess::HoverToastTimerDueMs(backplate) == expire);
    FD2D_CHECK(scene.tip->Frames() == 3);

    FD2D_REQUIRE(scene.PumpUntil([&]() { return !BackplateTestAccess::ToastShown(backplate); }, 5000, animatedWakes));
    FD2D_CHECK(BackplateTestAccess::TooltipShown(backplate));
    FD2D_CHECK(BackplateTestAccess::HoverToastTimerDueMs(backplate) == 0);
    std::printf("  frames %d (4 expected), animated wakes %d\n", scene.tip->Frames(), animatedWakes);
    FD2D_CHECK(scene.tip->Frames() == 4);
    FD2D_CHECK(animatedWakes == 0);
    const Canvas::Counts counts = scene.ReadCounts();
    FD2D_CHECK(counts.tooltip > 400 && counts.toast == 0);
}

namespace
{
    // A second tooltip target over the right half of the client area (added
    // after TooltipWnd, so it is hit first there).
    class RightHalfTooltipWnd : public FD2DWindowlessTest::FillRectD2DWnd
    {
    public:
        RightHalfTooltipWnd()
            : FillRectD2DWnd(L"tip2", D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f), D2D1::ColorF(0.0f, 0.5f, 0.0f, 1.0f))
        {
        }

        void Arrange(FD2D::Rect finalRect) override
        {
            FD2D::Wnd::Arrange({ finalRect.x + finalRect.w * 0.5f, finalRect.y, finalRect.w * 0.5f, finalRect.h });
        }

        std::wstring TooltipText() const override { return L"Second tooltip"; }
    };
}

FD2D_TEST(BackplateHoverToast_MovingToAnotherTargetErasesTheShownTooltip)
{
    TimerScene scene;
    FD2D_REQUIRE(scene.Create());
    FD2D::Backplate& backplate = scene.backplate;
    FD2D_REQUIRE(backplate.AddWnd(std::make_shared<RightHalfTooltipWnd>()));
    backplate.Render();

    // The first target's tooltip is showing.
    BackplateTestAccess::HoverAt(backplate, POINT { 20, 20 });
    int animatedWakes = 0;
    FD2D_REQUIRE(scene.PumpUntil([&]() { return BackplateTestAccess::TooltipShown(backplate); }, 3000, animatedWakes));
    Canvas::Counts counts = scene.ReadCounts();
    FD2D_REQUIRE(counts.tooltip > 400);
    const int framesShown = scene.tip->Frames();

    // Straight onto the second target (another non-empty tooltip): its dwell
    // starts, and the first tooltip is erased right away, not at the end of
    // the new dwell.
    BackplateTestAccess::HoverAt(backplate, POINT { 120, 20 });
    FD2D_CHECK(!BackplateTestAccess::TooltipShown(backplate));
    FD2D_CHECK(BackplateTestAccess::HoverToastTimerDueMs(backplate) != 0);
    scene.Settle(100, animatedWakes);
    counts = scene.ReadCounts();
    std::printf("  100 ms after moving to the second target: tooltip fill %d px, frames %d (%d when shown)\n",
        counts.tooltip, scene.tip->Frames(), framesShown);
    FD2D_CHECK(counts.tooltip == 0);
    FD2D_CHECK(scene.tip->Frames() == framesShown + 1);

    // Its own tooltip follows at its deadline.
    FD2D_REQUIRE(scene.PumpUntil([&]() { return BackplateTestAccess::TooltipShown(backplate); }, 3000, animatedWakes));
    counts = scene.ReadCounts();
    FD2D_CHECK(counts.tooltip > 400);
    FD2D_CHECK(scene.tip->Frames() == framesShown + 2);
    FD2D_CHECK(animatedWakes == 0);
}
