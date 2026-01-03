#pragma once

#include <memory>
#include <unordered_map>
#include <string>

#include "Core.h"
#include "Backplate.h"

namespace FD2D
{
    class Application
    {
    public:
        static Application& Instance();

        HRESULT Initialize(const InitContext& context);
        void Shutdown();

        std::shared_ptr<Backplate> CreateBackplate(const std::wstring& name);
        std::shared_ptr<Backplate> CreateWindowedBackplate(const std::wstring& name, const WindowOptions& options);

        std::shared_ptr<Backplate> GetBackplate(const std::wstring& name) const;

        int RunMessageLoop();

        HINSTANCE HInstance() const;
        bool Initialized() const;

    private:
        Application() = default;
        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;

        bool m_initialized { false };
        InitContext m_context {};
        std::unordered_map<std::wstring, std::shared_ptr<Backplate>> m_backplates {};
    };
}

