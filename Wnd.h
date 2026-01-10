#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Layout.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <d3d11_1.h>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>

namespace FD2D
{
    class Backplate;

    enum class FileDragVisual
    {
        None,
        Replace, // drop will replace current view
        Insert   // drop will insert a new ImageBrowser to the right
    };

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
        // Upward constraint: intrinsic minimum size requested by this control.
        // Default implementation aggregates children; containers can override.
        virtual Size MinSize() const;
        virtual void Arrange(Rect finalRect);
        void SetMargin(float margin) { m_margin = margin; }
        void SetPadding(float padding) { m_padding = padding; }

        bool AddChild(const std::shared_ptr<Wnd>& child);
        bool RemoveChild(const std::wstring& childName);
        void ClearChildren();
        const std::unordered_map<std::wstring, std::shared_ptr<Wnd>>& Children() const;
        // Deterministic child iteration order (insertion order).
        // Many panels assume child iteration order defines visual order.
        const std::vector<std::shared_ptr<Wnd>>& ChildrenInOrder() const;

        virtual void OnAttached(Backplate& backplate);
        virtual void OnDetached();
        // Optional D3D render pass (executed before D2D UI pass).
        // Default implementation forwards to children.
        virtual void OnRenderD3D(ID3D11DeviceContext* context);
        virtual void OnRender(ID2D1RenderTarget* target);
        virtual bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam);
        virtual bool OnFileDrop(const std::wstring& path, const POINT& clientPt);

        // File drag hover (OLE drag&drop). Default implementation hit-tests children (topmost first).
        // Return true if handled and visual state was updated.
        virtual bool OnFileDrag(const std::wstring& path, const POINT& clientPt, FileDragVisual& outVisual);
        // Called when the drag leaves the window or the hover target changes.
        virtual void OnFileDragLeave();

        void RequestFocus();
        bool HasFocus() const;

    protected:
        Backplate* BackplateRef() const;

    protected:
        std::wstring m_name {};
        Backplate* m_backplate { nullptr };
        std::unordered_map<std::wstring, std::shared_ptr<Wnd>> m_children {};
        std::vector<std::shared_ptr<Wnd>> m_childrenOrdered {};
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

