#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Layout.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <d3d11_1.h>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>

namespace FD2D
{
    class Backplate;

    struct GraphicsGeneration
    {
        uint64_t device { 0 };
        uint64_t target { 0 };
        uint64_t renderer { 0 };
    };

    enum class GraphicsInvalidationReason
    {
        TargetRecreated,
        DeviceLost,
        RendererFallback,
        Resize, // optional; prefer NOT bumping content generation on simple swapchain resize
        Shutdown
    };

    enum class FileDragVisual
    {
        None,
        Replace, // drop will replace the current drop target
        Insert   // drop will insert alongside the current drop target
    };

    enum class InputEventType
    {
        None,
        MouseMove,
        MouseDown,
        MouseUp,
        MouseDoubleClick,
        MouseWheel,
        MouseHWheel,
        MouseLeave,
        CaptureChanged,
        SetCursor,
        KeyDown,
        KeyUp,
        Char,
        SystemChar,
        DeadChar,
        SystemDeadChar,
        UniChar,
    };

    enum class MouseButton
    {
        None,
        Left,
        Right,
        Middle
    };

    struct InputModifiers
    {
        bool shift { false };
        bool control { false };
        bool alt { false };
        bool leftButton { false };
        bool rightButton { false };
        bool middleButton { false };
    };

    struct InputEvent
    {
        InputEventType type { InputEventType::None };
        MouseButton button { MouseButton::None };
        POINT point {};
        bool hasPoint { false };
        short wheelDelta { 0 };
        UINT keyCode { 0 };
        UINT repeatCount { 0 };
        UINT scanCode { 0 };
        bool isSystemKey { false };
        bool wasDown { false };
        bool isKeyUpTransition { false };
        bool isExtendedKey { false };
        InputModifiers modifiers {};
    };

    struct CommandEvent
    {
        UINT id { 0 };
        WPARAM wParam { 0 };
        LPARAM lParam { 0 };
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

        // Content layout for composite controls that embed Text (or similar)
        // outside the child-Wnd tree. Separate from SetPadding, which only
        // insets arranged child Wnds. Defaults are zero margin + Start/Start
        // so existing absolute-rect controls stay unchanged until they opt in.
        void SetContentMargin(float uniform);
        void SetContentMargin(float horizontal, float vertical);
        void SetContentMargin(const Thickness& margin);
        const Thickness& ContentMargin() const { return m_contentMargin; }

        void SetContentAlign(AlignH horizontal, AlignV vertical);
        void SetContentAlignH(AlignH horizontal);
        void SetContentAlignV(AlignV vertical);
        AlignH ContentAlignH() const { return m_contentAlignH; }
        AlignV ContentAlignV() const { return m_contentAlignV; }

        bool AddChild(const std::shared_ptr<Wnd>& child);
        bool RemoveChild(const std::wstring& childName);
        void ClearChildren();
        // Reorders the visual child iteration order without detaching/attaching children.
        // Returns false if any name is missing or duplicated; on failure, order is unchanged.
        bool ReorderChildren(const std::vector<std::wstring>& childNamesInOrder);
        const std::unordered_map<std::wstring, std::shared_ptr<Wnd>>& Children() const;
        // Deterministic child iteration order (insertion order).
        // Many panels assume child iteration order defines visual order.
        const std::vector<std::shared_ptr<Wnd>>& ChildrenInOrder() const;

        virtual void OnAttached(Backplate& backplate);
        virtual void OnDetached();
        // Graphics device/target/renderer recreation. Default forwards to children.
        // App controls should drop stale GPU handles here and recreate on the next render.
        virtual void OnGraphicsInvalidated(GraphicsInvalidationReason reason, const GraphicsGeneration& generation);
        // Optional D3D render pass (executed before D2D UI pass).
        // Default implementation forwards to children.
        virtual void OnRenderD3D(ID3D11DeviceContext* context);
        virtual void OnRender(ID2D1RenderTarget* target);
        // Second D2D pass after the full tree's OnRender. Used for popups
        // (ComboBox dropdowns) that must paint above sibling controls without
        // covering the whole window with a scrim.
        virtual void OnRenderOverlay(ID2D1RenderTarget* target);
        virtual bool OnInputEvent(const InputEvent& event);
        // True while this control has a popup that should steal mouse input
        // ahead of later siblings (see RouteOverlayMouseInput).
        virtual bool HasInputOverlay() const { return false; }
        virtual bool OnCommandEvent(const CommandEvent& event);
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

        // LayoutRect() as an FD2D::Rect (x/y/w/h).
        Rect BoundsRect() const;
        // Place content of the given intrinsic size inside `bounds` (or this
        // control's LayoutRect) using the current content margin + alignment.
        Rect ContentRectFor(const Size& contentSize) const;
        Rect ContentRectFor(const Rect& bounds, const Size& contentSize) const;
        void NotifyContentLayoutChanged();
        // Depth-first: give open overlay controls first chance at mouse input.
        bool RouteOverlayMouseInput(const InputEvent& event);

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
        Thickness m_contentMargin {};
        AlignH m_contentAlignH { AlignH::Start };
        AlignV m_contentAlignV { AlignV::Start };
    };
}

