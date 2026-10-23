#pragma once
#include "wifi_runtime_tests.hpp"
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace manualnetworkruntime {
class Fake final : public wifiruntime::Fake {
public:
    explicit Fake(const std::string& mode = "manual") : ui_thread(std::this_thread::get_id()) {
        files[L"settings.json"] = "{\"formatVersion\":1,\"monitoringMode\":\"" + mode +
            "\",\"area\":\"teaching\",\"probeMode\":\"complete\",\"appearanceMode\":\"system\"}";
        szu::NetworkMemory memory("00112233445566778899aabbccddeeff");
        memory.remember(route(false).descriptor, "teaching", true, false, szu::app::AppController::wall_now(), "11223344");
        memory.remember(route(true).descriptor, "teaching", true, false, szu::app::AppController::wall_now(), "55667788");
        files[L"network-memory.json"] = memory.encode();
    }
    static szu::physical::Route route(bool wired) {
        szu::physical::Route r;
        r.interface_key = wired ? "fixture-manual-wired" : "fixture-manual-wifi";
        r.interface_index = wired ? 23 : 22; r.interface_luid = r.interface_index;
        r.source_ipv4 = wired ? "198.51.100.7" : "192.0.2.7"; r.addresses = {r.source_ipv4};
        r.dns_servers = {wired ? "198.51.100.53" : "192.0.2.53"};
        r.gateway_ipv4 = wired ? "198.51.100.1" : "192.0.2.1";
        r.identity = wired ? "fixture-manual-wired-lease" : "fixture-manual-wifi-lease";
        r.is_system_primary = true; r.display_name = wired ? "Fixture cable" : "Fixture Wi-Fi";
        r.descriptor = {wired ? "ethernet" : "wifi", {{"gw", wired ? "02:00:00:00:00:23" : "02:00:00:00:00:22"},
            {"gwip", r.gateway_ipv4}, {"net", wired ? "198.51.100.0/24" : "192.0.2.0/24"}}};
        return r;
    }
    szu::win::NetworkSnapshot capture_network() override {
        ++captures;
        const auto selected = route(wired);
        szu::win::NetworkSnapshot result;
        result.access = selected.descriptor.kind; result.tunnel_known = true;
        result.path.usable = result.path.link_active = true;
        result.path.requires_physical_binding = true; result.path.tunnel = true;
        result.path.identity = selected.identity; result.path.descriptors = {selected.descriptor};
        result.path.physical_candidates = {selected};
        if (block_next.exchange(false)) {
            capture_off_ui = std::this_thread::get_id() != ui_thread;
            capture_waiting = true;
            std::unique_lock<std::mutex> lock(mutex);
            if (!cv.wait_for(lock, std::chrono::seconds(2), [&] { return released; })) capture_timed_out = true;
        }
        ++captures_completed;
        if (fail_capture) throw std::runtime_error("isolated capture failure");
        return result;
    }
    bool set_expected_physical_route(const std::optional<szu::physical::Route>& value) override {
        bound = value;
        return value && *value == route(wired);
    }
    szu::ConnectivityReport probe(szu::ProbeMode mode, const szu::CancelToken&) override {
        ++probes;
        auto result = szu::ConnectivityReport::simple(online || submitted, online || submitted);
        result.mode = mode;
        if (!result.online()) {
            result.apple.state = result.microsoft.state = szu::EndpointState::redirect;
            result.apple.campus_area_hint = result.microsoft.campus_area_hint = szu::CampusPortalHint::teaching;
        }
        return result;
    }
    std::optional<szu::ConnectivityReport> system_connectivity(szu::ProbeMode mode, const szu::CancelToken&) override {
        auto result = szu::ConnectivityReport::simple(true, true); result.mode = mode; return result;
    }
    szu::CampusDiscovery discover(szu::CampusArea, const szu::CancelToken&) override {
        ++discoveries; return {szu::Discovery::confirmed, szu::Discovery::confirmed, true};
    }
    szu::LoginAttempt attempt(const szu::Credentials& credentials, szu::CampusArea area, const szu::CancelToken&) override {
        ++attempts;
        if (credentials != szu::Credentials{"offline-fixture", "offline-password"} || area != szu::CampusArea::teaching || !bound)
            return {};
        submitted_on = bound->interface_key;
        ++evidence().fake_auth_submissions; submitted = true;
        szu::LoginAttempt result; result.outcome = szu::Outcome::success; return result;
    }
    void release_capture() {
        { std::lock_guard<std::mutex> lock(mutex); released = true; }
        cv.notify_all();
    }
    std::atomic<bool> wired{false}, online{false}, submitted{false}, block_next{false}, fail_capture{false};
    std::atomic<bool> capture_waiting{false}, capture_off_ui{false}, capture_timed_out{false};
    std::atomic<int> captures{0}, captures_completed{0}, probes{0}, discoveries{0}, attempts{0};
    std::optional<szu::physical::Route> bound;
    std::string submitted_on;
private:
    std::thread::id ui_thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;
};

inline void drain(szu::app::AppController& app, const std::function<bool()>& done) {
    wifiruntime::wait_for([&] { app.on_changed(); return done() && !app.snapshot().busy; });
    app.on_changed();
}
}

template<class Check> void run_manual_network_refresh_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    using manualnetworkruntime::Fake;
    for (const bool once : {false, true}) {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto settings = fake->files[L"settings.json"];
        fake->wired = true; fake->block_next = true;
        if (once) app.login_once(); else app.connect();
        wifiruntime::wait_for([&] { return fake->capture_waiting.load(); });
        app.connect(); app.login_once();
        check(fake->capture_off_ui && app.snapshot().busy &&
            app.snapshot().status == EngineStatus::checking && fake->captures == 2 && fake->probes == 0 && fake->attempts == 0,
            "manual and one-time login first capture the current physical network off the UI thread, with busy/cancel state and no duplicate queue");
        fake->release_capture();
        manualnetworkruntime::drain(app, [&] { return fake->submitted.load(); });
        check(!fake->capture_timed_out && fake->attempts == 1 && fake->submitted_on == "fixture-manual-wired" &&
            app.snapshot().online && app.snapshot().authentication_route == std::optional<physical::Route>(Fake::route(true)) &&
            app.network().access == "ethernet" && fake->files[L"settings.json"] == settings,
            "a missed network notification cannot make manual login use the old Wi-Fi binding; the fresh cable is authenticated once and displayed");
        app.shutdown();
        check(fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 &&
            fake->evidence().production_storage_accesses == 0 && fake->evidence().fake_wifi_calls == 0,
            "manual freshness regression uses only fake credentials, isolated memory and injected transport");
    }
    for (const std::string change : {"cancel", "signal", "refresh", "sleep", "area", "mode", "delete", "shutdown"}) {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto memory = fake->files[L"network-memory.json"];
        fake->wired = true; fake->block_next = true; app.connect();
        wifiruntime::wait_for([&] { return fake->capture_waiting.load(); });
        if (change == "cancel") app.cancel();
        else if (change == "signal") app.on_network_signal();
        else if (change == "refresh") { fake->wired = false; app.refresh_environment(); }
        else if (change == "sleep") app.on_suspend();
        else if (change == "area") app.set_area(CampusArea::dormitory);
        else if (change == "mode") app.set_mode(MonitoringMode::observe);
        else if (change == "delete") app.delete_account();
        if (change == "shutdown") {
            std::thread release([&] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); fake->release_capture(); });
            app.shutdown(); release.join();
        } else {
            fake->release_capture();
            manualnetworkruntime::drain(app, [&] { return fake->captures_completed >= (change == "refresh" ? 3 : 2); });
            app.shutdown();
        }
        check(!fake->capture_timed_out && fake->probes == 0 && fake->discoveries == 0 && fake->attempts == 0 &&
            fake->files[L"network-memory.json"] == memory && !app.snapshot().online,
            "cancel, network notification/refresh, sleep, area/mode change, account deletion and shutdown discard a late manual capture before any request");
    }
    {
        auto services = std::make_unique<Fake>("recover"); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.pause();
        const auto before = app.snapshot(); const auto due = app.next_check();
        fake->block_next = true; app.connect(); wifiruntime::wait_for([&] { return fake->capture_waiting.load(); });
        app.on_timer(kTimerAuto); app.on_changed();
        check(app.next_check() == due && app.snapshot().paused == before.paused && app.snapshot().holds.size() == before.holds.size() &&
            fake->probes == 0, "waiting for manual capture does not advance an existing monitoring deadline or release pause/holds");
        app.cancel(); fake->release_capture(); manualnetworkruntime::drain(app, [&] { return fake->captures_completed == 2; });
        check(app.next_check() == due && fake->attempts == 0, "cancelled fresh capture preserves the prior deadline without automatic authentication");
        app.shutdown();
    }
    for (const bool read_only : {false, true}) {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, read_only ? RuntimeMode::observeOnly : RuntimeMode::normal, std::move(services)); app.start();
        const auto memory = fake->files[L"network-memory.json"];
        fake->fail_capture = !read_only; fake->wired = true; app.connect();
        manualnetworkruntime::drain(app, [&] { return fake->captures_completed == 2; });
        check(fake->attempts == 0 && fake->files[L"network-memory.json"] == memory &&
            (read_only ? fake->probes > 0 : fake->probes == 0),
            "read-only manual freshness never authenticates or saves memory, and capture failure never falls back to the old route");
        if (!read_only) {
            check(app.notice() == "env.identification.readFailed", "manual capture failure has a visible notice even when the old connection was identifiable");
            fake->fail_capture = false; fake->online = true; app.connect();
            manualnetworkruntime::drain(app, [&] { return fake->probes > 0; });
            check(app.notice().empty() && fake->attempts == 0, "a successful new capture clears its own previous failure without forcing authentication");
            app.set_launch(true); const auto unrelated = app.notice();
            const int probes = fake->probes; app.connect();
            manualnetworkruntime::drain(app, [&] { return fake->probes > probes; });
            check(!unrelated.empty() && app.notice() == unrelated, "successful capture does not clear unrelated settings errors");
        }
        app.shutdown();
    }
    for (const std::string change : {"none", "decline", "dialogNetwork", "captureNetwork", "dialogReport"}) {
        auto services = std::make_unique<Fake>(); auto* fake = services.get(); fake->online = true;
        AppController* owner = nullptr;
        AppController app(nullptr, RuntimeMode::normal, std::move(services), {},
            [&](HWND, const std::wstring&, const std::wstring&) {
                if (change == "decline") return false;
                if (change == "dialogNetwork") { fake->wired = true; owner->refresh_environment(); }
                if (change == "dialogReport") {
                    const int probes = fake->probes;
                    owner->check(); manualnetworkruntime::drain(*owner, [&] { return fake->probes > probes; });
                }
                return true;
            });
        owner = &app; app.start(); app.connect();
        manualnetworkruntime::drain(app, [&] { return fake->probes > 0; });
        check(app.snapshot().offer_forced_login && fake->attempts == 0, "force fixture has a completed online report and requires its own explicit confirmation");
        const int captures = fake->captures;
        if (change == "captureNetwork") fake->wired = true;
        app.force_login();
        manualnetworkruntime::drain(app, [&] { return change != "none" || fake->attempts == 1; });
        check(change == "none" ? fake->attempts == 1 && fake->evidence().fake_auth_submissions == 1 : fake->attempts == 0,
            "force confirmation is bound to its original epoch and report; changed dialog/capture contexts never transfer it to a different target");
        check(change != "decline" || fake->captures == captures, "declining the force dialog starts no snapshot or request");
        app.shutdown();
    }
}
