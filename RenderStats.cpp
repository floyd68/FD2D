#include "RenderStats.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

namespace FD2D
{
    namespace
    {
        bool ReadEnabledFromEnvironment()
        {
            char* value = nullptr;
            std::size_t length = 0;
            if (_dupenv_s(&value, &length, "FD2D_RENDER_STATS") != 0 || value == nullptr)
            {
                return false;
            }
            const bool enabled = value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
            std::free(value);
            return enabled;
        }

        double Median(const std::vector<double>& samples)
        {
            return RenderStats::Percentile(samples, 50.0);
        }

        double Max(const std::vector<double>& samples)
        {
            return samples.empty() ? 0.0 : *std::max_element(samples.begin(), samples.end());
        }
    }

    bool RenderStats::Enabled()
    {
        static const bool enabled = ReadEnabledFromEnvironment();
        return enabled;
    }

    double RenderStats::Percentile(std::vector<double> samples, double percent)
    {
        if (samples.empty())
        {
            return 0.0;
        }
        std::sort(samples.begin(), samples.end());
        // Nearest rank: the smallest sample with at least `percent` of the
        // samples at or below it.
        const double rank = std::ceil(percent / 100.0 * static_cast<double>(samples.size()));
        const std::size_t index = static_cast<std::size_t>((std::max)(rank, 1.0)) - 1;
        return samples[(std::min)(index, samples.size() - 1)];
    }

    void RenderStats::OnMouseMove(std::uint32_t messageTime, std::uint32_t tickNow, double steadyNowMs)
    {
        (void)tickNow;
        ++m_moves;
        ++m_totalMoves;

        if (m_haveLastMove && steadyNowMs - m_lastMoveSteadyMs <= kMoveGapMs)
        {
            ++m_movePairs;
            m_movePairPresents += m_presentsSinceLastMove;
            m_movePairMax = (std::max)(m_movePairMax, m_presentsSinceLastMove);
            ++m_moveHistogram[(std::min)(m_presentsSinceLastMove, std::size_t { 3 })];
        }
        m_haveLastMove = true;
        m_lastMoveSteadyMs = steadyNowMs;
        m_presentsSinceLastMove = 0;

        // Latency is measured for the oldest input the screen does not show
        // yet. A pending move that no present followed within kMoveGapMs did
        // not cause one (a hover that renders nothing): restart from this move
        // so an unrelated later present is not reported as its latency.
        if (!m_inputPending || steadyNowMs - m_pendingDispatchSteadyMs > kMoveGapMs)
        {
            m_inputPending = true;
            m_pendingMessageTime = messageTime;
            m_pendingDispatchSteadyMs = steadyNowMs;
        }
    }

    void RenderStats::ResetWindow()
    {
        m_windowOpen = false;
        m_frames = 0;
        m_presents = 0;
        m_moves = 0;
        m_movePairs = 0;
        m_movePairPresents = 0;
        m_movePairMax = 0;
        std::fill(std::begin(m_moveHistogram), std::end(m_moveHistogram), std::size_t { 0 });
        m_frameCpuMs.clear();
        m_presentWaitMs.clear();
        m_inputLatencyMs.clear();
        m_dispatchLatencyMs.clear();
    }

    std::string RenderStats::OnFrame(
        double cpuMs,
        double presentWaitMs,
        bool presented,
        std::uint32_t tickNow,
        double steadyNowMs,
        unsigned long long windowNowMs)
    {
        // The window opens at its first frame; mouse moves that arrive before
        // it (since the previous summary) are counted in it.
        if (!m_windowOpen)
        {
            m_windowOpen = true;
            m_windowStartMs = windowNowMs;
        }

        ++m_frames;
        m_frameCpuMs.push_back(cpuMs);
        if (presented)
        {
            ++m_presents;
            ++m_totalPresents;
            ++m_presentsSinceLastMove;
            m_presentWaitMs.push_back(presentWaitMs);
            if (m_inputPending)
            {
                // Unsigned difference: correct across the 49.7-day tick wrap.
                m_inputLatencyMs.push_back(static_cast<double>(static_cast<std::uint32_t>(tickNow - m_pendingMessageTime)));
                m_dispatchLatencyMs.push_back(steadyNowMs - m_pendingDispatchSteadyMs);
                m_inputPending = false;
            }
        }

        const unsigned long long elapsed = windowNowMs - m_windowStartMs;
        if (elapsed < kWindowMs)
        {
            return {};
        }
        std::string line = Summarize(elapsed);
        ResetWindow();
        return line;
    }

    std::string RenderStats::Summarize(unsigned long long elapsedMs)
    {
        const double seconds = static_cast<double>((std::max)(elapsedMs, 1ULL)) / 1000.0;
        const double perMove = m_movePairs > 0
            ? static_cast<double>(m_movePairPresents) / static_cast<double>(m_movePairs)
            : 0.0;
        return std::format(
            "[RenderStats] window={}ms frames={} presents={} ({:.1f}/s) moves={} "
            "presentsPerMove(pairs={} avg={:.2f} max={} hist0={} hist1={} hist2={} hist3+={}) "
            "frameCpuMs(med={:.2f} p95={:.2f} max={:.2f}) presentWaitMs(med={:.2f}) "
            "inputToPresentMs(n={} med={:.0f} p95={:.0f} max={:.0f}) "
            "dispatchToPresentMs(med={:.2f} p95={:.2f} max={:.2f})",
            elapsedMs, m_frames, m_presents, static_cast<double>(m_presents) / seconds, m_moves,
            m_movePairs, perMove, m_movePairMax,
            m_moveHistogram[0], m_moveHistogram[1], m_moveHistogram[2], m_moveHistogram[3],
            Median(m_frameCpuMs), Percentile(m_frameCpuMs, 95.0), Max(m_frameCpuMs),
            Median(m_presentWaitMs),
            m_inputLatencyMs.size(), Median(m_inputLatencyMs), Percentile(m_inputLatencyMs, 95.0), Max(m_inputLatencyMs),
            Median(m_dispatchLatencyMs), Percentile(m_dispatchLatencyMs, 95.0), Max(m_dispatchLatencyMs));
    }
}
