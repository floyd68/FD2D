#pragma once

#include "Wnd.h"

namespace FD2D
{
    class Panel : public Wnd
    {
    public:
        Panel();
        explicit Panel(const std::wstring& name);
        ~Panel() override = default;

        void SetSpacing(float spacing);
        float Spacing() const;

        Size Measure(Size available) override;
        void Arrange(Rect finalRect) override;

    protected:
        float m_spacing { 0.0f };
    };
}

