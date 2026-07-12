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

    // Per-side insets used by composite controls (Button/ComboBox/etc.) for
    // the gap between chrome and their embedded content (usually Text).
    // Distinct from Wnd::SetPadding, which only insets child Wnds in the tree.
    struct Thickness
    {
        float left { 0.0f };
        float top { 0.0f };
        float right { 0.0f };
        float bottom { 0.0f };

        Thickness() = default;
        explicit Thickness(float uniform)
            : left(uniform), top(uniform), right(uniform), bottom(uniform)
        {
        }
        Thickness(float horizontal, float vertical)
            : left(horizontal), top(vertical), right(horizontal), bottom(vertical)
        {
        }
        Thickness(float l, float t, float r, float b)
            : left(l), top(t), right(r), bottom(b)
        {
        }

        float Horizontal() const { return left + right; }
        float Vertical() const { return top + bottom; }
    };

    inline D2D1_RECT_F ToD2D(const Rect& r)
    {
        return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h);
    }

    inline Rect FromD2D(const D2D1_RECT_F& r)
    {
        return { r.left, r.top, r.right - r.left, r.bottom - r.top };
    }

    inline Rect Inset(const Rect& r, float margin)
    {
        return { r.x + margin, r.y + margin, (std::max)(0.0f, r.w - 2 * margin), (std::max)(0.0f, r.h - 2 * margin) };
    }

    inline Rect Inset(const Rect& r, const Thickness& t)
    {
        return {
            r.x + t.left,
            r.y + t.top,
            (std::max)(0.0f, r.w - t.left - t.right),
            (std::max)(0.0f, r.h - t.top - t.bottom)
        };
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

    // Inset `bounds` by `contentMargin`, then place a content of `contentSize`
    // inside the remaining area using AlignH/AlignV.
    inline Rect LayoutContent(const Rect& bounds, const Size& contentSize,
        const Thickness& contentMargin, AlignH h, AlignV v)
    {
        return AlignRect(Inset(bounds, contentMargin), contentSize, h, v);
    }
}

