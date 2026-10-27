#include "TestHarness.h"
#include "WindowlessTestSupport.h"

#include "Backplate.h"
#include "Image.h"
#include "Util.h"

#include <windows.h>
#include <d2d1.h>
#include <wrl/client.h>

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
