#include "GridPanel.h"

namespace FD2D
{
    GridPanel::GridPanel()
        : Panel()
    {
    }

    GridPanel::GridPanel(const std::wstring& name)
        : Panel(name)
    {
    }

    void GridPanel::SetColumns(const std::vector<GridLength>& columns)
    {
        if (!columns.empty())
        {
            m_columns = columns;
        }
    }

    void GridPanel::SetRows(const std::vector<GridLength>& rows)
    {
        if (!rows.empty())
        {
            m_rows = rows;
        }
    }

    void GridPanel::SetChildCell(const std::shared_ptr<Wnd>& child, int col, int row, int colSpan, int rowSpan)
    {
        if (!child)
        {
            return;
        }

        GridCell cell {};
        cell.col = (std::max)(0, col);
        cell.row = (std::max)(0, row);
        cell.colSpan = (std::max)(1, colSpan);
        cell.rowSpan = (std::max)(1, rowSpan);
        m_cells[child.get()] = cell;
    }

    static float SumStar(const std::vector<GridLength>& defs)
    {
        float total = 0.0f;
        for (auto& d : defs)
        {
            if (d.type == GridLength::Type::Star)
            {
                total += d.value;
            }
        }
        return total;
    }

    static float SumFixedAuto(const std::vector<GridLength>& defs, const std::vector<float>& autoSizes)
    {
        float total = 0.0f;
        for (size_t i = 0; i < defs.size(); ++i)
        {
            if (defs[i].type == GridLength::Type::Fixed)
            {
                total += defs[i].value;
            }
            else if (defs[i].type == GridLength::Type::Auto)
            {
                total += autoSizes[i];
            }
        }
        return total;
    }

    Size GridPanel::Measure(Size available)
    {
        std::vector<float> colAuto(m_columns.size(), 0.0f);
        std::vector<float> rowAuto(m_rows.size(), 0.0f);

        // Measure each child and collect auto sizes.
        for (auto& child : ChildrenInOrder())
        {
            if (!child)
            {
                continue;
            }

            const GridCell cell = m_cells.count(child.get()) ? m_cells[child.get()] : GridCell {};

            Size childAvail { available.w, available.h };
            Size desired = child->Measure(childAvail);

            // Only handle span == 1 for auto sizing.
            if (cell.col < static_cast<int>(colAuto.size()) && cell.colSpan == 1 && m_columns[cell.col].type == GridLength::Type::Auto)
            {
                colAuto[cell.col] = (std::max)(colAuto[cell.col], desired.w);
            }
            if (cell.row < static_cast<int>(rowAuto.size()) && cell.rowSpan == 1 && m_rows[cell.row].type == GridLength::Type::Auto)
            {
                rowAuto[cell.row] = (std::max)(rowAuto[cell.row], desired.h);
            }
        }

        float totalStarW = SumStar(m_columns);
        float totalStarH = SumStar(m_rows);

        float fixedAutoW = SumFixedAuto(m_columns, colAuto);
        float fixedAutoH = SumFixedAuto(m_rows, rowAuto);

        float remainW = (std::max)(0.0f, available.w - fixedAutoW);
        float remainH = (std::max)(0.0f, available.h - fixedAutoH);

        float width = fixedAutoW + (totalStarW > 0.0f ? remainW : 0.0f);
        float height = fixedAutoH + (totalStarH > 0.0f ? remainH : 0.0f);

        m_desired = { width, height };
        return m_desired;
    }

    void GridPanel::Arrange(Rect finalRect)
    {
        size_t colCount = m_columns.empty() ? 1 : m_columns.size();
        size_t rowCount = m_rows.empty() ? 1 : m_rows.size();

        std::vector<float> colAuto(colCount, 0.0f);
        std::vector<float> rowAuto(rowCount, 0.0f);

        // Use desired sizes for auto columns/rows
        for (auto& child : ChildrenInOrder())
        {
            if (!child)
            {
                continue;
            }

            const GridCell cell = m_cells.count(child.get()) ? m_cells[child.get()] : GridCell {};
            Size desired = child->Measure({ finalRect.w, finalRect.h });

            if (cell.col < static_cast<int>(colCount) && cell.colSpan == 1 && m_columns[cell.col].type == GridLength::Type::Auto)
            {
                colAuto[cell.col] = (std::max)(colAuto[cell.col], desired.w);
            }
            if (cell.row < static_cast<int>(rowCount) && cell.rowSpan == 1 && m_rows[cell.row].type == GridLength::Type::Auto)
            {
                rowAuto[cell.row] = (std::max)(rowAuto[cell.row], desired.h);
            }
        }

        float totalStarW = SumStar(m_columns);
        float totalStarH = SumStar(m_rows);

        float fixedAutoW = SumFixedAuto(m_columns, colAuto);
        float fixedAutoH = SumFixedAuto(m_rows, rowAuto);

        float remainW = (std::max)(0.0f, finalRect.w - fixedAutoW);
        float remainH = (std::max)(0.0f, finalRect.h - fixedAutoH);

        std::vector<float> colWidths(colCount, 0.0f);
        std::vector<float> rowHeights(rowCount, 0.0f);

        for (size_t i = 0; i < colCount; ++i)
        {
            if (m_columns[i].type == GridLength::Type::Fixed)
            {
                colWidths[i] = m_columns[i].value;
            }
            else if (m_columns[i].type == GridLength::Type::Auto)
            {
                colWidths[i] = colAuto[i];
            }
            else
            {
                colWidths[i] = totalStarW > 0.0f ? remainW * (m_columns[i].value / totalStarW) : 0.0f;
            }
        }

        for (size_t i = 0; i < rowCount; ++i)
        {
            if (m_rows[i].type == GridLength::Type::Fixed)
            {
                rowHeights[i] = m_rows[i].value;
            }
            else if (m_rows[i].type == GridLength::Type::Auto)
            {
                rowHeights[i] = rowAuto[i];
            }
            else
            {
                rowHeights[i] = totalStarH > 0.0f ? remainH * (m_rows[i].value / totalStarH) : 0.0f;
            }
        }

        // Prefix sums
        std::vector<float> colOffsets(colCount + 1, 0.0f);
        for (size_t i = 0; i < colCount; ++i)
        {
            colOffsets[i + 1] = colOffsets[i] + colWidths[i];
        }

        std::vector<float> rowOffsets(rowCount + 1, 0.0f);
        for (size_t i = 0; i < rowCount; ++i)
        {
            rowOffsets[i + 1] = rowOffsets[i] + rowHeights[i];
        }

        for (auto& child : ChildrenInOrder())
        {
            if (!child)
            {
                continue;
            }

            const GridCell cell = m_cells.count(child.get()) ? m_cells[child.get()] : GridCell {};

            size_t c = (std::min)(static_cast<size_t>(cell.col), colCount - 1);
            size_t r = (std::min)(static_cast<size_t>(cell.row), rowCount - 1);
            size_t cs = (std::min)(static_cast<size_t>(cell.colSpan), colCount - c);
            size_t rs = (std::min)(static_cast<size_t>(cell.rowSpan), rowCount - r);

            float x = finalRect.x + colOffsets[c];
            float y = finalRect.y + rowOffsets[r];
            float w = colOffsets[c + cs] - colOffsets[c];
            float h = rowOffsets[r + rs] - rowOffsets[r];

            child->Arrange({ x, y, w, h });
        }

        m_bounds = finalRect;
        m_layoutRect = ToD2D(finalRect);
    }
}

