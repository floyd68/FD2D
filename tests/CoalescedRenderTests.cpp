#include "TestHarness.h"

#include "BackplateTestAccess.h"
#include "WindowlessTestSupport.h"

#include <cstdio>
#include <memory>
#include <string>

// Coalesced invalidation rendering (FD2D#4) on a windowless backplate:
// invalidations only request a render, any number of them produce one frame,
// an invalidation from a render callback is for the next frame (no extra
// pass), an explicit Render() draws the current state for ReadComposedPixels,
// and a file drag-over / drag-leave ends in one render with the final overlay.

using FD2D::BackplateTestAccess;
using FD2DWindowlessTest::Bgra;
using FD2DWindowlessTest::FillRectD2DWnd;
using FD2DWindowlessTest::Frame;

namespace
{
    constexpr UINT kW = 64;
    constexpr UINT kH = 32;

    const D2D1_COLOR_F kRed = D2D1::ColorF(1.0f, 0.0f, 0.0f, 1.0f);
    const D2D1_COLOR_F kGreen = D2D1::ColorF(0.0f, 1.0f, 0.0f, 1.0f);
    const D2D1_COLOR_F kBlue = D2D1::ColorF(0.0f, 0.0f, 1.0f, 1.0f);
    const Bgra kRedPx { 0, 0, 255, 255 };
    const Bgra kGreenPx { 0, 255, 0, 255 };
    const Bgra kBluePx { 255, 0, 0, 255 };

    struct Scene
    {
        FD2DWindowlessTest::ComScope com;
        FD2DWindowlessTest::CoreScope core;
        FD2D::Backplate backplate { L"coalesced-render-test" };

        bool Create()
        {
            return SUCCEEDED(backplate.InitializeWindowless(kW, kH, 1.0f));
        }

        Frame Read()
        {
            Frame frame;
            (void)frame.Read(backplate, D2D1::RectF(0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)));
            return frame;
        }
    };

    // A drop target over [left, right) of the client area: blue normally,
    // red while a file is dragged over it. Like a real control, a drag over
    // it first clears and then sets its overlay, invalidating each time.
    class DropZone : public FD2D::Wnd
    {
    public:
        DropZone(const std::wstring& name, float left, float right)
            : FD2D::Wnd(name)
            , m_left(left)
            , m_right(right)
        {
        }

        bool OnFileDrag(const std::wstring& path, const POINT& pt, FD2D::FileDragVisual& visual) override
        {
            (void)path;
            if (static_cast<float>(pt.x) < m_left || static_cast<float>(pt.x) >= m_right)
            {
                return false;
            }
            SetHover(false);
            SetHover(true);
            visual = FD2D::FileDragVisual::Replace;
            return true;
        }

        void OnFileDragLeave() override
        {
            SetHover(false);
        }

        void OnRender(ID2D1RenderTarget* target) override
        {
            ++m_frames;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
            if (target != nullptr && SUCCEEDED(target->CreateSolidColorBrush(m_hover ? kRed : kBlue, &brush)))
            {
                target->FillRectangle(D2D1::RectF(m_left, 0.0f, m_right, static_cast<float>(kH)), brush.Get());
            }
        }

        int Frames() const { return m_frames; }
        int Invalidations() const { return m_invalidations; }

    private:
        void SetHover(bool hover)
        {
            m_hover = hover;
            ++m_invalidations;
            Invalidate();
        }

        float m_left { 0.0f };
        float m_right { 0.0f };
        bool m_hover { false };
        int m_frames { 0 };
        int m_invalidations { 0 };
    };
}

FD2D_TEST(CoalescedRender_InvalidationsRequestOneRender)
{
    Scene scene;
    FD2D_REQUIRE(scene.Create());
    auto a = std::make_shared<FillRectD2DWnd>(L"a", D2D1::RectF(0.0f, 0.0f, 32.0f, 32.0f), kRed);
    auto b = std::make_shared<FillRectD2DWnd>(L"b", D2D1::RectF(32.0f, 0.0f, 64.0f, 32.0f), kRed);
    FD2D_REQUIRE(scene.backplate.AddWnd(a));
    FD2D_REQUIRE(scene.backplate.AddWnd(b));
    scene.backplate.Render();
    FD2D_REQUIRE(a->Frames() == 1);
    FD2D_CHECK(!scene.backplate.IsRenderPending());

    // Many invalidations from several controls and a layout request: no
    // frame yet, one request.
    for (int i = 0; i < 10; ++i)
    {
        a->Invalidate();
        b->Invalidate();
    }
    scene.backplate.RequestLayout();
    std::printf("  after 20 invalidations and a layout request: frames %d, pending %d\n",
        a->Frames(), scene.backplate.IsRenderPending() ? 1 : 0);
    FD2D_CHECK(a->Frames() == 1);
    FD2D_CHECK(scene.backplate.IsRenderPending());

    // The loop's flush renders it once; nothing is left.
    FD2D_CHECK(scene.backplate.RenderIfPending());
    FD2D_CHECK(a->Frames() == 2);
    FD2D_CHECK(b->Frames() == 2);
    FD2D_CHECK(!scene.backplate.IsRenderPending());
    FD2D_CHECK(!scene.backplate.RenderIfPending());
    FD2D_CHECK(a->Frames() == 2);
}

FD2D_TEST(CoalescedRender_InvalidationFromARenderCallbackIsForTheNextFrame)
{
    Scene scene;
    FD2D_REQUIRE(scene.Create());
    auto a = std::make_shared<FillRectD2DWnd>(L"a", D2D1::RectF(0.0f, 0.0f, 64.0f, 32.0f), kRed);
    FD2D::Wnd* self = a.get();
    a->SetOnRender([self]() { self->Invalidate(); self->Invalidate(); });
    FD2D_REQUIRE(scene.backplate.AddWnd(a));

    // A render whose callbacks invalidate draws one pass; the invalidations
    // request the next frame instead of re-rendering inside this one.
    scene.backplate.Render();
    std::printf("  one Render() with invalidating callbacks: %d pass(es), pending %d\n",
        a->Frames(), scene.backplate.IsRenderPending() ? 1 : 0);
    FD2D_CHECK(a->Frames() == 1);
    FD2D_CHECK(scene.backplate.IsRenderPending());

    // Each loop iteration then renders exactly one frame.
    FD2D_CHECK(scene.backplate.RenderIfPending());
    FD2D_CHECK(a->Frames() == 2);
    a->SetOnRender({});
    FD2D_CHECK(scene.backplate.RenderIfPending());
    FD2D_CHECK(a->Frames() == 3);
    FD2D_CHECK(!scene.backplate.IsRenderPending());
}

FD2D_TEST(CoalescedRender_ExplicitRenderThenReadComposedPixelsReadsTheCurrentFrame)
{
    Scene scene;
    FD2D_REQUIRE(scene.Create());
    auto a = std::make_shared<FillRectD2DWnd>(L"a", D2D1::RectF(0.0f, 0.0f, 64.0f, 32.0f), kRed);
    FD2D_REQUIRE(scene.backplate.AddWnd(a));
    scene.backplate.Render();
    FD2D_CHECK(scene.Read().Near(10, 10, kRedPx));

    // A change only requests a render: the composed frame is still the old one.
    a->SetColor(kGreen);
    a->Invalidate();
    FD2D_CHECK(scene.backplate.IsRenderPending());
    FD2D_CHECK(scene.Read().Near(10, 10, kRedPx));

    // The immediate path (as a screenshot does) renders the current state now
    // and satisfies the pending request.
    scene.backplate.Render();
    FD2D_CHECK(!scene.backplate.IsRenderPending());
    FD2D_CHECK(scene.Read().Near(10, 10, kGreenPx));
    FD2D_CHECK(a->Frames() == 2);
}

FD2D_TEST(CoalescedRender_FileDragOverAndLeaveEndInOneRenderWithTheFinalOverlay)
{
    Scene scene;
    FD2D_REQUIRE(scene.Create());
    auto left = std::make_shared<DropZone>(L"left", 0.0f, 32.0f);
    auto right = std::make_shared<DropZone>(L"right", 32.0f, 64.0f);
    FD2D_REQUIRE(scene.backplate.AddWnd(left));
    FD2D_REQUIRE(scene.backplate.AddWnd(right));
    scene.backplate.Render();
    FD2D_REQUIRE(left->Frames() == 1);

    struct Step
    {
        const char* what;
        int x;   // -1: drag leave
        Bgra leftPx;
        Bgra rightPx;
    };
    const Step steps[] = {
        { "drag over the right zone", 48, kBluePx, kRedPx },
        { "drag on over the right zone", 50, kBluePx, kRedPx },
        { "drag over the left zone", 10, kRedPx, kBluePx },
        { "drag leave", -1, kBluePx, kBluePx },
    };
    for (const Step& step : steps)
    {
        const int framesBefore = left->Frames();
        const int invalidationsBefore = left->Invalidations() + right->Invalidations();
        if (step.x >= 0)
        {
            FD2D_CHECK(BackplateTestAccess::FileDragOver(scene.backplate, L"C:\\dropped.png", POINT { step.x, 10 }));
        }
        else
        {
            BackplateTestAccess::FileDragLeave(scene.backplate);
        }
        const int invalidations = left->Invalidations() + right->Invalidations() - invalidationsBefore;
        const int syncFrames = left->Frames() - framesBefore;
        const bool pending = scene.backplate.IsRenderPending();
        (void)scene.backplate.RenderIfPending();
        const int frames = left->Frames() - framesBefore;
        std::printf("  %s: %d invalidations, %d synchronous frame(s), %d frame(s) after the flush\n",
            step.what, invalidations, syncFrames, frames);
        FD2D_CHECK(invalidations >= 2);
        FD2D_CHECK(syncFrames == 0);
        FD2D_CHECK(pending);
        FD2D_CHECK(frames == 1);
        const Frame frame = scene.Read();
        FD2D_CHECK(frame.Near(10, 10, step.leftPx));
        FD2D_CHECK(frame.Near(50, 10, step.rightPx));
    }
    FD2D_CHECK(!scene.backplate.IsDeferringRender());
}
