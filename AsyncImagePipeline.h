#pragma once

// AsyncImagePipeline: shared async image-load pipeline used by Image and ThumbImage.
//
// Owns the worker-shared state of an image control:
//   - loading flag (spinner / re-entrancy guard)
//   - request/inflight tokens (stale-completion gating)
//   - pending decoded payload staged by the worker thread
//   - failure bookkeeping (per-source-path, prevents retry loops)
//
// Threading model (unchanged from the original per-class implementations):
//   - Dispatch / Cancel / Clear / TakePending run on the UI/render thread.
//   - The decode completion runs on a worker thread: it stages the payload (or
//     records a failure) under the pending mutex and requests an async redraw.
//   - Stale completions are dropped via token comparison; the worker never
//     reads UI-thread strings like the owner's current file path.

#include "../ImageCore/ImageRequest.h"
#include "../ImageCore/ImageLoader.h"

#include <d2d1.h>
#include <wrl/client.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace FD2D
{
    class Backplate;

    class AsyncImagePipeline final
    {
    public:
        struct Payload
        {
            std::shared_ptr<std::vector<uint8_t>> blocks {};
            uint32_t width { 0 };
            uint32_t height { 0 };
            uint32_t rowPitch { 0 };
            DXGI_FORMAT format { DXGI_FORMAT_UNKNOWN };
            std::wstring sourcePath {};
        };

        // backplateSlot points at the owner's m_backplate pointer so that
        // completions observe the *current* attachment state (the owner can be
        // attached/detached between dispatch and completion).
        explicit AsyncImagePipeline(Backplate* const* backplateSlot);

        // Cancels any in-flight request handle.
        ~AsyncImagePipeline();

        AsyncImagePipeline(const AsyncImagePipeline&) = delete;
        AsyncImagePipeline& operator=(const AsyncImagePipeline&) = delete;

        // ---- loading flag ----
        bool IsLoading() const { return m_loading.load(); }
        void SetLoading(bool loading) { m_loading.store(loading); }

        // ---- failure bookkeeping (thread-safe) ----

        // True when the last attempt for exactly this path failed.
        bool IsFailedFor(const std::wstring& normalizedPath) const;

        // Clears the failure record when it matches the path (used for explicit
        // retry on re-selection). When requireFailedHr is true, only clears if
        // the recorded HRESULT is an actual failure code.
        // Returns true when a failure record was cleared.
        bool ClearFailureIfMatches(const std::wstring& normalizedPath, bool requireFailedHr = false);

        void ClearFailure();
        void RecordFailure(const std::wstring& normalizedPath, HRESULT hr);

        // ---- request lifecycle (UI thread) ----

        // Cancels the current loader handle, if any.
        void CancelInflight();

        // Clears the in-flight token so the next Dispatch starts fresh and any
        // still-running completion for the old request is dropped.
        void ResetInflightToken();

        // Drops the staged (not yet consumed) payload.
        void ClearPending();

        // Issues a decode request with token gating. On success the payload is
        // staged for the render thread; on failure the failure is recorded and
        // loading stops. Either way an async redraw is requested through the
        // owner's current backplate.
        // The caller must have set the loading flag beforehand (SetLoading(true)).
        void Dispatch(const ImageCore::ImageRequest& request, const std::wstring& requestedPath);

        // ---- render-thread consumption ----

        // Takes the staged payload (blocks is null when nothing is staged).
        Payload TakePending();

        // True when a CPU-displayable (BGRA8) payload for exactly `path` is
        // staged. Used by the D3D pass to avoid a 1-frame stale-SRV flash.
        bool HasPendingCpuBgra8For(const std::wstring& path) const;

        // Creates a D2D bitmap from a BGRA8 payload (96 DPI). Returns E_FAIL for
        // non-BGRA8 formats or invalid dimensions. Logs slow creations (>30ms).
        static HRESULT CreateD2DBitmap(
            ID2D1RenderTarget* target,
            const Payload& payload,
            D2D1_ALPHA_MODE alphaMode,
            Microsoft::WRL::ComPtr<ID2D1Bitmap>& outBitmap);

        // True for the CPU-displayable BGRA8 formats produced by the decoders.
        static bool IsCpuBgra8Format(DXGI_FORMAT fmt)
        {
            return fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        }

    private:
        // Worker thread: stage a successful decode or record a failure, then
        // request an async redraw so the render thread consumes the result.
        void OnDecodeComplete(const std::wstring& sourcePath, HRESULT hr, ImageCore::DecodedImage image);

        Backplate* const* m_backplateSlot { nullptr };

        ImageCore::ImageHandle m_currentHandle { 0 };
        std::atomic<bool> m_loading { false };
        std::atomic<unsigned long long> m_requestToken { 0 };
        std::atomic<unsigned long long> m_inflightToken { 0 };

        mutable std::mutex m_pendingMutex;
        Payload m_pending {};              // guarded by m_pendingMutex
        std::wstring m_failedFilePath {};  // guarded by m_pendingMutex
        HRESULT m_failedHr { S_OK };       // guarded by m_pendingMutex
    };
}
