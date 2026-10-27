# FD2D

FD2D is a lightweight modern C++ UI framework for Windows, implemented on top of:

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

Copyright (c) 2024 EunSuk, Lee (이은석, floyd)

- Win32 (`HWND`, message loop)
- Direct2D (2D rendering)
- DirectWrite (text layout/rendering)
- Optional D3D11 path (GPU texture/SRV rendering for specific controls)

FD2D is intentionally “small framework” style: a few core concepts (window/backplate, controls, layout) that you can extend
without fighting a giant retained-mode system.

## Key concepts

- **`FD2D::Application`**: process-level initialization and message loop.
- **`FD2D::Backplate`**: owns an `HWND` + render targets; drives redraw and dispatches messages to child `Wnd`s.
- **`FD2D::Wnd`**: base class for all visual/input elements. Implements `Measure/Arrange/OnRender/OnMessage`.
- **Panels**: `StackPanel`, `SplitPanel`, `ScrollView`, etc. manage children and layout.
- **Controls**: `Text`, `Image`, `Button`, `Spinner`, etc.

## Layout model

FD2D uses a modern **Measure/Arrange** pass:

- `Measure(available)` asks “how much size do you want?”
- `Arrange(finalRect)` assigns a rectangle to the element
- `OnRender(target)` draws using the computed layout rect(s)

The goal is to avoid hard-coded absolute positioning in user code.

## Rendering and the message loop

`Wnd::Invalidate()`, `Backplate::RequestLayout()`, async redraws and animation ticks never render synchronously: they call `Backplate::RequestRender()`, and any number of requests before the next render produce one frame. `Application::RunMessageLoop()` runs, per iteration:

1. a blocking wait for a message or an async-redraw event (it does not block while a render is pending, and it works with no backplate, so the loop never spins and returns the `WM_QUIT` code);
2. every signaled async-redraw event, once;
3. a bounded message drain (64 messages or 10 ms, and it ends after a `WM_PAINT`), so a queue that never empties cannot starve steps 2, 4 and 5;
4. the animation tick;
5. one render of each backplate with a pending request that has not rendered in this iteration.

All invalidations caused by one input message (for example a pan and the view sync of a second pane on the same backplate) therefore land in one present. A request also invalidates the window, so modal loops (size/move, menus, OLE drag, message boxes) render it on `WM_PAINT`. `Backplate::Render()` is the explicit immediate path, for when a frame is needed right away: a warm-up render before the window is shown, before `ReadComposedPixels()`, the first frame in `Show()`, and `WM_PAINT`. A layout request is arranged by the next render, or earlier by `Backplate::LayoutIfPending()`, which the backplate runs before it routes an input message or a file drag/drop, so input never hit-tests a stale arrangement; code that needs arranged bounds right after a change calls it instead of relying on a synchronous render.

Render instrumentation (`RenderStats`) is off by default; set the environment variable `FD2D_RENDER_STATS=1` to log a once-per-second `[RenderStats]` line per window with presents per second, presents per mouse move, frame time (median, p95) and input-to-present latency.

## Graphics resources and invalidation

`Backplate::GetGraphicsGeneration()` returns monotonic `device`, `target`, and `renderer` generations; they, not `GraphicsInvalidationReason`, decide whether a cached resource is still valid:

- D2D brushes, bitmaps and other render-target resources follow the **target** generation (FD2D also bumps it when it recreates the HWND render target or the D2D device).
- D3D resources and SRVs follow the **device** generation; an SRV survives a target-only recreation.
- DirectWrite text formats and layouts are device-independent and are rebuilt only when text, font or size changes.

Controls record a `GraphicsResourceStamp` (backplate + generation) with each cached resource and call `RefreshTargetResourceStamp` / `RefreshDeviceResourceStamp` before using it. A control that embeds another control as a member (for example a `Text` label) resets the member's resources from its own stamp, because the member is not attached to the backplate.

Any failed `EndDraw` (primary or offscreen copy) bumps the target generation and schedules a new frame; `D2DERR_RECREATE_TARGET` on the D3D11 device context and device-removed errors recreate the whole device (device and target generations). Per-device state shared by controls (such as the `Image` quad pipeline) lives in `Backplate::DeviceCacheEntry`, owned by each backplate and cleared when its device is discarded.

## Building

### Static library (default)

Build `FD2D.vcxproj` (or build the top-level app which references it).

- Output: `FD2D.lib`
- Macro: `FD2D_STATIC` is used for the static-link case

### Dynamic library (optional)

FD2D can also be built as a DLL.

1) Set `FD2D_LINK_TYPE=Dynamic` (or set `ConfigurationType` to `DynamicLibrary`).
2) When building the library: `FD2D_EXPORTS` is defined; outputs `FD2D.dll` + import `FD2D.lib`.
3) When consuming: **do not** define `FD2D_STATIC`; deploy `FD2D.dll` alongside your exe.

## Usage

Include the umbrella header:

```cpp
#include "FD2D/FD2D.h"
```

### Typical flow

1) Initialize the application
2) Create or attach a backplate
3) Build a tree of `Wnd` controls
4) Run the message loop

Host applications can wire FD2D into their own apps; see each consumer's documentation for end-to-end examples (split panes, image views, etc.).

## Notes

- FD2D is DPI-aware (uses DIPs; Direct2D handles scaling).
- Message routing is “top-most wins” style: the first child that handles a message stops propagation.
- For performance, keep `OnRender` pure (no heavy allocations); prepare resources lazily and cache them.