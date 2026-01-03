#include "Backplate.h"
#include "Core.h"

namespace FD2D
{
    Backplate::Backplate()
    {
    }

    Backplate::Backplate(const std::wstring& name)
        : m_name(name)
    {
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
                self->EnsureRenderTarget();
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
        case WM_SIZE:
        {
            Resize(LOWORD(lParam), HIWORD(lParam));
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

        case WM_DESTROY:
        {
            PostQuitMessage(0);
            result = 0;
            return true;
        }

        default:
            break;
        }

        bool handled = false;

        for (auto& pair : m_children)
        {
            if (pair.second && pair.second->OnMessage(message, wParam, lParam))
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

    bool Backplate::RegisterClass(const WindowOptions& options)
    {
        if (m_classRegistered)
        {
            return true;
        }

        WNDCLASSEXW wcex {};
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = Backplate::WndProc;
        wcex.cbClsExtra = 0;
        wcex.cbWndExtra = 0;
        wcex.hInstance = options.instance != nullptr ? options.instance : Core::Instance();
        wcex.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wcex.lpszMenuName = nullptr;
        wcex.lpszClassName = options.className;
        wcex.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

        if (RegisterClassExW(&wcex) == 0)
        {
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

        return CreateRenderTarget();
    }

    HRESULT Backplate::CreateWindowed(const WindowOptions& options)
    {
        WindowOptions opts = options;
        if (opts.instance == nullptr)
        {
            opts.instance = Core::Instance();
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
        if (!m_renderTarget)
        {
            return CreateRenderTarget();
        }

        return S_OK;
    }

    HRESULT Backplate::CreateRenderTarget()
    {
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

        RECT clientRect {};
        GetClientRect(m_window, &clientRect);
        m_size = D2D1::SizeU(clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);

        return factory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(m_window, m_size),
            &m_renderTarget);
    }

    void Backplate::DiscardDeviceResources()
    {
        m_renderTarget.Reset();
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

        if (m_renderTarget)
        {
            m_renderTarget->Resize(m_size);
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

    void Backplate::Render()
    {
        if (m_layoutDirty)
        {
            Layout();
        }
        if (FAILED(EnsureRenderTarget()))
        {
            return;
        }

        m_renderTarget->BeginDraw();
        m_renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
        m_renderTarget->Clear(D2D1::ColorF(D2D1::ColorF::DarkSlateGray));

        for (auto& pair : m_children)
        {
            if (pair.second)
            {
                pair.second->OnRender(m_renderTarget.Get());
            }
        }

        HRESULT hr = m_renderTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET)
        {
            DiscardDeviceResources();
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

    ID2D1HwndRenderTarget* Backplate::RenderTarget() const
    {
        return m_renderTarget.Get();
    }
}

