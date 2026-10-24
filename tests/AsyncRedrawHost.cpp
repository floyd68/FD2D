// Manual two-backplate host for the async redraw path (not a CTest entry: it
// needs a desktop). It opens two windows, each with a worker thread that
// requests async redraws through the backplate's AsyncRedrawToken, prints
// per-window frame counts once per second, closes window B while both workers
// are still requesting, and reports whether window B kept rendering while
// window A's worker was flooding requests.
//
// Usage: FD2DAsyncRedrawHost [seconds-before-closing-B=6] [b-idle]
//   b-idle: window B's worker never requests a redraw; B must then render only
//   its initial paints while A floods (unsignaled owners are not serviced).

#include "Application.h"
#include "Backplate.h"
#include "Wnd.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace
{
    class CounterWnd : public FD2D::Wnd
    {
    public:
        explicit CounterWnd(const std::wstring& name)
            : Wnd(name)
        {
        }

        FD2D::Size Measure(FD2D::Size available) override
        {
            m_desired = available;
            return m_desired;
        }

        void OnRender(ID2D1RenderTarget* target) override
        {
            const unsigned frame = ++frames;
            if (target != nullptr)
            {
                // Visible activity: a bar that advances with every frame.
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
                const float hue = static_cast<float>(frame % 60) / 60.0f;
                if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(hue, 0.6f, 1.0f - hue), &brush)))
                {
                    const D2D1_RECT_F r = LayoutRect();
                    const float w = (r.right - r.left) * static_cast<float>(frame % 100) / 100.0f;
                    target->FillRectangle(D2D1::RectF(r.left, r.top, r.left + w, r.top + 40.0f), brush.Get());
                }
            }
            Wnd::OnRender(target);
        }

        std::atomic<unsigned> frames { 0 };
    };

    struct Host
    {
        std::shared_ptr<FD2D::Backplate> backplate;
        std::shared_ptr<CounterWnd> counter;
        std::shared_ptr<FD2D::AsyncRedrawToken> token;
        std::atomic<unsigned long long> requests { 0 };
    };

    bool Open(Host& host, const std::wstring& name, int x, const wchar_t* title)
    {
        FD2D::WindowOptions opts {};
        opts.instance = GetModuleHandleW(nullptr);
        opts.title = title;
        opts.x = x;
        opts.y = 120;
        opts.width = 480;
        opts.height = 320;

        host.backplate = FD2D::Application::Instance().CreateWindowedBackplate(name, opts);
        if (!host.backplate)
        {
            std::fprintf(stderr, "CreateWindowedBackplate(%ls) failed\n", name.c_str());
            return false;
        }
        host.counter = std::make_shared<CounterWnd>(name + L"_counter");
        host.backplate->AddWnd(host.counter);
        host.token = host.backplate->GetAsyncRedrawToken();
        host.backplate->Show(SW_SHOW);
        return true;
    }
}

int main(int argc, char** argv)
{
    const int closeAfterSeconds = (argc > 1) ? std::atoi(argv[1]) : 6;
    const bool bIdle = (argc > 2) && std::string(argv[2]) == "b-idle";

    FD2D::InitContext context {};
    context.instance = GetModuleHandleW(nullptr);
    if (FAILED(FD2D::Application::Instance().Initialize(context)))
    {
        std::fprintf(stderr, "Application::Initialize failed\n");
        return 2;
    }

    Host a;
    Host b;
    if (!Open(a, L"a", 100, L"A: worker floods redraw requests") ||
        !Open(b, L"b", 620, L"B: worker requests every 50 ms"))
    {
        return 2;
    }

    std::atomic<bool> stop { false };
    // Worker A floods: a new "completion" arrives during practically every repaint.
    std::thread workerA([&]()
    {
        while (!stop.load())
        {
            a.token->RequestAsyncRedraw();
            ++a.requests;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    // Worker B is a normal decode cadence (or idle, to check that an unsignaled
    // backplate is not redrawn by A's traffic).
    std::thread workerB([&]()
    {
        while (!stop.load())
        {
            if (!bIdle)
            {
                b.token->RequestAsyncRedraw();
                ++b.requests;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    // Reporter: per-second counts; closes B while both workers are still running.
    std::atomic<unsigned> bFramesWhileAFloods { 0 };
    std::atomic<int> secondsWithoutBFrame { 0 };
    std::thread reporter([&]()
    {
        unsigned lastA = 0;
        unsigned lastB = 0;
        for (int second = 1; !stop.load(); ++second)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const unsigned fa = a.counter->frames.load();
            const unsigned fb = b.counter->frames.load();
            std::printf("t=%ds  A frames=%u (+%u, requests=%llu)  B frames=%u (+%u, requests=%llu)\n",
                second, fa, fa - lastA, a.requests.load(), fb, fb - lastB, b.requests.load());
            if (second <= closeAfterSeconds)
            {
                bFramesWhileAFloods += (fb - lastB);
                if (fb == lastB)
                {
                    ++secondsWithoutBFrame;
                }
            }
            lastA = fa;
            lastB = fb;
            if (second == closeAfterSeconds)
            {
                std::printf("closing window B while both workers keep requesting redraws\n");
                HWND hwndB = b.backplate->Window();
                if (hwndB)
                {
                    PostMessageW(hwndB, WM_CLOSE, 0, 0);
                }
            }
        }
    });

    // Note: FD2D posts WM_QUIT when any backplate window is destroyed, so closing
    // B ends the loop for both windows. Workers keep requesting until joined,
    // which exercises requests against a destroyed window and a dropped token.
    const int loopResult = FD2D::Application::Instance().RunMessageLoop();

    stop.store(true);
    workerA.join();
    workerB.join();
    reporter.join();

    std::printf("message loop returned %d\n", loopResult);
    std::printf("A total frames=%u requests=%llu; B total frames=%u requests=%llu\n",
        a.counter->frames.load(), a.requests.load(), b.counter->frames.load(), b.requests.load());
    std::printf("B frames while A flooded: %u; seconds in which B rendered nothing: %d\n",
        bFramesWhileAFloods.load(), secondsWithoutBFrame.load());

    bool failed = false;
    if (bIdle)
    {
        // B never requested: after its initial paints (show/size), A's flood must
        // not produce B frames. Allow the first second for those initial paints.
        const unsigned bFrames = b.counter->frames.load();
        std::printf("b-idle: B total frames=%u (initial paints only expected)\n", bFrames);
        failed = (bFramesWhileAFloods.load() > bFrames) || (bFrames > 5);
        std::printf("RESULT: %s\n", failed ? "FAIL (idle B was redrawn by A's traffic)" : "OK (idle B not serviced; clean exit)");
    }
    else
    {
        failed = secondsWithoutBFrame.load() > 0;
        std::printf("RESULT: %s\n", failed ? "FAIL (B starved)" : "OK (B kept rendering; clean exit)");
    }
    const bool bStarved = failed;

    a.token.reset();
    b.token.reset();
    a.counter.reset();
    b.counter.reset();
    a.backplate.reset();
    b.backplate.reset();
    FD2D::Application::Instance().Shutdown();
    return bStarved ? 1 : 0;
}
