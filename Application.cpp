#include "Application.h"

namespace FD2D
{
    Application& Application::Instance()
    {
        static Application instance;
        return instance;
    }

    HRESULT Application::Initialize(const InitContext& context)
    {
        if (m_initialized)
        {
            return S_FALSE;
        }

        m_context = context;
        HRESULT hr = Core::Initialize(m_context);
        if (FAILED(hr))
        {
            return hr;
        }

        m_initialized = true;
        return S_OK;
    }

    void Application::Shutdown()
    {
        m_backplates.clear();
        Core::Shutdown();
        m_initialized = false;
    }

    std::shared_ptr<Backplate> Application::CreateBackplate(const std::wstring& name)
    {
        if (name.empty() || m_backplates.find(name) != m_backplates.end())
        {
            return nullptr;
        }

        auto backplate = std::make_shared<Backplate>(name);
        m_backplates.emplace(name, backplate);
        return backplate;
    }

    std::shared_ptr<Backplate> Application::CreateWindowedBackplate(const std::wstring& name, const WindowOptions& options)
    {
        if (name.empty() || m_backplates.find(name) != m_backplates.end())
        {
            return nullptr;
        }

        auto backplate = std::make_shared<Backplate>(name);

        if (FAILED(backplate->CreateWindowed(options)))
        {
            return nullptr;
        }

        m_backplates.emplace(name, backplate);
        return backplate;
    }

    std::shared_ptr<Backplate> Application::GetBackplate(const std::wstring& name) const
    {
        auto it = m_backplates.find(name);
        if (it != m_backplates.end())
        {
            return it->second;
        }

        return nullptr;
    }

    int Application::RunMessageLoop()
    {
        MSG msg {};
        while (GetMessage(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        return static_cast<int>(msg.wParam);
    }

    HINSTANCE Application::HInstance() const
    {
        return m_context.instance != nullptr ? m_context.instance : Core::Instance();
    }

    bool Application::Initialized() const
    {
        return m_initialized;
    }
}

