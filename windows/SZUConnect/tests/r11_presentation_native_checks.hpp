#pragma once
#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace {

void verify_r11_basic_probe_detail(Canvas& canvas, float width) {
    Scene scene;
    Builder builder(scene, canvas, 0, tokens::kSpaceXxl, width - 2 * tokens::kSpaceXxl, 24);
    builder.card_begin("surface", "border", tokens::kSpaceXl, tokens::kRadiusLarge);
    const auto detail = T("internet.basicPassed.detail");
    builder.status_header("check", "success", T("internet.basicPassed.title"), detail, L"检测说明", [] {});
    builder.card_end();
    const auto found = std::find_if(scene.widgets.begin(), scene.widgets.end(), [&detail](const Widget& item) {
        return item.kind == Kind::text && item.style == TextStyle::callout && item.text == detail;
    });
    ComPtr<IDWriteFactory> factory;
    const bool ready = SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(factory.GetAddressOf())));
    LineSnapshot measured;
    if (ready && found != scene.widgets.end()) {
        const auto native = text_layout_detail::create(factory.Get(), canvas.text_format_for_testing(TextStyle::callout),
            detail, found->rect.right - found->rect.left);
        measured = line_snapshot(native.Get());
    }
    check(detail == L"Apple 或 Microsoft 已有一项通过。" && measured.valid && !measured.lines.empty() &&
        (measured.lines.size() == 1 || measured.lines.back().length - measured.lines.back().trailingWhitespaceLength -
            measured.lines.back().newlineLength >= 3),
        "actual narrow hero keeps its basic-probe explanation free of a one-character orphan at native DPI and large text sizes");
}

bool invoke_r11_action(FixtureHost& host, const std::wstring& id) {
    Accessibility access(host);
    ComPtr<IRawElementProviderFragment> root;
    root.Attach(access.root_for_testing());
    const auto node = find_node(root.Get(), id);
    ComPtr<IInvokeProvider> invoke;
    if (node) node.As(&invoke);
    const bool result = invoke && SUCCEEDED(invoke->Invoke());
    access.disconnect();
    return result;
}

void verify_r11_memory_actions(HWND window, Canvas& canvas) {
    for (const bool save_fails : {false, true}) {
        auto services = std::make_unique<r11presentationruntime::Fake>("recover", CampusArea::dormitory);
        auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto original_memory = fake->files[L"network-memory.json"];
        const auto original_labels = fake->files[L"network-labels.json"];
        FixtureHost host; host.window = window;
        UiState ui;
        ViewContext context{app, ui, window, [] {}, [](const std::string&) {}, [] {}, [](const std::string&) {}};
        const auto guard = [&] { build_guard(host, canvas, context, 480, 2000); };
        const auto setup_incomplete = [&] {
            Scene page; build_scene(page, canvas, context, 500, 2000);
            return page.find("setup.expand") != nullptr;
        };
        const auto tutorial_status = [&] {
            Scene scene; Builder builder(scene, canvas, 0, 24, 432, 24);
            ui.tutorial_step = 5;
            const auto state = app.snapshot();
            build_tutorial_coach(builder, context, state, status::present(status::input_from(state)));
            const auto* item = scene.find("tutorial.coach.status");
            return item ? item->text : std::wstring();
        };
        guard(); auto state = app.snapshot();
        check(state.remembered && state.needs_network_memory_update && !state.recovery_setup_has_network &&
            host.scene.find("guard.remember") && host.scene.find("guard.once") && host.scene.find("guard.forget") &&
            !host.scene.find("campus.verify") && setup_incomplete() && tutorial_status() == T("guide.memory.waiting"),
            "teaching-only memory needing dormitory eligibility exposes save, temporary approval and forget without completing setup or guide");
        check(invoke_r11_action(host, L"guard.once"), "native UIA invokes the visible one-time approval control");
        state = app.snapshot(); guard();
        check(state.trusted && !state.recovery_setup_has_network && host.scene.find("guard.remember") &&
            host.scene.find("guard.revoke") && host.scene.find("guard.forget") && setup_incomplete() &&
            tutorial_status() == T("guide.memory.waiting") && fake->files[L"network-memory.json"] == original_memory,
            "one-time approval keeps permanent saving available and leaves old memory bytes and setup completion unchanged");
        fake->block = true; fake->fail_write = save_fails;
        check(invoke_r11_action(host, L"guard.remember"), "native UIA starts the displayed permanent-scope verification action");
        wifiruntime::wait_for([&] { return fake->discoveries == 1; }); guard();
        check(app.snapshot().verifying_memory && host.scene.find("guard.memory.cancel") &&
            !host.scene.find("guard.once") && !host.scene.find("guard.remember") &&
            status::campus_verification_key(app.snapshot()) == "env.campusVerification.checking",
            "running memory verification replaces authority actions with cancel and visibly reports checking");
        fake->released = true;
        r11presentationruntime::drain(app, [&] { return !app.snapshot().busy && fake->returned == 1 &&
            app.memory_verification_message() == (save_fails ? "memory.verification.writeFailed" : "memory.verification.remembered"); });
        guard(); state = app.snapshot();
        const auto* feedback = host.scene.find("guard.memory.feedback");
        if (save_fails) {
            check(state.trusted && state.remembered && state.needs_network_memory_update && !state.recovery_setup_has_network &&
                host.scene.find("guard.remember") && host.scene.find("guard.forget") && !host.scene.find("campus.verify") &&
                feedback && feedback->text == T("memory.verification.writeFailed") && setup_incomplete() &&
                tutorial_status() == T("guide.memory.waiting") && fake->files[L"network-memory.json"] == original_memory,
                "failed durable scope save keeps temporary approval and visible retry/forget while setup and guide remain incomplete");
        } else {
            check(state.remembered && !state.needs_network_memory_update && state.recovery_setup_has_network &&
                host.scene.find("campus.verify") && host.scene.find("guard.forget") && !host.scene.find("guard.remember") &&
                feedback && feedback->text == T("memory.verification.remembered") && !setup_incomplete() &&
                tutorial_status() == T("guide.memory.saved") && fake->files[L"network-memory.json"] != original_memory,
                "successful current-scope write alone completes setup/guide and restores normal verify/forget actions");
            const auto persisted = fake->files[L"network-memory.json"];
            fake->fail_write = true;
            check(invoke_r11_action(host, L"guard.forget"), "native UIA invokes forget through the normal remembered-network control");
            state = app.snapshot(); guard();
            check(!state.remembered && !state.trusted && !state.recovery_setup_has_network && state.memory_write_failed &&
                fake->files[L"network-memory.json"] == persisted && setup_incomplete() && app.notice() == "memory.writeFailed",
                "failed forget write still revokes all current recovery authority and reports the preserved on-disk record");
        }
        check(fake->files[L"network-labels.json"] == original_labels &&
            fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0,
            "all native memory action paths preserve the network name and use no authentication or Wi-Fi calls");
        app.shutdown();
    }
    {
        auto services = std::make_unique<r11presentationruntime::Fake>("recover", CampusArea::dormitory);
        auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services)); app.start();
        const auto files = fake->files;
        FixtureHost host; host.window = window; UiState ui;
        ViewContext context{app, ui, window, [] {}, [](const std::string&) {}, [] {}, [](const std::string&) {}};
        build_guard(host, canvas, context, 480, 1600);
        check(!host.scene.find("guard.once") && !host.scene.find("guard.remember") && !host.scene.find("guard.forget") &&
            !host.scene.find("campus.verify") && !app.trust() && !app.remember_network() && !app.verify_campus_service(),
            "read-only native guard and controller both reject all trust, memory and service-verification mutations");
        app.forget_network(); app.shutdown();
        check(fake->files == files && fake->discoveries == 0 && fake->evidence().fake_auth_submissions == 0,
            "read-only action attempts leave all injected files unchanged and perform no discovery or login");
    }
    for (const bool remembered : {true, false}) {
        auto services = std::make_unique<r11presentationruntime::Fake>("manual", CampusArea::automatic, true);
        auto* fake = services.get(); fake->probe_online = false;
        if (!remembered) fake->files[L"network-memory.json"] = NetworkMemory("00112233445566778899aabbccddeeff").encode();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.connect();
        r11presentationruntime::drain(app, [&] { return fake->returned == 1 && !app.snapshot().busy; });
        FixtureHost host; host.window = window; UiState ui;
        ViewContext context{app, ui, window, [] {}, [](const std::string&) {}, [] {}, [](const std::string&) {}};
        build_guard(host, canvas, context, 500, 1600);
        const auto state = app.snapshot(); const auto* explanation = host.scene.find("campus.entries.selection");
        if (remembered) {
            check(!state.needs_selection && state.required_campus_confirmed && !explanation &&
                !host.scene.find("guard.area") && fake->evidence().fake_auth_submissions == 1,
                "current dormitory memory selects the verified entry without asking for a fixed region before manual authentication");
        } else {
            check(state.needs_selection && !state.required_campus_confirmed && explanation &&
                host.scene.find("guard.area") && status::present(status::input_from(state)).primary == "check" &&
                status::present(status::input_from(state)).secondary == "area" &&
                !status::system_path_can_lead(state) && fake->evidence().fake_auth_submissions == 0,
                "an unknown connection with two reachable homepages still needs an entry and cannot claim verification or submit credentials");
        }
        app.shutdown();
    }
    {
        auto services = std::make_unique<r11presentationruntime::Fake>("recover", CampusArea::dormitory);
        auto* fake = services.get(); fake->files[L"network-memory.json"] = "{unsupported-memory";
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        FixtureHost host; host.window = window; UiState ui;
        ViewContext context{app, ui, window, [] {}, [](const std::string&) {}, [] {}, [](const std::string&) {}};
        build_guard(host, canvas, context, 480, 1600);
        const auto* remember = host.scene.find("guard.remember"); const auto* blocker = host.scene.find("guard.memory.blocker");
        check(remember && !remember->enabled && blocker && blocker->text == T("memory.unreadable") && blocker->role == Role::status,
            "an unavailable remember action exposes its actual readable data reason beside the disabled control");
        check(fake->files[L"network-memory.json"] == "{unsupported-memory" && fake->evidence().fake_auth_submissions == 0,
            "displaying the unavailable reason preserves unreadable original data and submits no credentials");
        app.shutdown();
    }
    {
        auto services = std::make_unique<r11presentationruntime::Fake>("recover", CampusArea::dormitory);
        auto* fake = services.get(); fake->dormitory_confirmed = false;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.remember_network();
        r11presentationruntime::drain(app, [&] { return fake->returned == 1 && !app.snapshot().busy; });
        FixtureHost host; host.window = window; UiState ui;
        ViewContext context{app, ui, window, [] {}, [](const std::string&) {}, [] {}, [](const std::string&) {}};
        build_guard(host, canvas, context, 500, 1600);
        const auto state = app.snapshot(); const auto* status = host.scene.find("status.campusVerification");
        check(state.last_discovery && state.last_discovery->teaching == Discovery::confirmed &&
            !state.required_campus_confirmed && status && status->text == T("env.campusVerification.unconfirmed") &&
            !state.recovery_setup_has_network && host.scene.find("guard.remember") && host.scene.find("guard.forget") &&
            fake->evidence().fake_auth_submissions == 0,
            "native label and actions require current dormitory qualification even when raw teaching HTTPS identity passed");
        app.shutdown();
    }
}
}  // namespace
