#pragma once

#include "Backplate.h"

// Test access to Backplate internals (Backplate befriends
// FD2D::BackplateTestAccess). Shared by the FD2D test files, so the test
// executable has exactly one definition.

namespace FD2D
{
    struct BackplateTestAccess
    {
        // Generation changes, as Backplate itself does on recreation.
        static void TargetRecreated(Backplate& backplate)
        {
            backplate.InvalidateGraphics(GraphicsInvalidationReason::TargetRecreated, false, true, false);
        }

        static void DeviceLost(Backplate& backplate)
        {
            backplate.DiscardDeviceResources();
            backplate.InvalidateGraphics(GraphicsInvalidationReason::DeviceLost, true, true, false);
        }

        // Hover tooltip and toast drawing (Backplate::DrawHoverAndToast) onto a
        // caller's target, with the client size the boxes are placed in.
        static void SetClientSize(Backplate& backplate, UINT width, UINT height)
        {
            backplate.m_size = D2D1::SizeU(width, height);
        }

        // A tooltip whose dwell has elapsed, anchored at `anchor`.
        static void ShowTooltipNow(Backplate& backplate, const std::wstring& text, POINT anchor)
        {
            backplate.m_hoverTip = text;
            backplate.m_hoverPt = anchor;
            backplate.m_tipAnchor = anchor;
            backplate.m_tipShown = true;
        }

        static void DrawHoverAndToast(Backplate& backplate, ID2D1RenderTarget* target)
        {
            backplate.DrawHoverAndToast(target, true, false);
            backplate.DrawHoverAndToast(target, false, true);
        }

        // Gives a backplate (in windowless render mode) a hidden window for
        // timers and nothing else; Backplate's own window-bound requests
        // (Render on a timer, SetTimer) then work as in the app. Pass nullptr
        // to detach it before the window is destroyed.
        static void SetWindow(Backplate& backplate, HWND window)
        {
            backplate.m_window = window;
        }

        static bool HandleMessage(Backplate& backplate, HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
        {
            return backplate.HandleMessage(window, message, wParam, lParam, result);
        }

        // The hover bookkeeping a WM_MOUSEMOVE at `pt` runs.
        static void HoverAt(Backplate& backplate, POINT pt)
        {
            backplate.UpdateHoverTarget(pt);
        }

        static bool TooltipShown(const Backplate& backplate) { return backplate.m_tipShown; }
        static bool ToastShown(const Backplate& backplate) { return !backplate.m_toastText.empty(); }
        static unsigned long long ToastExpireMs(const Backplate& backplate) { return backplate.m_toastExpireMs; }
        static unsigned long long HoverSinceMs(const Backplate& backplate) { return backplate.m_hoverSinceMs; }
        // The deadline the hover/toast timer is armed for (0 = not armed).
        static unsigned long long HoverToastTimerDueMs(const Backplate& backplate) { return backplate.m_hoverToastTimerDueMs; }

        // The OLE drop target's DragOver / DragLeave handling.
        static bool FileDragOver(Backplate& backplate, const std::wstring& path, POINT pt)
        {
            return backplate.HandleFileDragOver(path, pt);
        }

        static void FileDragLeave(Backplate& backplate)
        {
            backplate.HandleFileDragLeave();
        }

        // Layout requests (Backplate::RequestLayout) mark the layout dirty.
        static bool LayoutDirty(const Backplate& backplate) { return backplate.m_layoutDirty; }
        static void ClearLayoutDirty(Backplate& backplate) { backplate.m_layoutDirty = false; }

        static ID2D1SolidColorBrush* TipBrush(const Backplate& backplate) { return backplate.m_tipBrush.Get(); }
        static IDWriteTextLayout* TooltipLayout(const Backplate& backplate) { return backplate.m_tipLayout.Get(); }
        static IDWriteTextLayout* ToastLayout(const Backplate& backplate) { return backplate.m_toastLayout.Get(); }
    };
}
