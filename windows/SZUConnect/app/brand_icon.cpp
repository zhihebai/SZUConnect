#include "brand_icon.hpp"

#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>

#include "../resources/resource.h"

namespace szu::app {

namespace {
struct OwnedIcon {
    HICON handle = nullptr;
    explicit OwnedIcon(HICON icon) : handle(icon) {}
    ~OwnedIcon() { if (handle) DestroyIcon(handle); }
    OwnedIcon(const OwnedIcon&) = delete;
    OwnedIcon& operator=(const OwnedIcon&) = delete;
};
}  // namespace

bool BrandIcon::draw(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float dpi) {
    if (!target || !std::isfinite(dpi) || dpi <= 0.0f ||
        !std::isfinite(rect.left) || !std::isfinite(rect.top) ||
        !std::isfinite(rect.right) || !std::isfinite(rect.bottom) ||
        rect.right <= rect.left || rect.bottom <= rect.top) return false;

    const double width = static_cast<double>(rect.right) - rect.left;
    const double height = static_cast<double>(rect.bottom) - rect.top;
    const int pixels = static_cast<int>(std::clamp(std::ceil(std::max(width, height) * dpi / 96.0), 1.0, 512.0));
    if (!bitmap_ || target_.Get() != target || dpi_ != dpi || pixel_size_ != pixels) {
        reset();
        // No LR_SHARED: this handle belongs to us and is released on every path.
        const OwnedIcon icon{static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP),
                                                         IMAGE_ICON, pixels, pixels, 0))};
        if (!icon.handle) return false;

        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.GetAddressOf())))) return false;
        Microsoft::WRL::ComPtr<IWICBitmap> source;
        if (FAILED(factory->CreateBitmapFromHICON(icon.handle, source.GetAddressOf()))) return false;
        Microsoft::WRL::ComPtr<IWICFormatConverter> converted;
        if (FAILED(factory->CreateFormatConverter(converted.GetAddressOf()))) return false;
        if (FAILED(converted->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                         nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;

        const auto properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
        if (FAILED(target->CreateBitmapFromWicBitmap(converted.Get(), &properties, bitmap_.GetAddressOf()))) return false;
        target_ = target;
        dpi_ = dpi;
        pixel_size_ = pixels;
    }
    target->DrawBitmap(bitmap_.Get(), &rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
    return true;
}

void BrandIcon::reset() {
    bitmap_.Reset();
    target_.Reset();
    dpi_ = 0.0f;
    pixel_size_ = 0;
}

}  // namespace szu::app
