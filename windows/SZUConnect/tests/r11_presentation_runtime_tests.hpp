#pragma once
#include "campus_verification_runtime_tests.hpp"

namespace r11presentationruntime {
class Fake : public campusverificationruntime::Fake {
public:
    explicit Fake(const std::string& mode = "manual", szu::CampusArea area = szu::CampusArea::automatic,
                  bool dormitory_memory = false) : campusverificationruntime::Fake(mode) {
        files[L"settings.json"] = "{\"formatVersion\":1,\"monitoringMode\":\"" + mode +
            "\",\"area\":\"" + std::string(szu::to_string(area)) +
            "\",\"probeMode\":\"quick\",\"appearanceMode\":\"system\"}";
        if (dormitory_memory) {
            szu::NetworkMemory memory("00112233445566778899aabbccddeeff");
            memory.remember(route().descriptor, "dormitory", true, true, szu::app::AppController::wall_now(), id);
            files[L"network-memory.json"] = memory.encode();
        }
    }
    szu::ConnectivityReport probe(szu::ProbeMode mode, const szu::CancelToken&) override {
        ++probes; auto result = szu::ConnectivityReport::simple(probe_online, probe_online); result.mode = mode;
        if (dormitory_hint && !probe_online) {
            result.apple.state = result.microsoft.state = szu::EndpointState::redirect;
            result.apple.campus_area_hint = result.microsoft.campus_area_hint = szu::CampusPortalHint::dormitory;
        }
        return result;
    }
    szu::CampusDiscovery discover(szu::CampusArea, const szu::CancelToken&) override {
        ++discoveries;
        while (block && !released) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++returned;
        return {teaching_confirmed ? szu::Discovery::confirmed : szu::Discovery::unconfirmed,
                dormitory_confirmed ? szu::Discovery::confirmed : szu::Discovery::unconfirmed, client_bound};
    }
    szu::LoginAttempt attempt(const szu::Credentials&, szu::CampusArea, const szu::CancelToken&) override {
        ++evidence().fake_auth_submissions;
        szu::LoginAttempt result; result.outcome = szu::Outcome::unavailable;
        result.credential_submission_started = false; return result;
    }
    std::atomic<bool> probe_online{true}, dormitory_hint{false};
    std::atomic<bool> teaching_confirmed{true}, dormitory_confirmed{true}, client_bound{false};
};
class HintRefreshFake final : public Fake {
public:
    HintRefreshFake() : Fake("recover") {
        szu::WebsitePreferences preferences;
        preferences.targets = {{"hint-fixture-site", "https://selection-fixture.invalid/", true}};
        files[L"websites.json"] = *preferences.encode();
        constrained = true;
        dormitory_confirmed = false;
    }
    szu::ConnectivityReport probe(szu::ProbeMode mode, const szu::CancelToken&) override {
        ++probes;
        auto report = szu::ConnectivityReport::simple(true, !old_hint); report.mode = mode;
        report.apple.elapsed = 0.125; report.microsoft.elapsed = 0.25;
        if (old_hint) { report.microsoft.state = szu::EndpointState::redirect; report.microsoft.campus_area_hint = szu::CampusPortalHint::dormitory; }
        return report;
    }
    szu::ConnectivityReport probe_verification_hints(const szu::CancelToken& token) override {
        ++hint_calls;
        while (block_hints && !release_hints) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++hint_returns;
        return probe(szu::ProbeMode::quick, token);
    }
    szu::WebsiteCheckResult check_website(const std::string&, bool, const szu::CancelToken&) override {
        ++website_checks; return {"accessible", 204, 0.75};
    }
    std::atomic<bool> old_hint{true}, block_hints{false}, release_hints{false};
    std::atomic<int> hint_calls{0}, hint_returns{0}, website_checks{0};
};
inline void drain(szu::app::AppController& app, const std::function<bool()>& done) {
    wifiruntime::wait_for([&] { app.on_changed(); return done(); });
    app.on_changed();
}
}

template<class Check> void run_r11_presentation_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    for (const bool confirmed : {false, true}) {
        auto services = std::make_unique<r11presentationruntime::Fake>(); auto* fake = services.get();
        fake->teaching_confirmed = fake->dormitory_confirmed = confirmed;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(app.verify_campus_service(), "area-scoped service feedback fixture admits a read-only campus check");
        r11presentationruntime::drain(app, [&] { return !app.snapshot().busy && fake->returned == 1 &&
            app.memory_verification_message() == (confirmed ? "campus.verify.confirmed" : "campus.verify.failed"); });
        check(app.memory_verification_message() == (confirmed ? "campus.verify.confirmed" : "campus.verify.failed"),
            "area-scoped feedback is visibly completed before its area changes");
        const auto memory = fake->files[L"network-memory.json"];
        app.check();
        r11presentationruntime::drain(app, [&] { return fake->probes == 1 && !app.snapshot().busy && app.last_checked().has_value(); });
        check(fake->discoveries == 1 && app.memory_verification_message() == (confirmed ? "campus.verify.confirmed" : "campus.verify.failed"),
            "a connectivity-only check preserves service feedback until a new actual campus discovery starts");
        app.set_area(CampusArea::dormitory);
        check(app.memory_verification_message().empty(), "changing area immediately removes old service success or failure");
        app.set_area(CampusArea::automatic);
        check(app.memory_verification_message().empty() && fake->files[L"network-memory.json"] == memory &&
            fake->evidence().fake_auth_submissions == 0,
            "returning to the old area cannot revive its feedback or alter remembered authority");
        app.shutdown();
    }
    for (const bool confirmed : {false, true}) {
        auto services = std::make_unique<r11presentationruntime::Fake>(); auto* fake = services.get();
        fake->teaching_confirmed = fake->dormitory_confirmed = confirmed;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        app.verify_campus_service();
        r11presentationruntime::drain(app, [&] { return !app.snapshot().busy && fake->returned == 1 &&
            app.memory_verification_message() == (confirmed ? "campus.verify.confirmed" : "campus.verify.failed"); });
        check(app.memory_verification_message() == (confirmed ? "campus.verify.confirmed" : "campus.verify.failed"),
            "service-only feedback is visibly completed before another actual discovery starts");
        const auto memory = fake->files[L"network-memory.json"];
        fake->probe_online = false; fake->block = true; fake->released = false;
        app.connect();
        wifiruntime::wait_for([&] { app.on_changed(); return fake->discoveries == 2; });
        check(fake->discoveries == 2 && app.memory_verification_message().empty(),
            "a new actual discovery invalidates old service-only feedback before it completes");
        app.on_changed();
        check(app.memory_verification_message().empty(), "processing a discovery revision cannot restore stale service feedback");
        app.cancel(); fake->released = true;
        r11presentationruntime::drain(app, [&] { return !app.snapshot().busy && fake->returned == 2; });
        check(fake->files[L"network-memory.json"] == memory && fake->evidence().fake_auth_submissions == 0,
            "cancelling the new discovery rejects its late proof and performs no credential submission");
        app.shutdown();
    }
    {
        auto services = std::make_unique<r11presentationruntime::Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto memory = fake->files[L"network-memory.json"];
        fake->fail_write = true;
        check(app.remember_network(), "memory-write failure fixture starts a fresh verification");
        r11presentationruntime::drain(app, [&] { return !app.snapshot().busy && fake->returned == 1 &&
            app.memory_verification_message() == "memory.verification.writeFailed"; });
        check(app.memory_verification_message() == "memory.verification.writeFailed" &&
            fake->files[L"network-memory.json"] == memory, "failed permanent save retains an explicit inline reason and original bytes");
        fake->fail_write = false; fake->probe_online = false; fake->block = true; fake->released = false;
        app.connect(); wifiruntime::wait_for([&] { app.on_changed(); return fake->discoveries == 2; }); app.on_changed();
        check(fake->discoveries == 2 && app.memory_verification_message() == "memory.verification.writeFailed",
            "ordinary new discovery does not silently erase a permanent-memory write failure");
        app.cancel(); fake->released = true;
        r11presentationruntime::drain(app, [&] { return !app.snapshot().busy && fake->returned == 2; });
        check(fake->files[L"network-memory.json"] == memory && fake->evidence().fake_auth_submissions == 0,
            "failed-save feedback lifecycle never changes memory bytes or sends credentials");
        app.shutdown();
    }
    {
        auto services = std::make_unique<r11presentationruntime::Fake>(); auto* fake = services.get(); fake->block = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        app.verify_campus_service(); wifiruntime::wait_for([&] { return fake->discoveries == 1; });
        app.set_area(CampusArea::dormitory); app.set_area(CampusArea::automatic);
        fake->released = true;
        r11presentationruntime::drain(app, [&] { return !app.snapshot().busy && fake->returned == 1; });
        check(app.memory_verification_message().empty() && fake->evidence().fake_auth_submissions == 0,
            "an area change discards an in-flight service result even after switching back");
        app.shutdown();
    }
    for (const bool cancelled : {false, true}) {
        auto services = std::make_unique<r11presentationruntime::HintRefreshFake>(); auto* fake = services.get();
        fake->old_hint = false;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.on_timer(kTimerAuto);
        wifiruntime::wait_for([&] { return fake->probes == 1 && !app.snapshot().busy; });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        r11presentationruntime::drain(app, [&] { return fake->probes == 1 && !app.snapshot().busy && app.next_check() &&
            *app.next_check() > AppController::wall_now() + 110; });
        fake->old_hint = true; app.set_probe(ProbeMode::complete); app.check();
        r11presentationruntime::drain(app, [&] { return fake->probes == 2 && fake->website_checks == 1 &&
            !app.snapshot().busy && app.last_checked() && app.next_check() && app.system_checked_at(); });
        const auto checked = app.last_checked(), system_checked = app.system_checked_at(), deadline = app.next_check();
        const auto published = app.internet_report(); const auto system = app.system_connectivity();
        check(published && published->base.mode == ProbeMode::complete && published->websites.size() == 1 &&
            deadline && *deadline > AppController::wall_now() + 110,
            "fresh-hint runtime fixture retains a complete partial report, site cache and existing long monitoring deadline");
        fake->constrained = false; app.refresh_environment();
        const auto files = fake->files;
        const int prior_discoveries = fake->discoveries, prior_sites = fake->website_checks;
        fake->old_hint = false; fake->block_hints = true;
        check(app.verify_campus_service(), "automatic service verification admits a private fixed-hint refresh");
        r11presentationruntime::drain(app, [&] { return fake->hint_calls == 1; });
        if (cancelled) app.cancel();
        fake->release_hints = true;
        r11presentationruntime::drain(app, [&] { return fake->hint_returns == 1 && !app.snapshot().busy && app.next_check() &&
            app.memory_verification_message() == (cancelled ? "campus.verify.cancelled" : "campus.verify.confirmed"); });
        const auto after = app.internet_report(); const auto after_system = app.system_connectivity();
        check(app.memory_verification_message() == (cancelled ? "campus.verify.cancelled" : "campus.verify.confirmed") &&
            app.last_checked() == checked && app.system_checked_at() == system_checked && app.next_check() && deadline && *app.next_check() >= *deadline,
            "private hint success or cancellation preserves both displayed check times and the original later deadline");
        check(published && after && system && after_system && after->base.mode == published->base.mode &&
            after->base.apple.state == published->base.apple.state && after->base.apple.elapsed == published->base.apple.elapsed &&
            after->base.microsoft.state == published->base.microsoft.state && after->base.microsoft.campus_area_hint == published->base.microsoft.campus_area_hint &&
            after->websites.size() == 1 && after->websites.front().result.state == published->websites.front().result.state &&
            after->websites.front().result.elapsed == published->websites.front().result.elapsed &&
            after_system->mode == system->mode && after_system->apple.state == system->apple.state && fake->website_checks == prior_sites,
            "private verification cannot replace either connectivity column or its retained custom website result");
        check(fake->files == files && fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0 &&
            fake->discoveries == prior_discoveries + (cancelled ? 0 : 1) && app.settings().area == CampusArea::automatic,
            "fresh-hint verification preserves settings, memory, labels and history and rejects discovery after cancellation");
        app.shutdown();
    }
}
