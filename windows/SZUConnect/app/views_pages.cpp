#include "views.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <shellapi.h>

#include "shared_copy.hpp"
#include "shared_tokens.hpp"

namespace szu::app {

const Link kOfficialLinks[11] = {
    {"nc", "https://www1.szu.edu.cn/nc/"},
    {"download", "https://www1.szu.edu.cn/nc/view.asp?id=64"},
    {"contact", "https://www1.szu.edu.cn/nc/view.asp?id=177"},
    {"identity", "https://www1.szu.edu.cn/v.asp?id=191"},
    {"activation", "https://authserver.szu.edu.cn/retrieve-password/accountActivation/index.html"},
    {"forgot", "https://authserver.szu.edu.cn/retrieve-password/retrievePassword/index.html"},
    {"self", "https://self.szu.edu.cn/self/nav_login"},
    {"teachingGuide", "https://www1.szu.edu.cn/nc/view.asp?id=641"},
    {"library", "https://www.lib.szu.edu.cn/space-and-facilities/wlan"},
    {"dormAccount", "https://www1.szu.edu.cn/nc/view.asp?id=249"},
    {"dormFAQ", "https://www1.szu.edu.cn/nc/view.asp?id=241"},
};

std::string U(std::string_view key) { return std::string(copy::text(key)); }
std::wstring T(std::string_view key) { return win::widen(copy::text(key)); }
std::wstring F(std::string_view key, const std::vector<std::pair<std::string_view, std::string>>& values) {
    return win::widen(copy::format(key, values));
}

std::string clock_text(double unix_seconds, bool with_date) {
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm local{};
    localtime_s(&local, &t);
    char buffer[32];
    std::strftime(buffer, sizeof buffer, with_date ? "%m-%d %H:%M" : "%H:%M", &local);
    return buffer;
}

std::string stamp_text(double unix_seconds) {
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm local{};
    localtime_s(&local, &t);
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%m-%d %H:%M:%S", &local);
    return buffer;
}

std::string clock_seconds(double unix_seconds) {
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm local{};
    localtime_s(&local, &t);
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%H:%M:%S", &local);
    return buffer;
}

std::wstring info_label(const std::string& topic) { return T("info.open") + L" " + T("info." + topic + ".title"); }

void perform_action(ViewContext& ctx, const std::string& action) {
    AppController& app = ctx.app;
    if (action == "check") app.check();
    else if (action == "login") app.connect();
    else if (action == "cancel") app.cancel();
    else if (action == "partialLogin") app.connect();  // fresh recovery candidate, never an implicit force
    else if (action == "forceLogin") app.force_login();
    else if (action == "resume") app.resume();
    else if (action == "remember" || action == "trust") ctx.ui.home_focus = "guard.memory";
    else if (action == "account" || action == "area") ctx.open("account");
    else if (action == "network") ctx.open("network");
    else if (action == "guard") ctx.open("guard");
    ctx.refresh();
}

namespace {

bool sample_website_issues(const records::Sample& sample) {
    return sample.site_total > 0 && sample.site_passed < sample.site_total;
}

bool sample_all_passed(const records::Sample& sample) {
    return sample.apple == "success" && sample.microsoft == "success" && !sample_website_issues(sample);
}

std::string stored_endpoint_text(const std::string& state) {
    // Old records may lack per-request timing; retain their original endpoint text.
    return U(state == "success" ? "endpoint.successTitle" : "endpoint." + state);
}

std::string endpoint_text(const EndpointResult& r) {
    if (r.succeeded()) {
        const auto elapsed = std::isfinite(r.elapsed) ? std::clamp(r.elapsed, 0.0, 120.0) : 0.0;
        return copy::format("endpoint.success", {{"ms", std::to_string(static_cast<long>(std::lround(elapsed * 1000)))}});
    }
    return U("endpoint." + std::string(to_string(r.state)));
}

// Keep the product name at every width; navigation icons yield before labels do.
float topbar(Scene& scene, Canvas& canvas, ViewContext& ctx, float width) {
    constexpr float help_w = 44, tab_gap = 2, help_gap = 4, drag_gap = tokens::kSizeWindowsCaptionDragWidth;
    float margin = 12, brand_gap = 6;
    struct Item { Page page; const char* key; const char* icon; };
    const Item items[] = {{Page::connection, "nav.connection", "signal"}, {Page::records, "nav.records", "records"},
                          {Page::settings, "nav.settings", "settings"}};
    const auto brand_text = T("app.name");
    const auto brand = canvas.measure(brand_text, TextStyle::headline, 10000);
    const auto mode_badge = canvas.measure(T("settings.readOnlyBadge"), TextStyle::caption, 10000);
    const float badge_width = std::ceil(mode_badge.width) + 2;
    const float brand_width = std::max(std::ceil(brand.width) + 2,
        ctx.app.read_only() ? badge_width : 0.0f);
    float text_widths[3]{}, compact_widths[3]{}, icon_widths[3]{}, fitted_icon_widths[3]{}, tab_widths[3]{};
    float text_span = tab_gap * 2, compact_span = tab_gap * 2, icon_span = tab_gap * 2, fitted_icon_span = tab_gap * 2;
    const float horizontal_padding = tokens::kSizeDesktopNavigationHorizontalPadding;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto normal = canvas.measure(T(items[i].key), TextStyle::body, 10000);
        const auto selected = canvas.measure(T(items[i].key), TextStyle::body_strong, 10000);
        const float text_width = std::ceil(std::max(normal.width, selected.width));
        text_widths[i] = std::max(tokens::kSizeWindowsNavigationTextItemWidth, text_width + horizontal_padding * 2);
        compact_widths[i] = std::max(tokens::kSizeMinTarget, text_width + 8);
        fitted_icon_widths[i] = std::max(tokens::kSizeMinTarget,
            text_width + horizontal_padding * 2 + tokens::kSizeDesktopNavigationIconSize + tokens::kSizeDesktopNavigationIconGap);
        icon_widths[i] = std::max(tokens::kSizeDesktopNavigationItemWidth, fitted_icon_widths[i]);
        text_span += text_widths[i]; compact_span += compact_widths[i]; icon_span += icon_widths[i];
        fitted_icon_span += fitted_icon_widths[i];
    }
    const float caption_left = width - ctx.ui.caption_width;
    const auto fits = [caption_left, help_w, help_gap, drag_gap](float left, float span) {
        return left + span + help_gap + help_w + drag_gap <= caption_left;
    };
    float brand_left = margin + brand_width + brand_gap;
    bool icons = fits(brand_left, icon_span);
    if (!icons) {
        margin = 8; brand_gap = 4;
        brand_left = margin + brand_width + brand_gap;
        icons = fits(brand_left, icon_span);
    }
    // Drop surplus tab width before dropping icons; the caption drag area stays usable.
    const bool fitted_icons = !icons && fits(brand_left, fitted_icon_span);
    icons = icons || fitted_icons;
    if (!icons && fits(12 + brand_width + 6, text_span)) {
        margin = 12; brand_gap = 6;
        brand_left = margin + brand_width + brand_gap;
    }
    const bool compact_nav = !icons && !fits(brand_left, text_span);
    const bool stacked = !icons && !fits(brand_left, compact_nav ? compact_span : text_span);
    const float nav_padding = compact_nav ? 4 : horizontal_padding;
    const float nav_left = stacked ? margin : brand_left;
    float tab_span = tab_gap * 2;
    // Long labels may wrap on the second row, but never expand beyond the client width.
    const float stacked_item_width = std::max(tokens::kSizeMinTarget,
        (width - margin * 2 - help_gap - help_w - tab_gap * 2) / 3);
    float label_height = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const float text_item_width = compact_nav ? compact_widths[i] : text_widths[i];
        tab_widths[i] = icons ? (fitted_icons ? fitted_icon_widths[i] : icon_widths[i]) :
            stacked ? std::min(text_item_width, stacked_item_width) : text_item_width;
        tab_span += tab_widths[i];
        const float label_width = std::max(1.0f, tab_widths[i] - nav_padding * 2 -
            (icons ? tokens::kSizeDesktopNavigationIconSize + tokens::kSizeDesktopNavigationIconGap : 0));
        label_height = std::max(label_height, std::max(canvas.measure(T(items[i].key), TextStyle::body, label_width).height,
            canvas.measure(T(items[i].key), TextStyle::body_strong, label_width).height));
    }
    const float nav_h = std::max(help_w, std::ceil(label_height) + 12);
    float row_h = tokens::kSizeTopBarHeight;
    const float brand_height = brand.height + (ctx.app.read_only() ? 4 + mode_badge.height : 0);
    row_h = std::max(row_h, brand_height + 12);
    if (!stacked) row_h = std::max(row_h, nav_h);
    const float nav_row_h = std::max(row_h, nav_h);
    const float bar_h = stacked ? row_h + nav_row_h : row_h;
    ctx.ui.caption_height = row_h;
    ctx.ui.toolbar_height = bar_h;
    const float nav_row_top = stacked ? row_h : 0;
    const float nav_top = nav_row_top + (nav_row_h - nav_h) / 2;
    const float help_top = nav_row_top + (nav_row_h - help_w) / 2;
    Builder b(scene, canvas, 0, 0, width, 0, false);
    Widget background;
    background.kind = Kind::panel;
    background.rect = D2D1::RectF(0, 0, width, bar_h);
    background.fill = "surface";
    b.add(background);
    const float brand_top = (row_h - brand_height) / 2;
    b.text_at(brand_text, TextStyle::headline, "textPrimary", margin, std::ceil(brand.width) + 2, brand_top);
    scene.widgets.back().id = "nav.brand";
    if (ctx.app.read_only()) {
        b.text_at(T("settings.readOnlyBadge"), TextStyle::caption, "accent", margin, badge_width,
            brand_top + brand.height + 4);
        scene.widgets.back().id = "nav.readOnly"; scene.widgets.back().name = T("settings.readOnlyMode");
    }
    const float help_left = nav_left + tab_span + help_gap;
    Widget help;
    help.kind = Kind::button;
    help.rect = D2D1::RectF(help_left, help_top, help_left + help_w, help_top + help_w);
    help.button = ButtonStyle::plain;
    help.icon = "help";
    help.radius = 10;
    help.id = "nav.help";
    help.name = T("action.help");
    help.role = Role::button;
    help.focusable = true;
    help.enabled = ctx.ui.overlays.empty();
    help.action = [&ctx] { ctx.open("help"); };
    float left = nav_left;
    const float nav_visual_h = std::min(nav_h, std::max(tokens::kSizeDesktopNavigationVisualHeight, label_height + 8));
    const float nav_inset = (nav_h - nav_visual_h) / 2;
    Widget group;
    group.kind = Kind::panel; group.fill = "surfaceMuted"; group.radius = tokens::kRadiusMedium;
    group.rect = D2D1::RectF(left - 2, nav_top + nav_inset - 2, left + tab_span + 2, nav_top + nav_h - nav_inset + 2);
    b.add(std::move(group));
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& item = items[i];
        Widget nav;
        nav.kind = Kind::nav;
        nav.rect = D2D1::RectF(left, nav_top, left + tab_widths[i], nav_top + nav_h);
        nav.text = T(item.key); nav.name = nav.text; nav.icon = icons ? item.icon : ""; nav.id = item.key;
        nav.compact = compact_nav;
        nav.role = Role::tab; nav.checked = ctx.ui.page == item.page; nav.focusable = true; nav.radius = 8;
        nav.enabled = ctx.ui.overlays.empty();
        nav.action = [&ctx, target = item.page] {
            if (target != Page::connection) { ctx.app.cancel_compat(); ctx.ui.proxy_rules_expanded = false; }
            ctx.ui.page = target; ctx.ui.show_password = false; ctx.ui.home_focus = "page.start"; ctx.refresh();
        };
        b.add(nav);
        left += tab_widths[i] + tab_gap;
    }
    b.add(std::move(help));
    return bar_h;
}

std::string short_url(std::string url) {
    for (const char* prefix : {"https://", "http://"}) {
        const std::size_t n = std::strlen(prefix);
        if (url.compare(0, n, prefix) == 0) { url.erase(0, n); break; }
    }
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

// The native EDIT keeps its accessible label without repeating a separate label
// above every compact search / add row. Input values remain owned by UiState.
void compact_field(Builder& b, std::string id, std::wstring label) {
    Widget field;
    field.kind = Kind::field;
    field.rect = D2D1::RectF(b.left(), b.y(), b.left() + b.width(), b.y() + tokens::kSizeMinTarget);
    field.radius = 8;
    field.id = std::move(id);
    field.role = Role::field;
    field.name = std::move(label);
    field.focusable = true;
    b.add(std::move(field));
    b.gap(tokens::kSizeMinTarget);
}

// Compact per-check chips: Apple, Microsoft and the selected sites. Skipped quick checks read "未等待", not failure.
std::vector<ChipSpec> check_chips(Builder& builder, ViewContext& ctx, const EngineSnapshot& snap, const std::optional<InternetCheckReport>& internet) {
    std::vector<ChipSpec> out;
    const bool checking = snap.busy && !snap.verifying_memory && !snap.verifying_campus;
    const auto activity = ctx.app.internet_activity();
    const auto add = [&out](const std::wstring& name, const std::wstring& text, const std::string& tone) {
        out.push_back({name + L" " + text, tone, std::string(tone_icon(tone)), name + L"：" + text});
        out.back().label = name;
    };
    const auto endpoint = [&](const wchar_t* name, const EndpointResult* result) {
        if (checking) { add(name, T("dashboard.checking"), "busy"); return; }
        if (!result) { add(name, T("compat.result.notRun"), "neutral"); return; }
        if (result->succeeded()) { add(name, win::widen(endpoint_text(*result)), "success"); return; }
        if (std::string(to_string(result->state)) == "notCompleted") {
            const bool quick = internet && internet->base.mode == ProbeMode::quick;
            add(name, quick ? T("dashboard.notWaited") : T("compat.result.notRun"), "neutral");
            return;
        }
        add(name, win::widen(endpoint_text(*result)), "warning");
    };
    const auto system = ctx.app.system_connectivity();
    const bool system_only = (snap.verifying_memory || snap.verifying_campus || status::system_path_can_lead(snap)) &&
        system && system->online() && (!internet || !internet->base.online());
    endpoint(L"Apple", system_only ? &system->apple : internet ? &internet->base.apple : nullptr);
    endpoint(L"Microsoft", system_only ? &system->microsoft : internet ? &internet->base.microsoft : nullptr);
    const auto endpoint_width = builder.widest_text(TextStyle::caption, {F("endpoint.success", {{"ms", "88888"}}), T("dashboard.checking"), T("dashboard.notWaited"),
        T("compat.result.notRun"), T("endpoint.timeout"), T("endpoint.dns"), T("endpoint.tls"), T("endpoint.redirect"),
        T("endpoint.unexpectedResponse"), T("endpoint.network"), T("endpoint.cancelled")});
    for (auto& chip : out) chip.sizing_detail = endpoint_width;
    std::optional<std::wstring> website_width;
    const auto website = [&](const WebsiteTarget& target, const WebsiteCheck* result, bool follow_system_proxy, bool running = false) {
        if (!website_width) website_width = builder.widest_text(TextStyle::caption, {T("dashboard.checking"), T("compat.result.notRun"),
            T("website.state.accessible.title"), T("website.state.redirected.title"), T("website.state.authenticationRequired.title"),
            T("website.state.forbidden.title"), T("website.state.httpStatus.title"), T("website.state.dns.title"), T("website.state.tls.title"),
            T("website.state.timeout.title"), T("website.state.cancelled.title"), T("website.state.securityPolicy.title"), T("website.state.network.title")});
        const std::wstring name = win::widen(short_url(target.url));
        add(name, running ? T("dashboard.checking") : result ? T("website.state." + result->result.state + ".title") :
            T("compat.result.notRun"), running ? "busy" : result ? (result->result.passed() ? "success" : "warning") : "neutral");
        auto& chip = out.back();
        chip.id = "website.result." + target.id;
        chip.sizing_detail = *website_width;
        chip.reserve_action = true;
        if (result && !running) {
            chip.name += L"，" + T("website.result.details");
            chip.action = [&ctx, captured = WebsiteDetails{*result, follow_system_proxy}] {
                ctx.ui.website_details = captured;
                ctx.open("websiteResult");
            };
        }
    };
    if (checking && activity) {
        for (const auto& target : activity->websites)
            website(target, nullptr, ctx.app.website_follow_system_proxy(), !activity->websites_deferred);
    } else if (checking || !internet || internet->websites_deferred) {
        const auto mode = !checking && internet ? internet->base.mode : snap.probe_mode;
        if (mode == ProbeMode::complete)
            for (const auto& target : ctx.app.website_targets()) if (target.enabled)
                website(target, nullptr, ctx.app.website_follow_system_proxy());
    } else {
        for (const auto& site : internet->websites) website(site.target, &site, internet->follow_system_proxy);
    }
    return out;
}

void wifi_selection_notice(Builder& b, ViewContext& ctx, bool in_settings) {
    if (!ctx.app.wifi_selection_required()) return;
    const auto candidates = ctx.app.wifi_candidates();
    const std::string id = in_settings ? "wifi.selection" : "home.wifiSelection";
    b.card_begin("warningSurface", "", tokens::kSpaceM, tokens::kRadiusMedium);
    b.icon_row("alert-triangle", "warning", T("wifiRecovery.selectionRequired.title"), TextStyle::callout_strong, "textPrimary", 18);
    b.gap(tokens::kSpaceXs);
    b.text(T(candidates.empty() ? "wifiRecovery.selectionRequired.noNetworks" : "wifiRecovery.selectionRequired.detail"),
        TextStyle::callout, "textPrimary", Role::status, id + ".detail");
    if (!in_settings) {
        b.gap(tokens::kSpaceS);
        b.button(id + ".open", T("wifiRecovery.selectionRequired.action"), ButtonStyle::secondary, [&ctx] {
            ctx.app.cancel_compat();
            ctx.ui.page = Page::settings;
            const auto targets = ctx.app.wifi_candidates();
            ctx.ui.home_focus = targets.empty() ? "wifi.targets" : "wifi.network." + targets.front().id;
            ctx.refresh();
        }, "signal");
    }
    b.card_end();
}

void setup_card(Builder& b, ViewContext& ctx, const EngineSnapshot& snap) {
    if (ctx.app.read_only()) return;
    // This checklist describes the current connection, not an unrelated saved network.
    const bool memory = snap.recovery_setup_has_network;
    const bool wired = snap.authentication_route && snap.authentication_route->descriptor.kind == "ethernet";
    const bool account = snap.has_saved && !snap.unsaved_changes;
    const bool automatic = snap.mode == MonitoringMode::recover;
    const bool ready = memory && account && automatic;
    const auto wifi_settings = [&ctx] {
        ctx.app.cancel_compat();
        ctx.ui.page = Page::settings;
        ctx.ui.home_focus = "wifi.autoJoin";
        ctx.refresh();
    };
    b.card_begin("surface", "border", 12, 14);
    if (ready) {
        const auto ready_text = T("setup.ready");
        const auto wifi_text = T(ctx.app.wifi_preferences().auto_join ? "setup.wifi.manage" : "setup.wifi.optional");
        const auto ready_size = b.measure_text(ready_text, TextStyle::caption, 10000);
        const float ready_width = std::ceil(ready_size.width) + 24;
        const float wifi_width = b.measure_button(wifi_text, "");
        if (wired) {
            b.icon_row("check", "success", ready_text, TextStyle::caption, "textSecondary", 14);
        } else if (ready_width + 8 + wifi_width <= b.width()) {
            const float wifi_height = std::max(44.0f, b.measure_text(wifi_text, TextStyle::body_strong, wifi_width - 32).height + 20);
            b.row({[ready_text, ready_size, wifi_height](Builder& row) {
                       row.gap(std::max(0.0f, (wifi_height - std::max(14.0f, ready_size.height)) / 2));
                       row.icon_row("check", "success", ready_text, TextStyle::caption, "textSecondary", 14);
                   }, [wifi_text, wifi_settings](Builder& row) {
                       row.button("setup.wifi", wifi_text, ButtonStyle::plain, wifi_settings);
                   }}, {ready_width, wifi_width}, 8);
        } else {
            b.icon_row("check", "success", ready_text, TextStyle::caption, "textSecondary", 14);
            b.gap(8);
            b.button("setup.wifi", wifi_text, ButtonStyle::plain, wifi_settings);
        }
    } else {
        const auto title = T("setup.title");
        const auto progress = F("setup.progress", {{"count", std::to_string(static_cast<int>(memory) + static_cast<int>(account) + static_cast<int>(automatic))}});
        const auto disclosure_label = T(ctx.ui.setup_expanded ? "disclosure.expanded" : "disclosure.collapsed");
        const float label_width = std::max(8.0f, b.width() - 22);
        const float progress_width = std::ceil(b.measure_text(progress, TextStyle::caption, 10000).width);
        const bool stacked = b.measure_text(title, TextStyle::callout_strong, 10000).width + 8 + progress_width > label_width;
        const float title_width = stacked ? label_width : std::max(8.0f, label_width - 8 - progress_width);
        const auto title_size = b.measure_text(title, TextStyle::callout_strong, title_width);
        const auto progress_size = b.measure_text(progress, TextStyle::caption, stacked ? label_width : progress_width);
        const float label_height = stacked ? title_size.height + 4 + progress_size.height : std::max(title_size.height, progress_size.height);
        const float header_height = std::max(tokens::kSizeWindowsSectionTitleVisualHeight, label_height);
        const float target_height = std::max(tokens::kSizeMinTarget, header_height);
        const float top = b.y(), label_top = top + (header_height - label_height) / 2;
        Widget disclosure; disclosure.kind = Kind::panel; disclosure.radius = 8;
        disclosure.rect = D2D1::RectF(b.left(), top + (header_height - target_height) / 2,
            b.left() + b.width(), top + (header_height + target_height) / 2);
        disclosure.id = "setup.expand"; disclosure.name = title + L"，" + progress + L"，" + disclosure_label;
        disclosure.role = Role::button; disclosure.focusable = true; disclosure.checked = ctx.ui.setup_expanded;
        disclosure.action = [&ctx] { ctx.ui.setup_expanded = !ctx.ui.setup_expanded; ctx.refresh(); };
        b.add(std::move(disclosure));
        Widget chevron; chevron.kind = Kind::icon; chevron.icon = ctx.ui.setup_expanded ? "chevron-down" : "chevron-right";
        chevron.color = "textSecondary";
        chevron.rect = D2D1::RectF(b.left(), top + header_height / 2 - 7, b.left() + 14, top + header_height / 2 + 7);
        b.add(std::move(chevron));
        Widget title_text; title_text.kind = Kind::text; title_text.text = title;
        title_text.style = TextStyle::callout_strong; title_text.color = "textPrimary";
        const float title_top = stacked ? label_top : top + (header_height - title_size.height) / 2;
        title_text.rect = D2D1::RectF(b.left() + 22, title_top, b.left() + 22 + title_width, title_top + title_size.height);
        b.add(std::move(title_text));
        Widget progress_text; progress_text.kind = Kind::text; progress_text.text = progress;
        progress_text.style = TextStyle::caption; progress_text.color = "textSecondary";
        const float progress_left = stacked ? b.left() + 22 : b.left() + b.width() - progress_width;
        const float progress_top = stacked ? label_top + title_size.height + 4 : top + (header_height - progress_size.height) / 2;
        progress_text.rect = D2D1::RectF(progress_left, progress_top, b.left() + b.width(), progress_top + progress_size.height);
        b.add(std::move(progress_text));
        b.gap(header_height);
        if (ctx.ui.setup_expanded) {
            const std::string step = !memory ? (snap.network_available ? "memory" : "network") : !account ? "account" : "recovery";
            b.gap(6);
            b.text(T("setup." + step + ".body"), TextStyle::caption, "textSecondary");
            b.gap(8);
            std::vector<ButtonSpec> actions;
            if (step == "network") actions.push_back({"setup.network", T("setup.network.action"), ButtonStyle::secondary, [&ctx] {
                if (ctx.app.offline_smoke()) return;
                const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(ctx.hwnd, L"open", L"ms-settings:network-wifi", nullptr, nullptr, SW_SHOWNORMAL));
                if (result <= 32) ctx.open("help");
            }, "signal"});
            else if (step == "memory") {
                actions.push_back({"setup.memory", T("setup.memory.action"), ButtonStyle::secondary,
                    [&ctx] { ctx.ui.home_focus = "guard.memory"; ctx.refresh(); }, "bookmark"});
            } else if (step == "account") actions.push_back({"setup.account", T("setup.account.action"), ButtonStyle::secondary,
                [&ctx] { ctx.open("account"); }, "user"});
            else actions.push_back({"setup.recovery", T("setup.recovery.action"), ButtonStyle::secondary,
                [&ctx] { ctx.app.set_mode(MonitoringMode::recover); ctx.refresh(); }, "shield-check", !snap.busy && ctx.app.settings_writable()});
            actions.push_back({"setup.help", T("setup.steps"), ButtonStyle::plain, [&ctx] { ctx.open("help"); }, ""});
            b.buttons(std::move(actions));
        }
    }
    b.card_end();
    b.gap(16);
}

void connection_page(Builder& b, ViewContext& ctx, const EngineSnapshot& snap, const status::Presentation& p) {
    AppController& app = ctx.app;
    const auto internet = app.internet_report();
    const auto& net = app.network();
    const auto system = app.system_connectivity();
    const bool system_only = status::system_path_can_lead(snap) && system && system->online() && (!internet || !internet->base.online());
    const bool pending_probe = !internet && snap.network_available && snap.status != EngineStatus::noNetwork &&
        status::system_path_can_lead(snap);
    const bool system_checking = snap.network_available && snap.status != EngineStatus::noNetwork && !system &&
        (!internet || !internet->base.online()) && app.system_connectivity_running() && status::system_path_can_lead(snap);

    // ---- 1. status hero: one visual focus, one primary action
    b.card_begin("surface", "border", tokens::kSpaceXl, tokens::kRadiusLarge);
    std::string headline = p.headline, detail = p.detail, tone = p.tone;
    if (!snap.busy && internet && snap.network_available && !snap.sleeping) {
        const auto verdict = internet->verdict();
        const std::string scope = internet->website_only_issues() ? "websiteIssues" : std::string(to_string(verdict));
        headline = "internet." + scope + ".title";
        detail = "internet." + scope + ".detail";
        tone = verdict == InternetVerdict::allPassed || verdict == InternetVerdict::basicPassed ? "success" : "warning";
    } else if (pending_probe) {
        headline = "internet.pending.title"; detail = "internet.pending.detail"; tone = "info";
    }
    if (system_only) { headline = "env.systemOnline.title"; detail = "env.systemOnline.detail"; tone = "success"; }
    if (system_checking) { headline = "env.systemChecking.title"; detail = "env.systemChecking.detail"; tone = "busy"; }
    b.status_header(snap.busy || system_checking ? "loader" : std::string(tone_icon(tone)), tone, T(headline), T(detail),
        info_label("status"), [&ctx] { ctx.open("info:status"); });
    if (!snap.busy && internet && snap.network_available && !snap.sleeping && internet->website_only_issues() &&
        (snap.tunnel || app.proxy_summary().state == "on")) {
        b.gap(8);
        b.banner("internet.routingWarning", "warning", T("internet.routingWarning.title"),
                 T("internet.routingWarning.body"), L"", {});
    }
    b.gap(12);
    // Bind the action to the rendered label. A path change after rendering must not
    // silently turn a failed "remember" admission into session-only authorization.
    const auto resolved_action = [&snap](const std::string& action) {
        return action == "trust" && snap.can_remember ? std::string("remember") : action;
    };
    const auto label = [](const std::string& action) {
        if (action == "area") return T("account.areaTitle");
        return T(action == "trust" ? std::string("setup.memory.action") : "action." + action);
    };
    std::vector<ButtonSpec> buttons;
    if (system_checking) {
        buttons.push_back({"hero.primary", T("action.cancel"), ButtonStyle::primary, [&ctx] { ctx.app.cancel(); ctx.refresh(); }, "x"});
        buttons.push_back({"hero.secondary", T("env.details"), ButtonStyle::plain, [&ctx] { ctx.open("network"); }, "info"});
    } else if (system_only || pending_probe) {
        buttons.push_back({"hero.primary", T("action.check"), ButtonStyle::primary, [&ctx] { ctx.app.check(); ctx.refresh(); }, "refresh"});
        buttons.push_back({"hero.secondary", T("env.details"), ButtonStyle::plain, [&ctx] { ctx.open("network"); }, "info"});
    } else if (p.primary) buttons.push_back({"hero.primary", label(*p.primary), ButtonStyle::primary,
        [&ctx, a = resolved_action(*p.primary)] { perform_action(ctx, a); }, *p.primary == "cancel" ? "x" : *p.primary == "check" ? "refresh" : "log-in"});
    if (p.secondary && !system_only && !pending_probe && !system_checking) buttons.push_back({"hero.secondary", label(*p.secondary), ButtonStyle::secondary,
        [&ctx, a = resolved_action(*p.secondary)] { perform_action(ctx, a); }, ""});
    b.buttons(buttons);
    b.gap(tokens::kSpaceM);
    Widget hero_rule; hero_rule.kind = Kind::divider;
    hero_rule.rect = D2D1::RectF(b.left(), b.y(), b.left() + b.width(), b.y()); b.add(std::move(hero_rule));
    b.gap(tokens::kSizeBorderWidth + tokens::kSpaceM);
    b.chips(check_chips(b, ctx, snap, internet));
    std::string meta = U(system_only ? "env.systemProbe" : "env.physicalProbe") + " · " + U("env.access." + net.access);
    if (app.proxy_summary().state == "on") meta += " · " + U("dashboard.proxy");
    if (net.tunnel_known && net.tunnel.tunnel) meta += " · " + U("dashboard.tunnel");
    const auto checked_at = system_only ? app.system_checked_at() : app.last_checked();
    if (checked_at) {
        const std::optional<ProbeMode> shown = system_only ? app.system_checked_mode() : internet ? std::optional<ProbeMode>(internet->base.mode) :
            snap.last_report ? std::optional<ProbeMode>(snap.last_report->mode) : std::nullopt;
        meta += " · " + (shown ? U(*shown == ProbeMode::quick ? "probe.quick.title" : "probe.complete.title") + " " : std::string()) +
            clock_seconds(*checked_at);
    } else meta += " · " + U("connection.neverChecked");
    if (const auto next = app.next_check(); next && !snap.busy) meta += " · " + copy::format("connection.nextCheck", {{"time", clock_text(*next)}});
    b.gap(12);
    b.selector_row("probe", win::widen(meta), {T("probe.quick.title"), T("probe.complete.title")},
        snap.probe_mode == ProbeMode::quick ? 0 : 1,
        [&ctx](int i) { ctx.app.set_probe(i == 0 ? ProbeMode::quick : ProbeMode::complete); ctx.refresh(); },
        info_label("probe"), [&ctx] { ctx.open("info:probe"); }, !snap.busy, 190);
    b.card_end();
    b.gap(16);

    build_egress_notice(b, ctx, "home.egress");
    if (app.should_suggest_proxy_assistant()) {
        b.card_begin("surface", "border", 16, 14);
        b.icon_row("info", "warning", T("proxyAssistant.offline.title"), TextStyle::callout_strong, "textPrimary", 18);
        b.gap(tokens::kSpaceS);
        b.text(T(app.proxy_assistant_suggestion().physical_unidentified ? "proxyAssistant.offline.unidentified" : "proxyAssistant.offline.detail"), TextStyle::callout, "textSecondary", Role::status, "proxy.suggestion.detail");
        b.gap(tokens::kSpaceS);
        b.button("proxy.suggestion.open", T("proxyAssistant.offline.action"), ButtonStyle::secondary, [&ctx] {
            ctx.app.open_proxy_assistant(); ctx.open("proxyAssistant");
        }, "settings");
        b.card_end();
        b.gap(16);
    }

    if (app.wifi_selection_required()) {
        wifi_selection_notice(b, ctx, false);
        b.gap(16);
    }
    setup_card(b, ctx, snap);

    // ---- 2. auto recovery, right below the status
    b.card_begin("surface", "border", 16, 14);
    build_inline_guard(b, ctx, snap);
    b.card_end();
    b.gap(16);

    // ---- 3. account
    b.card_begin("surface", "border", 16, 14);
    build_inline_account(b, ctx, snap);
    b.card_end();
    b.gap(16);

    // ---- 4. websites
    b.card_begin("surface", "border", 16, 14);
    const auto targets = app.website_targets();
    const auto enabled_targets = std::count_if(targets.begin(), targets.end(), [](const WebsiteTarget& target) { return target.enabled; });
    const auto websites_summary = targets.empty() ? T("internet.targets.empty") : F("internet.targets.summary",
        {{"total", std::to_string(targets.size())}, {"enabled", std::to_string(enabled_targets)}});
    build_home_section_header(b, ctx, "home.websites", "globe", T("internet.targets.title"), "websites", ctx.ui.websites_expanded, true,
        [&ctx] { ctx.ui.websites_expanded = !ctx.ui.websites_expanded; ctx.refresh(); },
        ctx.ui.websites_expanded ? std::wstring() : websites_summary);
    if (ctx.ui.websites_expanded) {
        const auto add_website = [&ctx](const std::string& input, bool enabled, bool clear_draft) {
            if (ctx.app.read_only()) return;
            if (ctx.app.snapshot().busy) { ctx.ui.internet_error = "internet.settingsBusy"; ctx.refresh(); return; }
            const auto normalized = website::normalize(input);
            if (!normalized.ok()) ctx.ui.internet_error = "website.error." + normalized.error;
            else {
                const auto canonical = validated_website_targets({{"new", normalized.url, enabled}});
                if (!canonical) { ctx.ui.internet_error = "website.error.invalid"; ctx.refresh(); return; }
                const std::string url = canonical->front().url;
                auto next = ctx.app.website_targets();
                if (next.size() >= WebsiteTarget::maximum_count || std::any_of(next.begin(), next.end(), [&](const auto& site) { return site.url == url; }))
                    ctx.ui.internet_error = "internet.duplicateOrLimit";
                else {
                    std::string id;
                    for (std::size_t n = 1; n <= WebsiteTarget::maximum_count; ++n) {
                        const std::string candidate = "website-" + std::to_string(n);
                        if (std::none_of(next.begin(), next.end(), [&](const auto& site) { return site.id == candidate; })) { id = candidate; break; }
                    }
                    if (id.empty()) { ctx.ui.internet_error = "internet.duplicateOrLimit"; ctx.refresh(); return; }
                    next.push_back({id, url, enabled});
                    if (ctx.app.replace_website_targets(next, enabled)) {
                        ctx.ui.internet_error.clear();
                        if (clear_draft) { ctx.ui.internet_url.clear(); ctx.ui.clear_internet_input = true; }
                    } else ctx.ui.internet_error = ctx.app.website_configuration_error().empty()
                        ? "internet.settingsWriteFailed" : ctx.app.website_configuration_error();
                }
            }
            ctx.refresh();
        };
        if (snap.probe_mode == ProbeMode::quick && !app.website_targets().empty() &&
            std::any_of(app.website_targets().begin(), app.website_targets().end(), [](const WebsiteTarget& site) { return site.enabled; })) {
            b.gap(tokens::kSpaceXs);
            b.text(T("internet.quickSkips"), TextStyle::caption, "textSecondary");
        }
        if (!app.website_configuration_error().empty() && app.website_configuration_error() != ctx.ui.internet_error) {
            b.gap(6); b.text(T(app.website_configuration_error()), TextStyle::caption, "warning", Role::status);
        }
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (i) b.gap(8);
            else b.gap(tokens::kSpaceS);
            const auto& target = targets[i];
            b.row({[&ctx, &snap, &target, i](Builder& row) {
                row.toggle("internet.enabled." + target.id, win::widen(target.url), L"", target.enabled, [&ctx, i] {
                    auto next = ctx.app.website_targets();
                    if (i >= next.size()) return;
                    next[i].enabled = !next[i].enabled;
                    ctx.app.replace_website_targets(next, next[i].enabled); ctx.refresh();
                }, !snap.busy && !ctx.app.read_only(), true);
            }, [&ctx, &snap, &target, i](Builder& row) {
                row.icon_button("internet.remove." + target.id, T("internet.remove") + L" " + win::widen(target.url), "x", [&ctx, i] {
                    auto next = ctx.app.website_targets();
                    if (i < next.size()) { next.erase(next.begin() + static_cast<std::ptrdiff_t>(i)); ctx.app.replace_website_targets(next, false); }
                    ctx.ui.internet_error.clear(); ctx.refresh();
                }, !snap.busy && !ctx.app.read_only());
            }}, {0, 44});
        }
        if (targets.size() < WebsiteTarget::maximum_count) {
            b.gap(tokens::kSpaceM);
            const float add_width = b.measure_button(T("internet.add"), "plus");
            b.row({[](Builder& row) { compact_field(row, "internet.url", T("internet.addLabel")); }, [&ctx, &snap, add_website](Builder& row) {
                row.button("internet.add", T("internet.add"), ButtonStyle::secondary, [&ctx, add_website] {
                    add_website(win::narrow(ctx.ui.internet_url), true, true);
                }, "plus", false, !snap.busy && !ctx.app.read_only());
            }}, {0, add_width}, tokens::kSpaceS, 128);
        }
        b.gap(tokens::kSpaceM);
        b.text(T("internet.presets.title"), TextStyle::callout_strong, "textPrimary", Role::heading, "internet.presets.title");
        b.gap(tokens::kSpaceS);
        std::vector<ButtonSpec> presets;
        for (const auto& preset : links::kWebsitePresets) {
            const bool present = std::any_of(targets.begin(), targets.end(), [&](const WebsiteTarget& target) { return target.url == preset.url; });
            const bool baidu = preset.key == "baidu";
            const auto preset_label = T(present ? (baidu ? "internet.presets.addedBaidu" : "internet.presets.addedTencent") :
                (baidu ? "internet.presets.addBaidu" : "internet.presets.addTencent"));
            presets.push_back({"internet.preset." + std::string(preset.key), preset_label, ButtonStyle::secondary,
                [add_website, url = std::string(preset.url)] { add_website(url, false, false); }, present ? "check" : "plus",
                !present && targets.size() < WebsiteTarget::maximum_count && !snap.busy && !ctx.app.read_only()});
        }
        b.buttons(std::move(presets));
        b.gap(tokens::kSpaceXs);
        b.text(T("internet.presets.detail"), TextStyle::caption, "textSecondary", Role::text, "internet.presets.detail");
        b.gap(tokens::kSpaceM);
        b.text(T("internet.targets.detail"), TextStyle::caption, "textSecondary");
        if (!ctx.ui.internet_error.empty()) { b.gap(6); b.text(T(ctx.ui.internet_error), TextStyle::caption, "danger", Role::status); }
        b.divider();
        build_website_strategy(b, ctx, snap);
    }
    b.card_end();
    b.gap(16);

    // ---- 5. Network details: connection values, comparison and campus settings.
    b.card_begin("surface", "border", 16, 14);
    std::wstring network_summary = T("env.access." + net.access);
    if (app.proxy_summary().state == "on") network_summary += L" · " + T("dashboard.proxy");
    if (net.tunnel_known && net.tunnel.tunnel) network_summary += L" · " + T("dashboard.tunnel");
    const float header_top = b.y(), main_width = b.width() - 44 - 8;
    const float title_x = b.left() + 50;
    const auto title_size = b.measure_text(T("dashboard.network"), TextStyle::headline, main_width - 50);
    const float summary_x = title_x + title_size.width + 8;
    const bool stacked = b.left() + main_width - summary_x < 80;
    const float summary_width = stacked ? main_width - 50 : b.left() + main_width - summary_x;
    const auto summary_size = b.measure_text(network_summary, TextStyle::caption, summary_width);
    const float head_h = std::max(tokens::kSizeWindowsSectionTitleVisualHeight,
        stacked ? title_size.height + summary_size.height + 8 : std::max(title_size.height, summary_size.height));
    const float target_h = std::max(tokens::kSizeMinTarget, head_h);
    Widget disclosure;
    disclosure.kind = Kind::panel;
    disclosure.rect = D2D1::RectF(b.left(), header_top + (head_h - target_h) / 2,
        b.left() + main_width, header_top + (head_h + target_h) / 2);
    disclosure.id = "network.disclosure";
    disclosure.name = T("dashboard.network") + L" · " + network_summary + L" · " + T(ctx.ui.advanced_network ? "disclosure.expanded" : "disclosure.collapsed");
    disclosure.role = Role::button;
    disclosure.focusable = true;
    disclosure.radius = 8;
    disclosure.checked = ctx.ui.advanced_network;
    disclosure.action = [&ctx] {
        ctx.ui.advanced_network = !ctx.ui.advanced_network;
        if (!ctx.ui.advanced_network) { ctx.ui.proxy_rules_expanded = false; ctx.app.cancel_compat(); }
        ctx.refresh();
    };
    b.add(std::move(disclosure));
    for (const auto& item : {std::pair<const char*, float>{ctx.ui.advanced_network ? "chevron-down" : "chevron-right", 0.0f}, {"globe", 24.0f}}) {
        Widget icon; icon.kind = Kind::icon; icon.icon = item.first; icon.color = item.second == 0 ? "textSecondary" : "accent";
        const float size = item.second == 0 ? 14.0f : 18.0f;
        icon.rect = D2D1::RectF(b.left() + item.second, header_top + (head_h - size) / 2, b.left() + item.second + size, header_top + (head_h + size) / 2);
        b.add(std::move(icon));
    }
    Widget title; title.kind = Kind::text; title.text = T("dashboard.network"); title.name = title.text; title.style = TextStyle::headline;
    title.id = "home.network"; title.role = Role::heading;
    const float title_top = stacked ? header_top + 4 : header_top + (head_h - title_size.height) / 2;
    title.rect = D2D1::RectF(title_x, title_top, title_x + title_size.width + 1, title_top + title_size.height); b.add(std::move(title));
    Widget summary; summary.kind = Kind::text; summary.style = TextStyle::caption; summary.text = network_summary; summary.name = network_summary;
    summary.id = "network.summary"; summary.role = Role::text; summary.color = "textSecondary";
    const float summary_top = stacked ? title_top + title_size.height + 4 : header_top + (head_h - summary_size.height) / 2;
    summary.rect = D2D1::RectF(stacked ? title_x : summary_x, summary_top, b.left() + main_width, summary_top + summary_size.height); b.add(std::move(summary));
    b.row({[](Builder& row) { row.gap(44); }, [&ctx, head_h](Builder& row) {
        row.gap((head_h - 44) / 2); row.icon_button("home.network.info", info_label("network"), "info", [&ctx] { ctx.open("info:network"); });
    }}, {0, 44}, 8, 1);
    b.set_y(header_top + head_h);
    if (ctx.ui.advanced_network) {
        b.divider();
        build_network_details(b, ctx, snap); b.gap(12);
        build_network_comparison(b, ctx, snap); b.gap(12);
        b.button("network.proxyAssistant", T("proxyAssistant.title"), ButtonStyle::secondary, [&ctx] {
            ctx.app.open_proxy_assistant(); ctx.open("proxyAssistant");
        }, "settings");
    }
    b.card_end();
    b.gap(16);
}

void records_page(Builder& b, ViewContext& ctx) {
    AppController& app = ctx.app;
    b.heading(T("records.title"));
    b.text(T("env.historyScope"), TextStyle::caption, "textSecondary");
    b.gap(tokens::kSpaceL);
    const double now = AppController::wall_now();
    const records::HealthSummary summary = app.health().summary(now);
    const records::HealthRange range = app.health().range(now);
    b.card_begin("surface", "border", 20, 14);
    b.title_row("records.health", "", T("records.health.title"), TextStyle::headline, "textPrimary", info_label("records"),
        [&ctx] { ctx.open("info:records"); });
    if (summary.samples == 0) {
        b.text(T("records.range.none"), TextStyle::body, "textSecondary");
    } else {
        std::wstring line = F("records.health.samples", {{"count", std::to_string(summary.samples)}}) + L" · " +
                            F("records.health.successRate", {{"percent", std::to_string(summary.success_percent())}});
        if (summary.median_latency_ms)
            line += L" · " + F("records.health.latency", {{"ms", std::to_string(static_cast<int>(*summary.median_latency_ms + 0.5))}});
        b.text(line, TextStyle::body_strong, "textPrimary");
    }
    b.gap(tokens::kSpaceM);
    // What the local history really covers: retention, the stored range, and the newest samples drawn below.
    b.text(F("records.range.retention", {{"max", std::to_string(records::HealthLog::kLimit)}}), TextStyle::caption, "textSecondary");
    if (range.first && range.last) {
        b.gap(tokens::kSpaceXxs);
        b.text(F("records.range.stored", {{"count", std::to_string(range.count)}, {"from", stamp_text(*range.first)}, {"to", stamp_text(*range.last)}}),
            TextStyle::caption, "textSecondary");
    }
    if (range.ribbon_first && range.ribbon_last) {
        b.gap(tokens::kSpaceXxs);
        b.text(F("records.range.ribbon", {{"count", std::to_string(range.ribbon_count)}, {"from", stamp_text(*range.ribbon_first)}, {"to", stamp_text(*range.ribbon_last)}}),
            TextStyle::caption, "textSecondary");
    }
    struct DisplaySample { const records::Sample* sample; std::string key; };
    std::vector<DisplaySample> recent;
    std::unordered_map<std::string, std::size_t> fingerprint_counts;
    fingerprint_counts.reserve(app.health().items().size());
    for (const auto& sample : app.health().items()) {
        // A UI-only identity: JSON preserves exact doubles (%.17g), and the ordinal
        // distinguishes even identical samples without changing the stored schema.
        auto fields = json::Value::array();
        fields.push(json::Value::number(sample.time));
        fields.push(json::Value::boolean(sample.online));
        fields.push(json::Value::string(sample.mode));
        fields.push(json::Value::string(sample.apple));
        fields.push(json::Value::string(sample.microsoft));
        fields.push(json::Value::number(sample.latency_ms));
        fields.push(json::Value::number(sample.site_total));
        fields.push(json::Value::number(sample.site_passed));
        const std::string fingerprint = json::serialize(fields);
        const auto ordinal = fingerprint_counts[fingerprint]++;
        if (now - sample.time <= records::HealthLog::kWindowSeconds)
            recent.push_back({&sample, "records.cell." + fingerprint + "." + std::to_string(ordinal)});
    }
    if (recent.size() > 24) recent.erase(recent.begin(), recent.end() - 24);
    if (!ctx.ui.selected_sample_key.empty() && std::none_of(recent.begin(), recent.end(), [&ctx](const DisplaySample& sample) {
        return sample.key == ctx.ui.selected_sample_key;
    })) ctx.ui.selected_sample_key.clear();
    const auto result_key = [](const records::Sample& sample) {
        const bool all = sample_all_passed(sample);
        return !sample.online ? std::string("failed") : sample.mode == "complete" ? (all ? "all" : "partial") : std::string("quick");
    };
    if (!recent.empty()) {
        b.gap(tokens::kSpaceM);
        std::vector<RibbonCell> cells;
        for (const auto& shown : recent) {
            const records::Sample* sample = shown.sample;
            const bool all = sample_all_passed(*sample);
            const bool partial = sample->mode == "complete" && sample->online && !all;
            const double time = sample->time;
            cells.push_back({shown.key, sample->online && !partial ? "success" : "warning",
                win::widen(stamp_text(time) + " · " + U("records.sampleResult." + result_key(*sample))), ctx.ui.selected_sample_key == shown.key,
                [&ctx, key = shown.key] {
                    ctx.ui.selected_sample_key = ctx.ui.selected_sample_key == key ? std::string{} : key;
                    ctx.refresh();
                }});
        }
        b.ribbon(std::move(cells));
        b.gap(tokens::kSpaceM);
        b.text(T("records.ribbon.hint"), TextStyle::caption, "textTertiary");
        for (const auto& shown : recent) {
            if (ctx.ui.selected_sample_key != shown.key) continue;
            const records::Sample* sample = shown.sample;
            b.gap(tokens::kSpaceM);
            b.card_begin("surfaceMuted", "", tokens::kSpaceM, 10);
            const auto close_title = T("records.detail.close");
            const float close_width = b.measure_button(close_title, "");
            const float close_height = std::max(tokens::kSizeMinTarget,
                b.measure_text(close_title, TextStyle::body_strong, close_width - 32).height + 20);
            const float title_height = b.measure_text(T("records.detail.title"), TextStyle::body_strong,
                std::max(1.0f, b.width() - close_width - tokens::kSpaceS)).height;
            const float title_inset = (std::max(close_height, title_height) - title_height) / 2;
            b.row({[title_inset](Builder& title) {
                    title.gap(title_inset);
                    title.text(T("records.detail.title"), TextStyle::body_strong, "textPrimary", Role::heading);
                }, [&ctx](Builder& close) {
                    close.button("records.detail.close", T("records.detail.close"), ButtonStyle::plain,
                        [&ctx] { ctx.ui.selected_sample_key.clear(); ctx.refresh(); });
                }}, {0, close_width}, tokens::kSpaceS, 72);
            const auto row = [&b](const char* key, const std::string& value) {
                b.gap(6);
                b.row({[key](Builder& label) { label.text(T(key), TextStyle::caption, "textSecondary"); },
                    [value](Builder& text) { text.text(win::widen(value), TextStyle::callout, "textPrimary"); }},
                    {72, 0}, tokens::kSpaceS, 72);
            };
            row("records.detail.time", stamp_text(sample->time));
            row("records.detail.mode", U(sample->mode == "complete" ? "probe.complete.title" : "probe.quick.title"));
            row("records.detail.result", U("records.sampleResult." + result_key(*sample)));
            const auto endpoint_detail = [](const std::string& state, const std::optional<double>& milliseconds) {
                std::string text = stored_endpoint_text(state);
                if (milliseconds) text += " · " + std::to_string(static_cast<long>(std::lround(*milliseconds))) + " ms";
                return text;
            };
            row("records.detail.apple", endpoint_detail(sample->apple, sample->apple_ms));
            row("records.detail.microsoft", endpoint_detail(sample->microsoft, sample->microsoft_ms));
            if (sample->site_total > 0) row("records.detail.sites", std::to_string(std::max(0, sample->site_passed)) + " / " + std::to_string(sample->site_total));
            if (sample_website_issues(*sample)) {
                b.gap(6);
                b.text(T("records.websiteWarning"), TextStyle::caption, "textSecondary");
            }
            if (sample->latency_ms >= 0) row("records.detail.latency", std::to_string(static_cast<int>(sample->latency_ms + 0.5)) + " ms");
            b.card_end();
            break; // The ribbon permits one selected sample and one details panel.
        }
    }
    std::size_t complete_total = 0, complete_passed = 0;
    for (const auto& sample : app.health().items()) {
        if (now - sample.time > records::HealthLog::kWindowSeconds || sample.mode != "complete") continue;
        ++complete_total;
        if (sample_all_passed(sample)) ++complete_passed;
    }
    if (complete_total) {
        b.gap(tokens::kSpaceM);
        b.text(F("dashboard.completeHistory", {{"passed", std::to_string(complete_passed)}, {"total", std::to_string(complete_total)}}),
            TextStyle::caption, "textSecondary");
    }
    b.card_end();
    b.gap(tokens::kSpaceL);
    b.segmented("records.kind", L"", {T("records.activities"), T("records.samples")},
        ctx.ui.showing_samples ? 1 : 0, [&ctx](int i) { ctx.ui.showing_samples = i == 1; ctx.refresh(); }, true, b.width());
    b.gap(tokens::kSpaceL);
    compact_field(b, "records.search", T("records.search"));
    b.gap(tokens::kSpaceL);
    b.toggle("records.failures", T("records.failuresOnly"), L"", ctx.ui.failures_only, [&ctx] {
        ctx.ui.failures_only = !ctx.ui.failures_only; ctx.refresh();
    }, true, true);
    b.gap(tokens::kSpaceL);
    b.title_row("records.diagnostics", "", T("records.diagnostics.title"), TextStyle::headline, "textPrimary", info_label("recordDiagnostics"),
        [&ctx] { ctx.open("info:recordDiagnostics"); });
    b.gap(tokens::kSpaceS);
    b.buttons({{"records.copy", T("action.copyDiagnostics"), ButtonStyle::secondary,
                [&ctx] {
                    ctx.ui.records_feedback = ctx.app.copy_diagnostics(ctx.hwnd) ? "records.copyDone" : "records.copyFailed";
                    ctx.refresh();
                },
                "copy"},
               {"records.export", T(ctx.app.diagnostics_export_running() ? "records.cancelExport" : "action.exportDiagnostics"), ButtonStyle::secondary,
                [&ctx] {
                    if (ctx.app.diagnostics_export_running()) ctx.app.cancel_diagnostics_export();
                    else if (ctx.app.export_diagnostics(ctx.hwnd)) ctx.ui.records_feedback.clear();
                    ctx.refresh();
                },
                ctx.app.diagnostics_export_running() ? "close" : "download"}});
    b.gap(tokens::kSpaceS);
    b.text(T("records.diagnostics.copyHint"), TextStyle::callout, "textSecondary", Role::text, "records.diagnostics.copyHint");
    b.gap(tokens::kSpaceXs);
    b.text(T("records.diagnostics.exportHint"), TextStyle::callout, "textSecondary", Role::text, "records.diagnostics.exportHint");
    if (!ctx.app.diagnostics_export_feedback().empty()) {
        b.gap(tokens::kSpaceS);
        b.text(T(ctx.app.diagnostics_export_feedback()), TextStyle::caption,
            ctx.app.diagnostics_export_feedback() == "records.exportFailed" ? "warning" :
                ctx.app.diagnostics_export_running() ? "textSecondary" : "success", Role::status, "records.exportFeedback");
    }
    if (!ctx.ui.records_feedback.empty()) {
        b.gap(tokens::kSpaceL);
        b.text(T(ctx.ui.records_feedback), TextStyle::caption,
            ctx.ui.records_feedback == "records.exportFailed" || ctx.ui.records_feedback == "records.copyFailed" ? "warning" : "success", Role::status, "records.feedback");
    }
    b.gap(tokens::kSpaceL);
    if (ctx.ui.showing_samples) {
        const auto& samples = app.health().items();
        const std::string query = win::narrow(ctx.ui.search);
        std::vector<const records::Sample*> shown;
        for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
            if (it->time < AppController::wall_now() - records::HealthLog::kWindowSeconds) continue;
            const bool all = sample_all_passed(*it);
            const bool partial = it->mode == "complete" && it->online && !all;
            const std::string result = !it->online ? "failed" : it->mode == "complete" ? (all ? "all" : "partial") : "quick";
            const std::string detail = "Apple: " + stored_endpoint_text(it->apple) + " · Microsoft: " + stored_endpoint_text(it->microsoft);
            const std::string title = U("records.sampleResult." + result);
            if (ctx.ui.failures_only && it->online && !partial) continue;
            if (!query.empty() && (title + " " + detail).find(query) == std::string::npos) continue;
            shown.push_back(&*it);
        }
        if (shown.empty()) {
            b.card_begin("surfaceMuted", "border", tokens::kSpaceXl, 14);
            b.text(T(samples.empty() ? "records.empty.title" : "records.noMatch.title"), TextStyle::headline, "textPrimary");
            b.gap(tokens::kSpaceM);
            b.text(T("records.noMatch.detail"), TextStyle::caption, "textSecondary");
            b.card_end();
            return;
        }
        b.card_begin("surface", "border", tokens::kSpaceS, 14);
        for (const records::Sample* sample : shown) {
            const bool all = sample_all_passed(*sample);
            const bool partial = sample->mode == "complete" && sample->online && !all;
            const std::string result = result_key(*sample);
            // Samples have an 8-DIP inset inside the card's 8-DIP padding.
            b.card_begin("", "", tokens::kSpaceS, 0);
            b.text(win::widen(stamp_text(sample->time)), TextStyle::caption, "textTertiary");
            b.gap(tokens::kSpaceXs);
            b.text(T("records.sampleResult." + result), TextStyle::body_strong, sample->online && !partial ? "success" : "warning");
            b.gap(tokens::kSpaceXs);
            b.text(win::widen("Apple：" + stored_endpoint_text(sample->apple) + " · Microsoft：" + stored_endpoint_text(sample->microsoft)),
                TextStyle::caption, "textPrimary");
            if (sample->site_total > 0) {
                b.gap(tokens::kSpaceXs);
                b.text(F("dashboard.siteHistory", {{"passed", std::to_string(std::max(0, sample->site_passed))}, {"total", std::to_string(sample->site_total)}}),
                    TextStyle::caption, "textSecondary");
            }
            if (sample_website_issues(*sample)) {
                b.gap(tokens::kSpaceXs);
                b.text(T("records.websiteWarning"), TextStyle::caption, "warning");
            }
            if (sample->latency_ms >= 0) {
                b.gap(tokens::kSpaceXs);
                b.text(win::widen(std::to_string(static_cast<int>(sample->latency_ms + 0.5)) + " ms"), TextStyle::caption, "textSecondary");
            }
            b.card_end();
            // Each sample divider has an 8-DIP gap on either side.
            b.divider();
        }
        b.gap(-tokens::kSpaceS); // No extra spacing after the final divider.
        b.card_end();
        return;
    }
    const auto& items = app.events().items();
    const std::string query = win::narrow(ctx.ui.search);
    std::vector<const records::Event*> shown;
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        if (ctx.ui.failures_only && !it->failure) continue;
        if (!query.empty() && it->message.find(query) == std::string::npos) continue;
        shown.push_back(&*it);
    }
    if (shown.empty()) {
        b.card_begin("surfaceMuted", "border", tokens::kSpaceXl, 14);
        const bool none = items.empty();
        b.icon_row(none ? "records" : "search", "textTertiary", T(none ? "records.empty.title" : "records.noMatch.title"), TextStyle::headline,
                   "textPrimary", 22);
        b.gap(tokens::kSpaceM);
        b.text(T(none ? "records.empty.detail" : "records.noMatch.detail"), TextStyle::body, "textSecondary");
        b.card_end();
        return;
    }
    b.card_begin("surface", "border", tokens::kSpaceS, 14);
    for (std::size_t i = 0; i < shown.size(); ++i) {
        const records::Event& e = *shown[i];
        // Activity rows add 12-DIP horizontal and 8-DIP vertical insets to the card padding.
        b.gap(tokens::kSpaceS);
        b.row({[](Builder&) {}, [&e](Builder& item) {
            item.text(win::widen(clock_text(e.time, true)), TextStyle::caption, e.failure ? "warning" : "textTertiary");
            item.gap(tokens::kSpaceXxs);
            item.text(win::widen(e.message), TextStyle::body, "textPrimary");
        }, [](Builder&) {}}, {tokens::kSpaceM, 0, tokens::kSpaceM}, 0, 1);
        b.gap(tokens::kSpaceS);
        // Activity rows have no inter-row spacing; retain only the divider itself.
        b.gap(-tokens::kSpaceS);
        b.divider();
        b.gap(-tokens::kSpaceS);
    }
    b.card_end();
}

void settings_page(Builder& b, ViewContext& ctx, const EngineSnapshot& snap, const status::Presentation&) {
    AppController& app = ctx.app;
    const bool editable = !app.read_only();
    const auto section = [&b](const std::string& key) {
        b.text(T(key), TextStyle::headline, "textPrimary", Role::heading);
    };
    const auto indented = [&b](const std::string& key, const std::string& color = "textSecondary") {
        // Windows switches are trailing controls; descriptions align with their labels.
        b.row({[](Builder&) {}, [key, color](Builder& text) {
            text.text(T(key), TextStyle::callout, color, Role::text, "settings.description." + key);
        }}, {tokens::kSpaceS, 0}, 0, 1);
    };

    // Settings section titles are separate from their controls.
    b.text(T("settings.title"), TextStyle::title, "textPrimary", Role::heading);
    b.gap(tokens::kSpaceL);
    if (!app.settings_writable()) {
        b.banner("settings.unreadable", "warning", T("error.settingsUnreadable"), L"", L"", {});
        b.gap(tokens::kSpaceL);
    }
    section("settings.appearance.title"); b.gap(tokens::kSpaceM);
    b.card_begin("surface", "border", tokens::kSpaceL, tokens::kRadiusLarge);
    const Appearance appearance = app.settings().appearance;
    b.segmented("appearance", L"", {T("settings.appearance.system"), T("settings.appearance.light"), T("settings.appearance.dark")},
        appearance == Appearance::system ? 0 : appearance == Appearance::light ? 1 : 2, [&ctx](int i) {
            ctx.app.set_appearance(i == 0 ? Appearance::system : i == 1 ? Appearance::light : Appearance::dark); ctx.refresh();
        }, true, b.width()); // Observe-only appearance stays an isolated, temporary preview.
    b.card_end();

    b.gap(tokens::kSpaceL);
    section("settings.backgroundTitle"); b.gap(tokens::kSpaceM);
    b.card_begin("surface", "border", tokens::kSpaceL, tokens::kRadiusLarge);
    b.toggle("settings.launch", T("settings.launch.title"), L"", app.launch_enabled(), [&ctx] {
        ctx.app.set_launch(!ctx.app.launch_enabled());
        ctx.refresh();
    }, editable);
    b.gap(tokens::kSpaceXs); indented("settings.launch.detail");
    if (app.notice() == "error.launchUpdate") {
        b.gap(tokens::kSpaceXs);
        b.text(T("error.launchUpdate"), TextStyle::caption, "warning", Role::status, "settings.launch.error");
    }
    if (app.launch_enabled() && !app.launch_approved()) {
        b.gap(tokens::kSpaceXs); b.text(T("error.launchApproval"), TextStyle::caption, "warning", Role::status);
    }
    b.card_end();

    b.gap(tokens::kSpaceL + tokens::kSpaceS);
    b.title_row("settings.memory", "", T("memory.title"), TextStyle::headline, "textPrimary", info_label("memory"),
        [&ctx] { ctx.open("info:memory"); }, {}, {}, {}, true);
    b.gap(tokens::kSpaceM);
    b.text(T("memory.networkScopeDetail"), TextStyle::callout, "textSecondary", Role::text, "settings.memory.networkScopeDetail");
    b.gap(tokens::kSpaceM);
    if (snap.memory_unreadable) {
        b.banner("settings.memoryUnreadable", "warning", T("memory.unreadable"), L"", L"", {});
        b.gap(tokens::kSpaceL);
    }
    const auto remembered = app.remembered_networks();
    const auto current_memory = app.current_network_record_id();
    std::map<std::string, std::size_t> memory_name_counts;
    for (const auto& record : remembered) ++memory_name_counts[app.network_name(record.id)];
    if (remembered.empty()) {
        b.text(T("memory.none"), TextStyle::callout, "textSecondary");
    } else {
        b.card_begin("surface", "border", tokens::kSpaceM, tokens::kRadiusLarge);
        b.text(F("memory.count", {{"count", std::to_string(remembered.size())}}), TextStyle::body_strong, "textPrimary");
        bool has_legacy_name = false;
        std::set<std::string> explained_names;
        for (const auto& record : remembered) {
            b.gap(tokens::kSpaceM);
            has_legacy_name = has_legacy_name || !app.network_name_known(record.id);
            const auto name = win::widen(app.network_display_name(record.id));
            if (memory_name_counts[app.network_name(record.id)] > 1 && explained_names.insert(app.network_name(record.id)).second) {
                b.text(T("memory.sameNameDetail"), TextStyle::caption, "textSecondary", Role::text, "memory.sameNameDetail." + record.id);
                b.gap(tokens::kSpaceS);
            }
            const bool current = current_memory && *current_memory == record.id;
            const auto context = (current ? T("memory.currentConnection") + L" · " : L"") +
                T("memory.kind." + app.network_kind(record.id)) +
                (record.area.empty() ? L"" : L" · " + T("memory.entry." + record.area));
            const auto verified = context + L"\n" + F("memory.verified", {{"date", stamp_text(record.verified)}});
            const float manage_width = std::ceil(b.measure_text(T("memory.manage"), TextStyle::callout, 10000).width) + 30;
            b.row({[name, verified, id = record.id](Builder& row) {
                const float text_width = std::max(8.0f, row.width() - 26);
                const float height = row.measure_text(name, TextStyle::callout_strong, text_width).height + tokens::kSpaceXxs +
                    row.measure_text(verified, TextStyle::caption, text_width).height;
                row.row({[height](Builder& icon) {
                    icon.gap(std::max(0.0f, (height - 18) / 2));
                    icon.icon_row("shield-check", "accent", L"", TextStyle::callout, "textPrimary", 18);
                }, [name, verified, id](Builder& text) {
                    text.text(name, TextStyle::callout_strong, "textPrimary", Role::text, "memory.name." + id); text.gap(tokens::kSpaceXxs);
                    text.text(verified, TextStyle::caption, "textSecondary", Role::text, "memory.detail." + id);
                }}, {18, 0}, tokens::kSpaceS, 1);
            }, [&ctx, id = record.id, name](Builder& menu) {
                menu.menu_button("memory.manage." + id, T("memory.manage"), T("memory.manage") + L"：" + name, [&ctx, id] {
                    HMENU choices = CreatePopupMenu(); if (!choices) return;
                    AppendMenuW(choices, MF_STRING, 1, T("memory.rename").c_str());
                    const bool wifi = ctx.app.can_edit_wifi_name(id);
                    if (wifi) AppendMenuW(choices, MF_STRING, 2, T("wifiRecovery.windows.manualSSID").c_str());
                    AppendMenuW(choices, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(choices, MF_STRING, 3, T("memory.forget").c_str());
                    POINT point{};
                    if (!GetCursorPos(&point)) { RECT frame{}; GetWindowRect(ctx.hwnd, &frame); point = {frame.left + 80, frame.top + 160}; }
                    SetForegroundWindow(ctx.hwnd);
                    const UINT choice = TrackPopupMenu(choices, TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, 0, ctx.hwnd, nullptr);
                    DestroyMenu(choices);
                    if (choice != 1 && !(choice == 2 && wifi) && choice != 3) return;
                    ctx.ui.network_name_id = id;
                    if (choice == 3) { ctx.open("memoryForget"); return; }
                    ctx.ui.network_name_draft = choice == 2 ? win::widen(ctx.app.wifi_name(id)) :
                        ctx.app.network_name_known(id) ? win::widen(ctx.app.network_name(id)) : L"";
                    ctx.ui.network_name_error.clear(); ctx.open(choice == 2 ? "wifiName" : "memoryRename");
                }, !ctx.app.read_only());
            }}, {0, manage_width}, tokens::kSpaceS, 120);
        }
        b.card_end(); b.gap(tokens::kSpaceL);
        if (has_legacy_name) {
            b.text(T("memory.legacyName"), TextStyle::caption, "textSecondary"); b.gap(tokens::kSpaceL);
        }
        b.gap(tokens::kSpaceS); b.text(T("memory.entry.explanation"), TextStyle::caption, "textSecondary");
        b.button("memory.forgetAll", T("memory.forgetAll"), ButtonStyle::plain,
            [&ctx] { ctx.open("memoryForgetAll"); }, "", false, editable);
    }
    b.gap(tokens::kSpaceL);
    b.text(T("memory.namesPrivacy"), TextStyle::caption, "textSecondary");

    b.gap(tokens::kSpaceL);
    section("wifiRecovery.title"); b.gap(tokens::kSpaceM);
    b.card_begin("surface", "border", tokens::kSpaceL, tokens::kRadiusLarge);
    b.text(T("setup.wifiOrder"), TextStyle::callout, "textSecondary");
    b.gap(tokens::kSpaceM);
    const auto wifi_preferences = app.wifi_preferences();
    const bool wifi_editable = editable && app.settings_writable() && !app.wifi_unreadable();
    b.toggle("wifi.autoJoin", T("wifiRecovery.autoJoin"), L"", wifi_preferences.auto_join && !app.wifi_unreadable(), [&ctx] {
        ctx.app.set_wifi_auto_join(!ctx.app.wifi_preferences().auto_join); ctx.refresh();
    }, wifi_editable, false, TextStyle::callout);
    b.gap(tokens::kSpaceXs); indented("wifiRecovery.autoJoinDetail");
    b.gap(tokens::kSpaceM);
    b.toggle("wifi.powerOn", T("wifiRecovery.powerOn"), L"", wifi_preferences.power_on && !app.wifi_unreadable(), [&ctx] {
        ctx.app.set_wifi_power_on(!ctx.app.wifi_preferences().power_on); ctx.refresh();
    }, wifi_editable, false, TextStyle::callout);
    b.gap(tokens::kSpaceXs); indented("wifiRecovery.powerDetail");
    if (wifi_preferences.power_on && !wifi_preferences.auto_join) {
        b.gap(tokens::kSpaceXs); indented("setup.powerNeedsJoin", "warning");
    }
    b.divider();
    b.text(T("wifiRecovery.networksTitle"), TextStyle::callout_strong, "textPrimary", Role::heading, "wifi.targets");
    const auto wifi_networks = app.wifi_candidates();
    if (app.wifi_selection_required()) {
        b.gap(tokens::kSpaceS);
        wifi_selection_notice(b, ctx, true);
    }
    if (wifi_networks.empty()) {
        b.gap(tokens::kSpaceS);
        b.text(T(!remembered.empty() && std::all_of(remembered.begin(), remembered.end(), [&app](const auto& record) { return app.network_kind(record.id) == "ethernet"; }) ? "wifiRecovery.noNetworksWired" : remembered.empty() ? "wifiRecovery.noRememberedNetworks" : "wifiRecovery.noNetworks"), TextStyle::callout, "textSecondary");
    } else {
        b.gap(tokens::kSpaceXs); b.text(T("wifiRecovery.targetsDetail"), TextStyle::callout, "textSecondary");
        b.gap(tokens::kSpaceXs);
        std::set<std::string> explained_wifi_names;
        for (const auto& network : wifi_networks) {
            if (memory_name_counts[app.network_name(network.id)] > 1 && explained_wifi_names.insert(app.network_name(network.id)).second) {
                b.gap(tokens::kSpaceS);
                b.text(T("memory.sameNameDetail"), TextStyle::caption, "textSecondary", Role::text, "wifi.sameNameDetail." + network.id);
            }
            const bool current = current_memory && *current_memory == network.id;
            const auto detail = (current ? T("memory.currentConnection") : L"") +
                (network.display_name == network.ssid ? L"" :
                    (current ? L" · " : L"") + win::widen(network.ssid));
            b.toggle("wifi.network." + network.id, win::widen(app.network_display_name(network.id)), detail, app.wifi_selected(network.id),
                [&ctx, id = network.id] { ctx.app.set_wifi_selected(id, !ctx.app.wifi_selected(id)); ctx.refresh(); }, wifi_editable, false, TextStyle::callout);
        }
    }
    b.gap(tokens::kSpaceS); b.text(T("wifiRecovery.windows.openOnly"), TextStyle::callout, "textSecondary");
    const auto wifi_status = app.wifi_status();
    b.gap(tokens::kSpaceM);
    b.icon_row("info", "textSecondary", T(wifi_status == "systemConnectionRequired" ? "wifiRecovery.windows.systemConnectionRequired" : "wifiRecovery.status." + wifi_status),
        TextStyle::callout, wifi_status == "failed" || wifi_status == "permissionRequired" ? "warning" : "textSecondary", 14);
    if (wifi_status == "permissionRequired") {
        b.gap(tokens::kSpaceXs); b.text(T("wifiRecovery.windows.permission"), TextStyle::callout, "warning");
    }
    if (app.wifi_unreadable()) {
        b.gap(tokens::kSpaceM); b.banner("settings.wifiUnreadable", "warning", T("wifiRecovery.unreadable"), L"", L"", {});
    }
    b.card_end();

    b.gap(tokens::kSpaceL);
    section("settings.helpTitle"); b.gap(tokens::kSpaceM);
    b.buttons({{"settings.tutorial", T("action.rewatchTutorial"), ButtonStyle::secondary, [&ctx] {
                    ctx.ui.tutorial_step = 1; ctx.open("tutorial");
                }, "sparkles"},
               {"settings.help", T("action.help"), ButtonStyle::secondary, [&ctx] { ctx.open("help"); }, "help"},
               {"settings.about", T("action.about"), ButtonStyle::secondary, [&ctx] { ctx.open("about"); }, "info"}});
    b.gap(tokens::kSpaceL); b.text(T("settings.privacy"), TextStyle::callout, "textSecondary");
    b.gap(tokens::kSpaceS); b.text(T("settings.closeNote"), TextStyle::callout, "textSecondary");
    if (app.read_only()) {
        b.gap(tokens::kSpaceL); b.text(T("settings.readOnlyLaunch"), TextStyle::caption, "warning");
    }
}

std::string tutorial_target(const UiState& ui, bool remembered) {
    const std::string step = kGuideSteps[static_cast<std::size_t>(std::clamp(ui.tutorial_step, 1, static_cast<int>(kGuideSteps.size())) - 1)];
    if (step == "welcome" || step == "finish") return {};
    if (step == "memory" && !remembered) return "guard.memory";
    return step == "account" ? "home.account" : step == "recovery" || step == "memory" ? "home.guard" :
        step == "websites" ? "home.websites" : step == "network" ? "home.network" :
        step == "startup" ? "settings.launch" : step == "wifi" ? "wifi.autoJoin" : "status.headline";
}

}  // namespace

void focus_tutorial(ViewContext& ctx) {
    const auto target = tutorial_target(ctx.ui, ctx.app.snapshot().recovery_setup_has_network);
    ctx.ui.page = target == "settings.launch" || target == "wifi.autoJoin" ? Page::settings : Page::connection;
    if (ctx.ui.page != Page::connection) {
        ctx.app.cancel_compat(); ctx.ui.proxy_rules_expanded = false;
    }
    ctx.ui.home_focus = target.empty() ? "page.start" : target;
    if (target == "home.account") { ctx.ui.account_expanded = true; ctx.ui.show_password = false; }
    if (target == "home.websites") ctx.ui.websites_expanded = true;
    if (target == "home.network") {
        ctx.ui.advanced_network = true; ctx.ui.proxy_rules_expanded = false;
    }
    if (target == "setup.remember") ctx.ui.setup_expanded = true;
    ctx.refresh();
}

void build_page(Builder& b, ViewContext& ctx, const EngineSnapshot& snap, const status::Presentation& p) {
    if (!ctx.app.notice().empty() && !(ctx.ui.page == Page::settings && ctx.app.notice() == "error.launchUpdate")) {
        b.banner("notice.close", "warning", T(ctx.app.notice()), L"", T("action.close"), [&ctx] {
            ctx.app.clear_notice();
            ctx.refresh();
        });
        b.gap(16);
    }
    switch (ctx.ui.page) {
        case Page::connection: connection_page(b, ctx, snap, p); break;
        case Page::records: records_page(b, ctx); break;
        case Page::settings: settings_page(b, ctx, snap, p); break;
    }
}

void build_scene(Scene& scene, Canvas& canvas, ViewContext& ctx, float width, float height) {
    if (ctx.ui.home_focus == "home.account") { ctx.ui.account_expanded = true; ctx.ui.show_password = false; }
    if (ctx.ui.home_focus == "home.websites") ctx.ui.websites_expanded = true;
    const std::vector<Layer> previous = scene.layers;
    scene.widgets.clear();
    scene.layers.assign(1 + ctx.ui.overlays.size(), Layer{});
    for (std::size_t i = 0; i < scene.layers.size() && i < previous.size(); ++i) scene.layers[i].scroll = previous[i].scroll;
    const EngineSnapshot snap = ctx.app.snapshot();
    const status::Presentation p = status::present(status::input_from(snap));

    const float bar_h = topbar(scene, canvas, ctx, width);
    scene.chrome_height = bar_h;
    float page_top = bar_h;
    if (ctx.ui.tutorial_visible) {
        Builder coach(scene, canvas, 0, 20, width - 40, bar_h + 12, false);
        build_tutorial_coach(coach, ctx, snap, p);
        page_top = coach.y() + 12;
        Widget background; background.kind = Kind::panel; background.fill = "surface";
        background.scrolls = false; background.rect = D2D1::RectF(0, bar_h, width, page_top);
        // Keep the coach under its text and buttons while it remains above page scrolling.
        const auto first = std::find_if(scene.widgets.begin(), scene.widgets.end(), [bar_h](const Widget& w) { return !w.scrolls && w.rect.top >= bar_h; });
        scene.widgets.insert(first, std::move(background));
        Widget edge; edge.kind = Kind::divider; edge.scrolls = false;
        edge.rect = D2D1::RectF(0, page_top - 1, width, page_top - 1); scene.widgets.push_back(std::move(edge));
    }
    const float padding = tokens::kSpaceXxl;
    const float content_max = ctx.ui.page == Page::connection ? tokens::kSizePanelMaxWidth : tokens::kSizeContentMaxWidth;
    const float column = std::min(content_max, width - 2 * padding);
    const float left = std::max(padding, (width - column) / 2);
    Builder page(scene, canvas, 0, left, column, page_top + tokens::kSpaceXl);
    build_page(page, ctx, snap, p);
    scene.layers[0].viewport = D2D1::RectF(0, page_top, width, height);
    scene.layers[0].content_height = page.y() + tokens::kSpaceXl - page_top;
    // The hero headline is the polite live region; the busy icon spins (unless reduced motion).
    for (Widget& w : scene.widgets) {
        if (w.layer == 0 && w.kind == Kind::text && w.style == TextStyle::hero) {
            w.role = Role::status;
            w.live = true;
            w.id = "status.headline";
        }
        if (w.kind == Kind::icon && w.icon == "loader") w.kind = Kind::spinner;
    }
    std::optional<float> tutorial_focus_top;
    if (ctx.ui.tutorial_visible) {
        if (const auto* target = scene.find(tutorial_target(ctx.ui, snap.recovery_setup_has_network)); target && target->scrolls) {
            Widget* card = nullptr;
            for (auto& w : scene.widgets) {
                if (w.kind != Kind::panel || !w.scrolls || w.layer != 0) continue;
                const auto& r = w.rect; const auto& t = target->rect;
                if (r.left <= t.left && r.right >= t.right && r.top <= t.top && r.bottom >= t.bottom &&
                    (!card || r.bottom - r.top < card->rect.bottom - card->rect.top)) card = &w;
            }
            if (card) { card->border = "accent"; card->checked = true; tutorial_focus_top = card->rect.top; }
            else {
                Widget outline; outline.kind = Kind::panel; outline.border = "accent"; outline.checked = true; outline.radius = 8;
                const auto r = target->rect;
                outline.rect = D2D1::RectF(r.left - 6, r.top - 6, r.right + 6, r.bottom + 6);
                tutorial_focus_top = outline.rect.top;
                scene.widgets.push_back(std::move(outline));
            }
        }
    }
    for (std::size_t i = 0; i < ctx.ui.overlays.size(); ++i) {
        const int layer = static_cast<int>(i) + 1;
        const auto& name = ctx.ui.overlays[i];
        if (name == "help" || name == "about" || name == "proxyAssistant") {
            // A stationary header sits above a separate scroll viewport.
            const float card_w = std::min(tokens::kSizeSheetMaxWidth, width - 32);
            const float x = (width - card_w) / 2;
            const float top = bar_h + 12, bottom = height - 12;
            const float inset = tokens::kSpaceXl;
            const auto title = T(name == "help" ? "help.title" : name == "proxyAssistant" ? "proxyAssistant.title" : "about.title");
            const float title_width = card_w - inset * 2 - 32 - tokens::kSpaceM - 44 - tokens::kSpaceS;
            const auto title_size = canvas.measure(title, TextStyle::title, title_width);
            const float head_h = std::max(tokens::kSizeMinTarget, title_size.height) + tokens::kSpaceS * 2;
            Builder host(scene, canvas, layer, x, card_w, top, false);
            const std::size_t frame_index = scene.widgets.size();
            Widget frame; frame.kind = Kind::panel; frame.id = "sheet.frame"; frame.fill = "background"; frame.border = "borderStrong"; frame.radius = 16;
            frame.rect = D2D1::RectF(x, top, x + card_w, bottom); host.add(std::move(frame));
            Widget head; head.kind = Kind::panel; head.fill = "surface"; head.radius = 16;
            head.rect = D2D1::RectF(x + 1, top + 1, x + card_w - 1, top + head_h); host.add(std::move(head));
            Widget square; square.kind = Kind::panel; square.fill = "surface";
            square.rect = D2D1::RectF(x + 1, top + 16, x + card_w - 1, top + head_h); host.add(std::move(square));
            Widget badge; badge.kind = Kind::panel; badge.fill = "accentSurface"; badge.radius = tokens::kRadiusMedium;
            badge.rect = D2D1::RectF(x + inset, top + (head_h - 32) / 2, x + inset + 32, top + (head_h + 32) / 2); host.add(std::move(badge));
            Widget symbol; symbol.kind = Kind::icon; symbol.icon = name == "help" ? "help" : "info"; symbol.color = "accent";
            symbol.rect = D2D1::RectF(x + inset + 6, top + (head_h - 20) / 2, x + inset + 26, top + (head_h + 20) / 2); host.add(std::move(symbol));
            host.text_at(title, TextStyle::title, "textPrimary", x + inset + 32 + tokens::kSpaceM, title_width, top + (head_h - title_size.height) / 2);
            scene.widgets.back().role = Role::heading; scene.widgets.back().id = "sheet.title";
            Widget close; close.kind = Kind::button; close.button = ButtonStyle::plain; close.icon = "x";
            close.id = "sheet.close"; close.name = T("action.close"); close.role = Role::button; close.focusable = true; close.radius = 8;
            close.rect = D2D1::RectF(x + card_w - inset - 44, top + (head_h - 44) / 2, x + card_w - inset, top + (head_h + 44) / 2);
            close.action = [&ctx] { ctx.close(); }; host.add(std::move(close));
            Widget edge; edge.kind = Kind::divider; edge.rect = D2D1::RectF(x + 1, top + head_h, x + card_w - 1, top + head_h); host.add(std::move(edge));
            const float body_top = top + head_h + 1;
            Builder body(scene, canvas, layer, x + inset, card_w - inset * 2, body_top + inset);
            build_overlay(body, ctx, name, snap, p);
            Layer& l = scene.layers[static_cast<std::size_t>(layer)];
            l.modal = true;
            const float fitted_bottom = name == "proxyAssistant" ? std::min(bottom, body.y() + inset) : bottom;
            scene.widgets[frame_index].rect.bottom = fitted_bottom;
            l.viewport = D2D1::RectF(x + 1, body_top, x + card_w - 1, fitted_bottom - 1);
            l.content_height = body.y() + inset - body_top;
            continue;
        }
        if (name == "websiteResult" || name == "wifiName" || name == "memoryRename" || name == "memoryForget" || name == "memoryForgetAll" || name == "accountDelete" || name == "proxyRestore" || name == "proxyDiscard") {
            // Compact auxiliary dialogs size to their actual content. Only their
            // body scrolls; the heading and close action remain at the top right.
            const float ideal_width = name == "websiteResult" ? tokens::kSizeWebsiteDetailsWidth :
                name == "accountDelete" ? 460.0f : tokens::kSizeNetworkNameDialogWidth;
            const float card_w = std::min(ideal_width, width - tokens::kSpaceL * 2);
            const float x = (width - card_w) / 2, inset = tokens::kSpaceL;
            const auto title = T(name == "websiteResult" ? "website.result.details" :
                name == "wifiName" ? "wifiRecovery.windows.manualSSID" : name == "memoryForget" ? "memory.forget" : name == "memoryForgetAll" ? "memory.forgetAll" :
                name == "accountDelete" ? "account.delete" : name == "proxyRestore" ? "proxyAssistant.restoreTitle" :
                name == "proxyDiscard" ? "proxyAssistant.discardTitle" : "memory.renameTitle");
            const float title_width = std::max(1.0f, card_w - inset * 2 - tokens::kSizeMinTarget - tokens::kSpaceS);
            const auto title_size = canvas.measure(title, TextStyle::headline, title_width);
            const float head_h = std::max(tokens::kSizeMinTarget, title_size.height) + tokens::kSpaceS * 2;
            const std::size_t body_begin = scene.widgets.size();
            Builder body(scene, canvas, layer, x + inset, card_w - inset * 2, 0);
            build_overlay(body, ctx, name, snap, p);
            const std::size_t body_end = scene.widgets.size();
            const float content_height = body.y() + inset * 2;
            Builder footer(scene, canvas, layer, x + inset, card_w - inset * 2, 0, false);
            if (name == "accountDelete") build_account_deletion_actions(footer, ctx);
            const std::size_t footer_end = scene.widgets.size();
            const float footer_height = name == "accountDelete" ? footer.y() + inset * 2 : 0;
            const float outer_gap = name == "accountDelete"
                ? std::min(tokens::kSpaceM, std::max(0.0f, (height - bar_h - head_h - tokens::kSizeMinTarget - 2) / 2))
                : tokens::kSpaceM;
            const float available_height = std::max(head_h + 2, height - bar_h - outer_gap * 2);
            // Keep a usable body viewport. On a very short work area the actions
            // join the scrollable body, while the close action stays fixed.
            const bool fixed_footer = footer_height > 0 && available_height >= head_h + footer_height + tokens::kSizeMinTarget + 2;
            const float reserved_footer = fixed_footer ? footer_height : 0;
            const float card_h = std::min(available_height, head_h + content_height + footer_height + 2);
            const float top = bar_h + outer_gap + (available_height - card_h) / 2;
            const float body_top = top + head_h + 1;
            for (std::size_t index = body_begin; index < body_end; ++index) {
                scene.widgets[index].rect.top += body_top + inset;
                scene.widgets[index].rect.bottom += body_top + inset;
            }
            const float footer_top = top + card_h - reserved_footer;
            for (std::size_t index = body_end; index < footer_end; ++index) {
                const float offset = fixed_footer ? footer_top + inset : body_top + content_height;
                scene.widgets[index].rect.top += offset;
                scene.widgets[index].rect.bottom += offset;
                scene.widgets[index].scrolls = !fixed_footer;
            }
            Builder host(scene, canvas, layer, x, card_w, top, false);
            Widget frame; frame.kind = Kind::panel; frame.fill = "surface"; frame.border = "borderStrong"; frame.radius = tokens::kRadiusLarge;
            frame.rect = D2D1::RectF(x, top, x + card_w, top + card_h); host.add(std::move(frame));
            host.text_at(title, TextStyle::headline, "textPrimary", x + inset, title_width, top + (head_h - title_size.height) / 2);
            scene.widgets.back().role = Role::heading; scene.widgets.back().id = "sheet.title";
            Widget close; close.kind = Kind::button; close.button = ButtonStyle::plain; close.icon = "x"; close.radius = tokens::kRadiusSmall;
            close.id = "sheet.close"; close.name = T("action.close"); close.role = Role::button; close.focusable = true;
            close.rect = D2D1::RectF(x + card_w - inset - tokens::kSizeMinTarget, top + (head_h - tokens::kSizeMinTarget) / 2,
                x + card_w - inset, top + (head_h + tokens::kSizeMinTarget) / 2);
            close.action = [&ctx] { ctx.close(); }; host.add(std::move(close));
            Widget edge; edge.kind = Kind::divider;
            edge.rect = D2D1::RectF(x + 1, top + head_h, x + card_w - 1, top + head_h); host.add(std::move(edge));
            if (fixed_footer) {
                Widget footer_edge; footer_edge.kind = Kind::divider;
                footer_edge.rect = D2D1::RectF(x + 1, footer_top, x + card_w - 1, footer_top);
                host.add(std::move(footer_edge));
            }
            // Fixed header comes first in keyboard/reading order as well as paint order.
            std::rotate(scene.widgets.begin() + body_begin, scene.widgets.begin() + footer_end, scene.widgets.end());
            Layer& l = scene.layers[static_cast<std::size_t>(layer)];
            l.modal = true; l.viewport = D2D1::RectF(x + 1, body_top, x + card_w - 1, footer_top - 1);
            const float footer_bottom_gap = std::min(inset, std::max(0.0f,
                l.viewport.bottom - l.viewport.top - footer.y()));
            l.content_height = content_height + (fixed_footer || !footer_height ? 0 : footer.y() + footer_bottom_gap);
            continue;
        }
        const float card_w = std::min(520.0f, width - 32);
        const float x = (width - card_w) / 2;
        Builder b(scene, canvas, layer, x, card_w, bar_h + 20);
        b.card_begin("surface", "borderStrong", 20, 16);
        build_overlay(b, ctx, ctx.ui.overlays[i], snap, p);
        b.card_end();
        Layer& l = scene.layers[static_cast<std::size_t>(layer)];
        l.modal = true;
        l.viewport = D2D1::RectF(0, bar_h, width, height);
        l.content_height = b.y() + 20 - bar_h;
    }
    // Caption backgrounds stop above the common edge; the separator is painted last.
    const char* icons[]{"minus", ctx.ui.maximized ? "window-restore" : "window-maximize", "x"};
    const char* ids[]{"frame.minimize", "frame.maximize", "frame.close"};
    const float button_w = ctx.ui.caption_width / 3;
    for (int i = 0; i < 3; ++i) {
        const float caption_x = width - ctx.ui.caption_width + i * button_w;
        Widget target; target.kind = Kind::panel; target.id = ids[i]; target.layer = scene.top_layer(); target.scrolls = false;
        target.rect = D2D1::RectF(caption_x, 0, caption_x + button_w, ctx.ui.caption_height - 1);
        scene.widgets.push_back(std::move(target));
        Widget glyph; glyph.kind = Kind::icon; glyph.id = std::string(ids[i]) + ".glyph";
        glyph.icon = icons[i]; glyph.layer = scene.top_layer(); glyph.scrolls = false;
        const float cx = caption_x + button_w / 2, cy = ctx.ui.caption_height / 2;
        glyph.rect = D2D1::RectF(cx - 7, cy - 7, cx + 7, cy + 7); scene.widgets.push_back(std::move(glyph));
    }
    update_caption_feedback(scene, ctx.ui);
    Widget edge; edge.kind = Kind::divider; edge.id = "frame.edge"; edge.layer = scene.top_layer(); edge.scrolls = false;
    edge.rect = D2D1::RectF(0, bar_h - 1, width, bar_h - 1); scene.widgets.push_back(std::move(edge));
    if (ctx.ui.overlays.empty() && !ctx.ui.home_focus.empty()) {
        if (ctx.ui.home_focus == "page.start") scene.layers[0].scroll = 0;
        else if (const auto* target = scene.find(ctx.ui.home_focus)) {
            const float top = tutorial_focus_top && ctx.ui.home_focus == tutorial_target(ctx.ui, snap.recovery_setup_has_network)
                ? *tutorial_focus_top : target->rect.top;
            scene.layers[0].scroll = std::max(0.0f, top - (page_top + 16));
        }
        ctx.ui.home_focus.clear();
    }
    for (int i = 0; i <= scene.top_layer(); ++i) scene.clamp_scroll(i);
}

void update_caption_feedback(Scene& scene, const UiState& ui) {
    for (auto& widget : scene.widgets) {
        const WPARAM hit = widget.id == "frame.minimize" || widget.id == "frame.minimize.glyph" ? HTMINBUTTON :
            widget.id == "frame.maximize" || widget.id == "frame.maximize.glyph" ? HTMAXBUTTON :
            widget.id == "frame.close" || widget.id == "frame.close.glyph" ? HTCLOSE : 0;
        if (!hit) continue;
        const bool hover = ui.hovered_caption == hit;
        if (widget.kind == Kind::panel)
            widget.fill = !hover ? "surface" : hit == HTCLOSE ? "danger" :
                ui.pressed_caption == hit ? "surfacePressed" : "surfaceMuted";
        else if (widget.kind == Kind::icon) widget.color = hover && hit == HTCLOSE ? "onAccent" : "textSecondary";
    }
}

}  // namespace szu::app
