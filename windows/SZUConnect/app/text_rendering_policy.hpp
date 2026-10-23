#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <cmath>

namespace szu::app {

struct SystemTextSmoothing {
    bool enabled = false;
    bool cleartype = false;
};

inline SystemTextSmoothing read_system_text_smoothing() {
    BOOL enabled = FALSE;
    UINT type = FE_FONTSMOOTHINGSTANDARD;
    const bool available = SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &enabled, 0) != FALSE;
    const bool typed = SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0, &type, 0) != FALSE;
    return {available && enabled != FALSE, typed && type == FE_FONTSMOOTHINGCLEARTYPE};
}

inline D2D1_TEXT_ANTIALIAS_MODE text_antialias_policy(SystemTextSmoothing smoothing, bool opaque,
                                                    DWRITE_PIXEL_GEOMETRY geometry, float cleartype_level) {
    const bool subpixels = geometry == DWRITE_PIXEL_GEOMETRY_RGB || geometry == DWRITE_PIXEL_GEOMETRY_BGR;
    return smoothing.enabled && smoothing.cleartype && opaque && subpixels &&
        std::isfinite(cleartype_level) && cleartype_level > 0.0f && cleartype_level <= 1.0f
        ? D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE : D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE;
}

inline DWORD native_edit_font_quality() {
    const auto smoothing = read_system_text_smoothing();
    return smoothing.enabled && smoothing.cleartype ? CLEARTYPE_QUALITY : ANTIALIASED_QUALITY;
}

}  // namespace szu::app
