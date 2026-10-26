#include "TestHarness.h"
#include "WindowlessTestSupport.h"

#include "Backplate.h"

#include <windows.h>
#include <d2d1.h>

#include <cstdio>
#include <memory>
#include <vector>

// Backplate windowless render mode: a WARP device and an offscreen BGRA8
// target, no window, swap chain or Present. Render() runs the windowed pass
// order (D3D pass, then D2D pass) and ReadComposedPixels() reads the frame.
//
// Scene (logical units): clear color opaque black; a D3D child clears
// [0, 32) x [0, 48) to opaque red with ClearRectD3D; a D2D child fills
// [16, 48) x [0, 48) with blue at 60% opacity. Composed D3D-under-D2D, the
// columns read: red | red under blue | blue over black | black.

using FD2DWindowlessTest::Bgra;
using FD2DWindowlessTest::ClearRectD3DWnd;
using FD2DWindowlessTest::FillRectD2DWnd;
using FD2DWindowlessTest::Frame;

namespace
{
    // 60% blue over red / over black, premultiplied: 0.6 * 255 = 153, 0.4 * 255 = 102.
    constexpr Bgra kRed { 0, 0, 255, 255 };
    constexpr Bgra kBlueOverRed { 153, 0, 102, 255 };
    constexpr Bgra kBlueOverBlack { 153, 0, 0, 255 };
    constexpr Bgra kBlack { 0, 0, 0, 255 };

    const D2D1_COLOR_F kRedColor = D2D1::ColorF(1.0f, 0.0f, 0.0f, 1.0f);
    const D2D1_COLOR_F kTranslucentBlue = D2D1::ColorF(0.0f, 0.0f, 1.0f, 0.6f);

    struct Scene
    {
        FD2DWindowlessTest::ComScope com;
        FD2DWindowlessTest::CoreScope core;
        FD2D::Backplate backplate { L"windowless-test" };
        std::shared_ptr<ClearRectD3DWnd> d3d {
            std::make_shared<ClearRectD3DWnd>(L"d3d", D2D1::RectF(0.0f, 0.0f, 32.0f, 48.0f), kRedColor) };
        std::shared_ptr<FillRectD2DWnd> d2d {
            std::make_shared<FillRectD2DWnd>(L"d2d", D2D1::RectF(16.0f, 0.0f, 48.0f, 48.0f), kTranslucentBlue) };

        bool Create(float dpiScale)
        {
            backplate.SetClearColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
            // The D3D child is added first, but the pass order - not the child
            // order - puts its content under the D2D child's.
            return SUCCEEDED(backplate.InitializeWindowless(64, 48, dpiScale)) &&
                backplate.AddWnd(d3d) && backplate.AddWnd(d2d);
        }
    };

    void CheckColumns(const Frame& frame, float dpiScale)
    {
        const auto x = [dpiScale](float logical) { return static_cast<UINT>(logical * dpiScale); };
        const UINT y = static_cast<UINT>(24.0f * dpiScale);
        frame.Print("  composed row", y);
        FD2D_CHECK(frame.Near(x(8.0f), y, kRed));
        FD2D_CHECK(frame.Near(x(24.0f), y, kBlueOverRed));
        FD2D_CHECK(frame.Near(x(40.0f), y, kBlueOverBlack));
        FD2D_CHECK(frame.Near(x(56.0f), y, kBlack));
        // Band edges land on the scaled logical boundaries.
        FD2D_CHECK(frame.Near(x(16.0f) - 1, y, kRed));
        FD2D_CHECK(frame.Near(x(16.0f), y, kBlueOverRed));
        FD2D_CHECK(frame.Near(x(32.0f) - 1, y, kBlueOverRed));
        FD2D_CHECK(frame.Near(x(32.0f), y, kBlueOverBlack));
        FD2D_CHECK(frame.Near(x(48.0f) - 1, y, kBlueOverBlack));
        FD2D_CHECK(frame.Near(x(48.0f), y, kBlack));
    }
}

FD2D_TEST(WindowlessRender_RejectsInvalidSetup)
{
    FD2DWindowlessTest::ComScope com;
    FD2DWindowlessTest::CoreScope core;
    FD2D::Backplate backplate(L"windowless-test");

    // Without the mode (and without a window) Render() draws nothing, as before.
    backplate.Render();
    Frame frame;
    FD2D_CHECK(frame.Read(backplate, D2D1::RectF(0.0f, 0.0f, 8.0f, 8.0f)) == E_NOINTERFACE);
    FD2D_CHECK(!backplate.IsWindowless());

    FD2D_CHECK(backplate.InitializeWindowless(0, 48) == E_INVALIDARG);
    FD2D_CHECK(backplate.InitializeWindowless(64, 0) == E_INVALIDARG);
    FD2D_CHECK(backplate.InitializeWindowless(64, 48, 0.0f) == E_INVALIDARG);
    FD2D_CHECK(backplate.InitializeWindowless(64, 48, -1.0f) == E_INVALIDARG);
    FD2D_CHECK(!backplate.IsWindowless());
}

FD2D_TEST(WindowlessRender_ComposesD3DUnderD2D)
{
    Scene scene;
    FD2D_REQUIRE(scene.Create(1.0f));
    FD2D::Backplate& backplate = scene.backplate;
    FD2D_CHECK(backplate.IsWindowless());
    FD2D_CHECK(backplate.Window() == nullptr);
    FD2D_CHECK(backplate.D3DDevice() != nullptr);
    FD2D_CHECK(backplate.D3DDevice()->GetFeatureLevel() >= D3D_FEATURE_LEVEL_10_0);

    // No frame before the first Render().
    Frame frame;
    FD2D_CHECK(frame.Read(backplate, D2D1::RectF(0.0f, 0.0f, 64.0f, 48.0f)) == HRESULT_FROM_WIN32(ERROR_NOT_READY));

    backplate.Render();
    FD2D_CHECK(scene.d3d->Frames() == 1);
    FD2D_CHECK(scene.d3d->LastClearSucceeded());
    FD2D_CHECK(scene.d2d->Frames() == 1);
    FD2D_CHECK(backplate.RenderSurfaceSize().width == 64);
    FD2D_CHECK(backplate.RenderSurfaceSize().height == 48);

    FD2D_REQUIRE(SUCCEEDED(frame.Read(backplate, D2D1::RectF(0.0f, 0.0f, 64.0f, 48.0f))));
    FD2D_CHECK(frame.width == 64);
    FD2D_CHECK(frame.height == 48);
    CheckColumns(frame, 1.0f);

    // A logical sub-rectangle reads only that part of the frame.
    Frame part;
    FD2D_REQUIRE(SUCCEEDED(part.Read(backplate, D2D1::RectF(16.0f, 8.0f, 32.0f, 16.0f))));
    FD2D_CHECK(part.width == 16);
    FD2D_CHECK(part.height == 8);
    FD2D_CHECK(part.Near(0, 0, kBlueOverRed));
    FD2D_CHECK(part.Near(15, 7, kBlueOverRed));

    // Each Render() is one frame.
    backplate.Render();
    FD2D_CHECK(scene.d3d->Frames() == 2);
    FD2D_CHECK(scene.d2d->Frames() == 2);
}

FD2D_TEST(WindowlessRender_DpiScaleMapsLogicalToSurface)
{
    Scene scene;
    FD2D_REQUIRE(scene.Create(1.5f));
    FD2D::Backplate& backplate = scene.backplate;
    backplate.Render();

    FD2D_CHECK(backplate.ClientSize().width == 64);
    FD2D_CHECK(backplate.ClientSize().height == 48);
    FD2D_CHECK(backplate.RenderSurfaceSize().width == 96);
    FD2D_CHECK(backplate.RenderSurfaceSize().height == 72);
    FD2D_CHECK(backplate.LogicalToRenderScale().width == 1.5f);
    FD2D_CHECK(backplate.LogicalToRenderScale().height == 1.5f);

    Frame frame;
    FD2D_REQUIRE(SUCCEEDED(frame.Read(backplate, D2D1::RectF(0.0f, 0.0f, 64.0f, 48.0f))));
    FD2D_CHECK(frame.width == 96);
    FD2D_CHECK(frame.height == 72);
    CheckColumns(frame, 1.5f);
}

FD2D_TEST(WindowlessRender_ReinitializeResizesTarget)
{
    Scene scene;
    FD2D_REQUIRE(scene.Create(1.0f));
    FD2D::Backplate& backplate = scene.backplate;
    backplate.Render();
    const FD2D::GraphicsGeneration before = backplate.GetGraphicsGeneration();

    FD2D_REQUIRE(SUCCEEDED(backplate.InitializeWindowless(64, 48, 2.0f)));
    FD2D_CHECK(backplate.GetGraphicsGeneration().device > before.device);
    FD2D_CHECK(backplate.GetGraphicsGeneration().target > before.target);
    backplate.Render();

    Frame frame;
    FD2D_REQUIRE(SUCCEEDED(frame.Read(backplate, D2D1::RectF(0.0f, 0.0f, 64.0f, 48.0f))));
    FD2D_CHECK(frame.width == 128);
    FD2D_CHECK(frame.height == 96);
    CheckColumns(frame, 2.0f);
}
