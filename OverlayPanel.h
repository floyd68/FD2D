#pragma once

#include "Panel.h"

namespace FD2D
{
    class OverlayPanel : public Panel
    {
    public:
        OverlayPanel();
        explicit OverlayPanel(const std::wstring& name);

        Size Measure(Size available) override;
        void Arrange(Rect finalRect) override;
    };
}

