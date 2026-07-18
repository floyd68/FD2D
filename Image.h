#pragma once

#include "Wnd.h"
#include <wrl/client.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11_1.h>
#include <string>

namespace FD2D
{
    // Handle-only image control: owns a D2D bitmap XOR a D3D Texture2D SRV and
    // renders aspect-fit + zoom/pan/rotation with optional alpha checkerboard.
    // Does not load files or manage async pipelines (those belong to the app).
    class Image : public Wnd
    {
    public:
        struct DrawState
        {
            float zoomScale { 1.0f };
            float panX { 0.0f };
            float panY { 0.0f };
            int rotationQuarters { 0 }; // 0/1/2/3
            bool highQualitySampling { true };
            bool alphaCheckerboardEnabled { false };
            // Channel isolation for the D3D (texture SRV) path: 0=RGBA (normal),
            // 1=R, 2=G, 3=B, 4=A - shown as grayscale. Ignored on the D2D
            // (bitmap) path.
            int channelMode { 0 };
            // True when the SRV holds premultiplied-alpha color (CPU BGRA8 from
            // WIC/DirectXTex); then a color-channel (R/G/B) isolation divides by
            // alpha so it reads the STRAIGHT channel value - matching straight-
            // alpha BCn textures. No effect on normal RGBA display (mode 0), which
            // must stay premultiplied to composite correctly over the checker.
            bool sourcePremultiplied { false };
        };

        Image();
        explicit Image(const std::wstring& name);

        void SetBitmap(Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap);
        // Texture2D SRV only; discover size via GetResource/GetDesc/SRV mip. Clears bitmap when set.
        void SetShaderResource(Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv);
        void Clear();
        void SetDrawState(const DrawState& state);
        DrawState GetDrawState() const;
        D2D1_SIZE_U ContentPixelSize() const; // texel size; 0,0 if empty

        void OnRender(ID2D1RenderTarget* target) override;
        void OnRenderD3D(ID3D11DeviceContext* context) override;
        void OnGraphicsInvalidated(GraphicsInvalidationReason reason, const GraphicsGeneration& generation) override;

    private:
        bool TryGetContentSize(D2D1_SIZE_F& outSize) const;
        void ResetCheckerBrushes();

        // Strong refs to current content; D2D and D3D mutually exclusive.
        Microsoft::WRL::ComPtr<ID2D1Bitmap> m_bitmap {};
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv {};
        UINT m_srvWidth { 0 };
        UINT m_srvHeight { 0 };
        DrawState m_drawState {};

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_checkerLightBrush {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_checkerDarkBrush {};
    };
}
