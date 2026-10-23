#pragma once
#include <functional>
#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "proxy_assistant_runtime_tests.hpp"
#include "proxy_assistant_suggestion_runtime_tests.hpp"

class ProxyViewFake final : public campusverificationruntime::Fake {
public:
    ProxyViewFake() : campusverificationruntime::Fake("manual") {}
    int selections = 0, saves = 0, restores = 0, discards = 0;
    bool backup = false;
    bool proxy_on = true, tunnel_known = true, virtual_tunnel = true;
    bool omit_system_campus = false;
    std::atomic<int> assistant_checks{0};
    win::NetworkSnapshot capture_network() override {
        auto result = campusverificationruntime::Fake::capture_network();
        result.tunnel_known = tunnel_known; result.tunnel.tunnel = virtual_tunnel;
        result.path.tunnel = virtual_tunnel;
        result.path.interfaces = "Fixture tunnel, Fixture Ethernet";
        return result;
    }
    proxy::WindowsRaw read_proxy() override {
        proxy::WindowsRaw result; result.proxy = proxy_on ? "http=127.0.0.1:7890" : ""; return result;
    }
    compat::Input proxy_config_check(bool proxy, const std::string& tunnel, const CancelToken&) override {
        ++assistant_checks;
        compat::Input result; result.proxy = proxy ? "on" : "off"; result.tunnel = tunnel;
        result.campus_direct = result.campus_system = result.public_direct = result.public_system = "reachable";
        if (omit_system_campus) result.campus_system = "notRun";
        return result;
    }
    proxyassist::Preview prepare_proxy_config(HWND, proxyassist::Format format) override {
        ++selections;
        return {format, "campus-fixture.json", {"rules", "dns", "tun"}, {}, true, true};
    }
    proxyassist::Result apply_proxy_config() override { ++saves; backup = true; return {{}, true, true}; }
    proxyassist::Result restore_proxy_config() override { ++restores; backup = false; return {{}, true, true}; }
    proxyassist::Result discard_proxy_config_backup() override { ++discards; backup = false; return {{}, true, true}; }
    bool proxy_config_has_backup() override { return backup; }
};

void verify_proxy_assistant_recommendation_view(HWND window, Canvas& canvas) {
    auto services = std::make_unique<proxysuggestionruntime::Fake>(); auto* fake = services.get();
    AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
    const auto files = fake->files;
    FixtureHost host; host.window = window;
    UiState ui;
    ViewContext context{app, ui, window, [] {}, [&](const std::string& overlay) { ui.overlays.push_back(overlay); },
        [&] { if (!ui.overlays.empty()) ui.overlays.pop_back(); }, [](const std::string&) {}};
    build_scene(host.scene, canvas, context, 500, 1200);
    check(!host.scene.find("proxy.suggestion.open"), "a TUN interface alone does not place a failure recommendation on the home page");
    proxysuggestionruntime::round(app, *fake);
    build_scene(host.scene, canvas, context, 500, 1200);
    check(!host.scene.find("proxy.suggestion.open"), "one completed failed check leaves the novice recommendation hidden");
    proxysuggestionruntime::round(app, *fake);
    build_scene(host.scene, canvas, context, 500, 1200);
    const auto* entry = host.scene.find("proxy.suggestion.open");
    const auto before = app.snapshot();
    const auto probes = fake->physical_calls.load() + fake->system_calls.load();
    check(entry && entry->action && host.scene.find("proxy.suggestion.detail"),
        "two actual failed controller rounds reach the displayed offline help entry");
    if (entry && entry->action) { const auto action = entry->action; action(); }
    build_scene(host.scene, canvas, context, 500, 1200);
    check(!ui.overlays.empty() && ui.overlays.back() == "proxyAssistant" && host.scene.find("proxy.client.generic") &&
        host.scene.find("proxy.chooseClient") && !host.scene.find("proxy.select"),
        "the failure action opens local client choices without service verification, region selection or file access");
    const auto after = app.snapshot();
    check(fake->files == files && fake->submissions == 0 && fake->discoveries == 0 &&
        fake->physical_calls + fake->system_calls == probes && after.trusted == before.trusted &&
        after.memory_count == before.memory_count && after.holds.size() == before.holds.size(),
        "opening recommended offline help leaves requests, data and authentication authority unchanged");
    app.close_proxy_assistant(); app.shutdown();
}

void verify_connection_details_and_website_scope(HWND window, Canvas& canvas, float width, float height, RuntimeMode mode) {
    auto services = std::make_unique<ProxyViewFake>(); auto* fake = services.get();
    AppController app(nullptr, mode, std::move(services)); app.start();
    const auto before = app.snapshot(); const auto due = app.next_check(); const auto files = fake->files;
    const auto checked = app.last_checked(), system_checked = app.system_checked_at();
    FixtureHost host; host.window = window;
    UiState ui; ui.websites_expanded = true;
    ViewContext context{app, ui, window, [] {}, [&](const std::string& overlay) { ui.overlays.push_back(overlay); },
        [&] { if (!ui.overlays.empty()) ui.overlays.pop_back(); }, [](const std::string&) {}};
    const auto build = [&] { build_scene(host.scene, canvas, context, width, height); };
    const auto contains = [](const D2D1_RECT_F& outer, const D2D1_RECT_F& inner) {
        return inner.left >= outer.left - 0.1f && inner.right <= outer.right + 0.1f &&
            inner.top >= outer.top - 0.1f && inner.bottom <= outer.bottom + 0.1f;
    };
    const auto fits = [&](const char* id) {
        const auto* item = host.scene.find(id);
        if (!item) return false;
        const auto& rect = item->rect;
        const auto& layer = host.scene.layers[static_cast<std::size_t>(item->layer)];
        const float bottom = item->scrolls ? layer.viewport.top + layer.content_height : height;
        return std::isfinite(rect.left) && std::isfinite(rect.right) && std::isfinite(rect.top) && std::isfinite(rect.bottom) &&
            rect.left >= 0 && rect.right <= width + 0.1f && rect.top >= 0 && rect.bottom <= bottom + 0.5f &&
            rect.right > rect.left && rect.bottom > rect.top &&
            (!item->focusable || (rect.right - rect.left >= tokens::kSizeMinTarget - 0.1f &&
                rect.bottom - rect.top >= tokens::kSizeMinTarget - 0.1f)) &&
            (item->kind != Kind::text || canvas.measure(item->text, item->style, rect.right - rect.left).height <=
                rect.bottom - rect.top + 0.2f);
    };
    build();
    const auto* websites = host.scene.find("home.websites");
    const auto* strategy = host.scene.find("internet.strategy.heading");
    const auto* direct = host.scene.find("internet.strategy.1");
    const auto* network = host.scene.find("network.disclosure");
    const auto card = std::find_if(host.scene.widgets.begin(), host.scene.widgets.end(), [&](const Widget& item) {
        return item.kind == Kind::panel && item.fill == "surface" && websites && strategy && direct &&
            contains(item.rect, websites->rect) && contains(item.rect, strategy->rect) && contains(item.rect, direct->rect);
    });
    bool strategy_geometry = true;
    for (const auto* id : {"internet.strategy.heading", "internet.strategy.heading.info", "internet.strategy.scope",
        "internet.strategy.0", "internet.strategy.1", "internet.strategy.detail"}) strategy_geometry = fits(id) && strategy_geometry;
    check(card != host.scene.widgets.end() && network && card->rect.bottom < network->rect.top && strategy_geometry &&
        !ui.advanced_network && !host.scene.find("network.environment.campusService"),
        "website detection preferences stay inside their own website card and remain readable with network details collapsed");
    check(invoke_r11_action(host, L"internet.strategy.heading.info") && !ui.overlays.empty() &&
        ui.overlays.back() == "info:websiteStrategy", "the relocated website-scope explanation keeps its native accessible information action");
    ui.overlays.clear(); build();
    check(invoke_r11_action(host, L"network.disclosure"), "native UIA opens the single network-details disclosure");
    build();
    const auto* interfaces = host.scene.find("network.environment.interfaces");
    const auto* authentication = host.scene.find("network.environment.authenticationTarget");
    const auto* primary = host.scene.find("network.environment.systemPrimary");
    const auto* campus = host.scene.find("network.environment.campusService");
    bool connection_geometry = true;
    for (const auto* id : {"network.proxyModes", "network.proxyModes.info", "network.refresh", "network.environment.access",
        "network.environment.proxy", "network.environment.tunnel", "network.environment.interfaces",
        "network.environment.authenticationTarget", "network.environment.systemPrimary", "network.environment.campusService"})
        connection_geometry = fits(id) && connection_geometry;
    check(ui.advanced_network && connection_geometry && interfaces && interfaces->text == win::widen(app.network().path.interfaces) &&
        authentication && authentication->text == win::widen(app.authentication_target_name()) &&
        primary && primary->text == win::widen(app.physical_system_primary_name()) && campus &&
        campus->text == T("env.connectionDetails.notChecked") && !host.scene.find("network.technical") &&
        !host.scene.find("network.technical.summary") && !host.scene.find("network.websiteStrategy"),
        "expanded connection details directly display aligned connection, login-network, system-primary and campus-service values without another advanced layer");
    for (const auto* state : {"notChecked", "checking", "confirmed", "unconfirmed"}) {
        auto snapshot = app.snapshot(); snapshot.network_available = true; snapshot.sleeping = false;
        snapshot.verifying_memory = false; snapshot.verifying_campus = std::string(state) == "checking"; snapshot.busy = false;
        snapshot.last_discovery = std::string(state) == "notChecked" ? std::optional<CampusDiscovery>{} :
            std::optional<CampusDiscovery>{CampusDiscovery{Discovery::confirmed, Discovery::confirmed}};
        snapshot.required_campus_verification = std::string(state) == "confirmed" ? Discovery::confirmed : Discovery::unconfirmed;
        host.scene = Scene{};
        host.scene.layers[0].viewport = D2D1::RectF(0, 0, width, height);
        Builder detail_builder(host.scene, canvas, 0, tokens::kSpaceL, width - 2 * tokens::kSpaceL, tokens::kSpaceL);
        build_network_details(detail_builder, context, snapshot);
        host.scene.layers[0].content_height = detail_builder.y() + tokens::kSpaceL;
        campus = host.scene.find("network.environment.campusService");
        check(campus && campus->text == T(std::string("env.connectionDetails.") + state) && fits("network.environment.campusService"),
            "direct campus-service row presents each unchecked, running, confirmed and failed state without clipping or an extra disclosure");
    }
    build();
    check(invoke_r11_action(host, L"network.disclosure"), "the same disclosure closes connection details");
    build();
    const auto* system = host.scene.find("internet.strategy.0");
    direct = host.scene.find("internet.strategy.1");
    check(!ui.advanced_network && system && direct && system->checked && !direct->checked &&
        system->enabled == (mode == RuntimeMode::normal) && direct->enabled == (mode == RuntimeMode::normal) &&
        !host.scene.find("network.environment.campusService"),
        "website-scope options remain present independently of network folding and are disabled only in read-only mode");
    const auto direct_action = direct ? direct->action : std::function<void()>{};
    bool selected = false;
    {
        Accessibility access(host); ComPtr<IRawElementProviderFragment> root;
        root.Attach(access.root_for_testing());
        const auto node = find_node(root.Get(), L"internet.strategy.1");
        ComPtr<IRawElementProviderSimple> simple;
        if (node) node.As(&simple);
        ComPtr<IUnknown> pattern;
        ComPtr<ISelectionItemProvider> selection;
        selected = simple && SUCCEEDED(simple->GetPatternProvider(UIA_SelectionItemPatternId, pattern.GetAddressOf())) && pattern &&
            SUCCEEDED(pattern.As(&selection)) && selection && SUCCEEDED(selection->Select());
        access.disconnect();
    }
    if (mode == RuntimeMode::observeOnly && direct_action) direct_action();
    build();
    const auto* detail = host.scene.find("internet.strategy.detail");
    if (mode == RuntimeMode::normal) {
        auto expected = files;
        const auto saved = fake->files.find(L"websites.json");
        if (saved != fake->files.end()) expected[L"websites.json"] = saved->second;
        check(selected && !app.website_follow_system_proxy() && saved != fake->files.end() && fake->files == expected &&
            detail && detail->text == T("website.strategy.direct.detail"),
            "native selection changes only injected website preferences and renders the newly selected next-round scope");
    } else check(!selected && app.website_follow_system_proxy() && fake->files == files &&
        detail && detail->text == T("website.strategy.system.detail"),
        "read-only native and direct callbacks cannot write the relocated website preferences");
    const auto after = app.snapshot();
    check(app.next_check() == due && app.last_checked() == checked && app.system_checked_at() == system_checked &&
        fake->probes == 0 && fake->system_probes == 0 && fake->discoveries == 0 && fake->assistant_checks == 0 &&
        fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0 &&
        after.trusted == before.trusted && after.memory_count == before.memory_count && after.holds.size() == before.holds.size(),
        "opening connection details and selecting a future website scope never detects, authenticates, reschedules or changes network authority");
    app.shutdown();
}

void verify_help_quick_start_disclosure(HWND window, Canvas& canvas, float width, float height, RuntimeMode mode) {
    auto services = std::make_unique<ProxyViewFake>(); auto* fake = services.get();
    AppController app(nullptr, mode, std::move(services)); app.start();
    const auto files = fake->files; const auto before = app.snapshot(); const auto due = app.next_check();
    const int credentials = fake->credential_reads;
    FixtureHost host; host.window = window;
    UiState ui; ui.overlays = {"help"};
    ViewContext context{app, ui, window, [] {}, [&](const std::string& overlay) { ui.overlays.push_back(overlay); },
        [&] { if (!ui.overlays.empty()) ui.overlays.pop_back(); }, [](const std::string&) {}};
    const auto build = [&] { build_scene(host.scene, canvas, context, width, height); };
    const auto faq_present = [&] {
        bool present = host.scene.find("help.faqTitle") != nullptr;
        for (const auto* question : {"wifi", "cooldown", "noInternet", "rejected", "vpn", "nonCampus", "dorm", "official"})
            present = host.scene.find(std::string("help.faq.") + question) && present;
        return present;
    };
    const auto body_visible = [&](bool expected) {
        bool correct = (host.scene.find("help.quickStart.intro") != nullptr) == expected;
        for (int step = 0; step < 6; ++step)
            for (const auto* suffix : {".title", ".detail"}) {
                const auto* item = host.scene.find("help.quickStart.step." + std::to_string(step) + suffix);
                correct = (item != nullptr) == expected && correct;
                if (!item) continue;
                const auto& rect = item->rect;
                const auto& layer = host.scene.layers[static_cast<std::size_t>(item->layer)];
                correct = !item->text.empty() && rect.left >= 0 && rect.right <= width + 0.1f &&
                    rect.bottom <= layer.viewport.top + layer.content_height + 0.5f &&
                    canvas.measure(item->text, item->style, rect.right - rect.left).height <= rect.bottom - rect.top + 0.2f && correct;
            }
        return correct;
    };
    build();
    const auto* disclosure = host.scene.find("help.quickStart.disclosure");
    check(disclosure && disclosure->role == Role::button && disclosure->enabled && disclosure->focusable &&
        !disclosure->checked && host.scene.find("help.quickStart") && !ui.help_quick_start_expanded &&
        body_visible(false) && faq_present(),
        "help initially shows the first-use heading and independent FAQs while all six first-use steps remain collapsed");
    {
        Accessibility access(host); ComPtr<IRawElementProviderFragment> root;
        root.Attach(access.root_for_testing());
        const auto node = find_node(root.Get(), L"help.quickStart.disclosure");
        ComPtr<IRawElementProviderSimple> simple;
        if (node) node.As(&simple);
        ComPtr<IUnknown> invoke_pattern, toggle_pattern;
        const bool invoke_available = simple && SUCCEEDED(simple->GetPatternProvider(UIA_InvokePatternId, invoke_pattern.GetAddressOf()));
        const bool toggle_checked = simple && SUCCEEDED(simple->GetPatternProvider(UIA_TogglePatternId, toggle_pattern.GetAddressOf()));
        check(invoke_available && invoke_pattern && toggle_checked && !toggle_pattern,
            "the first-use disclosure exposes native InvokePattern rather than a settings TogglePattern");
        access.disconnect();
    }
    check(invoke_r11_action(host, L"help.quickStart.disclosure"), "native UIA expands the first-use steps");
    build();
    check(ui.help_quick_start_expanded && body_visible(true) && faq_present(),
        "expanded first-use help retains every readable step and leaves the FAQ section available");
    check(invoke_r11_action(host, L"help.faq.noInternet"), "a FAQ can open independently of first-use help");
    build();
    check(host.scene.find("help.faq.noInternet.answer") && invoke_r11_action(host, L"sheet.close"),
        "the native top-right close leaves the open help state in this session");
    build();
    check(ui.overlays.empty() && !host.scene.find("help.quickStart") && invoke_r11_action(host, L"nav.help"),
        "navigation reopens help after its close action");
    build();
    check(ui.help_quick_start_expanded && body_visible(true) && host.scene.find("help.faq.noInternet.answer") &&
        invoke_r11_action(host, L"help.quickStart.disclosure"),
        "reopening retains expanded first-use and FAQ state until the user explicitly collapses first-use help");
    build();
    check(!ui.help_quick_start_expanded && body_visible(false) && faq_present() && host.scene.find("help.faq.noInternet.answer"),
        "collapsing first-use help removes only its instructions without hiding or closing the independently expanded FAQ");
    check(invoke_r11_action(host, L"sheet.close"), "help closes after first-use instructions are collapsed");
    build();
    check(invoke_r11_action(host, L"nav.help"), "help can reopen with its session-local collapsed preference");
    build();
    const auto after = app.snapshot();
    check(!ui.help_quick_start_expanded && body_visible(false) && faq_present() && fake->files == files &&
        fake->credential_reads == credentials && app.next_check() == due && fake->probes == 0 && fake->system_probes == 0 &&
        fake->discoveries == 0 && fake->assistant_checks == 0 && fake->evidence().fake_auth_submissions == 0 &&
        fake->evidence().fake_wifi_calls == 0 && fake->evidence().vault_accesses == 0 &&
        after.trusted == before.trusted && after.memory_count == before.memory_count && after.holds.size() == before.holds.size(),
        "help disclosure and session reopening never persist settings, request credentials, detect connectivity or alter network authority");
    app.shutdown();
}

void verify_proxy_assistant_view(HWND window, Canvas& canvas, float width, float height, RuntimeMode mode) {
    auto services = std::make_unique<ProxyViewFake>(); auto* fake = services.get();
    AppController app(nullptr, mode, std::move(services)); app.start();
    const auto before = app.snapshot(); const auto due = app.next_check(); const auto files = fake->files;
    app.open_proxy_assistant();
    FixtureHost host; host.window = window;
    UiState ui; ui.advanced_network = true; ui.websites_expanded = true;
    int copy_callbacks = 0;
    ViewContext context{app, ui, window, [] {}, [&](const std::string& overlay) { ui.overlays.push_back(overlay); },
        [&] { if (!ui.overlays.empty()) ui.overlays.pop_back(); }, [&](const std::string& id) { ++copy_callbacks; ui.copied = id; }};
    const auto build = [&] {
        build_scene(host.scene, canvas, context, width, height);
        bool geometry = true, readable = true, reachable = true;
        for (const auto& widget : host.scene.widgets) {
            if (widget.layer == 0) continue; // The underlying connection page has its own scroll viewport.
            const auto& r = widget.rect;
            const auto& layer = host.scene.layers[static_cast<std::size_t>(widget.layer)];
            geometry = geometry && std::isfinite(r.left) && std::isfinite(r.top) && std::isfinite(r.right) && std::isfinite(r.bottom) &&
                r.left >= -0.1f && r.right <= width + 0.1f && r.top >= 0 && r.right > r.left && r.bottom >= r.top;
            if (widget.kind == Kind::text && !widget.text.empty())
                readable = readable && canvas.measure(widget.text, widget.style, r.right - r.left).height <= r.bottom - r.top + 0.2f;
            const float bottom = widget.scrolls ? layer.viewport.top + layer.content_height : height;
            reachable = reachable && r.bottom <= bottom + 0.5f;
            if (widget.focusable) reachable = reachable && r.right - r.left >= tokens::kSizeMinTarget - 0.1f &&
                r.bottom - r.top >= tokens::kSizeMinTarget - 0.1f;
        }
        check(geometry && readable && reachable, "proxy sheet text and targets fit horizontally and remain reachable through their own scroll viewport");
        if (!ui.overlays.empty() && ui.overlays.back() == "proxyAssistant") {
            for (const auto& [text_id, copy_id] : std::vector<std::pair<const char*, const char*>>{
                {"proxy.generic.domain", "proxy.generic.copyDomain"}, {"proxy.generic.address", "proxy.generic.copyAddress"},
                {"proxy.party.domain", "proxy.party.domain.copy"}, {"proxy.party.address", "proxy.party.address.copy"},
                {"proxy.verge.domain", "proxy.verge.copyDomain"}, {"proxy.verge.address", "proxy.verge.copyAddress"}}) {
                const auto* target_text = host.scene.find(text_id);
                const auto* copy_button = host.scene.find(copy_id);
                if (!target_text && !copy_button) continue;
                const auto text_facts = target_text ? canvas.inspect_text_layout_for_testing(target_text->text, target_text->style,
                    target_text->rect.right - target_text->rect.left) : TextLayoutFacts{};
                const bool inline_target = target_text && copy_button && target_text->rect.right <= copy_button->rect.left - tokens::kSpaceS + 0.2f;
                const float text_center = target_text ? target_text->rect.top + text_facts.height / 2 : 0;
                const float button_center = copy_button ? (copy_button->rect.top + copy_button->rect.bottom) / 2 : 0;
                check(target_text && copy_button && copy_button->enabled && copy_button->focusable &&
                    text_facts.lines > 0 && text_facts.lines <= 2 && text_facts.height <= target_text->rect.bottom - target_text->rect.top + 0.2f &&
                    ((inline_target && std::abs(text_center - button_center) < 0.5f) ||
                        (!inline_target && copy_button->rect.top >= target_text->rect.bottom + tokens::kSpaceXs - 0.2f)),
                    "fixed campus targets use native font height to center one- or two-line text beside copy actions, with a deliberate readable stack when space is insufficient");
            }
            const auto* selected = host.scene.find("proxy.selectedClient");
            const auto* change = host.scene.find("proxy.changeClient");
            const auto* change_label = host.scene.find("proxy.changeClient.label");
            const auto* steps = host.scene.find("proxy.steps.heading");
            if (selected || change || change_label) {
                const auto contains = [](const D2D1_RECT_F& outer, const D2D1_RECT_F& inner) {
                    return inner.left >= outer.left - 0.1f && inner.right <= outer.right + 0.1f &&
                        inner.top >= outer.top - 0.1f && inner.bottom <= outer.bottom + 0.1f;
                };
                const bool same_line = selected && change_label && std::abs((selected->rect.top + selected->rect.bottom) -
                    (change_label->rect.top + change_label->rect.bottom)) < 0.5f;
                const bool separate_lines = selected && change_label && change_label->rect.top >= selected->rect.bottom - 0.1f;
                check(selected && change && change_label && steps && change->role == Role::button && change->enabled && change->focusable &&
                    selected->style == TextStyle::body_strong && change_label->style == TextStyle::body_strong &&
                    contains(change->rect, selected->rect) && contains(change->rect, change_label->rect) &&
                    change->rect.bottom - change->rect.top >= tokens::kSizeMinTarget - 0.1f &&
                    std::abs(change->rect.left - steps->rect.left) < 0.2f && std::abs(change->rect.right - steps->rect.right) < 0.2f &&
                    ((same_line && selected->rect.right <= change_label->rect.left - tokens::kSpaceS + 0.2f) || separate_lines),
                    "the complete selected-client row is one accessible target with consistently styled centered text or an intentional nonoverlapping two-line layout");
                if (steps && change) check(steps->rect.top - change->rect.bottom >= tokens::kSpaceS - 0.2f &&
                    steps->rect.top - change->rect.bottom <= tokens::kSpaceM * 2 + tokens::kSizeBorderWidth + 0.2f,
                    "selected-client instructions follow one compact divider rhythm rather than stacked empty gaps");
            }
            const auto* frame = host.scene.find("sheet.frame");
            float content_bottom = 0;
            if (frame) for (const auto& item : host.scene.widgets)
                if (item.layer == frame->layer && item.scrolls) content_bottom = std::max(content_bottom, item.rect.bottom);
            const float expected_bottom = std::min(height - 12, content_bottom + tokens::kSpaceXl);
            check(frame && frame->layer > 0 && content_bottom > 0 &&
                std::abs(frame->rect.bottom - expected_bottom) < 0.5f &&
                std::abs(host.scene.layers[static_cast<std::size_t>(frame->layer)].viewport.bottom - (frame->rect.bottom - 1)) < 0.2f &&
                host.scene.find("sheet.close") && !host.scene.find("sheet.close")->scrolls,
                "short proxy sheets end at their real content plus normal padding while long sheets keep a bounded scroll viewport and fixed close");
            const bool ordinary_client = !ui.proxy_client_picker &&
                (ui.proxy_assistant_client == 0 || ui.proxy_assistant_client >= 3);
            const auto* hint = host.scene.find("proxy.modeHint");
            const auto expected = T("proxyAssistant.ruleMode");
            const auto copies = std::count_if(host.scene.widgets.begin(), host.scene.widgets.end(), [&](const Widget& item) {
                return item.kind == Kind::text && item.text == expected;
            });
            check(ordinary_client ? hint && hint->text == expected && copies == 1 : !hint && copies == 0,
                "each ordinary client guide has one routing-mode explanation, while the picker and independent file formats make no client-mode claim");
            if (hint) {
                bool short_paragraphs = true;
                for (std::size_t begin = 0; begin < hint->text.size();) {
                    const auto end = hint->text.find(L"\n\n", begin);
                    const auto paragraph = hint->text.substr(begin, end == std::wstring::npos ? end : end - begin);
                    short_paragraphs = canvas.measure(paragraph, hint->style, hint->rect.right - hint->rect.left).height <=
                        2 * canvas.line_height(hint->style) + 0.5f && short_paragraphs;
                    if (end == std::wstring::npos) break;
                    begin = end + 2;
                }
                check(short_paragraphs, "the routing-mode explanation stays within two lines per paragraph in narrow and large-text client guides");
            }
        }
    };
    const auto invoke = [&](const char* id) {
        const auto* widget = host.scene.find(id);
        if (!widget || !widget->enabled || !widget->action) return false;
        const auto action = widget->action; action(); build(); return true;
    };
    const auto choose_client = [&](const char* id, bool file_options = false) {
        if (host.scene.find("proxy.changeClient")) {
            if (!invoke("proxy.changeClient")) return false;
            check(ui.proxy_client_picker && !host.scene.find("proxy.selectedClient") && !host.scene.find("proxy.select") &&
                !host.scene.find("proxy.copyScript") && !host.scene.find("proxy.fileSteps") && !host.scene.find("proxy.manualRules") &&
                !host.scene.find("proxy.v2rayn.copyRules") && !host.scene.find("proxy.party.domain") &&
                !host.scene.find("proxy.verge.domain") && !host.scene.find("proxy.generic.domain"),
                "reopening the client picker hides every previous client guide and file editor instead of mixing old operations with new choices");
        }
        if (file_options && !host.scene.find(id) && !invoke("proxy.fileOptions")) return false;
        return invoke(id);
    };
    build();
    bool information_geometry = true;
    for (const auto* id : {"network.proxyModes.info", "internet.strategy.heading.info", "network.environment.proxy", "network.environment.tunnel"}) {
        const auto* item = host.scene.find(id);
        if (!item) { information_geometry = false; continue; }
        const auto& r = item->rect;
        const auto& layer = host.scene.layers[static_cast<std::size_t>(item->layer)];
        const float bottom = item->scrolls ? layer.viewport.top + layer.content_height : height;
        information_geometry = information_geometry && r.left >= 0 && r.right <= width + 0.1f && r.bottom <= bottom + 0.5f &&
            r.right > r.left && r.bottom > r.top && (!item->focusable ||
                (r.right - r.left >= tokens::kSizeMinTarget - 0.1f && r.bottom - r.top >= tokens::kSizeMinTarget - 0.1f));
        if (item->kind == Kind::text)
            information_geometry = information_geometry && canvas.measure(item->text, item->style, r.right - r.left).height <= r.bottom - r.top + 0.2f;
    }
    check(information_geometry, "proxy information controls and simultaneous state rows fit narrow high-DPI text and remain scroll-reachable");
    check(!ui.proxy_advanced_expanded && !ui.proxy_verge_tun_expanded &&
        !host.scene.find("network.proxyRules") && !host.scene.find("compat.run") && !host.scene.find("compat.stop"),
        "connection details retain no duplicate manual-rule or campus-diagnosis entry and new disclosures start closed");
    for (const auto& [id, topic] : std::vector<std::pair<const char*, const char*>>{
        {"network.proxyModes.info", "info:proxyModes"}, {"internet.strategy.heading.info", "info:websiteStrategy"}}) {
        const auto* info = host.scene.find(id);
        check(info && info->enabled && info->action, "proxy-mode and website-scope explanations have reachable native information buttons");
        check(invoke_r11_action(host, win::widen(id)), "native UIA invokes each new explanation without starting a request");
        check(!ui.overlays.empty() && ui.overlays.back() == topic, "each information button opens its own explanation topic");
        ui.overlays.clear(); build();
    }
    const auto* proxy_value = host.scene.find("network.environment.proxy");
    const auto* tunnel_value = host.scene.find("network.environment.tunnel");
    check(proxy_value && proxy_value->text == T("env.proxy.on") && tunnel_value && tunnel_value->text == T("env.tunnel.detected") &&
        tunnel_value->text.find(L"已开启") == std::wstring::npos && tunnel_value->text.find(L"已启用") == std::wstring::npos,
        "system-proxy and virtual-connection evidence can both be displayed without treating the modes as exclusive");
    for (const bool known : {false, true}) {
        fake->tunnel_known = known; fake->virtual_tunnel = false; app.refresh_environment(); build();
        tunnel_value = host.scene.find("network.environment.tunnel");
        check(tunnel_value && tunnel_value->text == T(known ? "env.tunnel.notDetected" : "env.tunnel.unknown") &&
            tunnel_value->text.find(L"已关闭") == std::wstring::npos,
            "unavailable or absent virtual-route evidence is never presented as a confirmed client TUN-off setting");
    }
    fake->virtual_tunnel = true; app.refresh_environment();
    check(fake->files == files && fake->selections == 0 && fake->assistant_checks == 0 && fake->probes == 0 && fake->discoveries == 0,
        "information topics and local environment refresh change no files and start no connectivity or campus request");
    ui.overlays = {"proxyAssistant"};
    build();
    check(!host.scene.find("proxy.resolver") && !host.scene.find("proxy.resolver.detail") && !host.scene.find("proxy.dnsHelp") &&
        host.scene.find("proxy.check") && invoke_r11_action(host, L"proxy.check.info") &&
        !ui.overlays.empty() && ui.overlays.back() == "info:addressResolution",
        "the connection-check heading opens its combined comparison and address-resolution explanation without duplicate footer entries or starting a request");
    ui.overlays = {"proxyAssistant"}; build();
    check(host.scene.find("proxy.chooseClient") && host.scene.find("proxy.format.verge") && host.scene.find("proxy.client.generic") &&
        host.scene.find("proxy.client.v2rayn") && host.scene.find("proxy.client.party") &&
        !host.scene.find("proxy.format.mihomo") && !host.scene.find("proxy.format.singbox") &&
        !host.scene.find("proxy.select") && !ui.proxy_file_choices_expanded && fake->selections == 0,
        "first opening offers novice client guides while full-file formats remain behind an explicit advanced disclosure");
    const auto* client_heading = host.scene.find("proxy.client.heading");
    const auto* check_heading = host.scene.find("proxy.check");
    bool rows_aligned = client_heading && check_heading;
    float previous_bottom = client_heading ? client_heading->rect.bottom : 0;
    for (const auto* id : {"proxy.format.verge", "proxy.client.v2rayn", "proxy.client.party", "proxy.client.generic"}) {
        const auto* row = host.scene.find(id);
        rows_aligned = rows_aligned && row && row->focusable && row->enabled && row->rect.top >= previous_bottom &&
            row->rect.bottom - row->rect.top >= tokens::kSizeMinTarget - 0.1f && client_heading &&
            std::abs(row->rect.left - client_heading->rect.left) < 0.2f;
        if (row) previous_bottom = row->rect.bottom;
    }
    check(rows_aligned && check_heading && previous_bottom < check_heading->rect.top &&
        host.scene.find("proxy.intro") && host.scene.find("proxy.check.info"),
        "client choices share full-width list rows and precede a separate connection-check group at every DPI and text scale");
    check(invoke("proxy.client.generic") && host.scene.find("proxy.generic.scope") && !host.scene.find("proxy.generic.steps") &&
        !host.scene.find("proxy.select") && !host.scene.find("proxy.restore"),
        "selecting a general client opens its complete instructions directly without another disclosure, unsupported import or backup action");
    bool direct_steps = true;
    for (const char* key : {"proxyAssistant.generic.step1", "proxyAssistant.generic.step2", "proxyAssistant.generic.step3"})
        direct_steps = direct_steps && std::any_of(host.scene.widgets.begin(), host.scene.widgets.end(), [&](const Widget& item) {
            return item.kind == Kind::text && item.text == T(key);
        });
    check(direct_steps && host.scene.find("proxy.generic.domain") && host.scene.find("proxy.generic.address") &&
        host.scene.find("proxy.generic.copyDomain") && host.scene.find("proxy.generic.copyAddress") && !host.scene.find("proxy.generic.limit"),
        "all three general-client steps and both fixed campus targets are immediately visible with separate copy actions");
    ui.copied = "proxy.generic.copyDomain"; build();
    const auto* copied_domain = host.scene.find("proxy.generic.copyDomain");
    const auto* copied_address = host.scene.find("proxy.generic.copyAddress");
    check(copied_domain && copied_address && copied_domain->text == T("action.copied") &&
        copied_address->text == T("proxyAssistant.generic.copyAddress"),
        "local guide copy feedback belongs only to the selected target");
    ui.copied.clear();
    check(invoke("proxy.check.info") && ui.overlays.back() == "info:addressResolution",
        "the complete general guide reaches the shared check explanation without an extra guide or DNS-help disclosure");
    ui.overlays = {"proxyAssistant"}; build();
    check(fake->selections == 0 && fake->saves == 0 && fake->probes == 0 && fake->discoveries == 0,
        "the novice guide works entirely offline before any file selection or service verification");
    const auto copy_action = [&](const char* id) {
        ui.copied.clear(); build();
        const auto before_files = fake->files;
        const int callbacks = copy_callbacks;
        check(app.offline_smoke() && invoke_r11_action(host, win::widen(id)),
            "native UIA invokes the offline copy control without reading or replacing the real system clipboard");
        build();
        check(ui.copied.empty() && copy_callbacks == callbacks && fake->files == before_files && fake->selections == 0 && fake->saves == 0,
            "offline copy admission changes no data, opens no file picker and does not claim a successful clipboard write");
        ui.copied = id; build();
        const auto* label = host.scene.find(id);
        check(label && label->text == T("action.copied"), "successful copy feedback is rendered only on its own client action");
        ui.copied.clear(); build();
    };
    check(host.scene.find("proxy.generic.domain") && host.scene.find("proxy.generic.address"),
        "returning from the check explanation leaves both general-client copy targets immediately available");
    copy_action("proxy.generic.copyDomain");
    copy_action("proxy.generic.copyAddress");
    check(choose_client("proxy.client.v2rayn") && host.scene.find("proxy.v2rayn.copyRules") &&
        !host.scene.find("proxy.select") && !host.scene.find("proxy.copyScript") && !host.scene.find("proxy.restore") &&
        !host.scene.find("proxy.manualRules"), "v2rayN has a complete graphical route guide without full-file or backup operations");
    copy_action("proxy.v2rayn.copyRules");
    check(choose_client("proxy.client.party") && host.scene.find("proxy.party.domain") && host.scene.find("proxy.party.address") &&
        !host.scene.find("proxy.select") && !host.scene.find("proxy.copyScript") && !host.scene.find("proxy.restore") &&
        !host.scene.find("proxy.manualRules"), "Clash Party has graphical fixed-target steps without a full-file editor or automatic backup claim");
    copy_action("proxy.party.domain.copy");
    copy_action("proxy.party.address.copy");
    for (const auto* format : {"proxy.format.verge", "proxy.format.mihomo", "proxy.format.singbox"}) {
        const bool verge = std::string(format) == "proxy.format.verge";
        const bool mihomo = std::string(format) == "proxy.format.mihomo";
        build(); check(choose_client(format, !verge), "the selected-client header can reopen its picker and reach explicit advanced file formats");
        check(host.scene.find("proxy.selectedClient") && host.scene.find("proxy.changeClient") && !ui.proxy_client_picker,
            "selection leaves a compact client heading and a reachable change action instead of repeating the complete picker");
        check(!ui.proxy_advanced_expanded && !ui.proxy_verge_tun_expanded && !ui.proxy_rules_expanded,
            "changing client resets the remaining advanced, TUN and manual-rule disclosures");
        if (verge) {
            check(host.scene.find("proxy.verge.domain") && host.scene.find("proxy.verge.address") &&
                host.scene.find("proxy.verge.copyDomain") && host.scene.find("proxy.verge.copyAddress") &&
                !host.scene.find("proxy.select") && !host.scene.find("proxy.copyScript") && !host.scene.find("proxy.manualRules"),
                "Verge initially presents its graphical rule guide with target copies instead of file and script operations");
            copy_action("proxy.verge.copyDomain");
            copy_action("proxy.verge.copyAddress");
            check(invoke("proxy.verge.tunSteps") && host.scene.find("proxy.verge.tunDetail") && !host.scene.find("proxy.select"),
                "optional Verge TUN instructions remain separate from file editing");
            check(invoke("proxy.advanced") && host.scene.find("proxy.copyScript") && host.scene.find("proxy.select"),
                "an explicit advanced disclosure exposes complete script and file operations");
        } else check(host.scene.find("proxy.fileSteps") && !host.scene.find("proxy.copyScript"),
            "independent full configuration formats have their own file steps and no Verge script action");
        if (verge || mihomo) {
            check(host.scene.find("proxy.manualRules") && !host.scene.find("proxy.manualRules.summary") && invoke("proxy.manualRules") &&
                host.scene.find("proxy.manualRules.summary") && host.scene.find("compat.copyRules"),
                "manual Clash/Mihomo rules are available only inside the selected assistant advanced workflow");
        } else check(!host.scene.find("proxy.manualRules"), "sing-box does not present Clash syntax as an applicable rule list");
        const auto* close = host.scene.find("sheet.close");
        const auto* select = host.scene.find("proxy.select");
        check(close && !close->scrolls && close->action && select && select->enabled == (mode == RuntimeMode::normal),
            "proxy sheet has a fixed top-right close and protects file access in read-only mode");
        if (mode == RuntimeMode::normal) {
            check(invoke("proxy.select") && host.scene.find("proxy.file") && host.scene.find("proxy.save"),
                "explicit selection renders the filename, fixed change summary and save action");
            check(invoke("proxy.save") && host.scene.find("proxy.feedback") && !host.scene.find("proxy.save"),
                "successful save displays reload guidance and consumes the staged preview");
            check(invoke("proxy.restore") && ui.overlays.back() == "proxyRestore", "restore requires a second explicit dialog");
            check(invoke("proxy.restoreCancel") && ui.overlays.back() == "proxyAssistant", "cancel restore preserves the selected configuration");
            check(invoke("proxy.restore") && invoke("proxy.restoreConfirm") && ui.overlays.back() == "proxyAssistant",
                "explicit native restore returns to the assistant and disables exhausted backup");
            check(invoke("proxy.select") && invoke("proxy.save") && invoke("proxy.discard") && ui.overlays.back() == "proxyDiscard",
                "keeping a working configuration requires an independent backup-discard confirmation");
            check(invoke("proxy.discardCancel") && fake->backup && fake->discards + 1 == fake->restores,
                "cancelling backup removal preserves the available recovery material");
            check(invoke("proxy.discard") && invoke("proxy.discardConfirm") && !fake->backup && ui.overlays.back() == "proxyAssistant",
                "explicit backup removal returns to the assistant without restoring the external configuration");
        } else {
            check(!invoke("proxy.select") && !invoke("proxy.restore"), "read-only proxy actions do not dispatch file operations");
        }
        canvas.reset_drawn_text_layouts_for_testing();
        check(canvas.begin(), "begin native proxy sheet draw");
        paint(canvas, make_palette(false, false), host.scene, {}, width, height, 0);
        check(canvas.end() && canvas.drawn_text_layouts_for_testing() > 5, "actual proxy sheet text is measured and painted natively");
    }
    check(fake->selections == (mode == RuntimeMode::normal ? 6 : 0) && fake->saves == fake->selections && fake->restores * 2 == fake->selections && fake->discards == fake->restores,
        "all displayed proxy format transactions reach the injected runtime exclusively through explicit actions");
    check(app.diagnostic_summary_text().find("campus-fixture.json") == std::string::npos &&
        app.diagnostic_export_text().find("campus-fixture.json") == std::string::npos,
        "the selected proxy configuration name is absent from copied and JSON diagnostics");
    fake->omit_system_campus = true;
    check(!host.scene.find("proxy.diagnosisDetails") && !host.scene.find("proxy.test.feedback") &&
        !host.scene.find("compat.table.campus.direct") && invoke("proxy.test"),
        "an unrun diagnosis has no stale result table and one explicit assistant action starts its check");
    wifiruntime::wait_for([&] { app.on_changed(); return app.compat().done; }); build();
    const auto has_text = [&](const std::wstring& text) {
        return std::any_of(host.scene.widgets.begin(), host.scene.widgets.end(), [&](const Widget& item) { return item.text == text; });
    };
    const auto* direct_heading = host.scene.find("compat.table.direct");
    const auto* system_heading = host.scene.find("compat.table.system");
    bool table = direct_heading && system_heading && direct_heading->rect.right <= system_heading->rect.left &&
        direct_heading->rect.top == system_heading->rect.top;
    for (const char* row : {"campus", "public"}) {
        const std::string prefix = std::string("compat.table.") + row;
        const auto* label = host.scene.find(prefix + ".label");
        const auto* direct = host.scene.find(prefix + ".direct");
        const auto* system = host.scene.find(prefix + ".system");
        const auto* divider = host.scene.find(std::string("compat.table.divider.") + row);
        table = table && label && direct && system && divider && label->rect.right <= direct->rect.left &&
            direct->rect.right <= system->rect.left && direct->style == TextStyle::callout && system->style == TextStyle::callout &&
            direct->text == T("compat.result.reachable") && system->text == T(std::string(row) == "campus" ? "compat.result.notRun" : "compat.result.reachable");
    }
    check(fake->assistant_checks == 1 && table && host.scene.find("compat.table.divider.header") &&
        !host.scene.find("proxy.test.feedback") && !host.scene.find("proxy.diagnosisDetails"),
        "completed diagnosis directly displays the two-column table with aligned headings, row dividers and an honest unmeasured system-campus cell");
    check(has_text(T("compat.verdict.allReachable.title")) &&
        has_text(T("compat.row.campus")) && has_text(T("compat.row.public")) && has_text(T("compat.rec.nothingToFix")) &&
        !host.scene.find("compat.run") && !host.scene.find("compat.stop"),
        "the direct results preserve both address rows, verdict and recommendation without a summary duplicate, result disclosure or second run control");
    const auto after = app.snapshot();
    check(fake->files == files && app.next_check() == due && before.trusted == after.trusted && before.memory_count == after.memory_count &&
        before.holds.size() == after.holds.size() && before.paused == after.paused && fake->probes == 0 && fake->discoveries == 0 &&
        fake->evidence().fake_auth_submissions == 0, "proxy UI transactions do not change campus authority, timers, credentials or saved application data");
    app.close_proxy_assistant(); app.shutdown();
}
