#pragma once

#include <functional>
#include "Wnd.h"
#include "Text.h"

namespace FD2D
{
    class Button : public Wnd
    {
    public:
        using ClickHandler = std::function<void()>;

        Button();
        explicit Button(const std::wstring& name);

        Size Measure(Size available) override;
        void Arrange(Rect finalRect) override;
        void SetRect(const D2D1_RECT_F& rect);
        void SetLabel(const std::wstring& text);
        void SetColors(const D2D1_COLOR_F& normal, const D2D1_COLOR_F& hot, const D2D1_COLOR_F& pressed);
        void OnClick(ClickHandler handler);

        bool OnInputEvent(const InputEvent& event) override;
        void OnRender(ID2D1RenderTarget* target) override;

    private:
        bool HitTest(const POINT& pt) const;

        // Flat dark-surface defaults (a raised control that lightens on hover
        // and takes a slight accent-blue lean when pressed). Apps can still
        // override via SetColors.
        D2D1_COLOR_F m_colorNormal { D2D1::ColorF(0.20f, 0.21f, 0.24f, 1.0f) };
        D2D1_COLOR_F m_colorHot { D2D1::ColorF(0.27f, 0.29f, 0.34f, 1.0f) };
        D2D1_COLOR_F m_colorPressed { D2D1::ColorF(0.22f, 0.33f, 0.50f, 1.0f) };

        bool m_hovered { false };
        bool m_pressed { false };

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush {};
        Text m_label {};
        ClickHandler m_click {};
    };
}

