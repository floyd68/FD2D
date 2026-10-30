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

namespace FD2D
{
    struct SplitPanelTestAccess
    {
        // What a splitter drag does (Splitter::OnSplitChanged -> SplitPanel).
        static void OnSplitRatioChanged(SplitPanel& split, float ratio) { split.OnSplitRatioChanged(ratio); }
    };
}

// Characterization of a splitter-driven ratio change (OnSplitRatioChanged):
// with margin and padding, in both orientations, every node is laid out as a
// fresh Arrange of a split with the new ratio in the same rect would lay it
// out, and the split keeps its arranged rect. The change re-arranges itself
// only when the split is attached to a backplate (a windowless one here).
#include "WindowlessTestSupport.h"

namespace
{
    struct ArrangedSplit
    {
        std::shared_ptr<FD2D::SplitPanel> split;
        std::shared_ptr<FD2D::Wnd> first;
        std::shared_ptr<FD2D::Wnd> second;
    };

    ArrangedSplit MakeArranged(FD2D::SplitterOrientation orientation, float ratio, const FD2D::Rect& rect)
    {
        ArrangedSplit s;
        s.split = std::make_shared<FD2D::SplitPanel>(L"split", orientation);
        s.first = std::make_shared<FD2D::Wnd>(L"first");
        s.second = std::make_shared<FD2D::Wnd>(L"second");
        s.split->SetFirstChild(s.first);
        s.split->SetSecondChild(s.second);
        s.split->SetMargin(3.0f);
        s.split->SetPadding(5.0f);
        s.split->SetSplitRatio(ratio);
        (void)s.split->Measure({ rect.w, rect.h });
        s.split->Arrange(rect);
        return s;
    }

    bool SameRect(const D2D1_RECT_F& a, const D2D1_RECT_F& b)
    {
        const auto close = [](float x, float y) { return std::fabs(x - y) < 0.01f; };
        const bool same = close(a.left, b.left) && close(a.top, b.top) && close(a.right, b.right) && close(a.bottom, b.bottom);
        if (!same)
        {
            std::printf("    rect (%.2f, %.2f, %.2f, %.2f) != (%.2f, %.2f, %.2f, %.2f)\n",
                a.left, a.top, a.right, a.bottom, b.left, b.top, b.right, b.bottom);
        }
        return same;
    }

    // Every child (first, splitter, second) of `a` sits where it sits in `b`.
    bool SameLayout(const FD2D::SplitPanel& a, const FD2D::SplitPanel& b)
    {
        const auto& ca = a.ChildrenInOrder();
        const auto& cb = b.ChildrenInOrder();
        if (ca.size() != cb.size() || ca.size() != 3)
        {
            return false;
        }
        bool same = SameRect(a.LayoutRect(), b.LayoutRect());
        for (size_t i = 0; i < ca.size(); ++i)
        {
            same = SameRect(ca[i]->LayoutRect(), cb[i]->LayoutRect()) && same;
        }
        return same;
    }
}

FD2D_TEST(SplitPanel_SplitterRatioChangeReArrangesInTheArrangedRect)
{
    FD2DWindowlessTest::ComScope com;
    FD2DWindowlessTest::CoreScope core;
    const FD2D::Rect rect { 10.0f, 20.0f, 400.0f, 300.0f };
    for (const auto orientation : { FD2D::SplitterOrientation::Horizontal, FD2D::SplitterOrientation::Vertical })
    {
        FD2D::Backplate backplate(L"split-ratio-test");
        FD2D_REQUIRE(SUCCEEDED(backplate.InitializeWindowless(420, 330)));
        ArrangedSplit dragged = MakeArranged(orientation, 0.5f, rect);
        FD2D_REQUIRE(backplate.AddWnd(dragged.split));
        (void)dragged.split->Measure({ rect.w, rect.h });
        dragged.split->Arrange(rect);   // the backplate's layout may differ; arrange where the test says

        const D2D1_RECT_F before = dragged.first->LayoutRect();
        FD2D::SplitPanelTestAccess::OnSplitRatioChanged(*dragged.split, 0.3f);
        const D2D1_RECT_F after = dragged.first->LayoutRect();
        FD2D_CHECK(after.right != before.right || after.bottom != before.bottom);   // it did move

        const ArrangedSplit reference = MakeArranged(orientation, dragged.split->SplitRatio(), rect);
        std::printf("  %s: ratio %.3f, first child (%.1f, %.1f, %.1f, %.1f)\n",
            orientation == FD2D::SplitterOrientation::Horizontal ? "horizontal" : "vertical", dragged.split->SplitRatio(),
            dragged.first->LayoutRect().left, dragged.first->LayoutRect().top,
            dragged.first->LayoutRect().right, dragged.first->LayoutRect().bottom);
        FD2D_CHECK(SameLayout(*dragged.split, *reference.split));
        FD2D_CHECK(SameRect(dragged.split->LayoutRect(), D2D1::RectF(10.0f, 20.0f, 410.0f, 320.0f)));   // the arranged rect
    }
}
