#pragma once

// FD2DLog.h - self-contained logging/timing macros for FD2D internals.
//
// FD2D previously included "../AppLog.h" (a FICture2-app-level, spdlog-backed
// logger living at the FICture2 repo root) directly from Backplate.cpp,
// Image.cpp. That made FD2D uncompilable unless it
// was checked out as a subdirectory of the exact FICture2 repository layout
// (needing "../AppLog.h" and "../external/spdlog/include" to exist one level
// above FD2D) - a problem for any other project (e.g. nifskope's
// NifLiteViewer) that wants FD2D as a standalone submodule.
//
// This header replaces that with a fully dependency-free equivalent:
//   - FD2D_TIMER_START / FD2D_ELAPSED_MS: plain std::chrono, used both for
//     real stall-detection thresholds (e.g. "if (frameMs > 100)") and for
//     the log lines below - unconditionally functional, no dependency.
//   - FD2D_LOG_INFO/WARN/ERROR/TRACE/DEBUG, FD2D_LOG_STEP: forward through a
//     tiny "{}"-placeholder formatter (no fmt/spdlog dependency) to an
//     optional sink. With no sink installed, every call is a no-op at
//     effectively zero cost - matching AppLog.h's own documented default
//     behavior ("If Init is never called, every FIC2_LOG_* macro is a silent
//     no-op").
//
// A host application that wants these routed into its own logger (e.g.
// FICture2's spdlog-based AppLog/"fic2" logger) can call FD2D::Log::SetSink()
// once at startup; see FICture2.cpp for an example.
#include <chrono>
#include <sstream>
#include <string>

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

        template <typename T>
        void FormatArg(std::ostringstream& out, const T& value)
        {
            out << value;
        }
        inline void FormatArg(std::ostringstream& out, bool value)
        {
            out << (value ? "true" : "false");
        }

        // Minimal spdlog/fmt-style "{}"-placeholder formatter: substitutes
        // each successive "{}" in fmt with the corresponding argument via
        // operator<<. Deliberately does not support fmt's extended format
        // specifiers (e.g. "{:.1f}") - callers that need those should
        // pre-format the value (see Backplate.cpp's FPS summary log for an
        // example) and pass the result as a plain string argument.
        template <typename... Args>
        std::string Format(const char* fmt, const Args&... args)
        {
            std::ostringstream out;
            const char* p = fmt;
            auto emitOne = [&](const auto& value)
            {
                while (*p != '\0' && !(p[0] == '{' && p[1] == '}'))
                    out << *p++;
                if (*p != '\0')
                    p += 2; // skip "{}"
                FormatArg(out, value);
            };
            (emitOne(args), ...);
            out << p;
            return out.str();
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
// Startup timing helpers (identical semantics to AppLog.h's originals).
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
