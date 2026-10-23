#include "canvas.hpp"
#include "text_rendering_policy.hpp"
#include "shared_tokens.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <wrl/client.h>

using namespace szu::app;
using Microsoft::WRL::ComPtr;
namespace {
int checks = 0, failures = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", message); }
}

void verify_policy() {
    using Mode = D2D1_TEXT_ANTIALIAS_MODE;
    constexpr auto grayscale = D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE;
    constexpr auto cleartype = D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE;
    for (const bool enabled : {false, true}) for (const bool selected : {false, true})
        for (const bool opaque : {false, true})
            for (const auto geometry : {DWRITE_PIXEL_GEOMETRY_FLAT, DWRITE_PIXEL_GEOMETRY_RGB, DWRITE_PIXEL_GEOMETRY_BGR})
                for (const float level : {0.0f, 0.5f, 1.0f}) {
                    const Mode expected = enabled && selected && opaque && geometry != DWRITE_PIXEL_GEOMETRY_FLAT && level > 0
                        ? cleartype : grayscale;
                    check(text_antialias_policy({enabled, selected}, opaque, geometry, level) == expected,
                        "ClearType needs user preference, opaque surface, RGB/BGR pixels and a positive display level");
                }
    for (const float level : {-0.1f, 1.1f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        check(text_antialias_policy({true, true}, true, DWRITE_PIXEL_GEOMETRY_RGB, level) == grayscale,
            "invalid display rendering data safely uses grayscale");
    const auto settings = read_system_text_smoothing();
    check(native_edit_font_quality() == (settings.enabled && settings.cleartype ? CLEARTYPE_QUALITY : ANTIALIASED_QUALITY),
        "native edit quality follows the same read-only Windows font preference");
}

void verify_glyphs(Canvas& canvas, IDWriteFactory* factory) {
    const auto info = canvas.text_rendering_info();
    ComPtr<IDWriteFontCollection> collection;
    check(SUCCEEDED(factory->GetSystemFontCollection(collection.GetAddressOf(), FALSE)) && collection,
        "read the actual installed UI font collection");
    UINT32 index = 0;
    BOOL exists = FALSE;
    ComPtr<IDWriteFontFamily> family;
    if (!collection || FAILED(collection->FindFamilyName(info.font_family.c_str(), &index, &exists)) || !exists ||
        FAILED(collection->GetFontFamily(index, family.GetAddressOf())) || !family) {
        check(false, "selected UI font is present in the native DirectWrite collection");
        return;
    }
    const std::wstring glyphs = info.primary_font_has_cjk ? L"十土士干千王三田目日口中深大随连校园网络，。0123456789Agjpqy" : L"0123456789Agjpqy";
    for (const auto weight : {DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_WEIGHT_SEMI_BOLD}) {
        ComPtr<IDWriteFont> font;
        check(SUCCEEDED(family->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            font.GetAddressOf())) && font, "match actual regular and emphasized font faces");
        if (!font) continue;
        for (const wchar_t glyph : glyphs) {
            BOOL covered = FALSE;
            check(SUCCEEDED(font->HasCharacter(glyph, &covered)) && covered,
                "selected regular and emphasized UI faces cover the readability specimen");
        }
    }
}

void verify_widest_text_cache(Canvas& canvas, IDWriteFactory* factory) {
    canvas.set_text_scale(1.0f);
    const auto expected = [&](TextStyle style, std::initializer_list<std::wstring> candidates) {
        std::wstring widest;
        float width = 0;
        for (const auto& candidate : candidates) {
            ComPtr<IDWriteTextLayout> layout;
            DWRITE_TEXT_METRICS metrics{};
            const bool measured = SUCCEEDED(factory->CreateTextLayout(candidate.data(), static_cast<UINT32>(candidate.size()),
                canvas.text_format_for_testing(style), 10000, 100000, layout.GetAddressOf())) && layout &&
                SUCCEEDED(layout->GetMetrics(&metrics));
            check(measured, "widest-label expectation comes from an independent native DirectWrite layout");
            if (measured && metrics.widthIncludingTrailingWhitespace > width) {
                width = metrics.widthIncludingTrailingWhitespace; widest = candidate;
            }
        }
        return widest;
    };
    const auto selected = [&] { return canvas.widest_text(TextStyle::caption,
        {L"短", L"iiiiiiii", L"WWWWWWWW", L"需要 HTTPS 网址"}); };
    const auto correct = expected(TextStyle::caption, {L"短", L"iiiiiiii", L"WWWWWWWW", L"需要 HTTPS 网址"});
    auto calculations = canvas.widest_text_calculations_for_testing();
    check(selected() == correct && canvas.widest_text_calculations_for_testing() == calculations + 1,
        "the size label uses actual glyph width rather than character count");
    calculations = canvas.widest_text_calculations_for_testing();
    check(selected() == correct && canvas.widest_text_calculations_for_testing() == calculations,
        "identical ordered candidates and style reuse the measured widest label");
    check(canvas.widest_text(TextStyle::caption, {L"需要 HTTPS 网址", L"WWWWWWWW", L"iiiiiiii", L"短"}) ==
        expected(TextStyle::caption, {L"需要 HTTPS 网址", L"WWWWWWWW", L"iiiiiiii", L"短"}) &&
        canvas.widest_text_calculations_for_testing() == ++calculations, "candidate order is part of the size-cache identity");
    check(canvas.widest_text(TextStyle::caption_strong, {L"短", L"iiiiiiii", L"WWWWWWWW", L"需要 HTTPS 网址"}) ==
        expected(TextStyle::caption_strong, {L"短", L"iiiiiiii", L"WWWWWWWW", L"需要 HTTPS 网址"}) &&
        canvas.widest_text_calculations_for_testing() == ++calculations, "different text styles cannot reuse another style's measurement");
    check(canvas.widest_text(TextStyle::caption, {L"短", L"iiiiiiii", L"WWWWWWWWWWWW", L"需要 HTTPS 网址"}) ==
        expected(TextStyle::caption, {L"短", L"iiiiiiii", L"WWWWWWWWWWWW", L"需要 HTTPS 网址"}) &&
        canvas.widest_text_calculations_for_testing() == ++calculations, "a changed candidate list receives its own actual measurement");
    const auto cached = canvas.widest_text_cache_size_for_testing();
    for (int index = 0; index < 160; ++index)
        canvas.measure(L"ordinary page layout " + std::to_wstring(index), TextStyle::body, 300);
    check(selected() == correct && canvas.widest_text_calculations_for_testing() == calculations &&
        canvas.widest_text_cache_size_for_testing() == cached,
        "flushing the ordinary 64-entry layout cache does not evict fixed label measurements");
    canvas.set_dpi(192); canvas.resize(1200, 900);
    check(selected() == correct && canvas.widest_text_calculations_for_testing() == calculations &&
        canvas.widest_text_cache_size_for_testing() == cached, "DPI and render-target resize retain valid DIP measurements");
    canvas.set_text_scale(1.5f);
    check(canvas.widest_text_cache_size_for_testing() == 0, "changing text size discards stale label measurements");
    const auto scaled = expected(TextStyle::caption, {L"短", L"iiiiiiii", L"WWWWWWWW", L"需要 HTTPS 网址"});
    check(selected() == scaled && canvas.widest_text_calculations_for_testing() == ++calculations,
        "a changed text scale remeasures using the new native font format");
    check(selected() == scaled && canvas.widest_text_calculations_for_testing() == calculations,
        "the newly measured text scale can be reused without further calculation");
    for (int index = 0; index < 18; ++index)
        canvas.widest_text(TextStyle::body, {L"bounded set " + std::to_wstring(index), std::wstring(index + 2, L'W')});
    calculations += 18;
    check(canvas.widest_text_cache_size_for_testing() == 16 && canvas.widest_text_calculations_for_testing() == calculations,
        "distinct label sets retain at most sixteen cache entries");
    check(selected() == scaled && canvas.widest_text_calculations_for_testing() == ++calculations &&
        canvas.widest_text_cache_size_for_testing() == 16, "an evicted label set is correctly remeasured while the cache remains bounded");
    std::wstring large(4097, L'\u200b'); large += L"WWWWWWWWWWWW";
    const auto large_expected = expected(TextStyle::caption, {L"short", large});
    const auto large_first = canvas.widest_text(TextStyle::caption, {L"short", large});
    const auto large_second = canvas.widest_text(TextStyle::caption, {L"short", large});
    check(large_first == large_expected && large_first == large && large_second == large &&
        canvas.widest_text_calculations_for_testing() == calculations + 2 && canvas.widest_text_cache_size_for_testing() == 16,
        "oversized text is measured through its final visible glyphs without truncating or caching the input");
    calculations += 2;
    const auto excessive = [&] { return canvas.widest_text(TextStyle::caption, {
        L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"",
        L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"", L"",
        L"last candidate WWWWWWWW"}); };
    const auto excessive_first = excessive(), excessive_second = excessive();
    check(excessive_first == L"last candidate WWWWWWWW" && excessive_second == excessive_first &&
        canvas.widest_text_calculations_for_testing() == calculations + 2 && canvas.widest_text_cache_size_for_testing() == 16,
        "more than thirty-two candidates are fully considered but never retained in the size cache");
}

void verify_canvas(HWND window, Canvas& canvas, IDWriteFactory* factory, float dpi, float scale) {
    canvas.set_dpi(dpi);
    canvas.set_text_scale(scale);
    const auto rendering = canvas.text_rendering_info();
    check(rendering.target_opaque && rendering.custom_parameters && rendering.follows_system_policy &&
        rendering.rendering_mode == "automatic" && (rendering.antialias_mode == "cleartype" || rendering.antialias_mode == "grayscale"),
        "native opaque canvas uses automatic rasterization and the actual display/user antialias policy");
    constexpr std::array styles{TextStyle::hero, TextStyle::title, TextStyle::headline, TextStyle::body, TextStyle::body_strong,
        TextStyle::callout, TextStyle::callout_strong, TextStyle::caption, TextStyle::caption_strong, TextStyle::mono};
    const float body_ratio = canvas.font_metrics(TextStyle::body).line_height / canvas.font_metrics(TextStyle::body).size;
    for (const auto style : styles) {
        const auto metrics = canvas.font_metrics(style);
        check(metrics.face_metrics && metrics.size > 0 && metrics.ascent > 0 && metrics.descent > 0 &&
            metrics.line_height + 0.01f >= metrics.ascent + metrics.descent &&
            metrics.baseline + 0.01f >= metrics.ascent && metrics.line_height - metrics.baseline + 0.01f >= metrics.descent,
            "line box contains the actual face ascent/descent with balanced leading");
        auto* format = canvas.text_format_for_testing(style);
        DWRITE_LINE_SPACING_METHOD method{};
        FLOAT spacing = 0, baseline = 0;
        check(format && SUCCEEDED(format->GetLineSpacing(&method, &spacing, &baseline)) &&
            method == DWRITE_LINE_SPACING_METHOD_UNIFORM && std::abs(spacing - metrics.line_height) < 0.01f &&
            std::abs(baseline - metrics.baseline) < 0.01f && std::abs(format->GetFontSize() - metrics.size) < 0.01f,
            "the measured and painted DirectWrite format uses the actual face baseline and unchanged requested size");
        if (!format) continue;
        const bool heading = style == TextStyle::hero || style == TextStyle::title || style == TextStyle::headline;
        if (heading) check(metrics.line_height / metrics.size <= body_ratio + 0.01f,
            "heading line boxes are compact while retaining full font extents");
        const std::wstring specimen = style == TextStyle::mono ? L"Agjpqy []{} 0123456789" : L"十土士干千王三田目日口中，。Agjpqy";
        ComPtr<IDWriteTextLayout> layout;
        check(SUCCEEDED(factory->CreateTextLayout(specimen.data(), static_cast<UINT32>(specimen.size()), format, 1000,
            metrics.line_height, layout.GetAddressOf())) && layout, "create native layout from the exact application text format");
        if (!layout) continue;
        UINT32 count = 0;
        DWRITE_LINE_METRICS line{};
        const auto status = layout->GetLineMetrics(&line, 1, &count);
        check(SUCCEEDED(status) && count == 1 && std::abs(line.baseline - metrics.baseline) < 0.01f &&
            std::abs(line.height - metrics.line_height) < 0.01f,
            "Chinese strokes, punctuation and Latin descenders share the real application line metrics");
        DWRITE_OVERHANG_METRICS ink{};
        check(SUCCEEDED(layout->GetOverhangMetrics(&ink)) && ink.top <= 0.15f && ink.bottom <= 0.15f,
            "readability specimen ink does not escape the vertical line box");
    }
    const auto width = static_cast<UINT>(std::ceil(1000 * dpi / 96));
    const auto height = static_cast<UINT>(std::ceil(800 * dpi / 96));
    SetWindowPos(window, nullptr, 0, 0, static_cast<int>(width), static_cast<int>(height), SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
    canvas.resize(width, height);
    canvas.reset_drawn_text_layouts_for_testing();
    check(canvas.begin(), "begin native glyph specimen paint");
    canvas.clear(D2D1::ColorF(D2D1::ColorF::White));
    float top = 12;
    for (const auto style : styles) {
        const auto line = canvas.line_height(style);
        canvas.text(L"深大随连 十土士干千王三田目日口中，。Agjpqy", style,
            D2D1::RectF(12, top, 988, top + line), D2D1::ColorF(D2D1::ColorF::Black));
        top += line + 8;
    }
    check(canvas.end() && canvas.drawn_text_layouts_for_testing() == styles.size(),
        "all heading/body/caption glyph specimens pass the native draw path");
}
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    verify_policy();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Isolated text rendering", WS_POPUP,
        0, 0, 1000, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "create isolated hidden native rendering window");
    ComPtr<IDWriteFactory> factory;
    check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(factory.GetAddressOf()))) && factory, "create native DirectWrite factory for independent metric checks");
    if (window && factory) {
        Canvas canvas;
        const bool ready = canvas.init() && canvas.attach(window, 96);
        check(ready, "initialize actual application text rendering surface");
        if (ready) {
            verify_glyphs(canvas, factory.Get());
            verify_widest_text_cache(canvas, factory.Get());
            for (const float dpi : {120.0f, 144.0f, 168.0f, 192.0f})
                for (const float scale : {1.0f, 1.5f}) verify_canvas(window, canvas, factory.Get(), dpi, scale);
        }
    }
    if (window) DestroyWindow(window);
    CoUninitialize();
    std::printf("native font rendering: %d checks, %d failures; read-only system settings and isolated hidden HWND\n", checks, failures);
    return failures ? 1 : 0;
}
