#pragma once

#include "Wnd.h"
#include "Core.h"

namespace FD2D
{
    class Text : public Wnd
    {
    public:
        Text();
        explicit Text(const std::wstring& name);

        Size Measure(Size available) override;
        void SetText(const std::wstring& text);
        void SetColor(const D2D1_COLOR_F& color);
        void SetRect(const D2D1_RECT_F& rect);
        void SetFont(const std::wstring& familyName, FLOAT size = 16.0f);

        void OnRender(ID2D1RenderTarget* target) override;

    private:
        void EnsureResources(ID2D1RenderTarget* target);

        std::wstring m_text { L"Text" };
        std::wstring m_family { L"Segoe UI" };
        FLOAT m_size { 16.0f };
        D2D1_COLOR_F m_color { D2D1::ColorF(D2D1::ColorF::White) };

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush {};
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_format {};
    };
}

