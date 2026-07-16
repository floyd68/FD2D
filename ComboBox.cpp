#include "ComboBox.h"
#include <algorithm>

namespace FD2D
{
    ComboBox::ComboBox()
        : Wnd()
    {
        SetContentMargin(8.0f, 2.0f);
        SetContentAlign(AlignH::Start, AlignV::Center);
    }

    ComboBox::ComboBox(const std::wstring& name)
        : Wnd(name)
    {
        SetContentMargin(8.0f, 2.0f);
        SetContentAlign(AlignH::Start, AlignV::Center);
    }

    Size ComboBox::Measure(Size available)
    {
        Size textSize = m_text.Measure(available);
        float height = (std::max)(kItemHeight, textSize.h + m_contentMargin.Vertical());
        m_desired = { 160.0f + 2.0f * m_margin, height + 2.0f * m_margin };
        return m_desired;
    }

    void ComboBox::Arrange(Rect finalRect)
    {
        Wnd::Arrange(finalRect);

        // The arrow glyph owns the trailing chrome strip; content layout only
        // applies inside the remaining text area.
        Rect textBounds = BoundsRect();
        textBounds.w = (std::max)(0.0f, textBounds.w - kArrowWidth);

        Size textSize = m_text.Measure({ 0.0f, 0.0f });
        m_text.SetRect(ToD2D(ContentRectFor(textBounds, textSize)));
    }

    void ComboBox::SetItems(std::vector<std::wstring> items)
    {
        m_items = std::move(items);
        if (m_selectedIndex >= static_cast<int>(m_items.size()))
        {
            m_selectedIndex = m_items.empty() ? -1 : 0;
        }
    }

    void ComboBox::SetSelectedIndex(int index, bool notify)
    {
        if (index < -1 || index >= static_cast<int>(m_items.size()))
        {
            return;
        }
        bool changed = (index != m_selectedIndex);
        m_selectedIndex = index;
        if (changed && notify && m_changed)
        {
            m_changed(m_selectedIndex);
        }
        Invalidate();
    }

    std::wstring ComboBox::SelectedText() const
    {
        if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_items.size()))
        {
            return m_items[static_cast<size_t>(m_selectedIndex)];
        }
        return L"";
    }

    void ComboBox::OnSelectionChanged(SelectionChangedHandler handler)
    {
        m_changed = std::move(handler);
    }

    void ComboBox::SetDropdownBackground(const D2D1_COLOR_F& color)
    {
        m_dropdownBackground = color;
        Invalidate();
    }

    bool ComboBox::HitTestBox(const POINT& pt) const
    {
        const auto& rect = LayoutRect();
        return pt.x >= rect.left && pt.x <= rect.right && pt.y >= rect.top && pt.y <= rect.bottom;
    }

    bool ComboBox::HitTestDropdown(const POINT& pt) const
    {
        if (!m_open || m_items.empty())
        {
            return false;
        }
        D2D1_RECT_F dd = DropdownRect();
        return pt.x >= dd.left && pt.x <= dd.right && pt.y >= dd.top && pt.y <= dd.bottom;
    }

    D2D1_RECT_F ComboBox::DropdownRect() const
    {
        const auto& rect = LayoutRect();
        int visible = (std::min)(kMaxVisibleItems, static_cast<int>(m_items.size()));
        float height = kItemHeight * static_cast<float>(visible);
        return D2D1::RectF(rect.left, rect.bottom, rect.right, rect.bottom + height);
    }

    D2D1_RECT_F ComboBox::ItemRect(size_t index) const
    {
        D2D1_RECT_F dd = DropdownRect();
        float top = dd.top + kItemHeight * static_cast<float>(index);
        return D2D1::RectF(dd.left, top, dd.right, top + kItemHeight);
    }

    void ComboBox::Close()
    {
        if (m_open)
        {
            m_open = false;
            m_hoveredItem = -1;
            Invalidate();
        }
    }

    bool ComboBox::HasInputOverlay() const
    {
        return m_open && !m_items.empty();
    }

    bool ComboBox::OnInputEvent(const InputEvent& event)
    {
        switch (event.type)
        {
        case InputEventType::MouseMove:
        {
            if (!event.hasPoint)
            {
                return false;
            }
            bool prevHover = m_hoveredBox;
            m_hoveredBox = HitTestBox(event.point);

            if (m_open)
            {
                int newHover = -1;
                D2D1_RECT_F dd = DropdownRect();
                if (HitTestDropdown(event.point))
                {
                    newHover = static_cast<int>((event.point.y - dd.top) / kItemHeight);
                }
                if (newHover != m_hoveredItem)
                {
                    m_hoveredItem = newHover;
                    Invalidate();
                }
            }

            if (m_hoveredBox != prevHover)
            {
                Invalidate();
            }
            return m_hoveredBox || HitTestDropdown(event.point) || m_open;
        }
        case InputEventType::MouseDown:
        {
            if (event.button != MouseButton::Left || !event.hasPoint)
            {
                break;
            }

            if (m_open)
            {
                if (HitTestDropdown(event.point))
                {
                    D2D1_RECT_F dd = DropdownRect();
                    int index = static_cast<int>((event.point.y - dd.top) / kItemHeight);
                    if (index >= 0 && index < static_cast<int>(m_items.size()))
                    {
                        SetSelectedIndex(index, true);
                    }
                    Close();
                    return true;
                }

                if (HitTestBox(event.point))
                {
                    Close();
                    return true;
                }

                // Click was outside the combo entirely: close but let the click
                // continue to be routed to whatever is underneath it.
                Close();
                return false;
            }

            if (HitTestBox(event.point))
            {
                m_open = !m_items.empty();
                RequestFocus();
                Invalidate();
                return true;
            }
            break;
        }
        case InputEventType::MouseWheel:
        {
            if (!m_hoveredBox && !m_open)
            {
                break;
            }
            if (!m_items.empty())
            {
                int dir = (event.wheelDelta > 0) ? -1 : 1;
                int next = m_selectedIndex + dir;
                next = (std::max)(0, (std::min)(static_cast<int>(m_items.size()) - 1, next));
                SetSelectedIndex(next, true);
            }
            return true;
        }
        case InputEventType::MouseLeave:
        {
            if (m_hoveredBox)
            {
                m_hoveredBox = false;
                Invalidate();
            }
            break;
        }
        default:
            break;
        }

        return Wnd::OnInputEvent(event);
    }

    void ComboBox::EnsureBrush(ID2D1RenderTarget* target)
    {
        if (!m_brush)
        {
            target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_brush);
        }
    }

    void ComboBox::OnRender(ID2D1RenderTarget* target)
    {
        if (target == nullptr)
        {
            return;
        }

        EnsureBrush(target);

        const bool active = m_hoveredBox || m_open;
        const auto& rect = LayoutRect();
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, 4.0f, 4.0f);
        m_brush->SetColor(active ? D2D1::ColorF(0.25f, 0.27f, 0.31f, 1.0f) : D2D1::ColorF(0.17f, 0.18f, 0.20f, 1.0f));
        target->FillRoundedRectangle(rr, m_brush.Get());
        m_brush->SetColor(active ? D2D1::ColorF(0.26f, 0.55f, 0.96f, 0.90f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f));
        target->DrawRoundedRectangle(rr, m_brush.Get(), 1.0f);

        m_text.SetText(SelectedText());
        m_text.OnRender(target);

        // Chevron glyph, accent-tinted while active.
        m_brush->SetColor(active ? D2D1::ColorF(0.62f, 0.80f, 1.0f, 1.0f) : D2D1::ColorF(0.82f, 0.82f, 0.86f, 1.0f));
        float ax = rect.right - kArrowWidth * 0.5f;
        float ay = (rect.top + rect.bottom) * 0.5f;
        D2D1_POINT_2F p1 = D2D1::Point2F(ax - 4.0f, ay - 2.0f);
        D2D1_POINT_2F p2 = D2D1::Point2F(ax + 4.0f, ay - 2.0f);
        D2D1_POINT_2F p3 = D2D1::Point2F(ax, ay + 3.0f);
        target->DrawLine(p1, p3, m_brush.Get(), 1.5f);
        target->DrawLine(p2, p3, m_brush.Get(), 1.5f);

        Wnd::OnRender(target);
    }

    void ComboBox::OnRenderOverlay(ID2D1RenderTarget* target)
    {
        // Draw the list in the overlay pass so it sits above sibling controls
        // in the same strip, without a fullscreen scrim over the view panes.
        if (target == nullptr || !m_open || m_items.empty())
        {
            Wnd::OnRenderOverlay(target);
            return;
        }

        EnsureBrush(target);

        D2D1_RECT_F dd = DropdownRect();
        D2D1_ROUNDED_RECT ddr = D2D1::RoundedRect(dd, 4.0f, 4.0f);
        m_brush->SetColor(D2D1::ColorF(0.13f, 0.14f, 0.16f, 0.98f));
        target->FillRoundedRectangle(ddr, m_brush.Get());
        m_brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f));
        target->DrawRoundedRectangle(ddr, m_brush.Get(), 1.0f);

        int visible = (std::min)(kMaxVisibleItems, static_cast<int>(m_items.size()));
        for (int i = 0; i < visible; ++i)
        {
            D2D1_RECT_F itemRect = ItemRect(static_cast<size_t>(i));
            if (i == m_hoveredItem)
            {
                D2D1_RECT_F hi = itemRect;
                hi.left += 2.0f; hi.right -= 2.0f;
                m_brush->SetColor(D2D1::ColorF(0.26f, 0.55f, 0.96f, 0.55f));
                target->FillRoundedRectangle(D2D1::RoundedRect(hi, 3.0f, 3.0f), m_brush.Get());
            }

            Text itemText;
            itemText.SetText(m_items[static_cast<size_t>(i)]);
            Size itemTextSize = itemText.Measure({ 0.0f, 0.0f });
            Rect itemBounds = FromD2D(itemRect);
            Thickness itemMargin = m_contentMargin;
            itemMargin.right = (std::max)(itemMargin.right, 4.0f);
            itemText.SetRect(ToD2D(LayoutContent(itemBounds, itemTextSize, itemMargin,
                AlignH::Start, AlignV::Center)));
            itemText.OnRender(target);
        }

        Wnd::OnRenderOverlay(target);
    }
}
