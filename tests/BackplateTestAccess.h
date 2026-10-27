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

        static ID2D1SolidColorBrush* TipBrush(const Backplate& backplate) { return backplate.m_tipBrush.Get(); }
        static IDWriteTextLayout* TooltipLayout(const Backplate& backplate) { return backplate.m_tipLayout.Get(); }
        static IDWriteTextLayout* ToastLayout(const Backplate& backplate) { return backplate.m_toastLayout.Get(); }
    };
}
