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
        // Renderer backend selection (optional).
        // - nullptr or L"d3d11_swapchain": D3D11 swapchain + D2D interop (default, fastest, supports GPU DDS)
        // - L"d2d_hwndrt": D2D-only ID2D1HwndRenderTarget (more compatible, no D3D pass, no GPU DDS)
        const wchar_t* rendererId { nullptr };
    };

    class Backplate
    {
    public:
        // Worker thread -> UI thread redraw 요청용 커스텀 메시지
        static constexpr UINT WM_FD2D_REQUEST_REDRAW = WM_APP + 0x4D2; // 'FD2'
        // UI thread에서 paint를 "곧바로" 한 번만 유도하기 위한 flush 메시지 (coalesce 용도)
        static constexpr UINT WM_FD2D_FLUSH_REDRAW = WM_APP + 0x4D3; // 'FD3'

        Backplate();
        explicit Backplate(const std::wstring& name);
        ~Backplate();

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
        bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam);

        ID2D1RenderTarget* RenderTarget() const;
        ID3D11Device* D3DDevice() const { return m_rendererId == L"d2d_hwndrt" ? nullptr : m_d3dDevice.Get(); }
        ID3D11DeviceContext* D3DContext() const { return m_rendererId == L"d2d_hwndrt" ? nullptr : m_d3dContext.Get(); }
        D2D1_SIZE_U ClientSize() const { return m_size; }

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

    private:
        static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
        bool HandleMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
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

        std::unordered_map<std::wstring, std::shared_ptr<Wnd>> m_children {};
        WNDPROC m_prevWndProc { nullptr };
        bool m_classRegistered { false };
        std::wstring m_name {};
        bool m_layoutDirty { true };
        bool m_flushRedrawQueued { false };

        HANDLE m_asyncRedrawEvent { nullptr };
        std::atomic<bool> m_asyncRedrawPending { false };

        std::atomic<unsigned long long> m_lastAnimationRequestMs { 0 };
        std::atomic<unsigned long long> m_lastAnimationTickMs { 0 };
    };
}

