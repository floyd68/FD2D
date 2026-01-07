# FD2D

FD2D is a lightweight modern C++ UI framework for Windows, implemented on top of:

- Win32 (`HWND`, message loop)
- Direct2D (2D rendering)
- DirectWrite (text layout/rendering)
- WIC (image decode primitives)
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

The sample app in `FICture2` shows end-to-end usage, including thumbnail lists and split panes.

## Notes

- FD2D is DPI-aware (uses DIPs; Direct2D handles scaling).
- Message routing is “top-most wins” style: the first child that handles a message stops propagation.
- For performance, keep `OnRender` pure (no heavy allocations); prepare resources lazily and cache them.
