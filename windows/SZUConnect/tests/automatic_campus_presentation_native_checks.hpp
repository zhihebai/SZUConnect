#pragma once
#include <algorithm>
#include <memory>
#include <string>

#include "license_documents.hpp"

namespace {

class NativeCampusPresentationRuntime final : public r11presentationruntime::Fake {
public:
    using r11presentationruntime::Fake::Fake;
    RuntimeMode mode() const override { return RuntimeMode::normal; }
    CampusDiscovery discover(CampusArea area, const CancelToken& token) override {
        const auto result = r11presentationruntime::Fake::discover(area, token);
        return issue == CampusVerificationIssue::none ? result :
            CampusDiscovery{Discovery::unconfirmed, Discovery::notChecked, false, issue};
    }
    CampusVerificationIssue issue = CampusVerificationIssue::none;
};

void verify_automatic_campus_presentation(HWND window, Canvas& canvas, float width) {
    auto services = std::make_unique<NativeCampusPresentationRuntime>("recover", CampusArea::teaching);
    auto* fake = services.get();
    AppController app(nullptr, RuntimeMode::normal, std::move(services), [](UINT) { return true; }); app.start();
    const auto files = fake->files;
    FixtureHost host; host.window = window;
    UiState ui;
    ViewContext context{app, ui, window, [] {}, [](const std::string&) {},
        [&] { if (!ui.overlays.empty()) ui.overlays.pop_back(); }, [](const std::string&) {}};
    EngineSnapshot state = app.snapshot();
    state.online = true; state.status = EngineStatus::online;
    state.auto_login = AutoLoginState::awaitingProof;
    state.required_campus_verification = Discovery::notChecked;
    state.last_discovery.reset();
    const auto build = [&] {
        host.scene = {};
        host.scene.layers[0].viewport = D2D1::RectF(0, 0, width, 2000);
        Builder builder(host.scene, canvas, 0, tokens::kSpaceL, width - 2 * tokens::kSpaceL, tokens::kSpaceL);
        builder.card_begin("surface", "border", tokens::kSpaceL, tokens::kRadiusLarge);
        build_inline_guard(builder, context, state);
        builder.card_end();
        host.scene.layers[0].content_height = builder.y() + tokens::kSpaceL;
    };
    for (int phase = 0; phase < 4; ++phase) {
        state.busy = state.verifying_campus = state.auto_verifying_campus = phase == 1;
        state.automatic_campus_verification_failed = state.automatic_campus_verification_pending = phase == 2;
        state.status = phase == 1 ? EngineStatus::discovering : EngineStatus::online;
        state.required_campus_verification = phase == 2 ? Discovery::unconfirmed :
            phase == 3 ? Discovery::confirmed : Discovery::notChecked;
        if (phase >= 2) state.last_discovery = CampusDiscovery{
            phase == 3 ? Discovery::confirmed : Discovery::unconfirmed,
            phase == 3 ? Discovery::confirmed : Discovery::unconfirmed};
        state.auto_login = phase == 3 ? AutoLoginState::ready : AutoLoginState::awaitingProof;
        build();
        const char* hint_key = phase == 0 ? "campus.verify.autoPendingHint" : phase == 1 ? "campus.verify.checking" :
            phase == 2 ? "campus.verify.autoFailed" : "campus.verify.confirmedHint";
        const auto* hint = host.scene.find("guard.campus.hint");
        check(hint && hint->text == T(hint_key) && hint->role == Role::status &&
            (phase != 1 || host.scene.find("guard.memory.cancel")),
            "native remembered-network guard renders automatic pending, checking, retry and confirmed states with cancellation");
        const bool conflicting_recovery = std::any_of(host.scene.widgets.begin(), host.scene.widgets.end(), [](const Widget& item) {
            return item.text == T("auto.prioritizeCampus");
        });
        check(!conflicting_recovery, "online and service-verification guard states never also claim a campus-login recovery is running");
        verify_bounds(canvas, host.scene, width, 2000);
        Accessibility access(host); ComPtr<IRawElementProviderFragment> root;
        root.Attach(access.root_for_testing());
        const auto node = find_node(root.Get(), L"guard.campus.hint");
        check(node && property(node.Get(), UIA_NamePropertyId) == T(hint_key),
            "native UIA exposes each automatic verification explanation exactly as rendered at narrow widths and large text");
        access.disconnect();
    }

    const auto scheduled = app.next_check();
    for (const auto issue : {CampusVerificationIssue::dnsTls, CampusVerificationIssue::tls, CampusVerificationIssue::none}) {
        fake->issue = issue;
        const int completed = fake->returned + 1;
        check(app.verify_campus_service(), "native failure hint fixture starts an isolated credential-free Controller verification");
        const auto expected_feedback = issue == CampusVerificationIssue::none ? "campus.verify.confirmed" : "campus.verify.failed";
        // Core completion can precede release of the Controller worker slot.
        // Its reserved monitoring deadline is restored only after that release.
        r11presentationruntime::drain(app, [&] { return !app.snapshot().busy && fake->returned == completed &&
            app.memory_verification_message() == expected_feedback && app.next_check().has_value(); });
        state = app.snapshot(); build();
        const auto* hint = host.scene.find("guard.memory.feedback");
        const char* key = issue == CampusVerificationIssue::dnsTls ? "campus.verify.dnsTlsHint" :
            issue == CampusVerificationIssue::tls ? "campus.verify.tlsHint" : "campus.verify.confirmed";
        check(hint && hint->text == T(key) && hint->role == Role::status,
            "actual Controller DNS-TLS and campus-TLS failures render distinct explanations, and successful verification clears them");
        bool short_paragraphs = hint != nullptr;
        if (hint) for (std::size_t begin = 0; begin < hint->text.size();) {
            const auto end = hint->text.find(L"\n\n", begin);
            const auto paragraph = hint->text.substr(begin, end == std::wstring::npos ? end : end - begin);
            short_paragraphs = canvas.measure(paragraph, hint->style, hint->rect.right - hint->rect.left).height <=
                2 * canvas.line_height(hint->style) + 0.5f && short_paragraphs;
            if (end == std::wstring::npos) break;
            begin = end + 2;
        }
        check(short_paragraphs, "native verification explanations fit two lines per paragraph at narrow widths and large text");
        verify_bounds(canvas, host.scene, width, 2000);
        Accessibility access(host); ComPtr<IRawElementProviderFragment> root;
        root.Attach(access.root_for_testing());
        const auto node = find_node(root.Get(), L"guard.memory.feedback");
        check(node && property(node.Get(), UIA_NamePropertyId) == T(key),
            "UIA exposes the actual Controller failure explanation rather than a generic campus certificate warning");
        access.disconnect();
        const auto exported = json::parse(app.diagnostic_export_text());
        const auto* campus = exported ? exported->find("campusVerification") : nullptr;
        const auto token = issue == CampusVerificationIssue::dnsTls ? "dnsTls" : issue == CampusVerificationIssue::tls ? "tls" : "none";
        const auto next = app.next_check();
        check(campus && campus->string_at("issue") == token &&
            app.diagnostic_summary_text().find(std::string("issue=") + token) != std::string::npos &&
            (!scheduled || (next && *next >= *scheduled)),
            "the same Controller result reaches native UI, copied diagnostics and JSON without accelerating the existing plan");
    }

    for (const bool replace : {false, true}) {
        fake->issue = CampusVerificationIssue::dnsTls;
        fake->block = true; fake->released = false;
        const int completed = fake->returned + 1;
        check(app.verify_campus_service(), "native late DNS-TLS fixture starts isolated verification");
        wifiruntime::wait_for([&] { return fake->discoveries == completed; });
        if (replace) { fake->changed = true; app.refresh_environment(); }
        else app.cancel();
        fake->released = true;
        r11presentationruntime::drain(app, [&] { return !app.snapshot().busy && fake->returned == completed && app.next_check().has_value(); });
        state = app.snapshot(); build();
        const auto* feedback = host.scene.find("guard.memory.feedback");
        const auto* hint = host.scene.find("guard.campus.hint");
        const auto invalid_hint = T("campus.verify.dnsTlsHint");
        check((!feedback || feedback->text != invalid_hint) && (!hint || hint->text != invalid_hint) &&
            !state.last_discovery && app.diagnostic_summary_text().find("issue=dnsTls") == std::string::npos,
            "cancelled or replaced Controller verification discards late DNS-TLS results before they reach native text or copied diagnostics");
        const auto exported = json::parse(app.diagnostic_export_text());
        const auto* campus = exported ? exported->find("campusVerification") : nullptr;
        check(campus && campus->string_at("issue") == "none",
            "late DNS-TLS failures after cancellation or network replacement cannot reappear in JSON exports");
        Accessibility access(host); ComPtr<IRawElementProviderFragment> root;
        root.Attach(access.root_for_testing());
        for (const auto* id : {L"guard.memory.feedback", L"guard.campus.hint"}) {
            const auto node = find_node(root.Get(), id);
            check(!node || property(node.Get(), UIA_NamePropertyId) != invalid_hint,
                "native accessibility excludes a deliberately late DNS-TLS explanation after cancellation or replacement");
        }
        access.disconnect();
    }

    ui.overlays = {"about"}; ui.page = Page::settings;
    const auto about = [&] { build_scene(host.scene, canvas, context, width, 1200); };
    about();
    const auto project_link_count = std::count_if(host.scene.widgets.begin(), host.scene.widgets.end(), [](const Widget& item) {
        return item.layer == 1 && item.role == Role::link;
    });
    check(project_link_count == 1 && host.scene.find("about.project") && host.scene.find("about.licenses") &&
        !host.scene.find("about.creditsTitle") && !host.scene.find("about.atmluck"),
        "About keeps its canonical project link and collapsible licenses without a duplicate credit section or upstream link");
    const auto close = host.scene.find("sheet.close");
    check(close && !close->scrolls && close->enabled && close->action &&
        close->rect.bottom <= host.scene.layers[1].viewport.top && close->rect.right <= width,
        "About retains an accessible fixed top-right close button at narrow widths and large text");
    const auto* license_group = host.scene.find("about.licenses.disclosure");
    check(license_group && !license_group->checked && license_group->enabled &&
        std::none_of(license_documents().begin(), license_documents().end(), [&](const auto& document) {
            return host.scene.find(std::string("about.license.") + document.id) != nullptr;
        }), "About initially hides the bundled component list behind one collapsed license group");
    check(invoke_r11_action(host, L"about.licenses.disclosure"), "native UIA expands the license component group");
    about();
    bool scrolled_license = false;
    for (const auto& document : license_documents()) {
        const std::string id = document.id;
        const std::wstring control = win::widen("about.license." + id);
        const auto* collapsed = host.scene.find("about.license." + id);
        check(collapsed && !collapsed->checked && !host.scene.find("about.license.text." + id) &&
            !document.text.empty(), "each bundled full license starts collapsed with its real offline resource available");
        check(invoke_r11_action(host, control), "native UIA expands the actual embedded-license disclosure");
        about();
        const auto* expanded = host.scene.find("about.license.text." + id);
        const auto* copy = host.scene.find("about.license.copy." + id);
        check(expanded && expanded->text == win::widen(document.text) && copy && copy->enabled && copy->focusable && copy->action,
            "expanded About license preserves every original byte and exposes its reachable copy action without touching the clipboard");
        host.scene.layers[1].scroll = host.scene.layers[1].content_height;
        host.scene.clamp_scroll(1);
        scrolled_license |= host.scene.layers[1].scroll > 0;
        if (id == "autoszuweb") {
            check(host.scene.find("about.license.autoszuweb")->text == L"AutoSZUWeb · MIT" &&
                document.text.find("Copyright (c) 2026 ATMLuck") != std::string::npos &&
                document.text.find("Permission is hereby granted") != std::string::npos &&
                document.text.find("THE SOFTWARE IS PROVIDED \"AS IS\"") != std::string::npos,
                "upstream name, attribution and complete MIT terms remain readable in the offline About license");
        }
        check(invoke_r11_action(host, control), "native UIA collapses the same license independently");
        about();
        check(!host.scene.find("about.license.text." + id), "collapsed About disclosure removes only its own long license body");
    }
    check(scrolled_license && invoke_r11_action(host, L"sheet.close") && ui.overlays.empty(),
        "native UIA reaches the fixed About close action after scrolling and license interactions");
    check(fake->files == files && fake->probes == 0 && fake->discoveries == 5 &&
        fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0 &&
        fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 &&
        fake->evidence().production_storage_accesses == 0,
        "automatic-service verification and About rendering/UIA use injected data only, preserve files and never submit credentials or access production storage");
    app.shutdown();
}

}  // namespace
