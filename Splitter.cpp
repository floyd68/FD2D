#include "Splitter.h"
#include "Backplate.h"
#include <windowsx.h>
#include <algorithm>

namespace FD2D
{
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
                Invalidate();
            }

            if (m_dragging)
            {
                UpdateDrag(pt);
                return true;
            }

            return m_hovered;
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
        if (!m_brushHover)
        {
            target->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.8f), &m_brushHover);
        }
        if (!m_brushDrag)
        {
            target->CreateSolidColorBrush(D2D1::ColorF(0.7f, 0.7f, 0.7f, 1.0f), &m_brushDrag);
        }

        // Splitter 그리기
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush = m_brushNormal;
        if (m_dragging)
        {
            brush = m_brushDrag;
        }
        else if (m_hovered)
        {
            brush = m_brushHover;
        }

        if (brush)
        {
            if (m_orientation == SplitterOrientation::Horizontal)
            {
                // 좌우 분할: 세로 선
                float centerX = (rect.left + rect.right) * 0.5f;
                D2D1_RECT_F lineRect { centerX - m_thickness * 0.5f, rect.top, centerX + m_thickness * 0.5f, rect.bottom };
                target->FillRectangle(&lineRect, brush.Get());
            }
            else
            {
                // 상하 분할: 가로 선
                float centerY = (rect.top + rect.bottom) * 0.5f;
                D2D1_RECT_F lineRect { rect.left, centerY - m_thickness * 0.5f, rect.right, centerY + m_thickness * 0.5f };
                target->FillRectangle(&lineRect, brush.Get());
            }
        }

        Wnd::OnRender(target);
    }
}

