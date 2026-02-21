#pragma once

#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dxgi1_2.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <memory>
#include <unordered_map>
#include <string>
#include <atomic>
#include <functional>
#include <vector>

#include "Wnd.h"

namespace FD2D
{
    enum class ChromeStyle
    {
        Standard,
        Borderless
    };

    struct WindowOptions
    {
        HINSTANCE instance { nullptr };
        const wchar_t* title { L"FD2D Window" };
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
        void Render();
        void Show(int nCmdShow);

        bool AddWnd(const std::shared_ptr<Wnd>& wnd);

        void SetFocusedWnd(Wnd* wnd);
        Wnd* FocusedWnd() const { return m_focusedWnd; }
        void ClearFocusIf(Wnd* wnd);

        ID2D1RenderTarget* RenderTarget() const;
        ID3D11Device* D3DDevice() const { return m_rendererId == L"d2d_hwndrt" ? nullptr : m_d3dDevice.Get(); }
        ID3D11DeviceContext* D3DContext() const { return m_rendererId == L"d2d_hwndrt" ? nullptr : m_d3dContext.Get(); }
        D2D1_SIZE_U ClientSize() const { return m_size; }
        D2D1_SIZE_U RenderSurfaceSize() const { return m_renderSurfaceSize; }
        D2D1_SIZE_F LogicalToRenderScale() const { return m_logicalToRenderScale; }

        // Cross-thread redraw signaling without PostMessage:
        // worker thread calls RequestAsyncRedraw() -> signals event (coalesced)
        // UI thread waits on AsyncRedrawEvent() and calls ProcessAsyncRedraw().
        HANDLE AsyncRedrawEvent() const { return m_asyncRedrawEvent; }
        void RequestAsyncRedraw();
        void ProcessAsyncRedraw();

        // Animation scheduling (spinner / cross-fade): avoids busy WM_PAINT loops.
        void RequestAnimationFrame();
        bool HasActiveAnimation(unsigned long long nowMs) const;
        void ProcessAnimationTick(unsigned long long nowMs);

        // Force layout recalculation on next render.
        void RequestLayout();

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

        // Per-rect clear for the D3D swapchain backend (used for per-ImageBrowser background).
        // Returns false if not supported/available (e.g., D2D-only backend).
        bool ClearRectD3D(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color);

    private:
        static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
        bool RegisterClass(const WindowOptions& options);
        HRESULT Subclass(HWND windowHandle);
        HRESULT CreateRenderTarget();
        HRESULT CreateRenderTargetD2D();
        HRESULT EnsureRenderTargetD2D();
        HRESULT RecreateSwapChainTargets();
        HRESULT FallbackToD2DOnly(HRESULT causeHr);
        void DiscardD2DTargets();
        void DiscardDeviceResources();
        void Layout();
        void InvokeBeforeDestroyOnce();
        void SchedulePlacementAutosave();
        void FlushPlacementAutosave();

        class DropTarget;

    protected:
        virtual Wnd* FindTargetWnd(const POINT& ptClient);
        virtual bool HandleMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
        virtual bool HandleFileDropPaths(const std::vector<std::wstring>& paths, const POINT& ptClient);

        // OLE drag&drop (for live drag-hover visuals)
        bool EnsureDropTargetRegistered();
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

        std::unordered_map<std::wstring, std::shared_ptr<Wnd>> m_children {};
        WNDPROC m_prevWndProc { nullptr };
        bool m_classRegistered { false };
        std::wstring m_name {};
        bool m_layoutDirty { true };

        HANDLE m_asyncRedrawEvent { nullptr };
        std::atomic<bool> m_asyncRedrawPending { false };

        std::atomic<unsigned long long> m_lastAnimationRequestMs { 0 };
        std::atomic<unsigned long long> m_lastAnimationTickMs { 0 };

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

        D2D1_COLOR_F m_clearColor { 0.09f, 0.09f, 0.10f, 1.0f };
        D2D1_SIZE_U m_renderSurfaceSize { 0, 0 };
        D2D1_SIZE_F m_logicalToRenderScale { 1.0f, 1.0f };
        
        // Prevent recursive rendering (e.g., when layout changes during OnRender)
        bool m_isRendering { false };
        bool m_renderRequested { false };
    };
}

