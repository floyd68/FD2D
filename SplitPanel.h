#pragma once

#include "Panel.h"
#include "Splitter.h"
#include <functional>

namespace FD2D
{
    enum class ConstraintPropagation
    {
        None,       // No effect on parent
        Minimum,    // Propagate minimum size only (reflected in Measure's desired)
        Strict      // (Currently equivalent to Minimum. Can be linked to overflow policy/window min-size in the future)
    };

    class SplitPanel : public Panel
    {
    public:
        SplitPanel();
        explicit SplitPanel(const std::wstring& name, SplitterOrientation orientation = SplitterOrientation::Horizontal);

        void SetOrientation(SplitterOrientation orientation);
        SplitterOrientation Orientation() const { return m_orientation; }

        void SetFirstChild(const std::shared_ptr<Wnd>& child);
        void SetSecondChild(const std::shared_ptr<Wnd>& child);
        void SetSplitRatio(float ratio);  // 0.0 ~ 1.0
        float SplitRatio() const { return m_splitRatio; }

        // Constrain pane extents (width for Horizontal, height for Vertical)
        void SetFirstPaneMinExtent(float extent);
        void SetFirstPaneMaxExtent(float extent);
        void SetSecondPaneMinExtent(float extent);
        void SetSecondPaneMaxExtent(float extent);

        void SetConstraintPropagation(ConstraintPropagation policy);
        ConstraintPropagation PropagationPolicy() const { return m_propagation; }

        void OnSplitChanged(std::function<void(float ratio)> handler);
        bool IsSplitterDragging() const;

        Size Measure(Size available) override;
        Size MinSize() const override;
        void Arrange(Rect finalRect) override;
        void OnRender(ID2D1RenderTarget* target) override;

    private:
        void OnSplitRatioChanged(float ratio);
        float ClampRatioForPaneConstraints(const Rect& childArea, float splitterExtent, float ratio) const;

        SplitterOrientation m_orientation { SplitterOrientation::Horizontal };
        // m_splitRatio is the *effective* ratio actually used for the most recent
        // Arrange pass (after clamping to the pane min/max extents below).
        // m_requestedSplitRatio is the ratio the user actually asked for (via
        // SetSplitRatio(), or by dragging the Splitter) - i.e. what the split
        // *would* be if the current window size allowed it. Arrange() always
        // re-derives m_splitRatio from m_requestedSplitRatio + the *current*
        // bounds, rather than re-clamping the previous effective m_splitRatio.
        // Without this distinction, once a resize clamped m_splitRatio down (e.g.
        // a short window forcing the second pane to its min extent), a later
        // resize back to a larger size would find the already-clamped value
        // still technically "in range" for the new size and leave it alone
        // instead of growing back toward what the user actually asked for -
        // making the second pane's effective size (and how many of its child
        // rows fit) depend on the *history* of window resizes rather than only
        // the current window size, which showed up as control rows at the
        // bottom of NifCompareControlPanel appearing/disappearing seemingly at
        // random as the window was resized up and down through the same sizes.
        float m_splitRatio { 0.5f };
        float m_requestedSplitRatio { 0.5f };
        float m_firstPaneMinExtent { 0.0f };  // 0 = no limit
        float m_firstPaneMaxExtent { 0.0f };  // 0 = no limit
        float m_secondPaneMinExtent { 0.0f }; // 0 = no limit
        float m_secondPaneMaxExtent { 0.0f }; // 0 = no limit
        ConstraintPropagation m_propagation { ConstraintPropagation::None };
        std::shared_ptr<Wnd> m_firstChild {};
        std::shared_ptr<Wnd> m_secondChild {};
        std::shared_ptr<Splitter> m_splitter {};

        std::function<void(float)> m_splitChanged {};

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_dragDimBrush {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_dragOutlineBrush {};
    };
}


