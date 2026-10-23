#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace proxysuggestionruntime {
class Fake final : public wifiruntime::Fake {
public:
    Fake() {
        files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"automatic","probeMode":"quick","appearanceMode":"system"})";
        files.erase(L"network-memory.json"); files.erase(L"network-labels.json");
    }
    szu::app::RuntimeMode mode() const override { return szu::app::RuntimeMode::normal; }
    szu::win::NetworkSnapshot capture_network() override {
        szu::physical::Route route;
        route.interface_key = "fixture-suggestion-wired"; route.interface_index = 12; route.interface_luid = 12;
        route.source_ipv4 = "192.0.2.7"; route.addresses = {route.source_ipv4}; route.dns_servers = {"192.0.2.53"};
        route.gateway_ipv4 = "192.0.2.1"; route.identity = "fixture-suggestion-lease-" + std::to_string(network_number.load());
        route.descriptor = {"ethernet", {{"gwip", "192.0.2.1"}, {"net", "192.0.2.0/24"}}};
        route.is_system_primary = true;
        szu::win::NetworkSnapshot network; network.access = "ethernet";
        network.path.usable = true; network.path.link_active = link_active;
        network.path.tunnel = tunnel;
        network.path.requires_physical_binding = true;
        network.path.identity = "fixture-suggestion-topology-" + std::to_string(network_number.load());
        network.path.descriptors = {route.descriptor}; network.path.physical_candidates = {route};
        if (!physical_identified || !link_active) { network.path.descriptors.clear(); network.path.physical_candidates.clear(); }
        if (multiple_physical) {
            network.path.physical_candidates.front().is_system_primary = false;
            auto second = route; second.interface_key += "-other"; second.interface_index = 13; second.interface_luid = 13;
            second.identity += "-other"; second.is_system_primary = false;
            second.gateway_ipv4 = "192.0.2.2";
            second.descriptor.raw["gwip"] = "192.0.2.2";
            network.path.physical_candidates.push_back(second); network.path.descriptors.push_back(second.descriptor);
        }
        network.tunnel_known = true; network.tunnel.tunnel = tunnel;
        return network;
    }
    bool set_expected_physical_route(const std::optional<szu::physical::Route>& route) override { return route.has_value(); }
    szu::ConnectivityReport probe(szu::ProbeMode mode, const szu::CancelToken& token) override {
        ++physical_calls;
        while (block_physical && !token.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto result = szu::ConnectivityReport::simple(physical_online, physical_online); result.mode = mode;
        if (cancelled_report) { result.apple.state = szu::EndpointState::cancelled; result.complete_result = szu::CompleteCheckResult::cancelled; }
        ++physical_returns;
        return result;
    }
    std::optional<szu::ConnectivityReport> system_connectivity(szu::ProbeMode mode, const szu::CancelToken& token) override {
        ++system_calls;
        while (block_system && !token.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        // Intentionally return a completed failure after cancellation. Publication,
        // not transport cooperation, must reject a superseded generation.
        auto result = szu::ConnectivityReport::simple(system_online, system_online); result.mode = mode;
        ++system_returns;
        return result;
    }
    szu::compat::Input proxy_config_check(bool proxy, const std::string& tunnel_state, const szu::CancelToken& token) override {
        ++campus_calls;
        while (block_campus && !token.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        szu::compat::Input result;
        result.proxy = proxy ? "on" : "off"; result.tunnel = tunnel_state;
        result.campus_direct = campus_online ? "reachable" : "dns";
        result.campus_system = "reachable";
        result.public_direct = result.public_system = "reachable";
        ++campus_returns;
        return result;
    }
    szu::CampusDiscovery discover(szu::CampusArea, const szu::CancelToken&) override {
        ++discoveries;
        return campus_discovery_confirmed ? szu::CampusDiscovery{szu::Discovery::confirmed, szu::Discovery::confirmed} : szu::CampusDiscovery{};
    }
    szu::LoginAttempt attempt(const szu::Credentials&, szu::CampusArea, const szu::CancelToken&) override { ++submissions; return {}; }
    std::atomic<bool> tunnel{true}, physical_online{false}, system_online{false}, campus_online{false}, campus_discovery_confirmed{false};
    std::atomic<bool> physical_identified{true}, multiple_physical{false};
    std::atomic<bool> link_active{true};
    std::atomic<bool> block_physical{false}, block_system{false}, block_campus{false}, cancelled_report{false};
    std::atomic<int> network_number{1}, physical_calls{0}, physical_returns{0}, system_calls{0}, system_returns{0};
    std::atomic<int> campus_calls{0}, campus_returns{0}, discoveries{0}, submissions{0};
};

inline void round(szu::app::AppController& app, Fake& fake) {
    const int physical = fake.physical_returns + 1, system = fake.system_returns + 1;
    app.check();
    wifiruntime::wait_for([&] { app.on_changed(); return fake.physical_returns >= physical && fake.system_returns >= system &&
        !app.snapshot().busy && !app.system_connectivity_running(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    app.on_changed();
}
inline void campus_check(szu::app::AppController& app, Fake& fake) {
    const int completed = fake.campus_returns + 1;
    app.run_compat(true);
    wifiruntime::wait_for([&] { return fake.campus_returns >= completed && app.compat().done; });
    app.on_changed();
}
inline void system_only_round(szu::app::AppController& app, Fake& fake, unsigned expected_failures) {
    const int completed = fake.system_returns + 1;
    app.check();
    wifiruntime::wait_for([&] { app.on_changed(); return fake.system_returns >= completed && !app.system_connectivity_running() &&
        app.proxy_assistant_suggestion().connection_failures == expected_failures; });
    app.on_changed();
}
}

template<class Check> void run_proxy_assistant_suggestion_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    using proxysuggestionruntime::Fake;
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get(); fake->tunnel = false;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        proxysuggestionruntime::round(app, *fake); proxysuggestionruntime::round(app, *fake);
        proxysuggestionruntime::campus_check(app, *fake); proxysuggestionruntime::campus_check(app, *fake);
        const auto state = app.proxy_assistant_suggestion();
        check(!state.suggested && state.connection_failures == 0 && state.campus_failures == 0,
            "failed connection and campus checks without TUN do not recommend TUN configuration");
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(!app.should_suggest_proxy_assistant(), "TUN detection alone never recommends changing a proxy configuration");
        proxysuggestionruntime::round(app, *fake);
        check(!app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().connection_failures == 1,
            "one completed failed TUN round counts once despite two failed endpoints per column");
        const auto calls = fake->physical_calls.load() + fake->system_calls.load();
        for (int i = 0; i < 20; ++i) { app.on_changed(); (void)app.should_suggest_proxy_assistant(); }
        check(app.proxy_assistant_suggestion().connection_failures == 1 && fake->physical_calls + fake->system_calls == calls,
            "redraws and repeated suggestion reads neither count rounds nor send requests");
        proxysuggestionruntime::round(app, *fake);
        check(app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().connection_failures == 2,
            "two completed failed physical and system TUN rounds recommend the offline assistant");
        proxysuggestionruntime::round(app, *fake);
        check(app.proxy_assistant_suggestion().connection_failures == 2,
            "repeated failures keep the bounded recommendation threshold without accumulating an unbounded counter");
        fake->system_online = true; proxysuggestionruntime::round(app, *fake);
        check(!app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().connection_failures == 0,
            "a working current-system path clears generic offline recommendation even if the physical column fails");
        fake->system_online = false; fake->physical_online = true; proxysuggestionruntime::round(app, *fake);
        check(!app.should_suggest_proxy_assistant(), "a working physical path also suppresses a generic offline recommendation");
        app.shutdown();
    }
    for (const bool identified : {false, true}) {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        fake->physical_identified = identified;
        fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"observe","area":"automatic","probeMode":"quick","appearanceMode":"system"})";
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        if (identified) { proxysuggestionruntime::round(app, *fake); proxysuggestionruntime::round(app, *fake); }
        else { proxysuggestionruntime::system_only_round(app, *fake, 1); proxysuggestionruntime::system_only_round(app, *fake, 2); }
        check(app.should_suggest_proxy_assistant(), "the repeated-failure fixture recommends help before a display-only network recheck");
        const int physical = fake->physical_calls, completed = fake->system_returns + 1;
        fake->system_online = true; app.on_network_signal(); app.on_timer(kTimerNetwork);
        wifiruntime::wait_for([&] { return fake->system_returns >= completed && !app.system_connectivity_running(); }); app.on_changed();
        check(!app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().connection_failures == 0 && fake->physical_calls == physical,
            "a valid display-only system recheck clears generic offline advice without inventing a new physical round");
        app.shutdown();
    }
    for (const bool multiple : {false, true}) {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        fake->physical_identified = multiple; fake->multiple_physical = multiple;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(!app.snapshot().authentication_route && app.proxy_assistant_suggestion().physical_unidentified,
            "unidentified or ambiguous physical attachments remain ineligible for authentication");
        proxysuggestionruntime::system_only_round(app, *fake, 1);
        check(!app.should_suggest_proxy_assistant() && fake->physical_calls == 0,
            "one real failed system sample cannot recommend the assistant or fabricate a physical probe");
        proxysuggestionruntime::system_only_round(app, *fake, 2);
        check(app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().physical_unidentified &&
              fake->physical_calls == 0 && fake->system_calls == 2 && fake->discoveries == 0 && fake->submissions == 0,
            "two actual system failures on a connected TUN path can offer offline help without physical authentication authority");
        fake->system_online = true; proxysuggestionruntime::system_only_round(app, *fake, 0);
        check(!app.should_suggest_proxy_assistant() && !app.snapshot().authentication_route,
            "system success clears unidentified-path help without selecting an attachment or granting trust");
        fake->system_online = false; proxysuggestionruntime::system_only_round(app, *fake, 1);
        fake->block_system = true; const int started = fake->system_calls + 1;
        app.check(); wifiruntime::wait_for([&] { return fake->system_calls >= started; });
        app.cancel(); fake->block_system = false;
        wifiruntime::wait_for([&] { return fake->system_returns >= started; }); app.on_changed();
        check(!app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().connection_failures == 0,
            "cancel also invalidates a late system failure when the physical path is unidentified");
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get(); fake->link_active = false;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        proxysuggestionruntime::system_only_round(app, *fake, 0); proxysuggestionruntime::system_only_round(app, *fake, 0);
        check(!app.should_suggest_proxy_assistant() && fake->system_calls == 2 && fake->physical_calls == 0,
            "a TUN-only snapshot without a connected physical link does not turn ordinary disconnection into proxy advice");
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get(); fake->system_online = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        proxysuggestionruntime::round(app, *fake); proxysuggestionruntime::round(app, *fake);
        check(!app.should_suggest_proxy_assistant(), "system-online physical-offline samples alone do not imply a TUN outage");
        proxysuggestionruntime::campus_check(app, *fake);
        check(!app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().campus_failures == 1,
            "one explicit campus connection failure does not immediately recommend configuration changes");
        proxysuggestionruntime::campus_check(app, *fake);
        check(app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().campus_failures == 2,
            "two measured campus failures can recommend the assistant while ordinary websites remain reachable");
        proxysuggestionruntime::round(app, *fake);
        check(app.should_suggest_proxy_assistant(),
            "ordinary system connectivity does not erase independently repeated campus connection failures");
        fake->campus_online = true; proxysuggestionruntime::campus_check(app, *fake);
        check(!app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().campus_failures == 0,
            "a successful physical campus connection clears the campus recommendation source");
        check(fake->discoveries == 0 && fake->submissions == 0,
            "recommendation sampling does not add discovery or credential submissions");
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        proxysuggestionruntime::round(app, *fake);
        fake->cancelled_report = true; proxysuggestionruntime::round(app, *fake);
        check(!app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().connection_failures == 0,
            "an explicitly cancelled or incomplete physical report cannot complete a failed round");
        fake->cancelled_report = false; proxysuggestionruntime::round(app, *fake);
        fake->block_system = true; const auto started = fake->system_calls + 1;
        app.check(); wifiruntime::wait_for([&] { return fake->system_calls >= started; });
        check(!app.should_suggest_proxy_assistant(), "a second round still waiting for its system column cannot trigger the assistant");
        app.cancel(); fake->block_system = false;
        wifiruntime::wait_for([&] { return fake->system_returns >= started; }); app.on_changed();
        check(!app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().connection_failures == 0,
            "cancel clears consecutive failures and rejects a late completed failure from the cancelled generation");
        proxysuggestionruntime::round(app, *fake);
        check(app.proxy_assistant_suggestion().connection_failures == 1 && !app.should_suggest_proxy_assistant(),
            "a fresh round after cancellation starts a new failure sequence");
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        proxysuggestionruntime::round(app, *fake);
        fake->block_system = true; const auto started = fake->system_calls + 1;
        app.check(); wifiruntime::wait_for([&] { return fake->system_calls >= started; });
        ++fake->network_number; app.on_network_signal(); app.refresh_environment();
        fake->block_system = false; wifiruntime::wait_for([&] { return fake->system_returns >= started; }); app.on_changed();
        check(!app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().connection_failures == 0,
            "a physical network change discards both the prior failure sequence and late system results");
        proxysuggestionruntime::round(app, *fake);
        check(app.proxy_assistant_suggestion().connection_failures == 1, "a different physical binding must collect its own completed rounds");
        proxysuggestionruntime::round(app, *fake);
        check(app.should_suggest_proxy_assistant(), "the second valid failed round on the new binding is eligible");
        fake->tunnel = false; app.refresh_environment();
        check(!app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().connection_failures == 0,
            "closing TUN immediately clears a formerly recommended offline state");
        fake->tunnel = true; app.refresh_environment();
        check(!app.should_suggest_proxy_assistant(), "reopening TUN cannot reuse the previous session's failure threshold");
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        proxysuggestionruntime::campus_check(app, *fake);
        fake->block_campus = true; const auto started = fake->campus_calls + 1;
        app.run_compat(true); wifiruntime::wait_for([&] { return fake->campus_calls >= started; });
        app.cancel_compat(); fake->block_campus = false;
        wifiruntime::wait_for([&] { return fake->campus_returns >= started; }); app.on_changed();
        check(!app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().campus_failures == 0,
            "cancelling a campus check breaks its failure sequence and rejects its late transport result");
        proxysuggestionruntime::campus_check(app, *fake);
        check(!app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().campus_failures == 1,
            "a new campus check after cancellation cannot borrow a stale failure");
        ++fake->network_number; app.refresh_environment();
        check(!app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().campus_failures == 0,
            "campus failure recommendations do not carry across physical networks");
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        NetworkMemory memory("00112233445566778899aabbccddeeff");
        const NetworkDescriptor descriptor{"ethernet", {{"gwip", "192.0.2.1"}, {"net", "192.0.2.0/24"}}};
        memory.remember(descriptor, "teaching", true, false, AppController::wall_now(), "01234567");
        fake->files[L"network-memory.json"] = memory.encode();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        proxysuggestionruntime::campus_check(app, *fake); proxysuggestionruntime::campus_check(app, *fake);
        check(app.should_suggest_proxy_assistant() && app.snapshot().can_verify_campus,
            "repeated campus failures coexist with eligibility for a fresh independent campus verification");
        fake->block_campus = true;
        const int blocked_check = fake->campus_calls + 1;
        app.run_compat(true); wifiruntime::wait_for([&] { return fake->campus_calls >= blocked_check; });
        fake->campus_discovery_confirmed = true;
        check(app.verify_campus_service(), "fresh independent campus verification remains available after repeated helper failures");
        wifiruntime::wait_for([&] { app.on_changed(); return !app.snapshot().busy && app.proxy_assistant_suggestion().campus_failures == 0; });
        check(app.snapshot().required_campus_confirmed && !app.should_suggest_proxy_assistant() && fake->submissions == 0,
            "a new confirmed campus-service discovery clears stale campus advice without submitting credentials");
        fake->block_campus = false;
        wifiruntime::wait_for([&] { return fake->campus_returns >= blocked_check && app.compat().done; }); app.on_changed();
        check(app.proxy_assistant_suggestion().campus_failures == 0 && !app.should_suggest_proxy_assistant(),
            "a helper request started before successful campus verification cannot restore stale failure advice when it returns late");
        proxysuggestionruntime::campus_check(app, *fake); proxysuggestionruntime::campus_check(app, *fake);
        fake->physical_online = fake->system_online = true; proxysuggestionruntime::round(app, *fake);
        check(app.should_suggest_proxy_assistant() && app.proxy_assistant_suggestion().campus_failures == 2,
            "an older campus confirmation and a later public connectivity success do not erase two newer campus helper failures");
        fake->campus_discovery_confirmed = false; app.verify_campus_service();
        wifiruntime::wait_for([&] { app.on_changed(); return !app.snapshot().busy && app.memory_verification_message() == "campus.verify.failed"; });
        check(app.proxy_assistant_suggestion().campus_failures == 2 && app.should_suggest_proxy_assistant(),
            "a newer failed campus verification does not clear independently repeated campus failures");
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"recover","area":"automatic","probeMode":"quick","appearanceMode":"system"})";
        fake->files[L"recovery.json"] = "{\"schema\":1,\"generation\":1,\"notBefore\":" +
            std::to_string(static_cast<long long>(AppController::wall_now() + 90)) + ",\"holds\":[{\"reason\":2,\"origin\":2}]}";
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.pause();
        const auto before = app.snapshot(); const auto before_next = app.next_check(); const auto before_memory = app.remembered_networks();
        proxysuggestionruntime::round(app, *fake); proxysuggestionruntime::round(app, *fake);
        const auto after = app.snapshot();
        check(app.should_suggest_proxy_assistant() && after.paused && !after.trusted &&
              proxyassistantruntime::same_holds(before.holds, after.holds) &&
              proxyassistantruntime::same_memories(before_memory, app.remembered_networks()) && app.next_check() == before_next &&
              after.cooldown_remaining && before.cooldown_remaining && *after.cooldown_remaining <= *before.cooldown_remaining &&
              *after.cooldown_remaining > *before.cooldown_remaining - 5 && fake->submissions == 0 && fake->discoveries == 0,
            "recommendations leave pause holds, trust, memory, cooldown and authentication scheduling untouched");
        app.shutdown();
    }
}
