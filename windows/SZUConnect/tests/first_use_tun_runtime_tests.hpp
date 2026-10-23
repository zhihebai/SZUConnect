#pragma once
#include "wifi_runtime_tests.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace firstusetunruntime {
// Cold installation state and a single reliable physical attachment below a TUN.
// Protocol responses are injected: these checks do not exercise a real DNS server or proxy client.
class Fake final : public wifiruntime::Fake {
public:
    Fake() { files.clear(); }
    static szu::Credentials credentials() { return {"offline-new-user", "offline-new-password"}; }
    szu::physical::Route route() const {
        szu::physical::Route value;
        value.interface_key = changed ? "fixture-first-use-second" : "fixture-first-use-first";
        value.interface_index = changed ? 32 : 31; value.interface_luid = value.interface_index;
        value.source_ipv4 = changed ? "198.51.100.7" : "192.0.2.7"; value.addresses = {value.source_ipv4};
        value.gateway_ipv4 = changed ? "198.51.100.1" : "192.0.2.1";
        value.dns_servers = {changed ? "198.51.100.53" : "192.0.2.53"};
        value.identity = changed ? "fixture-first-use-second-lease" : "fixture-first-use-first-lease";
        value.is_system_primary = false; value.display_name = "Fixture physical Wi-Fi";
        value.descriptor = {"wifi", {{"gw", changed ? "02:00:00:00:00:32" : "02:00:00:00:00:31"},
            {"gwip", value.gateway_ipv4}, {"net", changed ? "198.51.100.0/24" : "192.0.2.0/24"}}};
        return value;
    }
    szu::win::NetworkSnapshot capture_network() override {
        ++captures;
        const auto current = route();
        if (hold_capture.exchange(false)) gate(capture_entered, capture_released);
        szu::win::NetworkSnapshot value; value.access = "wifi"; value.tunnel_known = true;
        value.path.usable = value.path.link_active = value.path.requires_physical_binding = value.path.tunnel = true;
        value.path.identity = current.identity; value.path.descriptors = {current.descriptor};
        value.path.physical_candidates = {current}; ++captures_returned; return value;
    }
    bool set_expected_physical_route(const std::optional<szu::physical::Route>& value) override {
        bound = value;
        if (!value || *value != route()) { ++binding_failures; return false; }
        return true;
    }
    szu::ConnectivityReport probe(szu::ProbeMode mode, const szu::CancelToken&) override {
        ++probes;
        if (hold_probe.exchange(false)) gate(probe_entered, probe_released);
        if (!bound || *bound != route()) ++binding_failures;
        auto report = szu::ConnectivityReport::simple(submitted, submitted); report.mode = mode;
        if (!submitted) {
            report.apple.state = report.microsoft.state = szu::EndpointState::redirect;
            report.apple.campus_area_hint = report.microsoft.campus_area_hint = szu::CampusPortalHint::teaching;
        }
        ++probes_returned; return report;
    }
    std::optional<szu::ConnectivityReport> system_connectivity(szu::ProbeMode mode, const szu::CancelToken&) override {
        ++system_probes;
        auto report = szu::ConnectivityReport::simple(true, true); report.mode = mode; return report;
    }
    szu::CampusDiscovery discover(szu::CampusArea, const szu::CancelToken&) override {
        ++discoveries;
        if (hold_discovery.exchange(false)) gate(discovery_entered, discovery_released);
        if (!bound || *bound != route()) ++binding_failures;
        ++discoveries_returned;
        // Deliberately return a successful late payload after cancellation. The engine must discard it.
        return {szu::Discovery::confirmed, szu::Discovery::confirmed, true};
    }
    szu::LoginAttempt attempt(const szu::Credentials& account, szu::CampusArea area, const szu::CancelToken&) override {
        ++attempts;
        if (!bound || *bound != route() || account != credentials() || area != szu::CampusArea::teaching) {
            ++binding_failures; return {};
        }
        ++evidence().fake_auth_submissions; submitted = true;
        szu::LoginAttempt result; result.outcome = szu::Outcome::success; return result;
    }
    szu::win::CredentialVault::ReadStatus read_credentials(szu::Credentials& account) override {
        ++credential_reads;
        if (!saved) return szu::win::CredentialVault::ReadStatus::missing;
        account = *saved; return szu::win::CredentialVault::ReadStatus::found;
    }
    bool write_credentials(const szu::Credentials& account) override { ++credential_writes; saved = account; return true; }
    bool erase_credentials() override { saved.reset(); return true; }
    void release_all() {
        { std::lock_guard lock(gate_mutex); capture_released = probe_released = discovery_released = true; }
        gate_changed.notify_all();
    }
    void release_probe() { release(probe_released); }
    void release_capture() { release(capture_released); }
    void release_discovery() { release(discovery_released); }
    std::atomic<bool> changed{false}, submitted{false}, timed_out{false};
    std::atomic<bool> hold_capture{false}, hold_probe{false}, hold_discovery{false};
    std::atomic<bool> capture_entered{false}, probe_entered{false}, discovery_entered{false};
    std::atomic<int> captures{0}, captures_returned{0}, probes{0}, probes_returned{0}, system_probes{0};
    std::atomic<int> discoveries{0}, discoveries_returned{0}, attempts{0}, binding_failures{0};
    int credential_reads = 0, credential_writes = 0;
    std::optional<szu::Credentials> saved;
    std::optional<szu::physical::Route> bound;
private:
    void gate(std::atomic<bool>& entered, bool& released) {
        std::unique_lock lock(gate_mutex); entered = true; gate_changed.notify_all();
        if (!gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return released; })) timed_out = true;
    }
    void release(bool& released) {
        { std::lock_guard lock(gate_mutex); released = true; }
        gate_changed.notify_all();
    }
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool capture_released = false, probe_released = false, discovery_released = false;
};

inline void drain(szu::app::AppController& app, const std::function<bool()>& complete) {
    wifiruntime::wait_for([&] { app.on_changed(); return complete(); });
    app.on_changed();
}
}

template<class Check> void run_first_use_tun_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    using firstusetunruntime::Fake;
    for (const bool save : {false, true}) {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        check(fake->files.empty() && !fake->saved, "first-use fixture has no settings, credentials, memory, recovery journal or previous success");
        AppController app(nullptr, RuntimeMode::normal, std::move(services), [](UINT) { return true; }); app.start();
        const auto initial = app.snapshot();
        check(app.settings().mode == MonitoringMode::manual && initial.area == CampusArea::automatic &&
            !initial.has_saved && !initial.remembered && !initial.trusted && !initial.last_discovery &&
            initial.authentication_route == std::optional<physical::Route>(fake->route()) &&
            !fake->route().is_system_primary && app.network().path.tunnel && fake->probes == 0 && fake->attempts == 0,
            "a fresh TUN installation selects its sole reliable non-primary physical route without history or automatic traffic");
        // The user opts into monitoring. Consume its timer during each blocked foreground stage;
        // the restored public deadline then proves the entire Controller worker handoff has finished.
        app.set_mode(MonitoringMode::recover);
        fake->hold_probe = true; app.check();
        firstusetunruntime::drain(app, [&] { return fake->probe_entered.load(); });
        app.on_timer(kTimerAuto);
        app.set_draft(Fake::credentials().account, Fake::credentials().password);
        check(app.snapshot().busy && app.snapshot().draft_complete && !app.snapshot().has_saved && !app.next_check() &&
            fake->discoveries == 0 && fake->attempts == 0 && !fake->files.count(L"network-memory.json"),
            "the first credential-free check can coexist with a new account draft but cannot discover, authenticate or remember it");
        fake->release_probe();
        firstusetunruntime::drain(app, [&] {
            return !app.snapshot().busy && fake->probes_returned == 1 && app.last_checked() && app.next_check();
        });
        check(!app.snapshot().busy && !app.snapshot().online && app.last_checked() && app.next_check() && fake->attempts == 0,
            "a first offline check finishes its Controller handoff and leaves the original automatic area available for the next user action");
        if (save) check(app.save_account() == ConnectionEngine::SaveResult::saved && fake->saved == Fake::credentials(),
            "a first saved account is persisted only by the explicit save action");
        fake->changed = true; fake->hold_capture = fake->hold_discovery = true;
        if (save) app.connect(); else app.login_once();
        firstusetunruntime::drain(app, [&] { return fake->capture_entered.load(); });
        check(app.snapshot().busy && fake->captures == 2 && fake->probes == 1 && fake->attempts == 0,
            "first one-time and saved-account login reserve the fresh capture stage without requiring a remembered network");
        fake->release_capture();
        firstusetunruntime::drain(app, [&] { return fake->discovery_entered.load(); });
        app.on_timer(kTimerAuto);
        check(app.snapshot().busy && app.snapshot().area == CampusArea::automatic && fake->bound == fake->route() &&
            app.snapshot().authentication_route == std::optional<physical::Route>(fake->route()) && fake->attempts == 0 &&
            !fake->files.count(L"network-memory.json"),
            "a missed initial route notification is repaired before independent service discovery, with no stale binding or implicit memory");
        fake->release_discovery();
        firstusetunruntime::drain(app, [&] { return !app.snapshot().busy && app.snapshot().online && app.next_check(); });
        check(fake->attempts == 1 && fake->evidence().fake_auth_submissions == 1 && fake->binding_failures == 0 &&
            app.snapshot().online && app.snapshot().area == CampusArea::automatic &&
            !app.snapshot().remembered && !fake->files.count(L"network-memory.json") &&
            fake->credential_writes == (save ? 1 : 0) && app.snapshot().has_saved == save,
            "cold TUN login selects the freshly bound teaching entry and submits once, without old success, trust, memory or an unsolicited credential save");
        const int submissions = fake->attempts;
        check(app.remember_network(), "after first login the same fresh physical attachment admits explicit credential-free remember verification");
        firstusetunruntime::drain(app, [&] { return !app.snapshot().busy && app.next_check() &&
            app.memory_verification_message() == "memory.verification.remembered"; });
        check(app.snapshot().remembered && app.remembered_networks().size() == 1 &&
            fake->files.count(L"network-memory.json") == 1 && fake->attempts == submissions &&
            fake->credential_writes == (save ? 1 : 0) && fake->binding_failures == 0 && !fake->timed_out,
            "first memory is created only after its own successful bound service verification, without a second login or credential write");
        app.shutdown();
        check(fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 &&
            fake->evidence().production_storage_accesses == 0 && fake->evidence().fake_wifi_calls == 0,
            "first-use TUN coverage uses injected protocol responses and isolated files and never changes DNS, proxy settings or real Wi-Fi");
    }
    for (const int interruption : {0, 1, 2}) {
        auto services = std::make_unique<Fake>(); auto* fake = services.get(); fake->hold_discovery = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        app.set_draft(Fake::credentials().account, Fake::credentials().password); app.login_once();
        firstusetunruntime::drain(app, [&] { return fake->discovery_entered.load(); });
        const auto before = app.snapshot();
        check(before.busy && fake->attempts == 0 && !before.remembered,
            "a truly cold default-manual first login reaches independent discovery without requiring prior checks, settings or memory");
        if (interruption == 2) { fake->changed = true; app.refresh_environment(); }
        else if (interruption == 1) app.cancel();
        fake->release_all();
        firstusetunruntime::drain(app, [&] { return fake->discoveries_returned == 1 && !app.snapshot().busy &&
            (interruption != 0 || app.snapshot().online); });
        if (interruption == 0) {
            check(app.snapshot().online && app.snapshot().area == CampusArea::automatic &&
                app.settings().mode == MonitoringMode::manual && fake->attempts == 1 &&
                fake->evidence().fake_auth_submissions == 1 && fake->binding_failures == 0 &&
                !app.snapshot().remembered && !fake->files.count(L"settings.json") &&
                !fake->files.count(L"network-memory.json") && fake->credential_writes == 0 && !fake->timed_out,
                "the default-manual first login succeeds once on a non-primary TUN attachment without settings, old success, memory or a saved account");
        } else {
            check(fake->attempts == 0 && !app.snapshot().last_discovery && !app.snapshot().remembered &&
                !app.snapshot().required_campus_confirmed && !fake->files.count(L"network-memory.json") &&
                fake->credential_writes == 0 && (interruption != 2 || app.snapshot().epoch != before.epoch) && !fake->timed_out,
                "cancelling or replacing a first-use TUN attachment rejects a late successful discovery before credentials, trust or memory");
        }
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services)); app.start();
        app.set_draft(Fake::credentials().account, Fake::credentials().password); app.login_once();
        firstusetunruntime::drain(app, [&] { return fake->probes_returned > 0 && !app.snapshot().busy && app.last_checked(); });
        check(!app.remember_network() && fake->credential_reads == 0 && fake->credential_writes == 0 &&
            fake->attempts == 0 && fake->discoveries == 0 && fake->files.empty() && !app.snapshot().has_saved &&
            !app.snapshot().remembered && !app.snapshot().trusted && !fake->timed_out,
            "cold read-only TUN detection performs only bound checks, with zero credential reads, writes, authentication or memory changes");
        app.shutdown();
    }
}
