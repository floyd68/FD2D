#include "ScrollView.h"
#include "Backplate.h"
#include "Util.h"
#include <algorithm>
#include <float.h>
#include <cmath>

namespace FD2D
{
    bool ScrollView::IsPointInViewport(int x, int y) const
    {
        return Util::RectContainsPoint(LayoutRect(), POINT { x, y });
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

    void ScrollView::SetScrollBarsVisible(bool visible)
    {
        m_showScrollBars = visible;
        Invalidate();
    }

    namespace
    {
        constexpr float kBarThick = 9.0f;   // scrollbar thickness
        constexpr float kBarPad = 2.0f;     // inset from the viewport edges
        constexpr float kBarMinThumb = 28.0f;
    }

    bool ScrollView::HScrollBarRects(D2D1_RECT_F& track, D2D1_RECT_F& thumb) const
    {
        if (!m_showScrollBars || !m_enableHScroll)
            return false;
        const float maxScrollX = (std::max)(0.0f, m_contentSize.w - m_viewportSize.w);
        if (maxScrollX <= 0.5f)
            return false;
        const bool vBar = m_enableVScroll && (m_contentSize.h - m_viewportSize.h) > 0.5f;
        const D2D1_RECT_F vp = LayoutRect();
        const float rightInset = vBar ? (kBarThick + kBarPad) : 0.0f;
        track = D2D1::RectF(vp.left + kBarPad, vp.bottom - kBarThick - kBarPad,
                            vp.right - kBarPad - rightInset, vp.bottom - kBarPad);
        const float trackW = track.right - track.left;
        if (trackW <= kBarMinThumb)
            return false;
        const float thumbW = (std::max)(kBarMinThumb, trackW * m_viewportSize.w / m_contentSize.w);
        const float thumbX = track.left + (trackW - thumbW) * (m_scrollX / maxScrollX);
        thumb = D2D1::RectF(thumbX, track.top, thumbX + thumbW, track.bottom);
        return true;
    }

    bool ScrollView::VScrollBarRects(D2D1_RECT_F& track, D2D1_RECT_F& thumb) const
    {
        if (!m_showScrollBars || !m_enableVScroll)
            return false;
        const float maxScrollY = (std::max)(0.0f, m_contentSize.h - m_viewportSize.h);
        if (maxScrollY <= 0.5f)
            return false;
        const bool hBar = m_enableHScroll && (m_contentSize.w - m_viewportSize.w) > 0.5f;
        const D2D1_RECT_F vp = LayoutRect();
        const float bottomInset = hBar ? (kBarThick + kBarPad) : 0.0f;
        track = D2D1::RectF(vp.right - kBarThick - kBarPad, vp.top + kBarPad,
                            vp.right - kBarPad, vp.bottom - kBarPad - bottomInset);
        const float trackH = track.bottom - track.top;
        if (trackH <= kBarMinThumb)
            return false;
        const float thumbH = (std::max)(kBarMinThumb, trackH * m_viewportSize.h / m_contentSize.h);
        const float thumbY = track.top + (trackH - thumbH) * (m_scrollY / maxScrollY);
        thumb = D2D1::RectF(track.left, thumbY, track.right, thumbY + thumbH);
        return true;
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

    void ScrollView::SetSmoothTimeMs(unsigned int timeMs)
    {
        m_smoothTimeMs = (std::max)(1U, timeMs);
    }

    void ScrollView::EnsureCentered(const D2D1_RECT_F& rect, bool Immediate)
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
            if (Immediate)
            {
                SetScrollX(newScrollX);
            }
            else
            {
                SetTargetScrollX(newScrollX);
            }
        }
        if (m_enableVScroll && std::fabs(newScrollY - m_scrollY) > eps)
        {
            if (Immediate)
            {
                SetScrollY(newScrollY);
            }
            else
            {
                SetTargetScrollY(newScrollY);
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

        AdvanceSmoothScroll(Util::NowMs());

        // Clip to viewport + apply translation
        const D2D1_RECT_F clip = LayoutRect();
        
        // Debug: Check if clip rect is valid
        if (Name() == L"thumbScroll")
        {
        }
        
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

        // Scrollbars are drawn in viewport space (not scrolled), on top of the
        // content, for whichever enabled axis actually overflows.
        if (m_showScrollBars)
        {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
            auto drawBar = [&](const D2D1_RECT_F& tr, const D2D1_RECT_F& th, bool active)
            {
                const float r = 0.5f * (std::min)(th.right - th.left, th.bottom - th.top);
                if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f), &brush)))
                    target->FillRoundedRectangle(D2D1::RoundedRect(tr, r, r), brush.Get());
                const D2D1_COLOR_F c = active ? D2D1::ColorF(0.56f, 0.61f, 0.70f, 0.95f)
                                              : D2D1::ColorF(0.42f, 0.45f, 0.52f, 0.85f);
                if (SUCCEEDED(target->CreateSolidColorBrush(c, &brush)))
                    target->FillRoundedRectangle(D2D1::RoundedRect(th, r, r), brush.Get());
            };
            D2D1_RECT_F track {}, thumb {};
            if (HScrollBarRects(track, thumb))
                drawBar(track, thumb, m_barDragAxis == 0 || m_barHover);
            if (VScrollBarRects(track, thumb))
                drawBar(track, thumb, m_barDragAxis == 1);
        }
    }

    bool ScrollView::OnInputEvent(const InputEvent& event)
    {
        // Draggable scrollbars (opt-in). Handled before the content dispatch so a
        // grab on a thumb never falls through to the content beneath it. All of
        // this is skipped entirely unless SetScrollBarsVisible(true) was called.
        if (m_showScrollBars)
        {
            if (event.type == InputEventType::MouseDown && event.button == MouseButton::Left && event.hasPoint)
            {
                D2D1_RECT_F track {}, thumb {};
                for (int axis = 0; axis < 2; ++axis)
                {
                    const bool ok = (axis == 0) ? HScrollBarRects(track, thumb) : VScrollBarRects(track, thumb);
                    if (!ok)
                        continue;
                    if (Util::RectContainsPoint(thumb, event.point))
                    {
                        m_barDragAxis = axis;
                        m_barDragMouse = (axis == 0) ? static_cast<float>(event.point.x) : static_cast<float>(event.point.y);
                        m_barDragScroll = (axis == 0) ? m_scrollX : m_scrollY;
                        if (BackplateRef() != nullptr)
                            SetCapture(BackplateRef()->Window());
                        Invalidate();
                        return true;
                    }
                    if (Util::RectContainsPoint(track, event.point))
                    {
                        const float page = 0.9f * ((axis == 0) ? m_viewportSize.w : m_viewportSize.h);
                        const bool before = (axis == 0) ? (event.point.x < thumb.left) : (event.point.y < thumb.top);
                        const float d = before ? -page : page;
                        if (axis == 0) SetScrollX(m_scrollX + d); else SetScrollY(m_scrollY + d);
                        return true;
                    }
                }
            }
            else if (event.type == InputEventType::MouseMove && m_barDragAxis >= 0 && event.hasPoint)
            {
                D2D1_RECT_F track {}, thumb {};
                const bool ok = (m_barDragAxis == 0) ? HScrollBarRects(track, thumb) : VScrollBarRects(track, thumb);
                if (ok)
                {
                    if (m_barDragAxis == 0)
                    {
                        const float span = (track.right - track.left) - (thumb.right - thumb.left);
                        const float maxScrollX = (std::max)(0.0f, m_contentSize.w - m_viewportSize.w);
                        if (span > 0.5f)
                            SetScrollX(m_barDragScroll + (static_cast<float>(event.point.x) - m_barDragMouse) / span * maxScrollX);
                    }
                    else
                    {
                        const float span = (track.bottom - track.top) - (thumb.bottom - thumb.top);
                        const float maxScrollY = (std::max)(0.0f, m_contentSize.h - m_viewportSize.h);
                        if (span > 0.5f)
                            SetScrollY(m_barDragScroll + (static_cast<float>(event.point.y) - m_barDragMouse) / span * maxScrollY);
                    }
                }
                return true;
            }
            else if (event.type == InputEventType::MouseUp && m_barDragAxis >= 0)
            {
                m_barDragAxis = -1;
                if (BackplateRef() != nullptr)
                    ReleaseCapture();
                Invalidate();
                return true;
            }
            else if (event.type == InputEventType::MouseMove && m_barDragAxis < 0 && event.hasPoint)
            {
                D2D1_RECT_F track {}, thumb {};
                const bool over = HScrollBarRects(track, thumb) && Util::RectContainsPoint(thumb, event.point);
                if (over != m_barHover)
                {
                    m_barHover = over;
                    Invalidate();
                }
            }
        }

        switch (event.type)
        {
        case InputEventType::MouseWheel:
        {
            if (!event.hasPoint)
            {
                break;
            }
            // Only scroll when the cursor is over this viewport.
            const int x = event.point.x;
            const int y = event.point.y;
            const bool inViewport = IsPointInViewport(x, y);

            // If cursor is over viewport, handle scrolling
            if (inViewport)
            {
                const short delta = event.wheelDelta;
                const float ticks = static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA);
                const bool shift = event.modifiers.shift;
                const float step = -ticks * m_scrollStep;
                if (m_enableHScroll && (!m_enableVScroll || shift))
                {
                    SetTargetScrollX(m_targetScrollX + step);
                    return true;
                }
                else if (m_enableVScroll)
                {
                    SetTargetScrollY(m_targetScrollY + step);
                    return true;
                }
            }
            // If not handled (not in viewport or no scroll enabled), fall through to forward to content
            break;
        }
        case InputEventType::MouseHWheel:
        {
            if (!event.hasPoint)
            {
                break;
            }
            const int x = event.point.x;
            const int y = event.point.y;
            if (!IsPointInViewport(x, y))
            {
                break;
            }

            const short delta = event.wheelDelta;
            const float ticks = static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA);
            const float step = -ticks * m_scrollStep;
            if (m_enableHScroll)
            {
                SetTargetScrollX(m_targetScrollX + step);
                return true;
            }
            break;
        }
        default:
            break;
        }

        // Forward mouse events to content using scrolled coordinates so hit-testing matches rendering.
        if (m_content && Util::IsMouseInputEventType(event.type))
        {
            if (event.type == InputEventType::CaptureChanged)
            {
                m_forwardCapture = false;
                return m_content->OnInputEvent(event);
            }

            // If we're not in an active drag/capture sequence, ignore events outside the viewport.
            if (!m_forwardCapture)
            {
                if (!event.hasPoint)
                {
                    return false;
                }
                const int x = event.point.x;
                const int y = event.point.y;
                if (!IsPointInViewport(x, y))
                {
                    return false;
                }
            }

            InputEvent translated = event;

            // For client-coordinate mouse messages, translate by scroll offset.
            if (translated.hasPoint &&
                translated.type != InputEventType::MouseWheel &&
                translated.type != InputEventType::MouseHWheel &&
                translated.type != InputEventType::CaptureChanged)
            {
                translated.point.x = static_cast<int>(std::lround(static_cast<double>(translated.point.x) + static_cast<double>(m_scrollX)));
                translated.point.y = static_cast<int>(std::lround(static_cast<double>(translated.point.y) + static_cast<double>(m_scrollY)));
            }

            const bool handled = m_content->OnInputEvent(translated);

            // Track capture-like sequences so we keep forwarding move/up even if cursor exits the viewport.
            if (event.type == InputEventType::MouseDown)
            {
                if (handled)
                {
                    m_forwardCapture = true;
                }
            }
            if (event.type == InputEventType::MouseUp)
            {
                m_forwardCapture = false;
            }

            return handled;
        }

        return Wnd::OnInputEvent(event);
    }
}


