#pragma once

#include <d2d1.h>
#include <windows.h>
#include <cstdint>

namespace FD2D
{
    enum class InputEventType;
}

namespace FD2D::Util
{
    unsigned long long NowMs();
    float Clamp01(float v);

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

    // Maps clipRect from the frame an image is drawn in into the image's
    // pre-rotation frame: Image rotates its pre-rotation drawing by
    // rotationQuarters * 90 degrees (clockwise on screen) around the center of
    // layoutRect, so the clip is rotated back around that center. Exact for
    // quarter turns; for odd rotationQuarters the width and height swap.
    D2D1_RECT_F MapRectToPreRotationFrame(
        const D2D1_RECT_F& clipRect,
        const D2D1_RECT_F& layoutRect,
        int rotationQuarters);

    // The alpha checkerboard behind an image, in the image's pre-rotation
    // frame. The grid origin is (floor(destRect.left / tile) * tile,
    // floor(destRect.top / tile) * tile); tile (i, j) covers
    // [originX + tile * i, originX + tile * (i + 1)) x
    // [originY + tile * j, originY + tile * (j + 1)), and is light when
    // (i + j) is even and dark otherwise. The covered area is the union of the
    // tiles that overlap destRect. ComputeCheckerboardTiles limits it to the
    // tiles that also overlap visibleRect: columns [firstColumn, endColumn)
    // and rows [firstRow, endRow).
    struct CheckerboardTiles
    {
        float originX { 0.0f };
        float originY { 0.0f };
        float tileSize { 8.0f };
        int firstColumn { 0 };
        int endColumn { 0 };
        int firstRow { 0 };
        int endRow { 0 };

        bool Empty() const { return endColumn <= firstColumn || endRow <= firstRow; }
        // The visible tiles' bounds (whole tiles; the caller clips).
        D2D1_RECT_F Bounds() const;
        // Origin of the two-tile-period pattern, anchored at the even column
        // and row at or before the first visible tile: the same phase as the
        // grid origin, but next to the visible area, so the anchor stays small
        // at any zoom.
        D2D1_POINT_2F PatternOrigin() const;
    };

    CheckerboardTiles ComputeCheckerboardTiles(
        const D2D1_RECT_F& destRect,
        const D2D1_RECT_F& visibleRect,
        float tileSize = 8.0f);
}

