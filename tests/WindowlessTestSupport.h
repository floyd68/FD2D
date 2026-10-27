#pragma once

// Helpers for FD2D tests that render through Backplate's windowless mode
// (Backplate::InitializeWindowless). Deliberately does not define
// FD2D::BackplateTestAccess: FD2D's tests share BackplateTestAccess.h, and
// each other test executable defines its own.

#include "Backplate.h"
#include "Core.h"
#include "Wnd.h"

#include <windows.h>
#include <d2d1.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace FD2DWindowlessTest
{
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

    // The backplate's D2D device comes from Core's factory.
    struct CoreScope
    {
        HRESULT hr = S_FALSE;
        CoreScope()
        {
            FD2D::InitContext context {};
            context.instance = GetModuleHandleW(nullptr);
            hr = FD2D::Core::Initialize(context);
        }
        ~CoreScope()
        {
            if (hr == S_OK)
            {
                FD2D::Core::Shutdown();
            }
        }
    };

    struct Bgra
    {
        std::uint8_t b { 0 };
        std::uint8_t g { 0 };
        std::uint8_t r { 0 };
        std::uint8_t a { 0 };
    };

    // A frame read with Backplate::ReadComposedPixels.
    struct Frame
    {
        std::vector<std::uint8_t> pixels;
        UINT width { 0 };
        UINT height { 0 };
        UINT stride { 0 };

        HRESULT Read(FD2D::Backplate& backplate, const D2D1_RECT_F& logicalRect)
        {
            return backplate.ReadComposedPixels(logicalRect, pixels, width, height, stride);
        }

        Bgra At(UINT x, UINT y) const
        {
            if (x >= width || y >= height)
            {
                return Bgra { 0xEE, 0xEE, 0xEE, 0x00 };
            }
            const std::uint8_t* p = pixels.data() + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u;
            return Bgra { p[0], p[1], p[2], p[3] };
        }

        // Exact expected BGRA within +/-1 per channel.
        bool Near(UINT x, UINT y, const Bgra& expected) const
        {
            const Bgra actual = At(x, y);
            const auto close = [](std::uint8_t a, std::uint8_t b) { return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= 1; };
            const bool ok = close(actual.b, expected.b) && close(actual.g, expected.g) &&
                close(actual.r, expected.r) && close(actual.a, expected.a);
            if (!ok)
            {
                std::printf("    pixel (%u, %u): BGRA %3u %3u %3u %3u, expected %3u %3u %3u %3u\n",
                    x, y, actual.b, actual.g, actual.r, actual.a, expected.b, expected.g, expected.r, expected.a);
            }
            return ok;
        }

        // Prints every 4th pixel of row y as BGRA.
        void Print(const char* label, UINT y) const
        {
            std::printf("%s y=%u:", label, y);
            for (UINT x = 0; x < width; x += 4)
            {
                const Bgra p = At(x, y);
                std::printf(" %02X%02X%02X%02X", p.b, p.g, p.r, p.a);
            }
            std::printf("\n");
        }
    };

    // D3D-pass child: clears a fixed logical rect with Backplate::ClearRectD3D.
    class ClearRectD3DWnd : public FD2D::Wnd
    {
    public:
        ClearRectD3DWnd(const std::wstring& name, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color)
            : FD2D::Wnd(name)
            , m_rect(rect)
            , m_color(color)
        {
        }

        void SetColor(const D2D1_COLOR_F& color) { m_color = color; }
        void SetOnRender(std::function<void()> onRender) { m_onRender = std::move(onRender); }
        int Frames() const { return m_frames; }
        bool LastClearSucceeded() const { return m_lastClearSucceeded; }

        void OnRenderD3D(ID3D11DeviceContext* context) override
        {
            ++m_frames;
            if (m_onRender)
            {
                m_onRender();
            }
            FD2D::Backplate* backplate = BackplateRef();
            m_lastClearSucceeded = backplate != nullptr && context != nullptr &&
                backplate->ActiveD3DRenderTarget() != nullptr &&
                backplate->ClearRectD3D(m_rect, m_color);
        }

    private:
        D2D1_RECT_F m_rect {};
        D2D1_COLOR_F m_color {};
        std::function<void()> m_onRender {};
        int m_frames { 0 };
        bool m_lastClearSucceeded { false };
    };

    // D2D-pass child: fills a fixed logical rect with a brush cached per
    // target generation (a stale brush would fail EndDraw on a new device).
    class FillRectD2DWnd : public FD2D::Wnd
    {
    public:
        FillRectD2DWnd(const std::wstring& name, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color)
            : FD2D::Wnd(name)
            , m_rect(rect)
            , m_color(color)
        {
        }

        void SetColor(const D2D1_COLOR_F& color) { m_color = color; }
        void SetOnRender(std::function<void()> onRender) { m_onRender = std::move(onRender); }
        int Frames() const { return m_frames; }
        int BrushesCreated() const { return m_brushesCreated; }

        void OnRender(ID2D1RenderTarget* target) override
        {
            ++m_frames;
            if (m_onRender)
            {
                m_onRender();
            }
            if (target == nullptr)
            {
                return;
            }
            if (RefreshTargetResourceStamp(m_stamp) || !m_brush)
            {
                m_brush.Reset();
                if (SUCCEEDED(target->CreateSolidColorBrush(m_color, &m_brush)))
                {
                    ++m_brushesCreated;
                }
            }
            if (m_brush)
            {
                m_brush->SetColor(m_color);
                target->FillRectangle(m_rect, m_brush.Get());
            }
        }

    private:
        D2D1_RECT_F m_rect {};
        D2D1_COLOR_F m_color {};
        std::function<void()> m_onRender {};
        FD2D::GraphicsResourceStamp m_stamp {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush {};
        int m_frames { 0 };
        int m_brushesCreated { 0 };
    };
}
