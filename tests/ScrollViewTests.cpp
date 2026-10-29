#include "TestHarness.h"

#include "ScrollView.h"

#include <cmath>
#include <memory>

// ScrollView::ShiftScrollX: compensating a content change keeps a smooth
// scroll in progress (current and target move together) and clamps.

namespace
{
    // Content of a fixed size.
    class FixedContent : public FD2D::Wnd
    {
    public:
        FixedContent(float w, float h)
            : FD2D::Wnd(L"content")
            , m_size { w, h }
        {
        }

        FD2D::Size Measure(FD2D::Size) override
        {
            m_desired = m_size;
            return m_desired;
        }

    private:
        FD2D::Size m_size;
    };

    std::shared_ptr<FD2D::ScrollView> MakeStrip()
    {
        auto scroll = std::make_shared<FD2D::ScrollView>(L"strip");
        scroll->SetVerticalScrollEnabled(false);
        scroll->SetContent(std::make_shared<FixedContent>(1000.0f, 50.0f));
        scroll->Measure({ 200.0f, 50.0f });
        scroll->Arrange({ 0.0f, 0.0f, 200.0f, 50.0f });
        return scroll;
    }

    bool Near(float a, float b)
    {
        return std::fabs(a - b) < 0.01f;
    }
}

FD2D_TEST(ScrollViewShift_KeepsASmoothScrollRunning)
{
    auto scroll = MakeStrip();
    scroll->SetScrollX(100.0f);
    FD2D_CHECK(Near(scroll->TargetScrollX(), 100.0f));

    // Start a smooth scroll towards an item further right.
    scroll->EnsureCentered(D2D1::RectF(600.0f, 0.0f, 650.0f, 50.0f), false);
    const float target = scroll->TargetScrollX();
    FD2D_REQUIRE(target > scroll->ScrollX() + 1.0f);
    const float remaining = target - scroll->ScrollX();

    scroll->ShiftScrollX(30.0f);
    FD2D_CHECK(Near(scroll->ScrollX(), 130.0f));
    FD2D_CHECK(Near(scroll->TargetScrollX() - scroll->ScrollX(), remaining));   // still animating

    // SetScrollX, by contrast, ends the animation.
    scroll->SetScrollX(scroll->ScrollX());
    FD2D_CHECK(Near(scroll->TargetScrollX(), scroll->ScrollX()));
}

FD2D_TEST(ScrollViewShift_ClampsAndHonoursDisabledAxis)
{
    auto scroll = MakeStrip();
    scroll->ShiftScrollX(10000.0f);
    FD2D_CHECK(Near(scroll->ScrollX(), 800.0f));        // content 1000 - viewport 200
    FD2D_CHECK(Near(scroll->TargetScrollX(), 800.0f));
    scroll->ShiftScrollX(-5000.0f);
    FD2D_CHECK(Near(scroll->ScrollX(), 0.0f));
    FD2D_CHECK(Near(scroll->TargetScrollX(), 0.0f));

    scroll->SetHorizontalScrollEnabled(false);
    scroll->ShiftScrollX(50.0f);
    FD2D_CHECK(Near(scroll->ScrollX(), 0.0f));
}

// Scrollbar gutters (SetScrollBarGutter): on an overflowing axis the content
// viewport, the content's arranged size, the scroll range and the content
// clip leave the bar's gutter free, the bar is drawn and hit-tested inside the
// gutter, and nothing changes without overflow or with the option off.

namespace
{
    constexpr float kGutter = FD2D::ScrollView::kScrollBarGutter;

    // Content whose width follows the height it gets (thumbnails of a fixed
    // aspect), as the thumbnail strip's does.
    class AspectContent : public FD2D::Wnd
    {
    public:
        explicit AspectContent(float widthPerHeight)
            : FD2D::Wnd(L"aspect")
            , m_widthPerHeight(widthPerHeight)
        {
        }

        FD2D::Size Measure(FD2D::Size available) override
        {
            m_desired = { m_widthPerHeight * available.h, available.h };
            return m_desired;
        }

    private:
        float m_widthPerHeight;
    };

    std::shared_ptr<FD2D::ScrollView> MakeView(std::shared_ptr<FD2D::Wnd> content, bool hScroll, bool vScroll,
        bool gutter, float w, float h)
    {
        auto scroll = std::make_shared<FD2D::ScrollView>(L"view");
        scroll->SetHorizontalScrollEnabled(hScroll);
        scroll->SetVerticalScrollEnabled(vScroll);
        scroll->SetScrollBarsVisible(true);
        scroll->SetScrollBarGutter(gutter);
        scroll->SetContent(std::move(content));
        scroll->Measure({ w, h });
        scroll->Arrange({ 0.0f, 0.0f, w, h });
        return scroll;
    }

    bool SameRect(const D2D1_RECT_F& a, const D2D1_RECT_F& b)
    {
        return Near(a.left, b.left) && Near(a.top, b.top) && Near(a.right, b.right) && Near(a.bottom, b.bottom);
    }

    bool Intersects(const D2D1_RECT_F& a, const D2D1_RECT_F& b)
    {
        return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
    }

    bool Inside(const D2D1_RECT_F& inner, const D2D1_RECT_F& outer)
    {
        return inner.left >= outer.left - 0.01f && inner.right <= outer.right + 0.01f &&
            inner.top >= outer.top - 0.01f && inner.bottom <= outer.bottom + 0.01f;
    }

    FD2D::InputEvent Mouse(FD2D::InputEventType type, float x, float y)
    {
        FD2D::InputEvent event {};
        event.type = type;
        event.button = FD2D::MouseButton::Left;
        event.hasPoint = true;
        event.point = POINT { static_cast<LONG>(x), static_cast<LONG>(y) };
        return event;
    }
}

FD2D_TEST(ScrollViewGutter_HorizontalOverflowLeavesTheBarItsGutter)
{
    auto content = std::make_shared<FixedContent>(1000.0f, 50.0f);
    auto scroll = MakeView(content, true, false, true, 200.0f, 50.0f);

    const D2D1_RECT_F viewport = scroll->ContentViewportRect();
    FD2D_CHECK(SameRect(viewport, D2D1::RectF(0.0f, 0.0f, 200.0f, 50.0f - kGutter)));
    const D2D1_RECT_F arranged = content->LayoutRect();
    FD2D_CHECK(Near(arranged.bottom - arranged.top, 50.0f - kGutter));   // content arranged to the viewport

    D2D1_RECT_F track {}, thumb {};
    FD2D_REQUIRE(scroll->HScrollBarRects(track, thumb));
    const D2D1_RECT_F gutter = D2D1::RectF(0.0f, 50.0f - kGutter, 200.0f, 50.0f);
    FD2D_CHECK(Inside(track, gutter));
    FD2D_CHECK(Inside(thumb, track));
    FD2D_CHECK(!Intersects(viewport, track));   // the content clip is the viewport

    // The scroll range uses the viewport width (unchanged by a bottom gutter).
    scroll->SetScrollX(10000.0f);
    FD2D_CHECK(Near(scroll->ScrollX(), 800.0f));
    scroll->SetScrollX(0.0f);

    // A press on the drawn thumb grabs it; dragging scrolls.
    FD2D_REQUIRE(scroll->HScrollBarRects(track, thumb));
    const float cx = 0.5f * (thumb.left + thumb.right);
    const float cy = 0.5f * (thumb.top + thumb.bottom);
    FD2D_CHECK(scroll->OnInputEvent(Mouse(FD2D::InputEventType::MouseDown, cx, cy)));
    FD2D_CHECK(scroll->OnInputEvent(Mouse(FD2D::InputEventType::MouseMove, cx + 20.0f, cy)));
    FD2D_CHECK(scroll->ScrollX() > 1.0f);
    FD2D_CHECK(scroll->OnInputEvent(Mouse(FD2D::InputEventType::MouseUp, cx + 20.0f, cy)));

    // A press in the gutter beside the track does not reach the content.
    FD2D_CHECK(!scroll->OnInputEvent(Mouse(FD2D::InputEventType::MouseDown, 100.0f, 50.0f - 1.0f)));
}

FD2D_TEST(ScrollViewGutter_NoOverflowOrOptionOffKeepsTheGeometry)
{
    // No overflow: no gutter, no bar.
    auto fits = MakeView(std::make_shared<FixedContent>(150.0f, 50.0f), true, false, true, 200.0f, 50.0f);
    FD2D_CHECK(SameRect(fits->ContentViewportRect(), fits->LayoutRect()));
    D2D1_RECT_F track {}, thumb {};
    FD2D_CHECK(!fits->HScrollBarRects(track, thumb));

    // Option off: the bar overlays the full viewport, as before.
    auto overlay = MakeView(std::make_shared<FixedContent>(1000.0f, 50.0f), true, false, false, 200.0f, 50.0f);
    FD2D_CHECK(SameRect(overlay->ContentViewportRect(), D2D1::RectF(0.0f, 0.0f, 200.0f, 50.0f)));
    FD2D_REQUIRE(overlay->HScrollBarRects(track, thumb));
    FD2D_CHECK(SameRect(track, D2D1::RectF(2.0f, 39.0f, 198.0f, 48.0f)));
    FD2D_CHECK(Intersects(overlay->ContentViewportRect(), track));
}

FD2D_TEST(ScrollViewGutter_ContentAtTheThresholdSettles)
{
    // 4.1 widths per height: 205 wide at the full 50 (overflows a 200 viewport),
    // 151.7 at the 37 left by a gutter (fits). The gutter is dropped, and every
    // later arrange decides the same way.
    auto scroll = MakeView(std::make_shared<AspectContent>(4.1f), true, false, true, 200.0f, 50.0f);
    const D2D1_RECT_F first = scroll->ContentViewportRect();
    FD2D_CHECK(SameRect(first, scroll->LayoutRect()));
    for (int i = 0; i < 5; ++i)
    {
        scroll->Measure({ 200.0f, 50.0f });
        scroll->Arrange({ 0.0f, 0.0f, 200.0f, 50.0f });
        FD2D_CHECK(SameRect(scroll->ContentViewportRect(), first));
    }

    // Wide enough to overflow with the gutter too: the gutter stays, every time.
    auto wide = MakeView(std::make_shared<AspectContent>(10.0f), true, false, true, 200.0f, 50.0f);
    for (int i = 0; i < 5; ++i)
    {
        wide->Measure({ 200.0f, 50.0f });
        wide->Arrange({ 0.0f, 0.0f, 200.0f, 50.0f });
        FD2D_CHECK(Near(wide->ContentViewportRect().bottom, 50.0f - kGutter));
    }
}

FD2D_TEST(ScrollViewGutter_VerticalAndBothAxes)
{
    // Vertical overflow: the right gutter narrows the viewport; the range uses
    // the full height.
    auto content = std::make_shared<FixedContent>(50.0f, 1000.0f);
    auto vertical = MakeView(content, false, true, true, 200.0f, 200.0f);
    FD2D_CHECK(SameRect(vertical->ContentViewportRect(), D2D1::RectF(0.0f, 0.0f, 200.0f - kGutter, 200.0f)));
    D2D1_RECT_F track {}, thumb {};
    FD2D_REQUIRE(vertical->VScrollBarRects(track, thumb));
    FD2D_CHECK(Inside(track, D2D1::RectF(200.0f - kGutter, 0.0f, 200.0f, 200.0f)));
    FD2D_CHECK(!Intersects(vertical->ContentViewportRect(), track));
    vertical->SetScrollY(10000.0f);
    FD2D_CHECK(Near(vertical->ScrollY(), 800.0f));

    // Both axes: both gutters, the bars stay out of the viewport and of each other.
    auto both = MakeView(std::make_shared<FixedContent>(1000.0f, 1000.0f), true, true, true, 200.0f, 200.0f);
    const D2D1_RECT_F viewport = both->ContentViewportRect();
    FD2D_CHECK(SameRect(viewport, D2D1::RectF(0.0f, 0.0f, 200.0f - kGutter, 200.0f - kGutter)));
    D2D1_RECT_F hTrack {}, hThumb {}, vTrack {}, vThumb {};
    FD2D_REQUIRE(both->HScrollBarRects(hTrack, hThumb));
    FD2D_REQUIRE(both->VScrollBarRects(vTrack, vThumb));
    FD2D_CHECK(!Intersects(viewport, hTrack));
    FD2D_CHECK(!Intersects(viewport, vTrack));
    FD2D_CHECK(!Intersects(hTrack, vTrack));
    both->SetScrollX(10000.0f);
    FD2D_CHECK(Near(both->ScrollX(), 1000.0f - (200.0f - kGutter)));

    // Vertical overflow only, but its gutter makes the width overflow: both gutters.
    auto narrowed = MakeView(std::make_shared<FixedContent>(195.0f, 1000.0f), true, true, true, 200.0f, 200.0f);
    FD2D_CHECK(SameRect(narrowed->ContentViewportRect(), D2D1::RectF(0.0f, 0.0f, 200.0f - kGutter, 200.0f - kGutter)));
}