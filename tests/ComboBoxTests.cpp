#include "TestHarness.h"
#include "BackplateTestAccess.h"
#include "WindowlessTestSupport.h"

#include "Backplate.h"
#include "ComboBox.h"
#include "Text.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// ComboBox popup item texts and selection layout. FICture2 does not use
// ComboBox, so these windowless tests are its only coverage.
//
// Resources: as in GraphicsGenerationTests, two WIC render targets from
// different D2D factories stand in for "before" and "after" a target
// recreation; a brush from the first fails EndDraw on the second.

namespace FD2D
{
    struct TextTestAccess
    {
        static ID2D1SolidColorBrush* Brush(const Text& text) { return text.m_brush.Get(); }
        static IDWriteTextLayout* Layout(const Text& text) { return text.m_textLayout.Get(); }
        static const std::wstring& String(const Text& text) { return text.m_text; }
    };

    struct ComboBoxTestAccess
    {
        static std::vector<const Text*> ItemTexts(const ComboBox& combo)
        {
            std::vector<const Text*> out;
            for (const auto& text : combo.m_itemTexts)
            {
                out.push_back(text.get());
            }
            return out;
        }

        static D2D1_RECT_F TextRect(const ComboBox& combo) { return combo.m_text.LayoutRect(); }
    };
}

using Microsoft::WRL::ComPtr;
using FD2D::BackplateTestAccess;
using FD2D::ComboBoxTestAccess;
using FD2D::TextTestAccess;

namespace
{
    struct Canvas
    {
        ComPtr<ID2D1Factory> d2d;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<ID2D1RenderTarget> target;

        bool Create(IWICImagingFactory* wic)
        {
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
                FAILED(wic->CreateBitmap(160, 120, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)))
            {
                return false;
            }
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            return SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target));
        }

        // The open item list (the popup overlay band).
        HRESULT DrawPopup(FD2D::ComboBox& combo)
        {
            target->BeginDraw();
            target->Clear(D2D1::ColorF(0, 0, 0, 1));
            combo.RenderOverlayTree(target.Get(), FD2D::OverlayLayer::Popup);
            return target->EndDraw();
        }
    };

    struct Fixture
    {
        FD2DWindowlessTest::ComScope com;
        FD2DWindowlessTest::CoreScope core; // DirectWrite factory
        ComPtr<IWICImagingFactory> wic;
        Canvas a;
        Canvas b;

        bool Create()
        {
            return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                a.Create(wic.Get()) && b.Create(wic.Get());
        }
    };

    void Open(FD2D::ComboBox& combo)
    {
        FD2D::InputEvent click {};
        click.type = FD2D::InputEventType::MouseDown;
        click.button = FD2D::MouseButton::Left;
        click.point = POINT { 10, 10 };
        click.hasPoint = true;
        (void)combo.OnInputEvent(click);
    }
}

FD2D_TEST(ComboBox_PopupItemTextsAreReusedAcrossRenders)
{
    Fixture fx;
    FD2D_REQUIRE(fx.Create());
    FD2D::Backplate backplate(L"combo-test");
    auto combo = std::make_shared<FD2D::ComboBox>(L"combo");
    combo->SetItems({ L"one", L"two", L"three" });
    FD2D_REQUIRE(backplate.AddWnd(combo));
    combo->SetLayoutRect(D2D1::RectF(0.0f, 0.0f, 160.0f, 22.0f));
    Open(*combo);
    FD2D_REQUIRE(combo->HasActiveOverlayInTree(FD2D::OverlayLayer::Popup));

    // Created by SetItems, one per item.
    const std::vector<const FD2D::Text*> items = ComboBoxTestAccess::ItemTexts(*combo);
    FD2D_REQUIRE(items.size() == 3);

    FD2D_CHECK(SUCCEEDED(fx.a.DrawPopup(*combo)));
    // Held here, so a rebuilt layout or brush cannot reuse the address.
    std::vector<ComPtr<IDWriteTextLayout>> layouts;
    std::vector<ComPtr<ID2D1SolidColorBrush>> brushes;
    for (const auto* item : items)
    {
        layouts.emplace_back(TextTestAccess::Layout(*item));
        brushes.emplace_back(TextTestAccess::Brush(*item));
        FD2D_CHECK(layouts.back() != nullptr);
        FD2D_CHECK(brushes.back() != nullptr);
    }

    // Later renders reuse the same Text objects with their layouts and brushes.
    for (int frame = 0; frame < 3; ++frame)
    {
        FD2D_CHECK(SUCCEEDED(fx.a.DrawPopup(*combo)));
        FD2D_CHECK(ComboBoxTestAccess::ItemTexts(*combo) == items);
        for (size_t i = 0; i < items.size(); ++i)
        {
            FD2D_CHECK(TextTestAccess::Layout(*items[i]) == layouts[i].Get());
            FD2D_CHECK(TextTestAccess::Brush(*items[i]) == brushes[i].Get());
        }
    }

    // A new item set gets its own Text objects.
    combo->SetItems({ L"four", L"five" });
    const std::vector<const FD2D::Text*> newItems = ComboBoxTestAccess::ItemTexts(*combo);
    FD2D_REQUIRE(newItems.size() == 2);
    FD2D_CHECK(TextTestAccess::String(*newItems[0]) == L"four");
    FD2D_CHECK(TextTestAccess::String(*newItems[1]) == L"five");
}

FD2D_TEST(ComboBox_TargetGenerationResetsPopupItemBrushesKeepsLayouts)
{
    Fixture fx;
    FD2D_REQUIRE(fx.Create());
    FD2D::Backplate backplate(L"combo-test");
    auto combo = std::make_shared<FD2D::ComboBox>(L"combo");
    combo->SetItems({ L"one", L"two", L"three" });
    FD2D_REQUIRE(backplate.AddWnd(combo));
    combo->SetLayoutRect(D2D1::RectF(0.0f, 0.0f, 160.0f, 22.0f));
    Open(*combo);

    FD2D_REQUIRE(SUCCEEDED(fx.a.DrawPopup(*combo)));
    const std::vector<const FD2D::Text*> items = ComboBoxTestAccess::ItemTexts(*combo);
    FD2D_REQUIRE(items.size() == 3);
    std::vector<ComPtr<IDWriteTextLayout>> layouts;
    std::vector<ComPtr<ID2D1SolidColorBrush>> brushes;
    for (const auto* item : items)
    {
        layouts.emplace_back(TextTestAccess::Layout(*item));
        brushes.emplace_back(TextTestAccess::Brush(*item));
    }

    // Same generation, other resource domain: the cached brushes fail.
    const HRESULT stale = fx.b.DrawPopup(*combo);

    // After a target recreation the popup renders on the new target: the
    // item brushes are new, the item layouts the same.
    BackplateTestAccess::TargetRecreated(backplate);
    const HRESULT afterBump = fx.b.DrawPopup(*combo);
    std::printf("  popup on other domain, same generation 0x%08X, after target bump 0x%08X\n",
        static_cast<unsigned>(stale), static_cast<unsigned>(afterBump));
    FD2D_CHECK(FAILED(stale));
    FD2D_CHECK(SUCCEEDED(afterBump));
    for (size_t i = 0; i < items.size(); ++i)
    {
        FD2D_CHECK(TextTestAccess::Brush(*items[i]) != nullptr);
        FD2D_CHECK(TextTestAccess::Brush(*items[i]) != brushes[i].Get());
        FD2D_CHECK(TextTestAccess::Layout(*items[i]) == layouts[i].Get());
    }

    // And after a device loss.
    BackplateTestAccess::DeviceLost(backplate);
    FD2D_CHECK(SUCCEEDED(fx.a.DrawPopup(*combo)));
    for (size_t i = 0; i < items.size(); ++i)
    {
        FD2D_CHECK(TextTestAccess::Layout(*items[i]) == layouts[i].Get());
    }
}

FD2D_TEST(ComboBox_SelectionChangingTextBoundsRequestsLayout)
{
    FD2DWindowlessTest::ComScope com;
    FD2DWindowlessTest::CoreScope core; // DirectWrite measures the text
    FD2D::Backplate backplate(L"combo-test");
    auto combo = std::make_shared<FD2D::ComboBox>(L"combo");
    combo->SetItems({ L"a", L"a much longer item", L"a much longer item" });
    FD2D_REQUIRE(backplate.AddWnd(combo));
    combo->SetSelectedIndex(0);
    combo->Measure({ 300.0f, 100.0f });
    combo->Arrange({ 0.0f, 0.0f, 300.0f, 22.0f });
    const D2D1_RECT_F shortRect = ComboBoxTestAccess::TextRect(*combo);

    // Longer text: a layout request, and the next layout pass widens the
    // text rect to the new text.
    BackplateTestAccess::ClearLayoutDirty(backplate);
    combo->SetSelectedIndex(1);
    FD2D_CHECK(combo->SelectedText() == L"a much longer item");
    FD2D_CHECK(BackplateTestAccess::LayoutDirty(backplate));
    combo->Measure({ 300.0f, 100.0f });
    combo->Arrange({ 0.0f, 0.0f, 300.0f, 22.0f });
    const D2D1_RECT_F longRect = ComboBoxTestAccess::TextRect(*combo);
    std::printf("  text rect width %.1f -> %.1f\n", shortRect.right - shortRect.left, longRect.right - longRect.left);
    FD2D_CHECK(longRect.right - longRect.left > shortRect.right - shortRect.left + 20.0f);

    // Same text (another item with equal text), or the same item: no request.
    BackplateTestAccess::ClearLayoutDirty(backplate);
    combo->SetSelectedIndex(2);
    FD2D_CHECK(!BackplateTestAccess::LayoutDirty(backplate));
    combo->SetSelectedIndex(2);
    FD2D_CHECK(!BackplateTestAccess::LayoutDirty(backplate));

    // A new item set that changes the selected text requests layout too.
    combo->SetItems({ L"x" });
    FD2D_CHECK(combo->SelectedText() == L"x");
    FD2D_CHECK(BackplateTestAccess::LayoutDirty(backplate));
}
