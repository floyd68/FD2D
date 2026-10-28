#include "TestHarness.h"

#include "Application.h"
#include "MessagePump.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

// Application::RunMessageLoop policy (FD2D#4).
//
// MessagePump_*: the window-free policy (MessagePump.h) driven by a simulated
// host whose queue never empties. Every bounded drain is followed by servicing
// the async-redraw events, the animation tick and one render, so none of them
// starves however long messages keep arriving.
//
// MessageLoopQuit_*: the real loop with no backplate registered (so no
// async-redraw event to wait on) on a worker thread with a message-only
// window. It must block for messages, dispatch posted messages and return the
// WM_QUIT code. A loop that spins without pumping cannot be stopped, so every
// wait is bounded and a timeout ends the test process with a failure code.

using FD2D::DrainMessagesBounded;
using FD2D::MessagePumpBudget;
using FD2D::PumpMessagesOnce;

namespace
{
    // A simulated loop host. The queue is `source`; dispatching an input
    // message requests a render, dispatching WM_PAINT renders and (like a
    // self-sustaining paint loop) immediately invalidates again.
    struct FakeHost
    {
        std::function<bool(MSG&)> source;
        unsigned long long msPerDispatch { 0 };

        unsigned long long clock { 0 };
        bool renderPending { false };
        bool asyncSignaled { true };   // a worker re-signals all the time
        std::vector<DWORD> timeouts;
        std::vector<std::size_t> dispatchedPerIteration;
        std::vector<int> rendersPerIteration;
        std::size_t dispatchedThisIteration { 0 };
        int rendersThisIteration { 0 };
        int services { 0 };
        int ticks { 0 };
        int renders { 0 };
        std::size_t totalDispatched { 0 };

        bool HasPendingRender() const { return renderPending; }
        DWORD IdleTimeoutMs() const { return 1000; }

        bool Wait(DWORD timeoutMs)
        {
            timeouts.push_back(timeoutMs);
            dispatchedThisIteration = 0;
            rendersThisIteration = 0;
            return true;
        }

        void ServiceAsyncEvents()
        {
            if (asyncSignaled)
            {
                ++services;
                renderPending = true;
            }
        }

        bool Peek(MSG& msg) { return source(msg); }

        void Dispatch(const MSG& msg)
        {
            ++dispatchedThisIteration;
            ++totalDispatched;
            clock += msPerDispatch;
            if (msg.message == WM_PAINT)
            {
                Render();
                renderPending = true;   // the render invalidated again
            }
            else
            {
                renderPending = true;
            }
        }

        unsigned long long NowMs() const { return clock; }

        void Tick(unsigned long long) { ++ticks; }

        // As Application's host: at most one render per iteration (a paint
        // in the drain already rendered).
        void FlushRenders()
        {
            if (renderPending && rendersThisIteration == 0)
            {
                Render();
            }
            dispatchedPerIteration.push_back(dispatchedThisIteration);
            rendersPerIteration.push_back(rendersThisIteration);
        }

        void Render()
        {
            renderPending = false;
            ++renders;
            ++rendersThisIteration;
        }
    };

    // A queue that never empties (capped so that a drain which ignores its
    // budget still returns and the test can report it).
    std::function<bool(MSG&)> EndlessQueue(UINT message, std::size_t& produced)
    {
        return [message, &produced](MSG& msg)
        {
            if (produced >= 1000000)
            {
                return false;
            }
            ++produced;
            msg = MSG {};
            msg.message = message;
            return true;
        };
    }
}

FD2D_TEST(MessagePump_NeverEmptyInputQueueStarvesNothing)
{
    FakeHost host;
    std::size_t produced = 0;
    host.source = EndlessQueue(WM_MOUSEMOVE, produced);
    const MessagePumpBudget budget { 64, 1000000 };

    constexpr int kIterations = 200;
    for (int i = 0; i < kIterations; ++i)
    {
        FD2D_REQUIRE(!PumpMessagesOnce(host, budget).has_value());
    }

    std::size_t maxDrain = 0;
    int maxRenders = 0;
    for (int i = 0; i < kIterations; ++i)
    {
        maxDrain = (std::max)(maxDrain, host.dispatchedPerIteration[i]);
        maxRenders = (std::max)(maxRenders, host.rendersPerIteration[i]);
    }
    std::printf("  %d iterations: max %zu messages per drain, %d async services, %d ticks, %d renders (max %d per iteration)\n",
        kIterations, maxDrain, host.services, host.ticks, host.renders, maxRenders);
    FD2D_CHECK(maxDrain <= budget.maxMessages);
    FD2D_CHECK(host.services == kIterations);
    FD2D_CHECK(host.ticks == kIterations);
    FD2D_CHECK(host.renders == kIterations);
    FD2D_CHECK(maxRenders == 1);
}

FD2D_TEST(MessagePump_TimeSliceBoundsTheDrain)
{
    FakeHost host;
    std::size_t produced = 0;
    host.source = EndlessQueue(WM_MOUSEMOVE, produced);
    host.msPerDispatch = 3;   // slow handlers
    const MessagePumpBudget budget { 100000, 10 };

    for (int i = 0; i < 50; ++i)
    {
        FD2D_REQUIRE(!PumpMessagesOnce(host, budget).has_value());
    }
    std::size_t maxDrain = 0;
    for (const std::size_t n : host.dispatchedPerIteration)
    {
        maxDrain = (std::max)(maxDrain, n);
    }
    std::printf("  3 ms handlers, 10 ms slice: max %zu messages per drain, %d services, %d ticks, %d renders\n",
        maxDrain, host.services, host.ticks, host.renders);
    FD2D_CHECK(maxDrain == 4);   // 3, 6, 9, 12 ms: the slice ends after the 4th
    FD2D_CHECK(host.services == 50);
    FD2D_CHECK(host.ticks == 50);
    FD2D_CHECK(host.renders == 50);
}

FD2D_TEST(MessagePump_SelfSustainingPaintLoopStarvesNothing)
{
    // A render that invalidates again: WM_PAINT is always available. Each
    // drain ends after the paint, so the events and the tick run between
    // paints, and the loop renders once per iteration.
    FakeHost host;
    std::size_t produced = 0;
    host.source = EndlessQueue(WM_PAINT, produced);
    const MessagePumpBudget budget {};

    for (int i = 0; i < 100; ++i)
    {
        FD2D_REQUIRE(!PumpMessagesOnce(host, budget).has_value());
    }
    int maxRenders = 0;
    std::size_t maxDrain = 0;
    for (std::size_t i = 0; i < host.rendersPerIteration.size(); ++i)
    {
        maxRenders = (std::max)(maxRenders, host.rendersPerIteration[i]);
        maxDrain = (std::max)(maxDrain, host.dispatchedPerIteration[i]);
    }
    std::printf("  100 iterations of a paint loop: max %zu paints per drain, %d services, %d ticks, %d renders\n",
        maxDrain, host.services, host.ticks, host.renders);
    FD2D_CHECK(maxDrain == 1);
    FD2D_CHECK(host.services == 100);
    FD2D_CHECK(host.ticks == 100);
    // The paint renders and invalidates again; that request is rendered by
    // the next iteration (one render per iteration).
    FD2D_CHECK(maxRenders == 1);
    FD2D_CHECK(host.renders == 100);
    FD2D_CHECK(host.timeouts.back() == 0);
}

FD2D_TEST(MessagePump_PendingRenderDoesNotBlockAndIdleWaitsWithTimeout)
{
    FakeHost host;
    host.asyncSignaled = false;
    std::vector<MSG> queue;
    host.source = [&](MSG& msg)
    {
        if (queue.empty())
        {
            return false;
        }
        msg = queue.front();
        queue.erase(queue.begin());
        return true;
    };

    // Idle: nothing pending, the wait blocks (heartbeat timeout), nothing renders.
    FD2D_CHECK(!PumpMessagesOnce(host, MessagePumpBudget {}).has_value());
    FD2D_CHECK(host.timeouts.back() == 1000);
    FD2D_CHECK(host.renders == 0);

    // A pending render (made outside the drain) is rendered without blocking.
    host.renderPending = true;
    FD2D_CHECK(!PumpMessagesOnce(host, MessagePumpBudget {}).has_value());
    FD2D_CHECK(host.timeouts.back() == 0);
    FD2D_CHECK(host.renders == 1);

    // WM_QUIT ends the loop with its code, before the tick and the flush.
    MSG input {};
    input.message = WM_MOUSEMOVE;
    MSG quit {};
    quit.message = WM_QUIT;
    quit.wParam = 7;
    queue = { input, quit };
    const int ticks = host.ticks;
    const auto code = PumpMessagesOnce(host, MessagePumpBudget {});
    FD2D_REQUIRE(code.has_value());
    FD2D_CHECK(*code == 7);
    FD2D_CHECK(host.ticks == ticks);
}

FD2D_TEST(MessagePump_DrainStopsAtEmptyQueueQuitAndPaint)
{
    std::vector<UINT> messages { WM_MOUSEMOVE, WM_KEYDOWN, WM_PAINT, WM_MOUSEMOVE };
    std::size_t next = 0;
    std::vector<UINT> dispatched;
    unsigned long long clock = 0;
    const auto peek = [&](MSG& msg)
    {
        if (next >= messages.size())
        {
            return false;
        }
        msg = MSG {};
        msg.message = messages[next++];
        return true;
    };
    const auto dispatch = [&](const MSG& msg) { dispatched.push_back(msg.message); };
    const auto now = [&]() { return clock; };

    auto result = DrainMessagesBounded(MessagePumpBudget {}, peek, dispatch, now);
    FD2D_CHECK(result.paintDispatched);
    FD2D_CHECK(result.dispatched == 3);
    result = DrainMessagesBounded(MessagePumpBudget {}, peek, dispatch, now);
    FD2D_CHECK(result.dispatched == 1);
    FD2D_CHECK(!result.budgetExhausted && !result.quit && !result.paintDispatched);
    FD2D_CHECK(dispatched.size() == 4);

    messages = { WM_MOUSEMOVE, WM_QUIT, WM_MOUSEMOVE };
    next = 0;
    result = DrainMessagesBounded(MessagePumpBudget {}, peek, dispatch, now);
    FD2D_CHECK(result.quit);
    FD2D_CHECK(result.dispatched == 1);
}

namespace
{
    constexpr UINT kPrivateMessage = WM_APP + 0x51;
    constexpr DWORD kWaitMs = 5000;
    constexpr int kQuitCode = 42;

    HANDLE g_procRan = nullptr;

    LRESULT CALLBACK MessageOnlyProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == kPrivateMessage)
        {
            SetEvent(g_procRan);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    // A spinning loop cannot be stopped: report and end the process.
    [[noreturn]] void FailAndExit(const char* what)
    {
        std::printf("  TIMEOUT: %s within %lu ms (the message loop does not pump); ending the test process\n",
            what, static_cast<unsigned long>(kWaitMs));
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 3);
        for (;;)
        {
        }
    }
}

FD2D_TEST(MessageLoopQuit_NoBackplateLoopDispatchesAndReturnsTheQuitCode)
{
    g_procRan = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    FD2D_REQUIRE(g_procRan != nullptr && ready != nullptr && done != nullptr);

    std::atomic<HWND> window { nullptr };
    std::atomic<DWORD> threadId { 0 };
    std::atomic<int> loopResult { INT_MIN };

    std::thread worker([&]()
    {
        WNDCLASSEXW wc { sizeof(WNDCLASSEXW) };
        wc.lpfnWndProc = MessageOnlyProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FD2DMessageLoopQuitTest";
        (void)RegisterClassExW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        // Force this thread's message queue before anyone posts to it.
        MSG msg {};
        (void)PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        window = hwnd;
        threadId = GetCurrentThreadId();
        SetEvent(ready);
        if (hwnd != nullptr)
        {
            loopResult = FD2D::Application::Instance().RunMessageLoop();
            DestroyWindow(hwnd);
        }
        SetEvent(done);
    });

    if (WaitForSingleObject(ready, kWaitMs) != WAIT_OBJECT_0)
    {
        FailAndExit("the worker did not start");
    }
    FD2D_REQUIRE(window.load() != nullptr);

    // A posted message reaches the window procedure.
    FD2D_REQUIRE(PostMessageW(window.load(), kPrivateMessage, 0, 0));
    if (WaitForSingleObject(g_procRan, kWaitMs) != WAIT_OBJECT_0)
    {
        FailAndExit("the posted message was not dispatched");
    }

    // WM_QUIT ends the loop with its code.
    FD2D_REQUIRE(PostThreadMessageW(threadId.load(), WM_QUIT, static_cast<WPARAM>(kQuitCode), 0));
    if (WaitForSingleObject(done, kWaitMs) != WAIT_OBJECT_0)
    {
        FailAndExit("RunMessageLoop did not return after WM_QUIT");
    }
    worker.join();
    std::printf("  posted message dispatched; RunMessageLoop returned %d\n", loopResult.load());
    FD2D_CHECK(loopResult.load() == kQuitCode);

    CloseHandle(done);
    CloseHandle(ready);
    CloseHandle(g_procRan);
    g_procRan = nullptr;
}
