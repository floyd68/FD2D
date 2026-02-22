#include "Spinner.h"
#include "Backplate.h"
#include "../CommonUtil.h"
#include <algorithm>
#include <cmath>
#include <windows.h>

namespace FD2D
{
    Spinner::Spinner()
        : Wnd()
    {
    }

    Spinner::Spinner(const std::wstring& name)
        : Wnd(name)
    {
    }

    Size Spinner::Measure(Size available)
    {
        UNREFERENCED_PARAMETER(available);
        m_desired = { 0.0f, 0.0f };
        return m_desired;
    }

    Size Spinner::MinSize() const
    {
        return { 0.0f, 0.0f };
    }

    void Spinner::SetActive(bool active)
    {
        if (m_active == active)
        {
            return;
        }

        m_active = active;
        m_lastAnimMs = 0;
        Invalidate();

        // Ensure the application loop treats animations as active immediately, even before the first paint.
        if (m_active && BackplateRef() != nullptr)
        {
            BackplateRef()->RequestAnimationFrame();
        }
    }

    void Spinner::SetStyle(const Style& style)
    {
        m_style = style;

        // Recreate brushes next draw (color may have changed).
        m_brush.Reset();
        m_dimBrush.Reset();
        Invalidate();
    }

    void Spinner::OnRender(ID2D1RenderTarget* target)
    {
        if (target == nullptr)
        {
            return;
        }

        // Smooth fade in/out to avoid abrupt dim-overlay flashes.
        const unsigned long long now = CommonUtil::NowMs();
        if (m_lastAnimMs == 0)
        {
            m_lastAnimMs = now;
        }
        const unsigned long long dtMs = now - m_lastAnimMs;
        m_lastAnimMs = now;

        const unsigned int fadeMs = (m_style.fadeMs > 0) ? m_style.fadeMs : 100U;
        const float step = static_cast<float>(dtMs) / static_cast<float>(fadeMs);
        if (m_active)
        {
            m_opacity = (std::min)(1.0f, m_opacity + step);
        }
        else
        {
            m_opacity = (std::max)(0.0f, m_opacity - step);
        }

        if (m_opacity <= 0.0f)
        {
            return;
        }

        if (!m_brush)
        {
            (void)target->CreateSolidColorBrush(m_style.color, &m_brush);
        }

        if (!m_brush)
        {
            return;
        }

        const D2D1_RECT_F r = LayoutRect();
        const float w = r.right - r.left;
        const float h = r.bottom - r.top;
        if (!(w > 0.0f && h > 0.0f))
        {
            return;
        }

        if (m_style.dimBackground)
        {
            if (!m_dimBrush)
            {
                (void)target->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &m_dimBrush);
            }

            if (m_dimBrush)
            {
                const float a = CommonUtil::Clamp01(m_style.dimAlpha) * m_opacity;
                m_dimBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, a));
                target->FillRectangle(r, m_dimBrush.Get());
            }
        }

        const float cx = r.left + w * 0.5f;
        const float cy = r.top + h * 0.5f;

        const float baseRadius = (std::max)(
            m_style.minRadius,
            (std::min)(m_style.maxRadius, (std::min)(w, h) * m_style.radiusScaleOfMinDim));
        const float innerRadius = baseRadius * m_style.innerRadiusRatio;
        const float outerRadius = baseRadius;

        const unsigned int period = (m_style.periodMs > 0) ? m_style.periodMs : 900U;
        const unsigned long long t = CommonUtil::NowMs();
        const float phase = static_cast<float>(t % static_cast<unsigned long long>(period)) / static_cast<float>(period);
        const float baseAngle = phase * 6.28318530718f;

        const int ticks = (m_style.ticks >= 3) ? m_style.ticks : 12;

        for (int i = 0; i < ticks; ++i)
        {
            const float a = baseAngle + (static_cast<float>(i) * (6.28318530718f / static_cast<float>(ticks)));
            const float s = std::sinf(a);
            const float c = std::cosf(a);

            // Tail effect: the "leading" tick is brightest.
            const float alpha = 0.15f + (0.85f * (static_cast<float>(i) / static_cast<float>(ticks - 1)));
            m_brush->SetColor(D2D1::ColorF(
                m_style.color.r,
                m_style.color.g,
                m_style.color.b,
                alpha * m_style.color.a * m_opacity));

            const D2D1_POINT_2F p0 { cx + c * innerRadius, cy + s * innerRadius };
            const D2D1_POINT_2F p1 { cx + c * outerRadius, cy + s * outerRadius };
            target->DrawLine(p0, p1, m_brush.Get(), m_style.thickness);
        }

        // Keep animating: request a 60fps tick from the application loop.
        if (BackplateRef() != nullptr)
        {
            BackplateRef()->RequestAnimationFrame();
        }
    }
}


