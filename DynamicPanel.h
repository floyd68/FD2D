#pragma once

#include "Panel.h"
#include "Layout.h"

#include <vector>

namespace FD2D
{
    // Responsive (flex-wrap) layout container, the CSS "flex-wrap: wrap" of the
    // panel family. Children flow left to right and WRAP to a new row when the
    // next child would overflow the available width, so a narrow layout reflows
    // into more rows instead of clipping past the right edge.
    //
    // - Measure(available) lays rows out against available.w and reports the
    //   width the content actually USES (never greedily the offered width, so a
    //   nested DynamicPanel packs beside its siblings) and the wrapped height.
    // - Children receive the available width as their measure hint, so a child
    //   that is itself a DynamicPanel reflows its own rows when space is tight
    //   (e.g. a 2-column group of controls collapsing to a single column).
    // - SetForceSingleColumn(true) stacks every child on its own row even where
    //   more would fit - a "compact mode" switch the application can drive from
    //   a top-level breakpoint (window width), keeping Measure and Arrange
    //   consistent instead of each nested panel guessing from its local width.
    class DynamicPanel : public Panel
    {
    public:
        DynamicPanel();
        explicit DynamicPanel(const std::wstring& name);

        // Gap between items on a row (horizontal) and between rows (vertical).
        void SetGaps(float horizontal, float vertical);
        float HorizontalGap() const { return m_hgap; }
        float VerticalGap() const { return m_vgap; }

        // Compact mode: one child per row (see the class comment).
        void SetForceSingleColumn(bool force);
        bool ForceSingleColumn() const { return m_forceSingle; }

        Size Measure(Size available) override;
        void Arrange(Rect finalRect) override;

    protected:
        // One child's placement relative to the padded content-area origin.
        struct Placed
        {
            Wnd* wnd = nullptr;
            float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        };

        // Wrap children for the given content width; fills `out` (when non-null)
        // and returns the total content height. `outUsedWidth` (when non-null)
        // receives the widest row's width. `contentWidth` may be effectively
        // infinite for an unconstrained probe, which never wraps.
        float LayoutRows(float contentWidth, std::vector<Placed>* out,
                         float* outUsedWidth = nullptr) const;

        float m_hgap { 12.0f };
        float m_vgap { 8.0f };
        bool m_forceSingle { false };
    };
}
