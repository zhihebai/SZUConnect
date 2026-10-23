#include "canvas.hpp"

#include <cstdlib>
#include <cwchar>
#include <algorithm>

#include "shared_icons.hpp"
#include "shared_tokens.hpp"
#include "szu/win/text.hpp"
#include "text_rendering_policy.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

namespace szu::app {
namespace {
struct FontSpec { float size; int weight; bool heading = false; bool mono = false; };
FontSpec font_spec(TextStyle style) {
    switch (style) {
        case TextStyle::hero: return {tokens::kFontHeroSize, tokens::kFontHeroWeight, true};
        case TextStyle::title: return {tokens::kFontTitleSize, tokens::kFontTitleWeight, true};
        case TextStyle::headline: return {tokens::kFontHeadlineSize, tokens::kFontHeadlineWeight, true};
        case TextStyle::body_strong: return {tokens::kFontBodySize, 600};
        case TextStyle::callout: return {tokens::kFontCalloutSize, tokens::kFontCalloutWeight};
        case TextStyle::callout_strong: return {tokens::kFontCalloutStrongSize, tokens::kFontCalloutStrongWeight};
        case TextStyle::caption: return {tokens::kFontCaptionSize, tokens::kFontCaptionWeight};
        case TextStyle::caption_strong: return {tokens::kFontCaptionSize, 600};
        case TextStyle::mono: return {tokens::kFontCalloutSize, 400, false, true};
        default: return {tokens::kFontBodySize, tokens::kFontBodyWeight};
    }
}
}  // namespace

bool Canvas::init() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                        D2D1_LINE_JOIN_ROUND),
                            nullptr, 0, round_stroke_.GetAddressOf());
    // Prefer an installed Chinese UI family rather than relying on the Latin
    // family's implicit CJK fallback. The fallback remains usable on minimal OS images.
    ComPtr<IDWriteFontCollection> fonts;
    dwrite_->GetSystemFontCollection(fonts.GetAddressOf(), FALSE);
    std::wstring first_available;
    for (const auto family : tokens::kFontFamilies) {
        const std::wstring name = win::widen(family);
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (fonts && SUCCEEDED(fonts->FindFamilyName(name.c_str(), &index, &exists)) && exists) {
            if (first_available.empty()) first_available = name;
            ComPtr<IDWriteFontFamily> font_family;
            ComPtr<IDWriteFont> font;
            BOOL deep = FALSE, cross = FALSE, field = FALSE;
            if (SUCCEEDED(fonts->GetFontFamily(index, font_family.GetAddressOf())) && font_family &&
                SUCCEEDED(font_family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, font.GetAddressOf())) && font &&
                SUCCEEDED(font->HasCharacter(L'深', &deep)) && deep &&
                SUCCEEDED(font->HasCharacter(L'十', &cross)) && cross &&
                SUCCEEDED(font->HasCharacter(L'田', &field)) && field) {
                family_ = name;
                primary_font_has_cjk_ = true;
                break;
            }
        }
    }
    if (family_.empty()) family_ = first_available.empty() ? L"Segoe UI" : first_available;
    font_metrics_.clear();
    for (const auto& name : {family_, std::wstring(L"Consolas")}) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        ComPtr<IDWriteFontFamily> font_family;
        if (!fonts || FAILED(fonts->FindFamilyName(name.c_str(), &index, &exists)) || !exists ||
            FAILED(fonts->GetFontFamily(index, font_family.GetAddressOf())) || !font_family) continue;
        for (const int weight : {400, 500, 600}) {
            ComPtr<IDWriteFont> font;
            if (FAILED(font_family->GetFirstMatchingFont(static_cast<DWRITE_FONT_WEIGHT>(weight), DWRITE_FONT_STRETCH_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, font.GetAddressOf())) || !font) continue;
            FontFaceMetrics face;
            font->GetMetrics(&face.metrics);
            face.weight = static_cast<int>(font->GetWeight());
            if (face.metrics.designUnitsPerEm && face.metrics.ascent && face.metrics.descent)
                font_metrics_[{name, weight}] = face;
        }
    }
    return true;
}

bool Canvas::attach(HWND hwnd, float dpi) {
    if (!d2d_ || !dwrite_) return false;
    hwnd_ = hwnd;
    dpi_ = dpi;
    target_.Reset();
    brush_.Reset();
    brand_icon_.reset();
    RECT rc{};
    GetClientRect(hwnd, &rc);
    // The window is fully repainted on an opaque surface.
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    const auto hwnd_props = D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(static_cast<UINT32>(rc.right - rc.left),
                                                                               static_cast<UINT32>(rc.bottom - rc.top)));
    if (FAILED(d2d_->CreateHwndRenderTarget(props, hwnd_props, target_.GetAddressOf()))) return false;
    refresh_text_rendering();
    if (FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), brush_.GetAddressOf()))) {
        target_.Reset(); brush_.Reset(); return false;
    }
    return true;
}

void Canvas::resize(UINT width, UINT height) {
    if (target_) target_->Resize(D2D1::SizeU(width, height));
}

void Canvas::set_dpi(float dpi) {
    dpi_ = dpi;
    if (target_) target_->SetDpi(dpi, dpi);
    refresh_text_rendering();
}

void Canvas::refresh_text_rendering() {
    if (!target_ || !dwrite_) return;
    text_monitor_ = hwnd_ ? MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST) : nullptr;
    rendering_params_.Reset();
    monitor_parameters_ = false;
    custom_parameters_ = false;
    ComPtr<IDWriteRenderingParams> defaults;
    if (text_monitor_ && SUCCEEDED(dwrite_->CreateMonitorRenderingParams(text_monitor_, defaults.GetAddressOf())) && defaults) {
        monitor_parameters_ = true;
    } else {
        defaults.Reset();
        dwrite_->CreateRenderingParams(defaults.GetAddressOf());
    }

    // Keep monitor tuning and automatic font-size rasterization. Subpixel text
    // is used only on an opaque RGB/BGR display when enabled in Windows.
    if (defaults) {
        custom_parameters_ = SUCCEEDED(dwrite_->CreateCustomRenderingParams(
            defaults->GetGamma(), defaults->GetEnhancedContrast(), defaults->GetClearTypeLevel(),
            defaults->GetPixelGeometry(), DWRITE_RENDERING_MODE_DEFAULT, rendering_params_.GetAddressOf())) &&
            rendering_params_;
        if (!custom_parameters_) {
            rendering_params_.Reset();
            // A null parameter set also restores DirectWrite's default selection.
        }
    }
    target_->SetTextRenderingParams(rendering_params_.Get());
    const auto format = target_->GetPixelFormat();
    const bool opaque = format.alphaMode == D2D1_ALPHA_MODE_IGNORE;
    const auto geometry = rendering_params_ ? rendering_params_->GetPixelGeometry() : DWRITE_PIXEL_GEOMETRY_FLAT;
    const float level = rendering_params_ ? rendering_params_->GetClearTypeLevel() : 0;
    target_->SetTextAntialiasMode(text_antialias_policy(read_system_text_smoothing(), opaque, geometry, level));
}

TextRenderingInfo Canvas::text_rendering_info() const {
    TextRenderingInfo info;
    info.font_family = family_;
    info.primary_font_has_cjk = primary_font_has_cjk_;
    info.dpi = dpi_;
    info.text_scale = text_scale_;
    if (!target_) return info;
    float dpi_y = 0.0f;
    target_->GetDpi(&info.dpi, &dpi_y);
    const auto format = target_->GetPixelFormat();
    info.target_opaque = format.format == DXGI_FORMAT_B8G8R8A8_UNORM && format.alphaMode == D2D1_ALPHA_MODE_IGNORE;
    const auto aa = target_->GetTextAntialiasMode();
    if (aa == D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE) info.antialias_mode = "cleartype";
    else if (aa == D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE) info.antialias_mode = "grayscale";
    else if (aa == D2D1_TEXT_ANTIALIAS_MODE_ALIASED) info.antialias_mode = "aliased";
    else info.antialias_mode = "default";
    ComPtr<IDWriteRenderingParams> actual;
    target_->GetTextRenderingParams(actual.GetAddressOf());
    info.follows_system_policy = aa == text_antialias_policy(read_system_text_smoothing(), info.target_opaque,
        actual ? actual->GetPixelGeometry() : DWRITE_PIXEL_GEOMETRY_FLAT, actual ? actual->GetClearTypeLevel() : 0);
    if (actual) {
        info.monitor_parameters = monitor_parameters_ && actual.Get() == rendering_params_.Get();
        info.custom_parameters = custom_parameters_ && actual.Get() == rendering_params_.Get();
        info.gamma = actual->GetGamma();
        info.enhanced_contrast = actual->GetEnhancedContrast();
        info.cleartype_level = actual->GetClearTypeLevel();
        switch (actual->GetRenderingMode()) {
            case DWRITE_RENDERING_MODE_DEFAULT: info.rendering_mode = "automatic"; break;
            case DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC: info.rendering_mode = "natural_symmetric"; break;
            case DWRITE_RENDERING_MODE_NATURAL: info.rendering_mode = "natural"; break;
            case DWRITE_RENDERING_MODE_ALIASED: info.rendering_mode = "aliased"; break;
            default: info.rendering_mode = "system"; break;
        }
        switch (actual->GetPixelGeometry()) {
            case DWRITE_PIXEL_GEOMETRY_RGB: info.pixel_geometry = "rgb"; break;
            case DWRITE_PIXEL_GEOMETRY_BGR: info.pixel_geometry = "bgr"; break;
            default: info.pixel_geometry = "flat"; break;
        }
    }
    return info;
}

void Canvas::set_text_scale(float scale) {
    if (scale != text_scale_) {
        text_scale_ = scale;
        reset_formats();
    }
}

void Canvas::reset_formats() { formats_.clear(); text_layout_cache_.clear(); widest_text_cache_.clear(); }

bool Canvas::begin() {
    if ((!target_ || !brush_) && (!hwnd_ || !attach(hwnd_, dpi_))) return false;
    // Same-DPI monitor moves do not necessarily produce WM_DPICHANGED.
    if (MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST) != text_monitor_) refresh_text_rendering();
    target_->BeginDraw();
    app_icon_painted_ = false;
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    scroll_ = 0;
    return true;
}

bool Canvas::end() {
    const HRESULT hr = target_->EndDraw();
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        target_.Reset();
        brush_.Reset();
        return false;
    }
    return SUCCEEDED(hr);
}

void Canvas::clear(const D2D1_COLOR_F& color) { target_->Clear(color); }

void Canvas::app_icon(const D2D1_RECT_F& rect) {
    if (target_ && brand_icon_.draw(target_.Get(), rect, dpi_)) app_icon_painted_ = true;
}

void Canvas::fill_round(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color) {
    brush_->SetColor(color);
    if (radius <= 0) target_->FillRectangle(rect, brush_.Get());
    else target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get());
}

void Canvas::stroke_round(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color, float width) {
    brush_->SetColor(color);
    const float inset = width / 2;
    const D2D1_RECT_F r = D2D1::RectF(rect.left + inset, rect.top + inset, rect.right - inset, rect.bottom - inset);
    if (radius <= 0) target_->DrawRectangle(r, brush_.Get(), width);
    else target_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get(), width);
}

void Canvas::fill_rect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color) {
    brush_->SetColor(color);
    target_->FillRectangle(rect, brush_.Get());
}

void Canvas::line(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& color, float width) {
    brush_->SetColor(color);
    target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush_.Get(), width, round_stroke_.Get());
}

float Canvas::line_height(TextStyle style) const {
    return font_metrics(style).line_height;
}

FontMetricsInfo Canvas::font_metrics(TextStyle style) const {
    const auto spec = font_spec(style);
    FontMetricsInfo info;
    info.size = spec.size * text_scale_;
    info.line_height = info.size * (spec.heading ? tokens::kFontHeadingLineHeight : tokens::kFontLineHeight);
    info.actual_weight = spec.weight;
    const auto face = font_metrics_.find({spec.mono ? std::wstring(L"Consolas") : family_, spec.weight});
    if (face != font_metrics_.end()) {
        const auto& metrics = face->second.metrics;
        const float scale = info.size / metrics.designUnitsPerEm;
        info.ascent = metrics.ascent * scale;
        info.descent = metrics.descent * scale;
        info.line_height = std::max(info.line_height, info.ascent + info.descent);
        info.baseline = info.ascent + (info.line_height - info.ascent - info.descent) / 2;
        info.actual_weight = face->second.weight;
        info.face_metrics = true;
    } else {
        info.baseline = info.line_height * 0.8f;
        info.ascent = info.baseline;
        info.descent = info.line_height - info.baseline;
    }
    return info;
}

IDWriteTextFormat* Canvas::format(TextStyle style) {
    auto found = formats_.find(style);
    if (found != formats_.end()) return found->second.Get();
    const auto spec = font_spec(style);
    const auto metrics = font_metrics(style);
    const wchar_t* family = spec.mono ? L"Consolas" : family_.c_str();
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite_->CreateTextFormat(family, nullptr, static_cast<DWRITE_FONT_WEIGHT>(spec.weight), DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, metrics.size, L"zh-CN", fmt.GetAddressOf())))
        return nullptr;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, metrics.line_height, metrics.baseline);
    formats_[style] = fmt;
    return fmt.Get();
}

D2D1_SIZE_F Canvas::measure(std::wstring_view text, TextStyle style, float max_width) {
    IDWriteTextFormat* fmt = format(style);
    if (!fmt || text.empty()) return D2D1::SizeF(0, text.empty() ? 0 : line_height(style));
    const auto result = text_layout_cache_.get(dwrite_.Get(), fmt, text, max_width);
    if (!result.layout)
        return D2D1::SizeF(max_width, line_height(style));
    return D2D1::SizeF(result.facts.width, result.facts.height);
}

std::wstring Canvas::widest_text(TextStyle style, std::initializer_list<std::wstring> candidates) {
    if (candidates.size() == 0) return {};
    for (const auto& entry : widest_text_cache_) {
        if (entry.style == style && entry.candidates.size() == candidates.size() &&
            std::equal(entry.candidates.begin(), entry.candidates.end(), candidates.begin())) return entry.widest;
    }
#ifdef SZU_VIEW_TESTING
    ++widest_text_calculations_;
#endif
    std::wstring widest;
    float widest_width = 0;
    std::size_t character_count = 0;
    for (const auto& candidate : candidates) {
        const float width = measure(candidate, style, 10000).width;
        if (width > widest_width) { widest_width = width; widest = candidate; }
        character_count += candidate.size();
    }
    // Fixed label sets get their own small cache instead of displacing page layouts.
    // Larger or dynamic input remains measurable without retaining unbounded text.
    if (candidates.size() <= 32 && character_count <= 4096) {
        if (widest_text_cache_.size() == 16) widest_text_cache_.erase(widest_text_cache_.begin());
        widest_text_cache_.push_back({style, {candidates.begin(), candidates.end()}, widest});
    }
    return widest;
}

#ifdef SZU_VIEW_TESTING
TextLayoutFacts Canvas::inspect_text_layout_for_testing(std::wstring_view text, TextStyle style, float max_width) {
    return text_layout_cache_.get(dwrite_.Get(), format(style), text, max_width).facts;
}
#endif

void Canvas::text(std::wstring_view text, TextStyle style, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, bool center,
                  bool underline) {
    IDWriteTextFormat* fmt = format(style);
    if (!fmt || text.empty()) return;
    const float width = rect.right - rect.left > 1 ? rect.right - rect.left : 1;
    const auto result = text_layout_cache_.get(dwrite_.Get(), fmt, text, width);
    const auto& layout = result.layout;
    if (!layout) return;
#ifdef SZU_VIEW_TESTING
    ++drawn_text_layouts_;
#endif
    layout->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetUnderline(underline, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(text.size())});
    brush_->SetColor(color);
    const float left = rect.left + (center ? std::max(0.0f, (width - result.facts.layout_width) / 2) : 0.0f);
    target_->DrawTextLayout(D2D1::Point2F(left, rect.top), layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Canvas::text_single_line(std::wstring_view text, TextStyle style, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color) {
    IDWriteTextFormat* fmt = format(style);
    const float width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (!fmt || text.empty() || width <= 0 || height <= 0) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt, width, height, layout.GetAddressOf())))
        return;
#ifdef SZU_VIEW_TESTING
    ++drawn_text_layouts_;
#endif
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(fmt, ellipsis.GetAddressOf()))) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trimming, ellipsis.Get());
    }
    brush_->SetColor(color);
    target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
    target_->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    target_->PopAxisAlignedClip();
}

ID2D1PathGeometry* Canvas::geometry(std::string_view name) {
    auto found = icons_.find(name);
    if (found != icons_.end()) return found->second.Get();
    const std::string_view data = icons::path(name);
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (data.empty() || FAILED(d2d_->CreatePathGeometry(geo.GetAddressOf())) || FAILED(geo->Open(sink.GetAddressOf()))) return nullptr;
    const std::string copy(data);
    const char* p = copy.c_str();
    bool open = false;
    auto number = [&p]() {
        while (*p == ' ' || *p == ',') ++p;
        char* end = nullptr;
        const float v = std::strtof(p, &end);
        p = end;
        return v;
    };
    while (*p) {
        while (*p == ' ') ++p;
        const char command = *p;
        if (!command) break;
        ++p;
        if (command == 'M') {
            if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
            const float x = number(), y = number();
            sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_HOLLOW);
            open = true;
        } else if (command == 'L') {
            const float x = number(), y = number();
            sink->AddLine(D2D1::Point2F(x, y));
        } else if (command == 'C') {
            const float x1 = number(), y1 = number(), x2 = number(), y2 = number(), x = number(), y = number();
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), D2D1::Point2F(x, y)));
        } else if (command == 'Z') {
            if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false;
        } else {
            break;  // generator guarantees M/L/C/Z only
        }
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    icons_.emplace(std::string(name), geo);
    return geo.Get();
}

void Canvas::icon(std::string_view name, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float rotation_degrees) {
    ID2D1PathGeometry* geo = geometry(name);
    if (!geo) return;
    const float size = rect.right - rect.left;
    const float scale = size / 24.0f;
    D2D1_MATRIX_3X2_F previous;
    target_->GetTransform(&previous);
    D2D1_MATRIX_3X2_F local = D2D1::Matrix3x2F::Scale(D2D1::SizeF(scale, scale)) * D2D1::Matrix3x2F::Translation(rect.left, rect.top);
    if (rotation_degrees != 0)
        local = local * D2D1::Matrix3x2F::Rotation(rotation_degrees, D2D1::Point2F(rect.left + size / 2, rect.top + size / 2));
    target_->SetTransform(local * previous);
    brush_->SetColor(color);
    target_->DrawGeometry(geo, brush_.Get(), 1.75f, round_stroke_.Get());
    target_->SetTransform(previous);
}

void Canvas::push_clip(const D2D1_RECT_F& rect) { target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE); }
void Canvas::pop_clip() { target_->PopAxisAlignedClip(); }

void Canvas::set_scroll(float dy) {
    scroll_ = dy;
    target_->SetTransform(D2D1::Matrix3x2F::Translation(0, -dy));
}

}  // namespace szu::app
