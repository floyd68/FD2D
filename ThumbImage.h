#pragma once

#include "Wnd.h"
#include "../ImageCore/ImageRequest.h"
#include "../ImageCore/ImageLoader.h"
#include "SelectionStyle.h"

#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
            ImageCore::DecodedImage image);

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

        mutable std::mutex m_pendingMutex;
        // Pending decoded image produced on the worker thread.
        std::shared_ptr<std::vector<uint8_t>> m_pendingBlocks {};
        uint32_t m_pendingW { 0 };
        uint32_t m_pendingH { 0 };
        uint32_t m_pendingRowPitch { 0 };
        DXGI_FORMAT m_pendingFormat { DXGI_FORMAT_UNKNOWN };
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

