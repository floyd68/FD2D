#pragma once

#include <d2d1.h>

namespace FD2D::Util
{
    bool RectContainsPoint(const D2D1_RECT_F& r, const POINT& pt);
}

