#include "Image.h"
#include "Backplate.h"

namespace FD2D
{
    Image::Image()
        : Wnd()
        , m_request()
    {
    }

    Image::Image(const std::wstring& name)
        : Wnd(name)
        , m_request()
    {
    }

    Image::~Image()
    {
        if (m_currentHandle != 0)
        {
            ImageCore::ImageLoader::Instance().Cancel(m_currentHandle);
        }
    }

    void Image::SetRect(const D2D1_RECT_F& rect)
    {
        SetLayoutRect(rect);
    }

    Size Image::Measure(Size available)
    {
        // Thumbnail/Preview 용도에서는 targetSize를 fixed desired size로 사용 (thumb strip 레이아웃을 위해)
        if ((m_request.purpose == ImageCore::ImagePurpose::Thumbnail || m_request.purpose == ImageCore::ImagePurpose::Preview) &&
            (m_request.targetSize.w > 0.0f && m_request.targetSize.h > 0.0f))
        {
            // StackPanel(Horizontal)은 childRect의 height를 childArea.h(=윈도우 높이)에 맞추기 때문에,
            // height가 줄어들면 Image::OnRender의 aspect-fit 로직이 셀 내부 여백을 크게 만들 수 있다.
            // 썸네일 모드에서는 available에 맞춰 셀 자체를 줄여(정사각형) "간격이 벌어져 보이는" 현상을 완화한다.
            float size = m_request.targetSize.h;
            if (m_request.targetSize.w > 0.0f)
            {
                size = (std::min)(size, m_request.targetSize.w);
            }
            if (available.w > 0.0f)
            {
                size = (std::min)(size, available.w);
            }
            if (available.h > 0.0f)
            {
                size = (std::min)(size, available.h);
            }

            m_desired = { size, size };
            return m_desired;
        }

        // FullResolution은 윈도우 크기에 맞게 항상 available size 사용 (Aspect Ratio는 OnRender에서 처리)
        if (available.w > 0.0f && available.h > 0.0f)
        {
            m_desired = available;
        }
        else
        {
            // 기본 크기
            m_desired = { 800.0f, 600.0f };
        }
        return m_desired;
    }

    HRESULT Image::SetSourceFile(const std::wstring& filePath)
    {
        if (m_currentHandle != 0)
        {
            ImageCore::ImageLoader::Instance().Cancel(m_currentHandle);
            m_currentHandle = 0;
        }

        m_filePath = filePath;
        m_bitmap.Reset();
        m_loading = false;

        // Request 업데이트
        m_request.source = filePath;
        
        return S_OK;
    }

    void Image::SetThumbnailSize(const Size& size)
    {
        ImageCore::Size targetSize { size.w, size.h };
        m_request.targetSize = targetSize;
        if (size.w > 0.0f || size.h > 0.0f)
        {
            m_request.purpose = ImageCore::ImagePurpose::Thumbnail;
        }
    }

    void Image::SetImagePurpose(ImageCore::ImagePurpose purpose)
    {
        m_request.purpose = purpose;
    }

    void Image::RequestImageLoad()
    {
        if (m_filePath.empty() || m_loading)
        {
            return;
        }

        // 이미 로드된 경우 스킵
        if (m_bitmap)
        {
            return;
        }

        m_loading = true;
        m_request.source = m_filePath;

        m_currentHandle = ImageCore::ImageLoader::Instance().Request(
            m_request,
            [this](HRESULT hr, Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap, std::unique_ptr<DirectX::ScratchImage> scratchImage)
            {
                OnImageLoaded(hr, wicBitmap, std::move(scratchImage));
            });
    }

    void Image::OnImageLoaded(
        HRESULT hr,
        Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap,
        std::unique_ptr<DirectX::ScratchImage> scratchImage)
    {
        m_loading = false;
        m_currentHandle = 0;

        // 변환은 OnRender에서 render target을 사용하여 수행
        // 여기서는 저장만 하고 Invalidate로 OnRender 호출 유도
        if (SUCCEEDED(hr) && (wicBitmap || scratchImage))
        {
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                m_pendingWicBitmap = wicBitmap;
                m_pendingScratchImage = std::move(scratchImage);
            }
            
            // worker thread에서 UI thread로 명확히 redraw 요청
            if (m_backplate)
            {
                HWND hwnd = m_backplate->Window();
                if (hwnd)
                {
                    PostMessage(hwnd, Backplate::WM_FD2D_REQUEST_REDRAW, 0, 0);
                }
            }
        }
    }

    HRESULT Image::ConvertToD2DBitmap(
        ID2D1RenderTarget* target,
        Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap,
        std::unique_ptr<DirectX::ScratchImage> scratchImage)
    {
        if (target == nullptr)
        {
            return E_INVALIDARG;
        }

        Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dBitmap;
        HRESULT hr = E_FAIL;

        // DirectXTex 경로: ScratchImage를 직접 사용
        if (scratchImage)
        {
            const DirectX::Image* image = scratchImage->GetImage(0, 0, 0);
            if (image && image->pixels)
            {
                D2D1_BITMAP_PROPERTIES props = {};
                props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
                props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
                props.dpiX = 96.0f;
                props.dpiY = 96.0f;

                D2D1_SIZE_U size = D2D1::SizeU(
                    static_cast<UINT32>(image->width),
                    static_cast<UINT32>(image->height));

                hr = target->CreateBitmap(
                    size,
                    image->pixels,
                    static_cast<UINT32>(image->rowPitch),
                    &props,
                    &d2dBitmap);
            }
        }
        // WIC 경로: WIC bitmap을 사용
        else if (wicBitmap)
        {
            hr = target->CreateBitmapFromWicBitmap(wicBitmap.Get(), nullptr, &d2dBitmap);
        }

        if (SUCCEEDED(hr) && d2dBitmap)
        {
            m_bitmap = d2dBitmap;
        }

        return hr;
    }

    void Image::OnRender(ID2D1RenderTarget* target)
    {
        if (target == nullptr)
        {
            return;
        }

        // 변환 대기 중인 이미지가 있으면 D2D1Bitmap으로 변환
        Microsoft::WRL::ComPtr<IWICBitmapSource> pendingWic;
        std::unique_ptr<DirectX::ScratchImage> pendingScratch;
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            pendingWic = m_pendingWicBitmap;
            pendingScratch = std::move(m_pendingScratchImage);
            m_pendingWicBitmap.Reset();
        }

        if (!m_bitmap && (pendingWic || pendingScratch))
        {
            (void)ConvertToD2DBitmap(target, pendingWic, std::move(pendingScratch));
        }

        // 이미지가 없으면 로딩 요청
        if (!m_bitmap && !m_loading && !m_filePath.empty())
        {
            RequestImageLoad();
        }

        if (m_bitmap)
        {
            // Aspect Ratio 유지하면서 렌더링
            auto bitmapSize = m_bitmap->GetSize();
            auto layoutRect = LayoutRect();
            
            // LayoutRect가 유효한지 확인
            float layoutWidth = layoutRect.right - layoutRect.left;
            float layoutHeight = layoutRect.bottom - layoutRect.top;
            
            if (layoutWidth > 0.0f && layoutHeight > 0.0f && bitmapSize.width > 0.0f && bitmapSize.height > 0.0f)
            {
                float bitmapAspect = bitmapSize.width / bitmapSize.height;
                float layoutAspect = layoutWidth / layoutHeight;
                
                D2D1_RECT_F sourceRect = D2D1::RectF(0.0f, 0.0f, bitmapSize.width, bitmapSize.height);
                D2D1_RECT_F destRect = layoutRect;
                
                if (bitmapAspect > layoutAspect)
                {
                    // 비트맵이 더 넓음: 너비에 맞춤
                    float scaledHeight = layoutWidth / bitmapAspect;
                    float yOffset = (layoutHeight - scaledHeight) * 0.5f;
                    destRect.top = layoutRect.top + yOffset;
                    destRect.bottom = destRect.top + scaledHeight;
                }
                else
                {
                    // 비트맵이 더 높음: 높이에 맞춤
                    float scaledWidth = layoutHeight * bitmapAspect;
                    float xOffset = (layoutWidth - scaledWidth) * 0.5f;
                    destRect.left = layoutRect.left + xOffset;
                    destRect.right = destRect.left + scaledWidth;
                }
                
                target->DrawBitmap(
                    m_bitmap.Get(),
                    destRect,
                    1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                    sourceRect);
            }
        }

        Wnd::OnRender(target);
    }
}

