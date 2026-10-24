#include "TestHarness.h"

#include "Image.h"

#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

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
