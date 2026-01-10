#include "Image.h"
#include "Backplate.h"
#include "Spinner.h"
#include "Core.h"  // For GetSupportedD2DVersion
#include "../ImageCore/ImageRequest.h"
#include <algorithm>
#include <windowsx.h>
#include <cmath>
#include <d3dcompiler.h>
#include <d2d1_1.h>  // For Direct2D 1.1 interpolation modes
#include <cwctype>

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

        static Microsoft::WRL::ComPtr<ID3D11VertexShader> g_vs {};
        static Microsoft::WRL::ComPtr<ID3D11PixelShader> g_ps {};
        static Microsoft::WRL::ComPtr<ID3D11InputLayout> g_inputLayout {};
        static Microsoft::WRL::ComPtr<ID3D11Buffer> g_vb {};
        static Microsoft::WRL::ComPtr<ID3D11Buffer> g_cb {};
        static Microsoft::WRL::ComPtr<ID3D11SamplerState> g_sampler {};
        static Microsoft::WRL::ComPtr<ID3D11BlendState> g_blend {};
        static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> g_backdropSrv {};

        struct SrvCacheEntry
        {
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv {};
            UINT width { 0 };
            UINT height { 0 };
            std::list<std::wstring>::iterator lruIt {};
        };

        static std::mutex g_srvCacheMutex;
        static std::unordered_map<std::wstring, SrvCacheEntry> g_srvCache;
        static std::list<std::wstring> g_srvCacheLru;
        static size_t g_srvCacheCapacity = 64; // simple entry-count cap

        static bool TryGetSrvFromCache(const std::wstring& key, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv, UINT& outW, UINT& outH)
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
            return true;
        }

        static void PutSrvToCache(const std::wstring& key, const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv, UINT w, UINT h)
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
            if (g_vs && g_ps && g_inputLayout && g_vb && g_sampler && g_blend)
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

            // Create sampler state with highest quality settings for optimal image quality
            D3D11_SAMPLER_DESC sd {};
            sd.Filter = D3D11_FILTER_ANISOTROPIC;  // Highest quality filtering mode
            sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            
            // Query device feature level to determine maximum supported anisotropy
            D3D_FEATURE_LEVEL featureLevel = device->GetFeatureLevel();
            UINT maxAnisotropy = 16; // Default: D3D11 supports up to 16
            
            // Most modern hardware supports 16x anisotropy, which is the maximum
            // Some older hardware might support less, but we'll use 16 as it's the best quality
            // D3D11CreateDevice will fail if the requested anisotropy is not supported,
            // so we'll use the maximum standard value
            if (featureLevel >= D3D_FEATURE_LEVEL_10_0)
            {
                // Feature Level 10.0+ supports up to 16x anisotropy
                maxAnisotropy = 16;
            }
            else
            {
                // Feature Level 9.x supports up to 4x or 16x depending on driver
                // Use 16, but CreateSamplerState will clamp to device maximum if needed
                maxAnisotropy = 16;
            }
            
            sd.MaxAnisotropy = maxAnisotropy;
            sd.MinLOD = 0.0f;  // Use highest resolution mip level (best quality)
            sd.MaxLOD = D3D11_FLOAT32_MAX;  // Allow all mip levels for proper mipmapping
            sd.MipLODBias = 0.0f;  // No bias (neutral, best quality)
            sd.ComparisonFunc = D3D11_COMPARISON_NEVER;  // No comparison sampling (standard texture sampling)
            sd.BorderColor[0] = 0.0f;  // Black border (used with BORDER address mode, not CLAMP)
            sd.BorderColor[1] = 0.0f;
            sd.BorderColor[2] = 0.0f;
            sd.BorderColor[3] = 0.0f;
            
            hr = device->CreateSamplerState(&sd, &g_sampler);
            if (FAILED(hr))
            {
                // Fallback: Try with reduced anisotropy if 16x is not supported
                if (maxAnisotropy > 1)
                {
                    sd.MaxAnisotropy = 8;
                    hr = device->CreateSamplerState(&sd, &g_sampler);
                }
                if (FAILED(hr) && maxAnisotropy > 1)
                {
                    sd.MaxAnisotropy = 4;
                    hr = device->CreateSamplerState(&sd, &g_sampler);
                }
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

            // 1x1 backdrop SRV for drawing a stable letterbox under aspect-fit images.
            if (!g_backdropSrv)
            {
                // Match the Backplate clear (dark neutral gray with tiny blue bias).
                // IMPORTANT: this is AARRGGBB; in memory it's BGRA.
                // (R,G,B)=(0x17,0x17,0x1A) => bytes: 1A 17 17 FF
                const UINT32 backdrop = 0xFF17171A;

                D3D11_TEXTURE2D_DESC td {};
                td.Width = 1;
                td.Height = 1;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_IMMUTABLE;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                D3D11_SUBRESOURCE_DATA init {};
                init.pSysMem = &backdrop;
                init.SysMemPitch = sizeof(backdrop);

                Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
                hr = device->CreateTexture2D(&td, &init, &tex);
                if (FAILED(hr))
                {
                    return hr;
                }

                hr = device->CreateShaderResourceView(tex.Get(), nullptr, &g_backdropSrv);
                if (FAILED(hr))
                {
                    return hr;
                }
            }

            return S_OK;
        }

        static unsigned long long NowMs()
        {
            return static_cast<unsigned long long>(GetTickCount64());
        }

        static float Clamp01(float v)
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

        static std::wstring NormalizePath(const std::wstring& p)
        {
            if (p.empty())
            {
                return {};
            }

            std::wstring out;
            DWORD needed = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
            if (needed > 0)
            {
                out.resize(static_cast<size_t>(needed));
                DWORD written = GetFullPathNameW(p.c_str(), needed, &out[0], nullptr);
                if (written > 0 && written < needed)
                {
                    out.resize(static_cast<size_t>(written));
                }
                else if (written == 0)
                {
                    out = p;
                }
            }
            else
            {
                out = p;
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

        // (debug-only temp-file logging removed)
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
        const std::wstring normalized = NormalizePath(filePath);

        // When switching main images, preserve zoom/pan (comparison workflow),
        // but stop any in-flight interaction/animation state.
        if (!normalized.empty() && normalized != m_filePath && m_request.purpose == ImageCore::ImagePurpose::FullResolution)
        {
            // IMPORTANT: If the previous image was on the GPU path (DDS SRV) and the next image is CPU-decoded
            // (e.g., PNG with alpha), the D3D pass would keep drawing the old SRV behind the new D2D bitmap.
            // Clear GPU resources on selection change; the SRV cache fast-path will repopulate immediately when applicable.
            m_gpuSrv.Reset();
            m_prevGpuSrv.Reset();
            m_gpuWidth = 0;
            m_gpuHeight = 0;

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
            m_lastZoomAnimMs = NowMs();
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
            m_pendingWicBitmap.Reset();
            m_pendingScratchImage.reset();
            m_pendingSourcePath.clear();
        }

        // Selection changed: clear any in-flight token so we can start a new request on next render.
        m_inflightToken.store(0);

        // A) Fast reselect path: if SRV is cached, swap immediately (no disk/decode/upload).
        if (m_request.purpose == ImageCore::ImagePurpose::FullResolution && m_backplate)
        {
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cachedSrv;
            UINT cw = 0, ch = 0;
            if (TryGetSrvFromCache(normalized, cachedSrv, cw, ch))
            {
                // Setup cross-fade on GPU path.
                // NOTE: The cache doesn't currently store DXGI_FORMAT metadata, so use a conservative heuristic:
                // for common alpha formats (png/tga/gif), avoid crossfading to prevent "previous image show-through".
                bool incomingHasAlpha = false;
                const size_t lastSlash = normalized.find_last_of(L"\\/");
                const size_t lastDot = normalized.find_last_of(L'.');
                if (lastDot != std::wstring::npos && (lastSlash == std::wstring::npos || lastDot > lastSlash))
                {
                    const std::wstring ext = normalized.substr(lastDot);
                    if (_wcsicmp(ext.c_str(), L".png") == 0 ||
                        _wcsicmp(ext.c_str(), L".tga") == 0 ||
                        _wcsicmp(ext.c_str(), L".gif") == 0)
                    {
                        incomingHasAlpha = true;
                    }
                }

                if (!incomingHasAlpha && m_gpuSrv)
                {
                    m_prevGpuSrv = m_gpuSrv;
                    m_fadeStartMs = NowMs();
                }
                else
                {
                    m_prevGpuSrv.Reset();
                    m_fadeStartMs = 0;
                }

                m_gpuSrv = cachedSrv;
                m_gpuWidth = cw;
                m_gpuHeight = ch;
                m_loadedFilePath = normalized;
                m_filePath = normalized;

                // Cancel CPU path bitmaps for main image
                m_bitmap.Reset();
                m_prevBitmap.Reset();

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

        m_currentHandle = ImageCore::ImageLoader::Instance().Request(
            m_request,
            [this, token, requestedPath](HRESULT hr, Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap, std::unique_ptr<DirectX::ScratchImage> scratchImage)
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

                OnImageLoaded(requestedPath, hr, wicBitmap, std::move(scratchImage));
            });
    }

    void Image::OnImageLoaded(
        const std::wstring& sourcePath,
        HRESULT hr,
        Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap,
        std::unique_ptr<DirectX::ScratchImage> scratchImage)
    {
        m_currentHandle = 0;

        const std::wstring normalizedSource = NormalizePath(sourcePath);

        // 변환은 OnRender에서 render target을 사용하여 수행
        // 여기서는 저장만 하고 Invalidate로 OnRender 호출 유도
        if (SUCCEEDED(hr) && (wicBitmap || scratchImage))
        {
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                m_pendingWicBitmap = wicBitmap;
                m_pendingScratchImage = std::move(scratchImage);
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

    HRESULT Image::ConvertToD2DBitmap(
        ID2D1RenderTarget* target,
        const std::wstring& sourcePath,
        Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap,
        std::unique_ptr<DirectX::ScratchImage> scratchImage)
    {
        if (target == nullptr)
        {
            return E_INVALIDARG;
        }

        Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dBitmap;
        HRESULT hr = E_FAIL;

        // If the incoming image likely has transparency (alpha), avoid cross-fading with the previous image.
        // Cross-fade + alpha makes the previous image "show through" transparent pixels, which is visually wrong.
        bool incomingHasAlpha = false;
        if (scratchImage)
        {
            incomingHasAlpha = DirectX::HasAlpha(scratchImage->GetMetadata().format);
        }
        else if (wicBitmap)
        {
            WICPixelFormatGUID fmt {};
            if (SUCCEEDED(wicBitmap->GetPixelFormat(&fmt)))
            {
                incomingHasAlpha =
                    IsEqualGUID(fmt, GUID_WICPixelFormat32bppPBGRA) ||
                    IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) ||
                    IsEqualGUID(fmt, GUID_WICPixelFormat32bppPRGBA) ||
                    IsEqualGUID(fmt, GUID_WICPixelFormat32bppRGBA) ||
                    IsEqualGUID(fmt, GUID_WICPixelFormat64bppPRGBA) ||
                    IsEqualGUID(fmt, GUID_WICPixelFormat64bppRGBA) ||
                    IsEqualGUID(fmt, GUID_WICPixelFormat128bppPRGBAFloat) ||
                    IsEqualGUID(fmt, GUID_WICPixelFormat128bppRGBAFloat);
            }
        }

        // DirectXTex 경로: ScratchImage를 직접 사용
        if (scratchImage)
        {
            const DirectX::Image* image = scratchImage->GetImage(0, 0, 0);
            if (image && image->pixels)
            {
                D2D1_BITMAP_PROPERTIES props = {};
                props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
                props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
                props.dpiX = 96.0f;
                props.dpiY = 96.0f;

                D2D1_SIZE_U size = D2D1::SizeU(
                    static_cast<UINT32>(image->width),
                    static_cast<UINT32>(image->height));

                hr = target->CreateBitmap(
                    size,
                    image->pixels,
                    static_cast<UINT32>(image->rowPitch),
                    &props,
                    &d2dBitmap);
            }
        }
        // WIC 경로: WIC bitmap을 사용
        else if (wicBitmap)
        {
            hr = target->CreateBitmapFromWicBitmap(wicBitmap.Get(), nullptr, &d2dBitmap);
        }

        if (SUCCEEDED(hr) && d2dBitmap)
        {
            // Start cross-fade if the displayed bitmap is changing.
            if (!incomingHasAlpha && m_bitmap && m_loadedFilePath != sourcePath)
            {
                m_prevBitmap = m_bitmap;
                m_prevLoadedFilePath = m_loadedFilePath;
                m_fadeStartMs = NowMs();
            }
            else
            {
                // No meaningful fade (first image or same source).
                m_prevBitmap.Reset();
                m_prevLoadedFilePath.clear();
                m_fadeStartMs = 0;
            }

            m_bitmap = d2dBitmap;
            m_loadedFilePath = sourcePath;
        }

        return hr;
    }

    void Image::OnRender(ID2D1RenderTarget* target)
    {
        if (target == nullptr)
        {
            return;
        }

        // Advance smooth zoom animation
        if (m_request.purpose == ImageCore::ImagePurpose::FullResolution)
        {
            AdvanceZoomAnimation(NowMs());
        }

        const bool gpuActive = (m_request.purpose == ImageCore::ImagePurpose::FullResolution &&
            m_backplate && m_backplate->D3DDevice() != nullptr && m_gpuSrv);

        // Ensure a stable backdrop for the main image region so we never "see through" to the window clear
        // during fades/loads (especially with aspect-fit letterboxing).
        //
        // IMPORTANT: When GPU path is active, the main image is rendered in the D3D pass. The D2D pass runs
        // afterwards, so drawing a backdrop here would cover the GPU image.
        if (m_request.purpose == ImageCore::ImagePurpose::FullResolution && !gpuActive)
        {
            if (!m_backdropBrush)
            {
                // Match Backplate clear (dark neutral gray with tiny blue bias).
                (void)target->CreateSolidColorBrush(D2D1::ColorF(0.09f, 0.09f, 0.10f, 1.0f), &m_backdropBrush);
            }
            if (m_backdropBrush)
            {
                target->FillRectangle(LayoutRect(), m_backdropBrush.Get());
            }
        }

        // 변환 대기 중인 이미지가 있으면 D2D1Bitmap으로 변환
        Microsoft::WRL::ComPtr<IWICBitmapSource> pendingWic;
        std::unique_ptr<DirectX::ScratchImage> pendingScratch;
        std::wstring pendingSourcePath;
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            pendingWic = m_pendingWicBitmap;
            pendingScratch = std::move(m_pendingScratchImage);
            m_pendingWicBitmap.Reset();
            pendingSourcePath = m_pendingSourcePath;
            m_pendingSourcePath.clear();
        }

        if (pendingWic || pendingScratch)
        {
            // Only swap if this pending image still matches the current requested source.
            // Otherwise, drop it to avoid "wrong image stuck" due to out-of-order completion.
            if (!pendingSourcePath.empty() && pendingSourcePath == m_filePath)
            {
                // Try GPU path for main image: if scratch is GPU-compressed DDS, upload to SRV and render via D3D.
                bool usedGpu = false;
                bool applied = false;
                if (pendingScratch && m_request.purpose == ImageCore::ImagePurpose::FullResolution && m_backplate)
                {
                    const DirectX::Image* img = pendingScratch->GetImage(0, 0, 0);
                    if (img && DirectX::IsCompressed(img->format))
                    {
                        ID3D11Device* dev = m_backplate->D3DDevice();
                        if (dev && !m_forceCpuDecode.load())
                        {
                            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
                            const auto& meta = pendingScratch->GetMetadata();
                            HRESULT hrSrv = DirectX::CreateShaderResourceView(
                                dev,
                                pendingScratch->GetImages(),
                                pendingScratch->GetImageCount(),
                                meta,
                                &srv);
                            if (SUCCEEDED(hrSrv) && srv)
                            {
                                // Cross-fade for GPU path.
                                // If the incoming format has alpha, avoid crossfading to prevent "show-through"
                                // of the previous image behind transparent pixels.
                                const bool incomingHasAlpha = DirectX::HasAlpha(meta.format);
                                if (!incomingHasAlpha && m_gpuSrv)
                                {
                                    m_prevGpuSrv = m_gpuSrv;
                                    m_fadeStartMs = NowMs();
                                }
                                else
                                {
                                    m_prevGpuSrv.Reset();
                                    m_fadeStartMs = 0;
                                }

                                m_gpuSrv = srv;
                                m_gpuWidth = static_cast<UINT>(meta.width);
                                m_gpuHeight = static_cast<UINT>(meta.height);
                                m_loadedFilePath = pendingSourcePath;

                                // A) Cache SRV for fast reselect
                                PutSrvToCache(pendingSourcePath, m_gpuSrv, m_gpuWidth, m_gpuHeight);

                                // Drop CPU bitmaps for main image when using GPU path
                                m_bitmap.Reset();
                                m_prevBitmap.Reset();

                                usedGpu = true;
                                applied = true;
                            }
                            else
                            {
                                UNREFERENCED_PARAMETER(hrSrv);
                                // SRV creation failed (can happen intermittently). We cannot display compressed pixels via D2D.
                                // Fall back deterministically: request a CPU-decompressed decode for this selection.
                                m_forceCpuDecode.store(true);
                                m_loading.store(false);
                                RequestImageLoad();
                                usedGpu = true; // prevent ConvertToD2DBitmap with compressed data
                            }
                        }
                        else
                        {
                            // No GPU device (or GPU path disabled). Request a CPU-decompressed decode.
                            m_forceCpuDecode.store(true);
                            m_loading.store(false);
                            RequestImageLoad();
                            usedGpu = true; // prevent ConvertToD2DBitmap with compressed data
                        }
                    }
                }

                if (!usedGpu)
                {
                    const HRESULT hrBmp = ConvertToD2DBitmap(target, pendingSourcePath, pendingWic, std::move(pendingScratch));
                    if (FAILED(hrBmp))
                    {
                        UNREFERENCED_PARAMETER(hrBmp);
                        {
                            std::lock_guard<std::mutex> lock(m_pendingMutex);
                            m_failedFilePath = pendingSourcePath;
                            m_failedHr = hrBmp;
                        }
                        // Conversion failure completes the in-flight request (stop spinner).
                        m_loading.store(false);
                    }
                    else
                    {
                        applied = true;
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

        const auto drawBitmapAspectFit = [this, target, &computeAspectFitDestRect](ID2D1Bitmap* bmp, float opacity)
        {
            if (bmp == nullptr || opacity <= 0.0f)
            {
                return;
            }

            const auto bitmapSize = bmp->GetSize();
            const auto layoutRect = LayoutRect();

            const D2D1_RECT_F sourceRect = D2D1::RectF(0.0f, 0.0f, bitmapSize.width, bitmapSize.height);
            const D2D1_RECT_F destRect = computeAspectFitDestRect(layoutRect, bitmapSize);

            // Select interpolation mode based on Direct2D version and zoom level for optimal quality
            // Use the highest quality mode supported by the runtime Direct2D version
            D2D1_BITMAP_INTERPOLATION_MODE interpMode = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
            
            // Get current Direct2D version to select the best available option
            FD2D::D2DVersion d2dVersion = FD2D::Core::GetSupportedD2DVersion();
            
            if (m_request.purpose == ImageCore::ImagePurpose::FullResolution)
            {
                // Direct2D 1.1+ supports CUBIC and MULTI_SAMPLE_LINEAR
                if (d2dVersion >= FD2D::D2DVersion::D2D1_1)
                {
                    if (m_zoomScale > 1.0f)
                    {
                        // Zoomed in: use cubic interpolation for smooth upscaling (16-sample high quality)
                        #ifdef D2D1_BITMAP_INTERPOLATION_MODE_CUBIC
                        interpMode = D2D1_BITMAP_INTERPOLATION_MODE_CUBIC;
                        #endif
                    }
                    else if (m_zoomScale < 1.0f)
                    {
                        // Zoomed out: use multi-sample linear for smooth downscaling (anti-aliased)
                        #ifdef D2D1_BITMAP_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR
                        interpMode = D2D1_BITMAP_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR;
                        #else
                        // Fallback to cubic if multi-sample linear is not available
                        #ifdef D2D1_BITMAP_INTERPOLATION_MODE_CUBIC
                        interpMode = D2D1_BITMAP_INTERPOLATION_MODE_CUBIC;
                        #endif
                        #endif
                    }
                    else
                    {
                        // Normal size: use cubic for high quality rendering
                        #ifdef D2D1_BITMAP_INTERPOLATION_MODE_CUBIC
                        interpMode = D2D1_BITMAP_INTERPOLATION_MODE_CUBIC;
                        #endif
                    }
                }
                // Direct2D 1.0 fallback: use LINEAR (already set as default)
            }

            target->DrawBitmap(
                bmp,
                destRect,
                opacity,
                interpMode,
                sourceRect);
        };

        // Cross-fade (if we have a previous bitmap and fade has started).
        bool isFading = false;
        float fadeT = 1.0f;
        if (m_prevBitmap && m_fadeStartMs != 0 && m_fadeDurationMs > 0)
        {
            const unsigned long long elapsed = NowMs() - m_fadeStartMs;
            fadeT = Clamp01(static_cast<float>(elapsed) / static_cast<float>(m_fadeDurationMs));
            isFading = fadeT < 1.0f;
        }

        // GPU path cross-fade is rendered in OnRenderD3D, but we still need to drive the animation by
        // requesting redraws while the fade is active.
        bool isGpuFading = false;
        if (m_prevGpuSrv && m_fadeStartMs != 0 && m_fadeDurationMs > 0)
        {
            const unsigned long long elapsed = NowMs() - m_fadeStartMs;
            const float gpuT = Clamp01(static_cast<float>(elapsed) / static_cast<float>(m_fadeDurationMs));
            isGpuFading = gpuT < 1.0f;
        }

        if (m_bitmap)
        {
            if (isFading && m_prevBitmap)
            {
                // Crossfade:
                // Fade OUT the previous image while fading IN the new image.
                // This avoids "seeing the previous image through" transparent pixels of the new image.
                drawBitmapAspectFit(m_prevBitmap.Get(), 1.0f - fadeT);
                drawBitmapAspectFit(m_bitmap.Get(), fadeT);
            }
            else
            {
                // Fade completed: drop previous bitmap.
                if (m_prevBitmap)
                {
                    m_prevBitmap.Reset();
                    m_prevLoadedFilePath.clear();
                    m_fadeStartMs = 0;
                }
                drawBitmapAspectFit(m_bitmap.Get(), 1.0f);
            }
        }

        // Loading spinner overlay (only while an actual request is in-flight).
        // This avoids "infinite spinner" states on decode failure; we keep showing the previous image instead.
        const bool shouldShowSpinner = m_loadingSpinnerEnabled && m_loading.load();
        if (m_loadingSpinner)
        {
            m_loadingSpinner->SetActive(shouldShowSpinner);
        }

        Wnd::OnRender(target);

        // Drive fade animations by invalidating while active, but cap to ~60fps to avoid burning CPU.
        // Animation pacing is handled by the application loop (60fps tick) via Backplate::RequestAnimationFrame().
        if (isFading || isGpuFading)
        {
            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestAnimationFrame();
            }
        }
    }

    void Image::SetZoomScale(float scale)
    {
        constexpr float kMinZoom = 0.1f;
        constexpr float kMaxZoom = 10.0f;
        m_targetZoomScale = std::max(kMinZoom, std::min(kMaxZoom, scale));
        m_lastZoomAnimMs = NowMs();
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
        m_lastZoomAnimMs = NowMs();
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
    }

    bool Image::OnMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_LBUTTONDOWN:
        {
            // Backplate has already converted coordinates to client/Layout coordinate system
            // lParam now contains coordinates in Layout coordinate system (same as client coordinates)
            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
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
        case WM_MOUSEMOVE:
        {
            if ((m_panArmed || m_panning) && m_request.purpose == ImageCore::ImagePurpose::FullResolution)
            {
                // Backplate has already converted coordinates to client/Layout coordinate system
                // When mouse is captured, Backplate uses GetCursorPos() and converts to client coordinates
                // lParam now contains coordinates in Layout coordinate system (same as client coordinates)
                POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                
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
                    m_pointerZoomActive = false;
                    Invalidate();
                    return true;
                }

                // Armed but not yet panning: consume to avoid child hover interactions.
                return true;
            }
            break;
        }
        case WM_LBUTTONUP:
        {
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
        case WM_CAPTURECHANGED:
        {
            // If capture is lost while panning, stop panning
            if ((m_panArmed || m_panning) && GetCapture() != BackplateRef()->Window())
            {
                m_panning = false;
                m_panArmed = false;
            }
            break;
        }
        case WM_MOUSEWHEEL:
        {
            // Only handle zoom for main image (FullResolution)
            if (m_request.purpose != ImageCore::ImagePurpose::FullResolution)
            {
                break;
            }

            // Backplate has already converted coordinates to client/Layout coordinate system
            // lParam now contains coordinates in Layout coordinate system (same as client coordinates)
            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const D2D1_RECT_F r = LayoutRect();
#ifdef _DEBUG
            wchar_t dbg[256];
            swprintf_s(dbg, L"[FD2D][Image][WM_MOUSEWHEEL] pt=(%d,%d) LayoutRect=[%.1f,%.1f,%.1f,%.1f] zoomScale=%.2f\n",
                pt.x, pt.y, r.left, r.top, r.right, r.bottom, m_zoomScale);
            OutputDebugStringW(dbg);
#endif
            // Check if mouse is within image bounds (both in Layout coordinate system)
            if (static_cast<float>(pt.x) >= r.left &&
                static_cast<float>(pt.x) <= r.right &&
                static_cast<float>(pt.y) >= r.top &&
                static_cast<float>(pt.y) <= r.bottom)
            {
                const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
                const bool shiftPressed = (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0;
                
                // 비율 곱셈 + 누적 방식: 현재 targetZoomScale에 비율을 곱해서 누적
                // Shift: 10% 증가/감소, No Shift: 50% 증가/감소
                const float zoomStep = shiftPressed ? 0.1f : 0.5f;
                const float zoomFactor = (delta > 0) ? (1.0f + zoomStep) : (1.0f / (1.0f + zoomStep));
                const float newZoom = m_targetZoomScale * zoomFactor; // 비율 곱셈 + 누적
                
#ifdef _DEBUG
                swprintf_s(dbg, L"[FD2D][Image][WM_MOUSEWHEEL] delta=%d zoomFactor=%.3f target=%.2f -> new=%.2f\n",
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
                
                return true;
            }
            break;
        }
        default:
            break;
        }

        return Wnd::OnMessage(message, wParam, lParam);
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
            AdvanceZoomAnimation(NowMs());
        }

        ID3D11Device* device = m_backplate->D3DDevice();
        if (!device)
        {
            return;
        }

        (void)EnsureD3DQuadResources(device);

        const D2D1_SIZE_U cs = m_backplate->ClientSize();
        if (cs.width == 0 || cs.height == 0)
        {
            return;
        }

        const D2D1_RECT_F layout = LayoutRect();
        const float layoutW = layout.right - layout.left;
        const float layoutH = layout.bottom - layout.top;
        if (!(layoutW > 0.0f && layoutH > 0.0f))
        {
            return;
        }

        const auto toNdcX = [cs](float x) { return (x / static_cast<float>(cs.width)) * 2.0f - 1.0f; };
        const auto toNdcY = [cs](float y) { return 1.0f - (y / static_cast<float>(cs.height)) * 2.0f; };

        const auto drawSrvRect = [&](ID3D11ShaderResourceView* srv, const D2D1_RECT_F& rectPx, float opacity)
        {
            if (!srv || opacity <= 0.0f)
            {
                return;
            }

            // Apply zoom scale and pan offset for GPU path
            D2D1_RECT_F zoomedRect = rectPx;
            if (m_zoomScale != 1.0f)
            {
                const float centerX = (rectPx.left + rectPx.right) * 0.5f;
                const float centerY = (rectPx.top + rectPx.bottom) * 0.5f;
                const float width = rectPx.right - rectPx.left;
                const float height = rectPx.bottom - rectPx.top;
                const float scaledWidth = width * m_zoomScale;
                const float scaledHeight = height * m_zoomScale;
                zoomedRect.left = centerX - scaledWidth * 0.5f + m_panX;
                zoomedRect.right = zoomedRect.left + scaledWidth;
                zoomedRect.top = centerY - scaledHeight * 0.5f + m_panY;
                zoomedRect.bottom = zoomedRect.top + scaledHeight;
            }
            else if (std::abs(m_panX) > 0.001f || std::abs(m_panY) > 0.001f)
            {
                // Apply pan even when not zoomed (though this shouldn't normally happen)
                zoomedRect.left += m_panX;
                zoomedRect.right += m_panX;
                zoomedRect.top += m_panY;
                zoomedRect.bottom += m_panY;
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
                v[1] = { r, t, 1.0f, 0.0f };
                v[2] = { l, b, 0.0f, 1.0f };
                v[3] = { r, b, 1.0f, 1.0f };
                context->Unmap(g_vb.Get(), 0);
            }

            UINT stride = sizeof(QuadVertex);
            UINT offset = 0;
            context->IASetInputLayout(g_inputLayout.Get());
            context->IASetVertexBuffers(0, 1, g_vb.GetAddressOf(), &stride, &offset);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

            context->VSSetShader(g_vs.Get(), nullptr, 0);
            context->PSSetShader(g_ps.Get(), nullptr, 0);
            context->PSSetSamplers(0, 1, g_sampler.GetAddressOf());
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

        // Paint a stable letterbox backdrop for the main image region so window background won't "peek through"
        // during loading/transitions.
        if (m_request.purpose == ImageCore::ImagePurpose::FullResolution && g_backdropSrv)
        {
            drawSrvRect(g_backdropSrv.Get(), layout, 1.0f);
        }

        // Only draw the image SRV if it corresponds to the currently requested source.
        // Otherwise we'd draw a stale previous SRV behind CPU-decoded (alpha) images.
        if (m_loadedFilePath != m_filePath)
        {
            return;
        }

        if (!m_gpuSrv || m_gpuWidth == 0 || m_gpuHeight == 0)
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

        bool isFading = false;
        float fadeT = 1.0f;
        if (m_prevGpuSrv && m_fadeStartMs != 0 && m_fadeDurationMs > 0)
        {
            const unsigned long long elapsed = NowMs() - m_fadeStartMs;
            fadeT = Clamp01(static_cast<float>(elapsed) / static_cast<float>(m_fadeDurationMs));
            isFading = fadeT < 1.0f;
        }

        if (isFading && m_prevGpuSrv)
        {
            // Crossfade:
            // Fade OUT the previous image while fading IN the new image.
            // This avoids "seeing the previous image through" transparent pixels of the new image.
            drawSrvRect(m_prevGpuSrv.Get(), dest, 1.0f - fadeT);
            drawSrvRect(m_gpuSrv.Get(), dest, fadeT);
        }
        else
        {
            if (m_prevGpuSrv)
            {
                m_prevGpuSrv.Reset();
                m_fadeStartMs = 0;
            }
            drawSrvRect(m_gpuSrv.Get(), dest, 1.0f);
        }
    }
}



