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
