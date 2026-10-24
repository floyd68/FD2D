#include "TestHarness.h"

#include "AsyncRedrawDispatch.h"
#include "AsyncRedrawSignal.h"

#include <windows.h>

#include <array>
#include <vector>

// Dispatch fairness tests for the message loop's event servicing. They model
// two backplates by two AsyncRedrawSignals; "servicing" drains the signal, as
// Backplate::ProcessAsyncRedraw does before repainting.

using FD2D::AsyncRedrawSignal;
using FD2D::ServiceSignaledEvents;

namespace
{
    struct TwoOwners
    {
        std::array<AsyncRedrawSignal, 2> signals {};
        std::vector<HANDLE> events { signals[0].Event(), signals[1].Event() };
        std::array<int, 2> served { 0, 0 };

        // What the message loop's wait would report: the lowest signaled index.
        std::size_t LowestSignaled() const
        {
            const DWORD res = WaitForMultipleObjects(2, events.data(), FALSE, 0);
            FD2D_REQUIRE(res >= WAIT_OBJECT_0 && res < WAIT_OBJECT_0 + 2);
            return static_cast<std::size_t>(res - WAIT_OBJECT_0);
        }
    };
}

FD2D_TEST(AsyncRedrawDispatch_SecondOwnerIsServicedWhenFirstKeepsResignaling)
{
    // The starvation scenario: owner 0 receives a new completion during every
    // repaint, so its event is set again before each wait. Owner 1 signaled
    // once and must still be serviced on every pass.
    TwoOwners owners;
    FD2D_REQUIRE(owners.signals[0].Request());
    FD2D_REQUIRE(owners.signals[1].Request());

    constexpr int kPasses = 200;
    for (int pass = 0; pass < kPasses; ++pass)
    {
        const std::size_t first = owners.LowestSignaled();
        FD2D_CHECK(first == 0);

        ServiceSignaledEvents(owners.events, first, [&](std::size_t index)
        {
            owners.signals[index].Drain();
            ++owners.served[index];
            if (index == 0)
            {
                // Completion arrives while owner 0 repaints.
                (void)owners.signals[0].Request();
            }
        });

        // Owner 1 is serviced in the first pass (it was signaled) and then left
        // alone until it signals again; owner 0 is serviced exactly once per pass.
        FD2D_CHECK(owners.served[0] == pass + 1);
        FD2D_CHECK(owners.served[1] == 1);
        if (pass + 1 < kPasses)
        {
            (void)owners.signals[1].Request();
            owners.served[1] = 0;
        }
    }
}

FD2D_TEST(AsyncRedrawDispatch_UnsignaledOwnersAreNotServiced)
{
    TwoOwners owners;
    FD2D_REQUIRE(owners.signals[1].Request());

    ServiceSignaledEvents(owners.events, owners.LowestSignaled(), [&](std::size_t index)
    {
        owners.signals[index].Drain();
        ++owners.served[index];
    });

    FD2D_CHECK(owners.served[0] == 0);
    FD2D_CHECK(owners.served[1] == 1);
    FD2D_CHECK(!owners.signals[1].IsSignaled());
}

FD2D_TEST(AsyncRedrawDispatch_EachSignaledOwnerIsServicedOncePerPass)
{
    TwoOwners owners;
    FD2D_REQUIRE(owners.signals[0].Request());
    FD2D_REQUIRE(owners.signals[1].Request());

    // Start at index 1 (as if the wait had reported it) to cover wrap-around.
    ServiceSignaledEvents(owners.events, 1, [&](std::size_t index)
    {
        owners.signals[index].Drain();
        ++owners.served[index];
    });

    FD2D_CHECK(owners.served[0] == 1);
    FD2D_CHECK(owners.served[1] == 1);

    // Nothing signaled: a pass services nobody.
    ServiceSignaledEvents(owners.events, 0, [&](std::size_t index)
    {
        ++owners.served[index];
    });
    FD2D_CHECK(owners.served[0] == 1);
    FD2D_CHECK(owners.served[1] == 1);
}
