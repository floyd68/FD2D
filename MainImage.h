#pragma once

#include "Image.h"

namespace FD2D
{
    // Main image control (explicit type for clarity).
    // Currently reuses the full-featured Image implementation (zoom/pan/GPU path).
    class MainImage : public Image
    {
    public:
        using Image::Image;
    };
}

