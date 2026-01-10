#include "ThumbImage.h"
#include "Backplate.h"
#include "Spinner.h"
#include "Core.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <windowsx.h>

namespace FD2D
{
    namespace
    {
        static unsigned long long NowMs()
        {
            return static_cast<unsigned long long>(GetTickCount64());
        }

        static float Clamp01(float v)
        {
            if (v < 0.0f)
            {
                return 0.0f;
            }
            if (v > 1.0f)
            {
                return 1.0f;
            }
            return v;
        }

        static std::wstring NormalizePath(std::wstring_view p)
        {
            if (p.empty())
            {
                return {};
            }

            try
            {
                std::filesystem::path fp = std::filesystem::path(p).lexically_normal();
                fp.make_preferred();

                std::wstring out = fp.wstring();
                for (auto& ch : out)
                {
                    ch = static_cast<wchar_t>(towlower(ch));
                }
                return out;
            }
            catch (...)
            {
                std::wstring out(p);
                for (auto& ch : out)
                {
                    ch = static_cast<wchar_t>(towlower(ch));
                }
                return out;
            }
        }
    }

    ThumbImage::ThumbImage()
        : Wnd()
        , m_request()
    {
        m_request.purpose = ImageCore::ImagePurpose::Thumbnail;
        m_loadingSpinner = std::make_shared<Spinner>(L"loadingSpinner");
        m_loadingSpinner->SetActive(false);
        AddChild(m_loadingSpinner);
    }

    ThumbImage::ThumbImage(const std::wstring& name)
        : Wnd(name)
        , m_request()
    {
        m_request.purpose = ImageCore::ImagePurpose::Thumbnail;
        m_loadingSpinner = std::make_shared<Spinner>(L"loadingSpinner");
        m_loadingSpinner->SetActive(false);
        AddChild(m_loadingSpinner);
    }

    ThumbImage::~ThumbImage()
    {
        if (m_currentHandle != 0)
        {
            ImageCore::ImageLoader::Instance().Cancel(m_currentHandle);
        }
    }

    Size ThumbImage::Measure(Size available)
    {
        // Thumbnail mode: respect targetSize as fixed square (helps horizontal strip sizing)
        if (m_request.targetSize.w > 0.0f && m_request.targetSize.h > 0.0f)
        {
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

        m_desired = { 128.0f, 128.0f };
        return m_desired;
    }

    void ThumbImage::SetThumbnailSize(const Size& size)
    {
        m_request.targetSize = ImageCore::Size { size.w, size.h };
        m_request.purpose = ImageCore::ImagePurpose::Thumbnail;
    }

    HRESULT ThumbImage::SetSourceFile(const std::wstring& filePath)
    {
        const std::wstring normalized = NormalizePath(filePath);
        if (!normalized.empty() && normalized == m_filePath)
        {
            return S_FALSE;
        }

        if (m_currentHandle != 0)
        {
            ImageCore::ImageLoader::Instance().Cancel(m_currentHandle);
            m_currentHandle = 0;
        }

        m_filePath = normalized;
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_failedFilePath.clear();
            m_failedHr = S_OK;
        }

        m_loading.store(false);
        m_inflightToken.store(0);
        m_request.source = m_filePath;
        RequestImageLoad();
        Invalidate();
        return S_OK;
    }

    void ThumbImage::SetSelected(bool selected)
    {
        if (m_selected == selected)
        {
            return;
        }
        m_selected = selected;
        m_selectionAnimStartMs = NowMs();
        Invalidate();
        if (BackplateRef() != nullptr)
        {
            BackplateRef()->RequestAnimationFrame();
        }
    }

    void ThumbImage::SetSelectionStyle(const SelectionStyle& style)
    {
        m_selectionStyle = style;
        m_selectionBrush.Reset();
        m_selectionShadowBrush.Reset();
        m_selectionFillBrush.Reset();
        Invalidate();
    }

    void ThumbImage::SetOnClick(ClickHandler handler)
    {
        m_onClick = std::move(handler);
    }

    void ThumbImage::SetLoadingSpinnerEnabled(bool enabled)
    {
        if (m_loadingSpinnerEnabled == enabled)
        {
            return;
        }
        m_loadingSpinnerEnabled = enabled;
        Invalidate();
    }

    void ThumbImage::RequestImageLoad()
    {
        if (m_filePath.empty() || m_loading.load())
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            if (!m_failedFilePath.empty() && m_failedFilePath == m_filePath)
            {
                return;
            }
        }

        if (m_loadedFilePath == m_filePath && m_bitmap)
        {
            return;
        }

        m_loading.store(true);
        m_request.source = m_filePath;
        m_request.purpose = ImageCore::ImagePurpose::Thumbnail;
        m_request.allowGpuCompressedDDS = false; // thumbnails never use GPU DDS path

        const unsigned long long token = m_requestToken.fetch_add(1ULL) + 1ULL;
        m_inflightToken.store(token);
        const std::wstring requestedPath = m_filePath;

        m_currentHandle = ImageCore::ImageLoader::Instance().Request(
            m_request,
            [this, token, requestedPath](HRESULT hr, Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap, std::unique_ptr<DirectX::ScratchImage> scratchImage)
            {
                const unsigned long long current = m_inflightToken.load();
                if (token != current)
                {
                    if (current == 0)
                    {
                        m_loading.store(false);
                    }
                    return;
                }

                OnImageLoaded(requestedPath, hr, wicBitmap, std::move(scratchImage));
            });
    }

    void ThumbImage::OnImageLoaded(
        const std::wstring& sourcePath,
        HRESULT hr,
        Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap,
        std::unique_ptr<DirectX::ScratchImage> scratchImage)
    {
        m_currentHandle = 0;

        const std::wstring normalizedSource = NormalizePath(sourcePath);

        if (SUCCEEDED(hr) && (wicBitmap || scratchImage))
        {
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                m_pendingWicBitmap = wicBitmap;
                m_pendingScratchImage = std::move(scratchImage);
                m_pendingSourcePath = normalizedSource;
                m_failedFilePath.clear();
                m_failedHr = S_OK;
            }

            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestAsyncRedraw();
            }
        }
        else
        {
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                m_failedFilePath = normalizedSource;
                m_failedHr = hr;
            }
            m_loading.store(false);
            m_inflightToken.store(0);

            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestAsyncRedraw();
            }
        }
    }

    HRESULT ThumbImage::ConvertToD2DBitmap(
        ID2D1RenderTarget* target,
        const std::wstring& sourcePath,
        Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap,
        std::unique_ptr<DirectX::ScratchImage> scratchImage)
    {
        if (target == nullptr)
        {
            return E_INVALIDARG;
        }

        Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dBitmap;
        HRESULT hr = E_FAIL;

        // Always ignore alpha for thumbnails (opaque tiles).
        if (scratchImage)
        {
            const DirectX::Image* image = scratchImage->GetImage(0, 0, 0);
            if (image && image->pixels)
            {
                D2D1_BITMAP_PROPERTIES props {};
                props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
                props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
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
        else if (wicBitmap)
        {
            D2D1_BITMAP_PROPERTIES props {};
            props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
            props.dpiX = 96.0f;
            props.dpiY = 96.0f;
            hr = target->CreateBitmapFromWicBitmap(wicBitmap.Get(), &props, &d2dBitmap);
            if (FAILED(hr))
            {
                hr = target->CreateBitmapFromWicBitmap(wicBitmap.Get(), nullptr, &d2dBitmap);
            }
        }

        if (SUCCEEDED(hr) && d2dBitmap)
        {
            if (m_bitmap && m_loadedFilePath != sourcePath)
            {
                m_prevBitmap = m_bitmap;
                m_prevLoadedFilePath = m_loadedFilePath;
                m_fadeStartMs = NowMs();
            }

            m_bitmap = d2dBitmap;
            m_loadedFilePath = sourcePath;
        }

        return hr;
    }

    void ThumbImage::OnRender(ID2D1RenderTarget* target)
    {
        if (!target)
        {
            return;
        }

        // Consume pending decoded image -> D2D bitmap (render thread).
        Microsoft::WRL::ComPtr<IWICBitmapSource> pendingWic;
        std::unique_ptr<DirectX::ScratchImage> pendingScratch;
        std::wstring pendingSource;
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            if (m_pendingWicBitmap || m_pendingScratchImage)
            {
                pendingWic = m_pendingWicBitmap;
                pendingScratch = std::move(m_pendingScratchImage);
                pendingSource = m_pendingSourcePath;
                m_pendingWicBitmap.Reset();
                m_pendingSourcePath.clear();
            }
        }

        if (!pendingSource.empty() && (pendingWic || pendingScratch))
        {
            (void)ConvertToD2DBitmap(target, pendingSource, pendingWic, std::move(pendingScratch));
            m_loading.store(false);
            m_inflightToken.store(0);
        }

        if (m_bitmap == nullptr && !m_loading.load())
        {
            RequestImageLoad();
        }

        const auto computeAspectFitDestRect = [](const D2D1_RECT_F& layoutRect, const D2D1_SIZE_F& bitmapSize) -> D2D1_RECT_F
        {
            const float layoutWidth = layoutRect.right - layoutRect.left;
            const float layoutHeight = layoutRect.bottom - layoutRect.top;

            if (!(layoutWidth > 0.0f && layoutHeight > 0.0f && bitmapSize.width > 0.0f && bitmapSize.height > 0.0f))
            {
                return layoutRect;
            }

            const float bitmapAspect = bitmapSize.width / bitmapSize.height;
            const float layoutAspect = layoutWidth / layoutHeight;

            D2D1_RECT_F destRect = layoutRect;

            if (bitmapAspect > layoutAspect)
            {
                const float scaledHeight = layoutWidth / bitmapAspect;
                const float yOffset = (layoutHeight - scaledHeight) * 0.5f;
                destRect.top = layoutRect.top + yOffset;
                destRect.bottom = destRect.top + scaledHeight;
            }
            else
            {
                const float scaledWidth = layoutHeight * bitmapAspect;
                const float xOffset = (layoutWidth - scaledWidth) * 0.5f;
                destRect.left = layoutRect.left + xOffset;
                destRect.right = destRect.left + scaledWidth;
            }

            return destRect;
        };

        // Actual bitmap draw:
        auto drawBmp = [&](ID2D1Bitmap* bmp, float opacity)
        {
            if (!bmp || opacity <= 0.0f)
            {
                return;
            }
            const auto bitmapSize = bmp->GetSize();
            const auto layoutRect = LayoutRect();
            const D2D1_RECT_F sourceRect = D2D1::RectF(0.0f, 0.0f, bitmapSize.width, bitmapSize.height);
            const D2D1_RECT_F destRect = computeAspectFitDestRect(layoutRect, bitmapSize);

            D2D1_BITMAP_INTERPOLATION_MODE interpMode = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
            const FD2D::D2DVersion d2dVersion = FD2D::Core::GetSupportedD2DVersion();
            if (d2dVersion >= FD2D::D2DVersion::D2D1_1)
            {
                #ifdef D2D1_BITMAP_INTERPOLATION_MODE_CUBIC
                interpMode = D2D1_BITMAP_INTERPOLATION_MODE_CUBIC;
                #endif
            }

            target->DrawBitmap(bmp, destRect, opacity, interpMode, sourceRect);
        };

        bool isFading = false;
        float fadeT = 1.0f;
        if (m_prevBitmap && m_fadeStartMs != 0 && m_fadeDurationMs > 0)
        {
            const unsigned long long elapsed = NowMs() - m_fadeStartMs;
            fadeT = Clamp01(static_cast<float>(elapsed) / static_cast<float>(m_fadeDurationMs));
            isFading = fadeT < 1.0f;
        }

        if (m_bitmap)
        {
            if (isFading && m_prevBitmap)
            {
                drawBmp(m_prevBitmap.Get(), 1.0f - fadeT);
                drawBmp(m_bitmap.Get(), fadeT);
            }
            else
            {
                if (m_prevBitmap)
                {
                    m_prevBitmap.Reset();
                    m_prevLoadedFilePath.clear();
                    m_fadeStartMs = 0;
                }
                drawBmp(m_bitmap.Get(), 1.0f);
            }
        }

        const bool shouldShowSpinner = m_loadingSpinnerEnabled && m_loading.load();
        if (m_loadingSpinner)
        {
            m_loadingSpinner->SetActive(shouldShowSpinner);
        }

        // Selection overlay for thumbnails.
        if (m_selected)
        {
            if (!m_selectionBrush)
            {
                (void)target->CreateSolidColorBrush(m_selectionStyle.accent, &m_selectionBrush);
            }
            if (!m_selectionShadowBrush)
            {
                (void)target->CreateSolidColorBrush(m_selectionStyle.shadow, &m_selectionShadowBrush);
            }
            if (!m_selectionFillBrush)
            {
                (void)target->CreateSolidColorBrush(
                    D2D1::ColorF(m_selectionStyle.fill.r, m_selectionStyle.fill.g, m_selectionStyle.fill.b, 0.0f),
                    &m_selectionFillBrush);
            }

            if (m_selectionBrush)
            {
                D2D1_RECT_F r = LayoutRect();
                if (m_bitmap)
                {
                    r = computeAspectFitDestRect(r, m_bitmap->GetSize());
                }
                else if (m_prevBitmap)
                {
                    r = computeAspectFitDestRect(r, m_prevBitmap->GetSize());
                }

                float selT = 1.0f;
                bool selAnimating = false;
                if (m_selectionAnimStartMs != 0 && m_selectionAnimMs > 0)
                {
                    const unsigned long long elapsed = NowMs() - m_selectionAnimStartMs;
                    selT = Clamp01(static_cast<float>(elapsed) / static_cast<float>(m_selectionAnimMs));
                    selAnimating = selT < 1.0f;
                }

                const float ease = 1.0f - (1.0f - selT) * (1.0f - selT);
                const float popInflate = m_selectionStyle.popInflate * (1.0f - ease);
                const float baseInflate = m_selectionStyle.baseInflate;

                float breathe01 = 0.0f;
                if (m_selectionStyle.breatheEnabled && m_selectionStyle.breathePeriodMs > 0)
                {
                    const float period = static_cast<float>(m_selectionStyle.breathePeriodMs);
                    const float t = static_cast<float>(NowMs() % static_cast<unsigned long long>(m_selectionStyle.breathePeriodMs));
                    const float phase = (t / period) * 6.28318530718f;
                    breathe01 = 0.5f + 0.5f * std::sinf(phase);
                }
                const float breatheInflate = m_selectionStyle.breatheInflateAmp * breathe01;

                r.left -= (baseInflate + popInflate + breatheInflate);
                r.top -= (baseInflate + popInflate + breatheInflate);
                r.right += (baseInflate + popInflate + breatheInflate);
                r.bottom += (baseInflate + popInflate + breatheInflate);

                const float radius = m_selectionStyle.radius;
                const D2D1_ROUNDED_RECT rr { r, radius, radius };

                if (m_selectionFillBrush)
                {
                    const float fillA = m_selectionStyle.fillMaxAlpha * ease;
                    m_selectionFillBrush->SetColor(D2D1::ColorF(m_selectionStyle.fill.r, m_selectionStyle.fill.g, m_selectionStyle.fill.b, fillA));
                    const D2D1_ROUNDED_RECT fillRR { r, (std::max)(0.0f, radius - 1.0f), (std::max)(0.0f, radius - 1.0f) };
                    target->FillRoundedRectangle(fillRR, m_selectionFillBrush.Get());
                }

                const float shadowW = m_selectionStyle.shadowThickness;
                const float accentW = (std::max)(0.0f, m_selectionStyle.accentThickness + (1.0f - ease) + (m_selectionStyle.breatheThicknessAmp * breathe01));

                if (m_selectionBrush)
                {
                    const float baseA = m_selectionStyle.accent.a;
                    const float pulseA = baseA * (1.0f - m_selectionStyle.breatheAlphaAmp) + baseA * m_selectionStyle.breatheAlphaAmp * breathe01;
                    m_selectionBrush->SetColor(D2D1::ColorF(m_selectionStyle.accent.r, m_selectionStyle.accent.g, m_selectionStyle.accent.b, pulseA));
                }

                if (m_selectionShadowBrush)
                {
                    target->DrawRoundedRectangle(rr, m_selectionShadowBrush.Get(), shadowW);
                }
                target->DrawRoundedRectangle(rr, m_selectionBrush.Get(), accentW);

                if ((selAnimating || (m_selectionStyle.breatheEnabled && m_selectionStyle.breatheInflateAmp > 0.0f)) && BackplateRef() != nullptr)
                {
                    BackplateRef()->RequestAnimationFrame();
                }
            }
        }

        Wnd::OnRender(target);

        if (isFading && BackplateRef() != nullptr)
        {
            BackplateRef()->RequestAnimationFrame();
        }
    }

    bool ThumbImage::OnMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        UNREFERENCED_PARAMETER(wParam);

        switch (message)
        {
        case WM_LBUTTONDOWN:
        {
            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const D2D1_RECT_F r = LayoutRect();
            if (static_cast<float>(pt.x) >= r.left &&
                static_cast<float>(pt.x) <= r.right &&
                static_cast<float>(pt.y) >= r.top &&
                static_cast<float>(pt.y) <= r.bottom)
            {
                if (m_onClick)
                {
                    m_onClick();
                    return true;
                }
            }
            break;
        }
        default:
            break;
        }

        return Wnd::OnMessage(message, wParam, lParam);
    }
}

