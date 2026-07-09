#include "OverlayPanel.h"

namespace FD2D
{
    OverlayPanel::OverlayPanel()
        : Panel()
    {
    }

    OverlayPanel::OverlayPanel(const std::wstring& name)
        : Panel(name)
    {
    }

    Size OverlayPanel::Measure(Size available)
    {
        // OverlayPanel uses the maximum size of its children, but does not exceed the available size
        Size maxSize {};
        for (auto& child : ChildrenInOrder())
        {
            if (child)
            {
                Size s = child->Measure(available);
                maxSize.w = (std::max)(maxSize.w, s.w);
                maxSize.h = (std::max)(maxSize.h, s.h);
            }
        }

        // Clamp to not exceed the available size
        if (available.w > 0.0f && maxSize.w > available.w)
        {
            maxSize.w = available.w;
        }
        if (available.h > 0.0f && maxSize.h > available.h)
        {
            maxSize.h = available.h;
        }

        m_desired = maxSize;
        return m_desired;
    }

    void OverlayPanel::Arrange(Rect finalRect)
    {
        for (auto& child : ChildrenInOrder())
        {
            if (child)
            {
                child->Arrange(finalRect);
            }
        }

        m_bounds = finalRect;
        m_layoutRect = ToD2D(finalRect);
    }
}

