#include "TestHarness.h"

#include "Image.h"
#include "Util.h"

#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

// Image::OnRender transform tests on a windowless WIC bitmap render target.
// A parent (scroll offset, translation) or the backplate (live-resize scale)
// sets a transform before a child renders; a rotated Image must compose its
// rotation with that transform and leave it unchanged afterwards.

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr UINT kTargetSize = 64;

    struct Canvas
    {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<ID2D1Factory> d2d;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<ID2D1RenderTarget> target;

        bool Create()
        {
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
                FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
                FAILED(wic->CreateBitmap(kTargetSize, kTargetSize, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)))
            {
                return false;
            }
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            return SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target));
        }

        // BGRA pixel at (x, y) as 0xAARRGGBB.
        uint32_t Pixel(UINT x, UINT y) const
        {
            WICRect rect { 0, 0, static_cast<INT>(kTargetSize), static_cast<INT>(kTargetSize) };
            ComPtr<IWICBitmapLock> lock;
            if (FAILED(bitmap->Lock(&rect, WICBitmapLockRead, &lock)))
            {
                return 0xDEADBEEF;
            }
            UINT stride = 0;
            UINT size = 0;
            BYTE* data = nullptr;
            lock->GetStride(&stride);
            lock->GetDataPointer(&size, &data);
            const BYTE* p = data + y * stride + x * 4u;
            return (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) |
                (static_cast<uint32_t>(p[1]) << 8) | static_cast<uint32_t>(p[0]);
        }

        // Prints a 16x16 map (every 4th pixel) for diagnosing placement failures.
        void PrintMap(const char* label) const;
    };

    constexpr uint32_t kBlack = 0xFF000000u;
    constexpr uint32_t kRed = 0xFFFF0000u;
    constexpr uint32_t kBlue = 0xFF0000FFu;

    // Classifies a pixel: '.' background, 'R' mostly red, 'B' mostly blue,
    // '?' anything else (filtering can blend texels along an edge).
    char Classify(uint32_t px)
    {
        const uint32_t r = (px >> 16) & 0xFF;
        const uint32_t b = px & 0xFF;
        if (px == kBlack)
        {
            return '.';
        }
        if (r >= 0xC0 && b < 0x40)
        {
            return 'R';
        }
        if (b >= 0xC0 && r < 0x40)
        {
            return 'B';
        }
        return '?';
    }

    void Canvas::PrintMap(const char* label) const
    {
        std::printf("  %s (every 4th pixel, 64x64):\n", label);
        for (UINT y = 0; y < kTargetSize; y += 4)
        {
            std::printf("    ");
            for (UINT x = 0; x < kTargetSize; x += 4)
            {
                std::printf("%c", Classify(Pixel(x, y)));
            }
            std::printf("\n");
        }
    }

    // 2x1 source: left texel red, right texel blue.
    ComPtr<ID2D1Bitmap> MakeRedBlueBitmap(ID2D1RenderTarget* target)
    {
        const uint32_t texels[2] = { kRed, kBlue };
        ComPtr<ID2D1Bitmap> bitmap;
        const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        (void)target->CreateBitmap(D2D1::SizeU(2, 1), texels, 8, &props, &bitmap);
        return bitmap;
    }

    // Renders a 32x32 Image at logical (0,0) with `quarters` rotation under
    // `incoming`, and reports the transform left on the target afterwards.
    bool RenderImage(Canvas& canvas, int quarters, const D2D1::Matrix3x2F& incoming, D2D1::Matrix3x2F& after)
    {
        auto image = std::make_shared<FD2D::Image>(L"image");
        image->SetBitmap(MakeRedBlueBitmap(canvas.target.Get()));
        FD2D::Image::DrawState state {};
        state.rotationQuarters = quarters;
        state.highQualitySampling = false; // nearest neighbor: crisp colors
        image->SetDrawState(state);
        image->Arrange(FD2D::Rect { 0.0f, 0.0f, 32.0f, 32.0f });

        canvas.target->BeginDraw();
        canvas.target->SetTransform(D2D1::Matrix3x2F::Identity());
        canvas.target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
        canvas.target->SetTransform(incoming);
        image->OnRender(canvas.target.Get());
        canvas.target->GetTransform(&after);
        return SUCCEEDED(canvas.target->EndDraw());
    }

    bool SameMatrix(const D2D1_MATRIX_3X2_F& a, const D2D1_MATRIX_3X2_F& b)
    {
        return a._11 == b._11 && a._12 == b._12 && a._21 == b._21 &&
            a._22 == b._22 && a._31 == b._31 && a._32 == b._32;
    }

    struct ComScope
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope()
        {
            if (SUCCEEDED(hr))
            {
                CoUninitialize();
            }
        }
    };
}

FD2D_TEST(ImageTransform_RotationKeepsParentTranslation)
{
    ComScope com;
    Canvas canvas;
    FD2D_REQUIRE(canvas.Create());

    // A parent placed the image at (32, 32), e.g. a scrolled or offset pane.
    const D2D1::Matrix3x2F incoming = D2D1::Matrix3x2F::Translation(32.0f, 32.0f);
    D2D1::Matrix3x2F after;
    FD2D_REQUIRE(RenderImage(canvas, 1, incoming, after));
    canvas.PrintMap("rotated 90, translated (32,32)");

    // The caller's transform is unchanged after OnRender returns.
    FD2D_CHECK(SameMatrix(after, incoming));

    // The rotated image lands in the bottom-right quadrant, not at the origin.
    FD2D_CHECK(Classify(canvas.Pixel(8, 8)) == '.');
    FD2D_CHECK(Classify(canvas.Pixel(48, 48)) != '.');

    // 90 degrees clockwise: the left (red) texel is now on top, the right
    // (blue) texel at the bottom of the image area.
    FD2D_CHECK(Classify(canvas.Pixel(48, 36)) == 'R');
    FD2D_CHECK(Classify(canvas.Pixel(48, 60)) == 'B');
}

FD2D_TEST(ImageTransform_RotationKeepsLiveResizeScale)
{
    ComScope com;
    Canvas canvas;
    FD2D_REQUIRE(canvas.Create());

    // Live resize: the backplate draws the logical 32x32 layout at 2x.
    const D2D1::Matrix3x2F incoming = D2D1::Matrix3x2F::Scale(2.0f, 2.0f);
    D2D1::Matrix3x2F after;
    FD2D_REQUIRE(RenderImage(canvas, 1, incoming, after));
    canvas.PrintMap("rotated 90, scaled 2x");

    FD2D_CHECK(SameMatrix(after, incoming));

    // Scaled to the full 64x64 target: well outside the unscaled 32x32 area.
    FD2D_CHECK(Classify(canvas.Pixel(32, 12)) == 'R');
    FD2D_CHECK(Classify(canvas.Pixel(32, 52)) == 'B');
    FD2D_CHECK(Classify(canvas.Pixel(32, 60)) == 'B');
    FD2D_CHECK(Classify(canvas.Pixel(8, 32)) == '.');   // outside the scaled image
}

FD2D_TEST(ImageTransform_UnrotatedImageLeavesTransformAlone)
{
    ComScope com;
    Canvas canvas;
    FD2D_REQUIRE(canvas.Create());

    const D2D1::Matrix3x2F incoming = D2D1::Matrix3x2F::Translation(32.0f, 32.0f);
    D2D1::Matrix3x2F after;
    FD2D_REQUIRE(RenderImage(canvas, 0, incoming, after));

    FD2D_CHECK(SameMatrix(after, incoming));
    // Unrotated: red on the left, blue on the right of the translated area.
    FD2D_CHECK(Classify(canvas.Pixel(36, 48)) == 'R');
    FD2D_CHECK(Classify(canvas.Pixel(60, 48)) == 'B');
    FD2D_CHECK(Classify(canvas.Pixel(8, 8)) == '.');
}

FD2D_TEST(ImageTransform_AllQuartersRestoreTransform)
{
    ComScope com;
    Canvas canvas;
    FD2D_REQUIRE(canvas.Create());

    const D2D1::Matrix3x2F incoming =
        D2D1::Matrix3x2F::Scale(1.5f, 1.5f) * D2D1::Matrix3x2F::Translation(5.0f, 7.0f);
    for (int quarters = 0; quarters < 4; ++quarters)
    {
        D2D1::Matrix3x2F after;
        FD2D_REQUIRE(RenderImage(canvas, quarters, incoming, after));
        FD2D_CHECK(SameMatrix(after, incoming));
    }
}

// Checkerboard helpers (Util::MapRectToPreRotationFrame and
// Util::ComputeCheckerboardTiles): the clip mapped into the image's
// pre-rotation frame, and the tiles of the issue's oracle grid that overlap it.

namespace
{
    bool NearF(float a, float b)
    {
        return std::fabs(a - b) < 1e-3f;
    }

    bool SameRect(const D2D1_RECT_F& a, const D2D1_RECT_F& b)
    {
        const bool same = NearF(a.left, b.left) && NearF(a.top, b.top) &&
            NearF(a.right, b.right) && NearF(a.bottom, b.bottom);
        if (!same)
        {
            std::printf("    rect (%.3f, %.3f, %.3f, %.3f), expected (%.3f, %.3f, %.3f, %.3f)\n",
                a.left, a.top, a.right, a.bottom, b.left, b.top, b.right, b.bottom);
        }
        return same;
    }

    // Bounds of `rect` after Image's rotation (Matrix3x2F::Rotation around the
    // layout center): mapping the pre-rotation rect forward must give the clip.
    D2D1_RECT_F RotateForward(const D2D1_RECT_F& rect, const D2D1_RECT_F& layout, int quarters)
    {
        const D2D1::Matrix3x2F rotation = D2D1::Matrix3x2F::Rotation(
            static_cast<float>(quarters) * 90.0f,
            D2D1::Point2F((layout.left + layout.right) * 0.5f, (layout.top + layout.bottom) * 0.5f));
        const D2D1_POINT_2F corners[4] = {
            rotation.TransformPoint(D2D1::Point2F(rect.left, rect.top)),
            rotation.TransformPoint(D2D1::Point2F(rect.right, rect.top)),
            rotation.TransformPoint(D2D1::Point2F(rect.left, rect.bottom)),
            rotation.TransformPoint(D2D1::Point2F(rect.right, rect.bottom)),
        };
        D2D1_RECT_F out { corners[0].x, corners[0].y, corners[0].x, corners[0].y };
        for (const auto& c : corners)
        {
            out.left = (std::min)(out.left, c.x);
            out.top = (std::min)(out.top, c.y);
            out.right = (std::max)(out.right, c.x);
            out.bottom = (std::max)(out.bottom, c.y);
        }
        return out;
    }

    // Brute force with the original per-tile loop: tile columns and rows from
    // the grid origin while the tile starts before destRect's right/bottom
    // edge, keeping those that overlap the visible rect.
    struct BruteTiles
    {
        int firstColumn { -1 };
        int endColumn { -1 };
        int firstRow { -1 };
        int endRow { -1 };
        long long count { 0 };
    };

    BruteTiles BruteForceTiles(const D2D1_RECT_F& dest, const D2D1_RECT_F& visible)
    {
        constexpr float tile = 8.0f;
        const float startX = std::floor(dest.left / tile) * tile;
        const float startY = std::floor(dest.top / tile) * tile;
        BruteTiles out;
        int j = 0;
        for (float y = startY; y < dest.bottom; y += tile, ++j)
        {
            int i = 0;
            for (float x = startX; x < dest.right; x += tile, ++i)
            {
                if (x + tile > visible.left && x < visible.right && y + tile > visible.top && y < visible.bottom)
                {
                    if (out.count == 0)
                    {
                        out.firstColumn = i;
                        out.firstRow = j;
                    }
                    out.firstColumn = (std::min)(out.firstColumn, i);
                    out.firstRow = (std::min)(out.firstRow, j);
                    out.endColumn = (std::max)(out.endColumn, i + 1);
                    out.endRow = (std::max)(out.endRow, j + 1);
                    ++out.count;
                }
            }
        }
        return out;
    }
}

FD2D_TEST(ImageTransform_ClipMapsIntoPreRotationFrame)
{
    // Non-square layout away from the origin: 100 x 60, center (60, 50).
    const D2D1_RECT_F layout = D2D1::RectF(10.0f, 20.0f, 110.0f, 80.0f);

    // The full clip: unchanged at 0 and 180 degrees, width and height swapped
    // around the center at 90 and 270 degrees.
    FD2D_CHECK(SameRect(FD2D::Util::MapRectToPreRotationFrame(layout, layout, 0), layout));
    FD2D_CHECK(SameRect(FD2D::Util::MapRectToPreRotationFrame(layout, layout, 2), layout));
    FD2D_CHECK(SameRect(FD2D::Util::MapRectToPreRotationFrame(layout, layout, 1), D2D1::RectF(30.0f, 0.0f, 90.0f, 100.0f)));
    FD2D_CHECK(SameRect(FD2D::Util::MapRectToPreRotationFrame(layout, layout, 3), D2D1::RectF(30.0f, 0.0f, 90.0f, 100.0f)));

    // An off-center clip (the layout's top-left quadrant) lands on a
    // different side for each rotation.
    const D2D1_RECT_F quadrant = D2D1::RectF(10.0f, 20.0f, 60.0f, 50.0f);
    FD2D_CHECK(SameRect(FD2D::Util::MapRectToPreRotationFrame(quadrant, layout, 1), D2D1::RectF(30.0f, 50.0f, 60.0f, 100.0f)));
    FD2D_CHECK(SameRect(FD2D::Util::MapRectToPreRotationFrame(quadrant, layout, 2), D2D1::RectF(60.0f, 50.0f, 110.0f, 80.0f)));
    FD2D_CHECK(SameRect(FD2D::Util::MapRectToPreRotationFrame(quadrant, layout, 3), D2D1::RectF(60.0f, 0.0f, 90.0f, 50.0f)));
    // Rotation quarters are taken modulo 4.
    FD2D_CHECK(SameRect(FD2D::Util::MapRectToPreRotationFrame(quadrant, layout, 5),
        FD2D::Util::MapRectToPreRotationFrame(quadrant, layout, 1)));
    FD2D_CHECK(SameRect(FD2D::Util::MapRectToPreRotationFrame(quadrant, layout, -1),
        FD2D::Util::MapRectToPreRotationFrame(quadrant, layout, 3)));

    // Image's own rotation maps every result back onto the clip.
    const D2D1_RECT_F clips[] = { layout, quadrant, D2D1::RectF(37.5f, 22.25f, 101.0f, 79.0f) };
    for (const auto& clip : clips)
    {
        for (int quarters = 0; quarters < 4; ++quarters)
        {
            const D2D1_RECT_F pre = FD2D::Util::MapRectToPreRotationFrame(clip, layout, quarters);
            FD2D_CHECK(SameRect(RotateForward(pre, layout, quarters), clip));
        }
    }
}

FD2D_TEST(ImageTransform_CheckerboardTilesOfNegativeUnalignedRect)
{
    // destRect.left/top negative and not multiples of 8: the grid origin is
    // floor(-21.5 / 8) * 8 = -24, floor(-13.25 / 8) * 8 = -16; 7 columns
    // (to 30) and 5 rows (to 18) overlap destRect.
    const D2D1_RECT_F dest = D2D1::RectF(-21.5f, -13.25f, 30.0f, 18.0f);

    // Everything visible: all 35 tiles, up to 8 px beyond destRect.
    const FD2D::Util::CheckerboardTiles all =
        FD2D::Util::ComputeCheckerboardTiles(dest, D2D1::RectF(-100.0f, -100.0f, 100.0f, 100.0f));
    FD2D_CHECK(all.originX == -24.0f);
    FD2D_CHECK(all.originY == -16.0f);
    FD2D_CHECK(all.firstColumn == 0 && all.endColumn == 7);
    FD2D_CHECK(all.firstRow == 0 && all.endRow == 5);
    FD2D_CHECK(SameRect(all.Bounds(), D2D1::RectF(-24.0f, -16.0f, 32.0f, 24.0f)));
    FD2D_CHECK(all.PatternOrigin().x == -24.0f && all.PatternOrigin().y == -16.0f);

    // A visible window inside: columns 2..4 ([-8, 16)), rows 2..4 ([0, 24)).
    const FD2D::Util::CheckerboardTiles part =
        FD2D::Util::ComputeCheckerboardTiles(dest, D2D1::RectF(-4.0f, 2.0f, 12.0f, 40.0f));
    FD2D_CHECK(part.firstColumn == 2 && part.endColumn == 5);
    FD2D_CHECK(part.firstRow == 2 && part.endRow == 5);
    FD2D_CHECK(SameRect(part.Bounds(), D2D1::RectF(-8.0f, 0.0f, 16.0f, 24.0f)));
    FD2D_CHECK(part.PatternOrigin().x == -8.0f && part.PatternOrigin().y == 0.0f);

    // An odd first column and row: the pattern anchor is the even tile before
    // them, so tile (3, 3) keeps its light color ((3 + 3) is even).
    const FD2D::Util::CheckerboardTiles odd =
        FD2D::Util::ComputeCheckerboardTiles(dest, D2D1::RectF(0.5f, 9.0f, 1.0f, 10.0f));
    FD2D_CHECK(odd.firstColumn == 3 && odd.endColumn == 4);
    FD2D_CHECK(odd.firstRow == 3 && odd.endRow == 4);
    FD2D_CHECK(odd.PatternOrigin().x == -8.0f && odd.PatternOrigin().y == 0.0f);

    // Nothing visible, or nothing to cover.
    FD2D_CHECK(FD2D::Util::ComputeCheckerboardTiles(dest, D2D1::RectF(40.0f, 0.0f, 50.0f, 10.0f)).Empty());
    FD2D_CHECK(FD2D::Util::ComputeCheckerboardTiles(dest, D2D1::RectF(-60.0f, 0.0f, -30.0f, 10.0f)).Empty());
    FD2D_CHECK(FD2D::Util::ComputeCheckerboardTiles(D2D1::RectF(5.0f, 5.0f, 5.0f, 9.0f), D2D1::RectF(0.0f, 0.0f, 50.0f, 50.0f)).Empty());
}

FD2D_TEST(ImageTransform_CheckerboardTilesFollowRotatedClip)
{
    // A non-square 96 x 64 layout with a 40 x 30 image, rotated 90 and 270
    // degrees, zoomed and panned: the tiles equal the original loop's tiles
    // restricted to the rotated-back clip.
    const D2D1_RECT_F layout = D2D1::RectF(16.0f, 16.0f, 112.0f, 80.0f);
    const D2D1_SIZE_F content = D2D1::SizeF(40.0f, 30.0f);
    struct ZoomPan
    {
        float zoom;
        float panX;
        float panY;
    };
    const ZoomPan cases[] = {
        { 1.0f, 0.0f, 0.0f },
        { 0.5f, 3.25f, -6.5f },
        { 3.7f, 13.3f, -7.6f },
        { 40.0f, -250.5f, 133.25f },
    };
    for (int quarters : { 1, 3 })
    {
        for (const auto& c : cases)
        {
            D2D1_RECT_F dest = FD2D::Util::ComputeAspectFitRect(layout, content, quarters);
            dest = FD2D::Util::ApplyZoomPanToRect(dest, c.zoom, c.panX, c.panY);
            const D2D1_RECT_F visible = FD2D::Util::MapRectToPreRotationFrame(layout, layout, quarters);
            // 90/270 degrees: the pre-rotation clip is 64 x 96 around (64, 48).
            FD2D_CHECK(SameRect(visible, D2D1::RectF(32.0f, 0.0f, 96.0f, 96.0f)));

            const FD2D::Util::CheckerboardTiles tiles = FD2D::Util::ComputeCheckerboardTiles(dest, visible);
            const BruteTiles brute = BruteForceTiles(dest, visible);
            std::printf("  q=%d zoom %.2f: dest (%.2f, %.2f, %.2f, %.2f) -> columns [%d, %d) rows [%d, %d), %lld visible tiles\n",
                quarters, c.zoom, dest.left, dest.top, dest.right, dest.bottom,
                tiles.firstColumn, tiles.endColumn, tiles.firstRow, tiles.endRow, brute.count);
            FD2D_CHECK(tiles.originX == std::floor(dest.left / 8.0f) * 8.0f);
            FD2D_CHECK(tiles.originY == std::floor(dest.top / 8.0f) * 8.0f);
            FD2D_REQUIRE(brute.count > 0);
            FD2D_CHECK(!tiles.Empty());
            FD2D_CHECK(tiles.firstColumn == brute.firstColumn && tiles.endColumn == brute.endColumn);
            FD2D_CHECK(tiles.firstRow == brute.firstRow && tiles.endRow == brute.endRow);
            // The visible tiles form a full rectangle of the grid.
            FD2D_CHECK(static_cast<long long>(tiles.endColumn - tiles.firstColumn) *
                (tiles.endRow - tiles.firstRow) == brute.count);
            // Bounded by the visible area, not by the zoomed destRect: at most
            // one partial tile on each side of the 64 x 96 clip.
            FD2D_CHECK(tiles.endColumn - tiles.firstColumn <= 64 / 8 + 1);
            FD2D_CHECK(tiles.endRow - tiles.firstRow <= 96 / 8 + 1);
        }
    }

    // Very high zoom (destRect millions of pixels wide, far negative origin):
    // still only the visible tiles, and the pattern anchor stays next to them.
    D2D1_RECT_F dest = FD2D::Util::ComputeAspectFitRect(layout, content, 1);
    dest = FD2D::Util::ApplyZoomPanToRect(dest, 50000.0f, 1234.5f, -987.25f);
    const D2D1_RECT_F visible = FD2D::Util::MapRectToPreRotationFrame(layout, layout, 1);
    const FD2D::Util::CheckerboardTiles tiles = FD2D::Util::ComputeCheckerboardTiles(dest, visible);
    std::printf("  q=1 zoom 50000: dest (%.1f, %.1f, %.1f, %.1f), origin (%.1f, %.1f) -> columns [%d, %d) rows [%d, %d)\n",
        dest.left, dest.top, dest.right, dest.bottom, tiles.originX, tiles.originY,
        tiles.firstColumn, tiles.endColumn, tiles.firstRow, tiles.endRow);
    FD2D_CHECK(dest.left < -1.0e6f && dest.right > 1.0e6f);
    FD2D_CHECK(!tiles.Empty());
    FD2D_CHECK(tiles.endColumn - tiles.firstColumn <= 64 / 8 + 1);
    FD2D_CHECK(tiles.endRow - tiles.firstRow <= 96 / 8 + 1);
    const D2D1_RECT_F bounds = tiles.Bounds();
    FD2D_CHECK(bounds.left <= visible.left && bounds.left > visible.left - 8.0f);
    FD2D_CHECK(bounds.top <= visible.top && bounds.top > visible.top - 8.0f);
    FD2D_CHECK(bounds.right >= visible.right && bounds.right < visible.right + 8.0f);
    FD2D_CHECK(bounds.bottom >= visible.bottom && bounds.bottom < visible.bottom + 8.0f);
    const D2D1_POINT_2F anchor = tiles.PatternOrigin();
    FD2D_CHECK(anchor.x <= bounds.left && anchor.x > bounds.left - 16.0f);
    FD2D_CHECK(anchor.y <= bounds.top && anchor.y > bounds.top - 16.0f);
    // Same phase as the grid origin: an even number of tiles away.
    FD2D_CHECK(std::fmod(static_cast<double>(anchor.x) - tiles.originX, 16.0) == 0.0);
    FD2D_CHECK(std::fmod(static_cast<double>(anchor.y) - tiles.originY, 16.0) == 0.0);
}
