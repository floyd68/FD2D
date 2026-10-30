#include "TestHarness.h"

#include "WindowlessTestSupport.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

// The move/size modal loop: Windows runs its own message loop between
// WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE, so Application::RunMessageLoop does
// not service the async-redraw event or the animation tick. A hidden real
// window is driven the same way (the messages, then a pump that only
// dispatches): a redraw requested by a worker thread renders before
// WM_EXITSIZEMOVE, an animation keeps rendering, and with nothing pending the
// window renders nothing. After WM_EXITSIZEMOVE the loop's timer is gone.

namespace
{
    constexpr UINT_PTR kModalLoopTimerId = 0xFD24;   // Backplate::kModalLoopTimerId

    // Counts its renders; while `animate` is above zero, each render requests
    // another animation frame (and counts it down).
    class CountingWnd : public FD2D::Wnd
    {
    public:
        CountingWnd()
            : FD2D::Wnd(L"counting")
        {
        }

        void OnRender(ID2D1RenderTarget* target) override
        {
            ++renders;
            if (animate > 0)
            {
                --animate;
                if (BackplateRef() != nullptr)
                {
                    BackplateRef()->RequestAnimationFrame();
                }
            }
            Wnd::OnRender(target);
        }

        std::atomic<int> renders { 0 };
        int animate { 0 };
    };

    struct ModalScene
    {
        FD2DWindowlessTest::ComScope com;
        FD2DWindowlessTest::CoreScope core;
        FD2D::Backplate backplate { L"modal-loop-test" };
        std::shared_ptr<CountingWnd> counter { std::make_shared<CountingWnd>() };

        bool Create()
        {
            FD2D::WindowOptions options {};
            options.instance = GetModuleHandleW(nullptr);
            options.title = L"FD2D modal loop test";
            options.x = 0;
            options.y = 0;
            options.width = 320;
            options.height = 200;
            options.chrome = FD2D::ChromeStyle::Borderless;
            options.style = WS_POPUP;
            options.exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
            options.className = L"FD2DModalLoopTest";
            return SUCCEEDED(backplate.CreateWindowed(options)) && backplate.AddWnd(counter);
        }

        ~ModalScene()
        {
            if (backplate.Window() != nullptr)
            {
                DestroyWindow(backplate.Window());
            }
        }
    };

    struct PumpResult
    {
        int loopTimers { 0 };
    };

    // What the modal loop does: dispatch messages, nothing else, for `ms` (or
    // until `until` returns true).
    template <typename Until>
    PumpResult Pump(int ms, Until until)
    {
        PumpResult result {};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < deadline && !until())
        {
            MSG msg {};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_TIMER && msg.wParam == kModalLoopTimerId)
                {
                    ++result.loopTimers;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(1);
        }
        return result;
    }

    PumpResult Pump(int ms)
    {
        return Pump(ms, [] { return false; });
    }
}

FD2D_TEST(ModalLoopRedraw_AsyncRedrawAndAnimationRenderDuringAMove)
{
    ModalScene scene;
    FD2D_REQUIRE(scene.Create());
    FD2D::Backplate& backplate = scene.backplate;
    HWND window = backplate.Window();
    FD2D_REQUIRE(window != nullptr);
    backplate.Render();
    (void)Pump(100);   // settle: initial paints

    SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0);

    // Nothing pending: the loop's timer ticks, nothing renders.
    int before = scene.counter->renders.load();
    const PumpResult idle = Pump(300);
    std::printf("  idle move: %d loop timer ticks, %d renders in 300 ms\n", idle.loopTimers,
        scene.counter->renders.load() - before);
    FD2D_CHECK(idle.loopTimers >= 5);
    FD2D_CHECK(scene.counter->renders.load() == before);

    // A worker thread's async redraw renders inside the loop, within a few frames.
    const auto token = backplate.GetAsyncRedrawToken();
    FD2D_REQUIRE(token != nullptr);
    before = scene.counter->renders.load();
    const auto requested = std::chrono::steady_clock::now();
    std::thread([&] { token->RequestAsyncRedraw(); }).join();
    (void)Pump(1000, [&] { return scene.counter->renders.load() > before; });
    const long long delayMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - requested).count();
    std::printf("  async redraw during the move rendered after %lld ms\n", delayMs);
    FD2D_CHECK(scene.counter->renders.load() > before);
    FD2D_CHECK(delayMs < 200);

    // An animation keeps rendering through the loop's ticks.
    before = scene.counter->renders.load();
    scene.counter->animate = 6;
    backplate.RequestAnimationFrame();
    (void)Pump(500);
    const int animated = scene.counter->renders.load() - before;
    std::printf("  animation during the move: %d renders in 500 ms\n", animated);
    FD2D_CHECK(animated >= 5);

    // Idle again once the animation ended.
    (void)Pump(200);
    before = scene.counter->renders.load();
    (void)Pump(300);
    FD2D_CHECK(scene.counter->renders.load() == before);

    // After the loop: the timer is gone (RunMessageLoop services the event again).
    SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
    const PumpResult after = Pump(200);
    FD2D_CHECK(after.loopTimers == 0);
    FD2D_CHECK(!backplate.IsInSizeMove());
}

FD2D_TEST(ModalLoopRedraw_LiveResizeOnlyOnceTheSizeChanges)
{
    // A pure move is not a live resize (work deferred for resizing can go
    // on); a size change inside the loop is, until the loop ends.
    ModalScene scene;
    FD2D_REQUIRE(scene.Create());
    FD2D::Backplate& backplate = scene.backplate;
    HWND window = backplate.Window();
    FD2D_REQUIRE(window != nullptr);
    backplate.Render();

    FD2D_CHECK(!backplate.IsInLiveResize());
    SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0);
    FD2D_CHECK(backplate.IsInSizeMove());
    FD2D_CHECK(!backplate.IsInLiveResize());
    SetWindowPos(window, nullptr, 40, 30, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);   // moved
    (void)Pump(50);
    FD2D_CHECK(!backplate.IsInLiveResize());
    SetWindowPos(window, nullptr, 0, 0, 360, 220, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);   // resized
    (void)Pump(50);
    FD2D_CHECK(backplate.IsInLiveResize());
    SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
    FD2D_CHECK(!backplate.IsInLiveResize());
}
