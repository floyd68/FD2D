#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Layout.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <memory>
#include <unordered_map>
#include <string>

namespace FD2D
{
    class Backplate;

    class Wnd : public std::enable_shared_from_this<Wnd>
    {
    public:
        Wnd();
        explicit Wnd(const std::wstring& name);
        virtual ~Wnd() = default;

        void SetName(const std::wstring& name);
        const std::wstring& Name() const;

        void Invalidate() const;

        void SetLayoutRect(const D2D1_RECT_F& rect);
        void SetAnchors(bool anchorLeft, bool anchorTop, bool anchorRight, bool anchorBottom);
        const D2D1_RECT_F& LayoutRect() const;
        virtual Size Measure(Size available);
        virtual void Arrange(Rect finalRect);
        void SetMargin(float margin) { m_margin = margin; }
        void SetPadding(float padding) { m_padding = padding; }

        bool AddChild(const std::shared_ptr<Wnd>& child);
        const std::unordered_map<std::wstring, std::shared_ptr<Wnd>>& Children() const;

        virtual void OnAttached(Backplate& backplate);
        virtual void OnDetached();
        virtual void OnRender(ID2D1RenderTarget* target);
        virtual bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam);

    protected:
        Backplate* BackplateRef() const;

    protected:
        std::wstring m_name {};
        Backplate* m_backplate { nullptr };
        std::unordered_map<std::wstring, std::shared_ptr<Wnd>> m_children {};
        D2D1_RECT_F m_layoutDesired { 0.0f, 0.0f, 100.0f, 30.0f };
        D2D1_RECT_F m_layoutRect { 0.0f, 0.0f, 100.0f, 30.0f };
        Rect m_bounds { 0.0f, 0.0f, 100.0f, 30.0f };
        Size m_desired { 100.0f, 30.0f };
        bool m_anchorLeft { true };
        bool m_anchorTop { true };
        bool m_anchorRight { false };
        bool m_anchorBottom { false };
        float m_margin { 0.0f };
        float m_padding { 0.0f };
    };
}

