#pragma once

#include <windows.h>

#include <cstddef>
#include <optional>

namespace FD2D
{
    // Window-free policy of Application::RunMessageLoop, factored out so a test
    // can drive it with a simulated queue (FD2D#4).
    //
    // One iteration of the loop:
    //   1. Wait (MsgWaitForMultipleObjectsEx) for a message or an async-redraw
    //      event. The wait does not block when a render is pending, and it
    //      also works with no events at all (only messages wake it), so the
    //      loop never spins and still sees WM_QUIT.
    //   2. Service every signaled async-redraw event once (each requests a
    //      coalesced render of its backplate).
    //   3. Drain messages, bounded by MessagePumpBudget: at most maxMessages,
    //      or until maxMilliseconds have passed, or until a WM_PAINT has been
    //      dispatched (paint is the lowest-priority message: once it is
    //      retrieved the queue holds no other input). WM_QUIT ends the loop.
    //   4. Run the animation tick (it requests renders, it does not render).
    //   5. Render every backplate with a pending render request, once, unless
    //      it already rendered in this iteration (a WM_PAINT in the drain):
    //      at most one render per backplate per iteration. A request left
    //      pending makes the next wait return at once.
    // A queue that never empties (a render that invalidates again, input that
    // keeps arriving) therefore cannot starve the async events, the tick or
    // rendering: each is serviced once per bounded drain.
    struct MessagePumpBudget
    {
        std::size_t maxMessages { 64 };
        unsigned long long maxMilliseconds { 10 };
    };

    struct MessageDrainResult
    {
        std::size_t dispatched { 0 };
        bool quit { false };
        int exitCode { 0 };
        // Stopped by the budget; messages may be left in the queue.
        bool budgetExhausted { false };
        // Stopped after dispatching a WM_PAINT.
        bool paintDispatched { false };
    };

    // `peek(MSG&)` removes the next message (false when the queue is empty),
    // `dispatch(const MSG&)` translates and dispatches it, `nowMs()` is a
    // monotonic millisecond clock.
    template <class Peek, class Dispatch, class Now>
    MessageDrainResult DrainMessagesBounded(const MessagePumpBudget& budget, Peek&& peek, Dispatch&& dispatch, Now&& nowMs)
    {
        MessageDrainResult result {};
        const unsigned long long start = nowMs();
        MSG msg {};
        while (result.dispatched < budget.maxMessages)
        {
            if (!peek(msg))
            {
                return result;
            }
            if (msg.message == WM_QUIT)
            {
                result.quit = true;
                result.exitCode = static_cast<int>(msg.wParam);
                return result;
            }
            dispatch(msg);
            ++result.dispatched;
            if (msg.message == WM_PAINT)
            {
                result.paintDispatched = true;
                return result;
            }
            if (nowMs() - start >= budget.maxMilliseconds)
            {
                result.budgetExhausted = true;
                return result;
            }
        }
        result.budgetExhausted = true;
        return result;
    }

    // One loop iteration (steps 1-5 above). `Host` provides:
    //   bool HasPendingRender()            a backplate has a render request
    //   DWORD IdleTimeoutMs()              wait timeout when none is pending
    //   bool Wait(DWORD timeoutMs)         step 1; false when the wait failed
    //   void ServiceAsyncEvents()          step 2
    //   bool Peek(MSG&), void Dispatch(const MSG&), unsigned long long NowMs()
    //   void Tick(unsigned long long nowMs)  step 4
    //   void FlushRenders()                step 5 (skips a backplate that
    //                                      rendered since Wait)
    // Returns the WM_QUIT exit code (or -1 when the wait failed) to end the
    // loop, otherwise nullopt.
    template <class Host>
    std::optional<int> PumpMessagesOnce(Host& host, const MessagePumpBudget& budget)
    {
        const DWORD timeoutMs = host.HasPendingRender() ? 0 : host.IdleTimeoutMs();
        if (!host.Wait(timeoutMs))
        {
            return -1;
        }

        host.ServiceAsyncEvents();

        const MessageDrainResult drain = DrainMessagesBounded(
            budget,
            [&](MSG& msg) { return host.Peek(msg); },
            [&](const MSG& msg) { host.Dispatch(msg); },
            [&]() { return host.NowMs(); });
        if (drain.quit)
        {
            return drain.exitCode;
        }

        host.Tick(host.NowMs());
        host.FlushRenders();
        return std::nullopt;
    }
}
