#include "TestHarness.h"

#include "SplitPanel.h"

#include <cmath>
#include <memory>

// SplitPanel keeps the requested ratio apart from the effective (clamped) one:
// while a pane extent is clamped, RequestedSplitRatio() still reports what was
// asked for, and a split given that ratio arranges like the original before and
// after the clamp releases. A split given the clamped SplitRatio() does not.

namespace
{
    constexpr float kWidth = 300.0f;
    constexpr float kSecondMax = 100.0f;

    std::shared_ptr<FD2D::SplitPanel> MakeSplit(float ratio)
    {
        auto split = std::make_shared<FD2D::SplitPanel>(L"split", FD2D::SplitterOrientation::Vertical);
        split->SetFirstChild(std::make_shared<FD2D::Wnd>(L"first"));
        split->SetSecondChild(std::make_shared<FD2D::Wnd>(L"second"));
        split->SetSecondPaneMaxExtent(kSecondMax);
        split->SetSplitRatio(ratio);
        return split;
    }

    void Layout(FD2D::SplitPanel& split, float height)
    {
        (void)split.Measure({ kWidth, height });
        split.Arrange({ 0.0f, 0.0f, kWidth, height });
    }

    float SecondHeight(const std::shared_ptr<FD2D::SplitPanel>& split)
    {
        const auto& children = split->ChildrenInOrder();
        for (const auto& child : children)
        {
            if (child && child->Name() == L"second")
            {
                const D2D1_RECT_F r = child->LayoutRect();
                return r.bottom - r.top;
            }
        }
        return -1.0f;
    }
}

FD2D_TEST(SplitPanel_RequestedRatioSurvivesAClampAndCarriesItsIntent)
{
    // At 400 the second pane is 80 (under its max); at 1000 it would be 200
    // and is clamped to 100.
    constexpr float kRatio = 0.8f;
    auto original = MakeSplit(kRatio);
    Layout(*original, 400.0f);
    const float unclampedSecond = SecondHeight(original);
    FD2D_REQUIRE(unclampedSecond > 0.0f && unclampedSecond < kSecondMax);

    Layout(*original, 1000.0f);
    FD2D_CHECK(std::fabs(SecondHeight(original) - kSecondMax) < 1.0f);
    FD2D_CHECK(std::fabs(original->RequestedSplitRatio() - kRatio) < 0.0001f);
    FD2D_CHECK(original->SplitRatio() > kRatio + 0.01f);   // the clamped, effective ratio

    // Copied while clamped: the requested ratio follows the original, the
    // effective one does not once the clamp releases.
    auto followsIntent = MakeSplit(original->RequestedSplitRatio());
    auto followsClamp = MakeSplit(original->SplitRatio());
    Layout(*followsIntent, 1000.0f);
    Layout(*followsClamp, 1000.0f);
    FD2D_CHECK(std::fabs(SecondHeight(followsIntent) - SecondHeight(original)) < 0.5f);
    FD2D_CHECK(std::fabs(SecondHeight(followsClamp) - SecondHeight(original)) < 0.5f);

    Layout(*original, 400.0f);
    Layout(*followsIntent, 400.0f);
    Layout(*followsClamp, 400.0f);
    FD2D_CHECK(std::fabs(SecondHeight(original) - unclampedSecond) < 0.5f);
    FD2D_CHECK(std::fabs(SecondHeight(followsIntent) - unclampedSecond) < 0.5f);
    FD2D_CHECK(std::fabs(SecondHeight(followsClamp) - unclampedSecond) > 5.0f);
}
