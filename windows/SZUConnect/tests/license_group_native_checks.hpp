#pragma once
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

void verify_license_group(HWND window, Canvas& canvas, float width, float height, RuntimeMode mode) {
    auto services = std::make_unique<NativeCampusPresentationRuntime>("manual", CampusArea::teaching);
    auto* fake = services.get();
    AppController app(nullptr, mode, std::move(services)); app.start();
    const auto files = fake->files; const auto before = app.snapshot(); const auto deadline = app.next_check();
    FixtureHost host; host.window = window; host.scale = canvas.dpi() / 96;
    UiState ui; ui.page = Page::settings; ui.overlays = {"about"};
    ui.help_quick_start_expanded = true; ui.account_feedback = "fixture-account-feedback";
    ViewContext context{app, ui, window, [] {}, [&](const std::string& name) {
        if (name == "about") ui.reset_about_presentation();
        ui.overlays.push_back(name);
    }, [&] { if (!ui.overlays.empty()) ui.overlays.pop_back(); }, [](const std::string&) {}};
    const auto build = [&] { build_scene(host.scene, canvas, context, width, height); };
    const auto hidden_children = [&] {
        return std::none_of(license_documents().begin(), license_documents().end(), [&](const auto& document) {
            const std::string id = document.id;
            return host.scene.find("about.license." + id) || host.scene.find("about.license.copy." + id) ||
                host.scene.find("about.license.text." + id);
        });
    };
    build();
    const auto expected_title = T("about.licenses.title") + L" · " + F("about.licenses.count", {{"count", std::to_string(license_documents().size())}});
    const auto* group = host.scene.find("about.licenses.disclosure");
    const auto* heading = host.scene.find("about.licenses");
    const bool heading_fits = heading && canvas.inspect_text_layout_for_testing(heading->text, heading->style,
        heading->rect.right - heading->rect.left).lines == 1 && heading->rect.left >= 0 && heading->rect.right <= width &&
        canvas.measure(heading->text, heading->style, heading->rect.right - heading->rect.left).height <= heading->rect.bottom - heading->rect.top + 0.2f;
    check(group && group->enabled && !group->checked && group->text == expected_title && heading_fits && hidden_children(),
        "About starts with one readable collapsed license heading and a count derived from the bundled document list");
    {
        Accessibility access(host); ComPtr<IRawElementProviderFragment> root; root.Attach(access.root_for_testing());
        const auto node = find_node(root.Get(), L"about.licenses.disclosure");
        check(node && property(node.Get(), UIA_NamePropertyId) == expected_title + L" · " + T("disclosure.collapsed") &&
            !find_node(root.Get(), win::widen(std::string("about.license.") + license_documents().front().id)),
            "collapsed license groups expose their state to UIA without exposing hidden component controls");
        access.disconnect();
    }
    check(invoke_r11_action(host, L"about.licenses.disclosure"), "normal and read-only users can expand the license component list");
    build();
    std::size_t component_count = 0;
    for (const auto& document : license_documents()) {
        const auto* item = host.scene.find(std::string("about.license.") + document.id);
        component_count += item && item->text == document.title && item->enabled && !item->checked;
    }
    check(ui.licenses_expanded && component_count == license_documents().size(),
        "the expanded group displays every bundled component once with each complete license independently collapsed");
    const auto& first = license_documents().front();
    const std::string first_id = first.id;
    check(invoke_r11_action(host, win::widen("about.license." + first_id)), "native UIA expands a component inside the license group");
    build();
    const auto* text = host.scene.find("about.license.text." + first_id);
    const auto* copy = host.scene.find("about.license.copy." + first_id);
    check(text && text->text == win::widen(first.text) && copy && copy->enabled && copy->focusable && copy->action,
        "the expanded component retains its original full license and copy control in normal and read-only modes");
    ui.copied = "about.license.copy." + first_id;
    build(); copy = host.scene.find(ui.copied);
    check(copy && copy->text == T("action.copied") && ui.licenses_expanded && ui.expanded_licenses.size() == 1,
        "copy feedback remains local to its component without changing group or individual expansion state");
    ui.overlays.push_back("accountDelete"); build();
    check(invoke_r11_action(host, L"sheet.close") && ui.overlays == std::vector<std::string>{"about"} &&
        ui.licenses_expanded && ui.expanded_licenses.size() == 1 && ui.account_feedback == "fixture-account-feedback",
        "closing a nested account dialog preserves the About license group and its independent component state");
    build(); check(invoke_r11_action(host, L"about.licenses.disclosure"), "the expanded license group can be collapsed as a whole");
    build();
    check(!ui.licenses_expanded && hidden_children() && ui.expanded_licenses == std::vector<std::string>{first_id},
        "collapsing the group hides every component body and copy action while retaining the user's in-session component choice");
    invoke_r11_action(host, L"about.licenses.disclosure"); build();
    check(host.scene.find("about.license.text." + first_id) && ui.expanded_licenses.size() == 1,
        "reopening the same license group restores its component state without expanding unrelated licenses");
    host.scene.layers[1].scroll = host.scene.layers[1].content_height; host.scene.clamp_scroll(1);
    const auto* close = host.scene.find("sheet.close");
    check(close && !close->scrolls && host.scene.visible_rect(*close).top == close->rect.top &&
        invoke_r11_action(host, L"sheet.close") && ui.overlays.empty(),
        "the fixed About close control remains available after a long license has been read");
    build(); check(invoke_r11_action(host, L"settings.about"), "settings reopens About through its normal action");
    build();
    check(!ui.licenses_expanded && ui.expanded_licenses.empty() && ui.copied.empty() && hidden_children() &&
        ui.help_quick_start_expanded && ui.account_feedback == "fixture-account-feedback",
        "each new About presentation resets only its license and copy state while retaining help and account presentation state");
    const auto* email = host.scene.find("about.feedback.email");
    check(email && email->text == win::widen(szu::links::kFeedbackEmail) && host.scene.find("about.feedback.copy") &&
        !host.scene.find("about.feedback.example.body"), "About offers the public feedback address while its mail example starts collapsed");
    check(invoke_r11_action(host, L"about.feedback.example"), "the mail example is reachable through UI Automation");
    build();
    const auto* example = host.scene.find("about.feedback.example.body");
    check(example && example->text == T("about.feedback.example") &&
        example->text.find(L"【深大随连】") != std::wstring::npos,
        "the optional example supplies a natural project message without reading private data");
    check(invoke_r11_action(host, L"about.feedback.example"), "the mail example can be collapsed independently");
    build();
    check(!host.scene.find("about.feedback.example.body") && !ui.licenses_expanded && hidden_children(),
        "mail example disclosure does not expand the license list");
    const auto after = app.snapshot();
    check(fake->files == files && app.next_check() == deadline && after.epoch == before.epoch &&
        after.remembered == before.remembered && after.trusted == before.trusted && after.holds.size() == before.holds.size() &&
        fake->probes == 0 && fake->discoveries == 0 && fake->evidence().fake_auth_submissions == 0 &&
        fake->evidence().fake_wifi_calls == 0 && fake->evidence().network_calls == 0 &&
        fake->evidence().vault_accesses == 0 && fake->evidence().production_storage_accesses == 0,
        "license disclosures preserve data, trust, protections and monitoring without network requests, authentication or real clipboard use");
    app.shutdown();
}
