#pragma once
#include "wifi_runtime_tests.hpp"

namespace campusverificationruntime {
class Fake : public wifiruntime::Fake {
public:
    explicit Fake(const std::string& mode = "manual") {
        files[L"settings.json"] = "{\"formatVersion\":1,\"monitoringMode\":\"" + mode +
            "\",\"area\":\"automatic\",\"probeMode\":\"quick\",\"appearanceMode\":\"system\"}";
        szu::NetworkMemory memory("00112233445566778899aabbccddeeff");
        memory.remember(route().descriptor, "teaching", true, false, szu::app::AppController::wall_now(), id);
        files[L"network-memory.json"] = memory.encode();
        szu::NetworkLabels labels;
        labels.update_automatic(id, {"Fixture Ethernet", "ethernet", std::nullopt, false});
        labels.rename(id, "Fixture laboratory cable");
        files[L"network-labels.json"] = labels.encode();
    }
    szu::physical::Route route() const {
        szu::physical::Route result;
        result.interface_key = "fixture-campus-verification-ethernet"; result.interface_index = 21; result.interface_luid = 21;
        result.source_ipv4 = "192.0.2.7"; result.addresses = {result.source_ipv4}; result.dns_servers = {"192.0.2.53"};
        result.gateway_ipv4 = "192.0.2.1"; result.identity = changed ? "fixture-second-cable" : "fixture-first-cable";
        result.display_name = "Fixture Ethernet"; result.is_system_primary = true;
        result.descriptor = {"ethernet", {{"gw", changed ? "02:00:00:00:00:02" : "02:00:00:00:00:01"},
            {"gwip", "192.0.2.1"}, {"net", "192.0.2.0/24"}}};
        return result;
    }
    szu::win::NetworkSnapshot capture_network() override {
        auto physical = route();
        szu::win::NetworkSnapshot result; result.access = "ethernet"; result.tunnel_known = true;
        result.path.usable = result.path.link_active = true; result.path.requires_physical_binding = true;
        result.path.constrained = constrained;
        result.path.identity = physical.identity; result.path.descriptors = {physical.descriptor};
        result.path.physical_candidates = {physical}; return result;
    }
    bool set_expected_physical_route(const std::optional<szu::physical::Route>& attachment) override {
        return attachment && *attachment == route();
    }
    szu::win::CredentialVault::ReadStatus read_credentials(szu::Credentials& credentials) override {
        ++credential_reads; return wifiruntime::Fake::read_credentials(credentials);
    }
    szu::ConnectivityReport probe(szu::ProbeMode mode, const szu::CancelToken&) override {
        ++probes; auto report = szu::ConnectivityReport::simple(true, true); report.mode = mode; return report;
    }
    std::optional<szu::ConnectivityReport> system_connectivity(szu::ProbeMode mode, const szu::CancelToken&) override {
        ++system_probes; auto report = szu::ConnectivityReport::simple(true, true); report.mode = mode; return report;
    }
    szu::CampusDiscovery discover(szu::CampusArea, const szu::CancelToken&) override {
        ++discoveries;
        while (block && !released) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++returned;
        return {confirmed ? szu::Discovery::confirmed : szu::Discovery::unconfirmed,
                confirmed ? szu::Discovery::confirmed : szu::Discovery::unconfirmed};
    }
    std::atomic<int> probes{0}, system_probes{0}, discoveries{0}, returned{0}, credential_reads{0};
    std::atomic<bool> block{false}, released{false}, changed{false}, confirmed{true}, constrained{false};
};
class WiredRecoveryFake final : public Fake {
public:
    WiredRecoveryFake() : Fake("recover") {}
    szu::ConnectivityReport probe(szu::ProbeMode mode, const szu::CancelToken&) override {
        const int call = ++probes;
        auto result = szu::ConnectivityReport::simple(call == 1 || submitted, call == 1 || submitted); result.mode = mode;
        if (call > 1 && !submitted) {
            result.apple.state = result.microsoft.state = szu::EndpointState::redirect;
            result.apple.campus_area_hint = result.microsoft.campus_area_hint = szu::CampusPortalHint::teaching;
        }
        return result;
    }
    szu::LoginAttempt attempt(const szu::Credentials& credentials, szu::CampusArea area, const szu::CancelToken&) override {
        ++evidence().fake_auth_submissions;
        fake_credentials_only = !credentials.account.empty() && !credentials.password.empty();
        chosen = area; submitted = true;
        szu::LoginAttempt result; result.outcome = szu::Outcome::success; return result;
    }
    std::atomic<bool> submitted{false}, fake_credentials_only{false};
    szu::CampusArea chosen = szu::CampusArea::automatic;
};
}

template<class Check> void run_campus_verification_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    for (const auto& mode : {std::string("manual"), std::string("recover")}) {
        auto services = std::make_unique<campusverificationruntime::Fake>(mode); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        app.pause(); app.check();
        wifiruntime::wait_for([&] { app.on_changed(); return fake->probes > 0 && !app.snapshot().busy && app.last_checked() && app.system_connectivity(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); app.on_changed();
        const auto memory = fake->files[L"network-memory.json"], labels = fake->files[L"network-labels.json"];
        const auto checked_at = app.last_checked(), scheduled = app.next_check();
        const auto before = app.snapshot(); const int probes = fake->probes, system_probes = fake->system_probes;
        const int explicit_completion = fake->returned + 1;
        app.set_probe(ProbeMode::complete); app.set_probe(ProbeMode::quick);
        check(fake->probes == probes && fake->system_probes == system_probes && app.last_checked() == checked_at &&
            app.internet_report() && app.internet_report()->base.mode == ProbeMode::quick && app.system_connectivity(),
            "scope selection issues no request and preserves physical/system evidence and its original check time");
        check(app.verify_campus_service(), "remembered physical Ethernet admits credential-free service verification while login is paused");
        wifiruntime::wait_for([&] { app.on_changed(); return fake->returned == explicit_completion && !app.snapshot().busy &&
            app.memory_verification_message() == "campus.verify.confirmed"; });
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); app.on_changed();
        const auto after = app.snapshot();
        check(app.memory_verification_message() == "campus.verify.confirmed" && after.last_discovery &&
            after.last_discovery->teaching == Discovery::confirmed && after.last_discovery->dormitory == Discovery::confirmed,
            "dual-entry information is accepted only from this fresh service-only discovery");
        check(fake->files[L"network-memory.json"] == memory && fake->files[L"network-labels.json"] == labels &&
            app.network_name(fake->id) == "Fixture laboratory cable" && after.remembered == before.remembered &&
            after.trusted == before.trusted && after.paused == before.paused && after.holds.size() == before.holds.size() &&
            after.cooldown_remaining == before.cooldown_remaining,
            "service-only verification preserves scope, local remark, trust, protection holds and cooldown");
        check(fake->probes == probes && fake->system_probes == system_probes && app.last_checked() == checked_at &&
            fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0,
            "checking campus services neither logs in nor changes connectivity results nor starts optional Wi-Fi finding");
        check((mode == "manual" && !app.next_check()) || (mode == "recover" && scheduled && app.next_check() &&
            *app.next_check() >= *scheduled), "service-only completion neither creates monitoring nor advances an existing deadline");
        app.shutdown();
        check(fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 &&
            fake->evidence().production_storage_accesses == 0, "campus controller verification uses injected services and temporary data only");
    }
    for (int interruption : {0, 1, 2}) {
        auto services = std::make_unique<campusverificationruntime::Fake>(); auto* fake = services.get(); fake->block = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto memory = fake->files[L"network-memory.json"];
        check(app.verify_campus_service(), "late service-only fixture reserves its operation before queueing");
        wifiruntime::wait_for([&] { return fake->discoveries == 1; });
        if (interruption == 0) app.cancel();
        else if (interruption == 1) { fake->changed = true; app.refresh_environment(); }
        else app.on_suspend();
        fake->released = true;
        wifiruntime::wait_for([&] { app.on_changed(); return fake->returned == 1 && !app.snapshot().busy; });
        const auto state = app.snapshot();
        check(!state.last_discovery && app.memory_verification_message() != "campus.verify.confirmed" &&
            fake->files[L"network-memory.json"] == memory && fake->evidence().fake_auth_submissions == 0 &&
            !app.next_check(), "cancel, changed cable or suspend rejects late proof without memory changes, authentication or a new schedule");
        app.shutdown();
    }
    {
        auto services = std::make_unique<campusverificationruntime::Fake>("recover"); auto* fake = services.get();
        fake->constrained = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        app.on_timer(kTimerAuto);
        wifiruntime::wait_for([&] { return fake->probes == 1 && !app.snapshot().busy; });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        wifiruntime::wait_for([&] {
            app.on_changed();
            return fake->probes == 1 && !app.snapshot().busy && app.last_checked() &&
                app.next_check() && *app.next_check() > AppController::wall_now() + 110;
        });
        const auto scheduled = app.next_check(), checked_at = app.last_checked();
        const auto epoch = app.snapshot().epoch;
        check(scheduled && *scheduled > AppController::wall_now() + 110 && app.snapshot().online,
            "recover-mode cancellation fixture has an existing 120-second same-cable monitoring deadline");
        fake->constrained = false; app.refresh_environment();
        check(app.snapshot().epoch == epoch && app.next_check() == scheduled && app.snapshot().next_check_seconds == 30,
            "same-cable environment refresh preserves the old long deadline while the current policy permits a shorter interval");
        const auto memory = fake->files[L"network-memory.json"], labels = fake->files[L"network-labels.json"];
        const int probes = fake->probes, system_probes = fake->system_probes;
        const int explicit_discovery = fake->discoveries + 1, explicit_completion = fake->returned + 1;
        fake->block = true;
        check(app.verify_campus_service(), "recover-mode service verification temporarily reserves the existing monitoring schedule");
        wifiruntime::wait_for([&] { return fake->discoveries == explicit_discovery; });
        check(app.snapshot().verifying_campus && !app.next_check(),
            "service-only work suspends the timer while retaining its previous deadline for restoration");
        app.cancel(); fake->released = true;
        wifiruntime::wait_for([&] {
            app.on_changed();
            return fake->returned == explicit_completion && !app.snapshot().busy && app.next_check();
        });
        check(scheduled && app.next_check() && *app.next_check() >= *scheduled &&
            *app.next_check() > AppController::wall_now() + 100,
            "cancelling independent verification never shortens a longer pre-existing recover-mode deadline to the current 30-second cadence");
        check(!app.snapshot().last_discovery && !app.snapshot().verifying_campus && app.snapshot().epoch == epoch &&
            app.memory_verification_message() != "campus.verify.confirmed" &&
            fake->files[L"network-memory.json"] == memory && fake->files[L"network-labels.json"] == labels &&
            app.last_checked() == checked_at && fake->probes == probes && fake->system_probes == system_probes &&
            fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0,
            "cancelled recover-mode verification discards late proof without altering data, prior results, physical ownership or starting authentication");
        app.shutdown();
        check(fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 &&
            fake->evidence().production_storage_accesses == 0,
            "long-deadline cancellation exercises only injected services and temporary data");
    }
    {
        auto services = std::make_unique<campusverificationruntime::Fake>(); auto* fake = services.get();
        fake->confirmed = false;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto memory = fake->files[L"network-memory.json"];
        check(app.verify_campus_service(), "failed service verification fixture starts independently of authentication");
        wifiruntime::wait_for([&] {
            app.on_changed();
            return fake->returned == 1 && !app.snapshot().busy && app.memory_verification_message() == "campus.verify.failed";
        });
        check(app.memory_verification_message() == "campus.verify.failed" && app.snapshot().remembered &&
            fake->files[L"network-memory.json"] == memory && fake->evidence().fake_auth_submissions == 0,
            "unconfirmed service leaves the remembered Ethernet unchanged and never submits credentials");
        app.shutdown();
    }
    {
        auto services = std::make_unique<campusverificationruntime::Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services)); app.start();
        const auto files = fake->files;
        check(!app.verify_campus_service() && !app.snapshot().can_verify_campus && fake->discoveries == 0 && fake->credential_reads == 0 &&
            fake->files == files && fake->evidence().fake_auth_submissions == 0,
            "read-only controller disables campus verification and leaves isolated data, credentials and recovery untouched");
        app.shutdown();
    }
    {
        auto services = std::make_unique<campusverificationruntime::Fake>(); auto* fake = services.get();
        fake->files.erase(L"network-memory.json"); fake->files.erase(L"network-labels.json");
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.set_area(CampusArea::teaching);
        check(!app.snapshot().remembered && !app.snapshot().can_verify_campus && !app.verify_campus_service() &&
            fake->discoveries == 0, "selecting teaching alone never qualifies an unremembered physical cable for service recheck");
        app.shutdown();
    }
    for (const bool finding : {false, true}) {
        auto services = std::make_unique<campusverificationruntime::WiredRecoveryFake>(); auto* fake = services.get();
        wifi::Preferences preferences; preferences.auto_join = finding;
        fake->files[L"wifi-recovery.json"] = *preferences.encode();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.set_probe(ProbeMode::complete);
        const auto epoch = app.snapshot().epoch;
        for (int round = 1; round <= 3; ++round) {
            app.on_timer(kTimerWiFi); app.on_timer(kTimerAuto);
            wifiruntime::wait_for([&] { app.on_changed(); return fake->probes >= (round == 3 ? 4 : round) && !app.snapshot().busy; });
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); app.on_changed();
            if (round < 3) check(fake->evidence().fake_auth_submissions == 0,
                "wired runtime remains credential-free until two consecutive expired-session observations");
        }
        check(app.snapshot().online && app.snapshot().epoch == epoch && fake->chosen == CampusArea::teaching &&
            fake->evidence().fake_auth_submissions == 1 && fake->evidence().fake_wifi_calls == 0 && fake->fake_credentials_only,
            "optional Wi-Fi finding off or on leaves same-cable teaching reauthentication unchanged with exactly one fake submission");
        app.shutdown();
    }
}
