#include "Image.h"
#include "Backplate.h"
#include "Spinner.h"
#include "Core.h"  // For GetSupportedD2DVersion
#include "../CommonUtil.h"
#include "../ImageCore/ImageRequest.h"
#include <algorithm>
#include <cmath>
#include <d3dcompiler.h>
#include <d2d1_1.h>  // For Direct2D 1.1 interpolation modes
#include <vector>

namespace FD2D
{
    namespace
    {
        static void LogImageHr(const wchar_t* stage, const std::wstring& path, HRESULT hr)
        {
            if (SUCCEEDED(hr))
            {
                return;
            }

            wchar_t buf[512] {};
            if (!path.empty())
            {
                swprintf_s(buf, L"[Image] %s failed (%s): 0x%08X\n", stage, path.c_str(), static_cast<unsigned>(hr));
            }
            else
            {
                swprintf_s(buf, L"[Image] %s failed: 0x%08X\n", stage, static_cast<unsigned>(hr));
            }
            OutputDebugStringW(buf);
        }

        static bool IsCompressedDxgiFormat(DXGI_FORMAT fmt)
        {
            switch (fmt)
            {
            case DXGI_FORMAT_BC1_TYPELESS:
            case DXGI_FORMAT_BC1_UNORM:
            case DXGI_FORMAT_BC1_UNORM_SRGB:
            case DXGI_FORMAT_BC2_TYPELESS:
            case DXGI_FORMAT_BC2_UNORM:
            case DXGI_FORMAT_BC2_UNORM_SRGB:
            case DXGI_FORMAT_BC3_TYPELESS:
            case DXGI_FORMAT_BC3_UNORM:
            case DXGI_FORMAT_BC3_UNORM_SRGB:
            case DXGI_FORMAT_BC4_TYPELESS:
            case DXGI_FORMAT_BC4_UNORM:
            case DXGI_FORMAT_BC4_SNORM:
            case DXGI_FORMAT_BC5_TYPELESS:
            case DXGI_FORMAT_BC5_UNORM:
            case DXGI_FORMAT_BC5_SNORM:
            case DXGI_FORMAT_BC6H_TYPELESS:
            case DXGI_FORMAT_BC6H_UF16:
            case DXGI_FORMAT_BC6H_SF16:
            case DXGI_FORMAT_BC7_TYPELESS:
            case DXGI_FORMAT_BC7_UNORM:
            case DXGI_FORMAT_BC7_UNORM_SRGB:
                return true;
            default:
                return false;
            }
        }

        static bool IsCpuBgra8DxgiFormat(DXGI_FORMAT fmt)
        {
            return fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        }

        struct QuadVertex
        {
            float px;
            float py;
            float u;
            float v;
        };

        static Microsoft::WRL::ComPtr<ID3D11VertexShader> g_vs {};
        static Microsoft::WRL::ComPtr<ID3D11PixelShader> g_ps {};
        static Microsoft::WRL::ComPtr<ID3D11InputLayout> g_inputLayout {};
        static Microsoft::WRL::ComPtr<ID3D11Buffer> g_vb {};
        static Microsoft::WRL::ComPtr<ID3D11Buffer> g_cb {};
        static Microsoft::WRL::ComPtr<ID3D11SamplerState> g_samplerPoint {};
        static Microsoft::WRL::ComPtr<ID3D11SamplerState> g_samplerLinear {};
        static Microsoft::WRL::ComPtr<ID3D11SamplerState> g_samplerWrap {};
        static Microsoft::WRL::ComPtr<ID3D11BlendState> g_blend {};
        static Microsoft::WRL::ComPtr<ID3D11RasterizerState> g_rsScissor {};
        static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> g_checkerSrv {};

        struct SrvCacheEntry
        {
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv {};
            UINT width { 0 };
            UINT height { 0 };
            DXGI_FORMAT format { DXGI_FORMAT_UNKNOWN };
            std::list<std::wstring>::iterator lruIt {};
        };

        static std::mutex g_srvCacheMutex;
        static std::unordered_map<std::wstring, SrvCacheEntry> g_srvCache;
        static std::list<std::wstring> g_srvCacheLru;
        static size_t g_srvCacheCapacity = 64; // simple entry-count cap

        static bool TryGetSrvFromCache(
            const std::wstring& key,
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv,
            UINT& outW,
            UINT& outH,
            DXGI_FORMAT& outFormat)
        {
            std::lock_guard<std::mutex> lock(g_srvCacheMutex);
            auto it = g_srvCache.find(key);
            if (it == g_srvCache.end() || !it->second.srv)
            {
                return false;
            }

            // move-to-front (MRU)
            g_srvCacheLru.erase(it->second.lruIt);
            g_srvCacheLru.push_front(key);
            it->second.lruIt = g_srvCacheLru.begin();

            outSrv = it->second.srv;
            outW = it->second.width;
            outH = it->second.height;
            outFormat = it->second.format;
            return true;
        }

        static void PutSrvToCache(
            const std::wstring& key,
            const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv,
            UINT w,
            UINT h,
            DXGI_FORMAT format)
        {
            if (!srv)
            {
                return;
            }

            std::lock_guard<std::mutex> lock(g_srvCacheMutex);
            auto it = g_srvCache.find(key);
            if (it != g_srvCache.end())
            {
                g_srvCacheLru.erase(it->second.lruIt);
                g_srvCache.erase(it);
            }

            g_srvCacheLru.push_front(key);
            SrvCacheEntry entry {};
            entry.srv = srv;
            entry.width = w;
            entry.height = h;
            entry.format = format;
            entry.lruIt = g_srvCacheLru.begin();
            g_srvCache.emplace(key, std::move(entry));

            while (g_srvCache.size() > g_srvCacheCapacity && !g_srvCacheLru.empty())
            {
                const std::wstring victimKey = g_srvCacheLru.back();
                g_srvCacheLru.pop_back();
                g_srvCache.erase(victimKey);
            }
        }

        static HRESULT EnsureD3DQuadResources(ID3D11Device* device)
        {
            if (!device)
            {
                return E_INVALIDARG;
            }
            if (g_vs && g_ps && g_inputLayout && g_vb && g_samplerPoint && g_samplerLinear && g_samplerWrap && g_blend && g_rsScissor && g_cb && g_checkerSrv)
            {
                return S_OK;
            }

            // Minimal shaders (HLSL) compiled at runtime (debug-friendly). For production you’d precompile.
            const char* vsSrc =
                "struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };"
                "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
                "VSOut main(VSIn i){ VSOut o; o.pos=float4(i.pos,0,1); o.uv=i.uv; return o; }";

            const char* psSrc =
                "Texture2D tex0 : register(t0);"
                "SamplerState samp0 : register(s0);"
                "cbuffer Cb : register(b0) { float opacity; float3 pad; };"
                "float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target {"
                "  float4 c = tex0.Sample(samp0, uv);"
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

            hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs);
            if (FAILED(hr))
            {
                return hr;
            }
            hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps);
            if (FAILED(hr))
            {
                return hr;
            }

            const D3D11_INPUT_ELEMENT_DESC il[] =
            {
                { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            hr = device->CreateInputLayout(il, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_inputLayout);
            if (FAILED(hr))
            {
                return hr;
            }

            D3D11_BUFFER_DESC bd {};
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.ByteWidth = sizeof(QuadVertex) * 4;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            hr = device->CreateBuffer(&bd, nullptr, &g_vb);
            if (FAILED(hr))
            {
                return hr;
            }

            // Low-quality point sampling (pixelated)
            D3D11_SAMPLER_DESC sdPoint {};
            sdPoint.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sdPoint.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sdPoint.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sdPoint.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sdPoint.MaxAnisotropy = 1;
            sdPoint.MinLOD = 0.0f;
            sdPoint.MaxLOD = D3D11_FLOAT32_MAX;
            sdPoint.MipLODBias = 0.0f;
            sdPoint.ComparisonFunc = D3D11_COMPARISON_NEVER;
            sdPoint.BorderColor[0] = 0.0f;
            sdPoint.BorderColor[1] = 0.0f;
            sdPoint.BorderColor[2] = 0.0f;
            sdPoint.BorderColor[3] = 0.0f;

            hr = device->CreateSamplerState(&sdPoint, &g_samplerPoint);
            if (FAILED(hr))
            {
                return hr;
            }

            // High-quality sampling (smooth)
            D3D11_SAMPLER_DESC sdLinear = sdPoint;
            sdLinear.Filter = D3D11_FILTER_ANISOTROPIC;
            sdLinear.MaxAnisotropy = 16;
            hr = device->CreateSamplerState(&sdLinear, &g_samplerLinear);
            if (FAILED(hr))
            {
                return hr;
            }

            // Wrap sampler for tiled checkerboard background.
            if (!g_samplerWrap)
            {
                D3D11_SAMPLER_DESC sdWrap = sdPoint;
                sdWrap.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
                sdWrap.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
                sdWrap.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
                hr = device->CreateSamplerState(&sdWrap, &g_samplerWrap);
                if (FAILED(hr))
                {
                    return hr;
                }
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
            hr = device->CreateBlendState(&blend, &g_blend);
            if (FAILED(hr))
            {
                return hr;
            }

            // Rasterizer state with scissor enabled (needed to clip zoomed/panned images to their LayoutRect).
            if (!g_rsScissor)
            {
                D3D11_RASTERIZER_DESC rd {};
                rd.FillMode = D3D11_FILL_SOLID;
                rd.CullMode = D3D11_CULL_NONE;
                rd.DepthClipEnable = TRUE;
                rd.ScissorEnable = TRUE;
                hr = device->CreateRasterizerState(&rd, &g_rsScissor);
                if (FAILED(hr))
                {
                    return hr;
                }
            }

            // Dynamic constant buffer for opacity.
            D3D11_BUFFER_DESC cbd {};
            cbd.Usage = D3D11_USAGE_DYNAMIC;
            cbd.ByteWidth = 16; // float opacity + padding
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            hr = device->CreateBuffer(&cbd, nullptr, &g_cb);
            if (FAILED(hr))
            {
                return hr;
            }

            // 64x64 checkerboard SRV for alpha visualization (tiled via wrap sampler).
            if (!g_checkerSrv)
            {
                constexpr UINT texW = 64;
                constexpr UINT texH = 64;
                constexpr UINT tile = 8; // 8px squares

                std::vector<UINT32> pixels;
                pixels.resize(static_cast<size_t>(texW) * static_cast<size_t>(texH));

                const UINT32 light = 0xFFF0F0F0; // AARRGGBB
                const UINT32 dark = 0xFF707070;  // AARRGGBB

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

                hr = device->CreateShaderResourceView(tex.Get(), nullptr, &g_checkerSrv);
                if (FAILED(hr))
                {
                    return hr;
                }
            }

            return S_OK;
        }
    }

    void Image::SetAlphaCheckerboardEnabled(bool enabled)
    {
        if (m_alphaCheckerboardEnabled == enabled)
        {
            return;
        }

        m_alphaCheckerboardEnabled = enabled;
        Invalidate();
    }

    Image::Image()
        : Wnd()
        , m_request()
    {
        m_loadingSpinner = std::make_shared<Spinner>(L"loadingSpinner");
        m_loadingSpinner->SetActive(false);
        AddChild(m_loadingSpinner);
    }

    Image::Image(const std::wstring& name)
        : Wnd(name)
        , m_request()
    {
        m_loadingSpinner = std::make_shared<Spinner>(L"loadingSpinner");
        m_loadingSpinner->SetActive(false);
        AddChild(m_loadingSpinner);
    }

    Image::~Image()
    {
        if (m_currentHandle != 0)
        {
            ImageCore::ImageLoader::Instance().Cancel(m_currentHandle);
        }
    }

    Image::ViewTransform Image::GetViewTransform() const
    {
        ViewTransform vt {};
        vt.zoomScale = m_zoomScale;
        vt.targetZoomScale = m_targetZoomScale;
        vt.zoomVelocity = m_zoomVelocity;
        vt.panX = m_panX;
        vt.panY = m_panY;
        return vt;
    }

    Image::LoadedInfo Image::GetLoadedInfo() const
    {
        LoadedInfo info {};
        info.width = m_loadedW;
        info.height = m_loadedH;
        info.format = m_loadedFormat;
        info.sourcePath = m_loadedFilePath;
        return info;
    }

    void Image::SetViewTransform(const ViewTransform& vt, bool notify)
    {
        m_zoomScale = vt.zoomScale;
        m_targetZoomScale = vt.targetZoomScale;
        m_zoomVelocity = vt.zoomVelocity;
        m_panX = vt.panX;
        m_panY = vt.panY;

        // Stop any in-progress interaction state.
        m_panning = false;
        m_panArmed = false;
        m_pointerZoomActive = false;
        m_lastZoomAnimMs = CommonUtil::NowMs();

        ClampPanToVisible();

        const bool prevSuppress = m_suppressViewNotify;
        if (!notify)
        {
            m_suppressViewNotify = true;
        }

        Invalidate();

        if (notify && !m_suppressViewNotify && m_onViewChanged)
        {
            m_onViewChanged(GetViewTransform());
        }

        m_suppressViewNotify = prevSuppress;
    }

    void Image::SetOnViewChanged(ViewChangedHandler handler)
    {
        m_onViewChanged = std::move(handler);
    }

    bool Image::TryGetBitmapSize(D2D1_SIZE_F& outSize) const
    {
        if (m_loadedW > 0 && m_loadedH > 0)
        {
            outSize = { static_cast<float>(m_loadedW), static_cast<float>(m_loadedH) };
            return true;
        }

        if (m_gpuWidth > 0 && m_gpuHeight > 0)
        {
            outSize = { static_cast<float>(m_gpuWidth), static_cast<float>(m_gpuHeight) };
            return true;
        }

        if (m_bitmap)
        {
            outSize = m_bitmap->GetSize();
            return true;
        }

        return false;
    }

    bool Image::TryComputeAspectFitBaseRect(const D2D1_RECT_F& layoutRect, const D2D1_SIZE_F& bitmapSize, D2D1_RECT_F& outRect) const
    {
        const float layoutWidth = layoutRect.right - layoutRect.left;
        const float layoutHeight = layoutRect.bottom - layoutRect.top;

        if (!(layoutWidth > 0.0f && layoutHeight > 0.0f && bitmapSize.width > 0.0f && bitmapSize.height > 0.0f))
        {
            return false;
        }

        const float bitmapAspect = bitmapSize.width / bitmapSize.height;
        const float layoutAspect = layoutWidth / layoutHeight;

        D2D1_RECT_F destRect = layoutRect;
        if (bitmapAspect > layoutAspect)
        {
            const float scaledHeight = layoutWidth / bitmapAspect;
            const float yOffset = (layoutHeight - scaledHeight) * 0.5f;
            destRect.top = layoutRect.top + yOffset;
            destRect.bottom = destRect.top + scaledHeight;
        }
        else
        {
            const float scaledWidth = layoutHeight * bitmapAspect;
            const float xOffset = (layoutWidth - scaledWidth) * 0.5f;
            destRect.left = layoutRect.left + xOffset;
            destRect.right = destRect.left + scaledWidth;
        }

        outRect = destRect;
        return true;
    }

    void Image::ClampPanToVisible()
    {
        if (m_request.purpose != ImageCore::ImagePurpose::FullResolution)
        {
            return;
        }

        D2D1_SIZE_F bitmapSize {};
        if (!TryGetBitmapSize(bitmapSize))
        {
            return;
        }

        const D2D1_RECT_F layoutRect = LayoutRect();
        D2D1_RECT_F baseRect {};
        if (!TryComputeAspectFitBaseRect(layoutRect, bitmapSize, baseRect))
        {
            return;
        }

        const float width = baseRect.right - baseRect.left;
        const float height = baseRect.bottom - baseRect.top;
        if (width <= 0.0f || height <= 0.0f)
        {
            return;
        }

        const float scaledWidth = width * m_zoomScale;
        const float scaledHeight = height * m_zoomScale;
        if (scaledWidth <= 0.0f || scaledHeight <= 0.0f)
        {
            return;
        }

        const float centerX = (baseRect.left + baseRect.right) * 0.5f;
        const float centerY = (baseRect.top + baseRect.bottom) * 0.5f;
        constexpr float kMinVisible = 1.0f;

        float minPanX = (layoutRect.left + kMinVisible) - (centerX + scaledWidth * 0.5f);
        float maxPanX = (layoutRect.right - kMinVisible) - (centerX - scaledWidth * 0.5f);
        if (minPanX > maxPanX)
        {
            const float mid = (minPanX + maxPanX) * 0.5f;
            minPanX = mid;
            maxPanX = mid;
        }
        m_panX = (std::max)(minPanX, (std::min)(maxPanX, m_panX));

        float minPanY = (layoutRect.top + kMinVisible) - (centerY + scaledHeight * 0.5f);
        float maxPanY = (layoutRect.bottom - kMinVisible) - (centerY - scaledHeight * 0.5f);
        if (minPanY > maxPanY)
        {
            const float mid = (minPanY + maxPanY) * 0.5f;
            minPanY = mid;
            maxPanY = mid;
        }
        m_panY = (std::max)(minPanY, (std::min)(maxPanY, m_panY));
    }

    void Image::SetRect(const D2D1_RECT_F& rect)
    {
        SetLayoutRect(rect);
    }

    Size Image::Measure(Size available)
    {
        // FullResolution은 윈도우 크기에 맞게 항상 available size 사용 (Aspect Ratio는 OnRender에서 처리)
        if (available.w > 0.0f && available.h > 0.0f)
        {
            m_desired = available;
        }
        else
        {
            // 기본 크기
            m_desired = { 800.0f, 600.0f };
        }
        return m_desired;
    }

    HRESULT Image::SetSourceFile(const std::wstring& filePath)
    {
        const std::wstring normalized = CommonUtil::NormalizePath(filePath);

        // When switching main images, preserve zoom/pan (comparison workflow),
        // but stop any in-flight interaction/animation state.
        if (!normalized.empty() && normalized != m_filePath && m_request.purpose == ImageCore::ImagePurpose::FullResolution)
        {
            if (m_panning)
            {
                m_panning = false;
                if (BackplateRef() != nullptr && BackplateRef()->Window() != nullptr)
                {
                    if (GetCapture() == BackplateRef()->Window())
                    {
                        ReleaseCapture();
                    }
                }
            }

            m_pointerZoomActive = false;
            m_zoomVelocity = 0.0f;
            m_lastZoomAnimMs = CommonUtil::NowMs();
        }

        // If this path is already the current requested source, don't cancel/restart.
        // This is important for keyboard navigation with repeat + debounced apply, where the same selection
        // can be "applied" more than once (e.g. queued timers). Restarting would create token churn and
        // can lead to apparent "stuck spinner" states.
        if (!normalized.empty() && normalized == m_filePath)
        {
            // If this file previously failed, allow an explicit retry by clearing failure state.
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                if (m_failedFilePath == normalized && FAILED(m_failedHr))
                {
                    m_failedFilePath.clear();
                    m_failedHr = S_OK;
                }
                else
                {
                    return S_FALSE;
                }
            }
        }

        // If we're already showing this source, don't restart transitions (prevents "flash" on repeated clicks).
        if (!normalized.empty() && normalized == m_filePath && m_loadedFilePath == m_filePath)
        {
            const bool cpuLoaded = (m_bitmap != nullptr);
            const bool gpuLoaded = (m_request.purpose == ImageCore::ImagePurpose::FullResolution &&
                m_gpuSrv && m_gpuWidth != 0 && m_gpuHeight != 0);
            if (cpuLoaded || gpuLoaded)
            {
                return S_FALSE;
            }
        }

        if (m_currentHandle != 0)
        {
            ImageCore::ImageLoader::Instance().Cancel(m_currentHandle);
            m_currentHandle = 0;
        }

        // Important: ImageLoader::Cancel() does not guarantee the worker callback runs.
        // If we don't clear m_loading here, we can get stuck in an "infinite spinner" state where
        // OnRender refuses to start the next request because it believes a request is still in-flight.
        m_loading.store(false);

        // Drop any pending (UI-thread) apply from the previous selection.
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pendingBlocks.reset();
            m_pendingW = 0;
            m_pendingH = 0;
            m_pendingRowPitch = 0;
            m_pendingFormat = DXGI_FORMAT_UNKNOWN;
            m_pendingSourcePath.clear();
        }
        m_loadedW = 0;
        m_loadedH = 0;
        m_loadedFormat = DXGI_FORMAT_UNKNOWN;

        // Selection changed: clear any in-flight token so we can start a new request on next render.
        m_inflightToken.store(0);

        // A) Fast reselect path: if SRV is cached, swap immediately (no disk/decode/upload).
        if (m_request.purpose == ImageCore::ImagePurpose::FullResolution && m_backplate)
        {
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cachedSrv;
            UINT cw = 0, ch = 0;
            DXGI_FORMAT cachedFormat = DXGI_FORMAT_UNKNOWN;
            if (TryGetSrvFromCache(normalized, cachedSrv, cw, ch, cachedFormat))
            {
                m_gpuSrv = cachedSrv;
                m_gpuWidth = cw;
                m_gpuHeight = ch;
                m_loadedFilePath = normalized;
                m_filePath = normalized;
                m_loadedW = cw;
                m_loadedH = ch;
                m_loadedFormat = cachedFormat;

                // Cancel CPU path bitmaps for main image
                m_bitmap.Reset();

                m_loading.store(false);
                m_request.source = normalized;
                Invalidate();
                return S_OK;
            }
        }

        m_filePath = normalized;
        // Clear any previous failure state on explicit user selection (allow retry).
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_failedFilePath.clear();
            m_failedHr = S_OK;
        }
        m_forceCpuDecode.store(false);
        m_loading.store(false);

        // Request 업데이트
        m_request.source = normalized;
        
        return S_OK;
    }

    void Image::ClearSource()
    {
        if (m_currentHandle != 0)
        {
            ImageCore::ImageLoader::Instance().Cancel(m_currentHandle);
            m_currentHandle = 0;
        }

        m_loading.store(false);
        m_inflightToken.store(0);

        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pendingBlocks.reset();
            m_pendingW = 0;
            m_pendingH = 0;
            m_pendingRowPitch = 0;
            m_pendingFormat = DXGI_FORMAT_UNKNOWN;
            m_pendingSourcePath.clear();
            m_failedFilePath.clear();
            m_failedHr = S_OK;
        }

        m_filePath.clear();
        m_loadedFilePath.clear();
        m_loadedW = 0;
        m_loadedH = 0;
        m_loadedFormat = DXGI_FORMAT_UNKNOWN;

        m_bitmap.Reset();
        m_gpuSrv.Reset();
        m_gpuWidth = 0;
        m_gpuHeight = 0;

        m_request.source.clear();

        Invalidate();
    }

    void Image::SetInteractionEnabled(bool enabled)
    {
        if (m_interactionEnabled == enabled)
        {
            return;
        }

        m_interactionEnabled = enabled;
        if (!m_interactionEnabled)
        {
            m_panArmed = false;
            m_panning = false;
            m_pointerZoomActive = false;
            if (BackplateRef() != nullptr && BackplateRef()->Window() != nullptr)
            {
                if (GetCapture() == BackplateRef()->Window())
                {
                    ReleaseCapture();
                }
            }
        }
    }

    void Image::SetOnClick(ClickHandler handler)
    {
        m_onClick = std::move(handler);
    }

    void Image::SetLoadingSpinnerEnabled(bool enabled)
    {
        if (m_loadingSpinnerEnabled == enabled)
        {
            return;
        }
        m_loadingSpinnerEnabled = enabled;
        Invalidate();
    }

    void Image::RequestImageLoad()
    {
        if (m_filePath.empty() || m_loading.load())
        {
            return;
        }

        // If the last attempt failed for this file, don't spin/retry forever.
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            if (!m_failedFilePath.empty() && m_failedFilePath == m_filePath)
            {
                return;
            }
        }

        // Already displaying the requested source (no need to load again).
        if (m_loadedFilePath == m_filePath)
        {
            // CPU path loaded
            if (m_bitmap)
            {
                return;
            }

            // GPU-native DDS path loaded (main image)
            if (m_request.purpose == ImageCore::ImagePurpose::FullResolution && m_gpuSrv && m_gpuWidth != 0 && m_gpuHeight != 0)
            {
                return;
            }
        }

        m_loading.store(true);
        m_request.source = m_filePath;
        // D2D-only renderer: force CPU-displayable DDS output (avoid UI-thread BCn decompress).
        if (m_forceCpuDecode.load() || m_backplate == nullptr || m_backplate->D3DDevice() == nullptr)
        {
            m_request.allowGpuCompressedDDS = false;
        }
        else
        {
            m_request.allowGpuCompressedDDS = true;
        }

        const unsigned long long token = m_requestToken.fetch_add(1ULL) + 1ULL;
        m_inflightToken.store(token);
        const std::wstring requestedPath = m_filePath; // already normalized

        // (debug request tracing removed)

        m_currentHandle = ImageCore::ImageLoader::Instance().RequestDecoded(
            m_request,
            [this, token, requestedPath](HRESULT hr, ImageCore::DecodedImage image)
            {
                // NOTE: This callback runs on a worker thread.
                // Do NOT read m_filePath here (UI thread writes it). Use token gating instead.
                const unsigned long long current = m_inflightToken.load();
                if (token != current)
                {
                    // If there is no current in-flight request, ensure we don't get stuck "loading".
                    if (current == 0)
                    {
                        m_loading.store(false);
                    }
                    return;
                }

                OnImageLoaded(requestedPath, hr, std::move(image));
            });
    }

    void Image::OnImageLoaded(
        const std::wstring& sourcePath,
        HRESULT hr,
        ImageCore::DecodedImage image)
    {
        m_currentHandle = 0;

        const std::wstring normalizedSource = CommonUtil::NormalizePath(sourcePath);

        // 변환은 OnRender에서 render target을 사용하여 수행
        // 여기서는 저장만 하고 Invalidate로 OnRender 호출 유도
        if (SUCCEEDED(hr) && image.blocks && !image.blocks->empty())
        {
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                m_pendingBlocks = std::move(image.blocks);
                m_pendingW = image.width;
                m_pendingH = image.height;
                m_pendingRowPitch = image.rowPitchBytes;
                m_pendingFormat = image.dxgiFormat;
                m_pendingSourcePath = normalizedSource;
                m_failedFilePath.clear();
                m_failedHr = S_OK;
            }
            
            // worker thread에서 UI thread로 명확히 redraw 요청
            if (m_backplate)
            {
                // Wake UI thread without PostMessage; the UI loop waits on this event.
                m_backplate->RequestAsyncRedraw();
            }
        }
        else
        {
            // Mark as failed so we don't spin forever.
            // Keep displaying the previous image (if any).
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                m_failedFilePath = normalizedSource;
                m_failedHr = hr;
            }
            // Failure completes the in-flight request (stop spinner).
            m_loading.store(false);
            m_inflightToken.store(0);

            if (m_backplate)
            {
                m_backplate->RequestAsyncRedraw();
            }
        }
    }

    void Image::OnRender(ID2D1RenderTarget* target)
    {
        if (target == nullptr)
        {
            return;
        }

        // IMPORTANT: zoom/pan can expand the destination rect beyond LayoutRect.
        // Clip to our own bounds so we never draw over neighboring controls.
        const D2D1_RECT_F clipRect = LayoutRect();
        target->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        // Advance smooth zoom animation
        if (m_request.purpose == ImageCore::ImagePurpose::FullResolution)
        {
            AdvanceZoomAnimation(CommonUtil::NowMs());
        }

        const bool gpuActive = (m_request.purpose == ImageCore::ImagePurpose::FullResolution &&
            m_backplate && m_backplate->D3DDevice() != nullptr && m_gpuSrv);

        // Background/letterbox rendering belongs to container panes.
        // This image control renders only image content.

        // Apply pending decoded payload (set by worker thread).
        std::shared_ptr<std::vector<uint8_t>> pendingBlocks {};
        uint32_t pendingW = 0;
        uint32_t pendingH = 0;
        uint32_t pendingRowPitch = 0;
        DXGI_FORMAT pendingFormat = DXGI_FORMAT_UNKNOWN;
        std::wstring pendingSourcePath;
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            pendingBlocks = std::move(m_pendingBlocks);
            pendingW = m_pendingW;
            pendingH = m_pendingH;
            pendingRowPitch = m_pendingRowPitch;
            pendingFormat = m_pendingFormat;
            m_pendingBlocks.reset();
            m_pendingW = 0;
            m_pendingH = 0;
            m_pendingRowPitch = 0;
            m_pendingFormat = DXGI_FORMAT_UNKNOWN;
            pendingSourcePath = m_pendingSourcePath;
            m_pendingSourcePath.clear();
        }

        if (pendingBlocks)
        {
            // Only swap if this pending image still matches the current requested source.
            // Otherwise, drop it to avoid "wrong image stuck" due to out-of-order completion.
            if (!pendingSourcePath.empty() && pendingSourcePath == m_filePath)
            {
                // Try GPU path for main image: if scratch is GPU-compressed DDS, upload to SRV and render via D3D.
                bool usedGpu = false;
                bool applied = false;
                if (IsCompressedDxgiFormat(pendingFormat) && m_request.purpose == ImageCore::ImagePurpose::FullResolution && m_backplate)
                {
                    ID3D11Device* dev = m_backplate->D3DDevice();
                    if (dev && !m_forceCpuDecode.load())
                    {
                        (void)EnsureD3DQuadResources(dev);

                        D3D11_TEXTURE2D_DESC td {};
                        td.Width = pendingW;
                        td.Height = pendingH;
                        td.MipLevels = 1;
                        td.ArraySize = 1;
                        td.Format = pendingFormat;
                        td.SampleDesc.Count = 1;
                        td.Usage = D3D11_USAGE_IMMUTABLE;
                        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                        D3D11_SUBRESOURCE_DATA init {};
                        init.pSysMem = pendingBlocks->data();
                        init.SysMemPitch = pendingRowPitch;
                        init.SysMemSlicePitch = static_cast<UINT>(pendingBlocks->size());

                        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
                        HRESULT hrTex = dev->CreateTexture2D(&td, &init, &tex);
                        if (SUCCEEDED(hrTex) && tex)
                        {
                            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
                            HRESULT hrSrv = dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
                            if (SUCCEEDED(hrSrv) && srv)
                            {
                                m_gpuSrv = srv;
                                m_gpuWidth = pendingW;
                                m_gpuHeight = pendingH;
                                m_loadedFilePath = pendingSourcePath;
                                m_loadedW = pendingW;
                                m_loadedH = pendingH;
                                m_loadedFormat = pendingFormat;

                                PutSrvToCache(pendingSourcePath, m_gpuSrv, m_gpuWidth, m_gpuHeight, pendingFormat);

                                m_bitmap.Reset();

                                usedGpu = true;
                                applied = true;
                            }
                            else
                            {
                                LogImageHr(L"D3D CreateShaderResourceView", pendingSourcePath, hrSrv);
                            }
                        }
                        else
                        {
                            LogImageHr(L"D3D CreateTexture2D", pendingSourcePath, hrTex);
                        }

                        if (!applied)
                        {
                            // We cannot display BCn blocks via D2D. Force CPU decode for this selection.
                            m_forceCpuDecode.store(true);
                            m_loading.store(false);
                            RequestImageLoad();
                            usedGpu = true;
                        }
                    }
                    else
                    {
                        // No GPU device (or GPU path disabled). Request a CPU-decompressed decode.
                        m_forceCpuDecode.store(true);
                        m_loading.store(false);
                        RequestImageLoad();
                        usedGpu = true;
                    }
                }

                if (!usedGpu)
                {
                    HRESULT hrBmp = E_FAIL;
                    if (IsCpuBgra8DxgiFormat(pendingFormat) && pendingW > 0 && pendingH > 0 && pendingRowPitch > 0)
                    {
                        D2D1_BITMAP_PROPERTIES props {};
                        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
                        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
                        props.dpiX = 96.0f;
                        props.dpiY = 96.0f;

                        Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dBitmap;
                        const D2D1_SIZE_U size = D2D1::SizeU(pendingW, pendingH);
                        hrBmp = target->CreateBitmap(size, pendingBlocks->data(), pendingRowPitch, &props, &d2dBitmap);
                        if (SUCCEEDED(hrBmp) && d2dBitmap)
                        {
                            m_bitmap = d2dBitmap;
                            m_loadedFilePath = pendingSourcePath;
                            m_loadedW = pendingW;
                            m_loadedH = pendingH;
                            m_loadedFormat = pendingFormat;
                            // Ensure the D3D pass won't keep drawing a stale GPU SRV under CPU-decoded images.
                            m_gpuSrv.Reset();
                            m_gpuWidth = 0;
                            m_gpuHeight = 0;
                            applied = true;
                        }
                    }

                    if (FAILED(hrBmp) && !applied)
                    {
                        LogImageHr(L"D2D CreateBitmap", pendingSourcePath, hrBmp);
                        {
                            std::lock_guard<std::mutex> lock(m_pendingMutex);
                            m_failedFilePath = pendingSourcePath;
                            m_failedHr = hrBmp;
                        }
                        // Conversion failure completes the in-flight request (stop spinner).
                        m_loading.store(false);
                    }
                }

                // Success (GPU or CPU bitmap): mark request complete now that the result is applied on the UI thread.
                if (applied)
                {
                    m_loading.store(false);
                    m_inflightToken.store(0);
                }
            }
            else if (!pendingSourcePath.empty())
            {
                // (debug tracing removed)
            }
        }

        // Request loading if we are not already loading the latest requested source.
        // Note: we keep drawing the previous bitmap while the next image loads to avoid "black flash".
        if (!m_loading.load() && !m_filePath.empty())
        {
            // If the current file is marked failed, do not retry automatically (avoids infinite loops).
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                if (!m_failedFilePath.empty() && m_failedFilePath == m_filePath)
                {
                    if (m_loadingSpinner)
                    {
                        m_loadingSpinner->SetActive(false);
                    }
                    Wnd::OnRender(target);
                    return;
                }
            }

            const bool cpuLoaded = (m_bitmap && m_loadedFilePath == m_filePath);
            const bool gpuLoaded = (m_request.purpose == ImageCore::ImagePurpose::FullResolution &&
                m_gpuSrv && m_gpuWidth != 0 && m_gpuHeight != 0 && m_loadedFilePath == m_filePath);

            if (!cpuLoaded && !gpuLoaded)
            {
                RequestImageLoad();
            }
        }

        const auto computeAspectFitDestRect = [this](const D2D1_RECT_F& layoutRect, const D2D1_SIZE_F& bitmapSize) -> D2D1_RECT_F
        {
            const float layoutWidth = layoutRect.right - layoutRect.left;
            const float layoutHeight = layoutRect.bottom - layoutRect.top;

            if (!(layoutWidth > 0.0f && layoutHeight > 0.0f && bitmapSize.width > 0.0f && bitmapSize.height > 0.0f))
            {
                return layoutRect;
            }

            const float bitmapAspect = bitmapSize.width / bitmapSize.height;
            const float layoutAspect = layoutWidth / layoutHeight;

            D2D1_RECT_F destRect = layoutRect;

            if (bitmapAspect > layoutAspect)
            {
                // bitmap is wider: fit by width
                const float scaledHeight = layoutWidth / bitmapAspect;
                const float yOffset = (layoutHeight - scaledHeight) * 0.5f;
                destRect.top = layoutRect.top + yOffset;
                destRect.bottom = destRect.top + scaledHeight;
            }
            else
            {
                // bitmap is taller: fit by height
                const float scaledWidth = layoutHeight * bitmapAspect;
                const float xOffset = (layoutWidth - scaledWidth) * 0.5f;
                destRect.left = layoutRect.left + xOffset;
                destRect.right = destRect.left + scaledWidth;
            }

            // Apply zoom scale and pan offset (for main image only)
            if (m_request.purpose == ImageCore::ImagePurpose::FullResolution && m_zoomScale != 1.0f)
            {
                const float centerX = (destRect.left + destRect.right) * 0.5f;
                const float centerY = (destRect.top + destRect.bottom) * 0.5f;
                const float width = destRect.right - destRect.left;
                const float height = destRect.bottom - destRect.top;
                const float scaledWidth = width * m_zoomScale;
                const float scaledHeight = height * m_zoomScale;
                destRect.left = centerX - scaledWidth * 0.5f + m_panX;
                destRect.right = destRect.left + scaledWidth;
                destRect.top = centerY - scaledHeight * 0.5f + m_panY;
                destRect.bottom = destRect.top + scaledHeight;
            }
            else if (m_request.purpose == ImageCore::ImagePurpose::FullResolution && 
                     (std::abs(m_panX) > 0.001f || std::abs(m_panY) > 0.001f))
            {
                // Apply pan even when not zoomed (though this shouldn't normally happen)
                destRect.left += m_panX;
                destRect.right += m_panX;
                destRect.top += m_panY;
                destRect.bottom += m_panY;
            }

            return destRect;
        };

        const auto ensureCheckerBrushes = [this, target]()
        {
            if (m_checkerLightBrush && m_checkerDarkBrush)
            {
                return;
            }

            // Checker colors (opaque).
            (void)target->CreateSolidColorBrush(D2D1::ColorF(0.94f, 0.94f, 0.94f, 1.0f), &m_checkerLightBrush);
            (void)target->CreateSolidColorBrush(D2D1::ColorF(0.44f, 0.44f, 0.44f, 1.0f), &m_checkerDarkBrush);
        };

        const auto drawCheckerboard = [this, target, &ensureCheckerBrushes](const D2D1_RECT_F& rect)
        {
            if (!m_alphaCheckerboardEnabled)
            {
                return;
            }

            ensureCheckerBrushes();
            if (!m_checkerLightBrush || !m_checkerDarkBrush)
            {
                return;
            }

            constexpr float tile = 8.0f; // DIP
            if (tile <= 1.0f)
            {
                return;
            }

            const float startX = std::floor(rect.left / tile) * tile;
            const float startY = std::floor(rect.top / tile) * tile;

            for (float y = startY; y < rect.bottom; y += tile)
            {
                for (float x = startX; x < rect.right; x += tile)
                {
                    const int ix = static_cast<int>(std::floor((x - startX) / tile));
                    const int iy = static_cast<int>(std::floor((y - startY) / tile));
                    const bool dark = (((ix + iy) & 1) != 0);
                    const D2D1_RECT_F r { x, y, x + tile, y + tile };
                    target->FillRectangle(r, dark ? m_checkerDarkBrush.Get() : m_checkerLightBrush.Get());
                }
            }
        };

        const auto drawBitmapAspectFit = [this, target, &computeAspectFitDestRect, &drawCheckerboard](ID2D1Bitmap* bmp, float opacity)
        {
            if (bmp == nullptr || opacity <= 0.0f)
            {
                return;
            }

            const auto bitmapSize = bmp->GetSize();
            const auto layoutRect = LayoutRect();

            const D2D1_RECT_F sourceRect = D2D1::RectF(0.0f, 0.0f, bitmapSize.width, bitmapSize.height);
            const D2D1_RECT_F destRect = computeAspectFitDestRect(layoutRect, bitmapSize);

            drawCheckerboard(destRect);

            D2D1_BITMAP_INTERPOLATION_MODE interpMode = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
            
            // Get current Direct2D version to select the best available option
            FD2D::D2DVersion d2dVersion = FD2D::Core::GetSupportedD2DVersion();
            
            if (m_request.purpose == ImageCore::ImagePurpose::FullResolution)
            {
            if (m_highQualitySampling)
            {
                    if (d2dVersion >= FD2D::D2DVersion::D2D1_1)
                    {
                        Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
                        if (SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(&dc))) && dc)
                        {
                            dc->DrawBitmap(
                                bmp,
                                destRect,
                                opacity,
                                D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                                sourceRect);
                            return;
                        }
                    }
                    interpMode = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
            }
            else
            {
                // Pixel-perfect sampling for compare workflows.
                if (d2dVersion >= FD2D::D2DVersion::D2D1_1)
                {
                    interpMode = D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
                }
                // Direct2D 1.0 fallback: use LINEAR (already set as default)
            }
            }

            target->DrawBitmap(
                bmp,
                destRect,
                opacity,
                interpMode,
                sourceRect);
        };

        if (m_bitmap)
        {
            // No cross-fade: draw the currently available bitmap (can be the previous image while the next loads).
            drawBitmapAspectFit(m_bitmap.Get(), 1.0f);
        }

        // Loading spinner overlay (only while an actual request is in-flight).
        // This avoids "infinite spinner" states on decode failure; we keep showing the previous image instead.
        const bool shouldShowSpinner = m_loadingSpinnerEnabled && m_loading.load();
        if (m_loadingSpinner)
        {
            m_loadingSpinner->SetActive(shouldShowSpinner);
        }

        Wnd::OnRender(target);

        target->PopAxisAlignedClip();
    }

    void Image::SetZoomScale(float scale)
    {
        constexpr float kMinZoom = 0.1f;
        constexpr float kMaxZoom = 50.0f;
        m_targetZoomScale = std::max(kMinZoom, std::min(kMaxZoom, scale));
        m_lastZoomAnimMs = CommonUtil::NowMs();
        // Immediately request animation frame for fast response
        if (BackplateRef() != nullptr)
        {
            BackplateRef()->RequestAnimationFrame();
        }
        Invalidate();
    }

    void Image::ResetZoom()
    {
        m_targetZoomScale = 1.0f;
        m_zoomVelocity = 0.0f; // Reset velocity when resetting zoom
        m_panX = 0.0f;
        m_panY = 0.0f;
        m_panning = false;
        m_pointerZoomActive = false;
        m_lastZoomAnimMs = CommonUtil::NowMs();
        // Immediately request animation frame for fast response
        if (BackplateRef() != nullptr)
        {
            BackplateRef()->RequestAnimationFrame();
        }
        Invalidate();
    }

    void Image::SetZoomSpeed(float speed)
    {
        // Clamp speed to valid range (fraction of remaining distance per second)
        // e.g., 10.0 = cover 10x the remaining distance per second (very fast)
        // e.g., 5.0 = cover 5x the remaining distance per second (fast)
        // e.g., 2.0 = cover 2x the remaining distance per second (moderate)
        m_zoomSpeed = std::max(0.1f, std::min(100.0f, speed));
    }

    void Image::SetZoomStiffness(float stiffness)
    {
        // Clamp stiffness to valid range (spring constant for critically damped animation)
        // e.g., 40.0 = moderate speed
        // e.g., 80.0 = fast speed (default)
        // e.g., 120.0 = very fast speed
        m_zoomStiffness = std::max(10.0f, std::min(500.0f, stiffness));
    }

    void Image::AdvanceZoomAnimation(unsigned long long nowMs)
    {
        if (m_lastZoomAnimMs == 0)
        {
            m_lastZoomAnimMs = nowMs;
        }

        const unsigned long long elapsed = nowMs - m_lastZoomAnimMs;
        m_lastZoomAnimMs = nowMs;

        if (elapsed == 0)
        {
            return;
        }

        const float dt = static_cast<float>(elapsed) / 1000.0f; // Convert to seconds

        // Critically Damped Spring Animation
        // This ensures smooth, natural motion without overshoot
        const float stiffness = m_zoomStiffness; // 반응성 (higher = faster response, configurable via INI)
        const float damping = 2.0f * std::sqrt(stiffness); // Critically damped (no overshoot)

        const float diff = m_targetZoomScale - m_zoomScale;

        // Spring physics: F = k * x - c * v
        // Acceleration = stiffness * displacement - damping * velocity
        m_zoomVelocity += (diff * stiffness - m_zoomVelocity * damping) * dt;

        // Update position using velocity
        m_zoomScale += m_zoomVelocity * dt;

        // Pointer-based zoom: keep the mouse position fixed by updating pan as zoom changes.
        if (m_pointerZoomActive && !m_panning)
        {
            const float startZoom = (m_pointerZoomStartZoom > 0.0001f) ? m_pointerZoomStartZoom : 0.0001f;
            const float ratio = m_zoomScale / startZoom;

            // Zoom is applied around the center of LayoutRect(), so use that center.
            const D2D1_RECT_F r = LayoutRect();
            const float centerX = (r.left + r.right) * 0.5f;
            const float centerY = (r.top + r.bottom) * 0.5f;
            const float dx = m_pointerZoomMouseX - centerX;
            const float dy = m_pointerZoomMouseY - centerY;

            // For transform: screen = center + (x * zoom) + pan,
            // this maintains the same underlying point at (mouseX, mouseY) across zoom changes.
            m_panX = dx - ((dx - m_pointerZoomStartPanX) * ratio);
            m_panY = dy - ((dy - m_pointerZoomStartPanY) * ratio);
        }

        ClampPanToVisible();

        // Snap to target if very close and velocity is negligible
        if (std::abs(diff) < 0.001f && std::abs(m_zoomVelocity) < 0.001f)
        {
            m_zoomScale = m_targetZoomScale;
            m_zoomVelocity = 0.0f;
            m_pointerZoomActive = false;
        }
        else
        {
            // Continue animation
            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestAnimationFrame();
            }
        }

        Invalidate();

        if (!m_suppressViewNotify && m_onViewChanged && m_request.purpose == ImageCore::ImagePurpose::FullResolution)
        {
            m_onViewChanged(GetViewTransform());
        }
    }

    bool Image::OnInputEvent(const InputEvent& event)
    {
        if (!m_interactionEnabled)
        {
            switch (event.type)
            {
            case InputEventType::MouseDown:
            {
                if (event.button != MouseButton::Left || !event.hasPoint)
                {
                    break;
                }
                POINT pt = event.point;
                const D2D1_RECT_F r = LayoutRect();
                if (static_cast<float>(pt.x) >= r.left &&
                    static_cast<float>(pt.x) <= r.right &&
                    static_cast<float>(pt.y) >= r.top &&
                    static_cast<float>(pt.y) <= r.bottom)
                {
                    if (m_onClick)
                    {
                        m_onClick();
                        return true;
                    }
                }
                break;
            }
            case InputEventType::MouseMove:
            case InputEventType::MouseUp:
            case InputEventType::MouseWheel:
                return false;
            default:
                break;
            }
        }

        switch (event.type)
        {
        case InputEventType::MouseDown:
        {
            if (event.button != MouseButton::Left || !event.hasPoint)
            {
                break;
            }
            // Backplate has already converted coordinates to client/Layout coordinate system
            // lParam now contains coordinates in Layout coordinate system (same as client coordinates)
            POINT pt = event.point;
            const D2D1_RECT_F r = LayoutRect();
            
            // Check if mouse is within image bounds (both in Layout coordinate system)
            if (static_cast<float>(pt.x) >= r.left &&
                static_cast<float>(pt.x) <= r.right &&
                static_cast<float>(pt.y) >= r.top &&
                static_cast<float>(pt.y) <= r.bottom)
            {
                // Main image: arm panning immediately (actual panning starts after a small movement threshold).
                // This makes panning work on startup and after image changes even when zoom == 1.0.
                if (m_request.purpose == ImageCore::ImagePurpose::FullResolution)
                {
                    m_panArmed = true;
                    m_panning = false;
                    m_pointerZoomActive = false;
                    m_panStartX = static_cast<float>(pt.x);
                    m_panStartY = static_cast<float>(pt.y);
                    m_panStartOffsetX = m_panX;
                    m_panStartOffsetY = m_panY;
                    
                    // Capture mouse to track movement outside window
                    if (BackplateRef() != nullptr && BackplateRef()->Window() != nullptr)
                    {
                        SetCapture(BackplateRef()->Window());
                    }
                    
                    return true;
                }
                else if (m_onClick)
                {
                    // Normal click (not zoomed)
                    m_onClick();
                    return true;
                }
            }
            break;
        }
        case InputEventType::MouseMove:
        {
            if (!event.hasPoint)
            {
                break;
            }
            if ((m_panArmed || m_panning) && m_request.purpose == ImageCore::ImagePurpose::FullResolution)
            {
                // Backplate has already converted coordinates to client/Layout coordinate system
                // When mouse is captured, Backplate uses GetCursorPos() and converts to client coordinates
                // lParam now contains coordinates in Layout coordinate system (same as client coordinates)
                POINT pt = event.point;
                
                // Update pan offset based on mouse movement
                // Both m_panStartX/Y and pt are in Layout coordinate system (same as client coordinates)
                const float deltaX = static_cast<float>(pt.x) - m_panStartX;
                const float deltaY = static_cast<float>(pt.y) - m_panStartY;

                // Start panning after a small threshold so simple clicks still work.
                if (!m_panning)
                {
                    constexpr float kStartThresholdPx = 3.0f;
                    if (std::abs(deltaX) >= kStartThresholdPx || std::abs(deltaY) >= kStartThresholdPx)
                    {
                        m_panning = true;
                    }
                }

                if (m_panning)
                {
                    m_panX = m_panStartOffsetX + deltaX;
                    m_panY = m_panStartOffsetY + deltaY;
                    ClampPanToVisible();
                    m_pointerZoomActive = false;
                    Invalidate();
                    if (!m_suppressViewNotify && m_onViewChanged)
                    {
                        m_onViewChanged(GetViewTransform());
                    }
                    return true;
                }

                // Armed but not yet panning: consume to avoid child hover interactions.
                return true;
            }
            break;
        }
        case InputEventType::MouseUp:
        {
            if (event.button != MouseButton::Left)
            {
                break;
            }
            if ((m_panArmed || m_panning) && m_request.purpose == ImageCore::ImagePurpose::FullResolution)
            {
                const bool wasPanning = m_panning;
                m_panning = false;
                m_panArmed = false;
                
                // Release mouse capture
                if (BackplateRef() != nullptr && BackplateRef()->Window() != nullptr)
                {
                    ReleaseCapture();
                }

                // If we never crossed the drag threshold, treat it as a click.
                if (!wasPanning && m_onClick)
                {
                    m_onClick();
                }
                
                return true;
            }
            break;
        }
        case InputEventType::CaptureChanged:
        {
            // If capture is lost while panning, stop panning
            if ((m_panArmed || m_panning) && GetCapture() != BackplateRef()->Window())
            {
                m_panning = false;
                m_panArmed = false;
            }
            break;
        }
        case InputEventType::MouseWheel:
        {
            if (!event.hasPoint)
            {
                break;
            }
            // Only handle zoom for main image (FullResolution)
            if (m_request.purpose != ImageCore::ImagePurpose::FullResolution)
            {
                break;
            }

            // Backplate has already converted coordinates to client/Layout coordinate system
            // lParam now contains coordinates in Layout coordinate system (same as client coordinates)
            POINT pt = event.point;
            const D2D1_RECT_F r = LayoutRect();
#ifdef _DEBUG
            wchar_t dbg[256];
            swprintf_s(dbg, L"[FD2D][Image][MouseWheel] pt=(%d,%d) LayoutRect=[%.1f,%.1f,%.1f,%.1f] zoomScale=%.2f\n",
                pt.x, pt.y, r.left, r.top, r.right, r.bottom, m_zoomScale);
            OutputDebugStringW(dbg);
#endif
            // Check if mouse is within image bounds (both in Layout coordinate system)
            if (static_cast<float>(pt.x) >= r.left &&
                static_cast<float>(pt.x) <= r.right &&
                static_cast<float>(pt.y) >= r.top &&
                static_cast<float>(pt.y) <= r.bottom)
            {
                const short delta = event.wheelDelta;
                const bool shiftPressed = event.modifiers.shift;
                
                // 비율 곱셈 + 누적 방식: 현재 targetZoomScale에 비율을 곱해서 누적
                // Shift: 10% 증가/감소, No Shift: 50% 증가/감소
                const float zoomStep = shiftPressed ? 0.1f : 0.5f;
                const float zoomFactor = (delta > 0) ? (1.0f + zoomStep) : (1.0f / (1.0f + zoomStep));
                const float newZoom = m_targetZoomScale * zoomFactor; // 비율 곱셈 + 누적
                
#ifdef _DEBUG
                swprintf_s(dbg, L"[FD2D][Image][MouseWheel] delta=%d zoomFactor=%.3f target=%.2f -> new=%.2f\n",
                    delta, zoomFactor, m_targetZoomScale, newZoom);
                OutputDebugStringW(dbg);
#endif
                // Always do pointer-based zoom (keep the point under the mouse fixed).
                m_pointerZoomActive = true;
                m_pointerZoomStartZoom = m_zoomScale;
                m_pointerZoomStartPanX = m_panX;
                m_pointerZoomStartPanY = m_panY;
                m_pointerZoomMouseX = static_cast<float>(pt.x);
                m_pointerZoomMouseY = static_cast<float>(pt.y);

                SetZoomScale(newZoom);

                if (!m_suppressViewNotify && m_onViewChanged)
                {
                    m_onViewChanged(GetViewTransform());
                }
                
                return true;
            }
            break;
        }
        default:
            break;
        }

        return Wnd::OnInputEvent(event);
    }

    void Image::OnRenderD3D(ID3D11DeviceContext* context)
    {
        if (!context || !m_backplate)
        {
            return;
        }

        // Advance smooth zoom animation (for GPU path)
        if (m_request.purpose == ImageCore::ImagePurpose::FullResolution)
        {
            AdvanceZoomAnimation(CommonUtil::NowMs());
        }

        ID3D11Device* device = m_backplate->D3DDevice();
        if (!device)
        {
            return;
        }

        (void)EnsureD3DQuadResources(device);

        const D2D1_SIZE_U logicalCs = m_backplate->ClientSize();
        D2D1_SIZE_U cs = m_backplate->RenderSurfaceSize();
        if (cs.width == 0 || cs.height == 0)
        {
            cs = logicalCs;
        }
        if (cs.width == 0 || cs.height == 0 || logicalCs.width == 0 || logicalCs.height == 0)
        {
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
            return;
        }

        // Clip to our own bounds for GPU path (scissor rect).
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

        if (g_rsScissor)
        {
            context->RSSetState(g_rsScissor.Get());
        }
        context->RSSetScissorRects(1, &scissor);

        const auto toNdcX = [cs](float x) { return (x / static_cast<float>(cs.width)) * 2.0f - 1.0f; };
        const auto toNdcY = [cs](float y) { return 1.0f - (y / static_cast<float>(cs.height)) * 2.0f; };

        const auto drawSrvRect = [&](ID3D11ShaderResourceView* srv, const D2D1_RECT_F& rectPx, float opacity, float uMax, float vMax, ID3D11SamplerState* samplerOverride)
        {
            if (!srv || opacity <= 0.0f)
            {
                return;
            }

            // Apply zoom scale and pan offset for GPU path
            D2D1_RECT_F zoomedRect = rectPx;
            const float panRenderX = m_panX * logicalToRender.width;
            const float panRenderY = m_panY * logicalToRender.height;
            if (m_zoomScale != 1.0f)
            {
                const float centerX = (rectPx.left + rectPx.right) * 0.5f;
                const float centerY = (rectPx.top + rectPx.bottom) * 0.5f;
                const float width = rectPx.right - rectPx.left;
                const float height = rectPx.bottom - rectPx.top;
                const float scaledWidth = width * m_zoomScale;
                const float scaledHeight = height * m_zoomScale;
                zoomedRect.left = centerX - scaledWidth * 0.5f + panRenderX;
                zoomedRect.right = zoomedRect.left + scaledWidth;
                zoomedRect.top = centerY - scaledHeight * 0.5f + panRenderY;
                zoomedRect.bottom = zoomedRect.top + scaledHeight;
            }
            else if (std::abs(m_panX) > 0.001f || std::abs(m_panY) > 0.001f)
            {
                // Apply pan even when not zoomed (though this shouldn't normally happen)
                zoomedRect.left += panRenderX;
                zoomedRect.right += panRenderX;
                zoomedRect.top += panRenderY;
                zoomedRect.bottom += panRenderY;
            }

            const float l = toNdcX(zoomedRect.left);
            const float r = toNdcX(zoomedRect.right);
            const float t = toNdcY(zoomedRect.top);
            const float b = toNdcY(zoomedRect.bottom);

            // Update vertex buffer
            D3D11_MAPPED_SUBRESOURCE mapped {};
            if (SUCCEEDED(context->Map(g_vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                auto* v = reinterpret_cast<QuadVertex*>(mapped.pData);
                v[0] = { l, t, 0.0f, 0.0f };
                v[1] = { r, t, uMax, 0.0f };
                v[2] = { l, b, 0.0f, vMax };
                v[3] = { r, b, uMax, vMax };
                context->Unmap(g_vb.Get(), 0);
            }

            UINT stride = sizeof(QuadVertex);
            UINT offset = 0;
            context->IASetInputLayout(g_inputLayout.Get());
            context->IASetVertexBuffers(0, 1, g_vb.GetAddressOf(), &stride, &offset);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

            context->VSSetShader(g_vs.Get(), nullptr, 0);
            context->PSSetShader(g_ps.Get(), nullptr, 0);
            ID3D11SamplerState* samp = samplerOverride
                ? samplerOverride
                : (m_highQualitySampling ? g_samplerLinear.Get() : g_samplerPoint.Get());
            context->PSSetSamplers(0, 1, &samp);
            context->PSSetShaderResources(0, 1, &srv);

            float blendFactor[4] = { 0,0,0,0 };
            context->OMSetBlendState(g_blend.Get(), blendFactor, 0xFFFFFFFF);

            // Reuse global dynamic constant buffer.
            if (g_cb)
            {
                D3D11_MAPPED_SUBRESOURCE mappedCb {};
                if (SUCCEEDED(context->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedCb)))
                {
                    float* p = reinterpret_cast<float*>(mappedCb.pData);
                    p[0] = opacity;
                    p[1] = 0.0f;
                    p[2] = 0.0f;
                    p[3] = 0.0f;
                    context->Unmap(g_cb.Get(), 0);
                }

                ID3D11Buffer* cbp = g_cb.Get();
                context->PSSetConstantBuffers(0, 1, &cbp);
            }

            context->Draw(4, 0);

            // Unbind SRV to avoid hazards if it becomes a render target later
            ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
            context->PSSetShaderResources(0, 1, nullSrv);
        };

        // Background/letterbox rendering belongs to container panes.
        // This image control renders only image content.

        if (!m_gpuSrv || m_gpuWidth == 0 || m_gpuHeight == 0)
        {
            return;
        }

        // If a CPU-decoded image for the current selection is pending, avoid drawing the old GPU SRV this frame.
        // Backplate renders D3D first, then D2D; so this prevents a 1-frame "show-through" when the new CPU
        // image has transparency.
        //
        // NOTE: m_pending* is written by a worker thread; guard access to avoid data races.
        bool hasPendingCpuForCurrent = false;
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            hasPendingCpuForCurrent = (m_pendingBlocks
                && IsCpuBgra8DxgiFormat(m_pendingFormat)
                && m_pendingSourcePath == m_filePath);
        }
        if (hasPendingCpuForCurrent)
        {
            return;
        }

        // Aspect-fit dest rect in pixels
        const float bmpW = static_cast<float>(m_gpuWidth);
        const float bmpH = static_cast<float>(m_gpuHeight);
        const float bmpAspect = bmpW / bmpH;
        const float layoutAspect = layoutW / layoutH;

        D2D1_RECT_F dest = layout;
        if (bmpAspect > layoutAspect)
        {
            const float scaledH = layoutW / bmpAspect;
            const float yOff = (layoutH - scaledH) * 0.5f;
            dest.top = layout.top + yOff;
            dest.bottom = dest.top + scaledH;
        }
        else
        {
            const float scaledW = layoutH * bmpAspect;
            const float xOff = (layoutW - scaledW) * 0.5f;
            dest.left = layout.left + xOff;
            dest.right = dest.left + scaledW;
        }

        // Checkerboard for alpha visualization (behind the image, in the image dest rect only).
        if (m_alphaCheckerboardEnabled && g_checkerSrv && g_samplerWrap)
        {
            const float w = (std::max)(1.0f, dest.right - dest.left);
            const float h = (std::max)(1.0f, dest.bottom - dest.top);
            const float uMax = w / 64.0f;
            const float vMax = h / 64.0f;
            drawSrvRect(g_checkerSrv.Get(), dest, 1.0f, uMax, vMax, g_samplerWrap.Get());
        }

        // No cross-fade: draw the currently available SRV (can be the previous image while the next loads).
        drawSrvRect(m_gpuSrv.Get(), dest, 1.0f, 1.0f, 1.0f, nullptr);

        // Restore previous raster/scissor state so we don't affect other controls.
        if (prevScissorCount > 0 && !prevScissors.empty())
        {
            context->RSSetScissorRects(prevScissorCount, prevScissors.data());
        }
        else
        {
            // Reset to no scissor rects (count=0 is invalid for RSSetScissorRects).
            // Use full-viewport scissor as a safe default.
            D3D11_RECT full { 0, 0, static_cast<LONG>(cs.width), static_cast<LONG>(cs.height) };
            context->RSSetScissorRects(1, &full);
        }
        context->RSSetState(prevRs.Get());
    }
    void Image::SetHighQualitySampling(bool enabled)
    {
        if (m_highQualitySampling == enabled)
        {
            return;
        }

        m_highQualitySampling = enabled;
        Invalidate();
    }

    void Image::ToggleSamplingQuality()
    {
        m_highQualitySampling = !m_highQualitySampling;
        Invalidate();
    }
}

