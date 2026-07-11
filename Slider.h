#pragma once

#include <functional>
#include "Wnd.h"
#include "Text.h"

namespace FD2D
{
    // Simple horizontal value slider (drag thumb, click-on-track, mouse wheel).
    class Slider : public Wnd
    {
    public:
        using ValueChangedHandler = std::function<void(float value)>;

        Slider();
        explicit Slider(const std::wstring& name);

        Size Measure(Size available) override;
        void Arrange(Rect finalRect) override;

        void SetRange(float minValue, float maxValue);
        void SetValue(float value, bool notify = false);
        float Value() const { return m_value; }

        void SetStep(float step) { m_step = step; }
        void SetLabel(const std::wstring& text);
        void SetShowValueText(bool show) { m_showValueText = show; }
        void SetValueFormatter(std::function<std::wstring(float)> formatter);

        void OnValueChanged(ValueChangedHandler handler);

        bool OnInputEvent(const InputEvent& event) override;
        void OnRender(ID2D1RenderTarget* target) override;

    private:
        float ClampValue(float v) const;
        float RatioForValue(float v) const;
        float ValueForRatio(float ratio) const;
        D2D1_RECT_F TrackRect() const;
        D2D1_RECT_F ThumbRect() const;
        bool HitTestThumb(const POINT& pt) const;
        void SetValueFromPoint(const POINT& pt);

        float m_min { 0.0f };
        float m_max { 1.0f };
        float m_value { 0.0f };
        float m_step { 0.0f };

        bool m_hovered { false };
        bool m_dragging { false };
        bool m_showValueText { true };

        Text m_label {};
        std::function<std::wstring(float)> m_formatter {};
        ValueChangedHandler m_changed {};

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush {};

        static constexpr float kTrackHeight = 4.0f;
        static constexpr float kThumbRadius = 7.0f;
        static constexpr float kLabelHeight = 16.0f;
    };
}
