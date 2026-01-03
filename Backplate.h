#pragma once

#include <windows.h>
#include <d2d1.h>
#include <wrl/client.h>
#include <memory>
#include <unordered_map>
#include <string>

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
    };

    class Backplate
    {
    public:
        Backplate();
        explicit Backplate(const std::wstring& name);
        ~Backplate() = default;

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

        ID2D1HwndRenderTarget* RenderTarget() const;

    private:
        static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
        bool HandleMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
        bool RegisterClass(const WindowOptions& options);
        HRESULT Subclass(HWND windowHandle);
        HRESULT CreateRenderTarget();
        void DiscardDeviceResources();
        void Layout();

        HWND m_window { nullptr };
        D2D1_SIZE_U m_size { 0, 0 };
        Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_renderTarget {};
        std::unordered_map<std::wstring, std::shared_ptr<Wnd>> m_children {};
        WNDPROC m_prevWndProc { nullptr };
        bool m_classRegistered { false };
        std::wstring m_name {};
        bool m_layoutDirty { true };
    };
}

