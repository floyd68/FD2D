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

        // Scroll so that `rect` becomes visible within this ScrollView's viewport.
        // `rect` should be in the same coordinate space as Wnd::LayoutRect() (client coordinates).
        void EnsureVisible(const D2D1_RECT_F& rect, float padding = 0.0f);

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

        std::shared_ptr<Wnd> m_content {};
        float m_scrollX { 0.0f };
        float m_scrollY { 0.0f };
        float m_scrollStep { 48.0f };
        bool m_propagateMinSize { false };
        bool m_forwardCapture { false };
        bool m_enableHScroll { true };
        bool m_enableVScroll { true };

        // cached after Arrange
        Size m_viewportSize { 0.0f, 0.0f };
        Size m_contentSize { 0.0f, 0.0f };
    };
}


