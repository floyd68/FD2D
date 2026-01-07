#pragma once

#include "Wnd.h"
#include <functional>
#include <wrl/client.h>

namespace FD2D
{
    enum class SplitterOrientation
    {
        Horizontal,  // 좌우 분할 (세로 선)
        Vertical     // 상하 분할 (가로 선)
    };

    class Splitter : public Wnd
    {
    public:
        Splitter();
        explicit Splitter(const std::wstring& name, SplitterOrientation orientation = SplitterOrientation::Horizontal);

        void SetOrientation(SplitterOrientation orientation);
        SplitterOrientation Orientation() const { return m_orientation; }

        void SetThickness(float thickness);
        float Thickness() const { return m_thickness; }

        void SetHitAreaThickness(float thickness);
        float HitAreaThickness() const { return m_hitAreaThickness; }

        void SetSnapThreshold(float threshold);
        float SnapThreshold() const { return m_snapThreshold; }

        // Update internal ratio without invoking callbacks (used by SplitPanel clamping)
        void SetRatio(float ratio);
        float Ratio() const { return m_currentRatio; }

        void SetParentBounds(const Rect& bounds);
        void OnSplitChanged(std::function<void(float ratio)> handler);

        Size Measure(Size available) override;
        void Arrange(Rect finalRect) override;
        bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam) override;
        void OnRender(ID2D1RenderTarget* target) override;

        bool IsDragging() const { return m_dragging; }

    private:
        bool HitTest(const POINT& pt) const;
        void StartDrag(const POINT& pt);
        void UpdateDrag(const POINT& pt);
        void EndDrag();
        void HandleDoubleClick();
        float CalculateRatio(float position) const;
        Rect GetParentBounds() const;

        SplitterOrientation m_orientation { SplitterOrientation::Horizontal };
        float m_thickness { 4.0f };
        float m_hitAreaThickness { 12.0f };
        float m_snapThreshold { 0.02f };  // 2% 이내면 snap

        bool m_hovered { false };
        bool m_trackingMouseLeave { false };
        bool m_dragging { false };
        POINT m_dragStart {};
        float m_dragStartRatio { 0.5f };
        float m_currentRatio { 0.5f };
        Rect m_dragStartParentBounds {};  // 드래그 시작 시점의 부모 bounds

        // Hover fade animation (0 = normal, 1 = hover).
        float m_hoverT { 0.0f };
        unsigned long long m_lastHoverAnimMs { 0 };
        unsigned int m_hoverFadeMs { 140 };

        std::function<void(float)> m_splitChanged;

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushNormal {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushHoverOverlay {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushDrag {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushGrip {};
    };
}

