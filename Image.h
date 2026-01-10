#pragma once

#include "Wnd.h"
#include "../ImageCore/ImageRequest.h"
#include "../ImageCore/ImageLoader.h"
#include "../external/DirectXTex/DirectXTex/DirectXTex.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <d3d11_1.h>
#include <d2d1_1.h>  // For Direct2D 1.1+ interpolation modes
#include <functional>
#include <mutex>
#include <atomic>

namespace FD2D
{
    class Spinner;

    class Image : public Wnd
    {
    public:
        using ClickHandler = std::function<void()>;

        struct SelectionStyle
        {
            D2D1_COLOR_F accent { 1.0f, 0.60f, 0.24f, 1.0f };
            D2D1_COLOR_F shadow { 0.0f, 0.0f, 0.0f, 0.55f };
            D2D1_COLOR_F fill { 1.0f, 1.0f, 1.0f, 1.0f };

            float radius { 6.0f };
            float baseInflate { 1.0f };
            float popInflate { 4.0f };
            float shadowThickness { 3.0f };
            float accentThickness { 2.0f };
            float fillMaxAlpha { 0.10f };

            bool breatheEnabled { true };
            int breathePeriodMs { 1800 };
            float breatheInflateAmp { 0.60f };
            float breatheThicknessAmp { 0.35f };
            float breatheAlphaAmp { 0.08f };
        };

        Image();
        explicit Image(const std::wstring& name);
        ~Image();

        Size Measure(Size available) override;
        void SetRect(const D2D1_RECT_F& rect);
        HRESULT SetSourceFile(const std::wstring& filePath);
        
        // 썸네일 로딩
        void SetThumbnailSize(const Size& size);
        
        // 로딩 목적 설정
        void SetImagePurpose(ImageCore::ImagePurpose purpose);

        void SetSelected(bool selected);
        bool Selected() const { return m_selected; }

        void SetSelectionStyle(const SelectionStyle& style);

        void SetOnClick(ClickHandler handler);

        void SetLoadingSpinnerEnabled(bool enabled);
        bool LoadingSpinnerEnabled() const { return m_loadingSpinnerEnabled; }
        std::shared_ptr<Spinner> LoadingSpinner() const { return m_loadingSpinner; }

        // Zoom control (for main image only)
        void SetZoomScale(float scale);
        float ZoomScale() const { return m_zoomScale; }
        void ResetZoom();
        void SetZoomSpeed(float speed); // Set zoom interpolation speed (0.0-1.0, higher = faster)
        float ZoomSpeed() const { return m_zoomSpeed; }
        void AdvanceZoomAnimation(unsigned long long nowMs);

        void OnRenderD3D(ID3D11DeviceContext* context) override;
        void OnRender(ID2D1RenderTarget* target) override;
        bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

    private:
        void RequestImageLoad();
        void OnImageLoaded(
            const std::wstring& sourcePath,
            HRESULT hr,
            Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap,
            std::unique_ptr<DirectX::ScratchImage> scratchImage);
        HRESULT ConvertToD2DBitmap(
            ID2D1RenderTarget* target,
            const std::wstring& sourcePath,
            Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap,
            std::unique_ptr<DirectX::ScratchImage> scratchImage);

        std::wstring m_filePath {};
        // The source path currently represented by m_bitmap (can lag behind m_filePath while loading the next image).
        std::wstring m_loadedFilePath {};
        ImageCore::ImageRequest m_request {};
        ImageCore::ImageHandle m_currentHandle { 0 };
        std::atomic<bool> m_loading { false };
        std::atomic<unsigned long long> m_requestToken { 0 };
        std::atomic<unsigned long long> m_inflightToken { 0 };
        std::atomic<bool> m_forceCpuDecode { false };
        std::wstring m_failedFilePath {};
        HRESULT m_failedHr { S_OK };

        Microsoft::WRL::ComPtr<ID2D1Bitmap> m_bitmap {};
        Microsoft::WRL::ComPtr<ID2D1Bitmap> m_prevBitmap {};
        std::wstring m_prevLoadedFilePath {};
        unsigned long long m_fadeStartMs { 0 };
        unsigned long long m_fadeDurationMs { 200 }; // cross-fade duration

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_backdropBrush {};
        
        // 변환 대기 중인 이미지 데이터 (render thread에서 변환)
        mutable std::mutex m_pendingMutex;
        Microsoft::WRL::ComPtr<IWICBitmapSource> m_pendingWicBitmap {};
        std::unique_ptr<DirectX::ScratchImage> m_pendingScratchImage {};
        std::wstring m_pendingSourcePath {};

        bool m_selected { false };
        ClickHandler m_onClick {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_selectionBrush {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_selectionShadowBrush {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_selectionFillBrush {};
        SelectionStyle m_selectionStyle {};
        unsigned long long m_selectionAnimStartMs { 0 };
        unsigned long long m_selectionAnimMs { 150 }; // selection "pop" duration
        bool m_loadingSpinnerEnabled { true };
        std::shared_ptr<Spinner> m_loadingSpinner {};

        // Optional GPU-native DDS path (main image): upload ScratchImage to a D3D11 SRV and render via D3D.
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_gpuSrv {};
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_prevGpuSrv {};
        UINT m_gpuWidth { 0 };
        UINT m_gpuHeight { 0 };

        // Zoom state (for main image only)
        float m_zoomScale { 1.0f };
        float m_targetZoomScale { 1.0f };
        unsigned long long m_lastZoomAnimMs { 0 };
        float m_zoomSpeed { 100.0f }; // zoom interpolation speed (fraction of remaining distance per second, e.g., 10.0 = 10x per second)
        
        // Panning state (for main image only)
        float m_panX { 0.0f };  // Pan offset in layout coordinates
        float m_panY { 0.0f };
        bool m_panning { false };
        float m_panStartX { 0.0f };  // Mouse position when panning started
        float m_panStartY { 0.0f };
        float m_panStartOffsetX { 0.0f };  // Pan offset when panning started
        float m_panStartOffsetY { 0.0f };
    };
}

