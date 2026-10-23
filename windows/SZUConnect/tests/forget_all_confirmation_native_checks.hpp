#pragma once
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

void verify_forget_all_confirmation(HWND window, Canvas& canvas, float width, float height, RuntimeMode mode) {
    auto services = std::make_unique<memorymergeruntime::NameDisplayFake>(); auto* fake = services.get();
    AppController app(nullptr, mode, std::move(services)); app.start();
    const auto files = fake->files; const auto before = app.snapshot(); const auto deadline = app.next_check();
    FixtureHost host; host.window = window; host.scale = canvas.dpi() / 96;
    UiState ui; ui.page = Page::settings;
    ViewContext context{app, ui, window, [] {}, [&](const std::string& name) { ui.overlays.push_back(name); },
        [&] { if (!ui.overlays.empty()) ui.overlays.pop_back(); }, [](const std::string&) {}};
    const auto build = [&] { build_scene(host.scene, canvas, context, width, height); };
    const auto unchanged = [&] {
        const auto state = app.snapshot();
        return fake->files == files && app.next_check() == deadline && state.epoch == before.epoch &&
            state.trusted == before.trusted && state.remembered == before.remembered &&
            state.memory_count == before.memory_count && state.has_saved == before.has_saved &&
            state.paused == before.paused && state.holds.size() == before.holds.size();
    };
    build();
    if (mode == RuntimeMode::observeOnly) {
        const auto* action = host.scene.find("memory.forgetAll");
        check((!action || !action->enabled) && !invoke_r11_action(host, L"memory.forgetAll") && ui.overlays.empty() && unchanged(),
            "read-only settings cannot invoke the bulk network-forget confirmation");
        ui.overlays = {"memoryForgetAll"}; build();
        const auto* confirm = host.scene.find("memory.forgetAllConfirm");
        check(confirm && !confirm->enabled && !invoke_r11_action(host, L"memory.forgetAllConfirm") && unchanged(),
            "even an injected read-only bulk-forget dialog has no enabled destructive action");
        check(invoke_r11_action(host, L"sheet.close") && ui.overlays.empty() && unchanged(),
            "read-only bulk-forget preview can close without changing formal or isolated data");
        app.forget_all_networks();
        check(unchanged(), "read-only controller rejects bulk forgetting independently of the view");
    } else {
        check(invoke_r11_action(host, L"memory.forgetAll") && ui.overlays == std::vector<std::string>{"memoryForgetAll"} && unchanged(),
            "bulk-forget entry opens confirmation before changing memory, trust, finding targets or the monitoring deadline");
        build();
        const auto* cancel = host.scene.find("memory.forgetAllCancel");
        const auto* confirm = host.scene.find("memory.forgetAllConfirm");
        const auto* close = host.scene.find("sheet.close");
        const auto* title = host.scene.find("sheet.title");
        const auto focus = host.scene.focus_order();
        const auto explanation = std::find_if(host.scene.widgets.begin(), host.scene.widgets.end(), [](const Widget& item) {
            return item.layer == 1 && item.kind == Kind::text && item.text == T("memory.forgetAllConfirmDetail");
        });
        bool readable = explanation != host.scene.widgets.end();
        if (readable) {
            const auto& rect = explanation->rect;
            const float available = rect.right - rect.left;
            readable = rect.left >= 0 && rect.right <= width &&
                canvas.measure(explanation->text, explanation->style, available).height <= rect.bottom - rect.top + 0.2f;
            std::size_t begin = 0;
            for (;;) {
                const auto end = explanation->text.find(L"\n\n", begin);
                const auto paragraph = explanation->text.substr(begin, end == std::wstring::npos ? end : end - begin);
                readable = readable && !paragraph.empty() && canvas.inspect_text_layout_for_testing(paragraph,
                    explanation->style, available).lines <= 2;
                if (end == std::wstring::npos) break;
                begin = end + 2;
            }
        }
        check(cancel && confirm && close && title && title->text == T("memory.forgetAll") && cancel->enabled && confirm->enabled &&
            cancel->rect.left < confirm->rect.left && !close->scrolls && !focus.empty() && focus.front() == close && readable,
            "bulk-forget confirmation has readable short paragraphs, a fixed close control and a safe initial keyboard target at narrow DPI layouts");
        check(invoke_r11_action(host, L"memory.forgetAllCancel") && ui.overlays.empty() && unchanged(),
            "cancelling bulk forgetting preserves every file, trust state, finding choice and existing deadline");
        build(); invoke_r11_action(host, L"memory.forgetAll"); build();
        check(invoke_r11_action(host, L"sheet.close") && ui.overlays.empty() && unchanged(),
            "closing bulk-forget confirmation has the same non-destructive result as cancel");
        build(); invoke_r11_action(host, L"memory.forgetAll"); build();
        check(invoke_r11_action(host, L"memory.forgetAllConfirm") && ui.overlays.empty(),
            "explicit confirmation invokes bulk forgetting once and closes its dialog");
        const auto stored = NetworkMemory::decode(fake->files[L"network-memory.json"], AppController::wall_now());
        const auto finding = wifi::Preferences::decode(fake->files[L"wifi-recovery.json"]);
        check(app.remembered_networks().empty() && !app.snapshot().remembered && !app.snapshot().trusted && stored && stored->size() == 0 &&
            finding && !finding->auto_join && finding->manual_names.empty() && app.snapshot().has_saved == before.has_saved &&
            fake->files[L"settings.json"] == files.at(L"settings.json"),
            "confirmed bulk forgetting revokes all memory and finding while retaining the account and unrelated application settings");
        build();
        check(!host.scene.find("memory.forgetAll") && host.scene.top_layer() == 0,
            "settings remove the bulk-forget action after the last record has been removed");
    }
    app.shutdown();
    check(fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 &&
        fake->evidence().production_storage_accesses == 0 && fake->evidence().fake_auth_submissions == 0 &&
        fake->evidence().fake_wifi_calls == 0,
        "bulk-forget interaction tests never access real files, credentials, Wi-Fi or campus authentication");
}
