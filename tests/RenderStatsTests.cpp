#include "TestHarness.h"

#include "RenderStats.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// The render instrumentation's aggregation, without a window: times are
// passed in, so presents per mouse move, latency and the one-second summary
// are checked exactly.

using FD2D::RenderStats;

namespace
{
    bool Contains(const std::string& text, const char* part)
    {
        return text.find(part) != std::string::npos;
    }
}

FD2D_TEST(RenderStats_PercentileUsesNearestRank)
{
    FD2D_CHECK(RenderStats::Percentile({}, 50.0) == 0.0);
    FD2D_CHECK(RenderStats::Percentile({ 7.0 }, 95.0) == 7.0);
    std::vector<double> samples;
    for (int i = 100; i >= 1; --i)
    {
        samples.push_back(static_cast<double>(i));
    }
    FD2D_CHECK(RenderStats::Percentile(samples, 50.0) == 50.0);
    FD2D_CHECK(RenderStats::Percentile(samples, 95.0) == 95.0);
    FD2D_CHECK(RenderStats::Percentile(samples, 100.0) == 100.0);
}

FD2D_TEST(RenderStats_CountsPresentsPerMouseMoveAndLatency)
{
    RenderStats stats;
    // Ten moves 16 ms apart. After each of the first five moves one frame is
    // presented; after each of the next five, two frames.
    std::uint32_t tick = 1000;
    double steady = 5000.0;
    unsigned long long clock = 10000;
    std::string line;
    for (int move = 0; move < 10; ++move)
    {
        stats.OnMouseMove(tick - 4, tick, steady);   // message time 4 ms before dispatch
        const int presents = move < 5 ? 1 : 2;
        for (int p = 0; p < presents; ++p)
        {
            tick += 2;
            steady += 2.0;
            line = stats.OnFrame(1.5, 0.5, true, tick, steady, clock);
            FD2D_CHECK(line.empty());
        }
        tick += 12;
        steady += 12.0;
        clock += 16;
    }
    FD2D_CHECK(stats.TotalPresents() == 15);
    FD2D_CHECK(stats.TotalMouseMoves() == 10);

    // A frame after the window closed reports it. Nine consecutive pairs:
    // five with one present, four with two (the last move's presents are
    // counted when the next move arrives, which does not happen here).
    clock = 10000 + 1000;
    line = stats.OnFrame(1.0, 0.0, false, tick, steady, clock);
    std::printf("  %s\n", line.c_str());
    FD2D_CHECK(Contains(line, "[RenderStats] window=1000ms frames=16 presents=15 (15.0/s) moves=10 "));
    FD2D_CHECK(Contains(line, "presentsPerMove(pairs=9 avg=1.44 max=2 hist0=0 hist1=5 hist2=4 hist3+=0)"));
    // Every move is presented by the first frame after it: 4 ms message delay
    // + 2 ms to the present, and 2 ms from dispatch.
    FD2D_CHECK(Contains(line, "inputToPresentMs(n=10 med=6 p95=6 max=6)"));
    FD2D_CHECK(Contains(line, "dispatchToPresentMs(med=2.00 p95=2.00 max=2.00)"));
    FD2D_CHECK(Contains(line, "frameCpuMs(med=1.50 p95=1.50 max=1.50)"));

    // The next window starts empty at its first frame.
    line = stats.OnFrame(1.0, 0.0, true, tick, steady, clock + 5000);
    FD2D_CHECK(line.empty());
    line = stats.OnFrame(1.0, 0.0, false, tick, steady, clock + 6000);
    FD2D_CHECK(Contains(line, "frames=2 presents=1 "));
    FD2D_CHECK(Contains(line, "moves=0 presentsPerMove(pairs=0 avg=0.00"));
}

FD2D_TEST(RenderStats_LatencyIsMeasuredFromTheOldestUnpresentedMove)
{
    RenderStats stats;
    // Three moves without a present, then one present: one latency sample,
    // from the first move. A move further than the gap from the previous one
    // forms no pair.
    stats.OnMouseMove(100, 100, 0.0);
    stats.OnMouseMove(110, 110, 10.0);
    stats.OnMouseMove(120, 120, 20.0);
    std::string line = stats.OnFrame(1.0, 0.0, true, 130, 30.0, 0);
    FD2D_CHECK(line.empty());
    stats.OnMouseMove(500, 500, 400.0);
    line = stats.OnFrame(1.0, 0.0, false, 500, 400.0, RenderStats::kWindowMs);
    std::printf("  %s\n", line.c_str());
    FD2D_CHECK(Contains(line, "moves=4 presentsPerMove(pairs=2 avg=0.00 max=0 hist0=2 "));
    FD2D_CHECK(Contains(line, "inputToPresentMs(n=1 med=30 p95=30 max=30)"));
}

FD2D_TEST(RenderStats_AMoveThatRendersNothingIsNotTimedByALaterPresent)
{
    RenderStats stats;
    // A hover move that presents nothing, then 2 s later a move that does.
    stats.OnMouseMove(1000, 1000, 1000.0);
    stats.OnMouseMove(3000, 3000, 3000.0);
    (void)stats.OnFrame(1.0, 0.0, true, 3008, 3008.0, 0);
    const std::string line = stats.OnFrame(1.0, 0.0, false, 3008, 3008.0, RenderStats::kWindowMs);
    std::printf("  %s\n", line.c_str());
    FD2D_CHECK(Contains(line, "inputToPresentMs(n=1 med=8 p95=8 max=8)"));
}

FD2D_TEST(RenderStats_IsOffByDefault)
{
    // The test process does not set FD2D_RENDER_STATS (ctest runs it without).
    char* value = nullptr;
    std::size_t length = 0;
    const bool set = _dupenv_s(&value, &length, "FD2D_RENDER_STATS") == 0 && value != nullptr && value[0] != '\0';
    std::free(value);
    if (!set)
    {
        FD2D_CHECK(!RenderStats::Enabled());
    }
}
