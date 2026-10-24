#pragma once

#include <windows.h>

#include <cstddef>
#include <vector>

namespace FD2D
{
    // Services every event that is currently signaled, exactly once per call,
    // starting at `firstIndex` (the index a wait reported) and wrapping around.
    //
    // Each index is probed once, so an owner that re-signals while it is being
    // serviced is not revisited in this pass; the remaining owners still get
    // their turn. Servicing only the lowest signaled index per wait (the previous
    // behavior) let a busy low-index owner starve a higher one indefinitely when a
    // new completion arrived during every synchronous repaint.
    //
    // `service(i)` is expected to drain/reset events[i]; unsignaled events are
    // left alone.
    template <class Service>
    void ServiceSignaledEvents(const std::vector<HANDLE>& events, std::size_t firstIndex, Service&& service)
    {
        const std::size_t count = events.size();
        if (count == 0)
        {
            return;
        }

        for (std::size_t step = 0; step < count; ++step)
        {
            const std::size_t index = (firstIndex + step) % count;
            if (events[index] != nullptr && WaitForSingleObject(events[index], 0) == WAIT_OBJECT_0)
            {
                service(index);
            }
        }
    }
}
