#include "Application.h"
// Tear down ImageCore before FD2D::Core shutdown and before app CoUninitialize
#include "../ImageCore/ImageLoader.h"
#include <vector>

namespace FD2D
{
    static unsigned long long NowMs()
    {
        return static_cast<unsigned long long>(GetTickCount64());
    }

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
        ImageCore::ImageLoader::Instance().Shutdown();
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
        // emplace 대신 insert 사용 (Release 모드 최적화 문제 방지)
        m_backplates[name] = backplate;
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

        // emplace 대신 insert 사용 (Release 모드 최적화 문제 방지)
        m_backplates[name] = backplate;
        return backplate;
    }

    bool Application::RegisterBackplate(const std::shared_ptr<Backplate>& backplate)
    {
        if (!backplate)
        {
            return false;
        }

        const std::wstring name = backplate->Name();
        if (name.empty() || m_backplates.find(name) != m_backplates.end())
        {
            return false;
        }

        m_backplates[name] = backplate;
        return true;
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
        for (;;)
        {
            // Build the set of async redraw events (one per backplate).
            std::vector<HANDLE> events;
            events.reserve(m_backplates.size());
            for (const auto& kv : m_backplates)
            {
                if (kv.second)
                {
                    HANDLE ev = kv.second->AsyncRedrawEvent();
                    if (ev)
                    {
                        events.push_back(ev);
                    }
                }
            }

            const DWORD waitCount = static_cast<DWORD>(events.size());

            // If any backplate has an active animation, wake at ~60fps to paint smoothly.
            // Otherwise, keep a safety heartbeat (prevents "stuck forever" if a wakeup is missed).
            DWORD timeoutMs = 1000;
            const unsigned long long now = NowMs();
            for (const auto& kv : m_backplates)
            {
                if (kv.second && kv.second->HasActiveAnimation(now))
                {
                    timeoutMs = 16;
                    break;
                }
            }

            const DWORD waitRes = MsgWaitForMultipleObjectsEx(
                waitCount,
                waitCount > 0 ? events.data() : nullptr,
                timeoutMs,
                QS_ALLINPUT,
                MWMO_INPUTAVAILABLE);

            if (waitRes == WAIT_FAILED)
            {
                return -1;
            }

            // One of our async redraw events fired.
            if (waitRes >= WAIT_OBJECT_0 && waitRes < WAIT_OBJECT_0 + waitCount)
            {
                for (const auto& kv : m_backplates)
                {
                    if (kv.second)
                    {
                        kv.second->ProcessAsyncRedraw();
                    }
                }
                // Do NOT continue here:
                // When worker completions arrive frequently (e.g., heavy I/O), we can starve the animation tick
                // and make spinners/fades appear to "pause". We'll fall through to drain messages and run a
                // throttled animation tick each loop.
            }

            // Animation tick timeout (no messages/events, but animations are active).
            if (waitRes == WAIT_TIMEOUT)
            {
                const unsigned long long t = NowMs();
                for (const auto& kv : m_backplates)
                {
                    if (kv.second)
                    {
                        kv.second->ProcessAnimationTick(t);
                    }
                }
                continue;
            }

            // Process all pending Windows messages.
            MSG msg {};
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                {
                    return static_cast<int>(msg.wParam);
                }

                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            // Important: animations must advance even when the message queue is busy and we never hit WAIT_TIMEOUT.
            // So after draining messages, run a throttled animation tick.
            const unsigned long long t = NowMs();
            for (const auto& kv : m_backplates)
            {
                if (kv.second)
                {
                    kv.second->ProcessAnimationTick(t);
                }
            }
        }
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

