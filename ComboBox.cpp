#include "ComboBox.h"
#include <algorithm>

namespace FD2D
{
    ComboBox::ComboBox()
        : Wnd()
    {
    }

    ComboBox::ComboBox(const std::wstring& name)
        : Wnd(name)
    {
    }

    Size ComboBox::Measure(Size available)
    {
        UNREFERENCED_PARAMETER(available);
        m_desired = { 160.0f + 2.0f * m_margin, kItemHeight + 2.0f * m_margin };
        return m_desired;
    }

    void ComboBox::Arrange(Rect finalRect)
    {
        Wnd::Arrange(finalRect);
        D2D1_RECT_F textRect = LayoutRect();
        textRect.right -= kArrowWidth;
        m_text.SetRect(textRect);
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

    bool ComboBox::HitTestBox(const POINT& pt) const
    {
        const auto& rect = LayoutRect();
        return pt.x >= rect.left && pt.x <= rect.right && pt.y >= rect.top && pt.y <= rect.bottom;
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
                if (event.point.x >= dd.left && event.point.x <= dd.right &&
                    event.point.y >= dd.top && event.point.y <= dd.bottom)
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
            return m_hoveredBox || m_open;
        }
        case InputEventType::MouseDown:
        {
            if (event.button != MouseButton::Left || !event.hasPoint)
            {
                break;
            }

            if (m_open)
            {
                D2D1_RECT_F dd = DropdownRect();
                bool insideDropdown = event.point.x >= dd.left && event.point.x <= dd.right &&
                    event.point.y >= dd.top && event.point.y <= dd.bottom;
                if (insideDropdown)
                {
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

    void ComboBox::OnRender(ID2D1RenderTarget* target)
    {
        if (target == nullptr)
        {
            return;
        }

        if (!m_brush)
        {
            target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_brush);
        }

        const auto& rect = LayoutRect();
        m_brush->SetColor(m_hoveredBox || m_open ? D2D1::ColorF(0.26f, 0.26f, 0.29f, 1.0f) : D2D1::ColorF(0.18f, 0.18f, 0.20f, 1.0f));
        target->FillRectangle(rect, m_brush.Get());
        m_brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
        target->DrawRectangle(rect, m_brush.Get(), 1.0f);

        m_text.SetText(SelectedText());
        m_text.OnRender(target);

        // Arrow glyph.
        float ax = rect.right - kArrowWidth * 0.5f;
        float ay = (rect.top + rect.bottom) * 0.5f;
        D2D1_POINT_2F p1 = D2D1::Point2F(ax - 4.0f, ay - 2.0f);
        D2D1_POINT_2F p2 = D2D1::Point2F(ax + 4.0f, ay - 2.0f);
        D2D1_POINT_2F p3 = D2D1::Point2F(ax, ay + 3.0f);
        target->DrawLine(p1, p3, m_brush.Get(), 1.5f);
        target->DrawLine(p2, p3, m_brush.Get(), 1.5f);

        if (m_open && !m_items.empty())
        {
            D2D1_RECT_F dd = DropdownRect();
            m_brush->SetColor(D2D1::ColorF(0.14f, 0.14f, 0.16f, 1.0f));
            target->FillRectangle(dd, m_brush.Get());
            m_brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
            target->DrawRectangle(dd, m_brush.Get(), 1.0f);

            int visible = (std::min)(kMaxVisibleItems, static_cast<int>(m_items.size()));
            for (int i = 0; i < visible; ++i)
            {
                D2D1_RECT_F itemRect = ItemRect(static_cast<size_t>(i));
                if (i == m_hoveredItem)
                {
                    m_brush->SetColor(D2D1::ColorF(0.30f, 0.55f, 0.85f, 0.6f));
                    target->FillRectangle(itemRect, m_brush.Get());
                }

                Text itemText;
                D2D1_RECT_F textRect = itemRect;
                textRect.left += 6.0f;
                itemText.SetRect(textRect);
                itemText.SetText(m_items[static_cast<size_t>(i)]);
                itemText.OnRender(target);
            }
        }

        Wnd::OnRender(target);
    }
}
