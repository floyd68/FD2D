#pragma once

#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dxgi1_2.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <atomic>
#include <functional>
#include <vector>

#include "AsyncRedrawSignal.h"
#include "RenderStats.h"
#include "Wnd.h"

namespace FD2D
{
    // Thread-safe redraw handle for worker threads. Does not retain a raw Backplate*.
    // After Backplate destruction the token becomes a no-op.
    class AsyncRedrawToken
    {
    public:
        void RequestAsyncRedraw() const;

    private:
        friend class Backplate;

        struct ControlBlock
        {
            mutable std::mutex mutex;
            Backplate* backplate { nullptr };
        };

        explicit AsyncRedrawToken(std::weak_ptr<ControlBlock> control);
        std::weak_ptr<ControlBlock> m_control {};
    };

    enum class ChromeStyle
    {
        Standard,
        Borderless
    };

    struct WindowOptions
    {
        HINSTANCE instance { nullptr };
        const wchar_t* title { L"FD2D Window" };
        // Initial window position. Use CW_USEDEFAULT to let Windows decide.
        // When x == CW_USEDEFAULT, y is ignored by the system.
        int x { CW_USEDEFAULT };
        int y { 0 };
        UINT width { 960 };
        UINT height { 640 };
        ChromeStyle chrome { ChromeStyle::Standard };
        DWORD style { 0 };
        DWORD exStyle { 0 };
        const wchar_t* className { L"FD2DWindowClass" };
        HICON iconLarge { nullptr };
        HICON iconSmall { nullptr };
        // Renderer backend selection (optional).
        // - nullptr or L"d3d11_swapchain": D3D11 swapchain + D2D interop (default, fastest, supports GPU DDS)
        // - L"d2d_hwndrt": D2D-only ID2D1HwndRenderTarget (more compatible, no D3D pass, no GPU DDS)
        const wchar_t* rendererId { nullptr };
    };

    class Backplate
    {
    public:
        // Broadcast an application message to all top-level Wnds (bypasses focus-based routing).
        static constexpr UINT WM_FD2D_BROADCAST = WM_APP + 0x4D4; // 'FD4'

        struct BroadcastMessage
        {
            UINT message { 0 };
            WPARAM wParam { 0 };
            LPARAM lParam { 0 };
        };

        Backplate();
        explicit Backplate(const std::wstring& name);
        virtual ~Backplate();

        void SetName(const std::wstring& name);
        const std::wstring& Name() const;

        HRESULT Attach(HWND windowHandle);
        HRESULT CreateWindowed(const WindowOptions& options);
        int RunMessageLoop();
        HWND Window() const;

        HRESULT EnsureRenderTarget();
        void Resize(UINT width, UINT height);
        // Immediate render: lays out (if needed), renders and presents one
        // frame now, and clears any pending render request (RequestRender).
        // Use it only where a frame is needed right away: the startup warm-up
        // before the window is shown, before ReadComposedPixels(), the first
        // frame in Show(), and WM_PAINT. Everything else (Wnd::Invalidate(),
        // RequestLayout(), async redraws, animation ticks) requests a
        // coalesced render instead. A Render() call made while rendering adds
        // one more pass to the current frame.
        void Render();
        void Show(int nCmdShow);

        bool AddWnd(const std::shared_ptr<Wnd>& wnd);

        void SetFocusedWnd(Wnd* wnd);
        Wnd* FocusedWnd() const { return m_focusedWnd; }
        void ClearFocusIf(Wnd* wnd);

        ID2D1RenderTarget* RenderTarget() const;
        ID3D11Device* D3DDevice() const { return m_rendererId == L"d2d_hwndrt" ? nullptr : m_d3dDevice.Get(); }
        ID3D11DeviceContext* D3DContext() const { return m_rendererId == L"d2d_hwndrt" ? nullptr : m_d3dContext.Get(); }
        // Valid only while Backplate is dispatching the D3D render pass.
        // Shared presenters bind this explicitly because child renderers may
        // temporarily replace or unbind the output-merger target.
        ID3D11RenderTargetView* ActiveD3DRenderTarget() const
        {
            return m_activeD3DRenderTarget;
        }
        D2D1_SIZE_U ClientSize() const { return m_size; }
        D2D1_SIZE_U RenderSurfaceSize() const { return m_renderSurfaceSize; }
        D2D1_SIZE_F LogicalToRenderScale() const { return m_logicalToRenderScale; }
        // Reads a logical client-space rectangle from the last fully composed
        // offscreen frame (D3D + D2D) into tightly packed BGRA8 pixels. This is
        // independent of desktop occlusion. Call Render() first when a current
        // frame is required. Returns E_NOINTERFACE for the D2D-only backend.
        HRESULT ReadComposedPixels(
            const D2D1_RECT_F& logicalRect,
            std::vector<std::uint8_t>& pixels,
            UINT& width,
            UINT& height,
            UINT& stride);

        // Windowless render mode (for tests): no window, swap chain, Present or
        // hardware GPU. Creates a WARP D3D11 device (BGRA support) with a D2D
        // device/context on it and renders into an offscreen BGRA8 texture of
        // round(logical size * dpiScale) pixels. Layout uses the logical size;
        // LogicalToRenderScale() maps it to the surface, as on the windowed
        // path. Render() then runs the same layout, D3D pass and D2D pass, in
        // the same order, into that texture, and ReadComposedPixels() reads the
        // composed frame. Call it on a backplate without a window, before or
        // after adding children; calling it again re-creates the device stack
        // at the new size (new device and target generations). Nothing renders
        // implicitly in this mode (Wnd::Invalidate() only marks a render
        // pending; RenderIfPending() or Render() draws it): each Render() call
        // draws exactly one frame unless a re-entrant Render() request asks
        // for another.
        HRESULT InitializeWindowless(UINT logicalWidth, UINT logicalHeight, float dpiScale = 1.0f);
        bool IsWindowless() const { return m_windowless; }

        // Test hook: runs once per frame of the D3D backend (windowed or
        // windowless), after the D3D pass (every OnRenderD3D) and before the
        // D2D pass (every OnRender) of the same frame. ActiveD3DRenderTarget()
        // is null while it runs. A Render() request from inside it is deferred
        // like any re-entrant request. Pass an empty function to remove it.
        // The D2D-only backend has no D3D pass and never calls it.
        // The hook object persists across calls, so state in a mutable capture
        // carries from one frame to the next.
        void SetBetweenPassesHook(std::function<void()> hook);

        // Cross-thread redraw signaling without PostMessage:
        // worker thread calls RequestAsyncRedraw() -> signals event (coalesced)
        // UI thread waits on AsyncRedrawEvent() and calls ProcessAsyncRedraw().
        HANDLE AsyncRedrawEvent() const { return m_asyncRedrawSignal.Event(); }
        void RequestAsyncRedraw();
        void ProcessAsyncRedraw();

        // Prefer this over capturing raw Backplate* from worker threads.
        std::shared_ptr<AsyncRedrawToken> GetAsyncRedrawToken() const;

        // Monotonic graphics resource generations (device / target / renderer backend).
        // These, not GraphicsInvalidationReason, decide resource validity: target
        // for D2D resources, device for D3D resources and SRVs.
        GraphicsGeneration GetGraphicsGeneration() const { return m_graphicsGeneration; }

        // Per-device state shared by the controls of this backplate (for example
        // the Image quad pipeline), keyed by an address-unique key. Entries are
        // dropped whenever this backplate discards its device, so they never
        // outlive the device they were created on, and backplates never share or
        // evict each other's entries.
        std::shared_ptr<void> DeviceCacheEntry(const void* key) const;
        void SetDeviceCacheEntry(const void* key, std::shared_ptr<void> entry);

        // Animation scheduling (spinner / cross-fade): avoids busy WM_PAINT loops.
        void RequestAnimationFrame();
        bool HasActiveAnimation(unsigned long long nowMs) const;
        void ProcessAnimationTick(unsigned long long nowMs);

        // Force layout recalculation on next render (and request that render).
        void RequestLayout();
        // Runs a requested layout pass now, without rendering; returns true
        // when it did. Layout requests are coalesced with the next render, so
        // input that arrives before that render would hit-test the previous
        // arrangement: the backplate calls this before it routes an input
        // message or a file drag/drop. Code that needs arranged bounds right
        // after a change can call it too. No-op while rendering.
        bool LayoutIfPending();

        // Transient notification banner (e.g. "Path copied to clipboard"),
        // drawn over the UI near the bottom of the window and auto-dismissed
        // after a short delay. The Windows-native-feel confirmation for
        // in-app actions like the path-label right-click copy.
        void ShowToast(const std::wstring& text);

        // Copies UTF-16 text to the clipboard (owned by this window). Returns
        // false if the clipboard could not be opened.
        bool CopyTextToClipboard(const std::wstring& text);

        // Update window title bar with renderer information
        virtual void UpdateTitleBarInfo();

        // Called once right before the Backplate window begins destruction (WM_CLOSE/WM_DESTROY).
        // Use this to persist window placement/settings while the HWND is still valid.
        void SetOnBeforeDestroy(std::function<void(HWND)> handler);

        // Called after the window's placement likely changed (move/resize/maximize/restore),
        // debounced to avoid excessive INI writes.
        void SetOnWindowPlacementChanged(std::function<void(HWND)> handler);

        // Global clear/background color (applies to both D2D-only and D3D swapchain backends).
        // Default matches the previous hardcoded clear.
        void SetClearColor(const D2D1_COLOR_F& color);
        D2D1_COLOR_F ClearColor() const { return m_clearColor; }

        // Enable/disable off-screen double-buffering (reduces flicker, default: enabled)
        void SetUseOffscreenBuffer(bool enable) { m_useOffscreenBuffer = enable; }
        bool UseOffscreenBuffer() const { return m_useOffscreenBuffer; }

        // Check if currently rendering (to prevent recursive layout changes)
        bool IsRendering() const { return m_isRendering; }
        bool IsInSizeMove() const { return m_inSizeMove; }

        // Marks a batch of state changes (e.g. drag-hover overlay updates across
        // several children during a single OLE DragOver callback) whose owner
        // requests the frame when the batch ends. Invalidations are coalesced
        // anyway (Wnd::Invalidate() never renders synchronously), so this no
        // longer changes how an invalidation is handled; it tells nested
        // batching scopes (IsDeferringRender()) that an enclosing scope
        // requests the frame. Nestable; call EndDeferredRender() once per
        // BeginDeferredRender().
        void BeginDeferredRender() { ++m_deferRenderDepth; }
        void EndDeferredRender() { if (m_deferRenderDepth > 0) --m_deferRenderDepth; }
        bool IsDeferringRender() const { return m_deferRenderDepth > 0; }

        // Diagnostic-only: what caused the *next* Render() call. Lets the periodic
        // [FPS] summary log (see Render()) break frame counts/timing down by cause
        // (animation tick vs on-demand Invalidate/pan vs WM_PAINT) so slow periods can
        // be attributed to a specific trigger instead of just an aggregate frame time.
        enum class RenderTrigger { Other, Tick, Invalidate, Paint };
        void NoteRenderTrigger(RenderTrigger trigger) { m_pendingRenderTrigger = trigger; }

        // Coalesced render request: what Wnd::Invalidate(), RequestLayout(),
        // async redraws and animation ticks use. It never renders
        // synchronously; it marks the backplate dirty, and any number of
        // requests before the next render produce one frame:
        // - Application::RunMessageLoop renders every pending backplate once
        //   per loop iteration, after its bounded message drain (so all
        //   invalidations from one input message, e.g. a pan and the view
        //   sync of a second pane, land in the same present);
        // - the window is also invalidated, so a modal loop (size/move, menu,
        //   OLE drag, message box) renders it on WM_PAINT.
        // A request made while rendering is for the next frame. Without a
        // window (windowless mode, tests) it only marks the request.
        void RequestRender(RenderTrigger trigger = RenderTrigger::Invalidate);
        bool IsRenderPending() const { return m_renderPending; }
        // Renders now if a request is pending (and not already rendering);
        // returns true when it rendered. The message loop's flush.
        bool RenderIfPending();
        // Number of Render() calls that drew (or tried to draw) a frame. The
        // message loop uses it to render each backplate at most once per
        // iteration (a WM_PAINT in the drain already rendered it).
        unsigned long long RenderCount() const { return m_renderCount; }

        // Per-rect clear for the D3D swapchain backend (e.g. per-pane backgrounds).
        // Returns false if not supported/available (e.g., D2D-only backend).
        bool ClearRectD3D(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color);

    private:
        // Windowless tests drive generation changes and device discards directly.
        friend struct BackplateTestAccess;

        static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
        bool RegisterClass(const WindowOptions& options);
        HRESULT Subclass(HWND windowHandle);
        HRESULT CreateRenderTarget();
        // Creates the D3D11 device/context (BGRA support) and the D2D device and
        // context on it, with the backplate's default D2D rendering settings.
        HRESULT CreateDeviceStack(D3D_DRIVER_TYPE driverType);
        HRESULT CreateWindowlessRenderTarget();
        HRESULT CreateRenderTargetD2D();
        HRESULT EnsureRenderTargetD2D();
        HRESULT RecreateSwapChainTargets();
        HRESULT FallbackToD2DOnly(HRESULT causeHr);
        void DiscardD2DTargets();
        void DiscardDeviceResources();
        void InvalidateGraphics(
            GraphicsInvalidationReason reason,
            bool bumpDevice,
            bool bumpTarget,
            bool bumpRenderer);
        void NotifyGraphicsInvalidated(GraphicsInvalidationReason reason);
        void ScheduleNextFrame();
        void OnRecoverableEndDrawFailure(HRESULT hr, const char* where);
        bool HandleDeviceLostHr(HRESULT hr, const char* where);
        void LogDeviceRemovedReason(HRESULT triggerHr, const char* where) const;
        void Layout();

        // Hover-tooltip + toast support (see the .cpp). UpdateHoverTarget runs
        // on mouse move to find the control under the cursor and (re)arm the
        // dwell. The tooltip dwell and the toast expiry are deadlines on one
        // per-window timer (kHoverToastTimerId), not animation frames: waiting
        // renders nothing, and the WM_TIMER at a deadline runs
        // AdvanceHoverToast and renders once if it changed what is drawn.
        // Hover and toast can be emitted in separate priority bands.
        Wnd* HitTestTopLevel(const POINT& pt);
        void UpdateHoverTarget(const POINT& ptClient);
        void ClearHoverTooltip();
        // Shows a tooltip whose dwell has elapsed and drops an expired toast.
        // Returns true when that changed what DrawHoverAndToast draws.
        bool AdvanceHoverToast(unsigned long long nowMs);
        // Earliest pending tooltip-dwell or toast deadline (0 = none).
        unsigned long long NextHoverToastDeadline() const;
        // (Re)arms the timer for NextHoverToastDeadline(), or kills it.
        void ScheduleHoverToastTimer(unsigned long long nowMs);
        void OnHoverToastTimer(unsigned long long nowMs);
        void DrawHoverAndToast(
            ID2D1RenderTarget* target,
            bool drawHover,
            bool drawToast);
        // Returns `layout`, rebuilt from m_tipFormat only when `text` differs
        // from `layoutText` (the text it was built for).
        IDWriteTextLayout* EnsureTipLayout(
            Microsoft::WRL::ComPtr<IDWriteTextLayout>& layout,
            std::wstring& layoutText,
            const std::wstring& text);
        bool HasActiveOverlay(OverlayLayer layer) const;
        bool RouteOverlayInput(const InputEvent& event, OverlayLayer layer);
        void RenderOverlayLayer(ID2D1RenderTarget* target, OverlayLayer layer);

        void InvokeBeforeDestroyOnce();
        void SchedulePlacementAutosave();
        void FlushPlacementAutosave();
        void DetachAsyncRedrawControl();

        class DropTarget;

    public:
        // Opts this window into OLE file drag&drop: registers the drop
        // target (needs the HWND, so call after window creation; the
        // calling thread must have OLE initialized) and routes drags/drops
        // to the child Wnd tree via OnFileDrag/OnFileDropPaths. Idempotent.
        bool EnsureDropTargetRegistered();

    protected:
        virtual Wnd* FindTargetWnd(const POINT& ptClient);
        virtual bool HandleMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
        virtual bool HandleFileDropPaths(const std::vector<std::wstring>& paths, const POINT& ptClient);

        // OLE drag&drop (for live drag-hover visuals)
        void UnregisterDropTarget();
        bool HandleFileDragOver(const std::wstring& path, const POINT& ptClient);
        void HandleFileDragLeave();

        HWND m_window { nullptr };
        D2D1_SIZE_U m_size { 0, 0 };
        // D3D/DXGI render backend
        Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice {};
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3dContext {};
        Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain {};
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv {};

        Microsoft::WRL::ComPtr<ID2D1Device> m_d2dDevice {};
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> m_d2dContext {};
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_d2dTargetBitmap {};

        // D2D-only backend
        Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_hwndRenderTarget {};
        std::wstring m_rendererId {};

        // Off-screen buffer for double-buffering (reduces flicker)
        Microsoft::WRL::ComPtr<ID2D1BitmapRenderTarget> m_offscreenRT {};  // For D2D-only mode
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_offscreenBitmap {};         // For D3D11 D2D pass
        
        // D3D11 off-screen resources
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_offscreenTexture {};
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_offscreenRTV {};
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_offscreenD2DTarget {};      // D2D view of offscreen texture
        ID3D11RenderTargetView* m_activeD3DRenderTarget { nullptr };       // Current-frame D3D target (swapchain or offscreen)
        
        bool m_useOffscreenBuffer { true };

        // Windowless render mode (InitializeWindowless): the offscreen texture
        // is the only target, sized m_windowlessSurfaceSize pixels.
        bool m_windowless { false };
        D2D1_SIZE_U m_windowlessSurfaceSize { 0, 0 };
        // Shared so a running hook stays alive when it replaces or clears itself.
        std::shared_ptr<std::function<void()>> m_betweenPassesHook {};

        std::unordered_map<std::wstring, std::shared_ptr<Wnd>> m_children {};
        std::vector<std::shared_ptr<Wnd>> m_childrenOrdered {};
        WNDPROC m_prevWndProc { nullptr };
        bool m_classRegistered { false };
        std::wstring m_name {};
        bool m_layoutDirty { true };

        AsyncRedrawSignal m_asyncRedrawSignal {};
        std::shared_ptr<AsyncRedrawToken::ControlBlock> m_asyncRedrawControl {};
        GraphicsGeneration m_graphicsGeneration {};
        std::unordered_map<const void*, std::shared_ptr<void>> m_deviceCache {};
        unsigned m_endDrawFailureStreak { 0 };

        std::atomic<unsigned long long> m_lastAnimationRequestMs { 0 };
        std::atomic<unsigned long long> m_lastAnimationTickMs { 0 };
        // Diagnostic-only: last animation-tick cadence we logged, so ProcessAnimationTick
        // can log a one-line transition ("throttled to ~30fps" / "back to ~60fps") instead
        // of logging every single tick.
        unsigned long long m_lastLoggedTickIntervalMs { 0 };

        Wnd* m_focusedWnd { nullptr };

        std::function<void(HWND)> m_onBeforeDestroy {};
        bool m_beforeDestroyInvoked { false };

        std::function<void(HWND)> m_onWindowPlacementChanged {};
        UINT_PTR m_placeAutosaveTimerId { 0 };
        bool m_inSizeMove { false };
        bool m_resizeResourcesPending { false };
        bool m_offscreenResizePending { false };

        bool m_dropTargetRegistered { false };
        Microsoft::WRL::ComPtr<IDropTarget> m_dropTarget {};
        std::wstring m_dragPath {};

        // Hover tooltip state: the control under the cursor, its tooltip text,
        // when the hover began, whether the dwell has elapsed (so it is drawn),
        // and the anchor it was shown at. m_mouseTracking guards a single
        // TrackMouseEvent registration for WM_MOUSELEAVE.
        Wnd* m_hoverWnd { nullptr };
        std::wstring m_hoverTip {};
        POINT m_hoverPt { 0, 0 };
        POINT m_tipAnchor { 0, 0 };
        unsigned long long m_hoverSinceMs { 0 };
        bool m_tipShown { false };
        bool m_mouseTracking { false };
        // Transient toast banner text + its expiry time (0 = none).
        std::wstring m_toastText {};
        unsigned long long m_toastExpireMs { 0 };
        // Tooltip-dwell / toast-expiry deadline timer (SetTimer on m_window)
        // and the deadline it is armed for (0 = not armed).
        static constexpr UINT_PTR kHoverToastTimerId = 0xFD23;
        unsigned long long m_hoverToastTimerDueMs { 0 };
        // Lazily created DWrite format for tooltip/toast text, and the text
        // layouts built from it, rebuilt only when their text changes. DirectWrite
        // objects are device-independent and survive target/device recreation.
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_tipFormat {};
        Microsoft::WRL::ComPtr<IDWriteTextLayout> m_tipLayout {};
        std::wstring m_tipLayoutText {};
        Microsoft::WRL::ComPtr<IDWriteTextLayout> m_toastLayout {};
        std::wstring m_toastLayoutText {};
        // Tooltip/toast brush (recolored per fill), created on the current
        // target and reset by InvalidateGraphics on a new target generation.
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_tipBrush {};

        D2D1_COLOR_F m_clearColor { 0.09f, 0.09f, 0.10f, 1.0f };
        D2D1_SIZE_U m_renderSurfaceSize { 0, 0 };
        D2D1_SIZE_F m_logicalToRenderScale { 1.0f, 1.0f };
        
        // Prevent recursive rendering (e.g., when layout changes during OnRender)
        bool m_isRendering { false };
        bool m_renderRequested { false };
        // A coalesced render request (RequestRender) not rendered yet.
        bool m_renderPending { false };
        unsigned long long m_renderCount { 0 };
        int m_deferRenderDepth { 0 };

        // Diagnostic-only frame-time/FPS aggregation (see Render()). Logged once per
        // second via FD2D_LOG_INFO; the bookkeeping itself is a handful of arithmetic
        // ops per frame, so it stays cheap even when logging is disabled.
        RenderTrigger m_pendingRenderTrigger { RenderTrigger::Other };
        unsigned long long m_fpsWindowStartMs { 0 };
        int m_fpsWindowFrames { 0 };
        int m_fpsWindowTickFrames { 0 };
        int m_fpsWindowInvalidateFrames { 0 };
        int m_fpsWindowPaintFrames { 0 };
        int m_fpsWindowOtherFrames { 0 };
        int m_fpsWindowAsyncPendingFrames { 0 };
        double m_fpsWindowTotalMs { 0.0 };
        double m_fpsWindowMaxMs { 0.0 };

        // Optional render instrumentation (RenderStats::Enabled(), off by
        // default): per-window presents, presents per mouse move, frame time
        // and input-to-present latency. The two fields below collect the
        // Present wait and whether the frame reached Present, per Render().
        RenderStats m_renderStats {};
        double m_statsPresentWaitMs { 0.0 };
        bool m_statsPresented { false };
    };
}

