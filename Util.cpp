#include "Util.h"

#include <windows.h>
#include <cwctype>
#include <filesystem>

namespace FD2D::Util
{
    unsigned long long NowMs()
    {
        return static_cast<unsigned long long>(GetTickCount64());
    }

    float Clamp01(float v)
    {
        if (v < 0.0f)
        {
            return 0.0f;
        }
        if (v > 1.0f)
        {
            return 1.0f;
        }
        return v;
    }

    std::wstring NormalizePath(const std::wstring& path)
    {
        if (path.empty())
        {
            return {};
        }

        // 1) Make the path absolute without disk I/O (Win32 path resolver).
        std::wstring abs;
        DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (needed > 0)
        {
            abs.resize(static_cast<size_t>(needed));
            DWORD written = GetFullPathNameW(path.c_str(), needed, &abs[0], nullptr);
            if (written > 0 && written < needed)
            {
                abs.resize(static_cast<size_t>(written));
            }
            else if (written == 0)
            {
                abs = path;
            }
        }
        else
        {
            abs = path;
        }

        // 2) Normalize path lexically (no disk I/O) and use platform-preferred separators.
        std::wstring out;
        try
        {
            std::filesystem::path fp(abs);
            fp = fp.lexically_normal();
            fp.make_preferred();
            out = fp.wstring();
        }
        catch (...)
        {
            // Best-effort fallback (should be rare).
            out = abs;
        }

        for (auto& ch : out)
        {
            if (ch == L'/')
            {
                ch = L'\\';
            }
            ch = static_cast<wchar_t>(towlower(ch));
        }

        return out;
    }
}

