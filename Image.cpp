#include "Image.h"

namespace FD2D
{
    Image::Image()
        : Wnd()
    {
    }

    Image::Image(const std::wstring& name)
        : Wnd(name)
    {
    }

    void Image::SetRect(const D2D1_RECT_F& rect)
    {
        SetLayoutRect(rect);
    }

    Size Image::Measure(Size available)
    {
        if (m_bitmap)
        {
            auto size = m_bitmap->GetSize();
            float scale = std::min(available.w / size.width, available.h / size.height);
            m_desired = { size.width * scale, size.height * scale };
        }
        else if (available.w > 0.0f && available.h > 0.0f)
        {
            // 이미지가 로드되지 않았지만 available size가 있으면 그것을 사용
            m_desired = { available.w, available.h };
        }
        else
        {
            // 기본 크기
            m_desired = { 200.0f, 200.0f };
        }
        return m_desired;
    }

    HRESULT Image::SetSourceFile(const std::wstring& filePath)
    {
        m_filePath = filePath;
        ReleaseBitmap();
        return S_OK;
    }

    HRESULT Image::EnsureBitmap(ID2D1RenderTarget* target)
    {
        if (m_bitmap || target == nullptr || m_filePath.empty())
        {
            return S_OK;
        }

        IWICImagingFactory* wic = Core::WicFactory();
        if (wic == nullptr)
        {
            return E_POINTER;
        }

        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        HRESULT hr = wic->CreateDecoderFromFilename(
            m_filePath.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            &decoder);
        if (FAILED(hr))
        {
            return hr;
        }

        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr))
        {
            return hr;
        }

        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        hr = wic->CreateFormatConverter(&converter);
        if (FAILED(hr))
        {
            return hr;
        }

        hr = converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0f,
            WICBitmapPaletteTypeMedianCut);
        if (FAILED(hr))
        {
            return hr;
        }

        hr = target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &m_bitmap);
        return hr;
    }

    void Image::ReleaseBitmap()
    {
        m_bitmap.Reset();
    }

    void Image::OnRender(ID2D1RenderTarget* target)
    {
        if (FAILED(EnsureBitmap(target)))
        {
            return;
        }

        if (target != nullptr && m_bitmap)
        {
            target->DrawBitmap(
                m_bitmap.Get(),
                LayoutRect(),
                1.0f,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                D2D1::RectF(
                    0.0f,
                    0.0f,
                    static_cast<FLOAT>(m_bitmap->GetSize().width),
                    static_cast<FLOAT>(m_bitmap->GetSize().height)));
        }

        Wnd::OnRender(target);
    }
}

