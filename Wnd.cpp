#include "Wnd.h"
#include "Backplate.h"
#include <algorithm>
#include <windowsx.h>  // For GET_X_LPARAM, GET_Y_LPARAM, MAKELPARAM

namespace FD2D
{
    namespace
    {
        static bool IsMouseMessage(UINT message)
        {
            switch (message)
            {
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
            case WM_XBUTTONDBLCLK:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
            case WM_CAPTURECHANGED:
                return true;
            default:
                return false;
            }
        }

        // Note: LayoutRect() returns coordinates in client coordinate system (Backplate client area)
        // Since all LayoutRects are in the same client coordinate system, no conversion is needed
        // Parent Wnd and Child Wnd both receive coordinates in client/Layout coordinate system

        static bool RectContainsPoint(const D2D1_RECT_F& r, const POINT& pt)
        {
            return pt.x >= r.left &&
                pt.x <= r.right &&
                pt.y >= r.top &&
                pt.y <= r.bottom;
        }
    }

    Wnd::Wnd()
    {
    }

    Wnd::Wnd(const std::wstring& name)
        : m_name(name)
    {
    }

    void Wnd::SetLayoutRect(const D2D1_RECT_F& rect)
    {
        m_layoutDesired = rect;
        m_layoutRect = rect;
        m_desired = { rect.right - rect.left, rect.bottom - rect.top };
    }

    void Wnd::SetAnchors(bool anchorLeft, bool anchorTop, bool anchorRight, bool anchorBottom)
    {
        m_anchorLeft = anchorLeft;
        m_anchorTop = anchorTop;
        m_anchorRight = anchorRight;
        m_anchorBottom = anchorBottom;
    }

    const D2D1_RECT_F& Wnd::LayoutRect() const
    {
        return m_layoutRect;
    }

    Size Wnd::Measure(Size available)
    {
        // 기본 Wnd는 자식이 없으면 크기가 0
        if (m_childrenOrdered.empty())
        {
            m_desired = { 0.0f, 0.0f };
            return m_desired;
        }

        // 자식이 있으면 자식들의 최대 크기를 사용
        Size maxSize {};
        for (auto& child : m_childrenOrdered)
        {
            if (child)
            {
                Size childSize = child->Measure(available);
                maxSize.w = (std::max)(maxSize.w, childSize.w);
                maxSize.h = (std::max)(maxSize.h, childSize.h);
            }
        }

        m_desired = { maxSize.w + 2 * m_margin, maxSize.h + 2 * m_margin };
        return m_desired;
    }

    Size Wnd::MinSize() const
    {
        // Default: if no children, no intrinsic minimum.
        if (m_childrenOrdered.empty())
        {
            return { 0.0f, 0.0f };
        }

        Size maxSize {};
        for (const auto& child : m_childrenOrdered)
        {
            if (child)
            {
                Size childMin = child->MinSize();
                maxSize.w = (std::max)(maxSize.w, childMin.w);
                maxSize.h = (std::max)(maxSize.h, childMin.h);
            }
        }

        // Include this node's margin/padding.
        maxSize.w += 2.0f * m_margin + 2.0f * m_padding;
        maxSize.h += 2.0f * m_margin + 2.0f * m_padding;
        return maxSize;
    }

    void Wnd::Arrange(Rect finalRect)
    {
        Rect inset = Inset(finalRect, m_margin);
        m_bounds = inset;
        m_layoutRect = ToD2D(inset);

        Rect childArea = Inset(inset, m_padding);

        for (auto& child : m_childrenOrdered)
        {
            if (child)
            {
                child->Arrange(childArea);
            }
        }
    }

    void Wnd::SetName(const std::wstring& name)
    {
        m_name = name;
    }

    const std::wstring& Wnd::Name() const
    {
        return m_name;
    }

    bool Wnd::AddChild(const std::shared_ptr<Wnd>& child)
    {
        if (!child)
        {
            return false;
        }

        const std::wstring& childName = child->Name();

        if (childName.empty())
        {
            return false;
        }

        if (m_children.find(childName) != m_children.end())
        {
            return false;
        }

        m_children.emplace(childName, child);
        m_childrenOrdered.push_back(child);

        if (m_backplate != nullptr)
        {
            child->OnAttached(*m_backplate);
        }

        return true;
    }

    bool Wnd::RemoveChild(const std::wstring& childName)
    {
        if (childName.empty())
        {
            return false;
        }

        auto it = m_children.find(childName);
        if (it == m_children.end())
        {
            return false;
        }

        std::shared_ptr<Wnd> child = it->second;

        if (child && m_backplate != nullptr)
        {
            child->OnDetached();
        }

        m_children.erase(it);

        for (auto vit = m_childrenOrdered.begin(); vit != m_childrenOrdered.end(); ++vit)
        {
            if (*vit && (*vit)->Name() == childName)
            {
                m_childrenOrdered.erase(vit);
                break;
            }
        }

        return true;
    }

    void Wnd::ClearChildren()
    {
        if (m_backplate != nullptr)
        {
            for (auto& child : m_childrenOrdered)
            {
                if (child)
                {
                    child->OnDetached();
                }
            }
        }

        m_children.clear();
        m_childrenOrdered.clear();
    }

    bool Wnd::ReorderChildren(const std::vector<std::wstring>& childNamesInOrder)
    {
        std::unordered_map<std::wstring, bool> seen;
        seen.reserve(childNamesInOrder.size());

        std::vector<std::shared_ptr<Wnd>> newOrder;
        newOrder.reserve(childNamesInOrder.size());

        for (const auto& childName : childNamesInOrder)
        {
            if (childName.empty())
            {
                return false;
            }

            if (seen.find(childName) != seen.end())
            {
                return false;
            }
            seen.emplace(childName, true);

            auto it = m_children.find(childName);
            if (it == m_children.end())
            {
                return false;
            }

            newOrder.push_back(it->second);
        }

        m_childrenOrdered = std::move(newOrder);
        return true;
    }

    const std::unordered_map<std::wstring, std::shared_ptr<Wnd>>& Wnd::Children() const
    {
        return m_children;
    }

    const std::vector<std::shared_ptr<Wnd>>& Wnd::ChildrenInOrder() const
    {
        return m_childrenOrdered;
    }

    void Wnd::OnAttached(Backplate& backplate)
    {
        m_backplate = &backplate;
        for (auto& child : m_childrenOrdered)
        {
            if (child)
            {
                child->OnAttached(backplate);
            }
        }
    }

    void Wnd::OnDetached()
    {
        if (m_backplate != nullptr)
        {
            m_backplate->ClearFocusIf(this);
        }

        for (auto& child : m_childrenOrdered)
        {
            if (child)
            {
                child->OnDetached();
            }
        }

        m_backplate = nullptr;
    }

    void Wnd::OnRender(ID2D1RenderTarget* target)
    {
        UNREFERENCED_PARAMETER(target);

        for (auto& child : m_childrenOrdered)
        {
            if (child)
            {
                child->OnRender(target);
            }
        }
    }

    void Wnd::OnRenderD3D(ID3D11DeviceContext* context)
    {
        UNREFERENCED_PARAMETER(context);

        for (auto& child : m_childrenOrdered)
        {
            if (child)
            {
                child->OnRenderD3D(context);
            }
        }
    }

    bool Wnd::OnMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        UNREFERENCED_PARAMETER(message);
        UNREFERENCED_PARAMETER(wParam);
        UNREFERENCED_PARAMETER(lParam);

        // Mouse input should behave like hit-testing: topmost child first, stop at first handled.
        // Note: Coordinates are already in client/Layout coordinate system (converted by Backplate)
        // All LayoutRects are in the same client coordinate system, so no conversion needed
        if (IsMouseMessage(message))
        {
            // For wheel input, route based on cursor position so the pane under the mouse receives it
            // even if another control currently owns focus.
            if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
            {
                const POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                for (auto it = m_childrenOrdered.rbegin(); it != m_childrenOrdered.rend(); ++it)
                {
                    const auto& child = *it;
                    if (!child)
                    {
                        continue;
                    }
                    if (!RectContainsPoint(child->LayoutRect(), pt))
                    {
                        continue;
                    }
                    if (child->OnMessage(message, wParam, lParam))
                    {
                        return true;
                    }
                }
                return false;
            }

            for (auto it = m_childrenOrdered.rbegin(); it != m_childrenOrdered.rend(); ++it)
            {
                const auto& child = *it;
                if (child && child->OnMessage(message, wParam, lParam))
                {
                    return true;
                }
            }
            return false;
        }

        // Non-mouse messages: broadcast (layout/animation timers etc).
        bool handled = false;
        for (auto& child : m_childrenOrdered)
        {
            if (child && child->OnMessage(message, wParam, lParam))
            {
                handled = true;
            }
        }
        return handled;
    }

    bool Wnd::OnFileDrop(const std::wstring& path, const POINT& clientPt)
    {
        // Default behavior: hit-test children (topmost first) and forward.
        for (auto it = m_childrenOrdered.rbegin(); it != m_childrenOrdered.rend(); ++it)
        {
            const auto& child = *it;
            if (!child)
            {
                continue;
            }

            if (!RectContainsPoint(child->LayoutRect(), clientPt))
            {
                continue;
            }

            if (child->OnFileDrop(path, clientPt))
            {
                return true;
            }
        }
        return false;
    }

    bool Wnd::OnFileDrag(const std::wstring& path, const POINT& clientPt, FileDragVisual& outVisual)
    {
        UNREFERENCED_PARAMETER(path);
        // Default behavior: hit-test children (topmost first) and forward.
        for (auto it = m_childrenOrdered.rbegin(); it != m_childrenOrdered.rend(); ++it)
        {
            const auto& child = *it;
            if (!child)
            {
                continue;
            }

            if (!RectContainsPoint(child->LayoutRect(), clientPt))
            {
                continue;
            }

            if (child->OnFileDrag(path, clientPt, outVisual))
            {
                return true;
            }
        }

        outVisual = FileDragVisual::None;
        return false;
    }

    void Wnd::OnFileDragLeave()
    {
        // Default behavior: broadcast to children so any overlay state can be cleared.
        for (auto& child : m_childrenOrdered)
        {
            if (child)
            {
                child->OnFileDragLeave();
            }
        }
    }

    void Wnd::RequestFocus()
    {
        if (m_backplate != nullptr)
        {
            m_backplate->SetFocusedWnd(this);
        }
    }

    bool Wnd::HasFocus() const
    {
        return m_backplate != nullptr && m_backplate->FocusedWnd() == this;
    }

    Backplate* Wnd::BackplateRef() const
    {
        return m_backplate;
    }

    void Wnd::Invalidate() const
    {
        if (m_backplate != nullptr)
        {
            if (m_backplate->IsInSizeMove())
            {
                InvalidateRect(m_backplate->Window(), nullptr, FALSE);
            }
            else
            {
                // Direct rendering instead of message-loop based invalidation
                m_backplate->Render();
            }
        }
    }
}

