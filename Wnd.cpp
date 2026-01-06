#include "Wnd.h"
#include "Backplate.h"
#include <algorithm>

namespace FD2D
{
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

    Backplate* Wnd::BackplateRef() const
    {
        return m_backplate;
    }

    void Wnd::Invalidate() const
    {
        if (m_backplate != nullptr)
        {
            InvalidateRect(m_backplate->Window(), nullptr, FALSE);
        }
    }
}

