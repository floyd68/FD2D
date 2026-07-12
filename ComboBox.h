#pragma once

#include <functional>
#include <string>
#include <vector>
#include "Wnd.h"
#include "Text.h"

namespace FD2D
{
    // Simple drop-down list. The closed box is drawn in OnRender; the open
    // item list is drawn in OnRenderOverlay so it paints above sibling
    // controls in the same panel without a fullscreen scrim over view panes.
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

        // Dropdown list panel fill (rgb + alpha). Alpha 1 = opaque, 0 = invisible.
        // Default is a dark fill at 25% transparency (alpha 0.75).
        void SetDropdownBackground(const D2D1_COLOR_F& color);
        const D2D1_COLOR_F& DropdownBackground() const { return m_dropdownBackground; }

        bool HasInputOverlay() const override;
        bool OnInputEvent(const InputEvent& event) override;
        void OnRender(ID2D1RenderTarget* target) override;
        void OnRenderOverlay(ID2D1RenderTarget* target) override;

    private:
        bool HitTestBox(const POINT& pt) const;
        bool HitTestDropdown(const POINT& pt) const;
        D2D1_RECT_F ItemRect(size_t index) const;
        D2D1_RECT_F DropdownRect() const;
        void Close();
        void EnsureBrush(ID2D1RenderTarget* target);

        std::vector<std::wstring> m_items {};
        int m_selectedIndex { -1 };
        int m_hoveredItem { -1 };
        bool m_open { false };
        bool m_hoveredBox { false };

        Text m_text {};
        SelectionChangedHandler m_changed {};
        D2D1_COLOR_F m_dropdownBackground { 0.10f, 0.10f, 0.12f, 0.75f };

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush {};

        static constexpr float kItemHeight = 22.0f;
        static constexpr float kArrowWidth = 18.0f;
        static constexpr int kMaxVisibleItems = 8;
    };
}
