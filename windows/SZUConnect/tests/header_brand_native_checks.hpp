// Included after the native view fixture; uses its actual DirectWrite canvas and injected controller.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

void verify_header_brand(Canvas& canvas, ViewContext& original) {
    constexpr float height = 1200;
    const float rounded_caption_width = std::max(138.0f, 3 * std::ceil(46 * canvas.text_rendering_info().dpi / 96) /
        (canvas.text_rendering_info().dpi / 96));
    for (const float width : {480.0f, 500.0f, 560.0f, 640.0f, 960.0f})
        for (const float caption_width : {rounded_caption_width, 146.0f}) {
        UiState state = original.ui;
        state.overlays.clear(); state.tutorial_visible = false; state.page = Page::settings;
        state.caption_width = caption_width;
        Scene layout;
        ViewContext context{original.app, state, original.hwnd, [] {}, [](const std::string&) {}, [] {}, [](const std::string&) {}};
        build_scene(layout, canvas, context, width, height);
        const auto* brand = layout.find("nav.brand");
        const auto* first = layout.find("nav.connection");
        const auto* last = layout.find("nav.settings");
        const auto* help = layout.find("nav.help");
        const auto* minimize = layout.find("frame.minimize");
        const float caption_left = width - state.caption_width;
        const bool stacked = first && first->rect.top >= state.caption_height - 0.1f;
        bool no_mark = !layout.find("nav.brandIcon");
        for (const auto& widget : layout.widgets)
            if (widget.layer == 0 && !widget.scrolls && widget.kind == Kind::icon && widget.icon == "appBrand") no_mark = false;
        const auto brand_facts = brand ? canvas.inspect_text_layout_for_testing(brand->text, brand->style,
            brand->rect.right - brand->rect.left) : TextLayoutFacts{};
        check(brand && brand->text == T("app.name") && brand->style == TextStyle::headline && no_mark &&
            brand_facts.lines == 1 && (brand->rect.left == 12 || brand->rect.left == 8) && brand->rect.right <= caption_left - 24 &&
            brand->rect.top >= 0 && brand->rect.bottom <= state.caption_height &&
            (!first || stacked || brand->rect.right + (brand->rect.left == 12 ? 6 : 4) <= first->rect.left + 0.1f),
            "every compact, wide and enlarged-text toolbar keeps the complete four-character brand without a duplicate logo");
        bool labels_fit = true, targets_fit = true;
        for (const char* id : {"nav.connection", "nav.records", "nav.settings"}) {
            const auto* tab = layout.find(id);
            if (!tab) { labels_fit = false; targets_fit = false; continue; }
            const float padding = tab->compact ? 4 : tokens::kSizeDesktopNavigationHorizontalPadding;
            const float inner = tab->rect.right - tab->rect.left - padding * 2 -
                (tab->icon.empty() ? 0 : tokens::kSizeDesktopNavigationIconSize + tokens::kSizeDesktopNavigationIconGap);
            const auto normal = canvas.inspect_text_layout_for_testing(tab->text, TextStyle::body, inner);
            const auto selected = canvas.inspect_text_layout_for_testing(tab->text, TextStyle::body_strong, inner);
            labels_fit = labels_fit && normal.lines == 1 && selected.lines == 1 &&
                std::max(normal.height, selected.height) + 8 <= tab->rect.bottom - tab->rect.top + 0.1f;
            targets_fit = targets_fit && tab->rect.right - tab->rect.left >= tokens::kSizeMinTarget &&
                tab->rect.bottom - tab->rect.top >= tokens::kSizeMinTarget &&
                tab->rect.left >= 8 && tab->rect.right <= (help ? help->rect.left : width) &&
                (stacked || tab->rect.right <= caption_left - 24 + 0.1f);
        }
        check(first && last && help && minimize && labels_fit && targets_fit &&
            help->rect.right - help->rect.left == tokens::kSizeMinTarget &&
            help->rect.bottom - help->rect.top == tokens::kSizeMinTarget &&
            std::abs(help->rect.left - last->rect.right - 4) < 0.1f &&
            (stacked ? help->rect.right <= width - 12 : help->rect.right + 24 <= minimize->rect.left + 0.1f) &&
            std::abs((help->rect.top + help->rect.bottom - first->rect.top - first->rect.bottom) / 2) < 0.1f,
            "navigation and native caption targets keep aligned baselines and a clear 24-DIP drag area after help");
        if (!stacked && help && minimize) {
            bool drag_area_clear = true;
            const float drag_y = state.caption_height / 2;
            for (const float fraction : {0.1f, 0.5f, 0.9f}) {
                const float drag_x = help->rect.right + (minimize->rect.left - help->rect.right) * fraction;
                if (layout.hit(drag_x, drag_y)) drag_area_clear = false;
                for (const auto& widget : layout.widgets)
                    if (!widget.scrolls && widget.layer == 0 && widget.focusable &&
                        drag_x >= widget.rect.left && drag_x < widget.rect.right &&
                        drag_y >= widget.rect.top && drag_y < widget.rect.bottom) drag_area_clear = false;
            }
            check(drag_area_clear, "the reserved drag area contains no clickable or disabled focusable controls");
        }
        const auto* badge = layout.find("nav.readOnly");
        check(original.app.read_only() ? brand && badge && badge->rect.left == brand->rect.left &&
            badge->rect.top >= brand->rect.bottom + 3.9f && badge->rect.bottom <= state.caption_height : !badge,
            "read-only status stays below the brand without displacing navigation horizontally");
        if (width == 500) check(!stacked && state.toolbar_height == state.caption_height,
            "the unchanged 500-DIP default width retains a single toolbar row at normal and 150-percent text sizes");
        if (width == 480 && canvas.text_rendering_info().text_scale == 1.5f)
            check(!stacked && state.toolbar_height == state.caption_height && first &&
                (caption_width < 146 || first->compact),
                "480-DIP enlarged text keeps one header row with real 146-DIP DWM controls by reducing label insets, not type or hit targets");
        if (width == 500 && canvas.text_rendering_info().text_scale == 1.0f) {
            if (!first || first->icon.empty()) std::printf("  header mode=%d dpi=%.1f brand=%.2f caption=%.2f nav=%.2f..%.2f help=%.2f..%.2f\n",
                original.app.read_only(), canvas.dpi(), brand ? brand->rect.right - brand->rect.left : -1,
                state.caption_width, first ? first->rect.left : -1, last ? last->rect.right : -1,
                help ? help->rect.left : -1, help ? help->rect.right : -1);
            check(first && !first->icon.empty(), "the default 500-DIP toolbar preserves icons beside all three navigation labels");
        }
        if (width == 480 || width == 500) {
            const bool began = canvas.begin();
            if (began) paint(canvas, make_palette(false, false), layout, {}, width, height, 0);
            check(began && canvas.end(), "the fitted compact navigation paints using its exact measured label insets");
        }
    }
}
