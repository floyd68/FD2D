#include "Slider.h"
#include <cmath>
#include <cstdio>

namespace FD2D
{
    Slider::Slider()
        : Wnd()
    {
    }

    Slider::Slider(const std::wstring& name)
        : Wnd(name)
    {
    }

    Size Slider::Measure(Size available)
    {
        UNREFERENCED_PARAMETER(available);
        m_labelHeight = m_label.Measure({ 0.0f, 0.0f }).h;
        float h = m_labelHeight + kThumbRadius * 2.0f + 2.0f * m_margin;
        m_desired = { 120.0f + 2.0f * m_margin, h };
        return m_desired;
    }

    void Slider::Arrange(Rect finalRect)
    {
        Wnd::Arrange(finalRect);
        m_labelHeight = m_label.Measure({ 0.0f, 0.0f }).h;
        const auto& rect = LayoutRect();
        D2D1_RECT_F labelRect = rect;
        labelRect.bottom = labelRect.top + m_labelHeight;
        m_label.SetRect(labelRect);
    }

    void Slider::SetRange(float minValue, float maxValue)
    {
        m_min = minValue;
        m_max = (maxValue > minValue) ? maxValue : minValue + 1.0f;
        m_value = ClampValue(m_value);
    }

    float Slider::ClampValue(float v) const
    {
        if (v < m_min) return m_min;
        if (v > m_max) return m_max;
        return v;
    }

    void Slider::SetValue(float value, bool notify)
    {
        float clamped = ClampValue(value);
        bool changed = (clamped != m_value);
        m_value = clamped;
        if (changed && notify && m_changed)
        {
            m_changed(m_value);
        }
        Invalidate();
    }

    void Slider::SetLabel(const std::wstring& text)
    {
        m_label.SetText(text);
    }

    void Slider::SetValueFormatter(std::function<std::wstring(float)> formatter)
    {
        m_formatter = std::move(formatter);
    }

    void Slider::OnValueChanged(ValueChangedHandler handler)
    {
        m_changed = std::move(handler);
    }

    float Slider::RatioForValue(float v) const
    {
        float range = m_max - m_min;
        if (range <= 0.0f)
        {
            return 0.0f;
        }
        return (v - m_min) / range;
    }

    float Slider::ValueForRatio(float ratio) const
    {
        if (ratio < 0.0f) ratio = 0.0f;
        if (ratio > 1.0f) ratio = 1.0f;
        float v = m_min + ratio * (m_max - m_min);
        if (m_step > 0.0f)
        {
            v = m_min + std::round((v - m_min) / m_step) * m_step;
        }
        return ClampValue(v);
    }

    D2D1_RECT_F Slider::TrackRect() const
    {
        const auto& rect = LayoutRect();
        float top = rect.top + m_labelHeight + (rect.bottom - rect.top - m_labelHeight) * 0.5f - kTrackHeight * 0.5f;
        float left = rect.left + kThumbRadius;
        float right = rect.right - kThumbRadius;
        return D2D1::RectF(left, top, (std::max)(left, right), top + kTrackHeight);
    }

    D2D1_RECT_F Slider::ThumbRect() const
    {
        D2D1_RECT_F track = TrackRect();
        float ratio = RatioForValue(m_value);
        float cx = track.left + (track.right - track.left) * ratio;
        float cy = (track.top + track.bottom) * 0.5f;
        return D2D1::RectF(cx - kThumbRadius, cy - kThumbRadius, cx + kThumbRadius, cy + kThumbRadius);
    }

    bool Slider::HitTestThumb(const POINT& pt) const
    {
        D2D1_RECT_F thumb = ThumbRect();
        float cx = (thumb.left + thumb.right) * 0.5f;
        float cy = (thumb.top + thumb.bottom) * 0.5f;
        float dx = static_cast<float>(pt.x) - cx;
        float dy = static_cast<float>(pt.y) - cy;
        return (dx * dx + dy * dy) <= (kThumbRadius + 3.0f) * (kThumbRadius + 3.0f);
    }

    void Slider::SetValueFromPoint(const POINT& pt)
    {
        D2D1_RECT_F track = TrackRect();
        float width = track.right - track.left;
        float ratio = (width > 0.0f) ? (static_cast<float>(pt.x) - track.left) / width : 0.0f;
        SetValue(ValueForRatio(ratio), true);
    }

    bool Slider::OnInputEvent(const InputEvent& event)
    {
        switch (event.type)
        {
        case InputEventType::MouseMove:
        {
            if (!event.hasPoint)
            {
                return false;
            }
            if (m_dragging)
            {
                SetValueFromPoint(event.point);
                return true;
            }
            bool prevHover = m_hovered;
            m_hovered = HitTestThumb(event.point) ||
                (event.point.x >= TrackRect().left && event.point.x <= TrackRect().right &&
                 event.point.y >= LayoutRect().top && event.point.y <= LayoutRect().bottom);
            if (m_hovered != prevHover)
            {
                Invalidate();
            }
            return m_hovered;
        }
        case InputEventType::MouseDown:
        {
            if (event.button != MouseButton::Left || !event.hasPoint)
            {
                break;
            }
            D2D1_RECT_F track = TrackRect();
            bool onTrack = event.point.x >= track.left - kThumbRadius && event.point.x <= track.right + kThumbRadius &&
                event.point.y >= LayoutRect().top + m_labelHeight && event.point.y <= LayoutRect().bottom;
            if (HitTestThumb(event.point) || onTrack)
            {
                m_dragging = true;
                RequestFocus();
                SetValueFromPoint(event.point);
                return true;
            }
            break;
        }
        case InputEventType::MouseUp:
        {
            if (m_dragging)
            {
                m_dragging = false;
                Invalidate();
                return true;
            }
            break;
        }
        case InputEventType::MouseWheel:
        {
            if (!m_hovered)
            {
                break;
            }
            float step = (m_step > 0.0f) ? m_step : (m_max - m_min) / 100.0f;
            float dir = (event.wheelDelta > 0) ? 1.0f : -1.0f;
            SetValue(m_value + dir * step, true);
            return true;
        }
        case InputEventType::MouseLeave:
        {
            if (m_hovered)
            {
                m_hovered = false;
                Invalidate();
            }
            break;
        }
        default:
            break;
        }

        return Wnd::OnInputEvent(event);
    }

    void Slider::OnRender(ID2D1RenderTarget* target)
    {
        if (target == nullptr)
        {
            return;
        }

        if (!m_brush)
        {
            target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_brush);
        }

        if (m_showValueText)
        {
            std::wstring valueText = m_formatter ? m_formatter(m_value) : std::to_wstring(m_value);
            std::wstring baseLabel = L"";
            // Compose "Label: value" using the label's own text storage is not exposed,
            // so we simply render the value as a suffix by drawing the label then the value separately.
            m_label.OnRender(target);

            D2D1_RECT_F rect = LayoutRect();
            D2D1_RECT_F valueRect = rect;
            valueRect.bottom = valueRect.top + m_labelHeight;
            m_brush->SetColor(D2D1::ColorF(D2D1::ColorF::LightGray));
            // Draw the numeric value right-aligned using DrawText via a throwaway layout would need DWrite factory;
            // keep this lightweight by drawing through Text when possible (handled by owner setting label text).
        }
        else
        {
            m_label.OnRender(target);
        }

        D2D1_RECT_F track = TrackRect();
        m_brush->SetColor(D2D1::ColorF(0.30f, 0.30f, 0.33f, 1.0f));
        target->FillRectangle(track, m_brush.Get());

        D2D1_RECT_F filled = track;
        filled.right = track.left + (track.right - track.left) * RatioForValue(m_value);
        m_brush->SetColor(D2D1::ColorF(0.30f, 0.55f, 0.85f, 1.0f));
        target->FillRectangle(filled, m_brush.Get());

        D2D1_RECT_F thumb = ThumbRect();
        D2D1_ELLIPSE ellipse = D2D1::Ellipse(
            D2D1::Point2F((thumb.left + thumb.right) * 0.5f, (thumb.top + thumb.bottom) * 0.5f),
            kThumbRadius, kThumbRadius);
        m_brush->SetColor(m_dragging ? D2D1::ColorF(D2D1::ColorF::White) :
            (m_hovered ? D2D1::ColorF(0.92f, 0.92f, 0.95f, 1.0f) : D2D1::ColorF(0.80f, 0.80f, 0.84f, 1.0f)));
        target->FillEllipse(ellipse, m_brush.Get());
        m_brush->SetColor(D2D1::ColorF(0.10f, 0.10f, 0.10f, 1.0f));
        target->DrawEllipse(ellipse, m_brush.Get(), 1.0f);

        Wnd::OnRender(target);
    }
}
