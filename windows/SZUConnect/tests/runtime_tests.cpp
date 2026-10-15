// Runs with the real SDK on Windows, or declaration stubs on other OSes; no formal service is started.
#include "../app/controller.hpp"
#include "../app/smoke_options.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
using namespace szu;
using namespace szu::app;
#include "wifi_runtime_tests.hpp"
#include "manual_priority_runtime_tests.hpp"
#include "settings_runtime_tests.hpp"
#include "settings_preservation_runtime_tests.hpp"

namespace {
class NetworkNotificationFake final : public wifiruntime::Fake {
public:
    NetworkNotificationFake() {
        files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
    }
    win::NetworkSnapshot capture_network() override {
        ++captures;
        win::NetworkSnapshot network;
        network.path.usable = true; network.path.link_active = true;
        network.path.identity = "fixture-notification-path";
        return network;
    }
    std::atomic<int> captures{0};
};

class SystemQueueFake final : public wifiruntime::Fake {
public:
    SystemQueueFake() {
        files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
    }
    win::NetworkSnapshot capture_network() override {
        win::NetworkSnapshot network;
        network.path.usable = true; network.path.link_active = true;
        network.path.identity = next_path ? "fixture-next-path" : "fixture-stable-path";
        return network;
    }
    ConnectivityReport probe(ProbeMode mode, const CancelToken&) override {
        ++probe_calls;
        auto report = ConnectivityReport::simple(false, false); report.mode = mode; return report;
    }
    std::optional<ConnectivityReport> system_connectivity(const CancelToken& token) override {
        const auto call = ++system_calls;
        ++active; peak = std::max(peak.load(), active.load());
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return released >= call; });
        // Deliberately return success after cancellation: publication, rather than the
        // fake transport, must enforce generation ownership and reject this response.
        if (token.cancelled()) ++cancelled;
        --active;
        return ConnectivityReport::simple(true, true);
    }
    void release(int count) {
        { std::lock_guard<std::mutex> lock(mutex); released = count; }
        cv.notify_all();
    }
    std::atomic<int> probe_calls{0}, system_calls{0}, active{0}, peak{0}, cancelled{0};
    std::atomic<bool> next_path{false};
private:
    std::mutex mutex;
    std::condition_variable cv;
    int released = 0;
};

// Real normal-mode recovery controller and physical-route selection, with all
// transport, account and persistence values supplied by this isolated fixture.
class RecoveryPrerequisiteFake final : public wifiruntime::Fake {
public:
    explicit RecoveryPrerequisiteFake(bool with_account) : with_account_(with_account) {
        files.erase(L"network-memory.json"); files.erase(L"network-labels.json");
        files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"recover","area":"dormitory","probeMode":"quick","appearanceMode":"system"})";
    }
    win::CredentialVault::ReadStatus read_credentials(Credentials& credentials) override {
        if (!with_account_) return win::CredentialVault::ReadStatus::missing;
        credentials = {"fixture-user", "fixture-password"};
        return win::CredentialVault::ReadStatus::found;
    }
    win::NetworkSnapshot capture_network() override {
        physical::Route route;
        route.interface_key = "fixture-prerequisite-wired"; route.interface_index = 12; route.interface_luid = 12;
        route.source_ipv4 = "192.0.2.7"; route.addresses = {route.source_ipv4}; route.dns_servers = {"192.0.2.53"};
        route.gateway_ipv4 = "192.0.2.1"; route.identity = "fixture-prerequisite-lease"; route.display_name = "Fixture Ethernet";
        route.descriptor = {"ethernet", {{"gw", "a4:5e:60:93:01:01"}, {"gwip", "192.0.2.1"}, {"net", "192.0.2.0/24"}}};
        route.is_system_primary = true;
        win::NetworkSnapshot network; network.access = "ethernet";
        network.path.usable = true; network.path.link_active = true; network.path.tunnel = true;
        network.path.identity = "fixture-system-topology"; network.path.requires_physical_binding = true;
        network.path.descriptors = {route.descriptor}; network.path.physical_candidates = {route};
        return network;
    }
    bool set_expected_physical_route(const std::optional<physical::Route>& route) override { return route.has_value(); }
    ConnectivityReport probe(ProbeMode mode, const CancelToken&) override {
        ++physical_checks;
        auto report = ConnectivityReport::simple(false, false); report.mode = mode;
        report.apple.state = report.microsoft.state = EndpointState::timeout;
        return report;
    }
    std::optional<ConnectivityReport> system_connectivity(const CancelToken& token) override {
        ++system_checks;
        if (token.cancelled()) return std::nullopt;
        auto report = ConnectivityReport::simple(true, true); report.mode = ProbeMode::complete;
        return report;
    }
    CampusDiscovery discover(CampusArea, const CancelToken&) override { ++discoveries; return {}; }
    LoginAttempt attempt(const Credentials&, CampusArea, const CancelToken&) override {
        ++evidence().fake_auth_submissions; return {};
    }
    std::atomic<int> physical_checks{0}, system_checks{0}, discoveries{0};
private:
    bool with_account_;
};

class MemoryFeedbackFake final : public wifiruntime::Fake {
public:
    MemoryFeedbackFake() {
        files.erase(L"network-memory.json");
        files.erase(L"network-labels.json");
        files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
    }
    win::NetworkSnapshot capture_network() override {
        win::NetworkSnapshot network; network.access = "wifi";
        network.path.usable = available; network.path.link_active = available;
        network.path.identity = next_path ? "fixture-memory-next" : "fixture-memory-original";
        network.path.descriptors = {NetworkDescriptor{"wifi", {{"gwip","192.0.2.1"},{"net","192.0.2.0/24"}}}};
        if (unreliable) network.path.descriptors.push_back(NetworkDescriptor{"ethernet", {{"gwip","192.0.2.2"},{"net","192.0.2.0/24"}}});
        return network;
    }
    CampusDiscovery discover(CampusArea, const CancelToken& token) override {
        ++discoveries;
        const auto registration = token.on_cancel([this] { cv.notify_all(); });
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return !block || release || token.cancelled(); });
        }
        token.remove(registration);
        return confirmed ? CampusDiscovery{Discovery::confirmed, Discovery::notChecked} : CampusDiscovery{};
    }
    ConnectivityReport probe(ProbeMode mode, const CancelToken& token) override {
        ++probes;
        const auto registration = token.on_cancel([this] { cv.notify_all(); });
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return !block_probe || release || token.cancelled(); });
        }
        token.remove(registration);
        auto report = ConnectivityReport::simple(false, false); report.mode = mode; return report;
    }
    void unblock() {
        { std::lock_guard<std::mutex> lock(mutex); release = true; }
        cv.notify_all();
    }
    bool available = true, next_path = false, unreliable = false, confirmed = false, block = false, block_probe = false;
    std::atomic<int> discoveries{0}, probes{0};
private:
    std::mutex mutex;
    std::condition_variable cv;
    bool release = false;
};
}

int main() {
    int checks = 0, failures = 0;
    auto check = [&](bool pass, const char* what) { ++checks; if (!pass) { ++failures; std::printf("FAIL: %s\n", what); } };
    run_wifi_runtime_tests(check);
    run_manual_priority_runtime_tests(check);
    run_settings_runtime_tests(check);
    run_settings_preservation_runtime_tests(check);
    for (const bool with_account : {true, false}) {
        auto services = std::make_unique<RecoveryPrerequisiteFake>(with_account); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        for (int round = 1; round <= 3; ++round) {
            wifiruntime::wait_for([&] {
                if (fake->physical_checks < round) app.on_timer(kTimerAuto);
                return fake->physical_checks >= round && fake->system_checks >= round &&
                    app.system_connectivity().has_value() && !app.snapshot().busy;
            });
        }
        const auto snapshot = app.snapshot();
        const auto system = app.system_connectivity(); const auto direct = app.internet_report();
        check(!snapshot.read_only && snapshot.mode == MonitoringMode::recover && snapshot.authentication_route &&
              snapshot.status == (with_account ? EngineStatus::needsTrust : EngineStatus::needsAccount) &&
              snapshot.auto_login == (with_account ? AutoLoginState::needsTrust : AutoLoginState::needsAccount),
              "normal recovery on an unknown bound attachment retains its actual trust or account prerequisite");
        check(system && system->online() && direct && !direct->base.online() && status::system_path_can_lead(snapshot),
              "normal automatic trust/account prerequisites do not hide fresh system success behind physical timeouts");
        const auto readiness = status::readiness(status::readiness_input_from(snapshot));
        const auto expected_key = with_account ? "ready.network.needsTrust" : "ready.account.missing";
        check(readiness.title == "ready.title.attention" &&
              std::any_of(readiness.items.begin(), readiness.items.end(), [expected_key](const status::ReadinessItem& item) {
                  return item.key == expected_key && item.tone == "warning";
              }), "system connectivity presentation retains the automatic-recovery prerequisite warning");
        check(!snapshot.online && !snapshot.trusted && !snapshot.remembered && !snapshot.last_discovery &&
              fake->discoveries == 0 && fake->evidence().fake_auth_submissions == 0 && fake->evidence().network_calls == 0 &&
              fake->evidence().production_storage_accesses == 0 && fake->evidence().vault_accesses == 0,
              "system success cannot grant campus identity, session trust or credential admission");
        app.pause();
        check(app.system_connectivity() && app.system_connectivity()->online() && !status::system_path_can_lead(app.snapshot()),
              "explicit recovery pause still takes precedence over an existing system-path success");
        app.shutdown();
    }
    {
        auto services = std::make_unique<SystemQueueFake>(); auto* fake = services.get();
        fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"observe","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        app.on_network_signal(); app.on_timer(kTimerNetwork);
        check(fake->system_calls == 0 && !app.system_connectivity_running(),
              "network notifications cannot start a first system check before a monitoring round or user request");
        app.check();
        wifiruntime::wait_for([&] { return fake->system_calls == 1 && fake->probe_calls == 1 && !app.snapshot().busy; });
        fake->release(1); wifiruntime::wait_for([&] { return app.system_connectivity().has_value(); });
        const auto epoch = app.snapshot().epoch;
        check(app.system_connectivity() && app.system_connectivity()->online(), "fixture begins with fresh system-path success");
        for (int round = 2; round <= 4; ++round) {
            app.on_network_signal();
            check(!app.system_connectivity() && app.system_connectivity_running(),
                  "a network signal discards success immediately while retaining only pending display revalidation");
            app.on_timer(kTimerNetwork);
            wifiruntime::wait_for([&] { return fake->system_calls >= round; });
            check(fake->system_calls == round, "a monitored path refresh starts a fresh system generation without a manual click");
            if (fake->system_calls != round) break;
            check(!app.system_connectivity(), "previous system success is never reused while its new generation is in flight");
            fake->release(round); wifiruntime::wait_for([&] { return app.system_connectivity().has_value(); });
            check(app.system_connectivity() && app.system_connectivity()->online() && !app.system_connectivity_running(),
                  "each periodic network notification can recover fresh system-path evidence automatically");
        }
        check(!app.snapshot().online && !app.snapshot().trusted && app.snapshot().epoch == epoch && fake->probe_calls == 1 &&
              fake->peak == 1 && fake->evidence().fake_auth_submissions == 0 && fake->evidence().network_calls == 0,
              "system-only revalidation leaves physical checks, campus trust and authentication untouched with one worker");
        fake->release(100); app.shutdown();
    }
    {
        auto services = std::make_unique<SystemQueueFake>(); auto* fake = services.get();
        fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"observe","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services)); app.start(); app.check();
        wifiruntime::wait_for([&] { return fake->system_calls == 1 && fake->probe_calls == 1; });
        for (int signal = 0; signal < 8; ++signal) app.on_network_signal();
        app.on_timer(kTimerNetwork); fake->release(1);
        wifiruntime::wait_for([&] { return fake->system_calls == 2; });
        check(fake->system_calls == 2 && fake->cancelled == 1 && fake->peak == 1 && !app.system_connectivity(),
              "coalesced signals queue one replacement and reject a successful response from the cancelled old generation");
        fake->release(2); wifiruntime::wait_for([&] { return app.system_connectivity().has_value(); });
        check(app.system_connectivity() && app.system_connectivity()->online() && !app.snapshot().trusted &&
              fake->evidence().fake_auth_submissions == 0 && fake->evidence().network_calls == 0,
              "observe-only can publish only the fresh replacement's display evidence without authentication");
        fake->release(100); app.shutdown();
    }
    for (const std::string stop : {"cancel", "sleep", "manual", "shutdown"}) {
        auto services = std::make_unique<SystemQueueFake>(); auto* fake = services.get();
        fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"observe","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.check();
        wifiruntime::wait_for([&] { return fake->system_calls == 1 && fake->probe_calls == 1; });
        app.on_network_signal();
        if (stop == "cancel") app.cancel();
        else if (stop == "sleep") app.on_suspend();
        else if (stop == "manual") app.set_mode(MonitoringMode::manual);
        else { fake->release(100); app.shutdown(); }
        app.on_timer(kTimerNetwork); fake->release(100);
        wifiruntime::wait_for([&] { return fake->active == 0; });
        check(fake->system_calls == 1 && !app.system_connectivity() && !app.system_connectivity_running(),
              "cancel, sleep, manual mode and shutdown suppress an already scheduled display recheck and stale success");
        check(!app.snapshot().trusted && fake->evidence().fake_auth_submissions == 0 && fake->evidence().network_calls == 0,
              "stopping a pending display recheck preserves the isolated authentication boundary");
        app.shutdown();
    }
    for (const bool sleep : {false, true}) {
        auto services = std::make_unique<SystemQueueFake>(); auto* fake = services.get();
        fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"observe","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.check();
        wifiruntime::wait_for([&] { return fake->system_calls == 1 && fake->probe_calls == 1; });
        app.on_network_signal(); app.on_timer(kTimerNetwork);
        if (sleep) app.on_suspend(); else app.cancel();
        fake->release(100); wifiruntime::wait_for([&] { return fake->active == 0; });
        app.on_timer(kTimerNetwork);
        check(fake->system_calls == 1 && !app.system_connectivity() && !app.system_connectivity_running(),
              "cancel or sleep removes a replacement already queued behind the cancelled old system worker");
        app.shutdown();
    }
    {
        auto services = std::make_unique<SystemQueueFake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.check();
        wifiruntime::wait_for([&] { return fake->system_calls == 1 && fake->probe_calls == 1; });
        app.on_network_signal(); app.on_timer(kTimerNetwork); fake->release(100);
        wifiruntime::wait_for([&] { return fake->active == 0; });
        check(fake->system_calls == 1 && !app.system_connectivity() && !app.system_connectivity_running(),
              "manual monitoring mode never turns a path notification into an automatic public recheck");
        app.shutdown();
    }
    for (const std::string mode : {"manual", "observe"}) {
        auto services = std::make_unique<SystemQueueFake>(); auto* fake = services.get();
        fake->files[L"settings.json"] = "{\"formatVersion\":1,\"monitoringMode\":\"" + mode + "\",\"area\":\"teaching\",\"probeMode\":\"quick\",\"appearanceMode\":\"system\"}";
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.check();
        wifiruntime::wait_for([&] { return fake->system_calls == 1 && fake->probe_calls == 1; });
        fake->release(1); wifiruntime::wait_for([&] { return app.system_connectivity().has_value(); });
        fake->next_path = true; app.refresh_environment();
        check(fake->system_calls == 1 && !app.system_connectivity() && !app.system_connectivity_running(),
              "metadata-only refresh invalidates a changed path without starting public traffic or leaving a pending spinner");
        app.shutdown();
    }
#ifdef _WIN32
    {
        HWND owner = CreateWindowExW(0, L"STATIC", L"Fixture system revalidation ordering", 0,
                                     0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(owner != nullptr, "isolated system revalidation message owner can be created");
        if (owner) {
            auto services = std::make_unique<SystemQueueFake>(); auto* fake = services.get();
            fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"observe","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
            AppController app(owner, RuntimeMode::normal, std::move(services)); app.start();
            fake->next_path = true; app.on_timer(kTimerAuto);
            wifiruntime::wait_for([&] { return fake->system_calls == 1 && fake->probe_calls == 1; });
            MSG message{};
            const bool notified = PeekMessageW(&message, owner, kMsgNetwork, kMsgNetwork, PM_REMOVE) != FALSE;
            check(notified, "automatic path capture posts its actual UI network notification");
            if (notified) app.on_network_signal();
            app.on_timer(kTimerNetwork); fake->release(1);
            wifiruntime::wait_for([&] { return fake->system_calls == 2; });
            check(fake->system_calls == 2 && fake->cancelled == 1 && !app.system_connectivity() && fake->peak == 1,
                  "the automatic worker's own notification cannot permanently erase its system-path revalidation");
            fake->release(2); wifiruntime::wait_for([&] { return app.system_connectivity().has_value(); });
            check(app.system_connectivity() && app.system_connectivity()->online() && !app.snapshot().online && !app.snapshot().trusted,
                  "a fresh system result follows the automatic notification while physical failure remains independent");
            fake->release(100); app.shutdown(); DestroyWindow(owner);
        }
    }
    {
        // A message-only owner exercises actual Win32 timer replacement semantics. All
        // network, storage and credential services remain the existing isolated fake.
        HWND owner = CreateWindowExW(0, L"STATIC", L"Fixture network notification timer", 0,
                                     0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(owner != nullptr, "isolated network timer owner can be created");
        if (owner) {
            auto services = std::make_unique<NetworkNotificationFake>(); auto* fake = services.get();
            AppController app(owner, RuntimeMode::observeOnly, std::move(services)); app.start();
            const auto before = fake->captures.load();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
            while (std::chrono::steady_clock::now() < deadline) {
                app.on_network_signal();
                MSG message{};
                while (PeekMessageW(&message, owner, WM_TIMER, WM_TIMER, PM_REMOVE))
                    if (message.wParam == kTimerNetwork) app.on_timer(kTimerNetwork);
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
            }
            check(fake->captures > before,
                  "physical identity refresh is not starved by sustained notifications faster than the debounce interval");
            app.on_timer(kTimerNetwork); app.on_changed();
            check(!app.snapshot().trusted && fake->evidence().formal_services_created == 0 &&
                  fake->evidence().production_storage_accesses == 0 && fake->evidence().vault_accesses == 0 &&
                  fake->evidence().network_calls == 0 && fake->evidence().fake_auth_submissions == 0,
                  "coalesced network refresh cannot create trust or invoke formal networking, storage or credentials");
            app.shutdown();
            DestroyWindow(owner);
        }
    }
#endif
    {
        auto services = std::make_unique<SystemQueueFake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services)); app.start(); app.check();
        wifiruntime::wait_for([&] { return fake->system_calls == 1 && fake->probe_calls == 1 && !app.snapshot().busy; });
        wifiruntime::wait_for([&] { if (fake->probe_calls < 2) app.check(); return fake->probe_calls >= 2; });
        check(fake->probe_calls == 2 && fake->active == 1 && fake->system_calls == 1 && app.system_connectivity_running(),
              "a blocked system display probe retains independent progress without delaying a second physical check");
        fake->release(1);
        wifiruntime::wait_for([&] { return fake->system_calls == 2; });
        check(fake->cancelled == 1 && !app.system_connectivity() && fake->peak == 1 && app.system_connectivity_running(),
              "late superseded success cannot publish or clear the new generation's running display state");
        fake->release(2);
        wifiruntime::wait_for([&] { return app.system_connectivity().has_value(); });
        check(app.system_connectivity() && app.system_connectivity()->online() && !app.snapshot().online && !app.snapshot().trusted &&
              !app.system_connectivity_running(),
              "current system completion ends only its display progress without physical online or campus trust");
        app.check();
        wifiruntime::wait_for([&] { return fake->system_calls == 3 && fake->probe_calls >= 3; });
        check(!app.system_connectivity() && app.system_connectivity_running(),
              "a new manual round expires preceding success and exposes independent system-path progress");
        app.cancel();
        check(!app.system_connectivity_running(), "cancel immediately clears system display progress even before its request returns");
        fake->release(100); app.shutdown();
        check(fake->active == 0 && fake->cancelled == 2 && !app.system_connectivity() && !app.system_connectivity_running() &&
              fake->evidence().network_calls == 0 && fake->evidence().fake_auth_submissions == 0,
              "cancel and shutdown reject late success and join the isolated system worker without any real request");
    }
    {
        auto services = std::make_unique<SystemQueueFake>(); auto* fake = services.get();
        fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"observe","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services)); app.start(); app.check();
        fake->release(1);
        wifiruntime::wait_for([&] { return app.system_connectivity().has_value() && !app.snapshot().busy; });
        wifiruntime::wait_for([&] { if (fake->system_calls < 2) app.on_timer(kTimerAuto); return fake->system_calls >= 2 && fake->probe_calls >= 2; });
        check(!app.system_connectivity() && fake->probe_calls >= 2 && app.system_connectivity_running(),
              "automatic rounds refresh display evidence rather than indefinitely reusing a past system success");
        app.cancel(); fake->release(100); app.shutdown();
        check(fake->active == 0 && fake->peak == 1 && !app.system_connectivity_running() && fake->evidence().network_calls == 0,
              "automatic system refresh shares the bounded managed worker and shuts down in isolation");
    }
    {
        auto services = std::make_unique<MemoryFeedbackFake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services)); app.start();
        check(!app.remember_network() && app.memory_verification_message() == "memory.verification.readOnly" && fake->discoveries == 0,
              "read-only memory admission reports its reason before credential-free discovery or a write");
        app.shutdown();
    }
    for (const auto blocker : {"unavailable", "unreliable", "unreadable", "pending"}) {
        auto services = std::make_unique<MemoryFeedbackFake>(); auto* fake = services.get();
        if (std::string(blocker) == "unavailable") fake->available = false;
        if (std::string(blocker) == "unreliable") fake->unreliable = true;
        if (std::string(blocker) == "unreadable") fake->files[L"network-memory.json"] = "fixture-corrupted-memory";
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        if (std::string(blocker) == "pending") app.on_network_signal();
        check(!app.remember_network() && app.memory_verification_message() == std::string("memory.verification.") + blocker && fake->discoveries == 0,
              "memory admission reports the specific local blocker without scheduling discovery");
        app.shutdown();
    }
    for (const auto outcome : {"needsProof", "remembered", "writeFailed"}) {
        auto services = std::make_unique<MemoryFeedbackFake>(); auto* fake = services.get();
        fake->confirmed = std::string(outcome) != "needsProof"; fake->block = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        fake->fail_write = std::string(outcome) == "writeFailed";
        const auto before = fake->files;
        check(app.remember_network() && app.memory_verification_message() == "memory.verification.checking",
              "memory verification reports credential-free progress as soon as its operation is reserved");
        wifiruntime::wait_for([&] { return fake->discoveries == 1; });
        check(!app.remember_network() && app.memory_verification_message() == "memory.verification.checking" && fake->discoveries == 1,
              "repeated memory clicks retain progress and cannot enqueue duplicate verification");
        fake->unblock();
        wifiruntime::wait_for([&] { app.on_changed(); return app.memory_verification_message() == std::string("memory.verification.") + outcome; });
        check(app.memory_verification_message() == std::string("memory.verification.") + outcome && app.notice().empty() &&
              !app.snapshot().trusted && !app.last_checked(),
              "memory completion reports its result inline without granting session trust or claiming a fresh Internet check");
        if (std::string(outcome) != "remembered")
            check(fake->files == before, "failed or unconfirmed memory verification preserves every original fixture file");
        check(fake->discoveries == 1 && fake->evidence().fake_auth_submissions == 0 && fake->evidence().network_calls == 0,
              "memory verification makes one injected credential-free discovery and performs no authentication or native request");
        app.shutdown();
    }
    {
        auto services = std::make_unique<MemoryFeedbackFake>(); auto* fake = services.get();
        fake->confirmed = true; fake->block = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.remember_network();
        wifiruntime::wait_for([&] { return fake->discoveries == 1; });
        app.on_network_signal(); fake->unblock();
        wifiruntime::wait_for([&] { app.on_changed(); return app.memory_verification_message() == "memory.verification.pending"; });
        check(app.memory_verification_message() == "memory.verification.pending",
              "completed memory proof waits for a pending physical-network refresh before presenting success");
        app.on_timer(kTimerNetwork); app.on_changed();
        check(app.memory_verification_message() == "memory.verification.remembered" && app.snapshot().remembered && !app.snapshot().trusted,
              "an unchanged debounced path admits the original result while keeping remembered eligibility separate from session trust");
        app.shutdown();
    }
    {
        auto services = std::make_unique<MemoryFeedbackFake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(app.trust() && app.snapshot().trusted && !app.snapshot().remembered,
              "explicit session trust does not create remembered campus eligibility");
        app.remember_network();
        wifiruntime::wait_for([&] { app.on_changed(); return app.memory_verification_message() == "memory.verification.needsProof"; });
        check(app.snapshot().trusted && !app.snapshot().remembered && fake->discoveries == 1 && app.notice().empty(),
              "failed campus-memory verification explains its result without erasing independent explicit session trust or persisting it");
        app.shutdown();
    }
    for (const bool changed : {false, true}) {
        auto services = std::make_unique<MemoryFeedbackFake>(); auto* fake = services.get();
        fake->confirmed = true; fake->block = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto before = fake->files;
        app.remember_network(); wifiruntime::wait_for([&] { return fake->discoveries == 1; });
        if (changed) { fake->next_path = true; app.on_network_signal(); app.on_timer(kTimerNetwork); }
        else app.cancel();
        fake->unblock();
        wifiruntime::wait_for([&] { app.on_changed(); return !app.snapshot().busy; });
        app.on_changed();
        check(app.memory_verification_message() == (changed ? "memory.verification.changed" : "memory.verification.cancelled") &&
              !app.snapshot().remembered && !app.snapshot().trusted && fake->files == before,
              "cancel or a confirmed path boundary retains inline feedback and rejects late proof without remembering or trusting");
        app.shutdown();
    }
    {
        auto services = std::make_unique<MemoryFeedbackFake>(); auto* fake = services.get(); fake->block_probe = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.check();
        wifiruntime::wait_for([&] { return fake->probes == 1; });
        check(!app.remember_network() && app.memory_verification_message() == "memory.verification.busy" && fake->discoveries == 0,
              "memory action explains an existing connection operation instead of silently ignoring the click");
        app.cancel(); fake->unblock(); app.shutdown();
    }
    {
        class LaunchFake final : public wifiruntime::Fake {
        public:
            bool launch_state = true, approved = false, fail_launch = true;
            int launch_attempts = 0;
            bool launch_enabled() override { return launch_state; }
            bool launch_approved() override { return approved; }
            bool set_launch(bool requested) override {
                ++launch_attempts;
                if (fail_launch) return false;
                launch_state = requested; return true;
            }
        };
        auto services = std::make_unique<LaunchFake>(); auto* fake = services.get();
        fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto before = fake->files;
        check(!app.set_launch(false) && app.notice() == "error.launchUpdate" && app.launch_enabled() && !app.launch_approved(),
              "failed startup change reports its error and retains actual enabled/approval readback");
        fake->fail_launch = false; fake->approved = true;
        check(app.set_launch(false) && !app.launch_enabled() && app.launch_approved() && app.notice().empty(),
              "retry updates startup state and clears its recovered failure notice");
        fake->fail_write = true;
        check(!app.set_wifi_auto_join(true) && app.notice() == "wifiRecovery.saveFailed",
              "independent failed preference write provides a real unrelated notice");
        fake->fail_write = false;
        check(app.set_launch(true) && app.launch_enabled() && app.notice() == "wifiRecovery.saveFailed",
              "successful startup change preserves an unrelated outstanding notice");
        app.shutdown();
        check(fake->launch_attempts == 3 && fake->files == before && fake->evidence().autostart_writes == 0 &&
              fake->evidence().production_storage_accesses == 0 && fake->evidence().fake_auth_submissions == 0,
              "startup failure/retry regression changes only the injected fake and never native startup/storage/authentication");

        auto readonly_services = std::make_unique<LaunchFake>(); auto* readonly_fake = readonly_services.get();
        readonly_fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
        AppController readonly(nullptr, RuntimeMode::observeOnly, std::move(readonly_services)); readonly.start();
        const auto readonly_before = readonly_fake->files;
        const auto notice = readonly.notice();
        check(!readonly.set_launch(false) && readonly_fake->launch_attempts == 0 && readonly.launch_enabled() &&
              !readonly.launch_approved() && readonly.notice() == notice && readonly_fake->files == readonly_before,
              "observe-only rejects startup changes before the injected setter and preserves state, notice and files");
        readonly.shutdown();
        check(readonly_fake->evidence().autostart_writes == 0 && readonly_fake->evidence().production_storage_accesses == 0 &&
              readonly_fake->evidence().vault_accesses == 0 && readonly_fake->evidence().network_calls == 0 &&
              readonly_fake->evidence().fake_auth_submissions == 0,
              "observe-only startup regression has no native or authentication side effects");
    }
    {
        class PathFake final : public wifiruntime::Fake {
        public:
            bool changed = false;
            std::optional<ConnectivityReport> system_connectivity(const CancelToken&) override {
                return ConnectivityReport::simple(true, true);
            }
            win::NetworkSnapshot capture_network() override {
                win::NetworkSnapshot network;
                network.path.usable = true; network.path.link_active = true;
                network.path.identity = changed ? "fixture-next-path" : "fixture-original-path";
                return network;
            }
        };
        auto services = std::make_unique<PathFake>(); auto* fake = services.get();
        fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"complete","appearanceMode":"system"})";
        fake->files[L"recovery.json"] = "{\"schema\":1,\"generation\":1,\"notBefore\":" +
            std::to_string(static_cast<long long>(AppController::wall_now() + 90)) + ",\"holds\":[{\"reason\":2,\"origin\":2}]}";
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.check();
        wifiruntime::wait_for([&] { return fake->evidence().fake_probe_calls > 0 && !app.snapshot().busy; });
        const auto report = app.internet_report();
        check(report.has_value(), "physical check publishes endpoint evidence");
        wifiruntime::wait_for([&] { return app.system_connectivity().has_value(); });
        check(app.system_connectivity() && app.system_connectivity()->online() && !app.snapshot().online &&
              fake->evidence().fake_auth_submissions == 0 && !app.snapshot().trusted,
              "system public connectivity never becomes physical engine success, campus trust or authentication");
        check(!app.snapshot().holds.empty() && app.snapshot().cooldown_remaining && *app.snapshot().cooldown_remaining > 0,
              "system public success cannot clear uncertain-submission hold or the original cooldown");
        app.on_network_signal(); app.on_timer(kTimerNetwork);
        check(!app.system_connectivity(), "a network notification invalidates system-path display evidence");
        const auto retained = app.internet_report();
        check(retained && report && retained->base.apple.state == report->base.apple.state && retained->websites_deferred,
              "unchanged network notification retains physical endpoint failure reasons");
        fake->changed = true; app.on_network_signal(); app.on_timer(kTimerNetwork);
        check(!app.internet_report(), "confirmed physical path change clears old base and website evidence");
        app.shutdown();
        check(fake->evidence().fake_auth_submissions == 0 && fake->evidence().network_calls == 0,
              "network presentation regression performs no authentication or real network calls");
    }
    {
        class CancelSystemFake final : public wifiruntime::Fake {
        public:
            std::atomic<bool> entered{false}, cancelled{false};
            std::optional<ConnectivityReport> system_connectivity(const CancelToken& token) override {
                entered = true;
                wifiruntime::wait_for([&] { return token.cancelled(); });
                cancelled = token.cancelled();
                return ConnectivityReport::simple(true, true); // a late successful response must be discarded
            }
        };
        auto services = std::make_unique<CancelSystemFake>(); auto* fake = services.get();
        fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services)); app.start(); app.check();
        wifiruntime::wait_for([&] { return fake->entered.load(); }); app.cancel(); app.shutdown();
        check(fake->cancelled && !app.system_connectivity(), "cancel/shutdown discards a late system-path success");
        check(fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 && fake->evidence().autostart_writes == 0,
              "system-path cancellation regression remains isolated from native services");
    }
    for (const auto path : {L"C:\\new\\receipt.json", L"D:/new/receipt.json"})
        check(absolute_receipt_path(path), "absolute local-drive receipt path accepted");
    for (const auto path : {L"", L"receipt.json", L"C:receipt.json", L"\\receipt.json", L"C:\\", L"\\\\server\\share\\receipt.json", L"\\\\?\\C:\\receipt.json", L"\\\\.\\device", L"C:\\file.json:secret", L"C:\\a\n.json"})
        check(!absolute_receipt_path(path), "relative/device/stream/control paths rejected");
    const auto launch = parse_launch_options({L"--offline-smoke",L"--smoke-output",L"C:\\new\\receipt.json"});
    check(launch.error.empty() && launch.offline_smoke && launch.smoke_timeout_ms == 10000, "offline contract defaults are explicit");
    check(parse_launch_options({L"--portable"}).portable, "portable startup explicitly bypasses deployment");
    const std::wstring ready_token(32, L'a');
    check(parse_launch_options({L"--installation-ready", ready_token}).ready_token == ready_token,
          "installation readiness token accepts only the bounded local event identifier");
    for (const auto& args : std::vector<std::vector<std::wstring>>{
        {L"--installation-ready"}, {L"--installation-ready", L"short"},
        {L"--installation-ready", std::wstring(32, L'G')},
        {L"--installation-ready", ready_token, L"--portable"},
        {L"--installation-ready", ready_token, L"--observe-only"},
        {L"--installation-ready", ready_token, L"--offline-smoke", L"--smoke-output", L"C:\\r.json"},
        {L"--installation-ready", ready_token, L"--installation-ready", ready_token},
        {L"--portable", L"--portable"}})
        check(!parse_launch_options(args).error.empty(), "conflicting or malformed installation options fail before any deployment");
    for (const auto& args : std::vector<std::vector<std::wstring>>{
         {L"--offline-smoke"}, {L"--offline-smoke",L"--observe-only",L"--smoke-output",L"C:\\r.json"},
         {L"--offline-smoke",L"--background",L"--smoke-output",L"C:\\r.json"}, {L"--smoke-output",L"C:\\r.json"},
         {L"--offline-smoke",L"--smoke-output",L"relative.json"}, {L"--offline-smoke",L"--offline-smoke"},
         {L"--offline-smoke",L"--smoke-output",L"C:\\r.json",L"--smoke-timeout-ms",L"999"},
         {L"--offline-smoke",L"--smoke-output",L"C:\\r.json",L"--smoke-timeout-ms",L"30001"},
         {L"--offline-smoke",L"--smoke-output",L"C:\\r.json",L"--smoke-timeout-ms",L"+1000"}})
        check(!parse_launch_options(args).error.empty(), "unsafe/conflicting/incomplete smoke options fail before runtime creation");
    for (const auto n : {L"1000", L"30000"})
        check(parse_launch_options({L"--offline-smoke",L"--smoke-output",L"C:\\r.json",L"--smoke-timeout-ms",n}).error.empty(), "timeout range endpoints accepted");
    {
        win::Storage disabled(win::Storage::Access::disabled);
        std::string out;
        check(!disabled.available() && disabled.read(L"settings.json", out) == win::Storage::ReadStatus::failed &&
              !disabled.write(L"settings.json", "fixture") && !disabled.quarantine(L"settings.json") &&
              !disabled.write_preserving_existing(L"settings.json", "fixture", true), "disabled storage never initializes or writes a formal path");
    }
    {
        AppController readonly(nullptr, RuntimeMode::observeOnly);
        readonly.set_mode(MonitoringMode::recover); readonly.set_area(CampusArea::teaching); readonly.set_probe(ProbeMode::complete);
        readonly.set_appearance(Appearance::dark); readonly.set_tutorial("completed");
        check(readonly.settings().mode == MonitoringMode::manual && readonly.settings().area == CampusArea::automatic &&
              readonly.settings().probe == ProbeMode::complete && readonly.settings().appearance == Appearance::dark && readonly.settings().tutorial == "none" &&
              readonly.snapshot().probe_mode == ProbeMode::complete,
              "observe-only allows session-local probe/theme changes while monitoring/area/tutorial stay protected");
        check(!readonly.set_launch(true) && !readonly.replace_website_targets({{"fake","https://fixture.invalid/",true}}) &&
              readonly.save_account() == ConnectionEngine::SaveResult::failed, "observe-only gates autostart/websites/vault before side effects");
        auto& e = readonly.runtime_evidence();
        check(e.production_storage_accesses == 0 && e.vault_accesses == 0 && e.autostart_writes == 0 && e.network_calls == 0,
              "session-local and blocked observe-only commands never reach native operations");
        readonly.set_probe(ProbeMode::quick); readonly.set_appearance(Appearance::light);
        check(readonly.snapshot().probe_mode == ProbeMode::quick && readonly.settings().appearance == Appearance::light &&
              e.production_storage_accesses == 0 && e.vault_accesses == 0 && e.autostart_writes == 0,
              "observe-only can switch back without writing storage/vault/autostart");
        AppController fresh(nullptr, RuntimeMode::observeOnly);
        check(fresh.settings().probe == ProbeMode::quick && fresh.settings().appearance == Appearance::system,
              "a new observe-only controller does not inherit the previous session's probe/theme");
    }
    {
        AppController offline(nullptr, RuntimeMode::offlineSmoke); offline.start(); offline.connect();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while ((!offline.fake_auth_passed() || offline.snapshot().busy) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        offline.on_changed(); offline.shutdown(); offline.shutdown();
        auto& e = offline.runtime_evidence();
        check(offline.fake_auth_passed() && e.fake_auth_submissions == 1 && e.fake_probe_calls > 0,
              "offline controller completes one fake authentication through the real engine and shuts down twice safely");
        check(e.formal_services_created == 0 && e.production_storage_accesses == 0 && e.vault_accesses == 0 &&
              e.network_calls == 0 && e.watchers_started == 0 && e.autostart_writes == 0,
              "early offline DI constructs and invokes zero formal services");
    }
    std::printf("Runtime contract tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
