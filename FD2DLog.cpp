#include "FD2DLog.h"

#include <atomic>

namespace FD2D
{
namespace Log
{
    namespace
    {
        std::atomic<Sink> s_sink { nullptr };
    }

    void SetSink(Sink sink)
    {
        s_sink.store(sink, std::memory_order_relaxed);
    }

    namespace Detail
    {
        void Dispatch(Level level, std::string message)
        {
            if (Sink sink = s_sink.load(std::memory_order_relaxed))
                sink(level, message);
        }
    }
}
}
