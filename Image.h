#pragma once

#include "Wnd.h"
#include "../ImageCore/ImageRequest.h"
#include "../ImageCore/ImageLoader.h"
#include <wrl/client.h>
#include <d3d11_1.h>
#include <d2d1_1.h>  // For Direct2D 1.1+ interpolation modes
#include <functional>
#include <mutex>
#include <atomic>
#include <memory>
#include <vector>

namespace FD2D
{
    class Spinner;

    class Image : public Wnd
    {
    public:
        using ClickHandler = std::function<void()>;
        struct LoadedInfo
        {
            uint32_t width { 0 };
            uint32_t height { 0 };
            DXGI_FORMAT format { DXGI_FORMAT_UNKNOWN };
            std::wstring sourcePath {};
        };
        struct ViewTransform
        {
            float zoomScale { 1.0f };
            float targetZoomScale { 1.0f };
            float zoomVelocity { 0.0f };
            float panX { 0.0f };
            float panY { 0.0f };
        };
        using ViewChangedHandler = std::function<void(const ViewTransform&)>;

        Image();
        explicit Image(const std::wstring& name);
        ~Image();

        Size Measure(Size available) override;
        void SetRect(const D2D1_RECT_F& rect);
        HRESULT SetSourceFile(const std::wstring& filePath);
        
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
        void SetZoomStiffness(float stiffness); // Set zoom spring stiffness for critically damped animation (higher = faster response)
        float ZoomStiffness() const { return m_zoomStiffness; }
        void AdvanceZoomAnimation(unsigned long long nowMs);

        // Snapshot of the currently displayed (loaded) image metadata (UI thread).
        LoadedInfo GetLoadedInfo() const;

        // Clear any displayed image and pending loads.
        void ClearSource();

        // Enable/disable zoom and pan interactions (click can still be handled).
        void SetInteractionEnabled(bool enabled);
        bool InteractionEnabled() const { return m_interactionEnabled; }

        // View transform (zoom/pan) for sync scenarios (compare mode).
        ViewTransform GetViewTransform() const;
        void SetViewTransform(const ViewTransform& vt, bool notify = true);
        void SetOnViewChanged(ViewChangedHandler handler);

        // Alpha visualization:
        // - When enabled, draws a checkerboard backdrop in the image destination rect so transparent pixels
        //   show a standard checker pattern.
        // - When disabled, keeps the existing solid backdrop behavior.
        void SetAlphaCheckerboardEnabled(bool enabled);
        bool AlphaCheckerboardEnabled() const { return m_alphaCheckerboardEnabled; }

        // Backdrop color behind the image (letterbox + behind transparent pixels).
        // This is separate from Backplate::ClearColor so containers (e.g., ImageBrowser)
        // can control their own background and focused background colors.
        void SetBackdropColor(const D2D1_COLOR_F& color);
        D2D1_COLOR_F BackdropColor() const { return m_backdropColorOverride; }

        void OnRenderD3D(ID3D11DeviceContext* context) override;
        void OnRender(ID2D1RenderTarget* target) override;
        bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

    private:
        void RequestImageLoad();
        void OnImageLoaded(
            const std::wstring& sourcePath,
            HRESULT hr,
            ImageCore::DecodedImage image);
        bool TryGetBitmapSize(D2D1_SIZE_F& outSize) const;
        bool TryComputeAspectFitBaseRect(const D2D1_RECT_F& layoutRect, const D2D1_SIZE_F& bitmapSize, D2D1_RECT_F& outRect) const;
        void ClampPanToVisible();

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

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_backdropBrush {};
        D2D1_COLOR_F m_backdropColor { 0.0f, 0.0f, 0.0f, 0.0f };
        bool m_backdropColorValid { false };
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_checkerLightBrush {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_checkerDarkBrush {};
        bool m_alphaCheckerboardEnabled { false };
        bool m_interactionEnabled { true };

        D2D1_COLOR_F m_backdropColorOverride { 0.09f, 0.09f, 0.10f, 1.0f };
        bool m_backdropColorOverrideValid { false };

        // Pending decoded payload produced on a worker thread.
        // Consumed on the render/UI thread to create D2D bitmaps / upload to D3D resources.
        mutable std::mutex m_pendingMutex;
        std::shared_ptr<std::vector<uint8_t>> m_pendingBlocks {};
        uint32_t m_pendingW { 0 };
        uint32_t m_pendingH { 0 };
        uint32_t m_pendingRowPitch { 0 };
        DXGI_FORMAT m_pendingFormat { DXGI_FORMAT_UNKNOWN };
        std::wstring m_pendingSourcePath {};

        ClickHandler m_onClick {};
        bool m_loadingSpinnerEnabled { true };
        std::shared_ptr<Spinner> m_loadingSpinner {};

        // Optional GPU-native DDS path (main image): upload BCn blocks to a D3D11 SRV and render via D3D.
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_gpuSrv {};
        UINT m_gpuWidth { 0 };
        UINT m_gpuHeight { 0 };

        // Loaded metadata for the currently displayed image (GPU or CPU path).
        uint32_t m_loadedW { 0 };
        uint32_t m_loadedH { 0 };
        DXGI_FORMAT m_loadedFormat { DXGI_FORMAT_UNKNOWN };

        // Zoom state (for main image only)
        float m_zoomScale { 1.0f };
        float m_targetZoomScale { 1.0f };
        float m_zoomVelocity { 0.0f }; // Velocity for critically damped spring animation
        unsigned long long m_lastZoomAnimMs { 0 };
        float m_zoomSpeed { 100.0f }; // zoom interpolation speed (fraction of remaining distance per second, e.g., 10.0 = 10x per second)
        float m_zoomStiffness { 100.0f }; // Spring stiffness for critically damped animation (higher = faster response)
        
        // Panning state (for main image only)
        float m_panX { 0.0f };  // Pan offset in layout coordinates
        float m_panY { 0.0f };
        bool m_panning { false };
        bool m_panArmed { false }; // LButton is down; may become panning after threshold
        float m_panStartX { 0.0f };  // Mouse position when panning started
        float m_panStartY { 0.0f };
        float m_panStartOffsetX { 0.0f };  // Pan offset when panning started
        float m_panStartOffsetY { 0.0f };

        // Pointer-based zoom state (keep mouse position fixed while zoom animates)
        bool m_pointerZoomActive { false };
        float m_pointerZoomStartZoom { 1.0f };
        float m_pointerZoomStartPanX { 0.0f };
        float m_pointerZoomStartPanY { 0.0f };
        float m_pointerZoomMouseX { 0.0f }; // in layout/client coordinates
        float m_pointerZoomMouseY { 0.0f };

        ViewChangedHandler m_onViewChanged {};
        bool m_suppressViewNotify { false };
    };
}

