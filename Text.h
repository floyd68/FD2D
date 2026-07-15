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

        // When enabled, this control asks Backplate to show a hover tooltip
        // with the full text whenever the text does not fit its rect (i.e. it
        // is clipped / shown with an ellipsis). Pairs naturally with
        // SetEllipsisTrimmingEnabled for path labels.
        void SetTooltipOnTruncation(bool enabled) { m_tooltipOnTruncation = enabled; }
        // Explicit tooltip string, for labels whose displayed text is already
        // abbreviated by the caller (e.g. a middle-ellipsized path via
        // PathCompactPathEx) so the control itself never sees truncation. When
        // set and different from the displayed text, it is shown on hover.
        void SetTooltipText(const std::wstring& text) { m_tooltipText = text; }
        // When enabled, a right-click on this control copies text to the
        // clipboard (Backplate handles the write + a confirmation toast). By
        // default it copies the displayed text; SetCopyText overrides that
        // with a distinct string (e.g. the full, un-abbreviated path).
        void SetCopyTextOnRightClick(bool enabled) { m_copyTextOnRightClick = enabled; }
        void SetCopyText(const std::wstring& text) { m_copyText = text; }

        std::wstring TooltipText() const override;
        bool TryGetCopyText(std::wstring& out) const override;

        void OnRender(ID2D1RenderTarget* target) override;
        bool OnInputEvent(const InputEvent& event) override;

    private:
        void EnsureResources(ID2D1RenderTarget* target);
        void EnsureFormat();
        void EnsureNaturalSize();
        // True when the laid-out text is narrower than its intrinsic width, so
        // the on-screen text is clipped/ellipsized. Valid after the first
        // render (m_layoutWidth is set there).
        bool IsTruncated() const;

        std::wstring m_text { L"Text" };
        std::wstring m_family { L"Segoe UI" };
        FLOAT m_size { 16.0f };
        D2D1_COLOR_F m_color { D2D1::ColorF(D2D1::ColorF::White) };
        float m_fixedWidth { 0.0f };
        DWRITE_TEXT_ALIGNMENT m_textAlignment { DWRITE_TEXT_ALIGNMENT_LEADING };
        DWRITE_PARAGRAPH_ALIGNMENT m_paragraphAlignment { DWRITE_PARAGRAPH_ALIGNMENT_NEAR };
        bool m_ellipsisTrimmingEnabled { false };
        bool m_tooltipOnTruncation { false };
        bool m_copyTextOnRightClick { false };
        std::wstring m_tooltipText {}; // explicit override for pre-abbreviated labels
        std::wstring m_copyText {};    // explicit override for the copied string
        ClickHandler m_onClick {};

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush {};
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_format {};
        Microsoft::WRL::ComPtr<IDWriteInlineObject> m_ellipsisSign {};
        Microsoft::WRL::ComPtr<IDWriteTextLayout> m_textLayout {};
        float m_layoutWidth { 0.0f };
        float m_layoutHeight { 0.0f };
        bool m_textLayoutDirty { true };

        // Cached result of measuring m_text/m_family/m_size at effectively
        // unbounded width, i.e. its true intrinsic size (see EnsureNaturalSize
        // in Text.cpp for why Measure() needs this instead of the old
        // "fontSize * 1.2" / "charCount * fontSize * 0.6" heuristics).
        Size m_naturalSize { 0.0f, 0.0f };
        bool m_naturalSizeDirty { true };
    };
}

