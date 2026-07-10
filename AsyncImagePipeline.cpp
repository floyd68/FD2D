#include "AsyncImagePipeline.h"
#include "Backplate.h"
#include "../CommonUtil.h"
#include "../AppLog.h"

#include <chrono>

namespace FD2D
{
    AsyncImagePipeline::AsyncImagePipeline(Backplate* const* backplateSlot)
        : m_backplateSlot(backplateSlot)
    {
    }

    AsyncImagePipeline::~AsyncImagePipeline()
    {
        if (m_currentHandle != 0)
        {
            ImageCore::ImageLoader::Instance().Cancel(m_currentHandle);
        }
    }

    bool AsyncImagePipeline::IsFailedFor(const std::wstring& normalizedPath) const
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        return !m_failedFilePath.empty() && m_failedFilePath == normalizedPath;
    }

    bool AsyncImagePipeline::ClearFailureIfMatches(const std::wstring& normalizedPath, bool requireFailedHr)
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        const bool matches = !m_failedFilePath.empty() && m_failedFilePath == normalizedPath;
        if (!matches || (requireFailedHr && !FAILED(m_failedHr)))
        {
            return false;
        }

        m_failedFilePath.clear();
        m_failedHr = S_OK;
        return true;
    }

    void AsyncImagePipeline::ClearFailure()
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_failedFilePath.clear();
        m_failedHr = S_OK;
    }

    void AsyncImagePipeline::RecordFailure(const std::wstring& normalizedPath, HRESULT hr)
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_failedFilePath = normalizedPath;
        m_failedHr = hr;
    }

    void AsyncImagePipeline::CancelInflight()
    {
        if (m_currentHandle != 0)
        {
            ImageCore::ImageLoader::Instance().Cancel(m_currentHandle);
            m_currentHandle = 0;
        }
    }

    void AsyncImagePipeline::ResetInflightToken()
    {
        m_inflightToken.store(0);
    }

    void AsyncImagePipeline::ClearPending()
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pending = Payload {};
    }

    void AsyncImagePipeline::Dispatch(const ImageCore::ImageRequest& request, const std::wstring& requestedPath)
    {
        const unsigned long long token = m_requestToken.fetch_add(1ULL) + 1ULL;
        m_inflightToken.store(token);

        m_currentHandle = ImageCore::ImageLoader::Instance().RequestDecoded(
            request,
            [this, token, requestedPath](HRESULT hr, ImageCore::DecodedImage image)
            {
                // NOTE: This callback runs on a worker thread.
                // Do NOT read the owner's UI-thread state here; use token gating instead.
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

                OnDecodeComplete(requestedPath, hr, std::move(image));
            });
    }

    void AsyncImagePipeline::OnDecodeComplete(
        const std::wstring& sourcePath,
        HRESULT hr,
        ImageCore::DecodedImage image)
    {
        m_currentHandle = 0;

        const std::wstring normalizedSource = CommonUtil::NormalizePath(sourcePath);

        // Bitmap/SRV creation happens on the render thread; here we only stage
        // the payload and wake the UI thread.
        if (SUCCEEDED(hr) && image.blocks && !image.blocks->empty())
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pending.blocks = std::move(image.blocks);
            m_pending.width = image.width;
            m_pending.height = image.height;
            m_pending.rowPitch = image.rowPitchBytes;
            m_pending.format = image.dxgiFormat;
            m_pending.sourcePath = normalizedSource;
            m_failedFilePath.clear();
            m_failedHr = S_OK;
        }
        else
        {
            // Mark as failed so we don't spin forever.
            // The owner keeps displaying the previous image (if any).
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                m_failedFilePath = normalizedSource;
                m_failedHr = hr;
            }
            // Failure completes the in-flight request (stop spinner).
            m_loading.store(false);
            m_inflightToken.store(0);
        }

        Backplate* backplate = (m_backplateSlot != nullptr) ? *m_backplateSlot : nullptr;
        if (backplate != nullptr)
        {
            // Wake the UI thread without PostMessage; the UI loop waits on this event.
            backplate->RequestAsyncRedraw();
        }
    }

    AsyncImagePipeline::Payload AsyncImagePipeline::TakePending()
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        Payload out = std::move(m_pending);
        m_pending = Payload {};
        return out;
    }

    bool AsyncImagePipeline::HasPendingCpuBgra8For(const std::wstring& path) const
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        return m_pending.blocks
            && IsCpuBgra8Format(m_pending.format)
            && m_pending.sourcePath == path;
    }

    HRESULT AsyncImagePipeline::CreateD2DBitmap(
        ID2D1RenderTarget* target,
        const Payload& payload,
        D2D1_ALPHA_MODE alphaMode,
        Microsoft::WRL::ComPtr<ID2D1Bitmap>& outBitmap)
    {
        if (target == nullptr || !payload.blocks ||
            !IsCpuBgra8Format(payload.format) ||
            payload.width == 0 || payload.height == 0 || payload.rowPitch == 0)
        {
            return E_FAIL;
        }

        D2D1_BITMAP_PROPERTIES props {};
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = alphaMode;
        props.dpiX = 96.0f;
        props.dpiY = 96.0f;

        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        const D2D1_SIZE_U size = D2D1::SizeU(payload.width, payload.height);
        const auto t_bmp = std::chrono::steady_clock::now();
        const HRESULT hr = target->CreateBitmap(
            size,
            payload.blocks->data(),
            payload.rowPitch,
            &props,
            &bitmap);
        const auto bmpMs = FIC2_ELAPSED_MS(t_bmp);
        if (bmpMs > 30)
        {
            FIC2_LOG_INFO("[D2D] CreateBitmap {}x{} took {}ms", payload.width, payload.height, bmpMs);
        }

        if (FAILED(hr) || !bitmap)
        {
            return FAILED(hr) ? hr : E_FAIL;
        }

        outBitmap = bitmap;
        return S_OK;
    }
}
