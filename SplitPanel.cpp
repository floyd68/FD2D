#include "SplitPanel.h"

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

    void SplitPanel::OnSplitRatioChanged(float ratio)
    {
        m_splitRatio = ratio;
        
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
            m_desired = { totalWidth, maxHeight };
        }
        else
        {
            // 상하 분할
            float totalHeight = firstSize.h + splitterSize.h + secondSize.h;
            float maxWidth = (std::max)(firstSize.w, (std::max)(secondSize.w, splitterSize.w));
            m_desired = { maxWidth, totalHeight };
        }

        return m_desired;
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

