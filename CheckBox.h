#pragma once

#include <functional>
#include "Wnd.h"
#include "Text.h"

namespace FD2D
{
    // Simple labeled checkbox (box + checkmark + label).
    class CheckBox : public Wnd
    {
    public:
        using CheckedChangedHandler = std::function<void(bool checked)>;

        CheckBox();
        explicit CheckBox(const std::wstring& name);

        Size Measure(Size available) override;
        void Arrange(Rect finalRect) override;

        void SetLabel(const std::wstring& text);
        void SetChecked(bool checked, bool notify = false);
        bool Checked() const { return m_checked; }

        void OnCheckedChanged(CheckedChangedHandler handler);

        bool OnInputEvent(const InputEvent& event) override;
        void OnRender(ID2D1RenderTarget* target) override;

    private:
        bool HitTest(const POINT& pt) const;
        D2D1_RECT_F BoxRect() const;

        bool m_checked { false };
        bool m_hovered { false };
        bool m_pressed { false };

        Text m_label {};
        CheckedChangedHandler m_changed {};

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush {};

        static constexpr float kBoxSize = 16.0f;
        static constexpr float kLabelGap = 8.0f;
    };
}
