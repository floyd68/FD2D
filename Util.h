#pragma once

#include <string>

namespace FD2D::Util
{
    unsigned long long NowMs();
    float Clamp01(float v);
    std::wstring NormalizePath(const std::wstring& path);
}

