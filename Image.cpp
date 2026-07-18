#include "Image.h"
#include "Backplate.h"
#include "Core.h"
#include "ShaderResourcePresenter.h"
#include "Util.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <vector>

namespace FD2D
{
    namespace
    {
        struct QuadVertex
        {
            float px;
            float py;
            float u;
            float v;
        };

        // Device-generation keyed quad resources (not a forever process-global cache).
        struct D3DQuadResources
        {
            Microsoft::WRL::ComPtr<ID3D11Device> device {};
            uint64_t deviceGeneration { 0 };
            Microsoft::WRL::ComPtr<ID3D11VertexShader> vs {};
            Microsoft::WRL::ComPtr<ID3D11PixelShader> ps {};
            Microsoft::WRL::ComPtr<ID3D11PixelShader> psArray {};
            Microsoft::WRL::ComPtr<ID3D11PixelShader> psCube {};
            Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout {};
            Microsoft::WRL::ComPtr<ID3D11Buffer> vb {};
            Microsoft::WRL::ComPtr<ID3D11Buffer> cb {};
            Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerPoint {};
            Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerLinear {};
            Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerWrap {};
            Microsoft::WRL::ComPtr<ID3D11BlendState> blend {};
            Microsoft::WRL::ComPtr<ID3D11RasterizerState> rsScissor {};
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> checkerSrv {};
        };

        static D3DQuadResources g_quad {};

        static void ResetD3DQuadResources()
        {
            g_quad = {};
        }

        static HRESULT EnsureD3DQuadResources(ID3D11Device* device, uint64_t deviceGeneration)
        {
            if (!device)
            {
                return E_INVALIDARG;
            }

            const bool sameDevice = (g_quad.device.Get() == device);
            const bool sameGen = (g_quad.deviceGeneration == deviceGeneration);
            if (g_quad.vs && g_quad.ps && g_quad.psArray && g_quad.psCube &&
                g_quad.inputLayout && g_quad.vb &&
                g_quad.samplerPoint && g_quad.samplerLinear && g_quad.samplerWrap &&
                g_quad.blend && g_quad.rsScissor && g_quad.cb && g_quad.checkerSrv &&
                sameDevice && sameGen)
            {
                return S_OK;
            }

            ResetD3DQuadResources();
            g_quad.device = device;
            g_quad.deviceGeneration = deviceGeneration;

            const char* vsSrc =
                "struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };"
                "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
                "VSOut main(VSIn i){ VSOut o; o.pos=float4(i.pos,0,1); o.uv=i.uv; return o; }";

            // channelMode isolates one channel as grayscale: 0=RGBA (normal),
            // 1=R, 2=G, 3=B, 4=A. Alpha is forced opaque for isolated channels so
            // e.g. a packed _rmaos channel is readable on its own. Two orthogonal
            // inputs reconcile the source under the premultiplied blend (ONE/INV_SRC_ALPHA):
            //   enc  (0=straight, 1=premultiplied)  - how color is STORED
            //   use  (0=coverage, 1=data)           - what the alpha MEANS
            //  - normal display (mode 0): data => opaque straight RGB (alpha isn't
            //    transparency, so it must not fade the image); coverage => composite
            //    (straight premultiplies rgb*=a; premultiplied stays as-is).
            //  - color isolation (1/2/3): a premultiplied source is unpremultiplied
            //    (rgb/a) for the true straight channel value; straight as-is.
            //  - alpha isolation (4): always the stored alpha value.
            // The alpha checkerboard is only a background pass; it does not change
            // coverage here.
            const char* psSrc =
                "Texture2D tex0 : register(t0);"
                "SamplerState samp0 : register(s0);"
                "cbuffer Cb : register(b0) { float opacity; float channelMode; float enc; float use; };"
                "float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target {"
                "  float4 c = tex0.Sample(samp0, uv);"
                "  int m = (int)channelMode;"
                "  bool premul = enc > 0.5;"
                "  if (m == 0) {"
                "    if (use > 0.5) {"                          // data: opaque straight RGB
                "      if (premul && c.a > 0.0) c.rgb /= c.a;"
                "      c.a = 1.0;"
                "    } else if (!premul) {"                     // coverage + straight -> premultiply for blend
                "      c.rgb *= c.a;"
                "    }"                                          // coverage + premultiplied: as-is
                "  }"
                "  else if (m >= 1 && m <= 3) {"
                "    float3 rgb = c.rgb;"
                "    if (premul && c.a > 0.0) rgb /= c.a;"      // premultiplied -> straight channel value
                "    if (m == 1) c = float4(rgb.rrr, 1);"
                "    else if (m == 2) c = float4(rgb.ggg, 1);"
                "    else c = float4(rgb.bbb, 1);"
                "  }"
                "  else if (m == 4) c = float4(c.aaa, 1);"
                "  c.a *= opacity;"
                "  c.rgb *= opacity;"
                "  return c;"
                "}";

            const char* psArraySrc =
                "Texture2DArray tex0 : register(t0);"
                "SamplerState samp0 : register(s0);"
                "cbuffer Cb : register(b0) { float opacity; float channelMode; float enc; float use; };"
                "float4 Present(float4 c) {"
                "  int m = (int)channelMode; bool premul = enc > 0.5;"
                "  if (m == 0) { if (use > 0.5) { if (premul && c.a > 0.0) c.rgb /= c.a; c.a = 1.0; }"
                "    else if (!premul) c.rgb *= c.a; }"
                "  else if (m >= 1 && m <= 3) { float3 rgb=c.rgb; if (premul && c.a>0.0) rgb/=c.a;"
                "    if (m==1) c=float4(rgb.rrr,1); else if (m==2) c=float4(rgb.ggg,1); else c=float4(rgb.bbb,1); }"
                "  else if (m==4) c=float4(c.aaa,1);"
                "  c.a*=opacity; c.rgb*=opacity; return c;"
                "}"
                "float4 main(float4 pos:SV_Position,float2 uv:TEXCOORD0):SV_Target"
                "{ return Present(tex0.Sample(samp0,float3(uv,0))); }";

            const char* psCubeSrc =
                "TextureCube tex0 : register(t0);"
                "SamplerState samp0 : register(s0);"
                "cbuffer Cb : register(b0) { float opacity; float channelMode; float enc; float use; };"
                "float4 Present(float4 c) {"
                "  int m = (int)channelMode; bool premul = enc > 0.5;"
                "  if (m == 0) { if (use > 0.5) { if (premul && c.a > 0.0) c.rgb /= c.a; c.a = 1.0; }"
                "    else if (!premul) c.rgb *= c.a; }"
                "  else if (m >= 1 && m <= 3) { float3 rgb=c.rgb; if (premul && c.a>0.0) rgb/=c.a;"
                "    if (m==1) c=float4(rgb.rrr,1); else if (m==2) c=float4(rgb.ggg,1); else c=float4(rgb.bbb,1); }"
                "  else if (m==4) c=float4(c.aaa,1);"
                "  c.a*=opacity; c.rgb*=opacity; return c;"
                "}"
                "float4 main(float4 pos:SV_Position,float2 uv:TEXCOORD0):SV_Target"
                "{ float3 dir=float3(1,1-2*uv.y,2*uv.x-1); return Present(tex0.Sample(samp0,dir)); }";

            Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
            Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
            Microsoft::WRL::ComPtr<ID3DBlob> psArrayBlob;
            Microsoft::WRL::ComPtr<ID3DBlob> psCubeBlob;
            Microsoft::WRL::ComPtr<ID3DBlob> err;

            HRESULT hr = D3DCompile(vsSrc, strlen(vsSrc), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsBlob, &err);
            if (FAILED(hr))
            {
                return hr;
            }
            hr = D3DCompile(psSrc, strlen(psSrc), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psBlob, &err);
            if (FAILED(hr))
            {
                return hr;
            }
            hr = D3DCompile(psArraySrc, strlen(psArraySrc), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psArrayBlob, &err);
            if (FAILED(hr))
            {
                return hr;
            }
            hr = D3DCompile(psCubeSrc, strlen(psCubeSrc), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psCubeBlob, &err);
            if (FAILED(hr))
            {
                return hr;
            }

            hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_quad.vs);
            if (FAILED(hr))
            {
                return hr;
            }
            hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_quad.ps);
            if (FAILED(hr))
            {
                return hr;
            }
            hr = device->CreatePixelShader(
                psArrayBlob->GetBufferPointer(), psArrayBlob->GetBufferSize(), nullptr, &g_quad.psArray);
            if (FAILED(hr))
            {
                return hr;
            }
            hr = device->CreatePixelShader(
                psCubeBlob->GetBufferPointer(), psCubeBlob->GetBufferSize(), nullptr, &g_quad.psCube);
            if (FAILED(hr))
            {
                return hr;
            }

            const D3D11_INPUT_ELEMENT_DESC il[] =
            {
                { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            hr = device->CreateInputLayout(il, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_quad.inputLayout);
            if (FAILED(hr))
            {
                return hr;
            }

            D3D11_BUFFER_DESC bd {};
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.ByteWidth = sizeof(QuadVertex) * 4;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            hr = device->CreateBuffer(&bd, nullptr, &g_quad.vb);
            if (FAILED(hr))
            {
                return hr;
            }

            D3D11_SAMPLER_DESC sdPoint {};
            sdPoint.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sdPoint.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sdPoint.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sdPoint.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sdPoint.MaxAnisotropy = 1;
            sdPoint.MinLOD = 0.0f;
            sdPoint.MaxLOD = D3D11_FLOAT32_MAX;
            sdPoint.ComparisonFunc = D3D11_COMPARISON_NEVER;
            hr = device->CreateSamplerState(&sdPoint, &g_quad.samplerPoint);
            if (FAILED(hr))
            {
                return hr;
            }

            D3D11_SAMPLER_DESC sdLinear = sdPoint;
            sdLinear.Filter = D3D11_FILTER_ANISOTROPIC;
            sdLinear.MaxAnisotropy = 16;
            hr = device->CreateSamplerState(&sdLinear, &g_quad.samplerLinear);
            if (FAILED(hr))
            {
                return hr;
            }

            D3D11_SAMPLER_DESC sdWrap = sdPoint;
            sdWrap.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
            sdWrap.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
            sdWrap.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            hr = device->CreateSamplerState(&sdWrap, &g_quad.samplerWrap);
            if (FAILED(hr))
            {
                return hr;
            }

            D3D11_BLEND_DESC blend {};
            blend.RenderTarget[0].BlendEnable = TRUE;
            blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            hr = device->CreateBlendState(&blend, &g_quad.blend);
            if (FAILED(hr))
            {
                return hr;
            }

            D3D11_RASTERIZER_DESC rd {};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            rd.ScissorEnable = TRUE;
            hr = device->CreateRasterizerState(&rd, &g_quad.rsScissor);
            if (FAILED(hr))
            {
                return hr;
            }

            D3D11_BUFFER_DESC cbd {};
            cbd.Usage = D3D11_USAGE_DYNAMIC;
            cbd.ByteWidth = 16;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            hr = device->CreateBuffer(&cbd, nullptr, &g_quad.cb);
            if (FAILED(hr))
            {
                return hr;
            }

            constexpr UINT texW = 64;
            constexpr UINT texH = 64;
            constexpr UINT tile = 8;

            std::vector<UINT32> pixels;
            pixels.resize(static_cast<size_t>(texW) * static_cast<size_t>(texH));
            const UINT32 light = 0xFFF0F0F0;
            const UINT32 dark = 0xFF707070;
            for (UINT y = 0; y < texH; ++y)
            {
                for (UINT x = 0; x < texW; ++x)
                {
                    const UINT tx = x / tile;
                    const UINT ty = y / tile;
                    const bool useDark = (((tx + ty) & 1U) != 0);
                    pixels[static_cast<size_t>(y) * texW + x] = useDark ? dark : light;
                }
            }

            D3D11_TEXTURE2D_DESC td {};
            td.Width = texW;
            td.Height = texH;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA init {};
            init.pSysMem = pixels.data();
            init.SysMemPitch = texW * sizeof(UINT32);

            Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
            hr = device->CreateTexture2D(&td, &init, &tex);
            if (FAILED(hr))
            {
                return hr;
            }

            hr = device->CreateShaderResourceView(tex.Get(), nullptr, &g_quad.checkerSrv);
            if (FAILED(hr))
            {
                return hr;
            }

            return S_OK;
        }

        static bool TryDiscoverSrvTexelSize(ID3D11ShaderResourceView* srv, UINT& outW, UINT& outH)
        {
            outW = 0;
            outH = 0;
            if (!srv)
            {
                return false;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC svd {};
            srv->GetDesc(&svd);
            if (svd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D &&
                svd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DARRAY &&
                svd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURECUBE)
            {
                return false;
            }

            Microsoft::WRL::ComPtr<ID3D11Resource> res;
            srv->GetResource(&res);
            if (!res)
            {
                return false;
            }

            Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
            if (FAILED(res.As(&tex)) || !tex)
            {
                return false;
            }

            D3D11_TEXTURE2D_DESC td {};
            tex->GetDesc(&td);

            UINT mip = 0;
            if (svd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
            {
                mip = svd.Texture2D.MostDetailedMip;
            }
            else if (svd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
            {
                mip = svd.Texture2DArray.MostDetailedMip;
            }
            else if (svd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBE)
            {
                mip = svd.TextureCube.MostDetailedMip;
            }

            outW = (std::max)(1u, td.Width >> mip);
            outH = (std::max)(1u, td.Height >> mip);
            return true;
        }

        static int NormalizeRotationQuarters(int q)
        {
            return ((q % 4) + 4) % 4;
        }
    }

    bool TryGetShaderResourceTexelSize(
        ID3D11ShaderResourceView* srv,
        UINT& width,
        UINT& height)
    {
        return TryDiscoverSrvTexelSize(srv, width, height);
    }

    void ResetShaderResourcePresenter()
    {
        ResetD3DQuadResources();
    }

    HRESULT DrawShaderResource(
        ID3D11DeviceContext* context,
        Backplate& backplate,
        ID3D11ShaderResourceView* srv,
        const ShaderResourceDraw& draw)
    {
        if (!context || !srv || draw.contentWidth == 0 || draw.contentHeight == 0)
        {
            return E_INVALIDARG;
        }

        ID3D11Device* device = backplate.D3DDevice();
        if (!device)
        {
            return E_NOINTERFACE;
        }

        const uint64_t deviceGeneration = backplate.GetGraphicsGeneration().device;
        HRESULT hr = EnsureD3DQuadResources(device, deviceGeneration);
        if (FAILED(hr))
        {
            return hr;
        }

        const D2D1_SIZE_U logicalCs = backplate.ClientSize();
        D2D1_SIZE_U cs = backplate.RenderSurfaceSize();
        if (cs.width == 0 || cs.height == 0)
        {
            cs = logicalCs;
        }
        if (cs.width == 0 || cs.height == 0 || logicalCs.width == 0 || logicalCs.height == 0)
        {
            return E_FAIL;
        }

        const D2D1_SIZE_F logicalToRender = backplate.LogicalToRenderScale();
        D2D1_RECT_F layout
        {
            draw.layout.left * logicalToRender.width,
            draw.layout.top * logicalToRender.height,
            draw.layout.right * logicalToRender.width,
            draw.layout.bottom * logicalToRender.height
        };
        if (layout.right <= layout.left || layout.bottom <= layout.top)
        {
            return E_INVALIDARG;
        }

        D3D11_RECT scissor
        {
            static_cast<LONG>(std::floor(layout.left)),
            static_cast<LONG>(std::floor(layout.top)),
            static_cast<LONG>(std::ceil(layout.right)),
            static_cast<LONG>(std::ceil(layout.bottom))
        };
        scissor.left = (std::max)(0L, (std::min)(scissor.left, static_cast<LONG>(cs.width)));
        scissor.top = (std::max)(0L, (std::min)(scissor.top, static_cast<LONG>(cs.height)));
        scissor.right = (std::max)(0L, (std::min)(scissor.right, static_cast<LONG>(cs.width)));
        scissor.bottom = (std::max)(0L, (std::min)(scissor.bottom, static_cast<LONG>(cs.height)));
        if (scissor.left >= scissor.right || scissor.top >= scissor.bottom)
        {
            return S_FALSE;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc {};
        srv->GetDesc(&srvDesc);
        ID3D11PixelShader* shader = nullptr;
        switch (srvDesc.ViewDimension)
        {
        case D3D11_SRV_DIMENSION_TEXTURE2D:
            shader = g_quad.ps.Get();
            break;

        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
            shader = g_quad.psArray.Get();
            break;

        case D3D11_SRV_DIMENSION_TEXTURECUBE:
            shader = g_quad.psCube.Get();
            break;

        default:
            return E_INVALIDARG;
        }

        Microsoft::WRL::ComPtr<ID3D11RasterizerState> prevRs;
        context->RSGetState(&prevRs);
        UINT prevScissorCount = 0;
        context->RSGetScissorRects(&prevScissorCount, nullptr);
        std::vector<D3D11_RECT> prevScissors(prevScissorCount);
        if (prevScissorCount > 0)
        {
            context->RSGetScissorRects(&prevScissorCount, prevScissors.data());
        }

        Microsoft::WRL::ComPtr<ID3D11InputLayout> prevInputLayout;
        context->IAGetInputLayout(&prevInputLayout);
        Microsoft::WRL::ComPtr<ID3D11Buffer> prevVertexBuffer;
        UINT prevVertexStride = 0;
        UINT prevVertexOffset = 0;
        context->IAGetVertexBuffers(
            0,
            1,
            prevVertexBuffer.GetAddressOf(),
            &prevVertexStride,
            &prevVertexOffset);
        D3D11_PRIMITIVE_TOPOLOGY prevTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        context->IAGetPrimitiveTopology(&prevTopology);

        Microsoft::WRL::ComPtr<ID3D11VertexShader> prevVs;
        context->VSGetShader(&prevVs, nullptr, nullptr);
        Microsoft::WRL::ComPtr<ID3D11PixelShader> prevPs;
        context->PSGetShader(&prevPs, nullptr, nullptr);
        Microsoft::WRL::ComPtr<ID3D11SamplerState> prevSampler;
        context->PSGetSamplers(0, 1, prevSampler.GetAddressOf());
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> prevSrv;
        context->PSGetShaderResources(0, 1, prevSrv.GetAddressOf());
        Microsoft::WRL::ComPtr<ID3D11Buffer> prevConstantBuffer;
        context->PSGetConstantBuffers(0, 1, prevConstantBuffer.GetAddressOf());

        Microsoft::WRL::ComPtr<ID3D11BlendState> prevBlendState;
        float prevBlendFactor[4] {};
        UINT prevSampleMask = 0;
        context->OMGetBlendState(&prevBlendState, prevBlendFactor, &prevSampleMask);

        const auto restoreState = [&]()
        {
            context->IASetInputLayout(prevInputLayout.Get());
            ID3D11Buffer* vertexBuffer = prevVertexBuffer.Get();
            context->IASetVertexBuffers(
                0,
                1,
                &vertexBuffer,
                &prevVertexStride,
                &prevVertexOffset);
            context->IASetPrimitiveTopology(prevTopology);
            context->VSSetShader(prevVs.Get(), nullptr, 0);
            context->PSSetShader(prevPs.Get(), nullptr, 0);

            ID3D11SamplerState* samplerState = prevSampler.Get();
            context->PSSetSamplers(0, 1, &samplerState);
            ID3D11ShaderResourceView* shaderResource = prevSrv.Get();
            context->PSSetShaderResources(0, 1, &shaderResource);
            ID3D11Buffer* constantBuffer = prevConstantBuffer.Get();
            context->PSSetConstantBuffers(0, 1, &constantBuffer);

            context->OMSetBlendState(prevBlendState.Get(), prevBlendFactor, prevSampleMask);
            context->RSSetScissorRects(
                prevScissorCount,
                prevScissorCount > 0 ? prevScissors.data() : nullptr);
            context->RSSetState(prevRs.Get());
        };

        context->RSSetState(g_quad.rsScissor.Get());
        context->RSSetScissorRects(1, &scissor);

        const auto toNdcX = [cs](float x)
        {
            return (x / static_cast<float>(cs.width)) * 2.0f - 1.0f;
        };
        const auto toNdcY = [cs](float y)
        {
            return 1.0f - (y / static_cast<float>(cs.height)) * 2.0f;
        };

        const auto drawRect = [&](
            ID3D11ShaderResourceView* source,
            const D2D1_RECT_F& rectPx,
            float uMax,
            float vMax,
            ID3D11SamplerState* sampler,
            ID3D11PixelShader* shader,
            float channelMode,
            float encoding,
            float usage) -> HRESULT
        {
            const D2D1_RECT_F zoomed = Util::ApplyZoomPanToRect(
                rectPx,
                draw.zoomScale,
                draw.panX * logicalToRender.width,
                draw.panY * logicalToRender.height);
            const float cx = (layout.left + layout.right) * 0.5f;
            const float cy = (layout.top + layout.bottom) * 0.5f;
            const int q = NormalizeRotationQuarters(draw.rotationQuarters);
            const auto rotate = [cx, cy, q](float x, float y, float& outX, float& outY)
            {
                const float dx = x - cx;
                const float dy = y - cy;
                switch (q)
                {
                case 1:
                    outX = cx - dy;
                    outY = cy + dx;
                    break;

                case 2:
                    outX = cx - dx;
                    outY = cy - dy;
                    break;

                case 3:
                    outX = cx + dy;
                    outY = cy - dx;
                    break;

                default:
                    outX = x;
                    outY = y;
                    break;
                }
            };

            float tlx, tly, trx, try_, blx, bly, brx, bry;
            rotate(zoomed.left, zoomed.top, tlx, tly);
            rotate(zoomed.right, zoomed.top, trx, try_);
            rotate(zoomed.left, zoomed.bottom, blx, bly);
            rotate(zoomed.right, zoomed.bottom, brx, bry);

            D3D11_MAPPED_SUBRESOURCE mapped {};
            HRESULT mapHr = context->Map(g_quad.vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(mapHr))
            {
                return mapHr;
            }
            auto* vertices = reinterpret_cast<QuadVertex*>(mapped.pData);
            vertices[0] = { toNdcX(tlx), toNdcY(tly), 0.0f, 0.0f };
            vertices[1] = { toNdcX(trx), toNdcY(try_), uMax, 0.0f };
            vertices[2] = { toNdcX(blx), toNdcY(bly), 0.0f, vMax };
            vertices[3] = { toNdcX(brx), toNdcY(bry), uMax, vMax };
            context->Unmap(g_quad.vb.Get(), 0);

            UINT stride = sizeof(QuadVertex);
            UINT offset = 0;
            context->IASetInputLayout(g_quad.inputLayout.Get());
            context->IASetVertexBuffers(0, 1, g_quad.vb.GetAddressOf(), &stride, &offset);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            context->VSSetShader(g_quad.vs.Get(), nullptr, 0);
            context->PSSetShader(shader, nullptr, 0);
            context->PSSetSamplers(0, 1, &sampler);
            context->PSSetShaderResources(0, 1, &source);

            float blendFactor[4] = {};
            context->OMSetBlendState(g_quad.blend.Get(), blendFactor, 0xFFFFFFFF);

            D3D11_MAPPED_SUBRESOURCE mappedCb {};
            mapHr = context->Map(g_quad.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedCb);
            if (FAILED(mapHr))
            {
                return mapHr;
            }
            float* constants = reinterpret_cast<float*>(mappedCb.pData);
            constants[0] = 1.0f;
            constants[1] = channelMode;
            constants[2] = encoding;
            constants[3] = usage;
            context->Unmap(g_quad.cb.Get(), 0);

            ID3D11Buffer* cb = g_quad.cb.Get();
            context->PSSetConstantBuffers(0, 1, &cb);
            context->Draw(4, 0);

            ID3D11ShaderResourceView* nullSrv = nullptr;
            context->PSSetShaderResources(0, 1, &nullSrv);
            return S_OK;
        };

        const D2D1_RECT_F dest = Util::ComputeAspectFitRect(
            layout,
            D2D1::SizeF(
                static_cast<float>(draw.contentWidth),
                static_cast<float>(draw.contentHeight)),
            NormalizeRotationQuarters(draw.rotationQuarters));

        HRESULT drawHr = S_OK;
        if (draw.alphaCheckerboardEnabled)
        {
            const float width = (std::max)(1.0f, dest.right - dest.left);
            const float height = (std::max)(1.0f, dest.bottom - dest.top);
            drawHr = drawRect(
                g_quad.checkerSrv.Get(),
                dest,
                width / 64.0f,
                height / 64.0f,
                g_quad.samplerWrap.Get(),
                g_quad.ps.Get(),
                0.0f,
                0.0f,
                0.0f);
        }

        ID3D11SamplerState* sampler = draw.highQualitySampling
            ? g_quad.samplerLinear.Get()
            : g_quad.samplerPoint.Get();
        if (SUCCEEDED(drawHr))
        {
            drawHr = drawRect(
                srv,
                dest,
                1.0f,
                1.0f,
                sampler,
                shader,
                static_cast<float>(draw.channelMode),
                static_cast<float>(draw.sourceAlphaEncoding),
                static_cast<float>(draw.sourceAlphaUsage));
        }

        restoreState();
        return drawHr;
    }

    Image::Image()
        : Wnd()
    {
    }

    Image::Image(const std::wstring& name)
        : Wnd(name)
    {
    }

    void Image::ResetCheckerBrushes()
    {
        m_checkerLightBrush.Reset();
        m_checkerDarkBrush.Reset();
    }

    void Image::SetBitmap(Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap)
    {
        const bool changed =
            (m_bitmap.Get() != bitmap.Get()) ||
            (m_srv != nullptr);

        m_bitmap = std::move(bitmap);
        m_srv.Reset();
        m_srvWidth = 0;
        m_srvHeight = 0;

        if (changed)
        {
            Invalidate();
        }
    }

    void Image::SetShaderResource(Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv)
    {
        UINT w = 0;
        UINT h = 0;
        if (srv && !TryDiscoverSrvTexelSize(srv.Get(), w, h))
        {
            // Texture2D SRV only ??reject other view dimensions.
            srv.Reset();
            w = 0;
            h = 0;
        }

        const bool changed =
            (m_srv.Get() != srv.Get()) ||
            (m_bitmap != nullptr) ||
            (m_srvWidth != w) ||
            (m_srvHeight != h);

        m_srv = std::move(srv);
        m_srvWidth = w;
        m_srvHeight = h;
        m_bitmap.Reset();

        if (changed)
        {
            Invalidate();
        }
    }

    void Image::Clear()
    {
        const bool hadContent = (m_bitmap != nullptr) || (m_srv != nullptr);
        m_bitmap.Reset();
        m_srv.Reset();
        m_srvWidth = 0;
        m_srvHeight = 0;
        if (hadContent)
        {
            Invalidate();
        }
    }

    void Image::SetDrawState(const DrawState& state)
    {
        DrawState next = state;
        next.rotationQuarters = NormalizeRotationQuarters(next.rotationQuarters);

        const bool changed =
            m_drawState.zoomScale != next.zoomScale ||
            m_drawState.panX != next.panX ||
            m_drawState.panY != next.panY ||
            m_drawState.rotationQuarters != next.rotationQuarters ||
            m_drawState.highQualitySampling != next.highQualitySampling ||
            m_drawState.alphaCheckerboardEnabled != next.alphaCheckerboardEnabled ||
            m_drawState.channelMode != next.channelMode ||
            m_drawState.sourceAlphaEncoding != next.sourceAlphaEncoding ||
            m_drawState.sourceAlphaUsage != next.sourceAlphaUsage;

        m_drawState = next;
        if (changed)
        {
            Invalidate();
        }
    }

    Image::DrawState Image::GetDrawState() const
    {
        return m_drawState;
    }

    D2D1_SIZE_U Image::ContentPixelSize() const
    {
        if (m_bitmap)
        {
            return m_bitmap->GetPixelSize();
        }
        if (m_srv && m_srvWidth > 0 && m_srvHeight > 0)
        {
            return { m_srvWidth, m_srvHeight };
        }
        return { 0, 0 };
    }

    bool Image::TryGetContentSize(D2D1_SIZE_F& outSize) const
    {
        if (m_bitmap)
        {
            outSize = m_bitmap->GetSize();
            return outSize.width > 0.0f && outSize.height > 0.0f;
        }
        if (m_srv && m_srvWidth > 0 && m_srvHeight > 0)
        {
            outSize = { static_cast<float>(m_srvWidth), static_cast<float>(m_srvHeight) };
            return true;
        }
        return false;
    }

    void Image::OnGraphicsInvalidated(GraphicsInvalidationReason reason, const GraphicsGeneration& generation)
    {
        switch (reason)
        {
        case GraphicsInvalidationReason::TargetRecreated:
            // D2D bitmaps/brushes are target-bound; SRVs on the same D3D device remain valid.
            m_bitmap.Reset();
            ResetCheckerBrushes();
            Invalidate();
            break;

        case GraphicsInvalidationReason::DeviceLost:
        case GraphicsInvalidationReason::RendererFallback:
            m_bitmap.Reset();
            m_srv.Reset();
            m_srvWidth = 0;
            m_srvHeight = 0;
            ResetCheckerBrushes();
            ResetD3DQuadResources();
            Invalidate();
            break;

        case GraphicsInvalidationReason::Shutdown:
            // Teardown only - do not Invalidate/Render; the HWND may already be gone.
            m_bitmap.Reset();
            m_srv.Reset();
            m_srvWidth = 0;
            m_srvHeight = 0;
            ResetCheckerBrushes();
            ResetD3DQuadResources();
            break;

        case GraphicsInvalidationReason::Resize:
            // Simple swapchain resize keeps the same device resources.
            break;
        }

        Wnd::OnGraphicsInvalidated(reason, generation);
    }

    void Image::OnRender(ID2D1RenderTarget* target)
    {
        if (target == nullptr)
        {
            return;
        }

        const D2D1_RECT_F clipRect = LayoutRect();
        target->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        if (m_bitmap)
        {
            D2D1_SIZE_F contentSize {};
            if (TryGetContentSize(contentSize))
            {
                const D2D1_RECT_F layoutRect = LayoutRect();
                D2D1_RECT_F destRect = Util::ComputeAspectFitRect(
                    layoutRect,
                    contentSize,
                    m_drawState.rotationQuarters);
                destRect = Util::ApplyZoomPanToRect(
                    destRect,
                    m_drawState.zoomScale,
                    m_drawState.panX,
                    m_drawState.panY);

                if (m_drawState.rotationQuarters != 0)
                {
                    const float cx = (layoutRect.left + layoutRect.right) * 0.5f;
                    const float cy = (layoutRect.top + layoutRect.bottom) * 0.5f;
                    target->SetTransform(D2D1::Matrix3x2F::Rotation(
                        static_cast<float>(m_drawState.rotationQuarters) * 90.0f,
                        D2D1::Point2F(cx, cy)));
                }

                if (m_drawState.alphaCheckerboardEnabled)
                {
                    if (!m_checkerLightBrush || !m_checkerDarkBrush)
                    {
                        (void)target->CreateSolidColorBrush(
                            D2D1::ColorF(0.94f, 0.94f, 0.94f, 1.0f), &m_checkerLightBrush);
                        (void)target->CreateSolidColorBrush(
                            D2D1::ColorF(0.44f, 0.44f, 0.44f, 1.0f), &m_checkerDarkBrush);
                    }

                    if (m_checkerLightBrush && m_checkerDarkBrush)
                    {
                        constexpr float tile = 8.0f;
                        const float startX = std::floor(destRect.left / tile) * tile;
                        const float startY = std::floor(destRect.top / tile) * tile;
                        for (float y = startY; y < destRect.bottom; y += tile)
                        {
                            for (float x = startX; x < destRect.right; x += tile)
                            {
                                const int ix = static_cast<int>(std::floor((x - startX) / tile));
                                const int iy = static_cast<int>(std::floor((y - startY) / tile));
                                const bool dark = (((ix + iy) & 1) != 0);
                                const D2D1_RECT_F r { x, y, x + tile, y + tile };
                                target->FillRectangle(
                                    r,
                                    dark ? m_checkerDarkBrush.Get() : m_checkerLightBrush.Get());
                            }
                        }
                    }
                }

                const D2D1_RECT_F sourceRect = D2D1::RectF(0.0f, 0.0f, contentSize.width, contentSize.height);
                D2D1_BITMAP_INTERPOLATION_MODE interpMode = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
                bool drawn = false;

                const FD2D::D2DVersion d2dVersion = FD2D::Core::GetSupportedD2DVersion();
                if (m_drawState.highQualitySampling)
                {
                    if (d2dVersion >= FD2D::D2DVersion::D2D1_1)
                    {
                        Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
                        if (SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(&dc))) && dc)
                        {
                            dc->DrawBitmap(
                                m_bitmap.Get(),
                                destRect,
                                1.0f,
                                D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                                sourceRect);
                            drawn = true;
                        }
                    }
                }
                else if (d2dVersion >= FD2D::D2DVersion::D2D1_1)
                {
                    interpMode = D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
                }

                if (!drawn)
                {
                    target->DrawBitmap(
                        m_bitmap.Get(),
                        destRect,
                        1.0f,
                        interpMode,
                        sourceRect);
                }

                if (m_drawState.rotationQuarters != 0)
                {
                    target->SetTransform(D2D1::Matrix3x2F::Identity());
                }
            }
        }

        Wnd::OnRender(target);
        target->PopAxisAlignedClip();
    }

    void Image::OnRenderD3D(ID3D11DeviceContext* context)
    {
        if (context && m_backplate && m_srv && m_srvWidth > 0 && m_srvHeight > 0)
        {
            ShaderResourceDraw draw;
            draw.layout = LayoutRect();
            draw.contentWidth = m_srvWidth;
            draw.contentHeight = m_srvHeight;
            draw.zoomScale = m_drawState.zoomScale;
            draw.panX = m_drawState.panX;
            draw.panY = m_drawState.panY;
            draw.rotationQuarters = m_drawState.rotationQuarters;
            draw.highQualitySampling = m_drawState.highQualitySampling;
            draw.alphaCheckerboardEnabled = m_drawState.alphaCheckerboardEnabled;
            draw.channelMode = m_drawState.channelMode;
            draw.sourceAlphaEncoding = m_drawState.sourceAlphaEncoding;
            draw.sourceAlphaUsage = m_drawState.sourceAlphaUsage;
            (void)DrawShaderResource(context, *m_backplate, m_srv.Get(), draw);
        }
        Wnd::OnRenderD3D(context);
    }
}
