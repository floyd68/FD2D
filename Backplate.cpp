#include "Backplate.h"
#include "Core.h"
#include <cmath>
#include <dxgi1_3.h>
#include <string>
#include <algorithm>
#include <shellapi.h>
#include <windowsx.h>  // For GET_X_LPARAM, GET_Y_LPARAM, MAKELPARAM

namespace FD2D
{
    static bool IsDeviceRemovedHr(HRESULT hr)
    {
        return hr == DXGI_ERROR_DEVICE_REMOVED
            || hr == DXGI_ERROR_DEVICE_RESET
            || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }

    static unsigned long long NowMs()
    {
        return static_cast<unsigned long long>(GetTickCount64());
    }

    static D2D1_BITMAP_PROPERTIES1 MakeSwapChainBitmapProps()
    {
        const float dpi = 96.0f;

        D2D1_BITMAP_PROPERTIES1 bp {};
        bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        // Swapchain alpha is DXGI_ALPHA_MODE_IGNORE, so the D2D target must match.
        bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        bp.dpiX = dpi;
        bp.dpiY = dpi;
        // Recommended for swapchain-backed targets (can be set as target, but not used as a source).
        bp.bitmapOptions = static_cast<D2D1_BITMAP_OPTIONS>(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);
        return bp;
    }

    Backplate::Backplate()
    {
        m_asyncRedrawEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    Backplate::Backplate(const std::wstring& name)
        : m_name(name)
    {
        m_asyncRedrawEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    Backplate::~Backplate()
    {
        if (m_window != nullptr && m_placeAutosaveTimerId != 0)
        {
            KillTimer(m_window, m_placeAutosaveTimerId);
            m_placeAutosaveTimerId = 0;
        }

        if (m_asyncRedrawEvent)
        {
            CloseHandle(m_asyncRedrawEvent);
            m_asyncRedrawEvent = nullptr;
        }
    }

    void Backplate::SetOnBeforeDestroy(std::function<void(HWND)> handler)
    {
        m_onBeforeDestroy = std::move(handler);
    }

    void Backplate::SetOnWindowPlacementChanged(std::function<void(HWND)> handler)
    {
        m_onWindowPlacementChanged = std::move(handler);
    }

    void Backplate::InvokeBeforeDestroyOnce()
    {
        if (m_beforeDestroyInvoked)
        {
            return;
        }
        m_beforeDestroyInvoked = true;

        if (m_onBeforeDestroy && m_window != nullptr)
        {
            m_onBeforeDestroy(m_window);
        }
    }

    void Backplate::SchedulePlacementAutosave()
    {
        if (m_window == nullptr || !m_onWindowPlacementChanged)
        {
            return;
        }

        if (m_placeAutosaveTimerId == 0)
        {
            m_placeAutosaveTimerId = 0xFD22;
        }

        // Debounce (reset timer each time).
        (void)SetTimer(m_window, m_placeAutosaveTimerId, 200, nullptr);
    }

    void Backplate::FlushPlacementAutosave()
    {
        if (m_window == nullptr || !m_onWindowPlacementChanged)
        {
            return;
        }

        if (m_placeAutosaveTimerId != 0)
        {
            KillTimer(m_window, m_placeAutosaveTimerId);
        }

        m_onWindowPlacementChanged(m_window);
    }

    void Backplate::RequestAsyncRedraw()
    {
        if (!m_asyncRedrawEvent || !m_window)
        {
            return;
        }

        // Coalesce multiple worker completions into a single wakeup.
        const bool wasPending = m_asyncRedrawPending.exchange(true);
        if (!wasPending)
        {
            SetEvent(m_asyncRedrawEvent);
        }
    }

    void Backplate::ProcessAsyncRedraw()
    {
        if (!m_window || !m_asyncRedrawEvent)
        {
            return;
        }

        // Drain the pending flag and reset the event for future signals.
        m_asyncRedrawPending.store(false);
        ResetEvent(m_asyncRedrawEvent);

        // Trigger a prompt repaint (once per coalesced burst).
        RedrawWindow(m_window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }

    void Backplate::RequestAnimationFrame()
    {
        m_lastAnimationRequestMs.store(NowMs());
    }

    bool Backplate::HasActiveAnimation(unsigned long long nowMs) const
    {
        const unsigned long long last = m_lastAnimationRequestMs.load();
        // Consider animation active if someone requested frames recently.
        // Use a generous window so we don't accidentally drop out of 60fps ticking mid-fade
        // (e.g., due to a brief message burst / scheduling hiccup).
        return (last != 0) && (nowMs - last <= 2000ULL);
    }

    void Backplate::ProcessAnimationTick(unsigned long long nowMs)
    {
        if (!m_window)
        {
            return;
        }

        if (!HasActiveAnimation(nowMs))
        {
            return;
        }

        const unsigned long long lastTick = m_lastAnimationTickMs.load();
        if (lastTick != 0 && (nowMs - lastTick) < 16ULL)
        {
            return;
        }
        m_lastAnimationTickMs.store(nowMs);

        // Trigger one paint at most per tick.
        InvalidateRect(m_window, nullptr, FALSE);
        UpdateWindow(m_window);
    }

    LRESULT CALLBACK Backplate::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        Backplate* self = reinterpret_cast<Backplate*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

        if (message == WM_NCCREATE)
        {
            auto createStruct = reinterpret_cast<CREATESTRUCT*>(lParam);
            self = reinterpret_cast<Backplate*>(createStruct->lpCreateParams);
            if (self != nullptr)
            {
                SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                self->m_window = hWnd;
                // WM_NCCREATE 시점에는 창이 완전히 생성되지 않았으므로 렌더 타겟 생성을 지연
                // WM_CREATE 또는 첫 WM_SIZE에서 생성됨
            }
        }

        if (self != nullptr)
        {
            LRESULT result = 0;
            if (self->HandleMessage(hWnd, message, wParam, lParam, result))
            {
                return result;
            }

            if (self->m_prevWndProc != nullptr)
            {
                return CallWindowProc(self->m_prevWndProc, hWnd, message, wParam, lParam);
            }
        }

        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    bool Backplate::HandleMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        UNREFERENCED_PARAMETER(hWnd);

        switch (message)
        {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        {
            // ESC: exit application (handled at the window level so it works regardless of focus).
            if (wParam == VK_ESCAPE)
            {
                const bool isRepeat = ((lParam & (1LL << 30)) != 0);
                if (!isRepeat && m_window != nullptr)
                {
                    PostMessageW(m_window, WM_CLOSE, 0, 0);
                }
                result = 0;
                return true;
            }
            break;
        }

        case WM_CLOSE:
        {
            // Save settings while the HWND is still valid.
            InvokeBeforeDestroyOnce();
            // Let default behavior destroy the window.
            return false;
        }

        case WM_ENTERSIZEMOVE:
        {
            m_inSizeMove = true;
            result = 0;
            return true;
        }

        case WM_EXITSIZEMOVE:
        {
            m_inSizeMove = false;
            // User finished an interactive move/resize; persist immediately.
            FlushPlacementAutosave();
            result = 0;
            return true;
        }

        case WM_ERASEBKGND:
        {
            // We render via swapchain; prevent GDI background erase to avoid flicker.
            result = 1;
            return true;
        }

        case WM_GETMINMAXINFO:
        {
            // Enforce upward constraints (e.g., SplitPanel min sizes) at the window level.
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            if (mmi != nullptr && m_window != nullptr)
            {
                float minClientW = 0.0f;
                float minClientH = 0.0f;

                for (const auto& pair : m_children)
                {
                    if (pair.second)
                    {
                        Size ms = pair.second->MinSize();
                        minClientW = (std::max)(minClientW, ms.w);
                        minClientH = (std::max)(minClientH, ms.h);
                    }
                }

                if (minClientW > 0.0f || minClientH > 0.0f)
                {
                    RECT rc { 0, 0, static_cast<LONG>(std::ceil(minClientW)), static_cast<LONG>(std::ceil(minClientH)) };
                    const DWORD style = static_cast<DWORD>(GetWindowLongPtr(m_window, GWL_STYLE));
                    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(m_window, GWL_EXSTYLE));
                    const BOOL hasMenu = (GetMenu(m_window) != nullptr) ? TRUE : FALSE;

                    if (AdjustWindowRectEx(&rc, style, hasMenu, exStyle))
                    {
                        const LONG minTrackW = rc.right - rc.left;
                        const LONG minTrackH = rc.bottom - rc.top;

                        if (minTrackW > 0)
                        {
                            mmi->ptMinTrackSize.x = (std::max)(mmi->ptMinTrackSize.x, minTrackW);
                        }
                        if (minTrackH > 0)
                        {
                            mmi->ptMinTrackSize.y = (std::max)(mmi->ptMinTrackSize.y, minTrackH);
                        }
                    }
                }
            }

            result = 0;
            return true;
        }

        case WM_CREATE:
        {
            // 창이 완전히 생성된 후 렌더 타겟 생성
            EnsureRenderTarget();
            // 타이틀바 정보 업데이트
            UpdateTitleBarInfo();
            DragAcceptFiles(m_window, TRUE);
            result = 0;
            return true;
        }

        case WM_SIZE:
        {
            Resize(LOWORD(lParam), HIWORD(lParam));
            SchedulePlacementAutosave();
            result = 0;
            return true;
        }

        case WM_MOVE:
        {
            SchedulePlacementAutosave();
            result = 0;
            return true;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps {};
            BeginPaint(m_window, &ps);
            Render();
            EndPaint(m_window, &ps);
            result = 0;
            return true;
        }

        case WM_DROPFILES:
        {
            const HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            if (hDrop == nullptr)
            {
                result = 0;
                return true;
            }

            wchar_t pathBuf[MAX_PATH] {};
            const UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            if (fileCount == 0)
            {
                DragFinish(hDrop);
                result = 0;
                return true;
            }

            POINT pt {};
            (void)DragQueryPoint(hDrop, &pt); // client coordinates

            const UINT cch = DragQueryFileW(hDrop, 0, pathBuf, static_cast<UINT>(std::size(pathBuf)));
            DragFinish(hDrop);

            if (cch == 0)
            {
                result = 0;
                return true;
            }

            const std::wstring path(pathBuf);

            // Route to UI tree: hit-test top-level children and allow Wnd overrides to handle.
            for (auto& pair : m_children)
            {
                if (pair.second && pair.second->OnFileDrop(path, pt))
                {
                    break;
                }
            }

            result = 0;
            return true;
        }

        case Backplate::WM_FD2D_REQUEST_REDRAW:
        {
            // worker thread에서 PostMessage로 들어온 redraw 요청
            if (m_window)
            {
                // Fast but cheap:
                // - InvalidateRect is low overhead and coalesces dirty regions
                // - We schedule a single UI-thread "flush" message to call UpdateWindow()
                //   so paint happens promptly without forcing RDW_UPDATENOW for every completion.
                InvalidateRect(m_window, nullptr, FALSE);

                if (!m_flushRedrawQueued)
                {
                    m_flushRedrawQueued = true;
                    PostMessage(m_window, Backplate::WM_FD2D_FLUSH_REDRAW, 0, 0);
                }
            }
            result = 0;
            return true;
        }

        case Backplate::WM_FD2D_FLUSH_REDRAW:
        {
            m_flushRedrawQueued = false;
            if (m_window)
            {
                // If there is an invalid region, this synchronously triggers WM_PAINT once.
                UpdateWindow(m_window);
            }
            result = 0;
            return true;
        }

        case Backplate::WM_FD2D_BROADCAST:
        {
            auto* bm = reinterpret_cast<Backplate::BroadcastMessage*>(lParam);
            if (bm != nullptr)
            {
                for (auto& pair : m_children)
                {
                    if (pair.second)
                    {
                        (void)pair.second->OnMessage(bm->message, bm->wParam, bm->lParam);
                    }
                }
                delete bm;
            }
            result = 0;
            return true;
        }

        case WM_TIMER:
        {
            if (m_placeAutosaveTimerId != 0 && wParam == m_placeAutosaveTimerId)
            {
                if (m_window != nullptr)
                {
                    KillTimer(m_window, m_placeAutosaveTimerId);
                }
                if (m_onWindowPlacementChanged && m_window != nullptr)
                {
                    m_onWindowPlacementChanged(m_window);
                }
                result = 0;
                return true;
            }
            break;
        }

        case WM_DESTROY:
        {
            // WM_CLOSE isn't guaranteed (e.g., DestroyWindow()); ensure we still persist once.
            InvokeBeforeDestroyOnce();
            if (m_window != nullptr && m_placeAutosaveTimerId != 0)
            {
                KillTimer(m_window, m_placeAutosaveTimerId);
            }
            PostQuitMessage(0);
            result = 0;
            return true;
        }

        default:
            break;
        }

        auto IsKeyMessage = [](UINT msg) -> bool
        {
            switch (msg)
            {
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
            case WM_CHAR:
            case WM_SYSCHAR:
            case WM_DEADCHAR:
            case WM_SYSDEADCHAR:
            case WM_UNICHAR:
                return true;
            default:
                return false;
            }
        };

        // Route keyboard input only to the focused Wnd (if any).
        if (IsKeyMessage(message) && m_focusedWnd != nullptr)
        {
            const bool handledKey = m_focusedWnd->OnMessage(message, wParam, lParam);
            if (handledKey)
            {
                result = 0;
                return true;
            }
            return false;
        }

        bool handled = false;

        // For mouse messages: Backplate is responsible for converting screen coordinates to client coordinates
        // Then forward to children with client coordinates (which match Layout coordinates since LayoutRect uses client coordinate system)
        WPARAM convertedWParam = wParam;
        LPARAM convertedLParam = lParam;
        
        // Helper to check if message is a mouse message
        auto IsMouseMessage = [](UINT msg) -> bool
        {
            switch (msg)
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
        };

        if (IsMouseMessage(message) && m_window != nullptr)
        {
            // Extract coordinates from lParam
            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            POINT ptClient = pt;
            
            // Convert screen coordinates to client coordinates if needed
            // WM_MOUSEWHEEL always uses screen coordinates
            if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
            {
                // WM_MOUSEWHEEL always provides screen coordinates
                ScreenToClient(m_window, &ptClient);
            }
            else if (GetCapture() == m_window)
            {
                // Mouse is captured: use GetCursorPos to get reliable screen coordinates, then convert
                POINT cursorPos;
                GetCursorPos(&cursorPos);
                ScreenToClient(m_window, &cursorPos);
                ptClient = cursorPos;
            }
            // Otherwise: WM_LBUTTONDOWN, WM_MOUSEMOVE (when not captured) already provide client coordinates
            // No conversion needed
            
            // Reconstruct lParam with client coordinates (which match Layout coordinate system)
            convertedLParam = MAKELPARAM(ptClient.x, ptClient.y);
        }

        // Focus-based routing for non-mouse messages:
        // Avoid broadcasting custom/timer messages to every top-level Wnd when multiple ImageBrowsers exist.
        if (!IsMouseMessage(message) && m_focusedWnd != nullptr)
        {
            if (m_focusedWnd->OnMessage(message, convertedWParam, convertedLParam))
            {
                result = 0;
                return true;
            }
            return false;
        }

        // Forward to children with converted coordinates (client/Layout coordinate system)
        for (auto& pair : m_children)
        {
            if (pair.second && pair.second->OnMessage(message, convertedWParam, convertedLParam))
            {
                handled = true;
            }
        }

        if (handled)
        {
            result = 0;
            return true;
        }
        else
            return false;
    }

    void Backplate::SetFocusedWnd(Wnd* wnd)
    {
        m_focusedWnd = wnd;
    }

    void Backplate::ClearFocusIf(Wnd* wnd)
    {
        if (m_focusedWnd == wnd)
        {
            m_focusedWnd = nullptr;
        }
    }

    bool Backplate::RegisterClass(const WindowOptions& options)
    {
        if (m_classRegistered)
        {
            return true;
        }

        HINSTANCE hInstance = options.instance;
        if (hInstance == nullptr)
        {
            hInstance = Core::Instance();
            if (hInstance == nullptr)
            {
                return false;
            }
        }

        WNDCLASSEXW wcex {};
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = Backplate::WndProc;
        wcex.cbClsExtra = 0;
        wcex.cbWndExtra = 0;
        wcex.hInstance = hInstance;
        wcex.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        // No GDI background brush; the swapchain is the only surface we want presented.
        wcex.hbrBackground = nullptr;
        wcex.lpszMenuName = nullptr;
        wcex.lpszClassName = options.className;
        wcex.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

        if (RegisterClassExW(&wcex) == 0)
        {
            DWORD error = GetLastError();
            // 클래스가 이미 등록되어 있으면 성공으로 간주
            if (error == ERROR_ALREADY_EXISTS)
            {
                m_classRegistered = true;
                return true;
            }
            return false;
        }

        m_classRegistered = true;
        return true;
    }

    HRESULT Backplate::Subclass(HWND windowHandle)
    {
        SetWindowLongPtr(windowHandle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        m_prevWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(windowHandle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Backplate::WndProc)));

        if (m_prevWndProc == nullptr)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        return S_OK;
    }

    HRESULT Backplate::Attach(HWND windowHandle)
    {
        m_window = windowHandle;

        RECT clientRect {};
        GetClientRect(m_window, &clientRect);
        m_size = D2D1::SizeU(clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);

        HRESULT hr = Subclass(windowHandle);
        if (FAILED(hr))
        {
            return hr;
        }

        return EnsureRenderTarget();
    }

    HRESULT Backplate::CreateWindowed(const WindowOptions& options)
    {
        WindowOptions opts = options;
        if (opts.instance == nullptr)
        {
            HINSTANCE coreInstance = Core::Instance();
            if (coreInstance == nullptr)
            {
                return E_POINTER;
            }
            opts.instance = coreInstance;
        }

        if (!RegisterClass(opts))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        DWORD style = opts.style;
        DWORD exStyle = opts.exStyle;

        if (style == 0)
        {
            style = (opts.chrome == ChromeStyle::Standard) ? WS_OVERLAPPEDWINDOW : WS_POPUP;
        }

        HWND window = CreateWindowExW(
            exStyle,
            opts.className,
            opts.title,
            style,
            CW_USEDEFAULT,
            0,
            static_cast<int>(opts.width),
            static_cast<int>(opts.height),
            nullptr,
            nullptr,
            opts.instance,
            this);

        if (window == nullptr)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        RECT rc {};
        GetClientRect(window, &rc);
        m_size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

        m_rendererId = (opts.rendererId != nullptr) ? opts.rendererId : L"";

        return S_OK;
    }

    int Backplate::RunMessageLoop()
    {
        MSG msg {};
        while (GetMessage(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        return static_cast<int>(msg.wParam);
    }

    HWND Backplate::Window() const
    {
        return m_window;
    }

    void Backplate::SetName(const std::wstring& name)
    {
        m_name = name;
    }

    const std::wstring& Backplate::Name() const
    {
        return m_name;
    }

    HRESULT Backplate::EnsureRenderTarget()
    {
        // D2D-only path (compatibility renderer)
        if (m_rendererId == L"d2d_hwndrt")
        {
            return EnsureRenderTargetD2D();
        }

        HRESULT hr = S_OK;
        if (!m_d3dDevice || !m_d3dContext || !m_d2dDevice || !m_d2dContext || !m_swapChain)
        {
            hr = CreateRenderTarget();
            if (FAILED(hr))
            {
                return FallbackToD2DOnly(hr);
            }
        }

        if (!m_rtv || !m_d2dTargetBitmap)
        {
            hr = RecreateSwapChainTargets();
            if (FAILED(hr))
            {
                return FallbackToD2DOnly(hr);
            }
        }

        // Update title bar info after render target is created/ensured
        UpdateTitleBarInfo();

        return S_OK;
    }

    HRESULT Backplate::EnsureRenderTargetD2D()
    {
        if (!m_hwndRenderTarget)
        {
            return CreateRenderTargetD2D();
        }
        return S_OK;
    }

    HRESULT Backplate::CreateRenderTargetD2D()
    {
        // Tear down D3D/DXGI resources if we are switching or if they exist.
        DiscardDeviceResources();

        if (m_window == nullptr)
        {
            return E_INVALIDARG;
        }

        ID2D1Factory* factory = Core::D2DFactory();
        if (factory == nullptr)
        {
            return E_POINTER;
        }

        RECT rc {};
        GetClientRect(m_window, &rc);
        m_size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            // DEFAULT lets D2D decide the most compatible path (HW when possible).
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            96.0f,
            96.0f,
            D2D1_RENDER_TARGET_USAGE_NONE,
            D2D1_FEATURE_LEVEL_DEFAULT);

        const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
            m_window,
            m_size,
            D2D1_PRESENT_OPTIONS_NONE);

        HRESULT hr = factory->CreateHwndRenderTarget(props, hwndProps, &m_hwndRenderTarget);
        if (SUCCEEDED(hr) && m_hwndRenderTarget)
        {
            // Enable high-quality antialiasing for better image quality
            // PER_PRIMITIVE is the highest quality mode (available in all Direct2D versions)
            m_hwndRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            
            // Enable ClearType for text rendering (highest quality)
            m_hwndRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        }
        return hr;
    }

    HRESULT Backplate::FallbackToD2DOnly(HRESULT causeHr)
    {
        // Switch to D2D-only renderer as a compatibility fallback.
        // Note: this disables the D3D pass (e.g. GPU-native DDS), but keeps the app usable.
        UNREFERENCED_PARAMETER(causeHr);

        m_rendererId = L"d2d_hwndrt";
        HRESULT hr = EnsureRenderTargetD2D();
        UpdateTitleBarInfo();
        return hr;
    }

    HRESULT Backplate::RecreateSwapChainTargets()
    {
        if (!m_swapChain || !m_d3dDevice || !m_d2dContext)
        {
            return E_POINTER;
        }

        DiscardD2DTargets();

        // Recreate RTV from swapchain backbuffer.
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBufferTex;
        HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBufferTex));
        if (FAILED(hr))
        {
            return hr;
        }

        hr = m_d3dDevice->CreateRenderTargetView(backBufferTex.Get(), nullptr, &m_rtv);
        if (FAILED(hr))
        {
            return hr;
        }

        // Recreate D2D target bitmap from swapchain surface.
        Microsoft::WRL::ComPtr<IDXGISurface> backBuffer;
        hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr))
        {
            return hr;
        }

        const D2D1_BITMAP_PROPERTIES1 bp = MakeSwapChainBitmapProps();
        hr = m_d2dContext->CreateBitmapFromDxgiSurface(backBuffer.Get(), &bp, &m_d2dTargetBitmap);
        if (FAILED(hr))
        {
            return hr;
        }

        m_d2dContext->SetTarget(m_d2dTargetBitmap.Get());
        return S_OK;
    }

    HRESULT Backplate::CreateRenderTarget()
    {
        DiscardDeviceResources();

        if (m_window == nullptr)
        {
            return E_INVALIDARG;
        }

        // --- Create D3D11 device (BGRA required for D2D interop) ---
        UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        D3D_FEATURE_LEVEL featureLevels[] =
        {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };

        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            deviceFlags,
            featureLevels,
            static_cast<UINT>(std::size(featureLevels)),
            D3D11_SDK_VERSION,
            &m_d3dDevice,
            &featureLevel,
            &m_d3dContext);
        if (FAILED(hr))
        {
            // Retry without debug device if not installed/available.
            deviceFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                deviceFlags,
                featureLevels,
                static_cast<UINT>(std::size(featureLevels)),
                D3D11_SDK_VERSION,
                &m_d3dDevice,
                &featureLevel,
                &m_d3dContext);
        }
        if (FAILED(hr))
        {
            return hr;
        }

        // --- Create D2D device/context from the DXGI device ---
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        hr = m_d3dDevice.As(&dxgiDevice);
        if (FAILED(hr))
        {
            return hr;
        }

        ID2D1Factory1* factory1 = Core::D2DFactory1();
        if (factory1 == nullptr)
        {
            return E_POINTER;
        }

        hr = factory1->CreateDevice(dxgiDevice.Get(), &m_d2dDevice);
        if (FAILED(hr))
        {
            return hr;
        }

        hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2dContext);
        if (FAILED(hr))
        {
            return hr;
        }

        // Enable high-quality antialiasing and interpolation based on Direct2D version
        if (m_d2dContext)
        {
            // Antialiasing: PER_PRIMITIVE is the highest quality mode (available in all D2D versions)
            m_d2dContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            
            // Text antialiasing: CLEARTYPE is the highest quality mode (available in all D2D versions)
            m_d2dContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
            
            // Interpolation mode: Select the best mode based on Direct2D version
            FD2D::D2DVersion d2dVersion = FD2D::Core::GetSupportedD2DVersion();
            if (d2dVersion >= FD2D::D2DVersion::D2D1_3)
            {
                // Direct2D 1.3+ supports HIGH_QUALITY_CUBIC (best quality, available in Windows 10+)
                #ifdef D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
                m_d2dContext->SetInterpolationMode(D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                #elif defined(D2D1_INTERPOLATION_MODE_CUBIC)
                // Fallback to CUBIC if HIGH_QUALITY_CUBIC is not available
                m_d2dContext->SetInterpolationMode(D2D1_INTERPOLATION_MODE_CUBIC);
                #endif
            }
            else if (d2dVersion >= FD2D::D2DVersion::D2D1_1)
            {
                // Direct2D 1.1-1.2: Use CUBIC (best available)
                #ifdef D2D1_INTERPOLATION_MODE_CUBIC
                m_d2dContext->SetInterpolationMode(D2D1_INTERPOLATION_MODE_CUBIC);
                #endif
            }
            // Direct2D 1.0: Use default (LINEAR), which is already set
        }

        // Update title bar info after render target is created
        UpdateTitleBarInfo();

        RECT clientRect {};
        GetClientRect(m_window, &clientRect);
        m_size = D2D1::SizeU(clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);

        // --- Create swap chain for the window ---
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        hr = dxgiDevice->GetAdapter(&adapter);
        if (FAILED(hr))
        {
            return hr;
        }

        Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory;
        hr = adapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
        if (FAILED(hr))
        {
            return hr;
        }

        DXGI_SWAP_CHAIN_DESC1 scd {};
        scd.Width = m_size.width;
        scd.Height = m_size.height;
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.Stereo = FALSE;
        scd.SampleDesc.Count = 1;
        scd.SampleDesc.Quality = 0;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.Scaling = DXGI_SCALING_STRETCH;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        hr = dxgiFactory->CreateSwapChainForHwnd(
            m_d3dDevice.Get(),
            m_window,
            &scd,
            nullptr,
            nullptr,
            &m_swapChain);
        if (FAILED(hr))
        {
            return hr;
        }

        // Create D2D target bitmap from swap chain back buffer
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBufferTex;
        hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBufferTex));
        if (FAILED(hr))
        {
            return hr;
        }

        hr = m_d3dDevice->CreateRenderTargetView(backBufferTex.Get(), nullptr, &m_rtv);
        if (FAILED(hr))
        {
            return hr;
        }

        Microsoft::WRL::ComPtr<IDXGISurface> backBuffer;
        hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr))
        {
            return hr;
        }

        const D2D1_BITMAP_PROPERTIES1 bp = MakeSwapChainBitmapProps();
        hr = m_d2dContext->CreateBitmapFromDxgiSurface(backBuffer.Get(), &bp, &m_d2dTargetBitmap);
        if (FAILED(hr))
        {
            return hr;
        }

        m_d2dContext->SetTarget(m_d2dTargetBitmap.Get());
        return S_OK;
    }

    void Backplate::DiscardD2DTargets()
    {
        if (m_d2dContext)
        {
            // Release swapchain backbuffer references held by D2D before any swapchain operations.
            m_d2dContext->SetTarget(nullptr);
            m_d2dContext->Flush();
        }

        m_rtv.Reset();
        m_d2dTargetBitmap.Reset();
    }

    void Backplate::DiscardDeviceResources()
    {
        DiscardD2DTargets();

        if (m_d3dContext)
        {
            // Force deferred destruction of swapchain-related resources before releasing the swapchain.
            m_d3dContext->OMSetRenderTargets(0, nullptr, nullptr);
            m_d3dContext->ClearState();
            m_d3dContext->Flush();
        }

        m_d2dContext.Reset();
        m_d2dDevice.Reset();
        m_swapChain.Reset();
        m_d3dContext.Reset();
        m_d3dDevice.Reset();

        m_hwndRenderTarget.Reset();
    }

    void Backplate::Resize(UINT width, UINT height)
    {
        if (m_window != nullptr)
        {
            RECT rc {};
            GetClientRect(m_window, &rc);
            m_size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
        }
        else
        {
            m_size = D2D1::SizeU(width, height);
        }

        if (m_hwndRenderTarget)
        {
            (void)m_hwndRenderTarget->Resize(m_size);
        }
        else if (m_swapChain && m_d2dContext)
        {
            m_d2dTargetBitmap.Reset();
            (void)m_d2dContext->SetTarget(nullptr);
            m_rtv.Reset();

            // Resize swap chain buffers
            (void)m_swapChain->ResizeBuffers(0, m_size.width, m_size.height, DXGI_FORMAT_UNKNOWN, 0);

            Microsoft::WRL::ComPtr<ID3D11Texture2D> backBufferTex;
            if (SUCCEEDED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBufferTex))))
            {
                (void)m_d3dDevice->CreateRenderTargetView(backBufferTex.Get(), nullptr, &m_rtv);
            }

            Microsoft::WRL::ComPtr<IDXGISurface> backBuffer;
            if (SUCCEEDED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
            {
                const D2D1_BITMAP_PROPERTIES1 bp = MakeSwapChainBitmapProps();
                if (SUCCEEDED(m_d2dContext->CreateBitmapFromDxgiSurface(backBuffer.Get(), &bp, &m_d2dTargetBitmap)))
                {
                    m_d2dContext->SetTarget(m_d2dTargetBitmap.Get());
                }
            }
        }

        Rect root { 0.0f, 0.0f, static_cast<float>(m_size.width), static_cast<float>(m_size.height) };
        for (auto& pair : m_children)
        {
            if (pair.second)
            {
                pair.second->Measure({ root.w, root.h });
                pair.second->Arrange(root);
            }
        }

        if (m_window != nullptr)
        {
            InvalidateRect(m_window, nullptr, FALSE);
        }

        m_layoutDirty = true;
    }

    void Backplate::RequestLayout()
    {
        m_layoutDirty = true;
        if (m_window != nullptr)
        {
            InvalidateRect(m_window, nullptr, FALSE);
        }
    }

    void Backplate::Render()
    {
        if (m_layoutDirty)
        {
            Layout();
        }
        HRESULT hrEnsure = EnsureRenderTarget();
        if (FAILED(hrEnsure))
        {
            // If D3D path failed, try automatic fallback to D2D-only once.
            if (m_rendererId != L"d2d_hwndrt" && SUCCEEDED(FallbackToD2DOnly(hrEnsure)))
            {
                // Continue rendering with D2D-only below.
            }
            else
            {
                return;
            }
        }

        // D2D-only renderer path (no D3D pass).
        if (m_hwndRenderTarget)
        {
            m_hwndRenderTarget->BeginDraw();
            m_hwndRenderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
            // Dark neutral gray with a *tiny* blue bias (low saturation)
            m_hwndRenderTarget->Clear(D2D1::ColorF(0.09f, 0.09f, 0.10f, 1.0f));

            for (auto& pair : m_children)
            {
                if (pair.second)
                {
                    pair.second->OnRender(m_hwndRenderTarget.Get());
                }
            }

            HRESULT hr = m_hwndRenderTarget->EndDraw();
            if (hr == D2DERR_RECREATE_TARGET)
            {
                m_hwndRenderTarget.Reset();
            }
            return;
        }

        // D3D pass (background + GPU images)
        if (m_d3dContext && m_rtv)
        {
            // Dark neutral gray with a *tiny* blue bias (low saturation)
            const float clearColor[4] = { 0.09f, 0.09f, 0.10f, 1.0f };
            m_d3dContext->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
            m_d3dContext->ClearRenderTargetView(m_rtv.Get(), clearColor);

            D3D11_VIEWPORT vp {};
            vp.TopLeftX = 0.0f;
            vp.TopLeftY = 0.0f;
            vp.Width = static_cast<float>(m_size.width);
            vp.Height = static_cast<float>(m_size.height);
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            m_d3dContext->RSSetViewports(1, &vp);

            for (auto& pair : m_children)
            {
                if (pair.second)
                {
                    pair.second->OnRenderD3D(m_d3dContext.Get());
                }
            }

            // IMPORTANT: ensure we release the swapchain backbuffer from the D3D OM stage
            // before letting D2D draw to it.
            ID3D11RenderTargetView* nullRTV[1] = { nullptr };
            m_d3dContext->OMSetRenderTargets(1, nullRTV, nullptr);
        }

        // D2D pass (UI overlays)
        bool d2dOk = true;
        // Ensure we have a valid target for the UI pass.
        if (!m_d2dTargetBitmap || !m_rtv)
        {
            (void)RecreateSwapChainTargets();
        }

        // We detach the target after each frame; ensure it is set for this draw.
        if (m_d2dContext && m_d2dTargetBitmap)
        {
            m_d2dContext->SetTarget(m_d2dTargetBitmap.Get());
        }

        m_d2dContext->BeginDraw();
        m_d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());

        for (auto& pair : m_children)
        {
            if (pair.second)
            {
                pair.second->OnRender(m_d2dContext.Get());
            }
        }

        HRESULT hr = m_d2dContext->EndDraw();
        if (FAILED(hr))
        {
            d2dOk = false;
            // Release the D2D target immediately so the swapchain backbuffer isn't held.
            DiscardD2DTargets();

            // Device lost -> full recreate next frame. Otherwise, just recreate targets.
            if (IsDeviceRemovedHr(hr))
            {
                DiscardDeviceResources();
            }
        }

        // Detach target before Present for clean interop.
        if (m_d2dContext)
        {
            m_d2dContext->SetTarget(nullptr);
        }

        if (m_swapChain)
        {
            (void)m_swapChain->Present(1, 0);
        }
    }

    void Backplate::Layout()
    {
        D2D1_SIZE_F size { static_cast<FLOAT>(m_size.width), static_cast<FLOAT>(m_size.height) };

        for (auto& pair : m_children)
        {
            if (pair.second)
            {
                pair.second->Measure({ size.width, size.height });
                pair.second->Arrange({ 0.0f, 0.0f, size.width, size.height });
            }
        }

        m_layoutDirty = false;
    }

    void Backplate::Show(int nCmdShow)
    {
        if (m_window)
        {
            ShowWindow(m_window, nCmdShow);
            UpdateWindow(m_window);
        }
    }

    bool Backplate::AddWnd(const std::shared_ptr<Wnd>& wnd)
    {
        if (!wnd || wnd->Name().empty())
        {
            return false;
        }

        if (m_children.find(wnd->Name()) != m_children.end())
        {
            return false;
        }

        m_children.emplace(wnd->Name(), wnd);
        wnd->OnAttached(*this);
        wnd->Measure({ static_cast<float>(m_size.width), static_cast<float>(m_size.height) });
        wnd->Arrange({ 0.0f, 0.0f, static_cast<float>(m_size.width), static_cast<float>(m_size.height) });

        if (m_window != nullptr)
        {
            InvalidateRect(m_window, nullptr, FALSE);
        }

        m_layoutDirty = true;

        return true;
    }

    bool Backplate::OnMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        bool handled = false;

        for (auto& pair : m_children)
        {
            if (pair.second && pair.second->OnMessage(message, wParam, lParam))
            {
                handled = true;
            }
        }

        return handled;
    }

    ID2D1RenderTarget* Backplate::RenderTarget() const
    {
        if (m_hwndRenderTarget)
        {
            return m_hwndRenderTarget.Get();
        }
        return m_d2dContext.Get();
    }

    void Backplate::UpdateTitleBarInfo()
    {
        if (m_window == nullptr)
        {
            return;
        }

        // Get current window title
        wchar_t currentTitle[256] = {};
        GetWindowTextW(m_window, currentTitle, static_cast<int>(std::size(currentTitle)));

        // Extract base title (remove existing info if present)
        std::wstring baseTitle = currentTitle;
        size_t infoPos = baseTitle.find(L" [");
        if (infoPos != std::wstring::npos)
        {
            baseTitle = baseTitle.substr(0, infoPos);
        }
        if (baseTitle.empty())
        {
            baseTitle = L"FICture2"; // Default title
        }

        // Get Direct2D version
        const char* d2dVersionStr = Core::GetD2DVersionString();
        
        // Extract version number (e.g., "Direct2D 1.3 (Windows 10+)" -> "1.3")
        std::string d2dVersionA = d2dVersionStr;
        size_t versionPos = d2dVersionA.find("1.");
        std::string versionNum = "1.0";
        if (versionPos != std::string::npos)
        {
            size_t endPos = versionPos + 3; // "1.x"
            if (endPos > d2dVersionA.length()) endPos = d2dVersionA.length();
            versionNum = d2dVersionA.substr(versionPos, endPos - versionPos);
        }

        // Convert to wide string
        size_t len = versionNum.length();
        std::wstring versionNumW(len + 1, L'\0');
        mbstowcs_s(nullptr, &versionNumW[0], len + 1, versionNum.c_str(), len);
        versionNumW.resize(len);

        // Check if using D3D11 renderer
        bool usingD3D11 = (m_rendererId.empty() || m_rendererId == L"d3d11_swapchain");

        // Build new title with info on the right
        wchar_t newTitle[512];
        if (usingD3D11)
        {
            swprintf_s(newTitle, L"%ls [D2D %ls | D3D11]", baseTitle.c_str(), versionNumW.c_str());
        }
        else
        {
            swprintf_s(newTitle, L"%ls [D2D %ls]", baseTitle.c_str(), versionNumW.c_str());
        }

        SetWindowTextW(m_window, newTitle);
    }
}


