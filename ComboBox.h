#pragma once

#include <functional>
#include <memory>
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

        bool OnInputEvent(const InputEvent& event) override;
        void OnRender(ID2D1RenderTarget* target) override;

    protected:
        bool IsOverlayActive(OverlayLayer layer) const override;
        void OnRenderOverlay(ID2D1RenderTarget* target, OverlayLayer layer) override;
        bool OnOverlayInput(const InputEvent& event, OverlayLayer layer) override;

    private:
        // Tests inspect the popup item texts.
        friend struct ComboBoxTestAccess;

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
        // One Text per item, created by SetItems and reused by every popup
        // render. Like m_text they are not in the child tree, so EnsureBrush
        // resets their brushes on a new target generation (layouts are kept).
        std::vector<std::unique_ptr<Text>> m_itemTexts {};
        SelectionChangedHandler m_changed {};
        D2D1_COLOR_F m_dropdownBackground { 0.10f, 0.10f, 0.12f, 0.75f };

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush {};
        // Target generation the brushes were created under (see GraphicsResourceStamp).
        GraphicsResourceStamp m_brushStamp {};

        static constexpr float kItemHeight = 22.0f;
        static constexpr float kArrowWidth = 18.0f;
        static constexpr int kMaxVisibleItems = 8;
    };
}
