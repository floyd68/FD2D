#include "Text.h"
#include <cmath>

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
        if (m_text == text)
        {
            return;
        }
        m_text = text;
        m_textLayoutDirty = true;
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
        if (m_family == familyName && std::abs(m_size - size) < 0.01f)
        {
            return;
        }
        m_family = familyName;
        m_size = size;
        m_format.Reset();
        m_ellipsisSign.Reset();
        m_textLayout.Reset();
        m_textLayoutDirty = true;
    }

    void Text::SetFixedWidth(float width)
    {
        const float normalized = (width > 0.0f) ? width : 0.0f;
        if (std::abs(m_fixedWidth - normalized) < 0.01f)
        {
            return;
        }
        m_fixedWidth = normalized;
        m_textLayoutDirty = true;
    }

    void Text::SetTextAlignment(DWRITE_TEXT_ALIGNMENT alignment)
    {
        if (m_textAlignment == alignment)
        {
            return;
        }
        m_textAlignment = alignment;
        if (m_format)
        {
            (void)m_format->SetTextAlignment(alignment);
        }
        m_textLayoutDirty = true;
    }

    void Text::SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT alignment)
    {
        if (m_paragraphAlignment == alignment)
        {
            return;
        }
        m_paragraphAlignment = alignment;
        if (m_format)
        {
            (void)m_format->SetParagraphAlignment(alignment);
        }
        m_textLayoutDirty = true;
    }

    void Text::SetEllipsisTrimmingEnabled(bool enabled)
    {
        if (m_ellipsisTrimmingEnabled == enabled)
        {
            return;
        }
        m_ellipsisTrimmingEnabled = enabled;
        // Recreate format/resources so trimming sign gets applied.
        m_format.Reset();
        m_ellipsisSign.Reset();
        m_textLayout.Reset();
        m_textLayoutDirty = true;
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

                    m_textLayoutDirty = true;
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
            const float rectW = (std::max)(0.0f, rect.right - rect.left);
            const float rectH = (std::max)(0.0f, rect.bottom - rect.top);

            float layoutW = rectW;
            if (m_fixedWidth > 0.0f)
            {
                layoutW = (std::min)(layoutW, m_fixedWidth);
            }
            layoutW = (std::max)(1.0f, layoutW);
            const float layoutH = (std::max)(1.0f, rectH);

            const bool sizeChanged =
                std::abs(m_layoutWidth - layoutW) >= 0.5f ||
                std::abs(m_layoutHeight - layoutH) >= 0.5f;

            if (!m_textLayout || m_textLayoutDirty || sizeChanged)
            {
                m_textLayout.Reset();
                IDWriteFactory* factory = Core::DWriteFactory();
                if (factory != nullptr)
                {
                    (void)factory->CreateTextLayout(
                        m_text.c_str(),
                        static_cast<UINT32>(m_text.length()),
                        m_format.Get(),
                        layoutW,
                        layoutH,
                        &m_textLayout);
                }

                m_layoutWidth = layoutW;
                m_layoutHeight = layoutH;
                m_textLayoutDirty = false;
            }

            if (m_textLayout)
            {
                target->DrawTextLayout(
                    D2D1::Point2F(rect.left, rect.top),
                    m_textLayout.Get(),
                    m_brush.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            else
            {
                target->DrawTextW(
                    m_text.c_str(),
                    static_cast<UINT32>(m_text.length()),
                    m_format.Get(),
                    rect,
                    m_brush.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL);
            }
        }

        Wnd::OnRender(target);
    }

    bool Text::OnInputEvent(const InputEvent& event)
    {
        switch (event.type)
        {
        case InputEventType::MouseDown:
        {
            if (event.button != MouseButton::Left || !event.hasPoint)
            {
                break;
            }
            if (!m_onClick)
            {
                break;
            }
            const int x = event.point.x;
            const int y = event.point.y;
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

        return Wnd::OnInputEvent(event);
    }
}

