#include "ThumbImage.h"
#include "Backplate.h"
#include "Spinner.h"
#include "Core.h"
#include "Util.h"
#include "../CommonUtil.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <limits>

namespace FD2D
{
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
        // m_pipeline's destructor cancels any in-flight request.
    }

    Size ThumbImage::Measure(Size available)
    {
        // Thumbnail mode: respect targetSize (can be non-square for variable width mode)
        if (m_request.targetSize.w > 0.0f && m_request.targetSize.h > 0.0f)
        {
            float width = m_request.targetSize.w;
            float height = m_request.targetSize.h;
            
            if (available.w > 0.0f)
            {
                width = (std::min)(width, available.w);
            }
            if (available.h > 0.0f)
            {
                height = (std::min)(height, available.h);
            }

            m_desired = { width, height };
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
        const std::wstring normalized = CommonUtil::NormalizePath(filePath);
        if (!normalized.empty() && normalized == m_filePath)
        {
            // If a previous decode/create failed for the same path, allow explicit retry.
            // This prevents thumbnails from getting stuck blank after transient failures.
            const bool hadFailure = m_pipeline.ClearFailureIfMatches(normalized);

            if (hadFailure || (!m_pipeline.IsLoading() && m_bitmap == nullptr))
            {
                m_request.source = m_filePath;
                RequestImageLoad();
                Invalidate();
                return S_OK;
            }

            return S_FALSE;
        }

        m_pipeline.CancelInflight();

        m_filePath = normalized;
        m_pipeline.ClearFailure();

        m_pipeline.SetLoading(false);
        m_pipeline.ResetInflightToken();
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
        m_selectionAnimStartMs = CommonUtil::NowMs();
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

    D2D1_SIZE_F ThumbImage::GetBitmapSize() const
    {
        if (m_bitmap)
        {
            return m_bitmap->GetSize();
        }
        return D2D1::SizeF(0.0f, 0.0f);
    }

    void ThumbImage::RequestImageLoad()
    {
        if (m_filePath.empty() || m_pipeline.IsLoading())
        {
            return;
        }

        if (m_pipeline.IsFailedFor(m_filePath))
        {
            return;
        }

        if (m_loadedFilePath == m_filePath && m_bitmap)
        {
            return;
        }

        m_pipeline.SetLoading(true);
        m_request.source = m_filePath;
        m_request.purpose = ImageCore::ImagePurpose::Thumbnail;
        m_request.allowGpuCompressedDDS = false; // thumbnails never use GPU DDS path

        m_pipeline.Dispatch(m_request, m_filePath);
    }

    void ThumbImage::OnRender(ID2D1RenderTarget* target)
    {
        if (!target)
        {
            return;
        }

        FD2D::Backplate* bp = BackplateRef();
        const bool deferBitmapUpload = (bp != nullptr && bp->IsInSizeMove());

        // Consume pending decoded image -> D2D bitmap (render thread).
        AsyncImagePipeline::Payload pending {};
        if (!deferBitmapUpload)
        {
            pending = m_pipeline.TakePending();
        }

        if (!pending.sourcePath.empty() && pending.blocks &&
            pending.width > 0 && pending.height > 0 && pending.rowPitch > 0)
        {
            // Thumbnails always render via D2D bitmap; expect CPU BGRA8 output.
            Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dBitmap;
            const HRESULT hrBmp = AsyncImagePipeline::CreateD2DBitmap(
                target, pending, D2D1_ALPHA_MODE_IGNORE, d2dBitmap);

            if (SUCCEEDED(hrBmp) && d2dBitmap)
            {
                m_bitmap = d2dBitmap;
                m_loadedFilePath = pending.sourcePath;
            }
            else
            {
                m_pipeline.RecordFailure(pending.sourcePath, hrBmp);
            }
            m_pipeline.SetLoading(false);
            m_pipeline.ResetInflightToken();
        }

        if (m_bitmap == nullptr && !m_pipeline.IsLoading())
        {
            RequestImageLoad();
        }

        const auto computeAspectFitDestRect = [](const D2D1_RECT_F& layoutRect, const D2D1_SIZE_F& bitmapSize) -> D2D1_RECT_F
        {
            return Util::ComputeAspectFitRect(layoutRect, bitmapSize);
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
            target->DrawBitmap(bmp, destRect, opacity, interpMode, sourceRect);
        };

    if (m_bitmap)
    {
        drawBmp(m_bitmap.Get(), 1.0f);
    }

        const bool shouldShowSpinner = m_loadingSpinnerEnabled && m_pipeline.IsLoading();
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

                float selT = 1.0f;
                bool selAnimating = false;
                if (m_selectionAnimStartMs != 0 && m_selectionAnimMs > 0)
                {
                    const unsigned long long elapsed = CommonUtil::NowMs() - m_selectionAnimStartMs;
                    selT = CommonUtil::Clamp01(static_cast<float>(elapsed) / static_cast<float>(m_selectionAnimMs));
                    selAnimating = selT < 1.0f;
                }

                const float ease = 1.0f - (1.0f - selT) * (1.0f - selT);
                const float popInflate = m_selectionStyle.popInflate * (1.0f - ease);
                const float baseInflate = m_selectionStyle.baseInflate;

                float breathe01 = 0.0f;
                if (m_selectionStyle.breatheEnabled && m_selectionStyle.breathePeriodMs > 0)
                {
                    const float period = static_cast<float>(m_selectionStyle.breathePeriodMs);
                    const float t = static_cast<float>(CommonUtil::NowMs() % static_cast<unsigned long long>(m_selectionStyle.breathePeriodMs));
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

                const bool breatheAnimActive =
                    m_selectionStyle.breatheEnabled &&
                    m_selectionStyle.breathePeriodMs > 0 &&
                    (
                        m_selectionStyle.breatheInflateAmp > 0.0f ||
                        m_selectionStyle.breatheThicknessAmp > 0.0f ||
                        m_selectionStyle.breatheAlphaAmp > 0.0f
                    );

                if ((selAnimating || breatheAnimActive) && BackplateRef() != nullptr)
                {
                    BackplateRef()->RequestAnimationFrame();
                }
            }
        }

        Wnd::OnRender(target);
    }

    bool ThumbImage::OnInputEvent(const InputEvent& event)
    {
        switch (event.type)
        {
        case InputEventType::MouseDown:
        {
            if (event.button != MouseButton::Left || !event.hasPoint)
            {
                break;
            }
            POINT pt = event.point;
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

        return Wnd::OnInputEvent(event);
    }
}

