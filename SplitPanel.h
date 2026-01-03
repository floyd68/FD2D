#pragma once

#include "Panel.h"
#include "Splitter.h"

namespace FD2D
{
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

        Size Measure(Size available) override;
        void Arrange(Rect finalRect) override;

    private:
        void OnSplitRatioChanged(float ratio);

        SplitterOrientation m_orientation { SplitterOrientation::Horizontal };
        float m_splitRatio { 0.5f };
        std::shared_ptr<Wnd> m_firstChild {};
        std::shared_ptr<Wnd> m_secondChild {};
        std::shared_ptr<Splitter> m_splitter {};
    };
}

