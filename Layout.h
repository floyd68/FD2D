#pragma once

#include <algorithm>
#include <d2d1.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace FD2D
{
    struct Size
    {
        float w { 0.0f };
        float h { 0.0f };
    };

    struct Rect
    {
        float x { 0.0f };
        float y { 0.0f };
        float w { 0.0f };
        float h { 0.0f };
    };

    enum class AlignH
    {
        Start,
        Center,
        End,
        Stretch
    };

    enum class AlignV
    {
        Start,
        Center,
        End,
        Stretch
    };

    inline D2D1_RECT_F ToD2D(const Rect& r)
    {
        return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h);
    }

    inline Rect Inset(const Rect& r, float margin)
    {
        return { r.x + margin, r.y + margin, (std::max)(0.0f, r.w - 2 * margin), (std::max)(0.0f, r.h - 2 * margin) };
    }

    inline Rect CenterRect(const Rect& outer, const Size& inner)
    {
        float x = outer.x + (outer.w - inner.w) * 0.5f;
        float y = outer.y + (outer.h - inner.h) * 0.5f;
        return { x, y, inner.w, inner.h };
    }

    inline Rect AlignRect(const Rect& parent, const Size& child, AlignH h, AlignV v)
    {
        float x = parent.x;
        float y = parent.y;
        float w = (h == AlignH::Stretch) ? parent.w : child.w;
        float hgt = (v == AlignV::Stretch) ? parent.h : child.h;

        switch (h)
        {
        case AlignH::Start: x = parent.x; break;
        case AlignH::Center: x = parent.x + (parent.w - w) * 0.5f; break;
        case AlignH::End: x = parent.x + parent.w - w; break;
        case AlignH::Stretch: x = parent.x; break;
        }

        switch (v)
        {
        case AlignV::Start: y = parent.y; break;
        case AlignV::Center: y = parent.y + (parent.h - hgt) * 0.5f; break;
        case AlignV::End: y = parent.y + parent.h - hgt; break;
        case AlignV::Stretch: y = parent.y; break;
        }

        return { x, y, w, hgt };
    }
}

