#pragma once

#include "Wnd.h"
#include <wrl/client.h>

namespace FD2D
{
    // Overflow / Scroll container.
    // - Blocks upward MinSize propagation by default (so children constraints don't force window min-size).
    // - Provides basic clipping + vertical wheel scroll.
    class ScrollView : public Wnd
    {
    public:
        ScrollView();
        explicit ScrollView(const std::wstring& name);

        void SetContent(const std::shared_ptr<Wnd>& content);
        const std::shared_ptr<Wnd>& Content() const { return m_content; }

        void SetHorizontalScrollEnabled(bool enabled);
        bool HorizontalScrollEnabled() const { return m_enableHScroll; }

        // Draw (and let the user drag) a thin scrollbar on any enabled axis whose
        // content overflows the viewport. Off by default so existing wheel-only
        // ScrollViews are unchanged; opt in for a visible, draggable bar.
        void SetScrollBarsVisible(bool visible);
        bool ScrollBarsVisible() const { return m_showScrollBars; }

        // Scrollbar geometry: a bar kScrollBarThickness thick, kScrollBarInset
        // from the viewport edges; with a gutter, kScrollBarGutter is reserved
        // for it (an inset on both sides of the bar).
        static constexpr float kScrollBarThickness = 9.0f;
        static constexpr float kScrollBarInset = 2.0f;
        static constexpr float kScrollBarGutter = kScrollBarThickness + 2.0f * kScrollBarInset;

        // Reserve a gutter for each visible scrollbar (needs SetScrollBarsVisible)
        // so the bar never covers content: on an axis that overflows, the content
        // viewport - the size content is arranged to, the scroll range and the
        // content clip - leaves kScrollBarGutter free along the bottom (horizontal
        // bar) or right edge (vertical bar), and the bar is drawn there. Off by
        // default (bars drawn over the content).
        //
        // Overflow is decided anew in each Arrange, from the content and the
        // size alone, so it cannot toggle between arranges: starting without
        // gutters, every axis whose content overflows the current viewport gets
        // its gutter and the content is measured again for the reduced
        // viewport, until no further axis overflows. A gutter is never dropped
        // in that Arrange, even if the content then fits (the bar is hidden and
        // the gutter stays empty), so a visible bar is always in its gutter.
        void SetScrollBarGutter(bool reserve);
        bool ScrollBarGutter() const { return m_reserveGutter; }

        // Where content is visible and receives input (client coordinates): the
        // LayoutRect() without the gutters reserved in the last Arrange.
        D2D1_RECT_F ContentViewportRect() const;

        // Scrollbar geometry (client coords), as drawn and hit-tested. Returns
        // false when that axis has no visible bar (no overflow). outThumb is
        // the draggable handle inside outTrack.
        bool HScrollBarRects(D2D1_RECT_F& outTrack, D2D1_RECT_F& outThumb) const;
        bool VScrollBarRects(D2D1_RECT_F& outTrack, D2D1_RECT_F& outThumb) const;

        void SetVerticalScrollEnabled(bool enabled);
        bool VerticalScrollEnabled() const { return m_enableVScroll; }

        void SetScrollY(float y);
        float ScrollY() const { return m_scrollY; }

        void SetScrollX(float x);
        float ScrollX() const { return m_scrollX; }
        // Where a smooth scroll in progress is heading (ScrollX() when idle).
        float TargetScrollX() const { return m_targetScrollX; }

        // Moves the current and the target horizontal offset by `delta`
        // (clamped to the scrollable range). Use it to compensate content that
        // changed size before the viewport: unlike SetScrollX, a smooth scroll in
        // progress keeps running towards the shifted target.
        void ShiftScrollX(float delta);

        void SetScrollStep(float step);
        float ScrollStep() const { return m_scrollStep; }

        void SetSmoothTimeMs(unsigned int timeMs);
        unsigned int SmoothTimeMs() const { return m_smoothTimeMs; }

        // Scroll so that `rect` is centered within the viewport where possible.
        // Edge-zone rule: items near the start/end snap to the start/end (do NOT force centering).
        // `rect` should be in the same coordinate space as Wnd::LayoutRect() (client coordinates).
        void EnsureCentered(const D2D1_RECT_F& rect, bool Immediate = false);

        // If true, MinSize() will include content's MinSize(). Default false for overflow behavior.
        void SetPropagateMinSize(bool propagate);
        bool PropagateMinSize() const { return m_propagateMinSize; }

        Size Measure(Size available) override;
        Size MinSize() const override;
        void Arrange(Rect finalRect) override;
        void OnRender(ID2D1RenderTarget* target) override;
        bool OnInputEvent(const InputEvent& event) override;

    protected:
        void RenderChildOverlays(ID2D1RenderTarget* target, OverlayLayer layer) override;
        bool RouteChildOverlayInput(const InputEvent& event, OverlayLayer layer) override;

    private:
        void ClampScroll();
        bool IsPointInViewport(int x, int y) const;
        void ClampTargetScroll();
        void SetTargetScrollX(float x);
        void SetTargetScrollY(float y);
        void AdvanceSmoothScroll(unsigned long long nowMs);

        std::shared_ptr<Wnd> m_content {};
        float m_scrollX { 0.0f };
        float m_scrollY { 0.0f };
        float m_targetScrollX { 0.0f };
        float m_targetScrollY { 0.0f };
        float m_scrollStep { 48.0f };
        bool m_propagateMinSize { false };
        bool m_forwardCapture { false };
        bool m_enableHScroll { true };
        bool m_enableVScroll { true };
        bool m_showScrollBars { false };
        bool m_reserveGutter { false };
        // Gutters reserved in the last Arrange (horizontal bar: bottom; vertical: right).
        bool m_hGutter { false };
        bool m_vGutter { false };
        // Scrollbar drag: 0 = horizontal thumb, 1 = vertical thumb, -1 = none.
        int m_barDragAxis { -1 };
        float m_barDragMouse { 0.0f };  // cursor pos on the drag axis at grab
        float m_barDragScroll { 0.0f }; // scroll offset at grab
        bool m_barHover { false };
        unsigned long long m_lastSmoothAnimMs { 0 };
        unsigned int m_smoothTimeMs { 110 }; // smaller = snappier

        // cached after Arrange
        Size m_viewportSize { 0.0f, 0.0f };
        Size m_contentSize { 0.0f, 0.0f };

        // Scrollbar brush, recolored per fill and created once per target
        // generation (see GraphicsResourceStamp).
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_barBrush {};
        GraphicsResourceStamp m_barBrushStamp {};
    };
}


