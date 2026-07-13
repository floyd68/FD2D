#pragma once

// FD2DLog.h - self-contained logging/timing macros for FD2D internals.
//
// Dependency-free: optional sink via SetSink(); with no sink installed every
// FD2D_LOG_* call is a silent no-op. Format strings use C++20 std::format.
//
// A host application that wants these routed into its own logger can call
// FD2D::Log::SetSink() once at startup.

#include <chrono>
#include <format>
#include <string>
#include <utility>

namespace FD2D
{
namespace Log
{
    enum class Level { Trace, Debug, Info, Warn, Error };

    // Receives every FD2D_LOG_* message after formatting. Install with
    // SetSink(); pass nullptr to go back to the default no-op behavior.
    using Sink = void(*)(Level level, const std::string& message);

    void SetSink(Sink sink);

    namespace Detail
    {
        void Dispatch(Level level, std::string message);

        template <typename... Args>
        std::string Format(std::format_string<Args...> fmt, Args&&... args)
        {
            return std::format(fmt, std::forward<Args>(args)...);
        }
    }
}
}

#define FD2D_LOG_INFO(...)   ::FD2D::Log::Detail::Dispatch(::FD2D::Log::Level::Info,  ::FD2D::Log::Detail::Format(__VA_ARGS__))
#define FD2D_LOG_WARN(...)   ::FD2D::Log::Detail::Dispatch(::FD2D::Log::Level::Warn,  ::FD2D::Log::Detail::Format(__VA_ARGS__))
#define FD2D_LOG_ERROR(...)  ::FD2D::Log::Detail::Dispatch(::FD2D::Log::Level::Error, ::FD2D::Log::Detail::Format(__VA_ARGS__))

#ifdef _DEBUG
#define FD2D_LOG_TRACE(...)  ::FD2D::Log::Detail::Dispatch(::FD2D::Log::Level::Trace, ::FD2D::Log::Detail::Format(__VA_ARGS__))
#define FD2D_LOG_DEBUG(...)  ::FD2D::Log::Detail::Dispatch(::FD2D::Log::Level::Debug, ::FD2D::Log::Detail::Format(__VA_ARGS__))
#else
#define FD2D_LOG_TRACE(...)  do {} while (0)
#define FD2D_LOG_DEBUG(...)  do {} while (0)
#endif

// ---------------------------------------------------------------------------
// Startup timing helpers.
// ---------------------------------------------------------------------------
#define FD2D_TIMER_START(var) \
    auto var = std::chrono::steady_clock::now()

#define FD2D_ELAPSED_MS(var) \
    (std::chrono::duration_cast<std::chrono::milliseconds>( \
        std::chrono::steady_clock::now() - (var)).count())

#define FD2D_LOG_STEP(var, label) \
    do { \
        auto _now = std::chrono::steady_clock::now(); \
        auto _ms  = std::chrono::duration_cast<std::chrono::milliseconds>(_now - (var)).count(); \
        FD2D_LOG_INFO("[STARTUP] {}ms  {}", _ms, label); \
        (var) = _now; \
    } while (0)
