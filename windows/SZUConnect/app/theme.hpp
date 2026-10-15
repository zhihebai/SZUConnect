// Palette and typography resolved from shared/design-tokens.json for the current appearance.
#pragma once
#include <d2d1.h>

#include <string_view>

namespace szu::app {

enum class Appearance { system, light, dark };

struct Palette {
    bool dark = false;
    bool high_contrast = false;
    D2D1_COLOR_F background, sidebar, surface, surface_muted, surface_pressed, border, border_strong;
    D2D1_COLOR_F text_primary, text_secondary, text_tertiary;
    D2D1_COLOR_F accent, accent_hover, accent_pressed, on_accent, accent_surface, focus_ring;
    D2D1_COLOR_F selection_fill, on_selection;
    D2D1_COLOR_F success, success_surface, warning, warning_surface, danger, danger_surface;
    D2D1_COLOR_F info, info_surface, neutral, neutral_surface, busy, busy_surface;

    // Named role lookup ("textSecondary", "accent", tone names such as "warning" / "warningSurface").
    D2D1_COLOR_F role(std::string_view name) const;
    D2D1_COLOR_F tone(std::string_view tone) const;          // foreground for a tone
    D2D1_COLOR_F tone_surface(std::string_view tone) const;  // background for a tone
};

Palette make_palette(bool dark, bool high_contrast);
std::string_view tone_icon(std::string_view tone);
COLORREF to_colorref(const D2D1_COLOR_F& color);

}  // namespace szu::app
