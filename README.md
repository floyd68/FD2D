# FD2D Library

FD2D is a lightweight modern C++ UI library that wraps Win32 with Direct2D/DirectWrite.

## Building

### Static Library (Default)
FD2D is built as a static library by default. Simply build the project and link against `FD2D.lib`.

**Preprocessor Definitions:**
- `FD2D_STATIC` - Defined automatically when building as static library

### Dynamic Library
To build FD2D as a dynamic library:

1. Set the `FD2D_LINK_TYPE` property to `Dynamic`:
   - In Visual Studio: Project Properties → Configuration Properties → General → Configuration Type → Dynamic Library (.dll)
   - Or add `FD2D_LINK_TYPE=Dynamic` to your build configuration

2. When building the library:
   - `FD2D_EXPORTS` is automatically defined
   - Output: `FD2D.dll` and `FD2D.lib` (import library)

3. When using the library:
   - Do NOT define `FD2D_STATIC`
   - Link against `FD2D.lib` (import library)
   - Ensure `FD2D.dll` is available at runtime

## Usage

Include the main header:
```cpp
#include "FD2D/FD2D.h"
```

See the main README.md for usage examples.


