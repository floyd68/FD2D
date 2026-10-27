#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace FD2D
{
    // Optional per-window render instrumentation (diagnostics only).
    //
    // Off by default. It is enabled for the whole process when the environment
    // variable FD2D_RENDER_STATS is set to anything other than "0" or empty
    // before the first window renders; Enabled() reads it once. When it is off,
    // the backplate makes no RenderStats call: the cost is one branch on a
    // cached flag per render and per WM_MOUSEMOVE.
    //
    // When on, every rendered frame and every dispatched WM_MOUSEMOVE of a
    // window is recorded, and once per second (at the first frame after the
    // one-second window closed) a "[RenderStats]" line is logged through
    // FD2D_LOG_INFO with:
    // - presents: frames that reached Present (swap chain Present, or EndDraw
    //   of the D2D-only HWND target), and presents per second;
    // - presents per mouse move: for each pair of consecutive dispatched
    //   WM_MOUSEMOVE messages of the window (at most kMoveGapMs apart), the
    //   presents between them, as an average, a maximum and a histogram
    //   (0, 1, 2, 3+);
    // - frame time: per-frame CPU time of Render() (excluding the time spent
    //   blocked in Present), median, p95 and max, plus the median Present wait;
    // - input-to-present latency: from the message time (GetMessageTime) of
    //   the oldest mouse move not yet presented to the return of the Present
    //   that first shows it (a move not followed by a present within
    //   kMoveGapMs rendered nothing and is dropped). Message times have
    //   tick-count resolution
    //   (~16 ms), so the line also reports dispatch-to-present (from the
    //   moment the move was dispatched, high resolution).
    // An idle window renders nothing and logs nothing: no line means no
    // presents in that period.
    //
    // The class itself is window-free (times are passed in) so it can be unit
    // tested.
    class RenderStats
    {
    public:
        // Moves further apart than this do not form a "consecutive" pair (the
        // gap between two separate drags is not counted).
        static constexpr double kMoveGapMs = 250.0;
        static constexpr unsigned long long kWindowMs = 1000;

        // Process-wide switch, read once from FD2D_RENDER_STATS.
        static bool Enabled();

        // A WM_MOUSEMOVE of this window is being dispatched. `messageTime` is
        // GetMessageTime(), `tickNow` GetTickCount() (same clock), `steadyNowMs`
        // a high-resolution monotonic time in milliseconds.
        void OnMouseMove(std::uint32_t messageTime, std::uint32_t tickNow, double steadyNowMs);

        // A frame was rendered. `cpuMs` is its render time without the
        // Present wait, `presentWaitMs` the time blocked in Present, and
        // `presented` whether it reached Present. `windowNowMs` is the clock
        // the one-second window runs on. Returns the summary line when this
        // frame closed the window, otherwise an empty string.
        std::string OnFrame(
            double cpuMs,
            double presentWaitMs,
            bool presented,
            std::uint32_t tickNow,
            double steadyNowMs,
            unsigned long long windowNowMs);

        // Totals since construction (tests and diagnostics).
        std::size_t TotalPresents() const { return m_totalPresents; }
        std::size_t TotalMouseMoves() const { return m_totalMoves; }

        // Percentile (0..100) by nearest rank of `samples` (copied); 0 when empty.
        static double Percentile(std::vector<double> samples, double percent);

    private:
        void ResetWindow();
        std::string Summarize(unsigned long long elapsedMs);

        // Current one-second window.
        unsigned long long m_windowStartMs { 0 };
        bool m_windowOpen { false };
        std::size_t m_frames { 0 };
        std::size_t m_presents { 0 };
        std::size_t m_moves { 0 };
        std::size_t m_movePairs { 0 };
        std::size_t m_movePairPresents { 0 };
        std::size_t m_movePairMax { 0 };
        std::size_t m_moveHistogram[4] {};
        std::vector<double> m_frameCpuMs {};
        std::vector<double> m_presentWaitMs {};
        std::vector<double> m_inputLatencyMs {};
        std::vector<double> m_dispatchLatencyMs {};

        // Across windows.
        bool m_haveLastMove { false };
        double m_lastMoveSteadyMs { 0.0 };
        std::size_t m_presentsSinceLastMove { 0 };
        bool m_inputPending { false };
        std::uint32_t m_pendingMessageTime { 0 };
        double m_pendingDispatchSteadyMs { 0.0 };
        std::size_t m_totalPresents { 0 };
        std::size_t m_totalMoves { 0 };
    };
}
