#pragma once

#include "Wnd.h"
#include "Panel.h"

namespace FD2D
{
    enum class Orientation
    {
        Vertical,
        Horizontal
    };

    class StackPanel : public Panel
    {
    public:
        StackPanel();
        explicit StackPanel(const std::wstring& name, Orientation orientation = Orientation::Vertical);

        void SetOrientation(Orientation o);

        Size Measure(Size available) override;
        void Arrange(Rect finalRect) override;

    private:
        Orientation m_orientation { Orientation::Vertical };
    };
}

