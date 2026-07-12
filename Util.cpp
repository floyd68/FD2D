#include "Util.h"
#include "Wnd.h"
#include <cmath>

namespace FD2D::Util
{
    unsigned long long NowMs()
    {
        return static_cast<unsigned long long>(GetTickCount64());
    }

    float Clamp01(float v)
    {
        if (v < 0.0f)
        {
            return 0.0f;
        }
        if (v > 1.0f)
        {
            return 1.0f;
        }
        return v;
    }

    bool RectContainsPoint(const D2D1_RECT_F& r, const POINT& pt)
    {
        return pt.x >= r.left &&
            pt.x <= r.right &&
            pt.y >= r.top &&
            pt.y <= r.bottom;
    }

    bool IsMouseInputEventType(InputEventType type)
    {
        switch (type)
        {
        case InputEventType::MouseMove:
        case InputEventType::MouseDown:
        case InputEventType::MouseUp:
        case InputEventType::MouseDoubleClick:
        case InputEventType::MouseWheel:
        case InputEventType::MouseHWheel:
        case InputEventType::MouseLeave:
        case InputEventType::CaptureChanged:
        case InputEventType::SetCursor:
            return true;
        default:
            return false;
        }
    }

    D2D1_RECT_F ComputeAspectFitRect(
        const D2D1_RECT_F& layoutRect,
        const D2D1_SIZE_F& imageSize,
        int rotationQuarters)
    {
        const float layoutWidth = layoutRect.right - layoutRect.left;
        const float layoutHeight = layoutRect.bottom - layoutRect.top;

        if (!(layoutWidth > 0.0f && layoutHeight > 0.0f &&
              imageSize.width > 0.0f && imageSize.height > 0.0f))
        {
            return layoutRect;
        }

        // The returned rect is used as the *pre-rotation* draw/destination rect: the caller
        // draws the (unrotated) bitmap into it and then rotates the whole rect on screen.
        // For 90°/270° that on-screen (post-rotation) bounding box has swapped width/height
        // compared to the draw rect, so we must aspect-fit using the swapped dimensions and
        // then swap the resulting size back to get the actual pre-rotation draw rect.
        // Getting this backwards (fitting with the draw rect's own aspect ratio) would leave
        // the post-rotation bounding box mismatched from the image's true aspect ratio and
        // visibly stretch/squash the image.
        const bool swapDims = (rotationQuarters == 1 || rotationQuarters == 3);
        const float effectiveW = swapDims ? imageSize.height : imageSize.width;
        const float effectiveH = swapDims ? imageSize.width : imageSize.height;

        const float rotatedAspect = effectiveW / effectiveH;
        const float layoutAspect = layoutWidth / layoutHeight;

        // Size of the on-screen (post-rotation) bounding box, aspect-fit within layoutRect.
        float rotatedBoxW;
        float rotatedBoxH;
        if (rotatedAspect > layoutAspect)
        {
            rotatedBoxW = layoutWidth;
            rotatedBoxH = layoutWidth / rotatedAspect;
        }
        else
        {
            rotatedBoxH = layoutHeight;
            rotatedBoxW = layoutHeight * rotatedAspect;
        }

        // Swap back to get the pre-rotation draw rect size (matches the image's true aspect ratio).
        const float drawW = swapDims ? rotatedBoxH : rotatedBoxW;
        const float drawH = swapDims ? rotatedBoxW : rotatedBoxH;

        const float centerX = (layoutRect.left + layoutRect.right) * 0.5f;
        const float centerY = (layoutRect.top + layoutRect.bottom) * 0.5f;

        return D2D1_RECT_F
        {
            centerX - drawW * 0.5f,
            centerY - drawH * 0.5f,
            centerX + drawW * 0.5f,
            centerY + drawH * 0.5f
        };
    }

    D2D1_RECT_F ApplyZoomPanToRect(
        const D2D1_RECT_F& base,
        float zoomScale,
        float panX,
        float panY)
    {
        D2D1_RECT_F rect = base;
        if (zoomScale != 1.0f)
        {
            const float centerX = (base.left + base.right) * 0.5f;
            const float centerY = (base.top + base.bottom) * 0.5f;
            const float scaledWidth = (base.right - base.left) * zoomScale;
            const float scaledHeight = (base.bottom - base.top) * zoomScale;
            rect.left = centerX - scaledWidth * 0.5f + panX;
            rect.right = rect.left + scaledWidth;
            rect.top = centerY - scaledHeight * 0.5f + panY;
            rect.bottom = rect.top + scaledHeight;
        }
        else if (std::abs(panX) > 0.001f || std::abs(panY) > 0.001f)
        {
            // Apply pan even when not zoomed (though this shouldn't normally happen).
            rect.left += panX;
            rect.right += panX;
            rect.top += panY;
            rect.bottom += panY;
        }
        return rect;
    }
}

