#pragma once

#include <d2d1.h>

namespace FD2D
{
    struct SelectionStyle
    {
        D2D1_COLOR_F accent { 1.0f, 0.60f, 0.24f, 1.0f };
        D2D1_COLOR_F shadow { 0.0f, 0.0f, 0.0f, 0.55f };
        D2D1_COLOR_F fill { 1.0f, 1.0f, 1.0f, 1.0f };

        float radius { 6.0f };
        float baseInflate { 1.0f };
        float popInflate { 4.0f };
        float shadowThickness { 3.0f };
        float accentThickness { 2.0f };
        float fillMaxAlpha { 0.10f };

        bool breatheEnabled { true };
        int breathePeriodMs { 900 };
        float breatheInflateAmp { 1.40f };
        float breatheThicknessAmp { 1.10f };
        float breatheAlphaAmp { 0.25f };
    };
}

