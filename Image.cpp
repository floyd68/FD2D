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

        // Quad pipeline for one D3D device. Stored in the owning backplate's
        // device cache (Backplate::DeviceCacheEntry), which is cleared whenever
        // that backplate discards its device: each backplate/device keeps its
        // own set, and none outlives or crosses to another device.
        struct D3DQuadResources
        {
            Microsoft::WRL::ComPtr<ID3D11Device> device {};
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
            Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthDisabled {};
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> checkerSrv {};
        };

        // Address-unique key for this presenter's entry in the backplate device cache.
        static const char kQuadCacheKey = 0;

        static D3DQuadResources* EnsureD3DQuadResources(Backplate& backplate, ID3D11Device* device, HRESULT& outHr)
        {
            outHr = S_OK;
            if (!device)
            {
                outHr = E_INVALIDARG;
                return nullptr;
            }

            auto cached = std::static_pointer_cast<D3DQuadResources>(backplate.DeviceCacheEntry(&kQuadCacheKey));
            if (cached && cached->device.Get() == device)
            {
                return cached.get();
            }

            auto created = std::make_shared<D3DQuadResources>();
            D3DQuadResources& quad = *created;
            quad.device = device;

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
                outHr = hr;
                return nullptr;
            }
            hr = D3DCompile(psSrc, strlen(psSrc), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psBlob, &err);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
            }
            hr = D3DCompile(psArraySrc, strlen(psArraySrc), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psArrayBlob, &err);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
            }
            hr = D3DCompile(psCubeSrc, strlen(psCubeSrc), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psCubeBlob, &err);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
            }

            hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &quad.vs);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
            }
            hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &quad.ps);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
            }
            hr = device->CreatePixelShader(
                psArrayBlob->GetBufferPointer(), psArrayBlob->GetBufferSize(), nullptr, &quad.psArray);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
            }
            hr = device->CreatePixelShader(
                psCubeBlob->GetBufferPointer(), psCubeBlob->GetBufferSize(), nullptr, &quad.psCube);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
            }

            const D3D11_INPUT_ELEMENT_DESC il[] =
            {
                { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            hr = device->CreateInputLayout(il, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &quad.inputLayout);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
            }

            D3D11_BUFFER_DESC bd {};
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.ByteWidth = sizeof(QuadVertex) * 4;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            hr = device->CreateBuffer(&bd, nullptr, &quad.vb);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
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
            hr = device->CreateSamplerState(&sdPoint, &quad.samplerPoint);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
            }

            D3D11_SAMPLER_DESC sdLinear = sdPoint;
            sdLinear.Filter = D3D11_FILTER_ANISOTROPIC;
            sdLinear.MaxAnisotropy = 16;
            hr = device->CreateSamplerState(&sdLinear, &quad.samplerLinear);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
            }

            D3D11_SAMPLER_DESC sdWrap = sdPoint;
            sdWrap.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
            sdWrap.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
            sdWrap.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            hr = device->CreateSamplerState(&sdWrap, &quad.samplerWrap);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
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
            hr = device->CreateBlendState(&blend, &quad.blend);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
            }

            D3D11_RASTERIZER_DESC rd {};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            rd.ScissorEnable = TRUE;
            hr = device->CreateRasterizerState(&rd, &quad.rsScissor);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
            }

            D3D11_DEPTH_STENCIL_DESC depth {};
            depth.DepthEnable = FALSE;
            depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
            hr = device->CreateDepthStencilState(
                &depth,
                &quad.depthDisabled);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
            }

            D3D11_BUFFER_DESC cbd {};
            cbd.Usage = D3D11_USAGE_DYNAMIC;
            cbd.ByteWidth = 16;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            hr = device->CreateBuffer(&cbd, nullptr, &quad.cb);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
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
                outHr = hr;
                return nullptr;
            }

            hr = device->CreateShaderResourceView(tex.Get(), nullptr, &quad.checkerSrv);
            if (FAILED(hr))
            {
                outHr = hr;
                return nullptr;
            }

            backplate.SetDeviceCacheEntry(&kQuadCacheKey, created);
            return created.get();
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
        ID3D11RenderTargetView* destination =
            backplate.ActiveD3DRenderTarget();
        if (!destination)
        {
            return E_FAIL;
        }

        HRESULT hr = S_OK;
        D3DQuadResources* quadResources = EnsureD3DQuadResources(backplate, device, hr);
        if (!quadResources)
        {
            return FAILED(hr) ? hr : E_FAIL;
        }
        D3DQuadResources& quad = *quadResources;

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
            shader = quad.ps.Get();
            break;

        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
            shader = quad.psArray.Get();
            break;

        case D3D11_SRV_DIMENSION_TEXTURECUBE:
            shader = quad.psCube.Get();
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

        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> prevRenderTarget;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> prevDepthView;
        context->OMGetRenderTargets(
            1,
            &prevRenderTarget,
            &prevDepthView);
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> prevDepthState;
        UINT prevStencilReference = 0;
        context->OMGetDepthStencilState(
            &prevDepthState,
            &prevStencilReference);

        UINT prevViewportCount = 0;
        context->RSGetViewports(
            &prevViewportCount,
            nullptr);
        std::vector<D3D11_VIEWPORT> prevViewports(
            prevViewportCount);
        if (prevViewportCount > 0)
        {
            context->RSGetViewports(
                &prevViewportCount,
                prevViewports.data());
        }

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
            ID3D11RenderTargetView* renderTarget =
                prevRenderTarget.Get();
            context->OMSetRenderTargets(
                1,
                &renderTarget,
                prevDepthView.Get());
            context->OMSetDepthStencilState(
                prevDepthState.Get(),
                prevStencilReference);
            context->RSSetViewports(
                prevViewportCount,
                prevViewportCount > 0
                    ? prevViewports.data()
                    : nullptr);
            context->RSSetScissorRects(
                prevScissorCount,
                prevScissorCount > 0 ? prevScissors.data() : nullptr);
            context->RSSetState(prevRs.Get());
        };

        context->OMSetRenderTargets(
            1,
            &destination,
            nullptr);
        context->OMSetDepthStencilState(
            quad.depthDisabled.Get(),
            0);
        const D3D11_VIEWPORT presentationViewport
        {
            0.0f,
            0.0f,
            static_cast<float>(cs.width),
            static_cast<float>(cs.height),
            0.0f,
            1.0f
        };
        context->RSSetViewports(
            1,
            &presentationViewport);
        context->RSSetState(quad.rsScissor.Get());
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
            HRESULT mapHr = context->Map(quad.vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(mapHr))
            {
                return mapHr;
            }
            auto* vertices = reinterpret_cast<QuadVertex*>(mapped.pData);
            vertices[0] = { toNdcX(tlx), toNdcY(tly), 0.0f, 0.0f };
            vertices[1] = { toNdcX(trx), toNdcY(try_), uMax, 0.0f };
            vertices[2] = { toNdcX(blx), toNdcY(bly), 0.0f, vMax };
            vertices[3] = { toNdcX(brx), toNdcY(bry), uMax, vMax };
            context->Unmap(quad.vb.Get(), 0);

            UINT stride = sizeof(QuadVertex);
            UINT offset = 0;
            context->IASetInputLayout(quad.inputLayout.Get());
            context->IASetVertexBuffers(0, 1, quad.vb.GetAddressOf(), &stride, &offset);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            context->VSSetShader(quad.vs.Get(), nullptr, 0);
            context->PSSetShader(shader, nullptr, 0);
            context->PSSetSamplers(0, 1, &sampler);
            context->PSSetShaderResources(0, 1, &source);

            float blendFactor[4] = {};
            context->OMSetBlendState(quad.blend.Get(), blendFactor, 0xFFFFFFFF);

            D3D11_MAPPED_SUBRESOURCE mappedCb {};
            mapHr = context->Map(quad.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedCb);
            if (FAILED(mapHr))
            {
                return mapHr;
            }
            float* constants = reinterpret_cast<float*>(mappedCb.pData);
            constants[0] = 1.0f;
            constants[1] = channelMode;
            constants[2] = encoding;
            constants[3] = usage;
            context->Unmap(quad.cb.Get(), 0);

            ID3D11Buffer* cb = quad.cb.Get();
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
                quad.checkerSrv.Get(),
                dest,
                width / 64.0f,
                height / 64.0f,
                quad.samplerWrap.Get(),
                quad.ps.Get(),
                0.0f,
                0.0f,
                0.0f);
        }

        ID3D11SamplerState* sampler = draw.highQualitySampling
            ? quad.samplerLinear.Get()
            : quad.samplerPoint.Get();
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
        m_checkerBrush.Reset();
    }

    namespace
    {
        constexpr float kCheckerTile = 8.0f;
        // Light and dark checkerboard tiles (opaque gray levels 0.94 and 0.44).
        constexpr float kCheckerLight = 0.94f;
        constexpr float kCheckerDark = 0.44f;

        uint32_t OpaqueGrayBgra(float level)
        {
            const uint32_t v = static_cast<uint32_t>(level * 255.0f + 0.5f);
            return 0xFF000000u | (v << 16) | (v << 8) | v;
        }
    }

    bool Image::EnsureCheckerBrush(ID2D1RenderTarget* target)
    {
        if (RefreshTargetResourceStamp(m_checkerStamp))
        {
            ResetCheckerBrushes();
        }
        if (m_checkerBrush)
        {
            return true;
        }

        // One pattern period: 2 x 2 tiles of 8 x 8 pixels at 96 DPI (so 16 x 16
        // DIPs at any target DPI), light at (0, 0) and (1, 1).
        constexpr UINT kTile = static_cast<UINT>(kCheckerTile);
        constexpr UINT kPeriod = kTile * 2u;
        const uint32_t light = OpaqueGrayBgra(kCheckerLight);
        const uint32_t dark = OpaqueGrayBgra(kCheckerDark);
        std::vector<uint32_t> pixels(static_cast<size_t>(kPeriod) * kPeriod);
        for (UINT y = 0; y < kPeriod; ++y)
        {
            for (UINT x = 0; x < kPeriod; ++x)
            {
                const bool isDark = (((x / kTile) + (y / kTile)) & 1u) != 0;
                pixels[static_cast<size_t>(y) * kPeriod + x] = isDark ? dark : light;
            }
        }

        Microsoft::WRL::ComPtr<ID2D1Bitmap> pattern;
        const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f,
            96.0f);
        if (FAILED(target->CreateBitmap(
                D2D1::SizeU(kPeriod, kPeriod), pixels.data(), kPeriod * 4u, &props, &pattern)) ||
            !pattern)
        {
            return false;
        }
        // Linear sampling: one texel per pixel, so a tile edge that lands
        // between pixel centers (a rotation around a center off the pixel
        // grid, fractional layout or pan) gives the area-weighted mix of the
        // two tiles, as the antialiased per-tile fills did, instead of
        // snapping the edge to a whole pixel. Edges on the pixel grid stay
        // crisp.
        (void)target->CreateBitmapBrush(
            pattern.Get(),
            D2D1::BitmapBrushProperties(
                D2D1_EXTEND_MODE_WRAP,
                D2D1_EXTEND_MODE_WRAP,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
            &m_checkerBrush);
        return m_checkerBrush != nullptr;
    }

    void Image::SetBitmap(Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap)
    {
        const bool changed =
            (m_bitmap.Get() != bitmap.Get()) ||
            (m_srv != nullptr);

        m_bitmap = std::move(bitmap);
        m_bitmapStamp = m_bitmap ? CurrentTargetStamp() : GraphicsResourceStamp {};
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
        m_srvStamp = m_srv ? CurrentDeviceStamp() : GraphicsResourceStamp {};
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

    bool Image::DropStaleContent()
    {
        bool dropped = false;
        if (m_bitmap && !(m_bitmapStamp == CurrentTargetStamp()))
        {
            m_bitmap.Reset();
            dropped = true;
        }
        if (m_srv && !(m_srvStamp == CurrentDeviceStamp()))
        {
            m_srv.Reset();
            m_srvWidth = 0;
            m_srvHeight = 0;
            dropped = true;
        }
        return dropped;
    }

    void Image::OnGraphicsInvalidated(GraphicsInvalidationReason reason, const GraphicsGeneration& generation)
    {
        if (reason == GraphicsInvalidationReason::Shutdown)
        {
            // Teardown only - do not Invalidate/Render; the HWND may already be gone.
            m_bitmap.Reset();
            m_srv.Reset();
            m_srvWidth = 0;
            m_srvHeight = 0;
            ResetCheckerBrushes();
        }
        else if (DropStaleContent())
        {
            // The generations decide: a target-only recreation keeps the SRV,
            // a device change drops it. The owner re-supplies content.
            Invalidate();
        }

        Wnd::OnGraphicsInvalidated(reason, generation);
    }

    void Image::OnRender(ID2D1RenderTarget* target)
    {
        if (target == nullptr)
        {
            return;
        }

        // Never draw content from a previous target or device.
        (void)DropStaleContent();

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

                // The incoming transform carries the parents' placement (scroll
                // offsets, translations) and the backplate's logical-to-render
                // scale during live resize. Rotation is local to this image, so it
                // is applied first and the incoming transform after it (row-vector
                // convention: point * Rotation * incoming). The exact incoming
                // transform is restored after drawing.
                D2D1::Matrix3x2F incomingTransform;
                target->GetTransform(&incomingTransform);
                if (m_drawState.rotationQuarters != 0)
                {
                    const float cx = (layoutRect.left + layoutRect.right) * 0.5f;
                    const float cy = (layoutRect.top + layoutRect.bottom) * 0.5f;
                    const D2D1::Matrix3x2F rotation = D2D1::Matrix3x2F::Rotation(
                        static_cast<float>(m_drawState.rotationQuarters) * 90.0f,
                        D2D1::Point2F(cx, cy));
                    target->SetTransform(rotation * incomingTransform);
                }

                if (m_drawState.alphaCheckerboardEnabled)
                {
                    // Only the tiles inside the clip: the clip (pushed in the
                    // incoming frame) rotated back into this pre-rotation frame,
                    // where destRect (with zoom and pan) lives. One fill with the
                    // wrap-sampled pattern brush, anchored in phase with the
                    // tile grid origin, draws all of them.
                    const D2D1_RECT_F visibleRect = Util::MapRectToPreRotationFrame(
                        clipRect,
                        layoutRect,
                        m_drawState.rotationQuarters);
                    const Util::CheckerboardTiles tiles =
                        Util::ComputeCheckerboardTiles(destRect, visibleRect, kCheckerTile);
                    if (!tiles.Empty() && EnsureCheckerBrush(target))
                    {
                        const D2D1_POINT_2F origin = tiles.PatternOrigin();
                        m_checkerBrush->SetTransform(D2D1::Matrix3x2F::Translation(origin.x, origin.y));
                        target->FillRectangle(tiles.Bounds(), m_checkerBrush.Get());
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
                    target->SetTransform(incomingTransform);
                }
            }
        }

        Wnd::OnRender(target);
        target->PopAxisAlignedClip();
    }

    void Image::OnRenderD3D(ID3D11DeviceContext* context)
    {
        (void)DropStaleContent();
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
