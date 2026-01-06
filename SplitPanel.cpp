#include "SplitPanel.h"
#include <algorithm>

namespace FD2D
{
    SplitPanel::SplitPanel()
        : Panel()
    {
        m_splitter = std::make_shared<Splitter>(L"splitter", SplitterOrientation::Horizontal);
        m_splitter->OnSplitChanged([this](float ratio)
        {
            OnSplitRatioChanged(ratio);
        });
        AddChild(m_splitter);
    }

    SplitPanel::SplitPanel(const std::wstring& name, SplitterOrientation orientation)
        : Panel(name)
        , m_orientation(orientation)
    {
        m_splitter = std::make_shared<Splitter>(L"splitter", orientation);
        m_splitter->OnSplitChanged([this](float ratio)
        {
            OnSplitRatioChanged(ratio);
        });
        AddChild(m_splitter);
    }

    void SplitPanel::SetOrientation(SplitterOrientation orientation)
    {
        m_orientation = orientation;
        if (m_splitter)
        {
            m_splitter->SetOrientation(orientation);
        }
        Invalidate();
    }

    void SplitPanel::SetFirstChild(const std::shared_ptr<Wnd>& child)
    {
        m_firstChild = child;
        if (m_firstChild && !m_firstChild->Name().empty())
        {
            // 이미 추가되어 있지 않으면 추가
            if (Children().find(m_firstChild->Name()) == Children().end())
            {
                AddChild(m_firstChild);
            }
        }
    }

    void SplitPanel::SetSecondChild(const std::shared_ptr<Wnd>& child)
    {
        m_secondChild = child;
        if (m_secondChild && !m_secondChild->Name().empty())
        {
            // 이미 추가되어 있지 않으면 추가
            if (Children().find(m_secondChild->Name()) == Children().end())
            {
                AddChild(m_secondChild);
            }
        }
    }

    void SplitPanel::SetSplitRatio(float ratio)
    {
        m_splitRatio = (std::max)(0.0f, (std::min)(1.0f, ratio));
        if (m_splitter)
        {
            // Splitter의 현재 비율 업데이트 (내부적으로만)
            // 실제 Arrange에서 반영됨
        }
        Invalidate();
    }

    void SplitPanel::SetFirstPaneMinExtent(float extent)
    {
        m_firstPaneMinExtent = (std::max)(0.0f, extent);
        Invalidate();
    }

    void SplitPanel::SetFirstPaneMaxExtent(float extent)
    {
        m_firstPaneMaxExtent = (std::max)(0.0f, extent);
        Invalidate();
    }

    void SplitPanel::SetSecondPaneMinExtent(float extent)
    {
        m_secondPaneMinExtent = (std::max)(0.0f, extent);
        Invalidate();
    }

    void SplitPanel::SetSecondPaneMaxExtent(float extent)
    {
        m_secondPaneMaxExtent = (std::max)(0.0f, extent);
        Invalidate();
    }

    void SplitPanel::SetConstraintPropagation(ConstraintPropagation policy)
    {
        m_propagation = policy;
        Invalidate();
    }

    float SplitPanel::ClampRatioForPaneConstraints(const Rect& childArea, float splitterExtent, float ratio) const
    {
        float availableExtent = 0.0f;
        if (m_orientation == SplitterOrientation::Horizontal)
        {
            availableExtent = childArea.w - splitterExtent;
        }
        else
        {
            availableExtent = childArea.h - splitterExtent;
        }

        if (availableExtent <= 0.0f)
        {
            return (std::max)(0.0f, (std::min)(1.0f, ratio));
        }

        // Convert constraints into a ratio range.
        // firstExtent  = availableExtent * ratio
        // secondExtent = availableExtent * (1 - ratio)
        float minRatio = 0.0f;
        float maxRatio = 1.0f;

        const float firstMin = m_firstPaneMinExtent;
        const float firstMax = m_firstPaneMaxExtent;
        const float secondMin = m_secondPaneMinExtent;
        const float secondMax = m_secondPaneMaxExtent;

        if (firstMin > 0.0f)
        {
            minRatio = (std::max)(minRatio, firstMin / availableExtent);
        }
        if (firstMax > 0.0f)
        {
            maxRatio = (std::min)(maxRatio, firstMax / availableExtent);
        }

        if (secondMin > 0.0f)
        {
            maxRatio = (std::min)(maxRatio, 1.0f - (secondMin / availableExtent));
        }
        if (secondMax > 0.0f)
        {
            minRatio = (std::max)(minRatio, 1.0f - (secondMax / availableExtent));
        }

        minRatio = (std::max)(0.0f, (std::min)(1.0f, minRatio));
        maxRatio = (std::max)(0.0f, (std::min)(1.0f, maxRatio));

        float clamped = ratio;

        // If constraints are feasible, clamp directly.
        if (minRatio <= maxRatio)
        {
            clamped = (std::max)(minRatio, (std::min)(maxRatio, clamped));
            return clamped;
        }

        // Best-effort when constraints conflict: treat mins as hard, relax maxes.
        clamped = (std::max)(0.0f, (std::min)(1.0f, clamped));
        if (firstMin > 0.0f)
        {
            clamped = (std::max)(clamped, firstMin / availableExtent);
        }
        if (secondMin > 0.0f)
        {
            clamped = (std::min)(clamped, 1.0f - (secondMin / availableExtent));
        }
        clamped = (std::max)(0.0f, (std::min)(1.0f, clamped));
        return clamped;
    }

    void SplitPanel::OnSplitRatioChanged(float ratio)
    {
        // Clamp ratio based on current bounds and second-pane constraints
        Rect inset = Inset(m_bounds, m_margin);
        Rect childArea = Inset(inset, m_padding);

        float splitterExtent = 0.0f;
        if (m_splitter)
        {
            Size s = m_splitter->Measure({ childArea.w, childArea.h });
            splitterExtent = (m_orientation == SplitterOrientation::Horizontal) ? s.w : s.h;
        }

        float clamped = ClampRatioForPaneConstraints(childArea, splitterExtent, ratio);
        m_splitRatio = clamped;
        if (m_splitter)
        {
            m_splitter->SetRatio(clamped);
        }
        
        // 레이아웃을 다시 계산하기 위해 Measure/Arrange 호출
        if (BackplateRef() && m_bounds.w > 0.0f && m_bounds.h > 0.0f)
        {
            // 현재 bounds를 사용하여 레이아웃 재계산
            Measure({ m_bounds.w, m_bounds.h });
            Arrange(m_bounds);
        }
        
        Invalidate();
    }

    Size SplitPanel::Measure(Size available)
    {
        // 자식들의 desired size를 계산
        Size firstSize {};
        Size secondSize {};
        Size splitterSize {};

        if (m_firstChild)
        {
            firstSize = m_firstChild->Measure(available);
        }
        if (m_secondChild)
        {
            secondSize = m_secondChild->Measure(available);
        }
        if (m_splitter)
        {
            splitterSize = m_splitter->Measure(available);
        }

        if (m_orientation == SplitterOrientation::Horizontal)
        {
            // 좌우 분할
            float totalWidth = firstSize.w + splitterSize.w + secondSize.w;
            float maxHeight = (std::max)(firstSize.h, (std::max)(secondSize.h, splitterSize.h));

            // Upward constraint propagation (min width)
            if (m_propagation != ConstraintPropagation::None)
            {
                float minFirst = m_firstPaneMinExtent;
                float minSecond = m_secondPaneMinExtent;
                float minTotal = minFirst + splitterSize.w + minSecond;
                totalWidth = (std::max)(totalWidth, minTotal);
            }

            m_desired = { totalWidth, maxHeight };
        }
        else
        {
            // 상하 분할
            float totalHeight = firstSize.h + splitterSize.h + secondSize.h;
            float maxWidth = (std::max)(firstSize.w, (std::max)(secondSize.w, splitterSize.w));

            // Upward constraint propagation (min height)
            if (m_propagation != ConstraintPropagation::None)
            {
                float minFirst = m_firstPaneMinExtent;
                float minSecond = m_secondPaneMinExtent;
                float minTotal = minFirst + splitterSize.h + minSecond;
                totalHeight = (std::max)(totalHeight, minTotal);
            }

            m_desired = { maxWidth, totalHeight };
        }

        return m_desired;
    }

    Size SplitPanel::MinSize() const
    {
        // Child intrinsic mins (may be 0 for many leaf controls)
        Size firstMin {};
        Size secondMin {};
        if (m_firstChild)
        {
            firstMin = m_firstChild->MinSize();
        }
        if (m_secondChild)
        {
            secondMin = m_secondChild->MinSize();
        }

        const float splitterExtent = m_splitter ? m_splitter->HitAreaThickness() : 0.0f;

        // Apply SplitPanel constraints only when propagation is enabled.
        const bool propagate = (m_propagation != ConstraintPropagation::None);

        float minW = 0.0f;
        float minH = 0.0f;

        if (m_orientation == SplitterOrientation::Horizontal)
        {
            float a = firstMin.w;
            float b = secondMin.w;
            if (propagate)
            {
                a = (std::max)(a, m_firstPaneMinExtent);
                b = (std::max)(b, m_secondPaneMinExtent);
            }
            minW = a + splitterExtent + b;
            minH = (std::max)(firstMin.h, secondMin.h);
        }
        else
        {
            float a = firstMin.h;
            float b = secondMin.h;
            if (propagate)
            {
                a = (std::max)(a, m_firstPaneMinExtent);
                b = (std::max)(b, m_secondPaneMinExtent);
            }
            minH = a + splitterExtent + b;
            minW = (std::max)(firstMin.w, secondMin.w);
        }

        // Include this node's margin/padding (same convention as Wnd::Arrange)
        minW += 2.0f * m_margin + 2.0f * m_padding;
        minH += 2.0f * m_margin + 2.0f * m_padding;
        return { minW, minH };
    }

    void SplitPanel::Arrange(Rect finalRect)
    {
        Rect inset = Inset(finalRect, m_margin);
        Rect childArea = Inset(inset, m_padding);

        if (m_orientation == SplitterOrientation::Horizontal)
        {
            // 좌우 분할
            float totalWidth = childArea.w;
            float splitterWidth = m_splitter ? m_splitter->Measure({ childArea.w, childArea.h }).w : 0.0f;
            float availableWidth = totalWidth - splitterWidth;

            m_splitRatio = ClampRatioForPaneConstraints(childArea, splitterWidth, m_splitRatio);
            if (m_splitter)
            {
                m_splitter->SetRatio(m_splitRatio);
            }

            float firstWidth = availableWidth * m_splitRatio;
            float secondWidth = availableWidth * (1.0f - m_splitRatio);

            float x = childArea.x;

            // First child
            if (m_firstChild)
            {
                Rect firstRect { x, childArea.y, firstWidth, childArea.h };
                m_firstChild->Arrange(firstRect);
                x += firstWidth;
            }

            // Splitter
            if (m_splitter)
            {
                Rect splitterRect { x, childArea.y, splitterWidth, childArea.h };
                m_splitter->SetParentBounds(childArea);
                m_splitter->Arrange(splitterRect);
                x += splitterWidth;
            }

            // Second child
            if (m_secondChild)
            {
                Rect secondRect { x, childArea.y, secondWidth, childArea.h };
                m_secondChild->Arrange(secondRect);
            }
        }
        else
        {
            // 상하 분할
            float totalHeight = childArea.h;
            float splitterHeight = m_splitter ? m_splitter->Measure({ childArea.w, childArea.h }).h : 0.0f;
            float availableHeight = totalHeight - splitterHeight;

            m_splitRatio = ClampRatioForPaneConstraints(childArea, splitterHeight, m_splitRatio);
            if (m_splitter)
            {
                m_splitter->SetRatio(m_splitRatio);
            }

            float firstHeight = availableHeight * m_splitRatio;
            float secondHeight = availableHeight * (1.0f - m_splitRatio);

            float y = childArea.y;

            // First child
            if (m_firstChild)
            {
                Rect firstRect { childArea.x, y, childArea.w, firstHeight };
                m_firstChild->Arrange(firstRect);
                y += firstHeight;
            }

            // Splitter
            if (m_splitter)
            {
                Rect splitterRect { childArea.x, y, childArea.w, splitterHeight };
                m_splitter->SetParentBounds(childArea);
                m_splitter->Arrange(splitterRect);
                y += splitterHeight;
            }

            // Second child
            if (m_secondChild)
            {
                Rect secondRect { childArea.x, y, childArea.w, secondHeight };
                m_secondChild->Arrange(secondRect);
            }
        }

        m_bounds = finalRect;
        m_layoutRect = ToD2D(finalRect);
    }
}

