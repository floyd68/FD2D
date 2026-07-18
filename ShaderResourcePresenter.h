#pragma once

#include <d2d1.h>
#include <d3d11.h>
#include <cstdint>

namespace FD2D
{
    class Backplate;

    struct ShaderResourceDraw
    {
        D2D1_RECT_F layout {};
        UINT contentWidth { 0 };
        UINT contentHeight { 0 };
        float zoomScale { 1.0f };
        float panX { 0.0f };
        float panY { 0.0f };
        int rotationQuarters { 0 };
        bool highQualitySampling { true };
        bool alphaCheckerboardEnabled { false };
        int channelMode { 0 };
        int sourceAlphaEncoding { 0 };
        int sourceAlphaUsage { 0 };
    };

    bool TryGetShaderResourceTexelSize(
        ID3D11ShaderResourceView* srv,
        UINT& width,
        UINT& height);

    HRESULT DrawShaderResource(
        ID3D11DeviceContext* context,
        Backplate& backplate,
        ID3D11ShaderResourceView* srv,
        const ShaderResourceDraw& draw);

    void ResetShaderResourcePresenter();
}
