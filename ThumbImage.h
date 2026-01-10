#pragma once

#include "Wnd.h"
#include "../ImageCore/ImageRequest.h"
#include "../ImageCore/ImageLoader.h"
#include "../external/DirectXTex/DirectXTex/DirectXTex.h"
#include "SelectionStyle.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace FD2D
{
    class Spinner;

    // Thumbnail image control:
    // - Thumbnail decode (targetSize)
    // - No zoom/pan, no D3D SRV path
    // - Selection overlay + click
    class ThumbImage : public Wnd
    {
    public:
        using ClickHandler = std::function<void()>;

        ThumbImage();
        explicit ThumbImage(const std::wstring& name);
        ~ThumbImage();

        Size Measure(Size available) override;
        void SetThumbnailSize(const Size& size);
        HRESULT SetSourceFile(const std::wstring& filePath);

        void SetSelected(bool selected);
        bool Selected() const { return m_selected; }
        void SetSelectionStyle(const SelectionStyle& style);

        void SetOnClick(ClickHandler handler);
        void SetLoadingSpinnerEnabled(bool enabled);

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
        std::wstring m_loadedFilePath {};
        ImageCore::ImageRequest m_request {};
        ImageCore::ImageHandle m_currentHandle { 0 };
        std::atomic<bool> m_loading { false };
        std::atomic<unsigned long long> m_requestToken { 0 };
        std::atomic<unsigned long long> m_inflightToken { 0 };
        std::wstring m_failedFilePath {};
        HRESULT m_failedHr { S_OK };

        Microsoft::WRL::ComPtr<ID2D1Bitmap> m_bitmap {};
        Microsoft::WRL::ComPtr<ID2D1Bitmap> m_prevBitmap {};
        std::wstring m_prevLoadedFilePath {};
        unsigned long long m_fadeStartMs { 0 };
        unsigned long long m_fadeDurationMs { 180 };

        mutable std::mutex m_pendingMutex;
        Microsoft::WRL::ComPtr<IWICBitmapSource> m_pendingWicBitmap {};
        std::unique_ptr<DirectX::ScratchImage> m_pendingScratchImage {};
        std::wstring m_pendingSourcePath {};

        bool m_selected { false };
        SelectionStyle m_selectionStyle {};
        unsigned long long m_selectionAnimStartMs { 0 };
        unsigned long long m_selectionAnimMs { 150 };
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_selectionBrush {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_selectionShadowBrush {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_selectionFillBrush {};

        ClickHandler m_onClick {};

        bool m_loadingSpinnerEnabled { true };
        std::shared_ptr<Spinner> m_loadingSpinner {};
    };
}

