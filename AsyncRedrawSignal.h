#pragma once

#include <windows.h>

#include <atomic>
#include <functional>

namespace FD2D
{
    // Coalesced cross-thread "redraw requested" signal: a pending flag plus a
    // manual-reset event the UI thread waits on. Any number of Request() calls
    // between two Drain() calls produce exactly one wakeup.
    //
    // Invariant: while `pending` is true the event is set, or is about to be set
    // by the requester that flipped the flag. Drain() therefore resets the event
    // BEFORE clearing the flag. A Request() that lands between those two steps
    // sees pending == true and does not signal; the processing that follows
    // Drain() covers it. A Request() after the clear signals a fresh wakeup.
    // Clearing the flag first (the previous order) let a Request() set the event
    // and then have it reset, leaving pending == true with no event forever, so
    // no later request could ever wake the UI thread again.
    class AsyncRedrawSignal
    {
    public:
        AsyncRedrawSignal();
        ~AsyncRedrawSignal();

        AsyncRedrawSignal(const AsyncRedrawSignal&) = delete;
        AsyncRedrawSignal& operator=(const AsyncRedrawSignal&) = delete;

        // Waitable manual-reset event; nullptr if creation failed.
        HANDLE Event() const { return m_event; }
        bool IsValid() const { return m_event != nullptr; }

        // Any thread. Returns true when this call set the event (first request
        // since the last Drain), false when a wakeup was already pending.
        bool Request();

        // Consumer thread, immediately before processing. Resets the event, then
        // clears the pending flag. Returns true if a request was pending.
        bool Drain();

        bool IsPending() const { return m_pending.load(); }

        // Non-blocking probe of the event state.
        bool IsSignaled() const;

        // Tests only: Drain() with `betweenSteps` run after the event is reset and
        // before the flag is cleared, to pin down that interleaving.
        bool DrainForTesting(const std::function<void()>& betweenSteps);

    private:
        bool DrainImpl(const std::function<void()>* betweenSteps);

        HANDLE m_event { nullptr };
        std::atomic<bool> m_pending { false };
    };
}
