#pragma once

#include "Panel.h"
#include <vector>
#include <unordered_map>

namespace FD2D
{
    struct GridLength
    {
        enum class Type
        {
            Auto,
            Fixed,
            Star
        };

        Type type { Type::Star };
        float value { 1.0f };
    };

    struct GridCell
    {
        int col { 0 };
        int row { 0 };
        int colSpan { 1 };
        int rowSpan { 1 };
    };

    class GridPanel : public Panel
    {
    public:
        GridPanel();
        explicit GridPanel(const std::wstring& name);

        void SetColumns(const std::vector<GridLength>& columns);
        void SetRows(const std::vector<GridLength>& rows);
        void SetChildCell(const std::shared_ptr<Wnd>& child, int col, int row, int colSpan = 1, int rowSpan = 1);

        Size Measure(Size available) override;
        void Arrange(Rect finalRect) override;

    private:
        std::vector<GridLength> m_columns { GridLength { GridLength::Type::Star, 1.0f } };
        std::vector<GridLength> m_rows { GridLength { GridLength::Type::Star, 1.0f } };
        std::unordered_map<const Wnd*, GridCell> m_cells {};
    };
}

