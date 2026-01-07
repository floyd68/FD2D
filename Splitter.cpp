#include "Splitter.h"
#include "Backplate.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace FD2D
{
    namespace
    {
        static unsigned long long NowMs()
        {
            return static_cast<unsigned long long>(GetTickCount64());
        }

        static float Clamp01(float v)
        {
            if (v < 0.0f)
            {
                return 0.0f;
            }
            if (v > 1.0f)
            {
                return 1.0f;
            }
            return v;
        }

        static float Lerp(float a, float b, float t)
        {
            return a + (b - a) * t;
        }

        static D2D1_COLOR_F LerpColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t)
        {
            return D2D1::ColorF(
                Lerp(a.r, b.r, t),
                Lerp(a.g, b.g, t),
                Lerp(a.b, b.b, t),
                Lerp(a.a, b.a, t));
        }
    }

    Splitter::Splitter()
        : Wnd()
    {
    }

    Splitter::Splitter(const std::wstring& name, SplitterOrientation orientation)
        : Wnd(name)
        , m_orientation(orientation)
    {
    }

    void Splitter::SetOrientation(SplitterOrientation orientation)
    {
        m_orientation = orientation;
        Invalidate();
    }

    void Splitter::SetThickness(float thickness)
    {
        m_thickness = (std::max)(1.0f, thickness);
        Invalidate();
    }

    void Splitter::SetHitAreaThickness(float thickness)
    {
        m_hitAreaThickness = (std::max)(m_thickness, thickness);
    }

    void Splitter::SetSnapThreshold(float threshold)
    {
        m_snapThreshold = (std::max)(0.0f, (std::min)(0.5f, threshold));
    }

    void Splitter::SetRatio(float ratio)
    {
        float clamped = (std::max)(0.0f, (std::min)(1.0f, ratio));
        clamped = CalculateRatio(clamped);

        if (clamped != m_currentRatio)
        {
            m_currentRatio = clamped;
            Invalidate();
        }
    }

    void Splitter::OnSplitChanged(std::function<void(float ratio)> handler)
    {
        m_splitChanged = std::move(handler);
    }

    Size Splitter::Measure(Size available)
    {
        if (m_orientation == SplitterOrientation::Horizontal)
        {
            // 좌우 분할: 세로 선
            m_desired = { m_hitAreaThickness, available.h };
        }
        else
        {
            // 상하 분할: 가로 선
            m_desired = { available.w, m_hitAreaThickness };
        }
        return m_desired;
    }

    void Splitter::Arrange(Rect finalRect)
    {
        m_bounds = finalRect;
        m_layoutRect = ToD2D(finalRect);
    }

    bool Splitter::HitTest(const POINT& pt) const
    {
        const auto& rect = LayoutRect();
        return pt.x >= rect.left &&
            pt.x <= rect.right &&
            pt.y >= rect.top &&
            pt.y <= rect.bottom;
    }

    void Splitter::SetParentBounds(const Rect& bounds)
    {
        m_dragStartParentBounds = bounds;
    }

    void Splitter::StartDrag(const POINT& pt)
    {
        m_dragging = true;
        m_dragStart = pt;
        m_dragStartRatio = m_currentRatio;
        // m_dragStartParentBounds는 SetParentBounds로 설정됨
        Invalidate();
    }

    void Splitter::UpdateDrag(const POINT& pt)
    {
        if (!m_dragging)
        {
            return;
        }

        float newRatio = m_dragStartRatio;
        Rect parentBounds = m_dragStartParentBounds;

        if (m_orientation == SplitterOrientation::Horizontal)
        {
            // 좌우 분할: X 좌표 사용
            float deltaX = static_cast<float>(pt.x - m_dragStart.x);
            float parentWidth = parentBounds.w;
            
            if (parentWidth > 0.0f)
            {
                float deltaRatio = deltaX / parentWidth;
                newRatio = m_dragStartRatio + deltaRatio;
            }
        }
        else
        {
            // 상하 분할: Y 좌표 사용
            float deltaY = static_cast<float>(pt.y - m_dragStart.y);
            float parentHeight = parentBounds.h;
            
            if (parentHeight > 0.0f)
            {
                float deltaRatio = deltaY / parentHeight;
                newRatio = m_dragStartRatio + deltaRatio;
            }
        }

        // 0.0 ~ 1.0 범위로 제한
        newRatio = (std::max)(0.0f, (std::min)(1.0f, newRatio));

        // Snap 처리
        newRatio = CalculateRatio(newRatio);

        if (newRatio != m_currentRatio)
        {
            m_currentRatio = newRatio;
            if (m_splitChanged)
            {
                m_splitChanged(m_currentRatio);
            }
            Invalidate();
        }
    }

    void Splitter::EndDrag()
    {
        if (m_dragging)
        {
            m_dragging = false;
            Invalidate();
        }
    }

    void Splitter::HandleDoubleClick()
    {
        // 더블클릭 시 균등 분할
        m_currentRatio = 0.5f;
        if (m_splitChanged)
        {
            m_splitChanged(m_currentRatio);
        }
        Invalidate();
    }

    float Splitter::CalculateRatio(float ratio) const
    {
        // Snap 처리: 0.0, 0.5, 1.0 근처에서 snap
        if (std::abs(ratio - 0.0f) < m_snapThreshold)
        {
            return 0.0f;
        }
        if (std::abs(ratio - 0.5f) < m_snapThreshold)
        {
            return 0.5f;
        }
        if (std::abs(ratio - 1.0f) < m_snapThreshold)
        {
            return 1.0f;
        }
        return ratio;
    }

    bool Splitter::OnMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        UNREFERENCED_PARAMETER(wParam);

        switch (message)
        {
        case WM_MOUSEMOVE:
        {
            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            bool wasHovered = m_hovered;
            m_hovered = HitTest(pt);

            if (m_hovered != wasHovered)
            {
                m_lastHoverAnimMs = 0;
                Invalidate();

                // Track mouse leave so we can fade out when cursor exits the splitter.
                if (m_hovered && !m_trackingMouseLeave && BackplateRef() != nullptr)
                {
                    TRACKMOUSEEVENT tme {};
                    tme.cbSize = sizeof(TRACKMOUSEEVENT);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = BackplateRef()->Window();
                    if (TrackMouseEvent(&tme))
                    {
                        m_trackingMouseLeave = true;
                    }
                }

                // Prompt cursor update.
                if (m_hovered)
                {
                    SetCursor(LoadCursor(nullptr, m_orientation == SplitterOrientation::Horizontal ? IDC_SIZEWE : IDC_SIZENS));
                }
            }

            if (m_dragging)
            {
                UpdateDrag(pt);
                return true;
            }

            return m_hovered;
        }
        case WM_MOUSELEAVE:
        {
            m_trackingMouseLeave = false;
            if (m_hovered)
            {
                m_hovered = false;
                m_lastHoverAnimMs = 0;
                Invalidate();
            }
            return false;
        }
        case WM_SETCURSOR:
        {
            // Natural cursor when hovering/dragging the splitter.
            // Only set it if the cursor is actually over our hit area.
            POINT pt {};
            if (GetCursorPos(&pt))
            {
                if (BackplateRef() != nullptr)
                {
                    ScreenToClient(BackplateRef()->Window(), &pt);
                }
                else
                {
                    break;
                }
                if (m_dragging || HitTest(pt))
                {
                    SetCursor(LoadCursor(nullptr, m_orientation == SplitterOrientation::Horizontal ? IDC_SIZEWE : IDC_SIZENS));
                    return true;
                }
            }
            break;
        }
        case WM_LBUTTONDOWN:
        {
            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (HitTest(pt) && BackplateRef())
            {
                StartDrag(pt);
                SetCapture(BackplateRef()->Window());
                return true;
            }
            break;
        }
        case WM_LBUTTONUP:
        {
            if (m_dragging)
            {
                EndDrag();
                if (BackplateRef())
                {
                    ReleaseCapture();
                }
                return true;
            }
            break;
        }
        case WM_LBUTTONDBLCLK:
        {
            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (HitTest(pt))
            {
                HandleDoubleClick();
                return true;
            }
            break;
        }
        case WM_CAPTURECHANGED:
        {
            if (m_dragging)
            {
                EndDrag();
            }
            break;
        }
        default:
            break;
        }

        return Wnd::OnMessage(message, wParam, lParam);
    }

    void Splitter::OnRender(ID2D1RenderTarget* target)
    {
        if (target == nullptr)
        {
            return;
        }

        const auto& rect = LayoutRect();

        // 브러시 생성
        if (!m_brushNormal)
        {
            target->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.3f, 0.3f, 0.5f), &m_brushNormal);
        }
        if (!m_brushHoverOverlay)
        {
            // Slight overlay so the wide hit-area has feedback without looking "thick".
            target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f), &m_brushHoverOverlay);
        }
        if (!m_brushDrag)
        {
            target->CreateSolidColorBrush(D2D1::ColorF(0.7f, 0.7f, 0.7f, 1.0f), &m_brushDrag);
        }
        if (!m_brushGrip)
        {
            target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.65f), &m_brushGrip);
        }

        // Hover fade animation (time-based).
        const unsigned long long now = NowMs();
        if (m_lastHoverAnimMs == 0)
        {
            m_lastHoverAnimMs = now;
        }
        const unsigned long long dtMs = now - m_lastHoverAnimMs;
        m_lastHoverAnimMs = now;

        const float targetT = (m_hovered || m_dragging) ? 1.0f : 0.0f;
        const unsigned int fadeMs = (m_hoverFadeMs > 0) ? m_hoverFadeMs : 120U;
        const float step = static_cast<float>(dtMs) / static_cast<float>(fadeMs);
        if (m_hoverT < targetT)
        {
            m_hoverT = (std::min)(targetT, m_hoverT + step);
        }
        else if (m_hoverT > targetT)
        {
            m_hoverT = (std::max)(targetT, m_hoverT - step);
        }
        m_hoverT = Clamp01(m_hoverT);

        // Splitter visuals:
        // - Wide hit-area (rect) for usability
        // - Thin center line for aesthetics
        // - Grip dots that appear on hover/drag
        const D2D1_COLOR_F accent = D2D1::ColorF(1.0f, 0.60f, 0.24f, 1.0f); // warm orange

        // Update hover overlay alpha
        if (m_brushHoverOverlay)
        {
            const float overlayA = 0.06f * m_hoverT + (m_dragging ? 0.06f : 0.0f);
            m_brushHoverOverlay->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, overlayA));
        }

        // Line brush
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush = m_brushNormal;
        if (m_dragging)
        {
            if (m_brushDrag)
            {
                m_brushDrag->SetColor(accent);
            }
            brush = m_brushDrag;
        }
        else if (m_brushNormal)
        {
            const D2D1_COLOR_F normal = D2D1::ColorF(0.30f, 0.30f, 0.30f, 0.45f);
            const D2D1_COLOR_F hover = D2D1::ColorF(0.90f, 0.90f, 0.90f, 0.75f);
            m_brushNormal->SetColor(LerpColor(normal, hover, m_hoverT));
            brush = m_brushNormal;
        }

        // Draw subtle overlay across the whole hit area (only visible on hover/drag)
        if (m_brushHoverOverlay && (m_hoverT > 0.001f || m_dragging))
        {
            target->FillRectangle(rect, m_brushHoverOverlay.Get());
        }

        if (brush)
        {
            const float baseLineThickness = (std::min)(m_thickness, 3.0f);
            const float lineThickness = baseLineThickness + (m_dragging ? 1.0f : 0.0f) + (m_hoverT * 1.0f);

            if (m_orientation == SplitterOrientation::Horizontal)
            {
                // 좌우 분할: 세로 선
                float centerX = (rect.left + rect.right) * 0.5f;
                D2D1_RECT_F lineRect { centerX - lineThickness * 0.5f, rect.top, centerX + lineThickness * 0.5f, rect.bottom };
                target->FillRectangle(&lineRect, brush.Get());
            }
            else
            {
                // 상하 분할: 가로 선
                float centerY = (rect.top + rect.bottom) * 0.5f;
                D2D1_RECT_F lineRect { rect.left, centerY - lineThickness * 0.5f, rect.right, centerY + lineThickness * 0.5f };
                target->FillRectangle(&lineRect, brush.Get());
            }

            // Grip dots (appear with hover/drag)
            if (m_brushGrip && (m_hoverT > 0.001f || m_dragging))
            {
                const float gripT = Clamp01(m_hoverT + (m_dragging ? 0.35f : 0.0f));
                const float dotAlpha = 0.25f + 0.55f * gripT;
                m_brushGrip->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, dotAlpha));

                const float cx = (rect.left + rect.right) * 0.5f;
                const float cy = (rect.top + rect.bottom) * 0.5f;

                const int dotCount = 5;
                const float dotRadius = 1.35f + 0.25f * gripT;
                const float dotSpacing = 5.0f;

                if (m_orientation == SplitterOrientation::Horizontal)
                {
                    // Vertical splitter: dots stacked along Y
                    const float startY = cy - (static_cast<float>(dotCount - 1) * dotSpacing) * 0.5f;
                    for (int i = 0; i < dotCount; ++i)
                    {
                        const float y = startY + static_cast<float>(i) * dotSpacing;
                        const D2D1_ELLIPSE e { D2D1::Point2F(cx, y), dotRadius, dotRadius };
                        target->FillEllipse(e, m_brushGrip.Get());
                    }
                }
                else
                {
                    // Horizontal splitter: dots stacked along X
                    const float startX = cx - (static_cast<float>(dotCount - 1) * dotSpacing) * 0.5f;
                    for (int i = 0; i < dotCount; ++i)
                    {
                        const float x = startX + static_cast<float>(i) * dotSpacing;
                        const D2D1_ELLIPSE e { D2D1::Point2F(x, cy), dotRadius, dotRadius };
                        target->FillEllipse(e, m_brushGrip.Get());
                    }
                }
            }
        }

        // Keep animating while fade is in progress.
        if (!m_dragging && BackplateRef() != nullptr)
        {
            const float t = (m_hovered ? (1.0f - m_hoverT) : m_hoverT);
            if (t > 0.001f)
            {
                BackplateRef()->RequestAnimationFrame();
            }
        }

        Wnd::OnRender(target);
    }
}

