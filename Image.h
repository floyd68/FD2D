#pragma once

#include "Wnd.h"
#include "../ImageCore/ImageRequest.h"
#include "../ImageCore/ImageLoader.h"
#include "../external/DirectXTex/DirectXTex/DirectXTex.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <mutex>

namespace FD2D
{
    class Image : public Wnd
    {
    public:
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

        void OnRender(ID2D1RenderTarget* target) override;

    private:
        void RequestImageLoad();
        void OnImageLoaded(
            HRESULT hr,
            Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap,
            std::unique_ptr<DirectX::ScratchImage> scratchImage);
        HRESULT ConvertToD2DBitmap(
            ID2D1RenderTarget* target,
            Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap,
            std::unique_ptr<DirectX::ScratchImage> scratchImage);

        std::wstring m_filePath {};
        ImageCore::ImageRequest m_request {};
        ImageCore::ImageHandle m_currentHandle { 0 };
        bool m_loading { false };

        Microsoft::WRL::ComPtr<ID2D1Bitmap> m_bitmap {};
        
        // 변환 대기 중인 이미지 데이터 (render thread에서 변환)
        mutable std::mutex m_pendingMutex;
        Microsoft::WRL::ComPtr<IWICBitmapSource> m_pendingWicBitmap {};
        std::unique_ptr<DirectX::ScratchImage> m_pendingScratchImage {};
    };
}

