// Draws the compiled application icon without duplicating its raster resource.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <wrl/client.h>

namespace szu::app {

class BrandIcon {
public:
    // Call on the COM-initialized UI thread, between BeginDraw and EndDraw.
    // A true result means the draw was queued; EndDraw reports device errors.
    bool draw(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float dpi);
    void reset();

private:
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap_;
    float dpi_ = 0.0f;
    int pixel_size_ = 0;
};

}  // namespace szu::app
