#include "Text.h"
#include <windowsx.h>

namespace FD2D
{
    Text::Text()
        : Wnd()
    {
    }

    Text::Text(const std::wstring& name)
        : Wnd(name)
    {
    }

    void Text::SetText(const std::wstring& text)
    {
        m_text = text;
    }

    void Text::SetColor(const D2D1_COLOR_F& color)
    {
        m_color = color;
        m_brush.Reset();
    }

    void Text::SetRect(const D2D1_RECT_F& rect)
    {
        SetLayoutRect(rect);
    }

    void Text::SetFont(const std::wstring& familyName, FLOAT size)
    {
        m_family = familyName;
        m_size = size;
        m_format.Reset();
        m_ellipsisSign.Reset();
    }

    void Text::SetFixedWidth(float width)
    {
        m_fixedWidth = (width > 0.0f) ? width : 0.0f;
    }

    void Text::SetTextAlignment(DWRITE_TEXT_ALIGNMENT alignment)
    {
        m_textAlignment = alignment;
        if (m_format)
        {
            (void)m_format->SetTextAlignment(alignment);
        }
    }

    void Text::SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT alignment)
    {
        m_paragraphAlignment = alignment;
        if (m_format)
        {
            (void)m_format->SetParagraphAlignment(alignment);
        }
    }

    void Text::SetEllipsisTrimmingEnabled(bool enabled)
    {
        m_ellipsisTrimmingEnabled = enabled;
        // Recreate format/resources so trimming sign gets applied.
        m_format.Reset();
        m_ellipsisSign.Reset();
    }

    void Text::SetOnClick(ClickHandler handler)
    {
        m_onClick = std::move(handler);
    }

    void Text::EnsureResources(ID2D1RenderTarget* target)
    {
        if (target == nullptr)
        {
            return;
        }

        if (!m_brush)
        {
            target->CreateSolidColorBrush(m_color, &m_brush);
        }

        if (!m_format)
        {
            IDWriteFactory* factory = Core::DWriteFactory();
            if (factory != nullptr)
            {
                factory->CreateTextFormat(
                    m_family.c_str(),
                    nullptr,
                    DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL,
                    m_size,
                    L"",
                    &m_format);

                if (m_format)
                {
                    (void)m_format->SetTextAlignment(m_textAlignment);
                    (void)m_format->SetParagraphAlignment(m_paragraphAlignment);

                    if (m_ellipsisTrimmingEnabled)
                    {
                        if (!m_ellipsisSign)
                        {
                            (void)factory->CreateEllipsisTrimmingSign(m_format.Get(), &m_ellipsisSign);
                        }

                        DWRITE_TRIMMING trimming {};
                        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                        trimming.delimiter = 0;
                        trimming.delimiterCount = 0;
                        (void)m_format->SetTrimming(&trimming, m_ellipsisSign.Get());
                        (void)m_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    }
                }
            }
        }
    }

    Size Text::Measure(Size available)
    {
        if (m_text.empty())
        {
            const float w = (m_fixedWidth > 0.0f) ? m_fixedWidth : 0.0f;
            m_desired = { w, m_size };
            return m_desired;
        }

        const float lineH = m_size * 1.2f;

        if (m_fixedWidth > 0.0f)
        {
            float w = m_fixedWidth;
            if (available.w > 0.0f)
            {
                w = (std::min)(w, available.w);
            }
            m_desired = { w, lineH };
            return m_desired;
        }

        // Fallback: approximate width based on font size.
        float estimatedWidth = static_cast<float>(m_text.length()) * m_size * 0.6f;
        if (available.w > 0.0f)
        {
            estimatedWidth = (std::min)(estimatedWidth, available.w);
        }

        m_desired = { estimatedWidth, lineH };
        return m_desired;
    }

    void Text::OnRender(ID2D1RenderTarget* target)
    {
        EnsureResources(target);

        if (target != nullptr && m_brush && m_format)
        {
            D2D1_RECT_F rect = LayoutRect();
            target->DrawTextW(
                m_text.c_str(),
                static_cast<UINT32>(m_text.length()),
                m_format.Get(),
                rect,
                m_brush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                DWRITE_MEASURING_MODE_NATURAL);
        }

        Wnd::OnRender(target);
    }

    bool Text::OnMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        UNREFERENCED_PARAMETER(wParam);
        switch (message)
        {
        case WM_LBUTTONDOWN:
        {
            if (!m_onClick)
            {
                break;
            }
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const D2D1_RECT_F r = LayoutRect();
            if (static_cast<float>(x) >= r.left &&
                static_cast<float>(x) <= r.right &&
                static_cast<float>(y) >= r.top &&
                static_cast<float>(y) <= r.bottom)
            {
                m_onClick();
                return true;
            }
            break;
        }
        default:
            break;
        }

        return Wnd::OnMessage(message, wParam, lParam);
    }
}

