#include "TestHarness.h"
#include "WindowlessTestSupport.h"

#include "Backplate.h"
#include "Image.h"
#include "Util.h"

#include <windows.h>
#include <d2d1.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

// The alpha checkerboard on Image's D2D bitmap path, rendered through the
// windowless D3D-then-D2D backplate and compared pixel by pixel with the tile
// oracle of FD2D issue 6, in the image's pre-rotation frame:
//   startX = floor(destRect.left / 8) * 8, startY = floor(destRect.top / 8) * 8;
//   tile (i, j) covers [startX + 8i, startX + 8(i+1)) x [startY + 8j, startY + 8(j+1)),
//   dark when (i + j) is odd, light otherwise;
//   the covered area is the union of the tiles that overlap destRect, clipped
//   by the image's layout rect.
// The image is fully transparent, so every covered pixel shows the
// checkerboard and every other pixel the black clear color.
//
// Scene: a 128 x 96 backplate; the Image has a 16 px margin, so its layout
// (and clip) is the non-square 96 x 64 rect (16, 16, 112, 80) with its center
// (64, 48) on a pixel corner. The content is 40 x 30 texels.

using FD2DWindowlessTest::Bgra;
using FD2DWindowlessTest::Frame;

namespace
{
    constexpr UINT kSurfaceW = 128;
    constexpr UINT kSurfaceH = 96;
    constexpr float kMargin = 16.0f;
    const D2D1_RECT_F kLayout = D2D1::RectF(kMargin, kMargin, kSurfaceW - kMargin, kSurfaceH - kMargin);
    const D2D1_SIZE_F kContent = D2D1::SizeF(40.0f, 30.0f);

    // Light 0.94 and dark 0.44 gray (round(level * 255)).
    constexpr Bgra kLight { 240, 240, 240, 255 };
    constexpr Bgra kDark { 112, 112, 112, 255 };
    constexpr Bgra kBlack { 0, 0, 0, 255 };

    struct Scene
    {
        FD2DWindowlessTest::ComScope com;
        FD2DWindowlessTest::CoreScope core;
        FD2D::Backplate backplate { L"checkerboard-test" };
        std::shared_ptr<FD2D::Image> image { std::make_shared<FD2D::Image>(L"image") };

        bool Create()
        {
            backplate.SetClearColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
            image->SetMargin(kMargin);
            if (FAILED(backplate.InitializeWindowless(kSurfaceW, kSurfaceH, 1.0f)) || !backplate.AddWnd(image))
            {
                return false;
            }

            // Fully transparent content on the backplate's own D2D target.
            ID2D1RenderTarget* target = backplate.RenderTarget();
            if (target == nullptr)
            {
                return false;
            }
            const UINT w = static_cast<UINT>(kContent.width);
            const UINT h = static_cast<UINT>(kContent.height);
            std::vector<std::uint32_t> pixels(static_cast<size_t>(w) * h, 0u);
            const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
            if (FAILED(target->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w * 4u, &props, &bitmap)))
            {
                return false;
            }
            image->SetBitmap(bitmap);
            return image->ContentPixelSize().width == w;
        }
    };

    struct Case
    {
        float zoom;
        float panX;
        float panY;
    };

    // Expected color of the pixel at (x, y) by the oracle.
    Bgra Expected(UINT x, UINT y, const D2D1_RECT_F& dest, int quarters)
    {
        const float sx = static_cast<float>(x) + 0.5f;
        const float sy = static_cast<float>(y) + 0.5f;
        if (sx < kLayout.left || sx >= kLayout.right || sy < kLayout.top || sy >= kLayout.bottom)
        {
            return kBlack; // clipped
        }

        // Pixel center in the pre-rotation frame (inverse of Image's rotation).
        D2D1::Matrix3x2F inverse = D2D1::Matrix3x2F::Rotation(
            static_cast<float>(quarters) * 90.0f,
            D2D1::Point2F((kLayout.left + kLayout.right) * 0.5f, (kLayout.top + kLayout.bottom) * 0.5f));
        if (!inverse.Invert())
        {
            return Bgra { 1, 2, 3, 4 };
        }
        const D2D1_POINT_2F p = inverse.TransformPoint(D2D1::Point2F(sx, sy));

        const double startX = std::floor(static_cast<double>(dest.left) / 8.0) * 8.0;
        const double startY = std::floor(static_cast<double>(dest.top) / 8.0) * 8.0;
        const double columns = std::ceil((static_cast<double>(dest.right) - startX) / 8.0);
        const double rows = std::ceil((static_cast<double>(dest.bottom) - startY) / 8.0);
        const double i = std::floor((p.x - startX) / 8.0);
        const double j = std::floor((p.y - startY) / 8.0);
        if (i < 0.0 || j < 0.0 || i >= columns || j >= rows)
        {
            return kBlack; // outside the tiles that overlap destRect
        }
        const bool dark = (static_cast<long long>(i + j) & 1) != 0;
        return dark ? kDark : kLight;
    }

    // Renders one case and compares every pixel of the surface with the oracle.
    // Returns the number of mismatching pixels (printing the first few).
    int RenderAndCompare(Scene& scene, const Case& c, int quarters, double& renderMs)
    {
        FD2D::Image::DrawState state = scene.image->GetDrawState();
        state.zoomScale = c.zoom;
        state.panX = c.panX;
        state.panY = c.panY;
        state.rotationQuarters = quarters;
        state.highQualitySampling = false;
        state.alphaCheckerboardEnabled = true;
        scene.image->SetDrawState(state);

        const auto start = std::chrono::steady_clock::now();
        scene.backplate.Render();
        renderMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        Frame frame;
        if (FAILED(frame.Read(scene.backplate, D2D1::RectF(0.0f, 0.0f, kSurfaceW, kSurfaceH))) ||
            frame.width != kSurfaceW || frame.height != kSurfaceH)
        {
            std::printf("    readback failed\n");
            return -1;
        }

        D2D1_RECT_F dest = FD2D::Util::ComputeAspectFitRect(kLayout, kContent, quarters);
        dest = FD2D::Util::ApplyZoomPanToRect(dest, c.zoom, c.panX, c.panY);

        int mismatches = 0;
        for (UINT y = 0; y < kSurfaceH; ++y)
        {
            for (UINT x = 0; x < kSurfaceW; ++x)
            {
                const Bgra expected = Expected(x, y, dest, quarters);
                const Bgra actual = frame.At(x, y);
                const auto close = [](std::uint8_t a, std::uint8_t b) { return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= 1; };
                if (!(close(actual.b, expected.b) && close(actual.g, expected.g) &&
                      close(actual.r, expected.r) && close(actual.a, expected.a)))
                {
                    if (mismatches < 4)
                    {
                        (void)frame.Near(x, y, expected); // prints the pixel
                    }
                    ++mismatches;
                }
            }
        }
        if (mismatches > 0)
        {
            frame.Print("    row", kSurfaceH / 2);
        }
        return mismatches;
    }
}

FD2D_TEST(ImageCheckerboard_MatchesTileOracle)
{
    Scene scene;
    FD2D_REQUIRE(scene.Create());

    // Letterboxed (coverage edges inside the clip), smaller than the layout,
    // panned with fractional offsets, and zoomed far past the layout; with
    // destRect.left/top negative and not multiples of 8 in the zoomed cases.
    const Case cases[] = {
        { 1.0f, 0.0f, 0.0f },
        { 1.0f, 5.5f, -3.25f },
        { 0.5f, 3.25f, -6.5f },
        { 3.7f, 13.3f, -7.6f },
        { 40.0f, -250.5f, 133.25f },
    };
    for (int quarters = 0; quarters < 4; ++quarters)
    {
        for (const auto& c : cases)
        {
            double renderMs = 0.0;
            const int mismatches = RenderAndCompare(scene, c, quarters, renderMs);
            std::printf("  rotation %3d zoom %5.1f pan (%7.2f, %7.2f): %d mismatching pixels, render %.1f ms\n",
                quarters * 90, c.zoom, c.panX, c.panY, mismatches, renderMs);
            FD2D_CHECK(mismatches == 0);
        }
    }
}

FD2D_TEST(ImageCheckerboard_HighZoomDrawsOnlyTheVisibleTiles)
{
    Scene scene;
    FD2D_REQUIRE(scene.Create());

    // destRect is millions of pixels wide (about 10^11 tiles); only the
    // visible ones are drawn, so a frame stays fast and the pattern exact.
    const Case c { 50000.0f, 1234.5f, -987.25f };
    for (int quarters = 0; quarters < 4; ++quarters)
    {
        double renderMs = 0.0;
        const int mismatches = RenderAndCompare(scene, c, quarters, renderMs);
        std::printf("  rotation %3d zoom %.0f: %d mismatching pixels, render %.1f ms\n",
            quarters * 90, c.zoom, mismatches, renderMs);
        FD2D_CHECK(mismatches == 0);
        // Generous bound for a slow CI machine; the per-tile loop would not
        // finish at all.
        FD2D_CHECK(renderMs < 5000.0);
    }
}

// Fractional geometry and device scale, as in the app: a layout rect with
// fractional edges and odd, fractional dimensions puts the rotation center off
// the pixel grid, so at 90/180/270 degrees the tile edges land between pixel
// centers (a fractional pan moves destRect off the grid too), and a display
// scale of 125-200% maps each 8 DIP tile to 10-16 device pixels. The original
// per-tile fills antialias edges off the grid and keep edges on it crisp; the
// tile oracle at pixel centers cannot express the former, so these tests
// compare with the original rendering: LegacyCheckerboardWnd draws the old
// loop (one antialiased FillRectangle per 8 px tile over destRect, under the
// same clip and rotation) in a second backplate of the same size and scale.

namespace
{
    // Multiples of 4 DIPs, so the surface is a whole number of pixels at
    // every tested scale.
    constexpr UINT kLegacyW = 128;
    constexpr UINT kLegacyH = 96;
    // 96.55 x 64.75, center (64.575, 47.975).
    const FD2D::Rect kFractionalLayout { 16.3f, 15.6f, 96.55f, 64.75f };
    // 96 x 64 at (16, 16), center (64, 48): on the device pixel grid at every
    // tested scale (multiples of 4 DIPs).
    const FD2D::Rect kAlignedLayout { 16.0f, 16.0f, 96.0f, 64.0f };

    // Places its one child at a fixed rect, whatever the backplate gives it,
    // and draws it under an extra Scale(scaleX, scaleY) around the origin, as
    // a parent (or the backplate's per-axis live-resize scale) does.
    class FixedPlacement : public FD2D::Wnd
    {
    public:
        explicit FixedPlacement(const FD2D::Rect& rect)
            : FD2D::Wnd(L"placement")
            , m_rect(rect)
        {
        }

        void SetScale(float scaleX, float scaleY)
        {
            m_scaleX = scaleX;
            m_scaleY = scaleY;
        }

        void OnRender(ID2D1RenderTarget* target) override
        {
            D2D1::Matrix3x2F previous;
            target->GetTransform(&previous);
            target->SetTransform(D2D1::Matrix3x2F::Scale(m_scaleX, m_scaleY) * previous);
            FD2D::Wnd::OnRender(target);
            target->SetTransform(previous);
        }

        void Arrange(FD2D::Rect finalRect) override
        {
            m_bounds = finalRect;
            m_layoutRect = FD2D::ToD2D(finalRect);
            for (auto& child : m_childrenOrdered)
            {
                child->Arrange(m_rect);
            }
        }

    private:
        FD2D::Rect m_rect {};
        float m_scaleX { 1.0f };
        float m_scaleY { 1.0f };
    };

    class LegacyCheckerboardWnd : public FD2D::Wnd
    {
    public:
        LegacyCheckerboardWnd()
            : FD2D::Wnd(L"legacy")
        {
        }

        void SetDrawState(const FD2D::Image::DrawState& state) { m_state = state; }

        void OnRender(ID2D1RenderTarget* target) override
        {
            if (target == nullptr)
            {
                return;
            }
            if (RefreshTargetResourceStamp(m_stamp) || !m_light || !m_dark)
            {
                m_light.Reset();
                m_dark.Reset();
                (void)target->CreateSolidColorBrush(D2D1::ColorF(0.94f, 0.94f, 0.94f, 1.0f), &m_light);
                (void)target->CreateSolidColorBrush(D2D1::ColorF(0.44f, 0.44f, 0.44f, 1.0f), &m_dark);
            }
            const D2D1_RECT_F layoutRect = LayoutRect();
            target->PushAxisAlignedClip(layoutRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            D2D1_RECT_F destRect = FD2D::Util::ComputeAspectFitRect(layoutRect, kContent, m_state.rotationQuarters);
            destRect = FD2D::Util::ApplyZoomPanToRect(destRect, m_state.zoomScale, m_state.panX, m_state.panY);
            D2D1::Matrix3x2F incoming;
            target->GetTransform(&incoming);
            const D2D1::Matrix3x2F rotation = D2D1::Matrix3x2F::Rotation(
                static_cast<float>(m_state.rotationQuarters) * 90.0f,
                D2D1::Point2F((layoutRect.left + layoutRect.right) * 0.5f, (layoutRect.top + layoutRect.bottom) * 0.5f));
            target->SetTransform(rotation * incoming);
            if (m_light && m_dark)
            {
                // The original loop, verbatim.
                constexpr float tile = 8.0f;
                const float startX = std::floor(destRect.left / tile) * tile;
                const float startY = std::floor(destRect.top / tile) * tile;
                for (float y = startY; y < destRect.bottom; y += tile)
                {
                    for (float x = startX; x < destRect.right; x += tile)
                    {
                        const int ix = static_cast<int>(std::floor((x - startX) / tile));
                        const int iy = static_cast<int>(std::floor((y - startY) / tile));
                        const bool dark = (((ix + iy) & 1) != 0);
                        target->FillRectangle(D2D1_RECT_F { x, y, x + tile, y + tile }, dark ? m_dark.Get() : m_light.Get());
                    }
                }
            }
            target->SetTransform(incoming);
            target->PopAxisAlignedClip();
        }

    private:
        FD2D::Image::DrawState m_state {};
        FD2D::GraphicsResourceStamp m_stamp {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_light {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_dark {};
    };

    struct LegacyScene
    {
        FD2DWindowlessTest::ComScope com;
        FD2DWindowlessTest::CoreScope core;
        FD2D::Backplate imageBackplate { L"checkerboard-image" };
        FD2D::Backplate legacyBackplate { L"checkerboard-legacy" };
        std::shared_ptr<FD2D::Image> image { std::make_shared<FD2D::Image>(L"image") };
        std::shared_ptr<LegacyCheckerboardWnd> legacy { std::make_shared<LegacyCheckerboardWnd>() };
        std::shared_ptr<FixedPlacement> imagePlacement {};
        std::shared_ptr<FixedPlacement> legacyPlacement {};
        FD2D::Rect layout {};
        float scale { 1.0f };
        // The parent's (incoming) scale per axis, on top of the backplate's.
        float incomingX { 1.0f };
        float incomingY { 1.0f };

        void SetIncomingScale(float x, float y)
        {
            incomingX = x;
            incomingY = y;
            imagePlacement->SetScale(x, y);
            legacyPlacement->SetScale(x, y);
        }

        bool Create(const FD2D::Rect& layoutRect, float dpiScale, float scaleX = 1.0f, float scaleY = 1.0f)
        {
            layout = layoutRect;
            scale = dpiScale;
            imagePlacement = std::make_shared<FixedPlacement>(layout);
            legacyPlacement = std::make_shared<FixedPlacement>(layout);
            SetIncomingScale(scaleX, scaleY);
            imageBackplate.SetClearColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
            legacyBackplate.SetClearColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
            if (FAILED(imageBackplate.InitializeWindowless(kLegacyW, kLegacyH, scale)) ||
                FAILED(legacyBackplate.InitializeWindowless(kLegacyW, kLegacyH, scale)) ||
                !imagePlacement->AddChild(image) || !legacyPlacement->AddChild(legacy) ||
                !imageBackplate.AddWnd(imagePlacement) || !legacyBackplate.AddWnd(legacyPlacement))
            {
                return false;
            }
            ID2D1RenderTarget* target = imageBackplate.RenderTarget();
            if (target == nullptr)
            {
                return false;
            }
            const UINT w = static_cast<UINT>(kContent.width);
            const UINT h = static_cast<UINT>(kContent.height);
            std::vector<std::uint32_t> pixels(static_cast<size_t>(w) * h, 0u);
            const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
            if (FAILED(target->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w * 4u, &props, &bitmap)))
            {
                return false;
            }
            image->SetBitmap(bitmap);
            return image->ContentPixelSize().width == w;
        }

        struct Result
        {
            int interior { 0 };         // device pixels whose footprint lies in one tile
            int interiorWrong { 0 };    // ... where the image is not that tile's color
            int interiorVsLegacy { 0 }; // ... where image and original differ by more than 1
            int legacyWrong { 0 };      // ... where the original is not the tile's color (sanity)
            int seam { 0 };             // device pixels whose footprint crosses a tile edge
            int seamOutOfRange { 0 };   // ... where the image is not a light/dark mix
            int seamOffCoverage { 0 };  // ... where it is not the area-weighted mix
            int maxCoverageDiff { 0 };
            int seamDiffers { 0 };      // ... where image and original differ by more than 3
            int maxSeamDiff { 0 };
            bool ok { false };
        };

        // Renders both with `state` and classifies every device pixel that
        // lies fully inside the clip by its footprint mapped back into the
        // image's pre-rotation frame (DIPs).
        Result Compare(const FD2D::Image::DrawState& state)
        {
            Result result;
            image->SetDrawState(state);
            legacy->SetDrawState(state);
            imageBackplate.Render();
            legacyBackplate.Render();
            Frame actual;
            Frame expected;
            const D2D1_RECT_F all = D2D1::RectF(0.0f, 0.0f, static_cast<float>(kLegacyW), static_cast<float>(kLegacyH));
            if (FAILED(actual.Read(imageBackplate, all)) || FAILED(expected.Read(legacyBackplate, all)) ||
                actual.width != expected.width || actual.height != expected.height)
            {
                return result;
            }

            const D2D1_RECT_F layoutRect = FD2D::ToD2D(layout);
            D2D1_RECT_F dest = FD2D::Util::ComputeAspectFitRect(layoutRect, kContent, state.rotationQuarters);
            dest = FD2D::Util::ApplyZoomPanToRect(dest, state.zoomScale, state.panX, state.panY);
            const double startX = std::floor(static_cast<double>(dest.left) / 8.0) * 8.0;
            const double startY = std::floor(static_cast<double>(dest.top) / 8.0) * 8.0;
            // Pre-rotation DIPs -> device pixels: rotation, then the parent's
            // per-axis scale, then the backplate's scale.
            const double deviceX = static_cast<double>(scale) * incomingX;
            const double deviceY = static_cast<double>(scale) * incomingY;
            D2D1::Matrix3x2F inverse = D2D1::Matrix3x2F::Rotation(
                static_cast<float>(state.rotationQuarters) * 90.0f,
                D2D1::Point2F((layoutRect.left + layoutRect.right) * 0.5f, (layoutRect.top + layoutRect.bottom) * 0.5f)) *
                D2D1::Matrix3x2F::Scale(static_cast<float>(deviceX), static_cast<float>(deviceY));
            if (!inverse.Invert())
            {
                return result;
            }
            const double clipL = static_cast<double>(layoutRect.left) * deviceX;
            const double clipT = static_cast<double>(layoutRect.top) * deviceY;
            const double clipR = static_cast<double>(layoutRect.right) * deviceX;
            const double clipB = static_cast<double>(layoutRect.bottom) * deviceY;

            const auto close = [](const Bgra& a, const Bgra& b, int tolerance)
            {
                return std::abs(a.b - b.b) <= tolerance && std::abs(a.g - b.g) <= tolerance &&
                    std::abs(a.r - b.r) <= tolerance && std::abs(a.a - b.a) <= tolerance;
            };

            for (UINT y = 0; y < actual.height; ++y)
            {
                for (UINT x = 0; x < actual.width; ++x)
                {
                    const float px = static_cast<float>(x);
                    const float py = static_cast<float>(y);
                    if (px < clipL - 1e-3 || px + 1.0 > clipR + 1e-3 || py < clipT - 1e-3 || py + 1.0 > clipB + 1e-3)
                    {
                        continue; // on or outside the clip edge
                    }
                    // Pre-rotation bounds of the device pixel square (exact for quarter turns).
                    const D2D1_POINT_2F c0 = inverse.TransformPoint(D2D1::Point2F(px, py));
                    const D2D1_POINT_2F c1 = inverse.TransformPoint(D2D1::Point2F(px + 1.0f, py + 1.0f));
                    const double minX = (std::min)(c0.x, c1.x);
                    const double maxX = (std::max)(c0.x, c1.x);
                    const double minY = (std::min)(c0.y, c1.y);
                    const double maxY = (std::max)(c0.y, c1.y);
                    constexpr double kEps = 1e-3;
                    const double i0 = std::floor((minX + kEps - startX) / 8.0);
                    const double i1 = std::floor((maxX - kEps - startX) / 8.0);
                    const double j0 = std::floor((minY + kEps - startY) / 8.0);
                    const double j1 = std::floor((maxY - kEps - startY) / 8.0);

                    const Bgra a = actual.At(x, y);
                    const Bgra e = expected.At(x, y);
                    if (i0 == i1 && j0 == j1)
                    {
                        ++result.interior;
                        const Bgra color = ((static_cast<long long>(i0 + j0) & 1) != 0) ? kDark : kLight;
                        if (!close(a, color, 1))
                        {
                            if (result.interiorWrong < 3)
                            {
                                std::printf("    interior (%u, %u): image %3u, tile %3u, original %3u\n", x, y, a.g, color.g, e.g);
                            }
                            ++result.interiorWrong;
                        }
                        if (!close(a, e, 1))
                        {
                            ++result.interiorVsLegacy;
                        }
                        if (!close(e, color, 1))
                        {
                            ++result.legacyWrong;
                        }
                        continue;
                    }

                    ++result.seam;
                    const bool mix = a.a >= 254 && a.g >= kDark.g - 1 && a.g <= kLight.g + 1 &&
                        a.r == a.g && a.b == a.g;
                    if (!mix)
                    {
                        ++result.seamOutOfRange;
                    }
                    // Area-weighted mix of the tiles under the pixel (a box
                    // filter, as antialiased tile edges would give).
                    const double footprint = (maxX - minX) * (maxY - minY);
                    double expectedLevel = 0.0;
                    for (double i = i0; i <= i1; i += 1.0)
                    {
                        const double x0 = (std::max)(minX, startX + 8.0 * i);
                        const double x1 = (std::min)(maxX, startX + 8.0 * (i + 1.0));
                        for (double j = j0; j <= j1; j += 1.0)
                        {
                            const double y0 = (std::max)(minY, startY + 8.0 * j);
                            const double y1 = (std::min)(maxY, startY + 8.0 * (j + 1.0));
                            const double area = (std::max)(0.0, x1 - x0) * (std::max)(0.0, y1 - y0);
                            const bool dark = (static_cast<long long>(i + j) & 1) != 0;
                            expectedLevel += area * (dark ? kDark.g : kLight.g);
                        }
                    }
                    expectedLevel /= footprint;
                    const int coverageDiff = std::abs(static_cast<int>(std::lround(expectedLevel)) - a.g);
                    result.maxCoverageDiff = (std::max)(result.maxCoverageDiff, coverageDiff);
                    if (coverageDiff > 4)
                    {
                        if (result.seamOffCoverage < 3)
                        {
                            std::printf("    seam (%u, %u): image %3u, area-weighted %3.0f, original %3u\n",
                                x, y, a.g, expectedLevel, e.g);
                        }
                        ++result.seamOffCoverage;
                    }
                    const int d = std::abs(a.g - e.g);
                    result.maxSeamDiff = (std::max)(result.maxSeamDiff, d);
                    if (d > 3)
                    {
                        ++result.seamDiffers;
                    }
                }
            }
            result.ok = true;
            return result;
        }
    };

    // Zoomed past the layout (the tiles cover the whole clip), with whole and
    // fractional pans: destRect.left/top are fractional and, for the larger
    // pans, negative.
    const Case kLegacyCases[] = {
        { 5.06f, 0.0f, 0.0f },
        { 5.06f, 13.3f, -7.6f },
        { 2.5f, -3.25f, 5.75f },
        { 40.0f, -250.5f, 133.25f },
    };

    // Runs every case at every rotation; returns the number of failing ones.
    int CheckAgainstLegacy(const FD2D::Rect& layout, float scale, const char* label,
        float incomingX = 1.0f, float incomingY = 1.0f)
    {
        LegacyScene scene;
        if (!scene.Create(layout, scale, incomingX, incomingY))
        {
            std::printf("  %s scale %.2f: setup failed\n", label, scale);
            return 1;
        }
        int failures = 0;
        for (int quarters = 0; quarters < 4; ++quarters)
        {
            for (const auto& c : kLegacyCases)
            {
                FD2D::Image::DrawState state {};
                state.zoomScale = c.zoom;
                state.panX = c.panX;
                state.panY = c.panY;
                state.rotationQuarters = quarters;
                state.highQualitySampling = false;
                state.alphaCheckerboardEnabled = true;
                const LegacyScene::Result r = scene.Compare(state);
                // Interior: the tile phase and colors, exactly as the original.
                // Seams: the area-weighted mix of the tiles under the pixel, so
                // a tile edge keeps its sub-pixel position under any rotation
                // and scale. (The original fills also let the background show
                // through where two antialiased tile edges meet, which darkens
                // those seams; that is not reproduced.)
                const bool pass = r.ok && r.interior > 3000 && r.interiorWrong == 0 && r.interiorVsLegacy == 0 &&
                    r.legacyWrong == 0 && r.seamOutOfRange == 0 && r.seamOffCoverage == 0;
                std::printf("  %s scale %.2f x (%.2f, %.2f) rotation %3d zoom %5.2f pan (%7.2f, %7.2f): interior %d (wrong %d, vs original %d, "
                    "original wrong %d), seam %d (not a mix %d, off the area-weighted mix %d (max %d), differ from original %d (max %d))%s\n",
                    label, scale, incomingX, incomingY, quarters * 90, c.zoom, c.panX, c.panY, r.interior, r.interiorWrong, r.interiorVsLegacy,
                    r.legacyWrong, r.seam, r.seamOutOfRange, r.seamOffCoverage, r.maxCoverageDiff, r.seamDiffers,
                    r.maxSeamDiff, pass ? "" : "  FAILED");
                if (!pass)
                {
                    ++failures;
                }
            }
        }
        return failures;
    }
}

FD2D_TEST(ImageCheckerboard_FractionalGeometryKeepsTilePhase)
{
    FD2D_CHECK(CheckAgainstLegacy(kFractionalLayout, 1.0f, "fractional") == 0);
}

FD2D_TEST(ImageCheckerboard_DeviceScaleKeepsTileEdges)
{
    // Common display scales: each 8 DIP tile is 10, 12, 14 or 16 device
    // pixels. Tile edges on the device pixel grid (aligned layout) stay crisp;
    // edges off it (fractional layout and pans) are area-weighted mixes.
    for (float scale : { 1.25f, 1.5f, 1.75f, 2.0f })
    {
        FD2D_CHECK(CheckAgainstLegacy(kAlignedLayout, scale, "aligned") == 0);
        FD2D_CHECK(CheckAgainstLegacy(kFractionalLayout, scale, "fractional") == 0);
    }
}

FD2D_TEST(ImageCheckerboard_AnisotropicScaleKeepsTileEdges)
{
    // A parent (or the backplate's deferred live-resize scale) can scale the
    // two axes differently. Each axis of the pattern gets its own device
    // resolution: at 90/270 degrees the pattern's x axis runs along the
    // device y axis.
    const float scales[][2] = { { 0.75f, 1.0f }, { 1.0f, 1.5f }, { 1.25f, 1.75f } };
    for (const auto& s : scales)
    {
        FD2D_CHECK(CheckAgainstLegacy(kAlignedLayout, 1.0f, "aligned", s[0], s[1]) == 0);
        FD2D_CHECK(CheckAgainstLegacy(kFractionalLayout, 1.0f, "fractional", s[0], s[1]) == 0);
    }
}

namespace FD2D
{
    struct ImageTestAccess
    {
        static ID2D1BitmapBrush* CheckerBrush(const Image& image) { return image.m_checkerBrush.Get(); }
        static D2D1_SIZE_U CheckerTexelsPerTile(const Image& image)
        {
            return D2D1::SizeU(image.m_checkerTexelsX, image.m_checkerTexelsY);
        }
    };
}

FD2D_TEST(ImageCheckerboard_HeightOnlyScaleChangeRebuildsBrush)
{
    LegacyScene scene;
    FD2D_REQUIRE(scene.Create(kAlignedLayout, 1.0f));
    FD2D::Image::DrawState state {};
    state.zoomScale = 5.06f;
    state.alphaCheckerboardEnabled = true;
    state.highQualitySampling = false;

    FD2D_REQUIRE(scene.Compare(state).ok);
    const Microsoft::WRL::ComPtr<ID2D1BitmapBrush> unscaled = FD2D::ImageTestAccess::CheckerBrush(*scene.image);
    D2D1_SIZE_U texels = FD2D::ImageTestAccess::CheckerTexelsPerTile(*scene.image);
    std::printf("  scale (1, 1): %u x %u texels per tile\n", texels.width, texels.height);
    FD2D_REQUIRE(unscaled != nullptr);
    FD2D_CHECK(texels.width == 8 && texels.height == 8);

    // Same scale: the brush is kept.
    FD2D_REQUIRE(scene.Compare(state).ok);
    FD2D_CHECK(FD2D::ImageTestAccess::CheckerBrush(*scene.image) == unscaled.Get());

    // Height only: rebuilt with more texels down, the same across.
    scene.SetIncomingScale(1.0f, 1.5f);
    const LegacyScene::Result r = scene.Compare(state);
    texels = FD2D::ImageTestAccess::CheckerTexelsPerTile(*scene.image);
    std::printf("  scale (1, 1.5): %u x %u texels per tile, interior wrong %d\n", texels.width, texels.height, r.interiorWrong);
    FD2D_CHECK(FD2D::ImageTestAccess::CheckerBrush(*scene.image) != unscaled.Get());
    FD2D_CHECK(texels.width == 8 && texels.height == 12);
    FD2D_CHECK(r.ok && r.interiorWrong == 0);

    // Rotated 90 degrees, the pattern's x axis runs down the device.
    state.rotationQuarters = 1;
    FD2D_REQUIRE(scene.Compare(state).ok);
    texels = FD2D::ImageTestAccess::CheckerTexelsPerTile(*scene.image);
    std::printf("  scale (1, 1.5), rotated 90: %u x %u texels per tile\n", texels.width, texels.height);
    FD2D_CHECK(texels.width == 12 && texels.height == 8);
}
