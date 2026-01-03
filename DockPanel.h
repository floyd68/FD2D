#pragma once

#include "Panel.h"
#include <unordered_map>

namespace FD2D
{
    enum class Dock
    {
        Left,
        Top,
        Right,
        Bottom,
        Fill
    };

    class DockPanel : public Panel
    {
    public:
        DockPanel();
        explicit DockPanel(const std::wstring& name);

        void SetChildDock(const std::shared_ptr<Wnd>& child, Dock dock);

        Size Measure(Size available) override;
        void Arrange(Rect finalRect) override;

    private:
        std::unordered_map<const Wnd*, Dock> m_docks {};
        std::vector<std::shared_ptr<Wnd>> m_order {};
    };
}

