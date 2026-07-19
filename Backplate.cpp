#include "Backplate.h"
#include "Core.h"
#include "Util.h"
#include "FD2DLog.h"
#include <cmath>
#include <cstring>
#include <dxgi1_3.h>
#include <string>
#include <algorithm>
#include <shellapi.h>
#include <windowsx.h>  // For GET_X_LPARAM, GET_Y_LPARAM, MAKELPARAM
#include <ole2.h>
#include <oleidl.h>
#include <vector>

namespace FD2D
{
    static bool IsDeviceRemovedHr(HRESULT hr)
    {
        return hr == DXGI_ERROR_DEVICE_REMOVED
            || hr == DXGI_ERROR_DEVICE_RESET
            || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }

    static InputEventType ToInputEventType(UINT message)
    {
        static std::unordered_map <UINT, InputEventType> mapMsg2EventType = {
            { WM_MOUSEMOVE, InputEventType::MouseMove },
            { WM_LBUTTONDOWN, InputEventType::MouseDown },
            { WM_RBUTTONDOWN, InputEventType::MouseDown },
            { WM_MBUTTONDOWN, InputEventType::MouseDown },
            { WM_XBUTTONDOWN, InputEventType::MouseDown },
            { WM_LBUTTONUP, InputEventType::MouseUp },
            { WM_RBUTTONUP, InputEventType::MouseUp },
            { WM_MBUTTONUP, InputEventType::MouseUp },
            { WM_XBUTTONUP, InputEventType::MouseUp },
            { WM_LBUTTONDBLCLK, InputEventType::MouseDoubleClick },
            { WM_RBUTTONDBLCLK, InputEventType::MouseDoubleClick },
            { WM_MBUTTONDBLCLK, InputEventType::MouseDoubleClick },
            { WM_XBUTTONDBLCLK, InputEventType::MouseDoubleClick },
            { WM_MOUSEWHEEL, InputEventType::MouseWheel },
            { WM_MOUSEHWHEEL, InputEventType::MouseHWheel },
            { WM_MOUSELEAVE, InputEventType::MouseLeave },
            { WM_CAPTURECHANGED, InputEventType::CaptureChanged },
            { WM_SETCURSOR, InputEventType::SetCursor },
            { WM_KEYDOWN, InputEventType::KeyDown },
            { WM_SYSKEYDOWN, InputEventType::KeyDown },
            { WM_KEYUP, InputEventType::KeyUp },
            { WM_SYSKEYUP, InputEventType::KeyUp },
            { WM_CHAR, InputEventType::Char },
            { WM_SYSCHAR, InputEventType::SystemChar },
            { WM_DEADCHAR, InputEventType::DeadChar },
            { WM_SYSDEADCHAR, InputEventType::SystemDeadChar },
            { WM_UNICHAR, InputEventType::UniChar }
        };

		auto it = mapMsg2EventType.find(message);
        if (it != mapMsg2EventType.end())
            return it->second;
        else
			return InputEventType::None;
    }

    static MouseButton ToMouseButton(UINT message, WPARAM wParam)
    {
        static std::unordered_map<UINT, MouseButton> mapMsg2MouseButton = {
            { WM_LBUTTONDOWN, MouseButton::Left },
            { WM_RBUTTONDOWN, MouseButton::Right },
            { WM_MBUTTONDOWN, MouseButton::Middle },
            { WM_LBUTTONUP, MouseButton::Left },
            { WM_RBUTTONUP, MouseButton::Right },
            { WM_MBUTTONUP, MouseButton::Middle },
            { WM_LBUTTONDBLCLK, MouseButton::Left },
            { WM_RBUTTONDBLCLK, MouseButton::Right },
            { WM_MBUTTONDBLCLK, MouseButton::Middle },
		};
		auto it = mapMsg2MouseButton.find(message);
        if (it != mapMsg2MouseButton.end())
            return it->second;
        else
            return MouseButton::None;
    }

    static bool IsRoutedMouseMessage(UINT message)
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
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_CAPTURECHANGED:
            return true;
        default:
            return false;
        }
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
        m_asyncRedrawControl = std::make_shared<AsyncRedrawToken::ControlBlock>();
        m_asyncRedrawControl->backplate = this;
    }

    Backplate::Backplate(const std::wstring& name)
        : m_name(name)
    {
        m_asyncRedrawEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_asyncRedrawControl = std::make_shared<AsyncRedrawToken::ControlBlock>();
        m_asyncRedrawControl->backplate = this;
    }

    Backplate::~Backplate()
    {
        // Drop the HWND first so any Invalidate/Render triggered by the
        // Shutdown invalidation cascade cannot touch a destroyed window.
        m_window = nullptr;
        DetachAsyncRedrawControl();
        NotifyGraphicsInvalidated(GraphicsInvalidationReason::Shutdown);
        UnregisterDropTarget();

        if (m_asyncRedrawEvent)
        {
            CloseHandle(m_asyncRedrawEvent);
            m_asyncRedrawEvent = nullptr;
        }
    }

    AsyncRedrawToken::AsyncRedrawToken(std::weak_ptr<ControlBlock> control)
        : m_control(std::move(control))
    {
    }

    void AsyncRedrawToken::RequestAsyncRedraw() const
    {
        auto control = m_control.lock();
        if (!control)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(control->mutex);
        if (control->backplate)
        {
            control->backplate->RequestAsyncRedraw();
        }
    }

    void Backplate::DetachAsyncRedrawControl()
    {
        if (!m_asyncRedrawControl)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_asyncRedrawControl->mutex);
        m_asyncRedrawControl->backplate = nullptr;
    }

    std::shared_ptr<AsyncRedrawToken> Backplate::GetAsyncRedrawToken() const
    {
        if (!m_asyncRedrawControl)
        {
            return nullptr;
        }
        return std::shared_ptr<AsyncRedrawToken>(new AsyncRedrawToken(m_asyncRedrawControl));
    }

    void Backplate::InvalidateGraphics(
        GraphicsInvalidationReason reason,
        bool bumpDevice,
        bool bumpTarget,
        bool bumpRenderer)
    {
        if (bumpDevice)
        {
            ++m_graphicsGeneration.device;
        }
        if (bumpTarget)
        {
            ++m_graphicsGeneration.target;
        }
        if (bumpRenderer)
        {
            ++m_graphicsGeneration.renderer;
        }
        NotifyGraphicsInvalidated(reason);
    }

    void Backplate::NotifyGraphicsInvalidated(GraphicsInvalidationReason reason)
    {
        const GraphicsGeneration generation = m_graphicsGeneration;
        for (const auto& child : m_childrenOrdered)
        {
            if (child)
            {
                child->OnGraphicsInvalidated(reason, generation);
            }
        }
    }

    void Backplate::ScheduleNextFrame()
    {
        if (m_window != nullptr && IsWindow(m_window))
        {
            InvalidateRect(m_window, nullptr, FALSE);
        }
    }

    void Backplate::LogDeviceRemovedReason(HRESULT triggerHr, const char* where) const
    {
        if (m_d3dDevice)
        {
            const HRESULT reasonHr = m_d3dDevice->GetDeviceRemovedReason();
            FD2D_LOG_INFO(
                "[Graphics] device lost at {}: hr=0x{:08X} GetDeviceRemovedReason=0x{:08X}",
                where ? where : "?",
                static_cast<unsigned>(triggerHr),
                static_cast<unsigned>(reasonHr));
        }
        else
        {
            FD2D_LOG_INFO(
                "[Graphics] device lost at {}: hr=0x{:08X} (no D3D device)",
                where ? where : "?",
                static_cast<unsigned>(triggerHr));
        }
    }

    bool Backplate::HandleDeviceLostHr(HRESULT hr, const char* where)
    {
        if (!IsDeviceRemovedHr(hr))
        {
            return false;
        }

        LogDeviceRemovedReason(hr, where);
        DiscardDeviceResources();
        InvalidateGraphics(GraphicsInvalidationReason::DeviceLost, true, true, false);
        ScheduleNextFrame();
        return true;
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

    namespace
    {
        static bool DataObjectHasHDrop(IDataObject* dataObject)
        {
            if (dataObject == nullptr)
            {
                return false;
            }

            FORMATETC fmt {};
            fmt.cfFormat = CF_HDROP;
            fmt.ptd = nullptr;
            fmt.dwAspect = DVASPECT_CONTENT;
            fmt.lindex = -1;
            fmt.tymed = TYMED_HGLOBAL;
            return dataObject->QueryGetData(&fmt) == S_OK;
        }

        // Extracts file paths from a CF_HDROP data object.
        // When firstOnly is true, only the first path is queried.
        static std::vector<std::wstring> ExtractHDropPaths(IDataObject* dataObject, bool firstOnly)
        {
            std::vector<std::wstring> out;
            if (dataObject == nullptr)
            {
                return out;
            }

            FORMATETC fmt {};
            fmt.cfFormat = CF_HDROP;
            fmt.ptd = nullptr;
            fmt.dwAspect = DVASPECT_CONTENT;
            fmt.lindex = -1;
            fmt.tymed = TYMED_HGLOBAL;

            STGMEDIUM stg {};
            if (FAILED(dataObject->GetData(&fmt, &stg)))
            {
                return out;
            }

            const HDROP hDrop = reinterpret_cast<HDROP>(stg.hGlobal);
            if (hDrop != nullptr)
            {
                wchar_t buf[MAX_PATH] {};
                const UINT fileCount = firstOnly
                    ? 1U
                    : DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                out.reserve(fileCount);
                for (UINT i = 0; i < fileCount; ++i)
                {
                    const UINT cch = DragQueryFileW(hDrop, i, buf, static_cast<UINT>(std::size(buf)));
                    if (cch == 0)
                    {
                        continue;
                    }
                    out.emplace_back(buf);
                }
            }

            ReleaseStgMedium(&stg);
            return out;
        }

        static std::wstring GetFirstPathFromDataObject(IDataObject* dataObject)
        {
            const auto paths = ExtractHDropPaths(dataObject, true /*firstOnly*/);
            return paths.empty() ? std::wstring {} : paths.front();
        }

        static std::vector<std::wstring> GetAllPathsFromDataObject(IDataObject* dataObject)
        {
            return ExtractHDropPaths(dataObject, false /*firstOnly*/);
        }
    }

    class Backplate::DropTarget final : public IDropTarget
    {
        public:
            explicit DropTarget(Backplate* owner)
                : m_owner(owner)
            {
            }

            HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObject) override
            {
                if (ppvObject == nullptr)
                {
                    return E_POINTER;
                }

                if (riid == IID_IUnknown || riid == IID_IDropTarget)
                {
                    *ppvObject = static_cast<IDropTarget*>(this);
                    AddRef();
                    return S_OK;
                }

                *ppvObject = nullptr;
                return E_NOINTERFACE;
            }

            ULONG __stdcall AddRef() override
            {
                return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
            }

            ULONG __stdcall Release() override
            {
                const ULONG r = static_cast<ULONG>(InterlockedDecrement(&m_refCount));
                if (r == 0)
                {
                    delete this;
                }
                return r;
            }

            HRESULT __stdcall DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override
            {
                UNREFERENCED_PARAMETER(grfKeyState);

                if (pdwEffect == nullptr)
                {
                    return E_POINTER;
                }

                if (!DataObjectHasHDrop(pDataObj) || m_owner == nullptr)
                {
                    *pdwEffect = DROPEFFECT_NONE;
                    return S_OK;
                }

                m_owner->m_dragPath = GetFirstPathFromDataObject(pDataObj);
                return DragOver(grfKeyState, pt, pdwEffect);
            }

            HRESULT __stdcall DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override
            {
                UNREFERENCED_PARAMETER(grfKeyState);

                if (pdwEffect == nullptr)
                {
                    return E_POINTER;
                }

                if (m_owner == nullptr || m_owner->m_window == nullptr || m_owner->m_dragPath.empty())
                {
                    *pdwEffect = DROPEFFECT_NONE;
                    return S_OK;
                }

                POINT ptScreen { pt.x, pt.y };
                POINT ptClient = ptScreen;
                ScreenToClient(m_owner->m_window, &ptClient);

                const bool handled = m_owner->HandleFileDragOver(m_owner->m_dragPath, ptClient);
                *pdwEffect = handled ? DROPEFFECT_COPY : DROPEFFECT_NONE;
                return S_OK;
            }

            HRESULT __stdcall DragLeave() override
            {
                if (m_owner != nullptr)
                {
                    m_owner->m_dragPath.clear();
                    m_owner->HandleFileDragLeave();
                }
                return S_OK;
            }

            HRESULT __stdcall Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override
            {
                UNREFERENCED_PARAMETER(grfKeyState);

                if (pdwEffect == nullptr)
                {
                    return E_POINTER;
                }

                if (!DataObjectHasHDrop(pDataObj) || m_owner == nullptr || m_owner->m_window == nullptr)
                {
                    *pdwEffect = DROPEFFECT_NONE;
                    return S_OK;
                }

                const auto paths = GetAllPathsFromDataObject(pDataObj);

                POINT ptScreen { pt.x, pt.y };
                POINT ptClient = ptScreen;
                ScreenToClient(m_owner->m_window, &ptClient);

                // Clear overlays first, then route the drop as a normal file drop.
                m_owner->HandleFileDragLeave();
                m_owner->m_dragPath.clear();

                const bool handled = m_owner->HandleFileDropPaths(paths, ptClient);

                *pdwEffect = handled ? DROPEFFECT_COPY : DROPEFFECT_NONE;
                return S_OK;
            }

        private:
            Backplate* m_owner { nullptr };
            volatile LONG m_refCount { 1 };
    };

    bool Backplate::EnsureDropTargetRegistered()
    {
        if (m_dropTargetRegistered)
        {
            return true;
        }

        if (m_window == nullptr)
        {
            return false;
        }

        // Register OLE drop target for live drag-over updates (overlays + per-pane routing).
        m_dropTarget.Attach(new DropTarget(this));

        const HRESULT hr = RegisterDragDrop(m_window, m_dropTarget.Get());
        if (FAILED(hr))
        {
            m_dropTarget.Reset();
            m_dropTargetRegistered = false;
            return false;
        }

        m_dropTargetRegistered = true;
        return true;
    }

    void Backplate::UnregisterDropTarget()
    {
        if (m_dropTargetRegistered && m_window != nullptr)
        {
            (void)RevokeDragDrop(m_window);
        }
        m_dropTargetRegistered = false;
        m_dropTarget.Reset();
        m_dragPath.clear();
    }

    bool Backplate::HandleFileDragOver(const std::wstring& path, const POINT& ptClient)
    {
        // OLE calls DragOver on every mouse-move during a drag (many times/sec).
        // Clearing stale visuals and setting the new one each call Invalidate(),
        // which normally renders+presents immediately -- so without batching, a
        // single DragOver could present one frame with the overlay cleared and
        // another with it set, visible as a flicker on every drag move. Defer all
        // Invalidate() calls triggered below into a single Render() at the end.
        BeginDeferredRender();

        // Clear any prior visuals from children that are no longer the drag target
        // (the loop below only reaches the first child that claims the point).
        for (const auto& child : m_childrenOrdered)
        {
            if (child)
            {
                child->OnFileDragLeave();
            }
        }

        FileDragVisual visual = FileDragVisual::None;
        bool handled = false;
        for (auto it = m_childrenOrdered.rbegin(); it != m_childrenOrdered.rend(); ++it)
        {
            if (*it && (*it)->OnFileDrag(path, ptClient, visual))
            {
                handled = true;
                break;
            }
        }

        EndDeferredRender();

        if (m_window != nullptr)
        {
            Render();
        }

        return handled;
    }

    void Backplate::HandleFileDragLeave()
    {
        BeginDeferredRender();
        for (const auto& child : m_childrenOrdered)
        {
            if (child)
            {
                child->OnFileDragLeave();
            }
        }
        EndDeferredRender();

        if (m_window != nullptr)
        {
            Render();
        }
    }

    bool Backplate::HandleFileDropPaths(const std::vector<std::wstring>& paths, const POINT& ptClient)
    {
        if (paths.empty())
        {
            return false;
        }

        for (auto it = m_childrenOrdered.rbegin(); it != m_childrenOrdered.rend(); ++it)
        {
            if (*it && (*it)->OnFileDropPaths(paths, ptClient))
            {
                return true;
            }
        }

        return false;
    }

    void Backplate::RequestAsyncRedraw()
    {
        if (!m_asyncRedrawEvent || !m_window || !IsWindow(m_window))
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
        if (!m_window || !IsWindow(m_window) || !m_asyncRedrawEvent)
        {
            return;
        }

        // Drain the pending flag and reset the event for future signals.
        m_asyncRedrawPending.store(false);
        ResetEvent(m_asyncRedrawEvent);

        // During interactive resizing, avoid synchronous repaint pressure.
        if (m_inSizeMove)
        {
            InvalidateRect(m_window, nullptr, FALSE);
        }
        else
        {
            // Trigger a prompt repaint (once per coalesced burst).
            FD2D_TIMER_START(t_redraw);
            RedrawWindow(m_window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
            const auto redrawMs = FD2D_ELAPSED_MS(t_redraw);
            if (redrawMs > 50)
            {
                FD2D_LOG_INFO("[UI stall] ProcessAsyncRedraw: RedrawWindow(UPDATENOW) took {}ms", redrawMs);
            }
        }
    }

    void Backplate::RequestAnimationFrame()
    {
        m_lastAnimationRequestMs.store(Util::NowMs());
    }

    bool Backplate::HasActiveAnimation(unsigned long long nowMs) const
    {
        const unsigned long long last = m_lastAnimationRequestMs.load();
        // Consider animation active if someone requested frames recently.
        // Use a small window to detect stale animation requests (100ms is ~6 frames at 60fps).
        // This prevents stuck animation loops when no component actually needs animation.
        return (last != 0) && (nowMs - last <= 100ULL);
    }

    void Backplate::ProcessAnimationTick(unsigned long long nowMs)
    {
        if (!m_window || !IsWindow(m_window))
        {
            return;
        }

        // Advance tooltip dwell / toast expiry first: it re-arms the animation
        // while a tooltip is pending or a toast is showing, so these keep
        // ticking even when nothing else animates.
        AdvanceHoverToast(nowMs);

        if (!HasActiveAnimation(nowMs))
        {
            return;
        }

        // Adaptive animation cadence:
        // - Default: ~60fps for smooth interactions.
        // - While async redraw bursts are pending or during live resize:
        //   back off to ~30fps to reduce UI-thread render pressure.
        const bool asyncPending = m_asyncRedrawPending.load();
        const unsigned long long minTickIntervalMs =
            (m_inSizeMove || asyncPending) ? 33ULL : 16ULL;

        // Diagnostic: log only when the cadence actually changes (not every tick), so we
        // get crisp "throttle engaged/lifted" markers to correlate with the [FPS] summary.
        if (minTickIntervalMs != m_lastLoggedTickIntervalMs)
        {
            FD2D_LOG_INFO(
                "[FPS] animation tick cadence -> {}ms ({}fps target)  inSizeMove={} asyncRedrawPending={}",
                minTickIntervalMs, minTickIntervalMs > 0 ? (1000ULL / minTickIntervalMs) : 0ULL,
                m_inSizeMove, asyncPending);
            m_lastLoggedTickIntervalMs = minTickIntervalMs;
        }

        const unsigned long long lastTick = m_lastAnimationTickMs.load();
        if (lastTick != 0 && (nowMs - lastTick) < minTickIntervalMs)
        {
            return;
        }
        m_lastAnimationTickMs.store(nowMs);

        // Direct rendering: bypass message loop for smoother 60fps animation.
        // Log frames that take > 100ms (rate-limited to one log per 100ms to avoid flooding).
        FD2D_TIMER_START(t_frame);
        NoteRenderTrigger(RenderTrigger::Tick);
        Render();
        const auto frameMs = FD2D_ELAPSED_MS(t_frame);
        if (frameMs > 100)
        {
            static std::chrono::steady_clock::time_point s_lastSlowFrameLog {};
            const auto nowTp = std::chrono::steady_clock::now();
            if (nowTp - s_lastSlowFrameLog >= std::chrono::milliseconds(100))
            {
                FD2D_LOG_INFO("[UI stall] ProcessAnimationTick: Render took {}ms", frameMs);
                s_lastSlowFrameLog = nowTp;
            }
        }
    }

    Wnd* Backplate::FindTargetWnd(const POINT& ptClient)
    {
        UNREFERENCED_PARAMETER(ptClient);
        return nullptr;
    }

    namespace
    {
        constexpr unsigned long long kTooltipDwellMs = 500ULL;
        constexpr unsigned long long kToastDurationMs = 1800ULL;
    }

    Wnd* Backplate::HitTestTopLevel(const POINT& pt)
    {
        for (auto it = m_childrenOrdered.rbegin(); it != m_childrenOrdered.rend(); ++it)
        {
            if (*it)
            {
                if (Wnd* hit = (*it)->HitTestDeepest(pt))
                {
                    return hit;
                }
            }
        }
        return nullptr;
    }

    void Backplate::UpdateHoverTarget(const POINT& ptClient)
    {
        m_hoverPt = ptClient;
        Wnd* hit = HitTestTopLevel(ptClient);
        std::wstring tip = hit ? hit->TooltipText() : std::wstring();

        // Same control + same tip: keep the running dwell (and any shown
        // tooltip) so small jitters don't restart it.
        if (hit == m_hoverWnd && tip == m_hoverTip)
        {
            return;
        }

        const bool wasShown = m_tipShown;
        m_hoverWnd = hit;
        m_hoverTip = std::move(tip);
        m_hoverSinceMs = Util::NowMs();
        m_tipShown = false;
        if (!m_hoverTip.empty())
        {
            RequestAnimationFrame(); // drive the dwell timer via ProcessAnimationTick
        }
        else if (wasShown && m_window != nullptr)
        {
            InvalidateRect(m_window, nullptr, FALSE); // erase the tooltip that was showing
        }
    }

    void Backplate::ClearHoverTooltip()
    {
        m_hoverWnd = nullptr;
        m_hoverTip.clear();
        m_hoverSinceMs = 0;
        if (m_tipShown)
        {
            m_tipShown = false;
            if (m_window != nullptr)
            {
                InvalidateRect(m_window, nullptr, FALSE);
            }
        }
    }

    void Backplate::ShowToast(const std::wstring& text)
    {
        m_toastText = text;
        m_toastExpireMs = Util::NowMs() + kToastDurationMs;
        RequestAnimationFrame();
        if (m_window != nullptr)
        {
            InvalidateRect(m_window, nullptr, FALSE);
        }
    }

    bool Backplate::CopyTextToClipboard(const std::wstring& text)
    {
        if (m_window == nullptr || !OpenClipboard(m_window))
        {
            return false;
        }
        bool ok = false;
        if (EmptyClipboard())
        {
            const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes))
            {
                if (void* dst = GlobalLock(h))
                {
                    std::memcpy(dst, text.c_str(), bytes);
                    GlobalUnlock(h);
                    ok = (SetClipboardData(CF_UNICODETEXT, h) != nullptr);
                }
                if (!ok)
                {
                    GlobalFree(h); // ownership only transfers to the clipboard on success
                }
            }
        }
        CloseClipboard();
        return ok;
    }

    void Backplate::AdvanceHoverToast(unsigned long long nowMs)
    {
        bool needAnim = false;

        // Tooltip dwell: once elapsed, flip m_tipShown so the next render (this
        // tick's Render, since animation is active) paints it. Keep re-arming
        // the animation until then.
        if (m_hoverWnd != nullptr && !m_hoverTip.empty() && !m_tipShown)
        {
            if (nowMs - m_hoverSinceMs >= kTooltipDwellMs)
            {
                m_tipShown = true;
                m_tipAnchor = m_hoverPt;
            }
            else
            {
                needAnim = true;
            }
        }

        // Toast expiry: clear it (one more render this tick erases it).
        if (!m_toastText.empty())
        {
            if (nowMs >= m_toastExpireMs)
            {
                m_toastText.clear();
            }
            else
            {
                needAnim = true;
            }
        }

        if (needAnim)
        {
            RequestAnimationFrame();
        }
    }

    bool Backplate::HasActiveOverlay(OverlayLayer layer) const
    {
        for (const auto& child : m_childrenOrdered)
        {
            if (child && child->HasActiveOverlayInTree(layer))
            {
                return true;
            }
        }
        return false;
    }

    bool Backplate::RouteOverlayInput(const InputEvent& event, OverlayLayer layer)
    {
        for (auto it = m_childrenOrdered.rbegin(); it != m_childrenOrdered.rend(); ++it)
        {
            if (*it && (*it)->RouteOverlayInput(event, layer))
            {
                return true;
            }
        }
        return false;
    }

    void Backplate::RenderOverlayLayer(ID2D1RenderTarget* target, OverlayLayer layer)
    {
        for (const auto& child : m_childrenOrdered)
        {
            if (child)
            {
                child->RenderOverlayTree(target, layer);
            }
        }
    }

    void Backplate::DrawHoverAndToast(
        ID2D1RenderTarget* target,
        bool drawHover,
        bool drawToast)
    {
        if (target == nullptr)
        {
            return;
        }
        const bool hasHover =
            drawHover &&
            m_tipShown &&
            !m_hoverTip.empty();
        const bool hasToast =
            drawToast &&
            !m_toastText.empty();
        if (!hasHover && !hasToast)
        {
            return;
        }

        IDWriteFactory* dwrite = Core::DWriteFactory();
        if (dwrite == nullptr)
        {
            return;
        }
        if (!m_tipFormat)
        {
            (void)dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &m_tipFormat);
            if (!m_tipFormat)
            {
                return;
            }
            (void)m_tipFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        const float clientW = static_cast<float>(m_size.width);
        const float clientH = static_cast<float>(m_size.height);
        constexpr float padX = 9.0f;
        constexpr float padY = 5.0f;

        auto drawBox = [&](const std::wstring& text, float boxLeft, float boxTop,
                           bool clampBelowRightOfCursor, const D2D1_COLOR_F& bg,
                           const D2D1_COLOR_F& border, const D2D1_COLOR_F& fg)
        {
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                m_tipFormat.Get(), 100000.0f, 100000.0f, &layout)) || !layout)
            {
                return;
            }
            DWRITE_TEXT_METRICS m {};
            if (FAILED(layout->GetMetrics(&m)))
            {
                return;
            }
            const float boxW = m.width + padX * 2.0f;
            const float boxH = m.height + padY * 2.0f;
            if (clampBelowRightOfCursor)
            {
                // Keep the whole box on-screen: nudge left/up when it would
                // overflow the right/bottom edge.
                if (boxLeft + boxW > clientW - 2.0f) boxLeft = clientW - 2.0f - boxW;
                if (boxTop + boxH > clientH - 2.0f) boxTop = m_tipAnchor.y - 8.0f - boxH;
            }
            if (boxLeft < 2.0f) boxLeft = 2.0f;
            if (boxTop < 2.0f) boxTop = 2.0f;

            const D2D1_ROUNDED_RECT rr {
                D2D1::RectF(boxLeft, boxTop, boxLeft + boxW, boxTop + boxH), 4.0f, 4.0f };
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
            if (SUCCEEDED(target->CreateSolidColorBrush(bg, &brush)))
            {
                target->FillRoundedRectangle(rr, brush.Get());
            }
            if (SUCCEEDED(target->CreateSolidColorBrush(border, &brush)))
            {
                target->DrawRoundedRectangle(rr, brush.Get(), 1.0f);
            }
            if (SUCCEEDED(target->CreateSolidColorBrush(fg, &brush)))
            {
                target->DrawTextLayout(D2D1::Point2F(boxLeft + padX, boxTop + padY),
                    layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        };

        if (hasHover)
        {
            drawBox(m_hoverTip, static_cast<float>(m_tipAnchor.x) + 12.0f,
                    static_cast<float>(m_tipAnchor.y) + 20.0f, true,
                    D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.97f),
                    D2D1::ColorF(0.42f, 0.44f, 0.50f, 1.0f),
                    D2D1::ColorF(0.92f, 0.92f, 0.95f, 1.0f));
        }
        if (hasToast)
        {
            // Centered near the bottom of the window.
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwrite->CreateTextLayout(m_toastText.c_str(),
                static_cast<UINT32>(m_toastText.size()), m_tipFormat.Get(),
                100000.0f, 100000.0f, &layout)) && layout)
            {
                DWRITE_TEXT_METRICS m {};
                if (SUCCEEDED(layout->GetMetrics(&m)))
                {
                    const float boxW = m.width + padX * 2.0f;
                    const float left = (clientW - boxW) * 0.5f;
                    const float top = clientH - (m.height + padY * 2.0f) - 24.0f;
                    drawBox(m_toastText, left, top, false,
                            D2D1::ColorF(0.16f, 0.34f, 0.58f, 0.97f),
                            D2D1::ColorF(0.30f, 0.55f, 0.85f, 1.0f),
                            D2D1::ColorF(0.97f, 0.98f, 1.0f, 1.0f));
                }
            }
        }
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
                // At WM_NCCREATE the window is not fully created yet, so defer render target creation
                // Created in WM_CREATE or on the first WM_SIZE
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
            if (m_resizeResourcesPending)
            {
                m_resizeResourcesPending = false;
                RECT rc {};
                if (m_window != nullptr)
                {
                    GetClientRect(m_window, &rc);
                    Resize(static_cast<UINT>(rc.right - rc.left), static_cast<UINT>(rc.bottom - rc.top));
                }
            }
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

                for (const auto& child : m_childrenOrdered)
                {
                    if (child)
                    {
                        Size ms = child->MinSize();
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
            // Create render target after the window is fully created
            EnsureRenderTarget();
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
            NoteRenderTrigger(RenderTrigger::Paint);
            Render();
            EndPaint(m_window, &ps);
            result = 0;
            return true;
        }

        case Backplate::WM_FD2D_BROADCAST:
        {
            auto* bm = reinterpret_cast<Backplate::BroadcastMessage*>(lParam);
            if (bm != nullptr)
            {
                for (const auto& child : m_childrenOrdered)
                {
                    if (child)
                    {
                        const CommandEvent event { bm->message, bm->wParam, bm->lParam };
                        (void)child->OnCommandEvent(event);
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
                // Avoid synchronous placement persistence during interactive resize.
                // WM_EXITSIZEMOVE already performs a single final flush.
                if (m_inSizeMove)
                {
                    if (m_window != nullptr)
                    {
                        (void)SetTimer(m_window, m_placeAutosaveTimerId, 200, nullptr);
                    }
                    result = 0;
                    return true;
                }

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
                m_placeAutosaveTimerId = 0;
            }
            // HWND is about to become invalid; clear before any late Invalidate/Render.
            m_window = nullptr;
            PostQuitMessage(0);
            result = 0;
            return true;
        }

        default:
            break;
        }

        const InputEventType inputType = ToInputEventType(message);
        const bool isInputMessage = (inputType != InputEventType::None);
        const bool isMouseMessage = IsRoutedMouseMessage(message);

        if (isInputMessage)
        {
            InputEvent inputEvent {};
            inputEvent.type = inputType;
            inputEvent.button = ToMouseButton(message, wParam);

            inputEvent.modifiers.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            inputEvent.modifiers.control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            inputEvent.modifiers.leftButton = (wParam & MK_LBUTTON) != 0;
            inputEvent.modifiers.rightButton = (wParam & MK_RBUTTON) != 0;
            inputEvent.modifiers.middleButton = (wParam & MK_MBUTTON) != 0;
            inputEvent.modifiers.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

            if (isMouseMessage && m_window != nullptr && message != WM_CAPTURECHANGED)
            {
                POINT ptClient { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
                {
                    ScreenToClient(m_window, &ptClient);
                }
                else if (GetCapture() == m_window)
                {
                    POINT cursorPos {};
                    GetCursorPos(&cursorPos);
                    ScreenToClient(m_window, &cursorPos);
                    ptClient = cursorPos;
                }

                inputEvent.point = ptClient;
                inputEvent.hasPoint = true;
            }

            // Hover-tooltip + toast bookkeeping, independent of the child
            // input routing below. A move re-arms the dwell over the control
            // under the cursor; a leave/press/scroll dismisses any tooltip.
            if (inputType == InputEventType::MouseMove && inputEvent.hasPoint)
            {
                if (!m_mouseTracking && m_window != nullptr)
                {
                    TRACKMOUSEEVENT tme { sizeof(TRACKMOUSEEVENT), TME_LEAVE, m_window, 0 };
                    m_mouseTracking = (TrackMouseEvent(&tme) != FALSE);
                }
                UpdateHoverTarget(inputEvent.point);
            }
            else if (inputType == InputEventType::MouseLeave)
            {
                m_mouseTracking = false;
                ClearHoverTooltip();
            }
            else if (inputType == InputEventType::MouseDown ||
                     inputType == InputEventType::MouseWheel ||
                     inputType == InputEventType::MouseHWheel)
            {
                ClearHoverTooltip();
            }

            if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
            {
                inputEvent.wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
            }
            if (inputType == InputEventType::KeyDown ||
                inputType == InputEventType::KeyUp ||
                inputType == InputEventType::Char ||
                inputType == InputEventType::SystemChar ||
                inputType == InputEventType::DeadChar ||
                inputType == InputEventType::SystemDeadChar ||
                inputType == InputEventType::UniChar)
            {
                inputEvent.keyCode = static_cast<UINT>(wParam);
                inputEvent.repeatCount = static_cast<UINT>(lParam & 0xFFFF);
                inputEvent.scanCode = static_cast<UINT>((lParam >> 16) & 0xFF);
                inputEvent.isExtendedKey = (lParam & (1 << 24)) != 0;
                inputEvent.wasDown = (lParam & (1 << 30)) != 0;
                inputEvent.isKeyUpTransition = (lParam & (1u << 31)) != 0;
                inputEvent.isSystemKey = (message == WM_SYSKEYDOWN ||
                    message == WM_SYSKEYUP ||
                    message == WM_SYSCHAR ||
                    message == WM_SYSDEADCHAR);
            }

            // Overlay input follows the reverse of paint priority. An active
            // modal owns the entire input surface, including its outer margin.
            const bool modalActive =
                HasActiveOverlay(OverlayLayer::Modal);
            if (RouteOverlayInput(inputEvent, OverlayLayer::Modal) ||
                (modalActive && inputType != InputEventType::None))
            {
                ClearHoverTooltip();
                result = 0;
                return true;
            }
            if (RouteOverlayInput(inputEvent, OverlayLayer::Popup) ||
                RouteOverlayInput(inputEvent, OverlayLayer::Inspector) ||
                RouteOverlayInput(inputEvent, OverlayLayer::Chrome))
            {
                ClearHoverTooltip();
                result = 0;
                return true;
            }

            // Route keyboard input to the focused Wnd first (if any). A
            // focused control that declines the key does NOT swallow it:
            // fall through to the tree broadcast below so application-wide
            // shortcuts keep working while e.g. a checkbox or button holds
            // focus from the last click.
            if (!isMouseMessage &&
                (inputType == InputEventType::KeyDown ||
                    inputType == InputEventType::KeyUp ||
                    inputType == InputEventType::Char ||
                    inputType == InputEventType::SystemChar ||
                    inputType == InputEventType::DeadChar ||
                    inputType == InputEventType::SystemDeadChar ||
                    inputType == InputEventType::UniChar) &&
                m_focusedWnd != nullptr)
            {
                if (m_focusedWnd->OnInputEvent(inputEvent))
                {
                    result = 0;
                    return true;
                }
            }

            if (inputEvent.hasPoint && inputType == InputEventType::MouseDown)
            {
                Wnd* target = FindTargetWnd(inputEvent.point);
                if (target != nullptr)
                {
                    target->RequestFocus();
                }
            }

            // Right-click on a control that opts into TryGetCopyText (path
            // labels) copies its text + shows a confirmation toast, ahead of
            // the broadcast that would otherwise open a context menu.
            if (inputEvent.hasPoint &&
                inputType == InputEventType::MouseUp &&
                inputEvent.button == MouseButton::Right)
            {
                if (Wnd* hit = HitTestTopLevel(inputEvent.point))
                {
                    std::wstring copyText;
                    if (hit->TryGetCopyText(copyText) && !copyText.empty())
                    {
                        if (CopyTextToClipboard(copyText))
                        {
                            ShowToast(L"Path copied to clipboard");
                        }
                        result = 0;
                        return true;
                    }
                }
            }

            if (inputEvent.hasPoint &&
                inputType == InputEventType::MouseUp &&
                inputEvent.button == MouseButton::Right)
            {
                Wnd* target = FindTargetWnd(inputEvent.point);
                if (target != nullptr && target->OnInputEvent(inputEvent))
                {
                    result = 0;
                    return true;
                }
            }

            for (auto it = m_childrenOrdered.rbegin(); it != m_childrenOrdered.rend(); ++it)
            {
                if (*it && (*it)->OnInputEvent(inputEvent))
                {
                    result = 0;
                    return true;
                }
            }

            // Escape is an application close fallback, not a global preemptive
            // shortcut: popups and modals above get first chance to consume it.
            if (inputType == InputEventType::KeyDown &&
                inputEvent.keyCode == VK_ESCAPE &&
                !inputEvent.wasDown &&
                m_window != nullptr)
            {
                PostMessageW(m_window, WM_CLOSE, 0, 0);
                result = 0;
                return true;
            }
            return false;
        }

        const CommandEvent commandEvent { message, wParam, lParam };
        if (m_focusedWnd != nullptr)
        {
            if (m_focusedWnd->OnCommandEvent(commandEvent))
            {
                result = 0;
                return true;
            }
            return false;
        }

        bool handledCommand = false;
        for (const auto& child : m_childrenOrdered)
        {
            if (child && child->OnCommandEvent(commandEvent))
            {
                handledCommand = true;
            }
        }
        if (handledCommand)
        {
            result = 0;
            return true;
        }
        return false;
    }

    void Backplate::SetFocusedWnd(Wnd* wnd)
    {
        if (m_focusedWnd == wnd)
        {
            return;
        }
        m_focusedWnd = wnd;
        UpdateTitleBarInfo();
        if (m_window != nullptr)
        {
            Render();
        }
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
        wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wcex.lpfnWndProc = Backplate::WndProc;
        wcex.cbClsExtra = 0;
        wcex.cbWndExtra = 0;
        wcex.hInstance = hInstance;
        wcex.hIcon = options.iconLarge ? options.iconLarge : LoadIcon(nullptr, IDI_APPLICATION);
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        // No GDI background brush; the swapchain is the only surface we want presented.
        wcex.hbrBackground = nullptr;
        wcex.lpszMenuName = nullptr;
        wcex.lpszClassName = options.className;
        wcex.hIconSm = options.iconSmall ? options.iconSmall : LoadIcon(nullptr, IDI_APPLICATION);

        if (RegisterClassExW(&wcex) == 0)
        {
            DWORD error = GetLastError();
            // If the class is already registered, treat it as success
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

        m_rendererId = (opts.rendererId != nullptr) ? opts.rendererId : L"";

        HWND window = CreateWindowExW(
            exStyle,
            opts.className,
            opts.title,
            style,
            opts.x,
            opts.y,
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

        if (opts.iconLarge)
        {
            SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(opts.iconLarge));
        }
        if (opts.iconSmall)
        {
            SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(opts.iconSmall));
        }

        RECT rc {};
        GetClientRect(window, &rc);
        m_size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

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
            HRESULT hr = CreateRenderTargetD2D();
            if (SUCCEEDED(hr))
            {
                InvalidateGraphics(GraphicsInvalidationReason::TargetRecreated, true, true, false);
            }
            return hr;
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
        DiscardDeviceResources();
        InvalidateGraphics(GraphicsInvalidationReason::RendererFallback, true, true, true);

        HRESULT hr = CreateRenderTargetD2D();
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
        InvalidateGraphics(GraphicsInvalidationReason::TargetRecreated, true, true, false);

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
        m_offscreenBitmap.Reset();
        m_offscreenRTV.Reset();
        m_offscreenTexture.Reset();
        m_offscreenD2DTarget.Reset();
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
        m_offscreenRT.Reset();
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

        if (m_inSizeMove)
        {
            // Keep the primary render surface in sync with the live client size
            // so layout/visual coordinates stay correct while dragging.
            // Only defer off-screen resource recreation to WM_EXITSIZEMOVE.
            m_resizeResourcesPending = true;
            m_offscreenResizePending = true;
            m_layoutDirty = true;

            if (m_hwndRenderTarget)
            {
                (void)m_hwndRenderTarget->Resize(m_size);
            }
            else if (m_swapChain && m_d2dContext)
            {
                m_d2dTargetBitmap.Reset();
                (void)m_d2dContext->SetTarget(nullptr);
                m_rtv.Reset();

                const HRESULT hrResize = m_swapChain->ResizeBuffers(0, m_size.width, m_size.height, DXGI_FORMAT_UNKNOWN, 0);
                if (HandleDeviceLostHr(hrResize, "ResizeBuffers(inSizeMove)"))
                {
                    // Device discarded; recover on the next frame.
                }
                else if (SUCCEEDED(hrResize))
                {
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
                else
                {
                    FD2D_LOG_INFO(
                        "[Graphics] ResizeBuffers(inSizeMove) failed hr=0x{:08X}",
                        static_cast<unsigned>(hrResize));
                }
            }

            if (m_window != nullptr)
            {
                InvalidateRect(m_window, nullptr, FALSE);
            }
            return;
        }

        if (m_hwndRenderTarget)
        {
            (void)m_hwndRenderTarget->Resize(m_size);
            m_offscreenRT.Reset();
            m_offscreenResizePending = false;
        }
        else if (m_swapChain && m_d2dContext)
        {
            m_d2dTargetBitmap.Reset();
            (void)m_d2dContext->SetTarget(nullptr);
            m_rtv.Reset();
            // Reset all off-screen buffers so they get recreated with new size
            m_offscreenBitmap.Reset();
            m_offscreenTexture.Reset();
            m_offscreenRTV.Reset();
            m_offscreenD2DTarget.Reset();
            m_offscreenResizePending = false;

            // Resize swap chain buffers (same device resources — do NOT bump content generations)
            const HRESULT hrResize = m_swapChain->ResizeBuffers(0, m_size.width, m_size.height, DXGI_FORMAT_UNKNOWN, 0);
            if (HandleDeviceLostHr(hrResize, "ResizeBuffers"))
            {
                m_layoutDirty = true;
                if (m_window != nullptr && IsWindowVisible(m_window))
                {
                    // HandleDeviceLostHr already scheduled a frame.
                }
                return;
            }
            else if (FAILED(hrResize))
            {
                FD2D_LOG_INFO(
                    "[Graphics] ResizeBuffers failed hr=0x{:08X}",
                    static_cast<unsigned>(hrResize));
            }
            else
            {
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
        }

        // Let the normal layout pass handle child measure/arrange once (avoids duplicate work per WM_SIZE).
        m_layoutDirty = true;

        // Skip rendering if the window is not yet visible — the first visible render
        // will pick up the new size via m_layoutDirty. This avoids a DXGI Present()
        // VSync stall (~16 ms per frame) that SetWindowPlacement triggers before Show().
        if (m_window != nullptr && IsWindowVisible(m_window))
        {
            Render();
        }
    }

    void Backplate::SetClearColor(const D2D1_COLOR_F& color)
    {
        m_clearColor = color;
        // Only trigger an immediate render if the window is already visible.
        // Before Show() is called, Render() would be the very first D3D/D2D
        // rendering operation and triggers GPU driver cold-start (shader
        // compilation, pipeline state caching) — typically 150–200 ms.
        // Once the window is visible, the next WM_PAINT will pick up the
        // new clear color anyway, so an immediate Render() is only needed
        // to prevent a momentary flash when the user changes the color live.
        if (m_window != nullptr && IsWindowVisible(m_window))
        {
            Render();
        }
    }

    bool Backplate::ClearRectD3D(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color)
    {
        ID3D11RenderTargetView* const clearTarget = (m_activeD3DRenderTarget != nullptr)
            ? m_activeD3DRenderTarget
            : m_rtv.Get();
        if (m_rendererId == L"d2d_hwndrt" || !m_d3dContext || clearTarget == nullptr)
        {
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> ctx1 {};
        if (FAILED(m_d3dContext.As(&ctx1)) || !ctx1)
        {
            return false;
        }

        D2D1_SIZE_U cs = m_renderSurfaceSize;
        if (cs.width == 0 || cs.height == 0)
        {
            cs = ClientSize();
        }
        if (cs.width == 0 || cs.height == 0)
        {
            return false;
        }

        const D2D1_SIZE_F scale = m_logicalToRenderScale;
        const D2D1_RECT_F mappedRect
        {
            rect.left * scale.width,
            rect.top * scale.height,
            rect.right * scale.width,
            rect.bottom * scale.height
        };

        D3D11_RECT r {};
        r.left = static_cast<LONG>(std::floor(mappedRect.left));
        r.top = static_cast<LONG>(std::floor(mappedRect.top));
        r.right = static_cast<LONG>(std::ceil(mappedRect.right));
        r.bottom = static_cast<LONG>(std::ceil(mappedRect.bottom));

        r.left = (std::max)(0L, (std::min)(r.left, static_cast<LONG>(cs.width)));
        r.top = (std::max)(0L, (std::min)(r.top, static_cast<LONG>(cs.height)));
        r.right = (std::max)(0L, (std::min)(r.right, static_cast<LONG>(cs.width)));
        r.bottom = (std::max)(0L, (std::min)(r.bottom, static_cast<LONG>(cs.height)));

        if (r.left >= r.right || r.top >= r.bottom)
        {
            return false;
        }

        const float c[4] = { color.r, color.g, color.b, color.a };
        ctx1->ClearView(clearTarget, c, &r, 1);
        return true;
    }

    void Backplate::RequestLayout()
    {
        m_layoutDirty = true;
        if (m_window != nullptr)
        {
            if (m_inSizeMove || m_isRendering)
            {
                InvalidateRect(m_window, nullptr, FALSE);
            }
            else
            {
                Render();
            }
        }
    }

    HRESULT Backplate::ReadComposedPixels(
        const D2D1_RECT_F& logicalRect,
        std::vector<std::uint8_t>& pixels,
        UINT& width,
        UINT& height,
        UINT& stride)
    {
        pixels.clear();
        width = 0;
        height = 0;
        stride = 0;

        if (!m_d3dDevice || !m_d3dContext)
        {
            return E_NOINTERFACE;
        }
        if (!m_useOffscreenBuffer ||
            m_inSizeMove ||
            !m_offscreenTexture)
        {
            return HRESULT_FROM_WIN32(ERROR_NOT_READY);
        }

        D3D11_TEXTURE2D_DESC sourceDesc {};
        m_offscreenTexture->GetDesc(&sourceDesc);
        if (sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            sourceDesc.SampleDesc.Count != 1)
        {
            return DXGI_ERROR_UNSUPPORTED;
        }

        const float scaleX = m_logicalToRenderScale.width;
        const float scaleY = m_logicalToRenderScale.height;
        const LONG left = (std::max)(
            0L,
            (std::min)(
                static_cast<LONG>(sourceDesc.Width),
                static_cast<LONG>(
                    std::floor(logicalRect.left * scaleX))));
        const LONG top = (std::max)(
            0L,
            (std::min)(
                static_cast<LONG>(sourceDesc.Height),
                static_cast<LONG>(
                    std::floor(logicalRect.top * scaleY))));
        const LONG right = (std::max)(
            0L,
            (std::min)(
                static_cast<LONG>(sourceDesc.Width),
                static_cast<LONG>(
                    std::ceil(logicalRect.right * scaleX))));
        const LONG bottom = (std::max)(
            0L,
            (std::min)(
                static_cast<LONG>(sourceDesc.Height),
                static_cast<LONG>(
                    std::ceil(logicalRect.bottom * scaleY))));
        if (right <= left || bottom <= top)
        {
            return E_INVALIDARG;
        }

        width = static_cast<UINT>(right - left);
        height = static_cast<UINT>(bottom - top);
        stride = width * 4;

        D3D11_TEXTURE2D_DESC stagingDesc {};
        stagingDesc.Width = width;
        stagingDesc.Height = height;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = sourceDesc.Format;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        HRESULT result = m_d3dDevice->CreateTexture2D(
            &stagingDesc,
            nullptr,
            &staging);
        if (FAILED(result))
        {
            return result;
        }

        D3D11_BOX sourceBox {};
        sourceBox.left = static_cast<UINT>(left);
        sourceBox.top = static_cast<UINT>(top);
        sourceBox.front = 0;
        sourceBox.right = static_cast<UINT>(right);
        sourceBox.bottom = static_cast<UINT>(bottom);
        sourceBox.back = 1;
        m_d3dContext->CopySubresourceRegion(
            staging.Get(),
            0,
            0,
            0,
            0,
            m_offscreenTexture.Get(),
            0,
            &sourceBox);

        D3D11_MAPPED_SUBRESOURCE mapped {};
        result = m_d3dContext->Map(
            staging.Get(),
            0,
            D3D11_MAP_READ,
            0,
            &mapped);
        if (FAILED(result))
        {
            return result;
        }

        pixels.resize(
            static_cast<std::size_t>(stride) * height);
        for (UINT row = 0; row < height; ++row)
        {
            std::memcpy(
                pixels.data() +
                    static_cast<std::size_t>(row) * stride,
                static_cast<const std::uint8_t*>(mapped.pData) +
                    static_cast<std::size_t>(row) * mapped.RowPitch,
                stride);
        }
        m_d3dContext->Unmap(staging.Get(), 0);
        return S_OK;
    }

    void Backplate::Render()
    {
        // Prevent recursive rendering (e.g., layout changes during OnRender triggering Invalidate)
        if (m_isRendering)
        {
            m_renderRequested = true;
            return;
        }

        if (!m_window || !IsWindow(m_window))
        {
            return;
        }

        // Always clear m_isRendering, including early returns (e.g. D2DERR_RECREATE_TARGET).
        struct RenderingGuard
        {
            Backplate& self;
            explicit RenderingGuard(Backplate& s)
                : self(s)
            {
                self.m_isRendering = true;
            }
            ~RenderingGuard()
            {
                self.m_isRendering = false;
            }
        } renderingGuard(*this);

        // Diagnostic: snapshot+reset what triggered this call and whether an async
        // decode-completion redraw was already pending, for the [FPS] summary below.
        const RenderTrigger renderTrigger = m_pendingRenderTrigger;
        m_pendingRenderTrigger = RenderTrigger::Other;
        const bool asyncPendingAtStart = m_asyncRedrawPending.load();

        // Render loop: continue until no more render requests
        int renderLoopIterations = 0;
        const auto t_renderLoop = std::chrono::steady_clock::now();
        do
        {
            ++renderLoopIterations;
            m_renderRequested = false;
            m_renderSurfaceSize = m_size;
            m_logicalToRenderScale = D2D1::SizeF(1.0f, 1.0f);

            const auto updateRenderMapping = [this](UINT surfaceW, UINT surfaceH)
            {
                if (surfaceW == 0 || surfaceH == 0)
                {
                    m_renderSurfaceSize = m_size;
                    m_logicalToRenderScale = D2D1::SizeF(1.0f, 1.0f);
                    return;
                }

                m_renderSurfaceSize = D2D1::SizeU(surfaceW, surfaceH);
                if (m_size.width == 0 || m_size.height == 0)
                {
                    m_logicalToRenderScale = D2D1::SizeF(1.0f, 1.0f);
                    return;
                }

                // While resize resources are deferred, keep layout in logical client size
                // but render into the previous surface by applying a coordinate scale.
                if (m_inSizeMove &&
                    m_resizeResourcesPending &&
                    (surfaceW != m_size.width || surfaceH != m_size.height))
                {
                    m_logicalToRenderScale = D2D1::SizeF(
                        static_cast<float>(surfaceW) / static_cast<float>(m_size.width),
                        static_cast<float>(surfaceH) / static_cast<float>(m_size.height));
                }
                else
                {
                    m_logicalToRenderScale = D2D1::SizeF(1.0f, 1.0f);
                }
            };

            if (m_layoutDirty)
            {
                Layout();
            }
            const auto t_ensure = std::chrono::steady_clock::now();
            HRESULT hrEnsure = EnsureRenderTarget();
            {
                const auto ensureMs = FD2D_ELAPSED_MS(t_ensure);
                if (ensureMs > 30)
                {
                    FD2D_LOG_INFO("[Render] EnsureRenderTarget took {}ms", ensureMs);
                }
            }
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
            ID2D1RenderTarget* renderTarget = m_hwndRenderTarget.Get();

            // During live resize, avoid off-screen path to reduce realloc/copy overhead.
            const bool useOffscreenThisFrame = m_useOffscreenBuffer && !m_inSizeMove;
            if (useOffscreenThisFrame)
            {
                if (!m_offscreenRT)
                {
                    const D2D1_SIZE_F size = m_hwndRenderTarget->GetSize();
                    const D2D1_SIZE_U pixelSize = D2D1::SizeU(
                        static_cast<UINT32>(size.width),
                        static_cast<UINT32>(size.height));
                    
                    (void)m_hwndRenderTarget->CreateCompatibleRenderTarget(
                        &size,
                        &pixelSize,
                        nullptr,
                        D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE,
                        &m_offscreenRT);
                }

                if (m_offscreenRT)
                {
                    renderTarget = m_offscreenRT.Get();
                }
            }

            const D2D1_SIZE_U rtPx = renderTarget->GetPixelSize();
            updateRenderMapping(rtPx.width, rtPx.height);
            const D2D1_MATRIX_3X2_F logicalToRender = D2D1::Matrix3x2F::Scale(
                m_logicalToRenderScale.width,
                m_logicalToRenderScale.height);

            renderTarget->BeginDraw();
            renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
            // Dark neutral gray with a *tiny* blue bias (low saturation)
            renderTarget->Clear(m_clearColor);
            renderTarget->SetTransform(logicalToRender);

            for (const auto& child : m_childrenOrdered)
            {
                if (child)
                {
                    child->OnRender(renderTarget);
                }
            }
            RenderOverlayLayer(renderTarget, OverlayLayer::Chrome);
            RenderOverlayLayer(renderTarget, OverlayLayer::Inspector);
            RenderOverlayLayer(renderTarget, OverlayLayer::Popup);
            const bool transientOverlay =
                HasActiveOverlay(OverlayLayer::Inspector) ||
                HasActiveOverlay(OverlayLayer::Popup) ||
                HasActiveOverlay(OverlayLayer::Modal);
            DrawHoverAndToast(renderTarget, !transientOverlay, false);
            RenderOverlayLayer(renderTarget, OverlayLayer::Modal);
            DrawHoverAndToast(renderTarget, false, true);

            HRESULT hr = renderTarget->EndDraw();
            if (hr == D2DERR_RECREATE_TARGET)
            {
                m_hwndRenderTarget.Reset();
                m_offscreenRT.Reset();
                InvalidateGraphics(GraphicsInvalidationReason::TargetRecreated, false, true, false);
                ScheduleNextFrame();
                return;
            }

            // Copy offscreen buffer to window if double-buffering is active
            if (useOffscreenThisFrame && m_offscreenRT)
            {
                Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
                if (SUCCEEDED(m_offscreenRT->GetBitmap(&bitmap)) && bitmap)
                {
                    m_hwndRenderTarget->BeginDraw();
                    m_hwndRenderTarget->Clear(m_clearColor);
                    
                    const D2D1_SIZE_F size = m_hwndRenderTarget->GetSize();
                    const D2D1_RECT_F destRect = D2D1::RectF(0.0f, 0.0f, size.width, size.height);
                    
                    m_hwndRenderTarget->DrawBitmap(
                        bitmap.Get(),
                        destRect,
                        1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                        nullptr);
                    
                    hr = m_hwndRenderTarget->EndDraw();
                    if (hr == D2DERR_RECREATE_TARGET)
                    {
                        m_hwndRenderTarget.Reset();
                        m_offscreenRT.Reset();
                        InvalidateGraphics(GraphicsInvalidationReason::TargetRecreated, false, true, false);
                        ScheduleNextFrame();
                        return;
                    }
                }
            }
            
            // D2D-only path complete - continue to check if re-render needed
        }
        else
        {
        // Create D3D11 off-screen resources if enabled
        const bool useOffscreenThisFrame = m_useOffscreenBuffer && !m_inSizeMove;
        if (useOffscreenThisFrame && m_d3dDevice && m_size.width > 0 && m_size.height > 0)
        {
            if (!m_offscreenTexture || !m_offscreenRTV)
            {
                // Create off-screen texture
                D3D11_TEXTURE2D_DESC texDesc = {};
                texDesc.Width = m_size.width;
                texDesc.Height = m_size.height;
                texDesc.MipLevels = 1;
                texDesc.ArraySize = 1;
                texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                texDesc.SampleDesc.Count = 1;
                texDesc.SampleDesc.Quality = 0;
                texDesc.Usage = D3D11_USAGE_DEFAULT;
                texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                texDesc.CPUAccessFlags = 0;
                texDesc.MiscFlags = 0;

                if (SUCCEEDED(m_d3dDevice->CreateTexture2D(&texDesc, nullptr, &m_offscreenTexture)))
                {
                    (void)m_d3dDevice->CreateRenderTargetView(m_offscreenTexture.Get(), nullptr, &m_offscreenRTV);
                    
                    // Create D2D bitmap from offscreen texture
                    if (m_d2dContext && m_offscreenTexture)
                    {
                        Microsoft::WRL::ComPtr<IDXGISurface> surface;
                        if (SUCCEEDED(m_offscreenTexture.As(&surface)))
                        {
                            // Off-screen texture needs PREMULTIPLIED alpha (can be used as both target and source)
                            D2D1_BITMAP_PROPERTIES1 bp = {};
                            bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
                            bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
                            m_d2dContext->GetDpi(&bp.dpiX, &bp.dpiY);
                            bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
                            
                            (void)m_d2dContext->CreateBitmapFromDxgiSurface(
                                surface.Get(),
                                &bp,
                                &m_offscreenD2DTarget);
                        }
                    }
                }
            }
        }

        // Select D3D render target: offscreen or direct
        ID3D11RenderTargetView* d3dRenderTarget = m_rtv.Get();
        if (useOffscreenThisFrame && m_offscreenRTV)
        {
            d3dRenderTarget = m_offscreenRTV.Get();
        }

        UINT d3dSurfaceW = m_size.width;
        UINT d3dSurfaceH = m_size.height;
        if (d3dRenderTarget)
        {
            Microsoft::WRL::ComPtr<ID3D11Resource> rtResource {};
            d3dRenderTarget->GetResource(&rtResource);
            Microsoft::WRL::ComPtr<ID3D11Texture2D> rtTexture {};
            if (rtResource && SUCCEEDED(rtResource.As(&rtTexture)) && rtTexture)
            {
                D3D11_TEXTURE2D_DESC td {};
                rtTexture->GetDesc(&td);
                d3dSurfaceW = td.Width;
                d3dSurfaceH = td.Height;
            }
        }
        updateRenderMapping(d3dSurfaceW, d3dSurfaceH);

        // D3D pass (background + GPU images)
        if (m_d3dContext && d3dRenderTarget)
        {
            const float clearColor[4] = { m_clearColor.r, m_clearColor.g, m_clearColor.b, m_clearColor.a };
            m_d3dContext->OMSetRenderTargets(1, &d3dRenderTarget, nullptr);
            m_d3dContext->ClearRenderTargetView(d3dRenderTarget, clearColor);
            m_activeD3DRenderTarget = d3dRenderTarget;

            D3D11_VIEWPORT vp {};
            vp.TopLeftX = 0.0f;
            vp.TopLeftY = 0.0f;
            vp.Width = static_cast<float>(m_renderSurfaceSize.width);
            vp.Height = static_cast<float>(m_renderSurfaceSize.height);
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            m_d3dContext->RSSetViewports(1, &vp);

            const auto t_d3dPass = std::chrono::steady_clock::now();
            for (const auto& child : m_childrenOrdered)
            {
                if (child)
                {
                    child->OnRenderD3D(m_d3dContext.Get());
                }
            }
            {
                const auto d3dPassMs = FD2D_ELAPSED_MS(t_d3dPass);
                if (d3dPassMs > 30)
                {
                    FD2D_LOG_INFO("[Render] D3D OnRenderD3D pass took {}ms", d3dPassMs);
                }
            }

            // IMPORTANT: ensure we release the render target from the D3D OM stage
            // before letting D2D draw to it.
            ID3D11RenderTargetView* nullRTV[1] = { nullptr };
            m_d3dContext->OMSetRenderTargets(1, nullRTV, nullptr);
            m_activeD3DRenderTarget = nullptr;
        }

        // D2D pass (UI overlays)
        bool d2dOk = true;
        // Ensure we have a valid target for the UI pass.
        if (!m_d2dTargetBitmap || !m_rtv)
        {
            (void)RecreateSwapChainTargets();
        }

        // Select D2D render target: offscreen (shared with D3D) or direct
        ID2D1Image* d2dRenderTarget = m_d2dTargetBitmap.Get();
        if (useOffscreenThisFrame && m_offscreenD2DTarget)
        {
            // Use the D2D view of the offscreen texture (already has D3D content)
            d2dRenderTarget = m_offscreenD2DTarget.Get();
        }

        // We detach the target after each frame; ensure it is set for this draw.
        if (m_d2dContext && d2dRenderTarget)
        {
            m_d2dContext->SetTarget(d2dRenderTarget);
        }

        m_d2dContext->BeginDraw();
        m_d2dContext->SetTransform(D2D1::Matrix3x2F::Scale(
            m_logicalToRenderScale.width,
            m_logicalToRenderScale.height));

        for (const auto& child : m_childrenOrdered)
        {
            if (child)
            {
                child->OnRender(m_d2dContext.Get());
            }
        }
        RenderOverlayLayer(m_d2dContext.Get(), OverlayLayer::Chrome);
        RenderOverlayLayer(m_d2dContext.Get(), OverlayLayer::Inspector);
        RenderOverlayLayer(m_d2dContext.Get(), OverlayLayer::Popup);
        const bool transientOverlay =
            HasActiveOverlay(OverlayLayer::Inspector) ||
            HasActiveOverlay(OverlayLayer::Popup) ||
            HasActiveOverlay(OverlayLayer::Modal);
        DrawHoverAndToast(m_d2dContext.Get(), !transientOverlay, false);
        RenderOverlayLayer(m_d2dContext.Get(), OverlayLayer::Modal);
        DrawHoverAndToast(m_d2dContext.Get(), false, true);

        const auto t_endDraw = std::chrono::steady_clock::now();
        HRESULT hr = m_d2dContext->EndDraw();
        {
            const auto endDrawMs = FD2D_ELAPSED_MS(t_endDraw);
            if (endDrawMs > 30)
            {
                FD2D_LOG_INFO("[Render] D2D EndDraw (primary) took {}ms", endDrawMs);
            }
        }
        
        // Copy offscreen to swap chain backbuffer if double-buffering
        if (SUCCEEDED(hr) && useOffscreenThisFrame && m_offscreenD2DTarget && m_d2dTargetBitmap)
        {
            m_d2dContext->SetTarget(m_d2dTargetBitmap.Get());
            m_d2dContext->BeginDraw();
            m_d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
            
            // Draw the complete offscreen buffer (D3D + D2D) to the swap chain
            const D2D1_POINT_2F targetOffset = D2D1::Point2F(0.0f, 0.0f);
            
            m_d2dContext->DrawImage(
                m_offscreenD2DTarget.Get(),
                &targetOffset,
                nullptr,
                D2D1_INTERPOLATION_MODE_LINEAR,
                D2D1_COMPOSITE_MODE_SOURCE_COPY);
            
            const auto t_endDraw2 = std::chrono::steady_clock::now();
            hr = m_d2dContext->EndDraw();
            {
                const auto endDraw2Ms = FD2D_ELAPSED_MS(t_endDraw2);
                if (endDraw2Ms > 30)
                {
                    FD2D_LOG_INFO("[Render] D2D EndDraw (offscreen copy) took {}ms", endDraw2Ms);
                }
            }
        }
        if (FAILED(hr))
        {
            d2dOk = false;
            // Release the D2D target immediately so the swapchain backbuffer isn't held.
            DiscardD2DTargets();

            // Device lost -> full recreate next frame. Otherwise, just recreate targets.
            if (HandleDeviceLostHr(hr, "D2D EndDraw"))
            {
                return;
            }
        }

        // Detach target before Present for clean interop.
        if (m_d2dContext)
        {
            m_d2dContext->SetTarget(nullptr);
        }

        if (m_swapChain)
        {
            const auto t_present = std::chrono::steady_clock::now();
            const HRESULT hrPresent = m_swapChain->Present(1, 0);
            const auto presentMs = FD2D_ELAPSED_MS(t_present);
            if (presentMs > 30)
            {
                FD2D_LOG_INFO("[Render] SwapChain::Present(1,0) took {}ms", presentMs);
            }

            if (HandleDeviceLostHr(hrPresent, "SwapChain::Present"))
            {
                return;
            }
            else if (FAILED(hrPresent))
            {
                FD2D_LOG_INFO(
                    "[Graphics] SwapChain::Present failed hr=0x{:08X}",
                    static_cast<unsigned>(hrPresent));
            }
        }
        } // end else (D3D11 path)

        } while (m_renderRequested); // Render again if requested during this frame

        if (renderLoopIterations > 1)
        {
            const auto loopMs = FD2D_ELAPSED_MS(t_renderLoop);
            FD2D_LOG_INFO("[Render] do-while loop ran {} iterations in {}ms", renderLoopIterations, loopMs);
        }

        // Diagnostic: roll this frame into a once-per-second [FPS] summary so a sluggish
        // period (e.g. right after startup while async work is still completing) shows up
        // as objective fps/frame-time numbers, broken down by what triggered each frame
        // and whether an async decode-completion redraw was pending at the time.
        {
            const double frameMs = static_cast<double>(FD2D_ELAPSED_MS(t_renderLoop));
            const unsigned long long nowMs = Util::NowMs();
            if (m_fpsWindowStartMs == 0)
            {
                m_fpsWindowStartMs = nowMs;
            }
            ++m_fpsWindowFrames;
            m_fpsWindowTotalMs += frameMs;
            if (frameMs > m_fpsWindowMaxMs)
            {
                m_fpsWindowMaxMs = frameMs;
            }
            if (asyncPendingAtStart)
            {
                ++m_fpsWindowAsyncPendingFrames;
            }
            switch (renderTrigger)
            {
            case RenderTrigger::Tick:       ++m_fpsWindowTickFrames;       break;
            case RenderTrigger::Invalidate: ++m_fpsWindowInvalidateFrames; break;
            case RenderTrigger::Paint:      ++m_fpsWindowPaintFrames;      break;
            default:                        ++m_fpsWindowOtherFrames;     break;
            }

            const unsigned long long windowElapsedMs = nowMs - m_fpsWindowStartMs;
            if (windowElapsedMs >= 1000)
            {
                const double avgMs = m_fpsWindowTotalMs / (std::max)(1, m_fpsWindowFrames);
                const double fps = static_cast<double>(m_fpsWindowFrames) * 1000.0 /
                    static_cast<double>((std::max)(windowElapsedMs, 1ULL));
                FD2D_LOG_INFO(
                    "[FPS] {:.1f} fps  frames={} avg={:.1f}ms max={:.1f}ms  "
                    "trigger(tick={} invalidate={} paint={} other={})  asyncPending={}/{}",
                    fps, m_fpsWindowFrames, avgMs, m_fpsWindowMaxMs,
                    m_fpsWindowTickFrames, m_fpsWindowInvalidateFrames,
                    m_fpsWindowPaintFrames, m_fpsWindowOtherFrames,
                    m_fpsWindowAsyncPendingFrames, m_fpsWindowFrames);

                m_fpsWindowStartMs = nowMs;
                m_fpsWindowFrames = 0;
                m_fpsWindowTickFrames = 0;
                m_fpsWindowInvalidateFrames = 0;
                m_fpsWindowPaintFrames = 0;
                m_fpsWindowOtherFrames = 0;
                m_fpsWindowAsyncPendingFrames = 0;
                m_fpsWindowTotalMs = 0.0;
                m_fpsWindowMaxMs = 0.0;
            }
        }
    }

    void Backplate::Layout()
    {
        D2D1_SIZE_F size { static_cast<FLOAT>(m_size.width), static_cast<FLOAT>(m_size.height) };

        for (const auto& child : m_childrenOrdered)
        {
            if (child)
            {
                child->Measure({ size.width, size.height });
                child->Arrange({ 0.0f, 0.0f, size.width, size.height });
            }
        }

        m_layoutDirty = false;
    }

    void Backplate::Show(int nCmdShow)
    {
        if (m_window)
        {
            ShowWindow(m_window, nCmdShow);
            // Direct rendering for initial display
            Render();
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

        FD2D_TIMER_START(t_addwnd);

        m_children.emplace(wnd->Name(), wnd);
        m_childrenOrdered.push_back(wnd);
        wnd->OnAttached(*this);
        FD2D_LOG_STEP(t_addwnd, "[AddWnd] OnAttached");

        wnd->Measure({ static_cast<float>(m_size.width), static_cast<float>(m_size.height) });
        wnd->Arrange({ 0.0f, 0.0f, static_cast<float>(m_size.width), static_cast<float>(m_size.height) });
        FD2D_LOG_STEP(t_addwnd, "[AddWnd] Measure + Arrange");

        m_layoutDirty = true;

        if (m_window != nullptr)
        {
            Render();
            FD2D_LOG_STEP(t_addwnd, "[AddWnd] Render (first frame)");
        }

        return true;
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
        // Base Backplate does not impose application-specific title formatting.
    }
}


