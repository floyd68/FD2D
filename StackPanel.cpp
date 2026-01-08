#include "StackPanel.h"

namespace FD2D
{
    StackPanel::StackPanel()
        : Panel()
    {
    }

    StackPanel::StackPanel(const std::wstring& name, Orientation orientation)
        : Panel(name)
        , m_orientation(orientation)
    {
    }

    void StackPanel::SetOrientation(Orientation o)
    {
        m_orientation = o;
    }

    Size StackPanel::Measure(Size available)
    {
        float main = 0.0f;
        float cross = 0.0f;

        for (auto& child : ChildrenInOrder())
        {
            if (child)
            {
                Size s = child->Measure(available);
                if (m_orientation == Orientation::Vertical)
                {
                    main += s.h;
                    cross = (std::max)(cross, s.w);
                }
                else
                {
                    main += s.w;
                    cross = (std::max)(cross, s.h);
                }

                main += m_spacing;
            }
        }

        if (m_orientation == Orientation::Vertical)
        {
            m_desired = { cross, main > 0.0f ? main - m_spacing : 0.0f };
        }
        else
        {
            m_desired = { main > 0.0f ? main - m_spacing : 0.0f, cross };
        }

        // Include this panel's padding and margin so scroll containers compute correct content extents.
        m_desired.w += 2.0f * m_padding + 2.0f * m_margin;
        m_desired.h += 2.0f * m_padding + 2.0f * m_margin;

        return m_desired;
    }

    void StackPanel::Arrange(Rect r)
    {
        Rect inset = Inset(r, m_margin);
        Rect childArea = Inset(inset, m_padding);
        float offset = (m_orientation == Orientation::Vertical) ? childArea.y : childArea.x;

        for (auto& child : ChildrenInOrder())
        {
            if (!child)
            {
                continue;
            }

            Size desired = child->Measure({ childArea.w, childArea.h });

            if (m_orientation == Orientation::Vertical)
            {
                Rect childRect { childArea.x, offset, childArea.w, desired.h };
                child->Arrange(childRect);
                offset += desired.h + m_spacing;
            }
            else
            {
                Rect childRect { offset, childArea.y, desired.w, childArea.h };
                child->Arrange(childRect);
                offset += desired.w + m_spacing;
            }
        }

        m_bounds = r;
        m_layoutRect = ToD2D(r);
    }
}

