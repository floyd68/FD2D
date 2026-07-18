#include "Image.h"
#include "Backplate.h"
#include "Core.h"
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
            if (g_quad.vs && g_quad.ps && g_quad.inputLayout && g_quad.vb &&
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
            // 1=R, 2=G, 3=B, 4=A. Alpha is forced opaque for isolated channels
            // so e.g. a packed _rmaos channel is readable on its own. The blend
            // state is premultiplied (ONE / INV_SRC_ALPHA), so:
            //  - normal display (mode 0): a STRAIGHT source is premultiplied here
            //    (rgb*=a) so it composites correctly over the checkerboard; a
            //    premultiplied source is emitted as-is.
            //  - color isolation (1/2/3): a PREMULTIPLIED source is unpremultiplied
            //    (rgb/a) so it reads the straight channel value, matching straight
            //    BCn. `premultiplied` (1/0) comes from the source's real alpha mode.
            const char* psSrc =
                "Texture2D tex0 : register(t0);"
                "SamplerState samp0 : register(s0);"
                "cbuffer Cb : register(b0) { float opacity; float channelMode; float premultiplied; float pad; };"
                "float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target {"
                "  float4 c = tex0.Sample(samp0, uv);"
                "  int m = (int)channelMode;"
                "  if (m == 0) {"
                "    if (premultiplied < 0.5) c.rgb *= c.a;"   // straight -> premultiply for the premultiplied blend
                "  }"
                "  else if (m >= 1 && m <= 3) {"
                "    float3 rgb = c.rgb;"
                "    if (premultiplied > 0.5 && c.a > 0.0) rgb /= c.a;"  // premultiplied -> straight channel value
                "    if (m == 1) c = float4(rgb.rrr, 1);"
                "    else if (m == 2) c = float4(rgb.ggg, 1);"
                "    else c = float4(rgb.bbb, 1);"
                "  }"
                "  else if (m == 4) c = float4(c.aaa, 1);"
                "  c.a *= opacity;"
                "  c.rgb *= opacity;"
                "  return c;"
                "}";

            Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
            Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
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
                svd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
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
            else
            {
                mip = svd.Texture2DArray.MostDetailedMip;
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
            m_drawState.channelMode != next.channelMode;

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
        if (!context || !m_backplate || !m_srv || m_srvWidth == 0 || m_srvHeight == 0)
        {
            Wnd::OnRenderD3D(context);
            return;
        }

        ID3D11Device* device = m_backplate->D3DDevice();
        if (!device)
        {
            Wnd::OnRenderD3D(context);
            return;
        }

        const uint64_t deviceGeneration = m_backplate->GetGraphicsGeneration().device;
        if (FAILED(EnsureD3DQuadResources(device, deviceGeneration)))
        {
            Wnd::OnRenderD3D(context);
            return;
        }

        const D2D1_SIZE_U logicalCs = m_backplate->ClientSize();
        D2D1_SIZE_U cs = m_backplate->RenderSurfaceSize();
        if (cs.width == 0 || cs.height == 0)
        {
            cs = logicalCs;
        }
        if (cs.width == 0 || cs.height == 0 || logicalCs.width == 0 || logicalCs.height == 0)
        {
            Wnd::OnRenderD3D(context);
            return;
        }

        const D2D1_SIZE_F logicalToRender = m_backplate->LogicalToRenderScale();
        const D2D1_RECT_F rawLayout = LayoutRect();
        D2D1_RECT_F layout
        {
            rawLayout.left * logicalToRender.width,
            rawLayout.top * logicalToRender.height,
            rawLayout.right * logicalToRender.width,
            rawLayout.bottom * logicalToRender.height
        };
        const float layoutW = layout.right - layout.left;
        const float layoutH = layout.bottom - layout.top;
        if (!(layoutW > 0.0f && layoutH > 0.0f))
        {
            Wnd::OnRenderD3D(context);
            return;
        }

        D3D11_RECT scissor {};
        scissor.left = static_cast<LONG>(std::floor(layout.left));
        scissor.top = static_cast<LONG>(std::floor(layout.top));
        scissor.right = static_cast<LONG>(std::ceil(layout.right));
        scissor.bottom = static_cast<LONG>(std::ceil(layout.bottom));
        scissor.left = (std::max)(0L, (std::min)(scissor.left, static_cast<LONG>(cs.width)));
        scissor.top = (std::max)(0L, (std::min)(scissor.top, static_cast<LONG>(cs.height)));
        scissor.right = (std::max)(0L, (std::min)(scissor.right, static_cast<LONG>(cs.width)));
        scissor.bottom = (std::max)(0L, (std::min)(scissor.bottom, static_cast<LONG>(cs.height)));
        if (scissor.left >= scissor.right || scissor.top >= scissor.bottom)
        {
            Wnd::OnRenderD3D(context);
            return;
        }

        Microsoft::WRL::ComPtr<ID3D11RasterizerState> prevRs {};
        context->RSGetState(&prevRs);

        UINT prevScissorCount = 0;
        context->RSGetScissorRects(&prevScissorCount, nullptr);
        std::vector<D3D11_RECT> prevScissors;
        if (prevScissorCount > 0)
        {
            prevScissors.resize(prevScissorCount);
            context->RSGetScissorRects(&prevScissorCount, prevScissors.data());
        }

        if (g_quad.rsScissor)
        {
            context->RSSetState(g_quad.rsScissor.Get());
        }
        context->RSSetScissorRects(1, &scissor);

        const auto toNdcX = [cs](float x) { return (x / static_cast<float>(cs.width)) * 2.0f - 1.0f; };
        const auto toNdcY = [cs](float y) { return 1.0f - (y / static_cast<float>(cs.height)) * 2.0f; };

        const auto drawSrvRect = [&](
            ID3D11ShaderResourceView* srv,
            const D2D1_RECT_F& rectPx,
            float opacity,
            float uMax,
            float vMax,
            ID3D11SamplerState* samplerOverride,
            float channelMode = 0.0f,
            float premultiplied = 0.0f)
        {
            if (!srv || opacity <= 0.0f)
            {
                return;
            }

            const D2D1_RECT_F zoomedRect = Util::ApplyZoomPanToRect(
                rectPx,
                m_drawState.zoomScale,
                m_drawState.panX * logicalToRender.width,
                m_drawState.panY * logicalToRender.height);

            const float cxPx = (layout.left + layout.right) * 0.5f;
            const float cyPx = (layout.top + layout.bottom) * 0.5f;
            const int q = m_drawState.rotationQuarters;

            const auto rotPt = [cxPx, cyPx, q](float px, float py, float& rx, float& ry)
            {
                const float dx = px - cxPx;
                const float dy = py - cyPx;
                switch (q)
                {
                case 1: rx = cxPx - dy; ry = cyPx + dx; break;
                case 2: rx = cxPx - dx; ry = cyPx - dy; break;
                case 3: rx = cxPx + dy; ry = cyPx - dx; break;
                default: rx = px;       ry = py;        break;
                }
            };

            float tlx, tly, trx, trry, blx, bly, brx, bry;
            rotPt(zoomedRect.left,  zoomedRect.top,    tlx, tly);
            rotPt(zoomedRect.right, zoomedRect.top,    trx, trry);
            rotPt(zoomedRect.left,  zoomedRect.bottom, blx, bly);
            rotPt(zoomedRect.right, zoomedRect.bottom, brx, bry);

            D3D11_MAPPED_SUBRESOURCE mapped {};
            if (SUCCEEDED(context->Map(g_quad.vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                auto* v = reinterpret_cast<QuadVertex*>(mapped.pData);
                v[0] = { toNdcX(tlx), toNdcY(tly),  0.0f, 0.0f };
                v[1] = { toNdcX(trx), toNdcY(trry), uMax, 0.0f };
                v[2] = { toNdcX(blx), toNdcY(bly),  0.0f, vMax };
                v[3] = { toNdcX(brx), toNdcY(bry),  uMax, vMax };
                context->Unmap(g_quad.vb.Get(), 0);
            }

            UINT stride = sizeof(QuadVertex);
            UINT offset = 0;
            context->IASetInputLayout(g_quad.inputLayout.Get());
            context->IASetVertexBuffers(0, 1, g_quad.vb.GetAddressOf(), &stride, &offset);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            context->VSSetShader(g_quad.vs.Get(), nullptr, 0);
            context->PSSetShader(g_quad.ps.Get(), nullptr, 0);

            ID3D11SamplerState* samp = samplerOverride
                ? samplerOverride
                : (m_drawState.highQualitySampling ? g_quad.samplerLinear.Get() : g_quad.samplerPoint.Get());
            context->PSSetSamplers(0, 1, &samp);
            context->PSSetShaderResources(0, 1, &srv);

            float blendFactor[4] = { 0, 0, 0, 0 };
            context->OMSetBlendState(g_quad.blend.Get(), blendFactor, 0xFFFFFFFF);

            if (g_quad.cb)
            {
                D3D11_MAPPED_SUBRESOURCE mappedCb {};
                if (SUCCEEDED(context->Map(g_quad.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedCb)))
                {
                    float* p = reinterpret_cast<float*>(mappedCb.pData);
                    p[0] = opacity;
                    p[1] = channelMode;
                    p[2] = premultiplied;
                    p[3] = 0.0f;
                    context->Unmap(g_quad.cb.Get(), 0);
                }
                ID3D11Buffer* cbp = g_quad.cb.Get();
                context->PSSetConstantBuffers(0, 1, &cbp);
            }

            context->Draw(4, 0);

            ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
            context->PSSetShaderResources(0, 1, nullSrv);
        };

        const D2D1_RECT_F dest = Util::ComputeAspectFitRect(
            layout,
            D2D1::SizeF(static_cast<float>(m_srvWidth), static_cast<float>(m_srvHeight)),
            m_drawState.rotationQuarters);

        if (m_drawState.alphaCheckerboardEnabled && g_quad.checkerSrv && g_quad.samplerWrap)
        {
            const float w = (std::max)(1.0f, dest.right - dest.left);
            const float h = (std::max)(1.0f, dest.bottom - dest.top);
            drawSrvRect(
                g_quad.checkerSrv.Get(),
                dest,
                1.0f,
                w / 64.0f,
                h / 64.0f,
                g_quad.samplerWrap.Get());
        }

        drawSrvRect(m_srv.Get(), dest, 1.0f, 1.0f, 1.0f, nullptr,
                    static_cast<float>(m_drawState.channelMode),
                    m_drawState.sourcePremultiplied ? 1.0f : 0.0f);

        if (prevScissorCount > 0 && !prevScissors.empty())
        {
            context->RSSetScissorRects(prevScissorCount, prevScissors.data());
        }
        else
        {
            D3D11_RECT full { 0, 0, static_cast<LONG>(cs.width), static_cast<LONG>(cs.height) };
            context->RSSetScissorRects(1, &full);
        }
        context->RSSetState(prevRs.Get());

        Wnd::OnRenderD3D(context);
    }
}
