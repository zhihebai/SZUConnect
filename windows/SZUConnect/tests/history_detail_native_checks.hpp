#pragma once
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

void verify_historical_probe_timing(HWND window, Canvas& canvas, float width, float height, RuntimeMode mode) {
    auto services = std::make_unique<wifiruntime::Fake>(); auto* fake = services.get();
    fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"automatic","probeMode":"quick","appearanceMode":"system"})";
    auto failed = ConnectivityReport::simple(false, false);
    failed.apple.elapsed = 1.234; failed.microsoft.elapsed = 0.875;
    records::HealthLog health; health.append(records::sample_from(failed, AppController::wall_now()));
    fake->files[L"health.json"] = json::serialize(health.to_json());
    AppController app(nullptr, mode, std::move(services)); app.start();
    const auto files = fake->files; const auto deadline = app.next_check(); const auto before = app.snapshot();
    FixtureHost host; host.window = window; host.scale = canvas.dpi() / 96;
    UiState ui; ui.page = Page::records;
    ViewContext context{app, ui, window, [] {}, [&](const std::string& name) { ui.overlays.push_back(name); },
        [&] { if (!ui.overlays.empty()) ui.overlays.pop_back(); }, [](const std::string&) {}};
    const auto build = [&] { build_scene(host.scene, canvas, context, width, height); };
    build();
    const auto cell = std::find_if(host.scene.widgets.begin(), host.scene.widgets.end(), [](const Widget& widget) {
        return widget.id.starts_with("records.cell.");
    });
    const std::string selected = cell == host.scene.widgets.end() ? std::string() : cell->id;
    check(!selected.empty() && invoke_r11_action(host, win::widen(selected)),
        "native UIA can select the retained failed check without running another probe");
    build();
    for (const auto* timing : {L"1234 ms", L"875 ms"}) {
        const auto value = std::find_if(host.scene.widgets.begin(), host.scene.widgets.end(), [&](const Widget& widget) {
            return widget.kind == Kind::text && widget.text.find(timing) != std::wstring::npos;
        });
        check(value != host.scene.widgets.end(), "historical Apple and Microsoft failure details display their own actual elapsed time");
        if (value != host.scene.widgets.end()) {
            const auto facts = canvas.inspect_text_layout_for_testing(value->text, value->style, value->rect.right - value->rect.left);
            check(facts.lines <= 2 && facts.height <= value->rect.bottom - value->rect.top + 0.2f &&
                value->rect.left >= 0 && value->rect.right <= width,
                "historical endpoint timings remain readable at narrow widths, configured DPI and large text");
        }
    }
    check(invoke_r11_action(host, L"records.health.info") && ui.overlays == std::vector<std::string>{"info:records"},
        "historical timing explanation uses the existing accessible records information popover");
    check(fake->files == files && app.next_check() == deadline && app.snapshot().epoch == before.epoch &&
        fake->evidence().fake_auth_submissions == 0 && fake->evidence().network_calls == 0 &&
        fake->evidence().vault_accesses == 0 && fake->evidence().production_storage_accesses == 0,
        "reading historical failure timing and its explanation writes no data and changes no recovery state");
    app.shutdown();
}
