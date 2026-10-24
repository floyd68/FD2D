#include "AsyncRedrawSignal.h"

namespace FD2D
{
    AsyncRedrawSignal::AsyncRedrawSignal()
        : m_event(CreateEventW(nullptr, TRUE /*manual reset*/, FALSE, nullptr))
    {
    }

    AsyncRedrawSignal::~AsyncRedrawSignal()
    {
        if (m_event)
        {
            CloseHandle(m_event);
            m_event = nullptr;
        }
    }

    bool AsyncRedrawSignal::Request()
    {
        if (!m_event)
        {
            return false;
        }

        // Coalesce: only the request that flips the flag signals the event.
        const bool wasPending = m_pending.exchange(true);
        if (wasPending)
        {
            return false;
        }
        SetEvent(m_event);
        return true;
    }

    bool AsyncRedrawSignal::Drain()
    {
        return DrainImpl(nullptr);
    }

    bool AsyncRedrawSignal::DrainForTesting(const std::function<void()>& betweenSteps)
    {
        return DrainImpl(&betweenSteps);
    }

    bool AsyncRedrawSignal::DrainImpl(const std::function<void()>* betweenSteps)
    {
        if (!m_event)
        {
            return m_pending.exchange(false);
        }

        // Order matters: reset the event first, then clear the flag. See the
        // class comment for why the reverse order loses wakeups permanently.
        ResetEvent(m_event);
        if (betweenSteps && *betweenSteps)
        {
            (*betweenSteps)();
        }
        return m_pending.exchange(false);
    }

    bool AsyncRedrawSignal::IsSignaled() const
    {
        return m_event && WaitForSingleObject(m_event, 0) == WAIT_OBJECT_0;
    }
}
