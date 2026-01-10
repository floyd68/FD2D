#include "ScrollView.h"
#include "Backplate.h"
#include <algorithm>
#include <float.h>
#include <windowsx.h>
#include <cmath>

namespace FD2D
{
    namespace
    {
        static unsigned long long NowMs()
        {
            return static_cast<unsigned long long>(GetTickCount64());
        }

        static bool IsMouseMessage(UINT message)
        {
            switch (message)
            {
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
            case WM_XBUTTONDBLCLK:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
            case WM_CAPTURECHANGED:
                return true;
            default:
                return false;
            }
        }

        static LPARAM TranslateMouseLParam(LPARAM lParam, float deltaX, float deltaY)
        {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const int newX = static_cast<int>(std::lround(static_cast<double>(x) + static_cast<double>(deltaX)));
            const int newY = static_cast<int>(std::lround(static_cast<double>(y) + static_cast<double>(deltaY)));
            return MAKELPARAM(newX, newY);
        }
    }

    bool ScrollView::IsPointInViewport(int x, int y) const
    {
        const D2D1_RECT_F r = LayoutRect();
        return static_cast<float>(x) >= r.left &&
            static_cast<float>(x) <= r.right &&
            static_cast<float>(y) >= r.top &&
            static_cast<float>(y) <= r.bottom;
    }

    ScrollView::ScrollView()
        : Wnd()
    {
    }

    ScrollView::ScrollView(const std::wstring& name)
        : Wnd(name)
    {
    }

    void ScrollView::SetHorizontalScrollEnabled(bool enabled)
    {
        m_enableHScroll = enabled;
        if (!m_enableHScroll)
        {
            m_scrollX = 0.0f;
        }
        ClampScroll();
        Invalidate();
    }

    void ScrollView::SetVerticalScrollEnabled(bool enabled)
    {
        m_enableVScroll = enabled;
        if (!m_enableVScroll)
        {
            m_scrollY = 0.0f;
        }
        ClampScroll();
        Invalidate();
    }

    void ScrollView::SetContent(const std::shared_ptr<Wnd>& content)
    {
        m_content = content;
        if (m_content && !m_content->Name().empty())
        {
            if (Children().find(m_content->Name()) == Children().end())
            {
                AddChild(m_content);
            }
        }
        Invalidate();
    }

    void ScrollView::SetScrollY(float y)
    {
        if (!m_enableVScroll)
        {
            m_scrollY = 0.0f;
            m_targetScrollY = 0.0f;
            return;
        }
        m_scrollY = (std::max)(0.0f, y);
        m_targetScrollY = m_scrollY;
        ClampScroll();
        ClampTargetScroll();
        Invalidate();
    }

    void ScrollView::SetScrollX(float x)
    {
        if (!m_enableHScroll)
        {
            m_scrollX = 0.0f;
            m_targetScrollX = 0.0f;
            return;
        }
        m_scrollX = (std::max)(0.0f, x);
        m_targetScrollX = m_scrollX;
        ClampScroll();
        ClampTargetScroll();
        Invalidate();
    }

    void ScrollView::SetScrollStep(float step)
    {
        m_scrollStep = (std::max)(1.0f, step);
    }

    void ScrollView::SetSmoothScrollEnabled(bool enabled)
    {
        m_smoothScrollEnabled = enabled;
        m_lastSmoothAnimMs = 0;
        if (!m_smoothScrollEnabled)
        {
            m_targetScrollX = m_scrollX;
            m_targetScrollY = m_scrollY;
        }
        Invalidate();
    }

    void ScrollView::SetSmoothTimeMs(unsigned int timeMs)
    {
        m_smoothTimeMs = (std::max)(1U, timeMs);
    }

    void ScrollView::EnsureVisible(const D2D1_RECT_F& rect, float padding)
    {
        const float pad = (std::max)(0.0f, padding);

        // Viewport in client coordinates (same space as LayoutRect()).
        const D2D1_RECT_F viewport = LayoutRect();

        // Visible region in content coordinates is offset by current scroll.
        const float visibleLeft = viewport.left + m_scrollX;
        const float visibleRight = viewport.right + m_scrollX;
        const float visibleTop = viewport.top + m_scrollY;
        const float visibleBottom = viewport.bottom + m_scrollY;

        float newScrollX = m_scrollX;
        float newScrollY = m_scrollY;

        if (m_enableHScroll)
        {
            if (rect.left < (visibleLeft + pad))
            {
                newScrollX = (rect.left - viewport.left) - pad;
            }
            else if (rect.right > (visibleRight - pad))
            {
                newScrollX = (rect.right - viewport.right) + pad;
            }
        }

        if (m_enableVScroll)
        {
            if (rect.top < (visibleTop + pad))
            {
                newScrollY = (rect.top - viewport.top) - pad;
            }
            else if (rect.bottom > (visibleBottom - pad))
            {
                newScrollY = (rect.bottom - viewport.bottom) + pad;
            }
        }

        // Apply only if something changed (avoids extra invalidation churn).
        const float eps = 0.5f;
        if (m_enableHScroll && std::fabs(newScrollX - m_scrollX) > eps)
        {
            if (m_smoothScrollEnabled)
            {
                SetTargetScrollX(newScrollX);
            }
            else
            {
                SetScrollX(newScrollX);
            }
        }
        if (m_enableVScroll && std::fabs(newScrollY - m_scrollY) > eps)
        {
            if (m_smoothScrollEnabled)
            {
                SetTargetScrollY(newScrollY);
            }
            else
            {
                SetScrollY(newScrollY);
            }
        }
    }

    void ScrollView::EnsureCentered(const D2D1_RECT_F& rect)
    {
        const D2D1_RECT_F viewportOuter = LayoutRect();

        float newScrollX = m_scrollX;
        float newScrollY = m_scrollY;

        if (m_enableHScroll)
        {
            const float viewportW = (std::max)(0.0f, m_viewportSize.w);
            const float viewportCenter = (viewportOuter.left + viewportOuter.right) * 0.5f;
            const float rectCenter = (rect.left + rect.right) * 0.5f;

            const float maxScrollX = (std::max)(0.0f, m_contentSize.w - m_viewportSize.w);

            // Content is arranged into the padded child area.
            const float contentStart = viewportOuter.left + m_padding;
            const float contentEnd = contentStart + m_contentSize.w;

            // Items near the edges should NOT be centered: snap to start/end zones.
            const float edgeZone = 0.5f * viewportW;
            if (rect.left <= (contentStart + edgeZone))
            {
                newScrollX = 0.0f;
            }
            else if (rect.right >= (contentEnd - edgeZone))
            {
                newScrollX = maxScrollX;
            }
            else
            {
                newScrollX = rectCenter - viewportCenter;
                newScrollX = (std::max)(0.0f, (std::min)(maxScrollX, newScrollX));
            }
        }

        if (m_enableVScroll)
        {
            const float viewportCenter = (viewportOuter.top + viewportOuter.bottom) * 0.5f;
            const float rectCenter = (rect.top + rect.bottom) * 0.5f;
            const float maxScrollY = (std::max)(0.0f, m_contentSize.h - m_viewportSize.h);
            newScrollY = rectCenter - viewportCenter;
            newScrollY = (std::max)(0.0f, (std::min)(maxScrollY, newScrollY));
        }

        const float eps = 0.5f;
        if (m_enableHScroll && std::fabs(newScrollX - m_scrollX) > eps)
        {
            if (m_smoothScrollEnabled)
            {
                SetTargetScrollX(newScrollX);
            }
            else
            {
                SetScrollX(newScrollX);
            }
        }
        if (m_enableVScroll && std::fabs(newScrollY - m_scrollY) > eps)
        {
            if (m_smoothScrollEnabled)
            {
                SetTargetScrollY(newScrollY);
            }
            else
            {
                SetScrollY(newScrollY);
            }
        }
    }

    void ScrollView::SetPropagateMinSize(bool propagate)
    {
        m_propagateMinSize = propagate;
        Invalidate();
    }

    Size ScrollView::Measure(Size available)
    {
        // ScrollView itself wants to take whatever space the parent gives.
        m_desired = available;

        // Measure content with the same available as a hint (we'll do "infinite" measure in Arrange).
        if (m_content)
        {
            (void)m_content->Measure(available);
        }

        return m_desired;
    }

    Size ScrollView::MinSize() const
    {
        if (!m_propagateMinSize)
        {
            // Block upward constraint propagation.
            return { 0.0f, 0.0f };
        }

        // Propagate content min size (plus padding/margin like default Wnd does).
        if (!m_content)
        {
            return { 0.0f, 0.0f };
        }

        Size ms = m_content->MinSize();
        ms.w += 2.0f * m_margin + 2.0f * m_padding;
        ms.h += 2.0f * m_margin + 2.0f * m_padding;
        return ms;
    }

    void ScrollView::Arrange(Rect finalRect)
    {
        Rect inset = Inset(finalRect, m_margin);
        m_bounds = inset;
        m_layoutRect = ToD2D(inset);

        Rect childArea = Inset(inset, m_padding);
        m_viewportSize = { childArea.w, childArea.h };

        if (m_content)
        {
            // Measure content with "infinite only on scrollable axes" to learn intrinsic size without exploding
            // desired size for controls that scale to available space.
            Size probeAvailable
            {
                m_enableHScroll ? FLT_MAX : childArea.w,
                m_enableVScroll ? FLT_MAX : childArea.h
            };
            Size desired = m_content->Measure(probeAvailable);

            const float arrangedW = m_enableHScroll ? (std::max)(childArea.w, desired.w) : childArea.w;
            const float arrangedH = m_enableVScroll ? (std::max)(childArea.h, desired.h) : childArea.h;
            m_contentSize = { arrangedW, arrangedH };

            // Arrange content within the viewport (scrolling will translate during render).
            Rect contentRect { childArea.x, childArea.y, arrangedW, arrangedH };
            m_content->Arrange(contentRect);
        }

        ClampScroll();
        ClampTargetScroll();
    }

    void ScrollView::ClampScroll()
    {
        const float maxScrollX = m_enableHScroll ? (std::max)(0.0f, m_contentSize.w - m_viewportSize.w) : 0.0f;
        const float maxScrollY = m_enableVScroll ? (std::max)(0.0f, m_contentSize.h - m_viewportSize.h) : 0.0f;

        m_scrollX = (std::max)(0.0f, (std::min)(maxScrollX, m_scrollX));
        m_scrollY = (std::max)(0.0f, (std::min)(maxScrollY, m_scrollY));

        if (!m_enableHScroll)
        {
            m_scrollX = 0.0f;
        }
        if (!m_enableVScroll)
        {
            m_scrollY = 0.0f;
        }
    }

    void ScrollView::ClampTargetScroll()
    {
        const float maxScrollX = m_enableHScroll ? (std::max)(0.0f, m_contentSize.w - m_viewportSize.w) : 0.0f;
        const float maxScrollY = m_enableVScroll ? (std::max)(0.0f, m_contentSize.h - m_viewportSize.h) : 0.0f;

        m_targetScrollX = (std::max)(0.0f, (std::min)(maxScrollX, m_targetScrollX));
        m_targetScrollY = (std::max)(0.0f, (std::min)(maxScrollY, m_targetScrollY));

        if (!m_enableHScroll)
        {
            m_targetScrollX = 0.0f;
        }
        if (!m_enableVScroll)
        {
            m_targetScrollY = 0.0f;
        }
    }

    void ScrollView::SetTargetScrollX(float x)
    {
        if (!m_enableHScroll)
        {
            m_targetScrollX = 0.0f;
            return;
        }
        m_targetScrollX = (std::max)(0.0f, x);
        ClampTargetScroll();
        if (BackplateRef() != nullptr)
        {
            BackplateRef()->RequestAnimationFrame();
        }
        Invalidate();
    }

    void ScrollView::SetTargetScrollY(float y)
    {
        if (!m_enableVScroll)
        {
            m_targetScrollY = 0.0f;
            return;
        }
        m_targetScrollY = (std::max)(0.0f, y);
        ClampTargetScroll();
        if (BackplateRef() != nullptr)
        {
            BackplateRef()->RequestAnimationFrame();
        }
        Invalidate();
    }

    void ScrollView::AdvanceSmoothScroll(unsigned long long nowMs)
    {
        if (!m_smoothScrollEnabled)
        {
            return;
        }

        const float dx = m_targetScrollX - m_scrollX;
        const float dy = m_targetScrollY - m_scrollY;
        const float eps = 0.25f;
        if (std::fabs(dx) < eps && std::fabs(dy) < eps)
        {
            m_scrollX = m_targetScrollX;
            m_scrollY = m_targetScrollY;
            m_lastSmoothAnimMs = nowMs;
            return;
        }

        if (m_lastSmoothAnimMs == 0)
        {
            m_lastSmoothAnimMs = nowMs;
        }
        const unsigned long long dt = nowMs - m_lastSmoothAnimMs;
        m_lastSmoothAnimMs = nowMs;

        const float tauMs = (m_smoothTimeMs > 0) ? static_cast<float>(m_smoothTimeMs) : 90.0f;
        const float a = 1.0f - std::exp(-static_cast<float>(dt) / tauMs);

        m_scrollX += dx * a;
        m_scrollY += dy * a;
        ClampScroll();

        if (BackplateRef() != nullptr)
        {
            BackplateRef()->RequestAnimationFrame();
        }
    }

    void ScrollView::OnRender(ID2D1RenderTarget* target)
    {
        if (!target)
        {
            return;
        }

        AdvanceSmoothScroll(NowMs());

        // Clip to viewport + apply translation
        const D2D1_RECT_F clip = LayoutRect();
        target->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        D2D1_MATRIX_3X2_F oldTransform {};
        target->GetTransform(&oldTransform);

        const D2D1_MATRIX_3X2_F scrollTransform = D2D1::Matrix3x2F::Translation(-m_scrollX, -m_scrollY);
        target->SetTransform(oldTransform * scrollTransform);

        if (m_content)
        {
            m_content->OnRender(target);
        }
        else
        {
            // fallback
            Wnd::OnRender(target);
        }

        target->SetTransform(oldTransform);
        target->PopAxisAlignedClip();
    }

    bool ScrollView::OnMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_MOUSEWHEEL:
        {
            // Only scroll when the cursor is over this viewport.
            // Backplate normalizes mouse coordinates to client coordinates before forwarding.
            // So lParam is already in this ScrollView's coordinate space.
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const bool inViewport = IsPointInViewport(x, y);

            // If cursor is over viewport, handle scrolling
            if (inViewport)
            {
                const short delta = static_cast<short>(HIWORD(wParam));
                const float ticks = static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA);
                const bool shift = (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0;
                const float step = -ticks * m_scrollStep;
                if (m_enableHScroll && (!m_enableVScroll || shift))
                {
                    if (m_smoothScrollEnabled)
                    {
                        SetTargetScrollX(m_targetScrollX + step);
                    }
                    else
                    {
                        SetScrollX(m_scrollX + step);
                    }
                    return true;
                }
                else if (m_enableVScroll)
                {
                    if (m_smoothScrollEnabled)
                    {
                        SetTargetScrollY(m_targetScrollY + step);
                    }
                    else
                    {
                        SetScrollY(m_scrollY + step);
                    }
                    return true;
                }
            }
            // If not handled (not in viewport or no scroll enabled), fall through to forward to content
            break;
        }
        case WM_MOUSEHWHEEL:
        {
            // Backplate normalizes mouse coordinates to client coordinates before forwarding.
            // So lParam is already in this ScrollView's coordinate space.
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            if (!IsPointInViewport(x, y))
            {
                break;
            }

            const short delta = static_cast<short>(HIWORD(wParam));
            const float ticks = static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA);
            const float step = -ticks * m_scrollStep;
            if (m_enableHScroll)
            {
                if (m_smoothScrollEnabled)
                {
                    SetTargetScrollX(m_targetScrollX + step);
                }
                else
                {
                    SetScrollX(m_scrollX + step);
                }
                return true;
            }
            break;
        }
        default:
            break;
        }

        // Forward mouse events to content using scrolled coordinates so hit-testing matches rendering.
        if (m_content && IsMouseMessage(message))
        {
            if (message == WM_CAPTURECHANGED)
            {
                m_forwardCapture = false;
                return m_content->OnMessage(message, wParam, lParam);
            }

            // If we're not in an active drag/capture sequence, ignore events outside the viewport.
            if (!m_forwardCapture)
            {
                const int x = GET_X_LPARAM(lParam);
                const int y = GET_Y_LPARAM(lParam);
                if (!IsPointInViewport(x, y))
                {
                    return false;
                }
            }

            // For client-coordinate mouse messages, translate Y by scroll offset.
            // WM_MOUSEWHEEL/WM_MOUSEHWHEEL use screen coords but Backplate already converted to client.
            // We don't translate wheel messages by scroll offset since they're position-based.
            if (message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL && message != WM_CAPTURECHANGED)
            {
                lParam = TranslateMouseLParam(lParam, m_scrollX, m_scrollY);
            }

            const bool handled = m_content->OnMessage(message, wParam, lParam);

            // Track capture-like sequences so we keep forwarding move/up even if cursor exits the viewport.
            if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN)
            {
                if (handled)
                {
                    m_forwardCapture = true;
                }
            }
            if (message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP)
            {
                m_forwardCapture = false;
            }

            return handled;
        }

        return Wnd::OnMessage(message, wParam, lParam);
    }
}


