#pragma once
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include "wifi_runtime_tests.hpp"

void verify_wifi_zero_selection_view(HWND window, Canvas& canvas, float width, float height, RuntimeMode mode) {
    for (const int situation : {0, 1, 2}) {
        auto services = std::make_unique<wifiruntime::Fake>(); auto* fake = services.get();
        fake->virtual_path = true; fake->physical_path = situation == 1;
        if (situation == 2) fake->files.erase(L"network-labels.json");
        wifi::Preferences preferences; preferences.auto_join = preferences.power_on = true;
        if (mode != RuntimeMode::observeOnly || situation != 0) preferences.excluded.insert(fake->id);
        fake->files[L"wifi-recovery.json"] = *preferences.encode();
        AppController app(nullptr, mode, std::move(services)); app.start();
        const auto files = fake->files; const auto before = app.snapshot(); const auto due = app.next_check();
        const bool has_targets = !app.wifi_candidates().empty();
        FixtureHost host; host.window = window;
        UiState ui;
        ViewContext context{app, ui, window, [] {}, [](const std::string&) {}, [] {}, [](const std::string&) {}};
        const auto build = [&] { build_scene(host.scene, canvas, context, width, height); };
        const auto notice_fits = [&] {
            bool measured = true;
            for (const auto& item : host.scene.widgets) {
                if (item.id.find("wifiSelection") == std::string::npos && item.id.find("wifi.selection") == std::string::npos &&
                    item.text != T("wifiRecovery.selectionRequired.title")) continue;
                const auto& r = item.rect;
                measured = measured && r.left >= 0 && r.right <= width + 0.1f && r.right > r.left;
                if (item.kind == Kind::text)
                    measured = measured && canvas.measure(item.text, item.style, r.right - r.left).height <= r.bottom - r.top + 0.2f;
                if (item.focusable) measured = measured && r.right - r.left >= tokens::kSizeMinTarget - 0.1f &&
                    r.bottom - r.top >= tokens::kSizeMinTarget - 0.1f;
            }
            return measured;
        };
        const auto profile_help_fits = [&] {
            const auto text = T("wifiRecovery.windows.openOnly");
            const auto item = std::find_if(host.scene.widgets.begin(), host.scene.widgets.end(), [&](const auto& widget) {
                return widget.kind == Kind::text && widget.text == text;
            });
            if (item == host.scene.widgets.end() || std::count(text.begin(), text.end(), L'\n') != 2) return false;
            const auto& r = item->rect;
            const auto available = r.right - r.left;
            const auto full = canvas.inspect_text_layout_for_testing(text, item->style, available);
            bool fixed = full.lines == 3 && full.height <= r.bottom - r.top + 0.2f &&
                full.width <= available + 0.2f && r.left >= 0 && r.right <= width + 0.1f;
            std::size_t start = 0;
            for (int line = 0; line < 3; ++line) {
                const auto end = text.find(L'\n', start);
                const auto semantic = text.substr(start, end == std::wstring::npos ? end : end - start);
                fixed = fixed && !semantic.empty() && canvas.inspect_text_layout_for_testing(semantic, item->style, available).lines == 1;
                start = end == std::wstring::npos ? text.size() : end + 1;
            }
            return fixed;
        };
        build();
        const auto* home_notice = host.scene.find("home.wifiSelection.detail");
        const auto* open = host.scene.find("home.wifiSelection.open");
        if (mode == RuntimeMode::observeOnly) {
            check(app.wifi_preferences().auto_join && !app.wifi_selection_required() && !home_notice && !open,
                "read-only home omits missing-target warnings and setup actions when its isolated memory cannot represent formal target choices");
            ui.page = Page::settings; build();
            check(profile_help_fits(), "read-only settings render saved-profile help as three complete semantic lines without extra wrapping or clipping");
            const auto* target = host.scene.find("wifi.network." + fake->id);
            check(!host.scene.find("wifi.selection.detail") && !host.scene.find("wifi.selection.open") &&
                (!target || !target->enabled),
                "read-only settings omit the missing-target warning and expose no enabled target control");
            if (target) {
                Accessibility access(host);
                ComPtr<IRawElementProviderFragment> root; root.Attach(access.root_for_testing());
                const auto node = find_node(root.Get(), win::widen("wifi.network." + fake->id));
                ComPtr<IToggleProvider> toggle; if (node) node.As(&toggle);
                check(!toggle || FAILED(toggle->Toggle()), "read-only native target toggles cannot alter formal selections");
                access.disconnect();
            }
            const auto selected = app.wifi_selected(fake->id);
            check(!app.set_wifi_selected(fake->id, !selected) && app.wifi_selected(fake->id) == selected,
                "read-only target commands preserve selected and excluded choices even when no target is rendered");
            app.on_timer(kTimerWiFi); app.on_changed();
            check(fake->files == files && app.next_check() == due && app.snapshot().epoch == before.epoch &&
                app.snapshot().trusted == before.trusted && app.snapshot().holds.size() == before.holds.size() &&
                fake->evidence().fake_wifi_calls == 0 && fake->evidence().fake_auth_submissions == 0 &&
                fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0,
                "read-only home and settings leave memory, credentials, finding choices, protections and monitoring untouched");
            app.shutdown(); continue;
        }
        check(app.wifi_selection_required() && home_notice && home_notice->role == Role::status &&
            home_notice->text == T(has_targets ? "wifiRecovery.selectionRequired.detail" : "wifiRecovery.selectionRequired.noNetworks") &&
            open && open->enabled && open->text == T("wifiRecovery.selectionRequired.action") && notice_fits(),
            "a controller with no selected target renders a readable actionable home warning despite TUN, an existing connection or missing names");
        check(invoke_r11_action(host, L"home.wifiSelection.open") && ui.page == Page::settings,
            "native UIA opens the Wi-Fi target settings directly without a confirmation or changing a target");
        build();
        check(profile_help_fits(), "normal settings render saved-profile help as three complete semantic lines without extra wrapping or clipping");
        const auto* settings_notice = host.scene.find("wifi.selection.detail");
        const auto* target = host.scene.find(has_targets ? "wifi.network." + fake->id : "wifi.targets");
        const auto target_rect = target ? host.scene.visible_rect(*target) : D2D1::RectF(0, 0, 0, 0);
        const auto viewport = host.scene.layers[0].viewport;
        check(settings_notice && settings_notice->role == Role::status && target && notice_fits() &&
            target_rect.top >= viewport.top - 0.1f && target_rect.bottom <= viewport.bottom + 0.1f &&
            (!has_targets || (!target->checked && target->enabled == (mode == RuntimeMode::normal))),
            "the settings warning stays independent of radio status and the home action scrolls to the actual unselected target or empty target section");
        check(fake->files == files && app.next_check() == due && app.snapshot().epoch == before.epoch &&
            app.snapshot().trusted == before.trusted && app.snapshot().holds.size() == before.holds.size() &&
            fake->evidence().fake_wifi_calls == 0 && fake->evidence().fake_auth_submissions == 0 && fake->evidence().network_calls == 0,
            "showing and navigating the warning retains the user's exclusions, network memory, protections and schedule without requests");
        if (has_targets) {
            Accessibility access(host);
            ComPtr<IRawElementProviderFragment> root; root.Attach(access.root_for_testing());
            const auto node = find_node(root.Get(), win::widen("wifi.network." + fake->id));
            ComPtr<IToggleProvider> toggle; if (node) node.As(&toggle);
            const HRESULT toggled = toggle ? toggle->Toggle() : E_FAIL;
            access.disconnect();
            check(SUCCEEDED(toggled) && app.wifi_selected(fake->id) && !app.wifi_selection_required(),
                "one explicit native target toggle completes finding setup without toggling either enabled switch again");
            build(); check(!host.scene.find("wifi.selection.detail"), "selecting a target removes the settings warning immediately");
            ui.page = Page::connection; build();
            check(!host.scene.find("home.wifiSelection.detail") &&
                fake->files[L"network-memory.json"] == files.at(L"network-memory.json") && fake->evidence().fake_wifi_calls == 0,
                "target selection clears the home warning without remembering the network again or starting a radio request");
        }
        app.shutdown();
    }
}
