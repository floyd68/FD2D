#pragma once

#include "Wnd.h"
#include "Core.h"
#include <functional>

namespace FD2D
{
    class Text : public Wnd
    {
    public:
        using ClickHandler = std::function<void()>;

        Text();
        explicit Text(const std::wstring& name);

        Size Measure(Size available) override;
        void SetText(const std::wstring& text);
        void SetColor(const D2D1_COLOR_F& color);
        void SetRect(const D2D1_RECT_F& rect);
        void SetFont(const std::wstring& familyName, FLOAT size = 16.0f);
        void SetFixedWidth(float width);
        void SetTextAlignment(DWRITE_TEXT_ALIGNMENT alignment);
        void SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT alignment);
        void SetEllipsisTrimmingEnabled(bool enabled);
        void SetOnClick(ClickHandler handler);

        void OnRender(ID2D1RenderTarget* target) override;
        bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

    private:
        void EnsureResources(ID2D1RenderTarget* target);

        std::wstring m_text { L"Text" };
        std::wstring m_family { L"Segoe UI" };
        FLOAT m_size { 16.0f };
        D2D1_COLOR_F m_color { D2D1::ColorF(D2D1::ColorF::White) };
        float m_fixedWidth { 0.0f };
        DWRITE_TEXT_ALIGNMENT m_textAlignment { DWRITE_TEXT_ALIGNMENT_LEADING };
        DWRITE_PARAGRAPH_ALIGNMENT m_paragraphAlignment { DWRITE_PARAGRAPH_ALIGNMENT_NEAR };
        bool m_ellipsisTrimmingEnabled { false };
        ClickHandler m_onClick {};

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush {};
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_format {};
        Microsoft::WRL::ComPtr<IDWriteInlineObject> m_ellipsisSign {};
    };
}

