#pragma once

#include "Wnd.h"

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

        void SetVerticalScrollEnabled(bool enabled);
        bool VerticalScrollEnabled() const { return m_enableVScroll; }

        void SetScrollY(float y);
        float ScrollY() const { return m_scrollY; }

        void SetScrollX(float x);
        float ScrollX() const { return m_scrollX; }

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
        bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

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
        unsigned long long m_lastSmoothAnimMs { 0 };
        unsigned int m_smoothTimeMs { 110 }; // smaller = snappier

        // cached after Arrange
        Size m_viewportSize { 0.0f, 0.0f };
        Size m_contentSize { 0.0f, 0.0f };
    };
}


