#include "Image.h"
#include "Backplate.h"
#include "Spinner.h"
#include <algorithm>
#include <windowsx.h>
#include <cmath>
#include <d3dcompiler.h>

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

            D3D11_SAMPLER_DESC sd {};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            hr = device->CreateSamplerState(&sd, &g_sampler);
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
                // Match the Backplate clear (DarkSlateGray-ish).
                const UINT32 backdrop = 0xFF2F4F4F; // BGRA bytes: 4F 4F 2F FF

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
        // Thumbnail/Preview 용도에서는 targetSize를 fixed desired size로 사용 (thumb strip 레이아웃을 위해)
        if ((m_request.purpose == ImageCore::ImagePurpose::Thumbnail || m_request.purpose == ImageCore::ImagePurpose::Preview) &&
            (m_request.targetSize.w > 0.0f && m_request.targetSize.h > 0.0f))
        {
            // StackPanel(Horizontal)은 childRect의 height를 childArea.h(=윈도우 높이)에 맞추기 때문에,
            // height가 줄어들면 Image::OnRender의 aspect-fit 로직이 셀 내부 여백을 크게 만들 수 있다.
            // 썸네일 모드에서는 available에 맞춰 셀 자체를 줄여(정사각형) "간격이 벌어져 보이는" 현상을 완화한다.
            float size = m_request.targetSize.h;
            if (m_request.targetSize.w > 0.0f)
            {
                size = (std::min)(size, m_request.targetSize.w);
            }
            if (available.w > 0.0f)
            {
                size = (std::min)(size, available.w);
            }
            if (available.h > 0.0f)
            {
                size = (std::min)(size, available.h);
            }

            m_desired = { size, size };
            return m_desired;
        }

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
        // If we're already showing this source, don't restart transitions (prevents "flash" on repeated clicks).
        if (!filePath.empty() && filePath == m_filePath && m_loadedFilePath == m_filePath)
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

        // A) Fast reselect path: if SRV is cached, swap immediately (no disk/decode/upload).
        if (m_request.purpose == ImageCore::ImagePurpose::FullResolution && m_backplate)
        {
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cachedSrv;
            UINT cw = 0, ch = 0;
            if (TryGetSrvFromCache(filePath, cachedSrv, cw, ch))
            {
                // Setup cross-fade on GPU path
                if (m_gpuSrv)
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
                m_loadedFilePath = filePath;
                m_filePath = filePath;

                // Cancel CPU path bitmaps for main image
                m_bitmap.Reset();
                m_prevBitmap.Reset();

                m_loading = false;
                m_request.source = filePath;
                Invalidate();
                return S_OK;
            }
        }

        m_filePath = filePath;
        m_loading = false;

        // Request 업데이트
        m_request.source = filePath;
        
        return S_OK;
    }

    void Image::SetSelected(bool selected)
    {
        if (m_selected == selected)
        {
            return;
        }
        m_selected = selected;
        Invalidate();
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

    void Image::SetThumbnailSize(const Size& size)
    {
        ImageCore::Size targetSize { size.w, size.h };
        m_request.targetSize = targetSize;
        if (size.w > 0.0f || size.h > 0.0f)
        {
            m_request.purpose = ImageCore::ImagePurpose::Thumbnail;
        }
    }

    void Image::SetImagePurpose(ImageCore::ImagePurpose purpose)
    {
        m_request.purpose = purpose;
    }

    void Image::RequestImageLoad()
    {
        if (m_filePath.empty() || m_loading)
        {
            return;
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

        m_loading = true;
        m_request.source = m_filePath;
        // D2D-only renderer: force CPU-displayable DDS output (avoid UI-thread BCn decompress).
        if (m_backplate == nullptr || m_backplate->D3DDevice() == nullptr)
        {
            m_request.allowGpuCompressedDDS = false;
        }
        else
        {
            m_request.allowGpuCompressedDDS = true;
        }

        const std::wstring requestedPath = m_filePath;
        m_currentHandle = ImageCore::ImageLoader::Instance().Request(
            m_request,
            [this, requestedPath](HRESULT hr, Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap, std::unique_ptr<DirectX::ScratchImage> scratchImage)
            {
                // If the source changed since we requested, ignore the result (prevents stale swap + flicker).
                if (requestedPath != m_filePath)
                {
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
        m_loading = false;
        m_currentHandle = 0;

        // 변환은 OnRender에서 render target을 사용하여 수행
        // 여기서는 저장만 하고 Invalidate로 OnRender 호출 유도
        if (SUCCEEDED(hr) && (wicBitmap || scratchImage))
        {
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                m_pendingWicBitmap = wicBitmap;
                m_pendingScratchImage = std::move(scratchImage);
                m_pendingSourcePath = sourcePath;
            }
            
            // worker thread에서 UI thread로 명확히 redraw 요청
            if (m_backplate)
            {
                // Wake UI thread without PostMessage; the UI loop waits on this event.
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
            if (m_bitmap && m_loadedFilePath != sourcePath)
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
                // Match Backplate clear (DarkSlateGray-ish).
                (void)target->CreateSolidColorBrush(D2D1::ColorF(0.184f, 0.310f, 0.310f, 1.0f), &m_backdropBrush);
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
                if (pendingScratch && m_request.purpose == ImageCore::ImagePurpose::FullResolution && m_backplate)
                {
                    const DirectX::Image* img = pendingScratch->GetImage(0, 0, 0);
                    if (img && DirectX::IsCompressed(img->format))
                    {
                        ID3D11Device* dev = m_backplate->D3DDevice();
                        if (dev)
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
                                // Cross-fade for GPU path
                                if (m_gpuSrv)
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
                            }
                        }
                    }
                }

                if (!usedGpu)
                {
                    (void)ConvertToD2DBitmap(target, pendingSourcePath, pendingWic, std::move(pendingScratch));
                }
            }
        }

        // Request loading if we are not already loading the latest requested source.
        // Note: we keep drawing the previous bitmap while the next image loads to avoid "black flash".
        if (!m_loading && !m_filePath.empty())
        {
            const bool cpuLoaded = (m_bitmap && m_loadedFilePath == m_filePath);
            const bool gpuLoaded = (m_request.purpose == ImageCore::ImagePurpose::FullResolution &&
                m_gpuSrv && m_gpuWidth != 0 && m_gpuHeight != 0 && m_loadedFilePath == m_filePath);

            if (!cpuLoaded && !gpuLoaded)
            {
                RequestImageLoad();
            }
        }

        const auto computeAspectFitDestRect = [](const D2D1_RECT_F& layoutRect, const D2D1_SIZE_F& bitmapSize) -> D2D1_RECT_F
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

            target->DrawBitmap(
                bmp,
                destRect,
                opacity,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
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
                // Correct crossfade: draw prev opaque, then blend new over it.
                drawBitmapAspectFit(m_prevBitmap.Get(), 1.0f);
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

        // Loading spinner overlay (while a new image is being decoded).
        const bool shouldShowSpinner = m_loadingSpinnerEnabled && (!m_filePath.empty() && m_loadedFilePath != m_filePath);
        if (m_loadingSpinner)
        {
            m_loadingSpinner->SetActive(shouldShowSpinner);
        }

        if (m_selected)
        {
            if (!m_selectionBrush)
            {
                (void)target->CreateSolidColorBrush(
                    D2D1::ColorF(D2D1::ColorF::Orange, 1.0f),
                    &m_selectionBrush);
            }

            if (m_selectionBrush)
            {
                D2D1_RECT_F r = LayoutRect();
                // In thumbnail strips the control can be arranged taller than the bitmap draw-rect.
                // Draw the selection outline around the actual aspect-fit destination rectangle when possible.
                if (m_bitmap)
                {
                    r = computeAspectFitDestRect(r, m_bitmap->GetSize());
                }
                else if (m_prevBitmap)
                {
                    r = computeAspectFitDestRect(r, m_prevBitmap->GetSize());
                }

                // Slightly inflate so the stroke doesn't clip the image edges visually.
                r.left -= 1.0f;
                r.top -= 1.0f;
                r.right += 1.0f;
                r.bottom += 1.0f;
                target->DrawRectangle(r, m_selectionBrush.Get(), 2.0f);
            }
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

    bool Image::OnMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_LBUTTONDOWN:
        {
            if (!m_onClick)
            {
                break;
            }

            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const D2D1_RECT_F r = LayoutRect();
            if (static_cast<float>(x) >= r.left &&
                static_cast<float>(x) <= r.right &&
                static_cast<float>(y) >= r.top &&
                static_cast<float>(y) <= r.bottom)
            {
                m_onClick();
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

            const float l = toNdcX(rectPx.left);
            const float r = toNdcX(rectPx.right);
            const float t = toNdcY(rectPx.top);
            const float b = toNdcY(rectPx.bottom);

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
            // Correct crossfade: draw prev opaque, then blend new over it.
            drawSrvRect(m_prevGpuSrv.Get(), dest, 1.0f);
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

