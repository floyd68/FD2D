#pragma once

#include "Wnd.h"
#include <wrl/client.h>

namespace FD2D
{
    class Spinner final : public Wnd
    {
    public:
        struct Style
        {
            // Base spinner color (alpha is treated as a multiplier).
            D2D1_COLOR_F color { 1.0f, 1.0f, 1.0f, 1.0f };
            float thickness { 2.0f };
            int ticks { 12 };
            // Rotation period in milliseconds.
            unsigned int periodMs { 900 };

            // Size control
            float minRadius { 10.0f };
            float maxRadius { 24.0f };
            float radiusScaleOfMinDim { 0.06f }; // based on min(w,h)
            float innerRadiusRatio { 0.55f };     // inner = outer * ratio

            // Optional dim overlay behind the spinner (helps readability).
            bool dimBackground { false };
            float dimAlpha { 0.25f };

            // Soft fade-in/out for better UX (prevents 1-frame "flash" when toggling active).
            unsigned int fadeMs { 100 };
        };

        Spinner();
        explicit Spinner(const std::wstring& name);
        ~Spinner() override = default;

        Size Measure(Size available) override;
        Size MinSize() const override;
        void OnRender(ID2D1RenderTarget* target) override;

        void SetActive(bool active);
        bool Active() const { return m_active; }

        void SetStyle(const Style& style);
        const Style& GetStyle() const { return m_style; }

    private:
        bool m_active { false };
        float m_opacity { 0.0f };
        unsigned long long m_lastAnimMs { 0 };
        Style m_style {};

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush {};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_dimBrush {};
    };
}


