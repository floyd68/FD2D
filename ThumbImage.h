#pragma once

#include "Wnd.h"
#include "AsyncImagePipeline.h"
#include "../ImageCore/ImageRequest.h"
#include "../ImageCore/ImageLoader.h"
#include "SelectionStyle.h"

#include <wrl/client.h>

#include <functional>
#include <memory>
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

        // Get the loaded bitmap dimensions (returns 0,0 if not yet loaded)
        D2D1_SIZE_F GetBitmapSize() const;

        void OnRender(ID2D1RenderTarget* target) override;
        bool OnInputEvent(const InputEvent& event) override;

    private:
        void RequestImageLoad();

        std::wstring m_filePath {};
        std::wstring m_loadedFilePath {};
        ImageCore::ImageRequest m_request {};

        // Shared async load pipeline (tokens, pending payload, failure state).
        AsyncImagePipeline m_pipeline { &m_backplate };

        Microsoft::WRL::ComPtr<ID2D1Bitmap> m_bitmap {};

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

