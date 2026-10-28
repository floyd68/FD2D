#include "Application.h"
#include "AsyncRedrawDispatch.h"
#include "MessagePump.h"
#include "Util.h"
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

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
        // Use insert instead of emplace (prevents optimization issues in Release mode)
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

        // Use insert instead of emplace (prevents optimization issues in Release mode)
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

    namespace
    {
        // Application::RunMessageLoop's side of the pump policy (MessagePump.h):
        // the real wait, queue and backplates.
        class BackplatePumpHost
        {
        public:
            explicit BackplatePumpHost(const std::unordered_map<std::wstring, std::shared_ptr<Backplate>>& backplates)
                : m_backplates(backplates)
            {
            }

            bool HasPendingRender() const
            {
                for (const auto& kv : m_backplates)
                {
                    if (kv.second && kv.second->IsRenderPending())
                    {
                        return true;
                    }
                }
                return false;
            }

            // With an active animation, wake at ~60fps to paint smoothly.
            // Otherwise keep a safety heartbeat (an idle wake renders nothing).
            DWORD IdleTimeoutMs() const
            {
                const unsigned long long now = Util::NowMs();
                for (const auto& kv : m_backplates)
                {
                    if (kv.second && kv.second->HasActiveAnimation(now))
                    {
                        return 16;
                    }
                }
                return 1000;
            }

            // Waits for a message (MWMO_INPUTAVAILABLE: also one already seen
            // but left in the queue by a bounded drain) or an async-redraw
            // event. With no event (no backplate) it waits for messages only.
            bool Wait(DWORD timeoutMs)
            {
                // A new iteration: remember what each backplate has rendered so
                // far (see FlushRenders).
                m_renderCountsAtStart.clear();
                for (const auto& backplate : Snapshot())
                {
                    m_renderCountsAtStart.emplace_back(backplate, backplate->RenderCount());
                }
                CollectEvents();
                const DWORD count = static_cast<DWORD>(m_events.size());
                const DWORD result = MsgWaitForMultipleObjectsEx(
                    count,
                    count > 0 ? m_events.data() : nullptr,
                    timeoutMs,
                    QS_ALLINPUT,
                    MWMO_INPUTAVAILABLE);
                return result != WAIT_FAILED;
            }

            // Services every owner whose event is signaled now, once each. A busy
            // owner that re-signals during its servicing cannot starve the
            // others: it is not revisited until the next iteration. Owners are
            // held by shared_ptr for the pass.
            void ServiceAsyncEvents()
            {
                ServiceSignaledEvents(m_events, 0, [&](std::size_t index)
                {
                    m_eventOwners[index]->ProcessAsyncRedraw();
                });
            }

            bool Peek(MSG& msg) const
            {
                return PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE;
            }

            void Dispatch(const MSG& msg) const
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            unsigned long long NowMs() const
            {
                return Util::NowMs();
            }

            void Tick(unsigned long long nowMs)
            {
                for (const auto& backplate : Snapshot())
                {
                    backplate->ProcessAnimationTick(nowMs);
                }
            }

            // Renders each backplate with a pending request once, unless it
            // already rendered in this iteration (a WM_PAINT in the drain, or an
            // explicit Render()): then a request made during that render waits
            // for the next iteration, which does not block while it is pending.
            // At most one render per backplate per iteration.
            void FlushRenders()
            {
                for (const auto& [backplate, countAtStart] : m_renderCountsAtStart)
                {
                    if (backplate->RenderCount() == countAtStart)
                    {
                        (void)backplate->RenderIfPending();
                    }
                }
                m_renderCountsAtStart.clear();
            }

        private:
            // The async redraw events (one per backplate), with the owning
            // backplate at the same index so a signaled event maps back to it.
            void CollectEvents()
            {
                m_events.clear();
                m_eventOwners.clear();
                for (const auto& kv : m_backplates)
                {
                    if (kv.second)
                    {
                        HANDLE ev = kv.second->AsyncRedrawEvent();
                        if (ev)
                        {
                            m_events.push_back(ev);
                            m_eventOwners.push_back(kv.second);
                        }
                    }
                }
            }

            // Held for a pass so a backplate dropped while another renders stays
            // valid until the pass completes.
            std::vector<std::shared_ptr<Backplate>> Snapshot() const
            {
                std::vector<std::shared_ptr<Backplate>> backplates;
                backplates.reserve(m_backplates.size());
                for (const auto& kv : m_backplates)
                {
                    if (kv.second)
                    {
                        backplates.push_back(kv.second);
                    }
                }
                return backplates;
            }

            const std::unordered_map<std::wstring, std::shared_ptr<Backplate>>& m_backplates;
            std::vector<HANDLE> m_events {};
            std::vector<std::shared_ptr<Backplate>> m_eventOwners {};
            std::vector<std::pair<std::shared_ptr<Backplate>, unsigned long long>> m_renderCountsAtStart {};
        };
    }

    int Application::RunMessageLoop()
    {
        // Each iteration waits (without spinning, also with no backplate),
        // services the signaled async redraws, drains a bounded number of
        // messages, runs the animation tick and renders every backplate with a
        // pending request once (see MessagePump.h). Returns the WM_QUIT exit
        // code, or -1 if the wait fails.
        BackplatePumpHost host(m_backplates);
        const MessagePumpBudget budget {};
        for (;;)
        {
            if (const std::optional<int> exitCode = PumpMessagesOnce(host, budget))
            {
                return *exitCode;
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

