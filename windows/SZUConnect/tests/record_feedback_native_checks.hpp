#pragma once
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

void verify_record_feedback_explanations(HWND window, Canvas& canvas, float width, float height, RuntimeMode mode) {
    auto services = std::make_unique<wifiruntime::Fake>(); auto* fake = services.get();
    fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
    records::HealthLog health;
    health.append(records::sample_from(ConnectivityReport::simple(true, true), AppController::wall_now(), 2, 1));
    fake->files[L"health.json"] = json::serialize(health.to_json());
    records::EventLog events;
    events.append(AppController::wall_now(), "Fixture feedback event", false);
    fake->files[L"events.json"] = json::serialize(events.to_json());
    AppController app(nullptr, mode, std::move(services)); app.start();
    const auto files = fake->files; const auto before = app.snapshot(); const auto deadline = app.next_check();
    const auto initial_report = json::parse(app.diagnostic_export_text());
    const auto initial_summary = app.diagnostic_summary_text();
    FixtureHost host; host.window = window; host.scale = canvas.dpi() / 96;
    UiState ui; ui.page = Page::records;
    ViewContext context{app, ui, window, [] {}, [&](const std::string& name) { ui.overlays.push_back(name); },
        [&] { if (!ui.overlays.empty()) ui.overlays.pop_back(); }, [](const std::string&) {}};
    const auto build = [&] { build_scene(host.scene, canvas, context, width, height); };
    build();
    for (const char* id : {"records.diagnostics.copyHint", "records.diagnostics.exportHint"}) {
        const auto* hint = host.scene.find(id);
        const auto facts = hint ? canvas.inspect_text_layout_for_testing(hint->text, hint->style,
            hint->rect.right - hint->rect.left) : TextLayoutFacts{};
        check(hint && hint->text == T(id) && facts.lines > 0 && facts.lines <= 2 && hint->rect.left >= 0 &&
            hint->rect.right <= width && facts.height <= hint->rect.bottom - hint->rect.top + 0.2f,
            "record feedback hints remain complete within two lines at narrow widths, configured DPI and large text");
    }
    const auto* copy = host.scene.find("records.copy");
    const auto* report = host.scene.find("records.export");
    const auto* info = host.scene.find("records.diagnostics.info");
    check(copy && report && info && copy->enabled && report->enabled && info->enabled &&
        copy->text == T("action.copyDiagnostics") && report->text == T("action.exportDiagnostics") &&
        info->name == info_label("recordDiagnostics"),
        "records distinguish text summary and report-file actions with one accessible explanation beside the section heading");
    check(initial_report && initial_report->find("app") && initial_report->find("settings") &&
        initial_report->find("summary") && initial_report->find("samples") && initial_report->find("events") &&
        initial_report->find("campusVerification") && initial_summary.find("Fixture feedback event") == std::string::npos &&
        app.diagnostic_export_text().find("Fixture feedback event") != std::string::npos,
        "the documented summary and detailed report match actual Controller output rather than being two names for the same data");
    const auto samples = initial_report && initial_report->find("samples") ? json::serialize(*initial_report->find("samples")) : std::string();
    const auto event_rows = initial_report && initial_report->find("events") ? json::serialize(*initial_report->find("events")) : std::string();
    ui.search = L"Fixture no matching record"; ui.failures_only = true; ui.showing_samples = true; build();
    const auto filtered_report = json::parse(app.diagnostic_export_text());
    check(filtered_report && filtered_report->find("samples") && filtered_report->find("events") &&
        json::serialize(*filtered_report->find("samples")) == samples && json::serialize(*filtered_report->find("events")) == event_rows &&
        app.diagnostic_summary_text() == initial_summary && host.scene.find("records.copy") && host.scene.find("records.export"),
        "search and failure-only view filters do not change either feedback document's retained data");
    check(invoke_r11_action(host, L"records.diagnostics.info") && ui.overlays == std::vector<std::string>{"info:recordDiagnostics"},
        "the feedback information action opens the specific summary-versus-report popover without exporting or copying");
    ui.overlays.clear(); build();
    check(invoke_r11_action(host, L"records.copy") && ui.records_feedback == "records.copyFailed",
        "an injected clipboard refusal reports a copy failure rather than asking the user to choose an export folder");
    build();
    const auto* feedback = host.scene.find("records.feedback");
    check(feedback && feedback->text == T("records.copyFailed") && feedback->color == "warning",
        "copy failures display the matching warning next to the feedback actions");
    const auto after = app.snapshot();
    check(fake->files == files && app.next_check() == deadline && after.epoch == before.epoch &&
        after.trusted == before.trusted && after.remembered == before.remembered && after.holds.size() == before.holds.size() &&
        fake->evidence().network_calls == 0 && fake->evidence().fake_auth_submissions == 0 &&
        fake->evidence().fake_wifi_calls == 0 && fake->evidence().vault_accesses == 0 &&
        fake->evidence().production_storage_accesses == 0,
        "feedback explanations and isolated failure interactions never write real files, use the clipboard, upload reports or change recovery");
    const auto* hint = host.scene.find("records.diagnostics.exportHint");
    const float available_width = hint ? hint->rect.right - hint->rect.left : 0;
    for (const char* key : {"records.exporting", "records.exportCancelling", "records.exportCancelled", "records.exportDone", "records.exportFailed"}) {
        const auto facts = canvas.inspect_text_layout_for_testing(T(key), TextStyle::caption, available_width);
        check(available_width > 0 && facts.lines > 0 && facts.lines <= 2 && facts.height > 0,
            "every export status remains readable within two lines in the full narrow-window, DPI and large-text matrix");
    }
    const auto cancel_facts = canvas.inspect_text_layout_for_testing(T("records.cancelExport"), TextStyle::body, available_width);
    check(cancel_facts.lines == 1, "the stop-export label stays on one line throughout the configured layout matrix");
    app.shutdown();
}

void verify_record_export_interaction(HWND window, Canvas& canvas, float width, float height) {
    FixtureHost host; host.window = window; host.scale = canvas.dpi() / 96;
    auto export_services = std::make_unique<crashexportfixture::Fake>(); auto* export_fake = export_services.get(); export_fake->blocked = true;
    // Cancellation occurs during the injected scan, before any file is created.
    const auto report_path = (std::filesystem::temp_directory_path() /
        (L"SZUConnect-view-export-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".json")).wstring();
    AppController exporting(nullptr, RuntimeMode::normal, std::move(export_services), {}, {},
        [&](HWND) -> std::optional<std::wstring> { return report_path; }); exporting.start();
    UiState export_ui; export_ui.page = Page::records;
    ViewContext export_context{exporting, export_ui, window, [] {}, [&](const std::string& name) { export_ui.overlays.push_back(name); },
        [&] { if (!export_ui.overlays.empty()) export_ui.overlays.pop_back(); }, [](const std::string&) {}};
    build_scene(host.scene, canvas, export_context, width, height);
    check(invoke_r11_action(host, L"records.export"), "record export starts through the actual view action with an injected picker");
    wifiruntime::wait_for([&] { return export_fake->started.load(); });
    build_scene(host.scene, canvas, export_context, width, height);
    const auto* stop = host.scene.find("records.export");
    const auto* progress = host.scene.find("records.exportFeedback");
    check(stop && stop->text == T("records.cancelExport") && stop->enabled && progress && progress->text == T("records.exporting") &&
        export_ui.records_feedback.empty(), "pending export exposes a working stop action and does not display a premature saved message");
    check(invoke_r11_action(host, L"records.export"), "the same accessible record action cancels the pending export");
    crashexportfixture::wait(exporting);
    build_scene(host.scene, canvas, export_context, width, height);
    const auto* stopped = host.scene.find("records.exportFeedback");
    check(stopped && stopped->text == T("records.exportCancelled") && host.scene.find("records.export")->text == T("action.exportDiagnostics") &&
        !std::filesystem::exists(report_path), "cancelled export restores the export action and states that no report was saved");
    exporting.shutdown();
}
