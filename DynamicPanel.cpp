#include "DynamicPanel.h"

#include <algorithm>

namespace FD2D
{
    namespace
    {
        // "Unconstrained" width sentinel: anything at or beyond this is treated
        // as an infinite-width probe (avoids FLT_MAX arithmetic overflow).
        constexpr float kInfWidth = 1.0e9f;
    }

    DynamicPanel::DynamicPanel()
        : Panel()
    {
    }

    DynamicPanel::DynamicPanel(const std::wstring& name)
        : Panel(name)
    {
    }

    void DynamicPanel::SetGaps(float horizontal, float vertical)
    {
        m_hgap = horizontal;
        m_vgap = vertical;
        Invalidate();
    }

    void DynamicPanel::SetForceSingleColumn(bool force)
    {
        if (m_forceSingle == force)
        {
            return;
        }
        m_forceSingle = force;
        Invalidate();
    }

    float DynamicPanel::LayoutRows(float contentWidth, std::vector<Placed>* out,
                                   float* outUsedWidth) const
    {
        float x = 0.0f, y = 0.0f, rowH = 0.0f, usedW = 0.0f;
        bool rowEmpty = true;
        for (const auto& child : ChildrenInOrder())
        {
            if (!child)
            {
                continue;
            }
            // Hand the child the available width so a nested DynamicPanel can
            // reflow itself to fit.
            const Size s = child->Measure({ contentWidth, kInfWidth });
            // Start a new row when this child won't fit (unless the row is
            // empty - an over-wide child still gets its own row rather than
            // vanishing), or unconditionally in single-column (compact) mode.
            if (!rowEmpty && (m_forceSingle || (x + m_hgap + s.w) > contentWidth))
            {
                usedW = (std::max)(usedW, x); // widest row so far
                x = 0.0f;
                y += rowH + m_vgap;
                rowH = 0.0f;
                rowEmpty = true;
            }
            if (!rowEmpty)
            {
                x += m_hgap;
            }
            if (out != nullptr)
            {
                out->push_back({ child.get(), x, y, s.w, s.h });
            }
            x += s.w;
            rowH = (std::max)(rowH, s.h);
            rowEmpty = false;
        }
        usedW = (std::max)(usedW, x); // last row
        if (outUsedWidth != nullptr)
        {
            *outUsedWidth = usedW;
        }
        return y + rowH;
    }

    Size DynamicPanel::Measure(Size available)
    {
        const float chrome = 2.0f * m_padding + 2.0f * m_margin;
        const bool unconstrained = !(available.w < kInfWidth);
        const float innerW = unconstrained ? kInfWidth
                                           : (std::max)(0.0f, available.w - chrome);

        float usedW = 0.0f;
        const float totalH = LayoutRows(innerW, nullptr, &usedW);

        // Report the width the content actually USES, not the width we were
        // offered - otherwise a nested panel would greedily claim its whole row
        // and push its siblings onto new rows. Capped at the offered width when
        // constrained.
        const float desiredW = unconstrained ? usedW : (std::min)(usedW, innerW);

        m_desired = { desiredW + chrome, totalH + chrome };
        return m_desired;
    }

    void DynamicPanel::Arrange(Rect finalRect)
    {
        const Rect inset = Inset(finalRect, m_margin);
        const Rect childArea = Inset(inset, m_padding);

        std::vector<Placed> placed;
        LayoutRows(childArea.w, &placed);
        for (const Placed& p : placed)
        {
            if (p.wnd == nullptr)
            {
                continue;
            }
            p.wnd->Arrange({ childArea.x + p.x, childArea.y + p.y, p.w, p.h });
        }

        m_bounds = finalRect;
        m_layoutRect = ToD2D(finalRect);
    }
}
