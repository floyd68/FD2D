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
    };
}
