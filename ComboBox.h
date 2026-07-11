#pragma once

#include <functional>
#include <string>
#include <vector>
#include "Wnd.h"
#include "Text.h"

namespace FD2D
{
    // Simple drop-down list. When open, the item list is drawn as an overlay
    // directly below the box as part of this control's own OnRender pass, so for
    // correct visual layering the ComboBox should be one of the last children
    // added to its parent panel (painter's-algorithm z-order, no separate popup
    // surface is created).
    class ComboBox : public Wnd
    {
    public:
        using SelectionChangedHandler = std::function<void(int index)>;

        ComboBox();
        explicit ComboBox(const std::wstring& name);

        Size Measure(Size available) override;
        void Arrange(Rect finalRect) override;

        void SetItems(std::vector<std::wstring> items);
        void SetSelectedIndex(int index, bool notify = false);
        int SelectedIndex() const { return m_selectedIndex; }
        std::wstring SelectedText() const;

        void OnSelectionChanged(SelectionChangedHandler handler);

        bool OnInputEvent(const InputEvent& event) override;
        void OnRender(ID2D1RenderTarget* target) override;

    private:
        bool HitTestBox(const POINT& pt) const;
        D2D1_RECT_F ItemRect(size_t index) const;
        D2D1_RECT_F DropdownRect() const;
        void Close();

        std::vector<std::wstring> m_items {};
        int m_selectedIndex { -1 };
        int m_hoveredItem { -1 };
        bool m_open { false };
        bool m_hoveredBox { false };

        Text m_text {};
        SelectionChangedHandler m_changed {};

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush {};

        static constexpr float kItemHeight = 22.0f;
        static constexpr float kArrowWidth = 18.0f;
        static constexpr int kMaxVisibleItems = 8;
    };
}
