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
        // OverlayPanel은 자식들의 최대 크기를 사용하되, available size를 초과하지 않음
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

        // available size를 초과하지 않도록 제한
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
        for (auto& kv : Children())
        {
            if (kv.second)
            {
                kv.second->Arrange(finalRect);
            }
        }

        m_bounds = finalRect;
        m_layoutRect = ToD2D(finalRect);
    }
}

