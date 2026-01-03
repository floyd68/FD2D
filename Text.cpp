#include "Text.h"

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
            }
        }
    }

    Size Text::Measure(Size available)
    {
        if (m_text.empty())
        {
            m_desired = { 0.0f, m_size };
            return m_desired;
        }

        // 텍스트 크기를 계산하기 위해 임시 format이 필요
        // 실제 크기는 Arrange에서 계산되지만, 여기서는 폰트 크기 기반으로 추정
        float estimatedWidth = static_cast<float>(m_text.length()) * m_size * 0.6f; // 대략적인 추정
        if (available.w > 0.0f && estimatedWidth > available.w)
        {
            estimatedWidth = available.w;
        }
        
        m_desired = { estimatedWidth, m_size * 1.2f }; // line height
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
}

