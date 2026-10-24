#include "TestHarness.h"

#include "AsyncRedrawSignal.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// AsyncRedrawSignal handshake tests. No window is involved: the signal is the
// pending-flag/event pair that Backplate::RequestAsyncRedraw (worker threads)
// and Backplate::ProcessAsyncRedraw (UI thread) use.

using FD2D::AsyncRedrawSignal;

FD2D_TEST(AsyncRedrawSignal_CoalescesUntilDrained)
{
    AsyncRedrawSignal signal;
    FD2D_REQUIRE(signal.IsValid());
    FD2D_CHECK(!signal.IsPending());
    FD2D_CHECK(!signal.IsSignaled());

    // First request signals; later ones coalesce into the same wakeup.
    FD2D_CHECK(signal.Request());
    FD2D_CHECK(signal.IsPending());
    FD2D_CHECK(signal.IsSignaled());
    FD2D_CHECK(!signal.Request());
    FD2D_CHECK(!signal.Request());
    FD2D_CHECK(signal.IsSignaled());

    // Drain consumes the wakeup and reports that one was pending.
    FD2D_CHECK(signal.Drain());
    FD2D_CHECK(!signal.IsPending());
    FD2D_CHECK(!signal.IsSignaled());
    FD2D_CHECK(!signal.Drain());

    // The next request produces a new wakeup.
    FD2D_CHECK(signal.Request());
    FD2D_CHECK(signal.IsSignaled());
}

FD2D_TEST(AsyncRedrawSignal_RequestBetweenResetAndClearIsNotStranded)
{
    // The exact interleaving that stranded the old implementation: a worker
    // requests a redraw while the consumer is half-way through draining.
    AsyncRedrawSignal signal;
    FD2D_REQUIRE(signal.Request());

    bool requestSignaled = true;
    const bool wasPending = signal.DrainForTesting([&]()
    {
        // Event already reset, flag still set: this request must coalesce.
        requestSignaled = signal.Request();
    });
    FD2D_CHECK(wasPending);
    FD2D_CHECK(!requestSignaled);

    // After the drain the state is consistent (the processing that follows a
    // drain covers the coalesced request), and crucially the signal is not
    // stuck: the next request produces a wakeup.
    FD2D_CHECK(!(signal.IsPending() && !signal.IsSignaled()));
    FD2D_CHECK(signal.Request());
    FD2D_CHECK(signal.IsSignaled());
    FD2D_CHECK(signal.Drain());
}

FD2D_TEST(AsyncRedrawSignal_RequestAfterDrainSignalsAgain)
{
    AsyncRedrawSignal signal;
    FD2D_REQUIRE(signal.Request());
    FD2D_CHECK(signal.Drain());

    // A request after the drain completed is a fresh wakeup.
    FD2D_CHECK(signal.Request());
    FD2D_CHECK(signal.IsPending());
    FD2D_CHECK(signal.IsSignaled());
}

FD2D_TEST(AsyncRedrawSignal_StressNeverStrandsPendingWithoutEvent)
{
    AsyncRedrawSignal signal;
    FD2D_REQUIRE(signal.IsValid());

    constexpr int kProducers = 4;
    constexpr int kRequestsPerProducer = 5000;
    std::atomic<bool> producersDone { false };
    std::atomic<int> drains { 0 };

    std::thread consumer([&]()
    {
        while (!producersDone.load())
        {
            if (WaitForSingleObject(signal.Event(), 20) == WAIT_OBJECT_0)
            {
                signal.Drain();
                ++drains;
            }
        }
    });

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p)
    {
        producers.emplace_back([&]()
        {
            for (int i = 0; i < kRequestsPerProducer; ++i)
            {
                (void)signal.Request();
            }
        });
    }
    for (auto& producer : producers)
    {
        producer.join();
    }
    producersDone.store(true);
    consumer.join();

    // Quiesced. The invariant is "never pending without an event": a pending
    // request must still have its event set so the consumer would wake. The
    // reverse (event set, nothing pending) is an allowed spurious wakeup: a
    // producer that flipped the flag just before a drain sets the event just
    // after it, and the consumer then drains an empty signal.
    const bool pending = signal.IsPending();
    const bool signaled = signal.IsSignaled();
    FD2D_CHECK(!(pending && !signaled));
    if (pending || signaled)
    {
        FD2D_CHECK(signal.Drain() == pending);
    }
    FD2D_CHECK(!signal.IsPending());
    FD2D_CHECK(!signal.IsSignaled());

    FD2D_CHECK(drains.load() >= 1);
    FD2D_CHECK(drains.load() <= kProducers * kRequestsPerProducer);

    // The signal is still usable after the storm.
    FD2D_CHECK(signal.Request());
    FD2D_CHECK(signal.IsSignaled());
}
