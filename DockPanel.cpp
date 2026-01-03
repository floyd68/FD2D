#include "DockPanel.h"

namespace FD2D
{
    DockPanel::DockPanel()
        : Panel()
    {
    }

    DockPanel::DockPanel(const std::wstring& name)
        : Panel(name)
    {
    }

    void DockPanel::SetChildDock(const std::shared_ptr<Wnd>& child, Dock dock)
    {
        if (!child)
        {
            return;
        }

        m_docks[child.get()] = dock;
        m_order.push_back(child);
    }

    Size DockPanel::Measure(Size available)
    {
        // Measure children with remaining space; conservative return = available.
        Size remaining = available;
        for (auto& child : m_order)
        {
            if (!child)
            {
                continue;
            }

            Dock dock = Dock::Fill;
            auto it = m_docks.find(child.get());
            if (it != m_docks.end())
            {
                dock = it->second;
            }

            Size childAvail = remaining;
            child->Measure(childAvail);

            // For Auto measure we'd shrink remaining, but we just measure and keep available as-is.
        }

        m_desired = available;
        return m_desired;
    }

    void DockPanel::Arrange(Rect finalRect)
    {
        Rect rect = finalRect;

        for (auto& child : m_order)
        {
            if (!child)
            {
                continue;
            }

            Dock dock = Dock::Fill;
            auto it = m_docks.find(child.get());
            if (it != m_docks.end())
            {
                dock = it->second;
            }

            switch (dock)
            {
            case Dock::Left:
            {
                Size desired = child->Measure({ rect.w, rect.h });
                Rect childRect { rect.x, rect.y, desired.w, rect.h };
                child->Arrange(childRect);
                rect.x += desired.w;
                rect.w = (std::max)(0.0f, rect.w - desired.w);
                break;
            }
            case Dock::Right:
            {
                Size desired = child->Measure({ rect.w, rect.h });
                Rect childRect { rect.x + rect.w - desired.w, rect.y, desired.w, rect.h };
                child->Arrange(childRect);
                rect.w = (std::max)(0.0f, rect.w - desired.w);
                break;
            }
            case Dock::Top:
            {
                Size desired = child->Measure({ rect.w, rect.h });
                Rect childRect { rect.x, rect.y, rect.w, desired.h };
                child->Arrange(childRect);
                rect.y += desired.h;
                rect.h = (std::max)(0.0f, rect.h - desired.h);
                break;
            }
            case Dock::Bottom:
            {
                Size desired = child->Measure({ rect.w, rect.h });
                Rect childRect { rect.x, rect.y + rect.h - desired.h, rect.w, desired.h };
                child->Arrange(childRect);
                rect.h = (std::max)(0.0f, rect.h - desired.h);
                break;
            }
            case Dock::Fill:
            default:
            {
                child->Arrange(rect);
                // After fill we stop docking subsequent children.
                rect = { 0, 0, 0, 0 };
                break;
            }
            }
        }

        m_bounds = finalRect;
        m_layoutRect = ToD2D(finalRect);
    }
}

