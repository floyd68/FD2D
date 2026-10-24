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
    constexpr int kCheckpointEvery = 1000;
    constexpr auto kCheckpointTimeout = std::chrono::seconds(10);
    std::atomic<bool> producersDone { false };
    std::atomic<int> drains { 0 };
    std::atomic<int> checkpointTimeouts { 0 };

    // The consumer keeps draining until the producers are done AND the event is
    // no longer set, so the last wakeup is always consumed here, not by the
    // assertions below.
    std::thread consumer([&]()
    {
        for (;;)
        {
            if (WaitForSingleObject(signal.Event(), 20) == WAIT_OBJECT_0)
            {
                signal.Drain();
                ++drains;
            }
            if (producersDone.load() && !signal.IsSignaled())
            {
                return;
            }
        }
    });

    // Producers prove overlap with the consumer: at every checkpoint a producer
    // requests a redraw and then waits (bounded) until the consumer has drained
    // at least once more. The request guarantees a set event, so the consumer
    // must make progress; a timeout is a test failure, never a hang.
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p)
    {
        producers.emplace_back([&]()
        {
            for (int i = 1; i <= kRequestsPerProducer; ++i)
            {
                if (i % kCheckpointEvery == 0)
                {
                    const int before = drains.load();
                    (void)signal.Request();
                    const auto deadline = std::chrono::steady_clock::now() + kCheckpointTimeout;
                    while (drains.load() == before)
                    {
                        if (std::chrono::steady_clock::now() >= deadline)
                        {
                            ++checkpointTimeouts;
                            break;
                        }
                        std::this_thread::yield();
                    }
                }
                else
                {
                    (void)signal.Request();
                }
            }
        });
    }
    for (auto& producer : producers)
    {
        producer.join();
    }
    producersDone.store(true);
    consumer.join();

    // Every checkpoint saw the consumer drain while production was still running.
    // Checkpoints of different producers may be satisfied by the same coalesced
    // drain, but a producer's successive checkpoints need distinct drains, so at
    // least one drain per checkpoint round happened during production.
    FD2D_CHECK(checkpointTimeouts.load() == 0);
    FD2D_CHECK(drains.load() >= kRequestsPerProducer / kCheckpointEvery);
    FD2D_CHECK(drains.load() <= kProducers * kRequestsPerProducer);

    // Quiesced: the consumer exits only with the event clear, and the invariant
    // "never pending without an event" then implies nothing is pending either.
    FD2D_CHECK(!signal.IsSignaled());
    FD2D_CHECK(!signal.IsPending());

    // The signal is still usable after the storm.
    FD2D_CHECK(signal.Request());
    FD2D_CHECK(signal.IsSignaled());
}
