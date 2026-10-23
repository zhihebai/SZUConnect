// SDK adapter records are injected into the production projection. All radio,
// persistence and account operations use existing isolated runtime fixtures.
#include "szu/win/network_link_testing.hpp"
#include "controller.hpp"
#include "wifi_runtime_tests.hpp"

#include <atomic>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace szu;
using namespace szu::app;
namespace {
int checks = 0, failures = 0;
void check(bool passed, const char* message) {
    ++checks;
    if (!passed) { ++failures; std::printf("FAIL: %s\n", message); }
}

struct NativeAttachment {
    IP_ADAPTER_ADDRESSES adapter{};
    IP_ADAPTER_UNICAST_ADDRESS unicast{};
    IP_ADAPTER_GATEWAY_ADDRESS gateway{};
    sockaddr_in source{}, gateway_address{};
    MIB_IF_ROW2 state{};
    std::wstring description = L"Fixture Ethernet Adapter", label = L"Fixture attachment";
    bool readable = true;
    NativeAttachment() {
        adapter.Length = sizeof adapter; adapter.IfIndex = 7; adapter.Luid.Value = 70;
        adapter.IfType = IF_TYPE_ETHERNET_CSMACD; adapter.OperStatus = IfOperStatusUp;
        adapter.Description = description.data(); adapter.FriendlyName = label.data();
        source.sin_family = AF_INET; source.sin_addr.s_addr = htonl(0xc0000207u);
        gateway_address.sin_family = AF_INET; gateway_address.sin_addr.s_addr = htonl(0xc0000201u);
        unicast.Address.lpSockaddr = reinterpret_cast<sockaddr*>(&source); unicast.Address.iSockaddrLength = sizeof source;
        unicast.DadState = IpDadStatePreferred; unicast.ValidLifetime = 1000; unicast.OnLinkPrefixLength = 24;
        gateway.Address.lpSockaddr = reinterpret_cast<sockaddr*>(&gateway_address); gateway.Address.iSockaddrLength = sizeof gateway_address;
        adapter.FirstUnicastAddress = &unicast; adapter.FirstGatewayAddress = &gateway;
        state.InterfaceLuid = adapter.Luid; state.InterfaceIndex = adapter.IfIndex;
        state.OperStatus = IfOperStatusUp; state.MediaConnectState = MediaConnectStateConnected;
    }
    NativeAttachment(const NativeAttachment&) = delete;
    NativeAttachment& operator=(const NativeAttachment&) = delete;
    win::NetworkSnapshot project(bool virtual_default = true) const {
        return win::network_link_projection_for_testing(&adapter, [&](MIB_IF_ROW2& result) -> NETIO_STATUS {
            if (!readable) return ERROR_NOT_FOUND;
            result = state; return NO_ERROR;
        }, virtual_default);
    }
};

class FixtureRadio final : public wifi::Radio {
public:
    wifi::PhysicalState physical_state() override { return wifi::PhysicalState::disconnected; }
    std::optional<bool> power_state() override { return true; }
    wifi::Result power_on(const CancelToken&) override { ++power_calls; return wifi::Result::poweredOn; }
    wifi::ScanResult scan(const std::string& name, const CancelToken&) override {
        ++scan_calls; requested = name;
        return {wifi::Result::notFound, {{name, "fixture-radio", 80, !secure, secure, secure}}};
    }
    wifi::Result associate_open(const wifi::ScanCandidate& candidate, const CancelToken&) override {
        ++associations; associated = candidate.ssid; return wifi::Result::joined;
    }
    wifi::Result associate_profile(const wifi::ScanCandidate& candidate, const CancelToken&) override {
        ++profile_associations; ++associations; associated = candidate.ssid; return wifi::Result::joined;
    }
    int scan_calls = 0, associations = 0, power_calls = 0;
    int profile_associations = 0;
    bool secure = false;
    std::string requested, associated;
};
class ProjectionRuntime final : public wifiruntime::Fake {
public:
    explicit ProjectionRuntime(win::NetworkSnapshot projected) : snapshot(std::move(projected)) {
        wifi::Preferences preferences; preferences.auto_join = true;
        files[L"wifi-recovery.json"] = *preferences.encode();
    }
    win::NetworkSnapshot capture_network() override { return snapshot; }
    wifi::Result recover_wifi(const std::vector<wifi::Network>& networks, bool power, const CancelToken& token) override {
        ++evidence().fake_wifi_calls;
        const auto result = wifi::recover(radio, networks, power, token);
        completed = true; return result;
    }
    win::NetworkSnapshot snapshot;
    FixtureRadio radio;
    std::atomic<bool> completed{false};
};

void controller_gate(win::NetworkSnapshot snapshot, bool expected_join) {
    auto services = std::make_unique<ProjectionRuntime>(std::move(snapshot)); auto* fake = services.get();
    const auto memory = fake->files[L"network-memory.json"], preferences = fake->files[L"wifi-recovery.json"];
    AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
    check(app.wifi_candidates().size() == 1 && app.wifi_selected(fake->id),
        "native projection fixture keeps one explicitly available remembered Wi-Fi target");
    app.on_timer(kTimerWiFi);
    if (expected_join) {
        wifiruntime::wait_for([&] { return fake->completed && app.wifi_status() == "joined"; });
        app.on_changed();
        check(fake->evidence().fake_wifi_calls == 1 && fake->radio.scan_calls == 1 && fake->radio.associations == 1 &&
            fake->radio.requested == "SZU-fixture" && fake->radio.associated == "SZU-fixture" && app.wifi_status() == "joined",
            "software-only native link projection reaches Controller scheduling and the real Wi-Fi recovery gate with one fake association");
    } else {
        app.on_changed();
        check(fake->evidence().fake_wifi_calls == 0 && fake->radio.scan_calls == 0 && fake->radio.associations == 0 &&
            app.wifi_status() == "connected", "real or conservatively unknown physical attachment prevents Wi-Fi scanning and takeover");
    }
    check(!app.snapshot().trusted && !app.snapshot().remembered && !app.snapshot().authentication_route &&
        fake->files[L"network-memory.json"] == memory && fake->files[L"wifi-recovery.json"] == preferences &&
        fake->evidence().fake_auth_submissions == 0 && fake->evidence().network_calls == 0 && fake->radio.power_calls == 0,
        "link projection and optional finding never create campus authorization, rewrite memory or issue real traffic");
    app.shutdown();
    check(fake->evidence().formal_services_created == 0 && fake->evidence().production_storage_accesses == 0 &&
        fake->evidence().vault_accesses == 0 && fake->evidence().watchers_started == 0,
        "native-record Controller regression uses no actual storage, credentials, watchers or WLAN device");
}

class RecoveryPipelineRuntime final : public wifiruntime::Fake {
public:
    explicit RecoveryPipelineRuntime(win::NetworkSnapshot initial, bool selected, bool secure)
        : disconnected(std::move(initial)) {
        radio.secure = secure;
        files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"recover","area":"automatic","probeMode":"quick","appearanceMode":"system"})";
        wifi::Preferences preferences; preferences.auto_join = true;
        if (!selected) preferences.excluded.insert(id);
        files[L"wifi-recovery.json"] = *preferences.encode();
    }
    static physical::Route connected_route() {
        physical::Route result;
        result.interface_key = "fixture-recovered-radio"; result.interface_index = 7; result.interface_luid = 70;
        result.source_ipv4 = "192.0.2.7"; result.addresses = {result.source_ipv4}; result.dns_servers = {"192.0.2.53"};
        result.gateway_ipv4 = "192.0.2.1"; result.identity = "fixture-recovered-physical-lease";
        result.display_name = "Fixture recovered Wi-Fi"; result.is_system_primary = true;
        result.descriptor = {"wifi", {{"gwip", "192.0.2.1"}, {"net", "192.0.2.0/24"}}};
        return result;
    }
    win::NetworkSnapshot capture_network() override {
        ++captures;
        if (!associated) return disconnected;
        // Association completion stands in for the OS event. The new strict
        // candidate is injected, not captured from this computer's adapters.
        auto result = disconnected; const auto route = connected_route();
        result.access = "wifi"; result.path.link_active = result.path.usable = true;
        result.path.identity = route.identity; result.path.descriptors = {route.descriptor};
        result.path.physical_candidates = {route};
        return result;
    }
    bool set_expected_physical_route(const std::optional<physical::Route>& route) override {
        return associated && route && *route == connected_route();
    }
    win::CredentialVault::ReadStatus read_credentials(Credentials& credentials) override {
        credentials = {"fixture-user", "fixture-password"}; return win::CredentialVault::ReadStatus::found;
    }
    wifi::Result recover_wifi(const std::vector<wifi::Network>& networks, bool power, const CancelToken& token) override {
        ++evidence().fake_wifi_calls;
        const auto result = wifi::recover(radio, networks, power, token);
        if (result == wifi::Result::joined) associated = true;
        return result;
    }
    ConnectivityReport probe(ProbeMode mode, const CancelToken&) override {
        ++probes; auto result = ConnectivityReport::simple(submitted, submitted); result.mode = mode;
        if (!submitted) {
            result.apple.state = result.microsoft.state = EndpointState::redirect;
            result.apple.campus_area_hint = result.microsoft.campus_area_hint = CampusPortalHint::teaching;
        }
        return result;
    }
    CampusDiscovery discover(CampusArea, const CancelToken&) override {
        ++discoveries; CampusDiscovery result{Discovery::confirmed, Discovery::confirmed};
        result.teaching_client_bound = true; return result;
    }
    LoginAttempt attempt(const Credentials& credentials, CampusArea area, const CancelToken&) override {
        ++evidence().fake_auth_submissions;
        fake_credentials = credentials.account == "fixture-user" && credentials.password == "fixture-password";
        chosen = area; submitted = true;
        LoginAttempt result; result.outcome = Outcome::success; result.credential_submission_started = true; return result;
    }
    win::NetworkSnapshot disconnected;
    FixtureRadio radio;
    std::atomic<int> captures{0}, probes{0}, discoveries{0};
    std::atomic<bool> associated{false}, submitted{false}, fake_credentials{false};
    CampusArea chosen = CampusArea::automatic;
};

void recovery_pipeline(const win::NetworkSnapshot& disconnected, bool read_only, bool selected, bool secure = false) {
    auto services = std::make_unique<RecoveryPipelineRuntime>(disconnected, selected, secure); auto* fake = services.get();
    const auto memory = fake->files[L"network-memory.json"], preferences = fake->files[L"wifi-recovery.json"];
    AppController app(nullptr, read_only ? RuntimeMode::observeOnly : RuntimeMode::normal, std::move(services)); app.start();
    check(!app.network().path.link_active && app.network().path.tunnel && !app.snapshot().authentication_route,
        "recovery pipeline starts with a projected software TUN and no physical Wi-Fi attachment");
    app.on_timer(kTimerWiFi);
    if (read_only || !selected) {
        const int captures = fake->captures;
        app.on_changed(); app.on_timer(kTimerAuto);
        wifiruntime::wait_for([&] { return fake->captures > captures; });
        app.shutdown();
        check(fake->captures > captures && fake->evidence().fake_wifi_calls == 0 && fake->radio.scan_calls == 0 && fake->radio.associations == 0 &&
            fake->probes == 0 && fake->discoveries == 0 && fake->evidence().fake_auth_submissions == 0 && !fake->associated,
            "read-only mode or an excluded Wi-Fi target cannot enter the association-to-authentication pipeline");
    } else {
        wifiruntime::wait_for([&] { return fake->associated && app.wifi_status() == "joined"; });
        check(fake->evidence().fake_wifi_calls == 1 && fake->radio.associations == 1 &&
            fake->radio.profile_associations == (secure ? 1 : 0) && fake->radio.associated == "SZU-fixture" &&
            !app.snapshot().authentication_route && fake->probes == 0 && fake->evidence().fake_auth_submissions == 0,
            "a successful fake association alone cannot create a physical binding or admit credentials");
        // Deliver the network signal that a real successful association posts.
        app.on_network_signal(); app.on_timer(kTimerNetwork); app.on_changed();
        check(app.snapshot().authentication_route == std::optional<physical::Route>(RecoveryPipelineRuntime::connected_route()) &&
            app.snapshot().remembered && !app.snapshot().trusted && fake->evidence().fake_auth_submissions == 0,
            "the following capture rematches the selected Wi-Fi's saved physical evidence before automatic recovery");
        for (int round = 1; round <= 2; ++round) {
            app.on_timer(kTimerAuto);
            wifiruntime::wait_for([&] { return fake->probes >= (round == 1 ? 1 : 3) && !app.snapshot().busy; });
            app.on_changed();
            if (round == 1) check(app.snapshot().status == EngineStatus::confirming && fake->probes == 1 &&
                fake->discoveries == 0 && fake->evidence().fake_auth_submissions == 0,
                "the first failed physical round with a campus hint still cannot skip failure confirmation or submit credentials");
        }
        check(app.snapshot().online && fake->probes == 3 && fake->discoveries == 1 && fake->evidence().fake_auth_submissions == 1 &&
            fake->fake_credentials && fake->chosen == CampusArea::teaching && fake->radio.associations == 1 &&
            app.snapshot().cooldown_remaining && app.snapshot().holds.empty(),
            "association plus fresh physical failure confirmation and independent campus proof reaches one fake teaching submission and successful complete recheck");
        app.shutdown();
    }
    check(fake->files[L"network-memory.json"] == memory && fake->files[L"wifi-recovery.json"] == preferences &&
        fake->evidence().network_calls == 0 && fake->evidence().production_storage_accesses == 0 &&
        fake->evidence().vault_accesses == 0 && fake->evidence().formal_services_created == 0 &&
        fake->evidence().watchers_started == 0 && fake->radio.power_calls == 0,
        "the full recovery pipeline preserves saved choices using fake account, services and radio only");
}
}

int main() {
    for (const std::string kind : {"filter", "endpoint", "tunnel-medium", "loopback-access", "disconnected", "not-media-connected", "down"}) {
        NativeAttachment fixture;
        if (kind == "filter") fixture.state.InterfaceAndOperStatusFlags.FilterInterface = 1;
        else if (kind == "endpoint") fixture.state.InterfaceAndOperStatusFlags.EndPointInterface = 1;
        else if (kind == "tunnel-medium") fixture.state.MediaType = NdisMediumTunnel;
        else if (kind == "loopback-access") fixture.state.AccessType = NET_IF_ACCESS_LOOPBACK;
        else if (kind == "disconnected") fixture.state.MediaConnectState = MediaConnectStateDisconnected;
        else if (kind == "not-media-connected") fixture.state.InterfaceAndOperStatusFlags.NotMediaConnected = 1;
        else fixture.state.OperStatus = IfOperStatusDown;
        const auto snapshot = fixture.project();
        check(!snapshot.path.link_active && !snapshot.other_physical_link_active && snapshot.access == "none" &&
            snapshot.path.tunnel && snapshot.path.physical_candidates.empty() && snapshot.path.descriptors.empty(),
            "UP/type-6 name alone cannot turn confirmed software or disconnected SDK evidence into a physical link");
        controller_gate(snapshot, true);
    }
    {
        NativeAttachment fixture; fixture.description = L"Fixture Wintun Adapter"; fixture.adapter.Description = fixture.description.data();
        fixture.state.InterfaceAndOperStatusFlags.HardwareInterface = 1;
        const auto snapshot = fixture.project();
        check(!snapshot.path.link_active && !snapshot.other_physical_link_active && snapshot.access == "none",
            "known virtual names remain excluded even if contradictory hardware flags are present");
        controller_gate(snapshot, true);
    }
    for (const bool wireless : {false, true}) {
        NativeAttachment fixture; fixture.adapter.IfType = wireless ? IF_TYPE_IEEE80211 : IF_TYPE_ETHERNET_CSMACD;
        fixture.adapter.FirstUnicastAddress = nullptr; fixture.adapter.FirstGatewayAddress = nullptr;
        fixture.state.InterfaceAndOperStatusFlags.HardwareInterface = 1;
        const auto snapshot = fixture.project();
        check(snapshot.path.link_active && snapshot.path.physical_candidates.empty(),
            "confirmed connected Ethernet or WLAN hardware remains active while acquiring its first address and gateway");
        controller_gate(snapshot, false);
    }
    {
        NativeAttachment fixture; fixture.adapter.FirstUnicastAddress = nullptr; fixture.adapter.FirstGatewayAddress = nullptr;
        fixture.state.InterfaceAndOperStatusFlags.ConnectorPresent = 1;
        const auto snapshot = fixture.project();
        check(snapshot.path.link_active, "a confirmed connected hardware connector is protected before obtaining an address");
        controller_gate(snapshot, false);
    }
    for (const bool readable : {false, true}) {
        NativeAttachment fixture; fixture.readable = readable;
        const auto snapshot = fixture.project();
        check(snapshot.path.link_active && snapshot.access == "ethernet" && snapshot.path.physical_candidates.empty(),
            "unclassified usable source is conservatively preserved with unreadable or inconclusive hardware evidence");
        controller_gate(snapshot, false);
    }
    {
        NativeAttachment fixture; fixture.state.InterfaceLuid.Value = 999;
        fixture.state.MediaConnectState = MediaConnectStateDisconnected;
        const auto snapshot = fixture.project();
        check(snapshot.path.link_active, "mismatched MIB identity cannot revoke a different adapter's usable source");
        controller_gate(snapshot, false);
    }
    for (const std::string kind : {"unreadable", "mismatched", "unknown-media-hardware", "unknown-media-unclassified"}) {
        NativeAttachment fixture; fixture.adapter.FirstUnicastAddress = nullptr; fixture.adapter.FirstGatewayAddress = nullptr;
        if (kind == "unreadable") fixture.readable = false;
        else if (kind == "mismatched") fixture.state.InterfaceLuid.Value = 999;
        else {
            fixture.state.MediaConnectState = MediaConnectStateUnknown;
            fixture.state.InterfaceAndOperStatusFlags.HardwareInterface = kind == "unknown-media-hardware" ? 1 : 0;
        }
        const auto snapshot = fixture.project();
        check(snapshot.path.link_active && snapshot.path.physical_candidates.empty(),
            "UP physical classification with no source and unreadable, mixed or unknown media evidence remains protected from takeover");
        controller_gate(snapshot, false);
    }
    {
        NativeAttachment fixture; fixture.state.InterfaceAndOperStatusFlags.EndPointInterface = 1;
        const auto disconnected = fixture.project();
        recovery_pipeline(disconnected, false, true);
        recovery_pipeline(disconnected, true, true);
        recovery_pipeline(disconnected, false, false);
        recovery_pipeline(disconnected, false, true, true);
        recovery_pipeline(disconnected, true, true, true);
        recovery_pipeline(disconnected, false, false, true);
    }
    std::printf("Native network link projection: %d checks, %d failures; all radio operations injected\n", checks, failures);
    return failures ? 1 : 0;
}
