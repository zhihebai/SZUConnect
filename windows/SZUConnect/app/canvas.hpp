// Direct2D / DirectWrite drawing surface in device-independent pixels (DIPs).
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <map>
#include <string>
#include <string_view>
#include "brand_icon.hpp"

namespace szu::app {

enum class TextStyle { hero, title, headline, body, body_strong, callout, callout_strong, caption, caption_strong, mono };

struct TextRenderingInfo {
    std::string antialias_mode = "unavailable";
    std::string rendering_mode = "unavailable";
    std::string pixel_geometry = "unavailable";
    std::wstring font_family;
    float dpi = 96.0f;
    float text_scale = 1.0f;
    float gamma = 0.0f;
    float enhanced_contrast = 0.0f;
    float cleartype_level = 0.0f;
    bool target_opaque = false;
    bool monitor_parameters = false;
    bool custom_parameters = false;
    bool primary_font_has_cjk = false;
};

class Canvas {
public:
    bool init();                        // device-independent factories
    bool attach(HWND hwnd, float dpi);  // (re)creates the HWND render target
    void resize(UINT width, UINT height);
    void set_dpi(float dpi);
    void set_text_scale(float scale);
    void refresh_text_rendering();       // refresh after display or system text settings change
    TextRenderingInfo text_rendering_info() const;
    bool begin();
    bool end();                         // false: device lost, target recreated on next begin

    void clear(const D2D1_COLOR_F& color);
    void fill_round(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color);
    void stroke_round(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color, float width);
    void fill_rect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color);
    void line(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& color, float width);
    void text(std::wstring_view text, TextStyle style, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color,
              bool center = false, bool underline = false);
    // A bounded single line: overflow gets an ellipsis, never another line.
    void text_single_line(std::wstring_view text, TextStyle style, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color);
    D2D1_SIZE_F measure(std::wstring_view text, TextStyle style, float max_width);
    void icon(std::string_view name, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float rotation_degrees = 0);
    void app_icon(const D2D1_RECT_F& rect);
    bool app_icon_painted() const { return app_icon_painted_; }
    void push_clip(const D2D1_RECT_F& rect);
    void pop_clip();
    void set_scroll(float dy);          // translation applied to subsequent drawing
    float dpi() const { return dpi_; }
    float line_height(TextStyle style) const;
#ifdef SZU_VIEW_TESTING
    std::size_t drawn_text_layouts_for_testing() const { return drawn_text_layouts_; }
    void reset_drawn_text_layouts_for_testing() { drawn_text_layouts_ = 0; }
#endif

private:
    IDWriteTextFormat* format(TextStyle style);
    ID2D1PathGeometry* geometry(std::string_view name);
    void reset_formats();

    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round_stroke_;
    Microsoft::WRL::ComPtr<IDWriteRenderingParams> rendering_params_;
    std::map<TextStyle, Microsoft::WRL::ComPtr<IDWriteTextFormat>> formats_;
    std::map<std::string, Microsoft::WRL::ComPtr<ID2D1PathGeometry>, std::less<>> icons_;
    std::wstring family_;
    HWND hwnd_ = nullptr;
    HMONITOR text_monitor_ = nullptr;
    bool monitor_parameters_ = false;
    bool custom_parameters_ = false;
    bool primary_font_has_cjk_ = false;
    bool app_icon_painted_ = false;
    BrandIcon brand_icon_;
    float dpi_ = 96.0f;
    float text_scale_ = 1.0f;
    float scroll_ = 0.0f;
#ifdef SZU_VIEW_TESTING
    std::size_t drawn_text_layouts_ = 0;
#endif
};

}  // namespace szu::app
