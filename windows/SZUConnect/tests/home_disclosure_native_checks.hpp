#pragma once
#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

class HomeDisclosureFake final : public campusverificationruntime::Fake {
public:
    explicit HomeDisclosureFake(bool with_websites = true) : campusverificationruntime::Fake("manual") {
        if (with_websites) {
            WebsitePreferences preferences{{{"first", "https://first.example/", true},
                {"second", "https://second.example/", false}, {"third", "https://third.example/", true}}, true};
            files[L"websites.json"] = *preferences.encode();
        }
    }
    win::CredentialVault::ReadStatus read_credentials(Credentials& value) override {
        ++credential_reads;
        if (!saved) return win::CredentialVault::ReadStatus::missing;
        value = *saved; return win::CredentialVault::ReadStatus::found;
    }
    bool write_credentials(const Credentials& value) override { saved = value; return true; }
    bool erase_credentials() override { saved.reset(); return true; }
    std::optional<Credentials> saved{Credentials{"fixture-student-1234", "fixture-password-1234"}};
};

void verify_website_presets(FixtureHost& host, Canvas& canvas, ViewContext& ctx, HomeDisclosureFake& fake, float width, float height) {
    auto& app = ctx.app;
    const auto build = [&] { build_scene(host.scene, canvas, ctx, width, height); };
    const auto initial_files = fake.files;
    const auto initial_state = app.snapshot();
    const auto initial_deadline = app.next_check();
    check(!host.scene.find("internet.preset.baidu") && !host.scene.find("internet.preset.tencent") &&
        app.website_targets().empty() && fake.probes == 0 && fake.system_probes == 0 && fake.discoveries == 0,
        "website presets are absent from the collapsed group and create no saved targets or requests by default");
    check(invoke_r11_action(host, L"home.websites.disclosure"), "native UIA expands optional website presets"); build();
    std::vector<std::function<void()>> actions;
    for (const auto& preset : links::kWebsitePresets) {
        const auto id = "internet.preset." + std::string(preset.key);
        const auto* button = host.scene.find(id);
        const auto facts = button ? canvas.inspect_text_layout_for_testing(button->text, TextStyle::body_strong,
            button->rect.right - button->rect.left) : TextLayoutFacts{};
        check(button && button->action && button->enabled == !app.read_only() && button->role == Role::button &&
            button->rect.left >= 0 && button->rect.right <= width && button->rect.bottom - button->rect.top >= tokens::kSizeMinTarget - 0.1f &&
            facts.lines > 0 && facts.lines <= 2, "both preset add actions are accessible and fit narrow windows, DPI and large text");
        actions.push_back(button ? button->action : std::function<void()>{});
    }
    const auto* detail = host.scene.find("internet.presets.detail");
    check(detail && detail->text == T("internet.presets.detail") && fake.files == initial_files &&
        app.snapshot().epoch == initial_state.epoch && app.next_check() == initial_deadline &&
        fake.probes == 0 && fake.system_probes == 0 && fake.discoveries == 0,
        "expanding shortcuts only explains that an added website must be selected for a full check");
    ctx.ui.internet_url = L"https://unfinished.example/"; ctx.ui.clear_internet_input = false;
    if (app.read_only()) {
        check(!invoke_r11_action(host, L"internet.preset.baidu"), "read-only preset additions are disabled in UIA");
        for (const auto& action : actions) if (action) action();
        check(fake.files == initial_files && app.website_targets().empty() && ctx.ui.internet_url == L"https://unfinished.example/" &&
            !ctx.ui.clear_internet_input && app.settings().probe == ProbeMode::quick,
            "stale direct preset callbacks also preserve read-only data, input draft and probe selection");
        return;
    }
    fake.fail_write = true;
    check(invoke_r11_action(host, L"internet.preset.baidu"), "a native preset action reaches an isolated persistence failure"); build();
    check(app.website_targets().empty() && fake.files == initial_files && ctx.ui.internet_error == "internet.settingsWriteFailed" &&
        ctx.ui.internet_url == L"https://unfinished.example/" && !ctx.ui.clear_internet_input && app.settings().probe == ProbeMode::quick,
        "a failed shortcut save keeps the original list and unfinished manual URL without selecting full checks");
    fake.fail_write = false;
    fake.block = true;
    const bool verifying = app.verify_campus_service();
    wifiruntime::wait_for([&] { return fake.discoveries > 0; });
    const auto busy_files = fake.files;
    check(verifying && app.snapshot().busy, "the isolated busy guard fixture holds a credential-free service check");
    if (actions.front()) actions.front()();
    check(app.website_targets().empty() && fake.files == busy_files && ctx.ui.internet_error == "internet.settingsBusy",
        "a preset callback captured before a check cannot save while that check is busy");
    fake.released = true;
    wifiruntime::wait_for([&] { app.on_changed(); return !app.snapshot().busy; });
    fake.block = false;
    app.check();
    wifiruntime::wait_for([&] { app.on_changed(); return !app.snapshot().busy && app.internet_report() && app.last_checked() && app.system_connectivity(); });
    const auto checked = app.last_checked(), system_checked = app.system_checked_at();
    const auto previous_report = app.internet_report();
    const auto before = app.snapshot();
    const auto requests = fake.probes + fake.system_probes + fake.discoveries;
    const auto deadline = app.next_check();
    check(previous_report && previous_report->base.mode == ProbeMode::quick, "the shortcut fixture retains a completed quick result before editing websites");
    for (const auto& preset : links::kWebsitePresets) {
        build();
        check(invoke_r11_action(host, win::widen("internet.preset." + std::string(preset.key))),
            "native UIA adds the shared preset through the same website validation and persistence path as manual input"); build();
        const auto stored = WebsitePreferences::decode(fake.files[L"websites.json"]);
        const auto found = std::find_if(app.website_targets().begin(), app.website_targets().end(), [&](const WebsiteTarget& site) { return site.url == preset.url; });
        const auto* button = host.scene.find("internet.preset." + std::string(preset.key));
        check(stored && found != app.website_targets().end() && !found->id.empty() && !found->enabled && button && !button->enabled &&
            button->text == T(preset.key == "baidu" ? "internet.presets.addedBaidu" : "internet.presets.addedTencent") &&
            ctx.ui.internet_url == L"https://unfinished.example/" && !ctx.ui.clear_internet_input && app.settings().probe == ProbeMode::quick &&
            app.last_checked() == checked && app.system_checked_at() == system_checked && app.next_check() == deadline && app.internet_report() &&
            app.internet_report()->base.mode == previous_report->base.mode && app.internet_report()->selected_count == previous_report->selected_count &&
            app.internet_report()->websites.size() == previous_report->websites.size() && app.snapshot().last_report && before.last_report &&
            app.snapshot().last_report->mode == before.last_report->mode &&
            app.snapshot().last_report->apple.state == before.last_report->apple.state &&
            app.snapshot().last_report->microsoft.state == before.last_report->microsoft.state,
            "a preset is saved disabled, preserves the draft and previous result/times, and does not silently change the check scope");
    }
    const auto duplicate_files = fake.files;
    if (actions.front()) actions.front()();
    check(app.website_targets().size() == 2 && fake.files == duplicate_files && ctx.ui.internet_error == "internet.duplicateOrLimit",
        "a repeated stale shortcut callback cannot add a duplicate URL");
    build();
    const auto target_id = app.website_targets().front().id;
    check(invoke_r11_action(host, win::widen("internet.enabled." + target_id)) && app.website_targets().front().enabled &&
        app.settings().probe == ProbeMode::complete, "only explicitly selecting an added preset opts it into the next full check");
    std::vector<WebsiteTarget> full;
    for (std::size_t i = 0; i < WebsiteTarget::maximum_count; ++i)
        full.push_back({"capacity-" + std::to_string(i), "https://capacity" + std::to_string(i) + ".example/", false});
    check(app.replace_website_targets(full, false), "the isolated capacity fixture contains twelve other optional websites"); build();
    const auto full_files = fake.files;
    check(host.scene.find("internet.preset.baidu") && !host.scene.find("internet.preset.baidu")->enabled &&
        host.scene.find("internet.preset.tencent") && !host.scene.find("internet.preset.tencent")->enabled && !host.scene.find("internet.add"),
        "preset additions and the manual editor respect the same twelve-website limit");
    for (const auto& action : actions) if (action) action();
    check(app.website_targets().size() == WebsiteTarget::maximum_count && fake.files == full_files &&
        ctx.ui.internet_error == "internet.duplicateOrLimit" && ctx.ui.internet_url == L"https://unfinished.example/",
        "late preset actions at capacity preserve all saved rows and the manual draft");
    const auto after = app.snapshot();
    check(fake.probes + fake.system_probes + fake.discoveries == requests && fake.evidence().fake_auth_submissions == 0 &&
        fake.evidence().fake_wifi_calls == 0 && fake.evidence().network_calls == 0 && app.next_check() == deadline &&
        after.epoch == before.epoch && after.trusted == before.trusted && after.remembered == before.remembered &&
        after.paused == before.paused && after.holds.size() == before.holds.size(),
        "preset editing and selection never send requests, authenticate, grant trust or disturb recovery protections and timing");
}

void verify_website_add_capacity(HWND window, Canvas& canvas, float width, float height) {
    for (const bool legacy_ids : {false, true}) {
        auto services = std::make_unique<HomeDisclosureFake>(false); auto* fake = services.get();
        WebsitePreferences initial;
        const char* legacy[]{"legacy-profile-a", "website-6", "website-12", "学校网址"};
        for (std::size_t index = 0; index < 4; ++index)
            initial.targets.push_back({legacy_ids ? legacy[index] : "website-" + std::to_string(index + 1),
                "https://site" + std::to_string(index + 1) + ".example/", index != 1});
        fake->files[L"websites.json"] = *initial.encode();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto deadline = app.next_check();
        FixtureHost host; host.window = window; host.scale = canvas.dpi() / 96;
        UiState ui; ui.websites_expanded = true;
        ViewContext context{app, ui, window, [] {}, [](const std::string&) {}, [] {}, [](const std::string&) {}};
        const auto build = [&] { build_scene(host.scene, canvas, context, width, height); };
        ui.internet_url = L"https://site5.example/"; build();
        const auto before_failure = fake->files;
        fake->fail_write = true;
        check(invoke_r11_action(host, L"internet.add"), "native add-website action reaches an isolated write failure");
        build();
        const auto feedback_count = std::count_if(host.scene.widgets.begin(), host.scene.widgets.end(), [](const Widget& widget) {
            return widget.kind == Kind::text && widget.text == T("internet.settingsWriteFailed");
        });
        check(app.website_targets().size() == 4 && fake->files == before_failure && ui.internet_url == L"https://site5.example/" &&
            ui.internet_error == "internet.settingsWriteFailed" && feedback_count == 1,
            "a failed website save preserves every existing row and the unfinished URL and displays one explicit retryable error");
        fake->fail_write = false;
        std::function<void()> last_add;
        for (std::size_t count = 5; count <= WebsiteTarget::maximum_count; ++count) {
            ui.internet_url = L"https://site" + std::to_wstring(count) + L".example/"; ui.clear_internet_input = false; build();
            const auto* add = host.scene.find("internet.add");
            if (add) last_add = add->action;
            check(invoke_r11_action(host, L"internet.add"), "native UIA adds each optional website through the actual home button");
            build();
            const auto& targets = app.website_targets();
            std::set<std::string> ids;
            bool preserved = true;
            for (const auto& item : targets) ids.insert(item.id);
            for (const auto& original : initial.targets)
                preserved = preserved && std::any_of(targets.begin(), targets.end(), [&](const WebsiteTarget& item) {
                    return item.id == original.id && item.url == original.url && item.enabled == original.enabled;
                });
            const auto stored = WebsitePreferences::decode(fake->files[L"websites.json"]);
            check(targets.size() == count && stored && stored->targets.size() == count && ids.size() == count &&
                !ids.count("") && preserved && ui.internet_url.empty() && ui.internet_error.empty() &&
                (legacy_ids || targets.back().id == "website-" + std::to_string(count)),
                "the fifth through twelfth websites receive unique nonempty IDs while existing arbitrary IDs, names and choices remain unchanged");
        }
        const auto full_files = fake->files;
        ui.internet_url = L"https://thirteenth.example/"; ui.clear_internet_input = false;
        check(last_add && !host.scene.find("internet.add") && !host.scene.find("internet.url"),
            "the add editor is removed exactly at the twelve-website limit");
        if (last_add) last_add();
        build();
        check(app.website_targets().size() == WebsiteTarget::maximum_count && fake->files == full_files &&
            ui.internet_url == L"https://thirteenth.example/" && !ui.clear_internet_input &&
            ui.internet_error == "internet.duplicateOrLimit" && app.next_check() == deadline &&
            fake->probes == 0 && fake->system_probes == 0 && fake->discoveries == 0 &&
            fake->evidence().fake_auth_submissions == 0,
            "a delayed add callback at capacity preserves all twelve rows and its draft without checking connectivity or changing the monitoring schedule");
        app.shutdown();
        check(fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 &&
            fake->evidence().production_storage_accesses == 0,
            "website capacity and save-failure tests use only isolated files and fake services");
    }
}

void verify_home_disclosures(HWND window, Canvas& canvas, float width, float height, RuntimeMode mode) {
    auto services = std::make_unique<HomeDisclosureFake>(); auto* fake = services.get();
    AppController app(nullptr, mode, std::move(services)); app.start();
    const auto files = fake->files; const auto saved = fake->saved; const auto deadline = app.next_check();
    const auto checked = app.last_checked(), system_checked = app.system_checked_at();
    const auto before = app.snapshot(); const int reads = fake->credential_reads;
    FixtureHost host; host.window = window; host.scale = canvas.dpi() / 96;
    UiState ui;
    ViewContext context{app, ui, window, [] {}, [&](const std::string& name) { ui.overlays.push_back(name); },
        [&] { if (!ui.overlays.empty()) ui.overlays.pop_back(); }, [](const std::string&) {}};
    const auto build = [&] { build_scene(host.scene, canvas, context, width, height); };
    const auto no_io = [&] {
        const auto state = app.snapshot();
        return fake->files == files && fake->saved == saved && fake->credential_reads == reads &&
            app.next_check() == deadline && app.last_checked() == checked && app.system_checked_at() == system_checked &&
            app.website_follow_system_proxy() && !app.internet_report() && !app.system_connectivity() &&
            state.epoch == before.epoch && state.trusted == before.trusted && state.memory_count == before.memory_count &&
            state.paused == before.paused && state.holds.size() == before.holds.size() &&
            fake->probes == 0 && fake->system_probes == 0 && fake->discoveries == 0 &&
            fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0;
    };
    const auto header_fits = [&](const char* prefix) {
        const auto* title = host.scene.find(prefix);
        const auto* disclosure = host.scene.find(std::string(prefix) + ".disclosure");
        const auto* summary = host.scene.find(std::string(prefix) + ".summary");
        const auto* info = host.scene.find(std::string(prefix) + ".info");
        bool fits = title && info && (!disclosure || disclosure->rect.right <= info->rect.left + 0.1f);
        for (const auto* widget : {title, disclosure, summary, info}) {
            if (!widget) continue;
            const auto& rect = widget->rect;
            fits = fits && rect.left >= 0 && rect.right <= width + 0.1f && rect.right > rect.left && rect.bottom > rect.top;
            if (widget->focusable) fits = fits && rect.right - rect.left >= tokens::kSizeMinTarget - 0.1f &&
                rect.bottom - rect.top >= tokens::kSizeMinTarget - 0.1f;
            if (widget->kind == Kind::text) fits = fits &&
                canvas.inspect_text_layout_for_testing(widget->text, widget->style, rect.right - rect.left).lines <= 2 &&
                canvas.measure(widget->text, widget->style, rect.right - rect.left).height <= rect.bottom - rect.top + 0.2f;
        }
        return fits;
    };
    build();
    const auto* websites = host.scene.find("home.websites.disclosure");
    const auto* summary = host.scene.find("home.websites.summary");
    check(websites && !websites->checked && summary && summary->text == F("internet.targets.summary", {{"total", "3"}, {"enabled", "2"}}) &&
        !host.scene.find("internet.url") && !host.scene.find("internet.add") && !host.scene.find("internet.strategy.0") &&
        !host.scene.find("internet.enabled.first") && header_fits("home.websites") && no_io(),
        "websites start collapsed with exact saved/selected counts and no hidden editors, strategy controls or I/O");
    Accessibility access(host);
    ComPtr<IRawElementProviderFragment> root; root.Attach(access.root_for_testing());
    const auto summary_node = find_node(root.Get(), L"home.websites.summary");
    check(summary_node && property(summary_node.Get(), UIA_NamePropertyId) == summary->text,
        "native UIA exposes the same concise website count summary as the painted collapsed card");
    access.disconnect();
    check(invoke_r11_action(host, L"home.websites.info") && !ui.websites_expanded &&
        ui.overlays == std::vector<std::string>{"info:websites"} && no_io(),
        "the separate information target opens its explanation without toggling the adjacent collapsed card");
    ui.overlays.clear(); build();
    check(invoke_r11_action(host, L"home.websites.disclosure"), "native UIA opens website settings from the card header");
    build();
    check(ui.websites_expanded && host.scene.find("internet.url") && host.scene.find("internet.add") &&
        host.scene.find("internet.strategy.0") && host.scene.find("internet.strategy.1") && host.scene.find("internet.enabled.second") &&
        !host.scene.find("home.websites.summary") && no_io(),
        "expanded websites keep their editable list and detection strategy together without starting a check or changing settings");
    ui.internet_url = L"https://draft.example/";
    check(invoke_r11_action(host, L"home.websites.disclosure"), "native UIA closes website settings independently of connection details");
    build();
    check(!ui.websites_expanded && ui.internet_url == L"https://draft.example/" && !host.scene.find("internet.url") &&
        !host.scene.find("internet.strategy.0") && no_io(),
        "collapsing websites retains an unfinished address and previous result/times while hiding all group controls");
    ui.tutorial_step = 7; focus_tutorial(context); build();
    check(ui.websites_expanded && host.scene.find("internet.url") && no_io(),
        "the website tutorial explicitly opens its inline settings before positioning the guide");
    ui.websites_expanded = false; ui.home_focus = "home.websites"; build();
    check(ui.websites_expanded && host.scene.find("internet.url") && ui.home_focus.empty() && no_io(),
        "legacy website navigation expands the destination before resolving its scroll target");
    ui.websites_expanded = false;

    if (mode == RuntimeMode::normal) {
        build();
        const auto* account = host.scene.find("home.account.disclosure");
        const auto* account_summary = host.scene.find("home.account.summary");
        bool private_summary = true;
        for (const auto& widget : host.scene.widgets)
            private_summary = private_summary && widget.text.find(L"fixture-student-1234") == std::wstring::npos &&
                widget.name.find(L"fixture-student-1234") == std::wstring::npos &&
                widget.text.find(L"fixture-password-1234") == std::wstring::npos &&
                widget.name.find(L"fixture-password-1234") == std::wstring::npos;
        check(account && !account->checked && account_summary &&
            account_summary->text == F("account.saved", {{"account", before.saved_account_hint}}) &&
            !host.scene.find("account.account") && !host.scene.find("account.password") && !host.scene.find("guard.area") &&
            private_summary && header_fits("home.account") && no_io(),
            "saved accounts start collapsed and expose only the masked account summary, with no credential fields or unmasked accessible text");
        check(invoke_r11_action(host, L"home.account.disclosure"), "native UIA expands the saved account editor");
        build();
        check(ui.account_expanded && host.scene.find("account.account") && host.scene.find("account.password") &&
            host.scene.find("guard.area") && !host.scene.find("home.account.summary") && no_io(),
            "opening the saved account reveals the existing editor without rereading the credential store or changing it");
        ui.show_password = true;
        check(invoke_r11_action(host, L"home.account.disclosure"), "native UIA collapses the saved account editor");
        build();
        check(!ui.account_expanded && !ui.show_password && !host.scene.find("account.password") && no_io(),
            "collapsing accounts hides credential fields and resets password visibility without deleting or saving credentials");
        app.set_draft("fixture-edited-5678", "fixture-edited-password"); build();
        check(!ui.account_expanded && app.snapshot().unsaved_changes && host.scene.find("account.account") &&
            host.scene.find("account.password") && host.scene.find("account.save") && !host.scene.find("home.account.disclosure") && no_io(),
            "unsaved credential edits force the form open and cannot be concealed by the collapsed preference");
        app.discard_draft(); ui.tutorial_step = 2; focus_tutorial(context); build();
        check(ui.account_expanded && host.scene.find("account.account") && no_io(),
            "the account tutorial opens the editor even when saved credentials would normally collapse it");
        ui.account_expanded = false; ui.home_focus = "home.account"; build();
        check(ui.account_expanded && host.scene.find("account.account") && ui.home_focus.empty() && no_io(),
            "account navigation expands the editor before resolving its inline scroll target");
        check(app.delete_account(), "isolated account deletion succeeds for disclosure state regression");
        ui.account_expanded = false; build();
        check(!app.snapshot().has_saved && host.scene.find("account.account") && host.scene.find("account.password") &&
            !host.scene.find("home.account.disclosure"), "deleting a saved account automatically reopens the empty editor");
    } else {
        build();
        check(!app.snapshot().has_saved && host.scene.find("account.account") && host.scene.find("account.password") &&
            fake->credential_reads == 0 && no_io(),
            "read-only account presentation does not inspect formal credentials and keeps the isolated empty editor available");
    }
    app.shutdown();
    check(fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 &&
        fake->evidence().production_storage_accesses == 0,
        "account and website disclosure regressions use only isolated data and fake credential storage");

    auto empty_services = std::make_unique<HomeDisclosureFake>(false);
    auto* empty_fake = empty_services.get();
    AppController empty(nullptr, mode, std::move(empty_services)); empty.start();
    UiState empty_ui; FixtureHost empty_host; empty_host.window = window; empty_host.scale = canvas.dpi() / 96;
    ViewContext empty_context{empty, empty_ui, window, [] {}, [](const std::string&) {}, [] {}, [](const std::string&) {}};
    build_scene(empty_host.scene, canvas, empty_context, width, height);
    const auto* empty_summary = empty_host.scene.find("home.websites.summary");
    check(empty_summary && empty_summary->text == T("internet.targets.empty") && !empty_host.scene.find("internet.add"),
        "an empty website list stays compact and describes adding sites as optional rather than incomplete setup");
    verify_website_presets(empty_host, canvas, empty_context, *empty_fake, width, height);
    empty.shutdown();
    if (mode == RuntimeMode::normal) verify_website_add_capacity(window, canvas, width, height);
}
