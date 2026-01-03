#pragma once

#include "Wnd.h"
#include "Core.h"

namespace FD2D
{
    class Image : public Wnd
    {
    public:
        Image();
        explicit Image(const std::wstring& name);

        Size Measure(Size available) override;
        void SetRect(const D2D1_RECT_F& rect);
        HRESULT SetSourceFile(const std::wstring& filePath);

        void OnRender(ID2D1RenderTarget* target) override;

    private:
        HRESULT EnsureBitmap(ID2D1RenderTarget* target);
        void ReleaseBitmap();

        std::wstring m_filePath {};

        Microsoft::WRL::ComPtr<ID2D1Bitmap> m_bitmap {};
    };
}

