#pragma once

#include <d2d1.h>

namespace FD2D
{
    enum class InputEventType;
}

namespace FD2D::Util
{
    bool RectContainsPoint(const D2D1_RECT_F& r, const POINT& pt);

    // True for any mouse-related input event (move, buttons, wheel, capture, cursor).
    bool IsMouseInputEventType(InputEventType type);

    // Fits an image of imageSize into layoutRect preserving aspect ratio, centered.
    // rotationQuarters (0-3): for 90°/270° (1/3) the image dimensions are treated as
    // swapped, because the visual footprint after rotation swaps width/height.
    // Returns layoutRect unchanged when either size is not positive.
    D2D1_RECT_F ComputeAspectFitRect(
        const D2D1_RECT_F& layoutRect,
        const D2D1_SIZE_F& imageSize,
        int rotationQuarters = 0);

    // Applies zoom (around the rect center) and pan offset to a rect.
    // When zoomScale == 1, only a pan larger than the epsilon is applied.
    // panX/panY must already be in the same coordinate space as the rect.
    D2D1_RECT_F ApplyZoomPanToRect(
        const D2D1_RECT_F& base,
        float zoomScale,
        float panX,
        float panY);
}

