#include "theme.hpp"

#include <windows.h>

#include <string>

#include "shared_tokens.hpp"

namespace szu::app {
namespace {

D2D1_COLOR_F from_rgba(std::uint32_t v) {
    return D2D1::ColorF(static_cast<float>((v >> 24) & 255) / 255.0f, static_cast<float>((v >> 16) & 255) / 255.0f,
                        static_cast<float>((v >> 8) & 255) / 255.0f, static_cast<float>(v & 255) / 255.0f);
}

D2D1_COLOR_F token(std::string_view name, bool dark) {
    for (const auto& c : tokens::kColors)
        if (c.name == name) return from_rgba(dark ? c.dark : c.light);
    return D2D1::ColorF(D2D1::ColorF::Magenta);  // visible gap, never silently wrong
}

D2D1_COLOR_F sys(int index) {
    const COLORREF c = GetSysColor(index);
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, 1.0f);
}

}  // namespace

Palette make_palette(bool dark, bool high_contrast) {
    Palette p;
    p.dark = dark;
    p.high_contrast = high_contrast;
    p.background = token("background", dark);
    p.sidebar = token("sidebar", dark);
    p.surface = token("surface", dark);
    p.surface_muted = token("surfaceMuted", dark);
    p.surface_pressed = token("surfacePressed", dark);
    p.border = token("border", dark);
    p.border_strong = token("borderStrong", dark);
    p.text_primary = token("textPrimary", dark);
    p.text_secondary = token("textSecondary", dark);
    p.text_tertiary = token("textTertiary", dark);
    p.accent = token("accent", dark);
    p.accent_hover = token("accentHover", dark);
    p.accent_pressed = token("accentPressed", dark);
    p.on_accent = token("onAccent", dark);
    p.accent_surface = token("accentSurface", dark);
    p.focus_ring = token("focusRing", dark);
    p.selection_fill = token("selectionFill", dark);
    p.on_selection = token("onSelection", dark);
    p.success = token("success", dark);
    p.success_surface = token("successSurface", dark);
    p.warning = token("warning", dark);
    p.warning_surface = token("warningSurface", dark);
    p.danger = token("danger", dark);
    p.danger_surface = token("dangerSurface", dark);
    p.info = token("info", dark);
    p.info_surface = token("infoSurface", dark);
    p.neutral = token("neutral", dark);
    p.neutral_surface = token("neutralSurface", dark);
    p.busy = token("busy", dark);
    p.busy_surface = token("busySurface", dark);
    if (high_contrast) {
        // High contrast: every role maps to a system colour so the user's theme wins.
        const D2D1_COLOR_F window = sys(COLOR_WINDOW), text = sys(COLOR_WINDOWTEXT), highlight = sys(COLOR_HIGHLIGHT),
                           highlight_text = sys(COLOR_HIGHLIGHTTEXT), gray = sys(COLOR_GRAYTEXT), link = sys(COLOR_HOTLIGHT);
        p.background = p.sidebar = p.surface = p.surface_muted = p.surface_pressed = window;
        p.border = p.border_strong = text;
        p.text_primary = p.text_secondary = text;
        p.text_tertiary = gray;
        p.accent = p.accent_hover = p.accent_pressed = highlight;
        p.on_accent = highlight_text;
        p.selection_fill = highlight; p.on_selection = highlight_text;
        p.accent_surface = window;
        p.focus_ring = link;
        p.success = p.warning = p.danger = p.info = p.neutral = p.busy = text;
        p.success_surface = p.warning_surface = p.danger_surface = p.info_surface = p.neutral_surface = p.busy_surface = window;
    }
    return p;
}

D2D1_COLOR_F Palette::role(std::string_view name) const {
    struct Entry { std::string_view name; const D2D1_COLOR_F Palette::*field; };
    static const Entry entries[] = {
        {"background", &Palette::background}, {"sidebar", &Palette::sidebar}, {"surface", &Palette::surface},
        {"surfaceMuted", &Palette::surface_muted}, {"surfacePressed", &Palette::surface_pressed}, {"border", &Palette::border},
        {"borderStrong", &Palette::border_strong}, {"textPrimary", &Palette::text_primary}, {"textSecondary", &Palette::text_secondary},
        {"textTertiary", &Palette::text_tertiary}, {"accent", &Palette::accent}, {"accentHover", &Palette::accent_hover},
        {"accentPressed", &Palette::accent_pressed}, {"onAccent", &Palette::on_accent}, {"accentSurface", &Palette::accent_surface},
        {"focusRing", &Palette::focus_ring}, {"selectionFill", &Palette::selection_fill}, {"onSelection", &Palette::on_selection}, {"success", &Palette::success}, {"successSurface", &Palette::success_surface},
        {"warning", &Palette::warning}, {"warningSurface", &Palette::warning_surface}, {"danger", &Palette::danger},
        {"dangerSurface", &Palette::danger_surface}, {"info", &Palette::info}, {"infoSurface", &Palette::info_surface},
        {"neutral", &Palette::neutral}, {"neutralSurface", &Palette::neutral_surface}, {"busy", &Palette::busy},
        {"busySurface", &Palette::busy_surface}};
    for (const auto& e : entries)
        if (e.name == name) return this->*(e.field);
    return text_primary;
}

D2D1_COLOR_F Palette::tone(std::string_view t) const {
    for (const auto& entry : tokens::kTones)
        if (entry.name == t) return role(entry.foreground);
    return neutral;
}

D2D1_COLOR_F Palette::tone_surface(std::string_view t) const {
    for (const auto& entry : tokens::kTones)
        if (entry.name == t) return role(entry.surface);
    return neutral_surface;
}

std::string_view tone_icon(std::string_view t) {
    for (const auto& entry : tokens::kTones)
        if (entry.name == t) return entry.icon;
    return "info";
}

COLORREF to_colorref(const D2D1_COLOR_F& c) {
    return RGB(static_cast<int>(c.r * 255.0f + 0.5f), static_cast<int>(c.g * 255.0f + 0.5f), static_cast<int>(c.b * 255.0f + 0.5f));
}

}  // namespace szu::app
