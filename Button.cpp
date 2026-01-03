#include "Button.h"
#include <windowsx.h>

namespace FD2D
{
    Button::Button()
        : Wnd()
    {
    }

    Button::Button(const std::wstring& name)
        : Wnd(name)
    {
    }

    Size Button::Measure(Size available)
    {
        // 라벨의 크기를 기반으로 버튼 크기 계산
        Size labelSize = m_label.Measure(available);
        float padding = 20.0f; // 좌우 패딩
        m_desired = { labelSize.w + padding + 2 * m_margin, labelSize.h + 10.0f + 2 * m_margin };
        return m_desired;
    }

    void Button::SetRect(const D2D1_RECT_F& rect)
    {
        SetLayoutRect(rect);
    }

    void Button::Arrange(Rect finalRect)
    {
        Wnd::Arrange(finalRect);
        
        // 라벨을 버튼 중앙에 배치
        const auto& rect = LayoutRect();
        D2D1_RECT_F labelRect = rect;
        m_label.SetRect(labelRect);
    }

    void Button::SetLabel(const std::wstring& text)
    {
        m_label.SetText(text);
    }

    void Button::SetColors(const D2D1_COLOR_F& normal, const D2D1_COLOR_F& hot, const D2D1_COLOR_F& pressed)
    {
        m_colorNormal = normal;
        m_colorHot = hot;
        m_colorPressed = pressed;
    }

    void Button::OnClick(ClickHandler handler)
    {
        m_click = std::move(handler);
    }

    bool Button::OnMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        UNREFERENCED_PARAMETER(wParam);

        switch (message)
        {
        case WM_MOUSEMOVE:
        {
            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            bool prevHover = m_hovered;
            m_hovered = HitTest(pt);
            if (m_hovered != prevHover)
            {
                Invalidate();
            }
            return m_hovered;
        }
        case WM_LBUTTONDOWN:
        {
            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (HitTest(pt))
            {
                m_pressed = true;
                Invalidate();
                return true;
            }
            break;
        }
        case WM_LBUTTONUP:
        {
            bool wasPressed = m_pressed;
            m_pressed = false;

            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (wasPressed && HitTest(pt))
            {
                if (m_click)
                {
                    m_click();
                }
                Invalidate();
                return true;
            }
            if (wasPressed)
            {
                Invalidate();
            }
            break;
        }
        default:
            break;
        }

        return Wnd::OnMessage(message, wParam, lParam);
    }

    void Button::OnRender(ID2D1RenderTarget* target)
    {
        if (target == nullptr)
        {
            return;
        }

        if (!m_brush)
        {
            target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_brush);
        }

        D2D1_COLOR_F fillColor = m_colorNormal;
        if (m_pressed)
        {
            fillColor = m_colorPressed;
        }
        else if (m_hovered)
        {
            fillColor = m_colorHot;
        }

        m_brush->SetColor(fillColor);
        target->FillRectangle(LayoutRect(), m_brush.Get());

        m_brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
        target->DrawRectangle(LayoutRect(), m_brush.Get(), 1.5f);

        m_label.OnRender(target);

        Wnd::OnRender(target);
    }

    bool Button::HitTest(const POINT& pt) const
    {
        const auto& rect = LayoutRect();
        return pt.x >= rect.left &&
            pt.x <= rect.right &&
            pt.y >= rect.top &&
            pt.y <= rect.bottom;
    }
}

