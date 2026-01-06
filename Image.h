#pragma once

#include "Wnd.h"
#include "../ImageCore/ImageRequest.h"
#include "../ImageCore/ImageLoader.h"
#include "../external/DirectXTex/DirectXTex/DirectXTex.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <d3d11_1.h>
#include <functional>
#include <mutex>

namespace FD2D
{
    class Spinner;

    class Image : public Wnd
    {
    public:
        using ClickHandler = std::function<void()>;

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

        void SetOnClick(ClickHandler handler);

        void SetLoadingSpinnerEnabled(bool enabled);
        bool LoadingSpinnerEnabled() const { return m_loadingSpinnerEnabled; }
        std::shared_ptr<Spinner> LoadingSpinner() const { return m_loadingSpinner; }

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
        bool m_loading { false };

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
        bool m_loadingSpinnerEnabled { true };
        std::shared_ptr<Spinner> m_loadingSpinner {};

        // Optional GPU-native DDS path (main image): upload ScratchImage to a D3D11 SRV and render via D3D.
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_gpuSrv {};
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_prevGpuSrv {};
        UINT m_gpuWidth { 0 };
        UINT m_gpuHeight { 0 };
    };
}

