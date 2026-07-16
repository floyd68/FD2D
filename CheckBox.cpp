#include "CheckBox.h"

namespace FD2D
{
    CheckBox::CheckBox()
        : Wnd()
    {
        // Center the label against the checkbox glyph regardless of row height.
        // ContentAlignV::Center places the natural-size text rect; DWrite's
        // paragraph alignment is left at NEAR so descenders aren't clipped by
        // a taller assigned rect with CLIP enabled.
        SetContentMargin(4.0f, 0.0f);
        SetContentAlign(AlignH::Start, AlignV::Center);
    }

    CheckBox::CheckBox(const std::wstring& name)
        : Wnd(name)
    {
        SetContentMargin(4.0f, 0.0f);
        SetContentAlign(AlignH::Start, AlignV::Center);
    }

    Size CheckBox::Measure(Size available)
    {
        Size labelSize = m_label.Measure(available);
        m_desired = {
            kBoxSize + kLabelGap + labelSize.w + m_contentMargin.Horizontal() + 2.0f * m_margin,
            (std::max)(kBoxSize, labelSize.h + m_contentMargin.Vertical()) + 2.0f * m_margin
        };
        return m_desired;
    }

    void CheckBox::Arrange(Rect finalRect)
    {
        Wnd::Arrange(finalRect);

        // Leading chrome: checkbox glyph + gap. Content margin/align apply to
        // the remaining label area only.
        Rect labelBounds = BoundsRect();
        float leading = kBoxSize + kLabelGap;
        labelBounds.x += leading;
        labelBounds.w = (std::max)(0.0f, labelBounds.w - leading);

        Size labelSize = m_label.Measure({ 0.0f, 0.0f });
        m_label.SetRect(ToD2D(ContentRectFor(labelBounds, labelSize)));
    }

    void CheckBox::SetLabel(const std::wstring& text)
    {
        m_label.SetText(text);
    }

    void CheckBox::SetChecked(bool checked, bool notify)
    {
        bool changed = (checked != m_checked);
        m_checked = checked;
        if (changed && notify && m_changed)
        {
            m_changed(m_checked);
        }
        Invalidate();
    }

    void CheckBox::OnCheckedChanged(CheckedChangedHandler handler)
    {
        m_changed = std::move(handler);
    }

    void CheckBox::SetEnabled(bool enabled)
    {
        if (m_enabled == enabled)
        {
            return;
        }
        m_enabled = enabled;
        if (!enabled)
        {
            m_pressed = false;
            m_hovered = false;
        }
        Invalidate();
    }

    D2D1_RECT_F CheckBox::BoxRect() const
    {
        const auto& rect = LayoutRect();
        float cy = (rect.top + rect.bottom) * 0.5f;
        float top = cy - kBoxSize * 0.5f;
        return D2D1::RectF(rect.left, top, rect.left + kBoxSize, top + kBoxSize);
    }

    bool CheckBox::HitTest(const POINT& pt) const
    {
        const auto& rect = LayoutRect();
        return pt.x >= rect.left && pt.x <= rect.right && pt.y >= rect.top && pt.y <= rect.bottom;
    }

    bool CheckBox::OnInputEvent(const InputEvent& event)
    {
        if (!m_enabled)
        {
            return Wnd::OnInputEvent(event);
        }
        switch (event.type)
        {
        case InputEventType::MouseMove:
        {
            if (!event.hasPoint)
            {
                return false;
            }
            bool prevHover = m_hovered;
            m_hovered = HitTest(event.point);
            if (m_hovered != prevHover)
            {
                Invalidate();
            }
            return m_hovered;
        }
        case InputEventType::MouseDown:
        {
            if (event.button != MouseButton::Left || !event.hasPoint || !HitTest(event.point))
            {
                break;
            }
            m_pressed = true;
            Invalidate();
            return true;
        }
        case InputEventType::MouseUp:
        {
            if (event.button != MouseButton::Left || !event.hasPoint)
            {
                break;
            }
            bool wasPressed = m_pressed;
            m_pressed = false;
            if (wasPressed && HitTest(event.point))
            {
                SetChecked(!m_checked, true);
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

        return Wnd::OnInputEvent(event);
    }

    void CheckBox::OnRender(ID2D1RenderTarget* target)
    {
        if (target == nullptr)
        {
            return;
        }

        if (!m_brush)
        {
            target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_brush);
        }

        // Modern flat look: rounded box, accent-filled when checked (with a
        // white tick), a subtle border otherwise that tints to the accent on
        // hover. Accent is the shared UI blue used across the controls.
        const D2D1_COLOR_F accent = m_enabled ? D2D1::ColorF(0.26f, 0.55f, 0.96f, 1.0f)
                                              : D2D1::ColorF(0.34f, 0.40f, 0.50f, 1.0f);
        D2D1_RECT_F box = BoxRect();
        D2D1_ROUNDED_RECT rbox = D2D1::RoundedRect(box, 3.0f, 3.0f);

        if (m_checked)
        {
            m_brush->SetColor(m_pressed ? D2D1::ColorF(0.20f, 0.46f, 0.84f, 1.0f)
                                        : (m_hovered ? D2D1::ColorF(0.34f, 0.62f, 1.0f, 1.0f) : accent));
            target->FillRoundedRectangle(rbox, m_brush.Get());

            float pad = 3.5f;
            D2D1_POINT_2F p1 = D2D1::Point2F(box.left + pad, (box.top + box.bottom) * 0.5f);
            D2D1_POINT_2F p2 = D2D1::Point2F(box.left + kBoxSize * 0.42f, box.bottom - pad);
            D2D1_POINT_2F p3 = D2D1::Point2F(box.right - pad, box.top + pad);
            m_brush->SetColor(m_enabled ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)
                                        : D2D1::ColorF(0.82f, 0.86f, 0.92f, 1.0f));
            target->DrawLine(p1, p2, m_brush.Get(), 2.0f);
            target->DrawLine(p2, p3, m_brush.Get(), 2.0f);
        }
        else
        {
            m_brush->SetColor(m_pressed ? D2D1::ColorF(0.24f, 0.25f, 0.28f, 1.0f) :
                (m_hovered ? D2D1::ColorF(0.25f, 0.27f, 0.31f, 1.0f) : D2D1::ColorF(0.17f, 0.18f, 0.20f, 1.0f)));
            target->FillRoundedRectangle(rbox, m_brush.Get());

            m_brush->SetColor((m_hovered && m_enabled) ? accent :
                (m_enabled ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.34f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f)));
            target->DrawRoundedRectangle(rbox, m_brush.Get(), 1.25f);
        }

        m_label.OnRender(target);

        Wnd::OnRender(target);
    }
}
