#include "Panel.h"

namespace FD2D
{
    Panel::Panel()
        : Wnd()
    {
    }

    Panel::Panel(const std::wstring& name)
        : Wnd(name)
    {
    }

    void Panel::SetSpacing(float spacing)
    {
        m_spacing = spacing;
    }

    float Panel::Spacing() const
    {
        return m_spacing;
    }

    Size Panel::Measure(Size available)
    {
        // Base Panel defers to children Measure pass; default desired is max of children.
        Size maxSize {};
        for (auto& kv : Children())
        {
            if (kv.second)
            {
                Size s = kv.second->Measure(available);
                maxSize.w = (std::max)(maxSize.w, s.w);
                maxSize.h = (std::max)(maxSize.h, s.h);
            }
        }

        m_desired = maxSize;
        return m_desired;
    }

    void Panel::Arrange(Rect finalRect)
    {
        m_bounds = finalRect;
        m_layoutRect = ToD2D(finalRect);
        for (auto& kv : Children())
        {
            if (kv.second)
            {
                kv.second->Arrange(finalRect);
            }
        }
    }
}

