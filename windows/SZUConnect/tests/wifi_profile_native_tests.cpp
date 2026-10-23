// Production NativeRadio with SDK metadata and every OS operation injected.
// No WLAN handle, scan, connection, saved credential or profile content is read.
#include "szu/win/wifi_recovery_testing.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace szu;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}
void set_name(wchar_t* output, const std::wstring& value) {
    std::fill(output, output + WLAN_MAX_NAME_LENGTH, L'\0');
    std::copy_n(value.begin(), std::min<std::size_t>(value.size(), WLAN_MAX_NAME_LENGTH - 1), output);
}
void set_ssid(DOT11_SSID& output, const std::string& value) {
    output = {}; output.uSSIDLength = static_cast<ULONG>(value.size());
    std::memcpy(output.ucSSID, value.data(), value.size());
}
struct Fixture {
    const std::string ssid = "SZU-fixture-secure";
    const std::wstring profile = L"Fixture saved campus";
    GUID guid{70, 1, 2, {1, 2, 3, 4, 5, 6, 7, 8}};
    std::vector<WLAN_AVAILABLE_NETWORK> rows;
    WLAN_CONNECTION_ATTRIBUTES current{};
    WLAN_NOTIFICATION_CALLBACK callback = nullptr;
    void* context = nullptr;
    DWORD list_error = ERROR_SUCCESS, connect_error = ERROR_SUCCESS, query_error = ERROR_SUCCESS, enum_error = ERROR_SUCCESS;
    bool other_connected = false, accepted_pending = false;
    int scans = 0, lists = 0, connects = 0, queries = 0, gate_calls = 0, waits = 0, registers = 0;
    bool exact_guid = true, exact_ssid = true;
    WLAN_CONNECTION_MODE captured_mode{};
    DOT11_BSS_TYPE captured_bss{};
    DWORD captured_flags = 999;
    bool captured_bssid_list = false;
    std::wstring captured_profile;
    std::string captured_ssid;
    std::set<void*> allocations;
    std::function<void(int)> before_list;
    std::function<wifi::PhysicalState(int)> gate;
    std::function<void()> during_query, during_connect, during_wait;

    Fixture() { rows = {secure()}; current_from(rows.front()); }
    ~Fixture() { for (void* memory : allocations) std::free(memory); }
    WLAN_AVAILABLE_NETWORK secure(const std::wstring& name = L"Fixture saved campus") const {
        WLAN_AVAILABLE_NETWORK row{}; set_name(row.strProfileName, name); set_ssid(row.dot11Ssid, ssid);
        row.dot11BssType = dot11_BSS_type_infrastructure; row.bNetworkConnectable = TRUE; row.wlanSignalQuality = 60;
        row.bSecurityEnabled = TRUE; row.dot11DefaultAuthAlgorithm = DOT11_AUTH_ALGO_RSNA_PSK;
        row.dot11DefaultCipherAlgorithm = DOT11_CIPHER_ALGO_CCMP; row.dwFlags = WLAN_AVAILABLE_NETWORK_HAS_PROFILE; return row;
    }
    WLAN_AVAILABLE_NETWORK open_row() const {
        auto row = secure(); set_name(row.strProfileName, L""); row.dwFlags = 0; row.wlanSignalQuality = 99;
        row.bSecurityEnabled = FALSE; row.dot11DefaultAuthAlgorithm = DOT11_AUTH_ALGO_80211_OPEN;
        row.dot11DefaultCipherAlgorithm = DOT11_CIPHER_ALGO_NONE; return row;
    }
    void current_from(const WLAN_AVAILABLE_NETWORK& row) {
        current = {}; current.isState = wlan_interface_state_connected;
        current.wlanConnectionMode = row.bSecurityEnabled ? wlan_connection_mode_profile : wlan_connection_mode_temporary_profile;
        std::copy_n(row.strProfileName, WLAN_MAX_NAME_LENGTH, current.strProfileName);
        current.wlanAssociationAttributes.dot11Ssid = row.dot11Ssid;
        current.wlanAssociationAttributes.dot11BssType = row.dot11BssType;
        current.wlanSecurityAttributes.bSecurityEnabled = row.bSecurityEnabled;
        current.wlanSecurityAttributes.dot11AuthAlgorithm = row.dot11DefaultAuthAlgorithm;
        current.wlanSecurityAttributes.dot11CipherAlgorithm = row.dot11DefaultCipherAlgorithm;
    }
    void* allocate(std::size_t size) {
        void* memory = std::calloc(1, size);
        if (!memory) throw std::bad_alloc();
        allocations.insert(memory); return memory;
    }
    win::WifiNativeTestHooks hooks() {
        win::WifiNativeTestHooks hooks;
        hooks.physical_state = [this] { ++gate_calls; return gate ? gate(gate_calls) : wifi::PhysicalState::disconnected; };
        hooks.power_state = [] { return std::optional<bool>(true); };
        hooks.register_notification = [this](DWORD source, WLAN_NOTIFICATION_CALLBACK handler, void* opaque) -> DWORD {
            ++registers; callback = source == WLAN_NOTIFICATION_SOURCE_NONE ? nullptr : handler;
            context = opaque; return ERROR_SUCCESS;
        };
        hooks.scan = [this](const GUID& selected, const DOT11_SSID& target) -> DWORD {
            ++scans; exact_guid &= std::memcmp(&selected, &guid, sizeof guid) == 0;
            exact_ssid &= target.uSSIDLength == ssid.size() && std::memcmp(target.ucSSID, ssid.data(), ssid.size()) == 0;
            if (callback) {
                WLAN_NOTIFICATION_DATA message{}; message.NotificationSource = WLAN_NOTIFICATION_SOURCE_ACM;
                message.NotificationCode = wlan_notification_acm_scan_complete; message.InterfaceGuid = selected;
                callback(&message, context);
            }
            return ERROR_SUCCESS;
        };
        hooks.available_networks = [this](const GUID& selected, WLAN_AVAILABLE_NETWORK_LIST** output) -> DWORD {
            exact_guid &= std::memcmp(&selected, &guid, sizeof guid) == 0;
            ++lists; if (before_list) before_list(lists);
            if (list_error != ERROR_SUCCESS) return list_error;
            const auto bytes = std::max(sizeof(WLAN_AVAILABLE_NETWORK_LIST), offsetof(WLAN_AVAILABLE_NETWORK_LIST, Network) + rows.size() * sizeof(WLAN_AVAILABLE_NETWORK));
            auto* list = static_cast<WLAN_AVAILABLE_NETWORK_LIST*>(allocate(bytes)); list->dwNumberOfItems = static_cast<DWORD>(rows.size());
            for (std::size_t i = 0; i < rows.size(); ++i) list->Network[i] = rows[i];
            *output = list; return ERROR_SUCCESS;
        };
        hooks.connect = [this](const GUID& selected, const WLAN_CONNECTION_PARAMETERS& plan) -> DWORD {
            ++connects; exact_guid &= std::memcmp(&selected, &guid, sizeof guid) == 0;
            captured_mode = plan.wlanConnectionMode; captured_bss = plan.dot11BssType;
            captured_flags = plan.dwFlags; captured_bssid_list = plan.pDesiredBssidList != nullptr;
            captured_profile = plan.strProfile ? plan.strProfile : L"";
            if (plan.pDot11Ssid && plan.pDot11Ssid->uSSIDLength <= DOT11_SSID_MAX_LENGTH)
                captured_ssid.assign(reinterpret_cast<const char*>(plan.pDot11Ssid->ucSSID), plan.pDot11Ssid->uSSIDLength);
            if (during_connect) during_connect();
            return connect_error;
        };
        hooks.interfaces = [this](WLAN_INTERFACE_INFO_LIST** output) -> DWORD {
            if (enum_error != ERROR_SUCCESS) return enum_error;
            const std::size_t count = other_connected ? 2 : 1;
            const auto bytes = offsetof(WLAN_INTERFACE_INFO_LIST, InterfaceInfo) + count * sizeof(WLAN_INTERFACE_INFO);
            auto* list = static_cast<WLAN_INTERFACE_INFO_LIST*>(allocate(bytes)); list->dwNumberOfItems = static_cast<DWORD>(count);
            list->InterfaceInfo[0].InterfaceGuid = guid;
            list->InterfaceInfo[0].isState = accepted_pending ? wlan_interface_state_associating : wlan_interface_state_connected;
            if (other_connected) { list->InterfaceInfo[1].InterfaceGuid.Data1 = 999; list->InterfaceInfo[1].isState = wlan_interface_state_connected; }
            *output = list; return ERROR_SUCCESS;
        };
        hooks.current_connection = [this](const GUID& selected, DWORD* size, void** output) -> DWORD {
            ++queries; exact_guid &= std::memcmp(&selected, &guid, sizeof guid) == 0;
            if (during_query) during_query();
            if (query_error != ERROR_SUCCESS) return query_error;
            auto* result = static_cast<WLAN_CONNECTION_ATTRIBUTES*>(allocate(sizeof current)); *result = current;
            *size = sizeof current; *output = result; return ERROR_SUCCESS;
        };
        hooks.free_memory = [this](void* memory) { if (allocations.erase(memory)) std::free(memory); else check(false, "each injected SDK allocation is freed exactly once"); };
        hooks.wait = [this](const CancelToken&) { ++waits; if (during_wait) during_wait(); };
        return hooks;
    }
    std::unique_ptr<wifi::Radio> radio() { return win::make_wifi_native_radio_for_testing(guid, hooks()); }
    wifi::ScanCandidate selected(wifi::Radio& radio, const CancelToken& token) {
        const auto found = radio.scan(ssid, token);
        check(found.networks.size() == 1 && found.networks.front().saved_profile && found.networks.front().protected_network && !found.networks.front().open,
            "visible protected saved-profile metadata becomes an opaque secure candidate");
        return found.networks.empty() ? wifi::ScanCandidate{} : found.networks.front();
    }
};
}

int main() {
    {
        Fixture f; auto radio = f.radio(); CancelToken token; const auto candidate = f.selected(*radio, token);
        check(candidate.interface_key != f.ssid && candidate.interface_key.find("Fixture") == std::string::npos,
            "profile and SSID values are not used as the candidate's opaque lookup key");
        check(radio->associate_profile(candidate, token) == wifi::Result::joined && f.connects == 1 && f.lists == 2 && f.queries == 1,
            "exact scan metadata is revalidated before one saved-profile request and its actual connection verification");
        check(f.captured_mode == wlan_connection_mode_profile && f.captured_profile == f.profile && f.captured_ssid == f.ssid &&
            f.captured_flags == 0 && !f.captured_bssid_list && f.captured_bss == dot11_BSS_type_infrastructure && f.exact_guid && f.exact_ssid,
            "WlanConnect uses the exact saved profile, selected GUID and explicit SSID with zero policy-changing flags");
        check(f.gate_calls == 2 && f.allocations.empty(), "both physical gates run and every injected SDK result is released");
    }
    for (const std::string change : {"deleted", "renamed", "name-case", "auth", "cipher", "security", "has-profile", "connectable"}) {
        Fixture f; auto radio = f.radio(); CancelToken token; const auto candidate = f.selected(*radio, token);
        f.before_list = [&](int call) {
            if (call != 2) return;
            if (change == "deleted") f.rows.clear();
            else if (change == "renamed") set_name(f.rows[0].strProfileName, L"Renamed fixture profile");
            else if (change == "name-case") set_name(f.rows[0].strProfileName, L"fixture saved campus");
            else if (change == "auth") f.rows[0].dot11DefaultAuthAlgorithm = DOT11_AUTH_ALGO_RSNA;
            else if (change == "cipher") f.rows[0].dot11DefaultCipherAlgorithm = DOT11_CIPHER_ALGO_TKIP;
            else if (change == "security") f.rows[0].bSecurityEnabled = FALSE;
            else if (change == "has-profile") f.rows[0].dwFlags = 0;
            else f.rows[0].bNetworkConnectable = FALSE;
        };
        check(radio->associate_profile(candidate, token) == wifi::Result::systemConnectionRequired && f.connects == 0 && f.allocations.empty(),
            "deleted, renamed or changed saved-network metadata stops before WlanConnect without falling back to OPEN");
    }
    for (const std::string missing : {"flag", "empty", "unterminated", "unconnectable", "non-infrastructure"}) {
        Fixture f;
        if (missing == "flag") f.rows[0].dwFlags = 0;
        else if (missing == "empty") set_name(f.rows[0].strProfileName, L"");
        else if (missing == "unterminated") std::fill_n(f.rows[0].strProfileName, WLAN_MAX_NAME_LENGTH, L'x');
        else if (missing == "unconnectable") f.rows[0].bNetworkConnectable = FALSE;
        else f.rows[0].dot11BssType = dot11_BSS_type_independent;
        auto radio = f.radio(); CancelToken token; const auto result = radio->scan(f.ssid, token);
        check((result.networks.empty() || !result.networks.front().saved_profile) && f.connects == 0 && f.allocations.empty(),
            "only connectable infrastructure rows with a bounded nonempty saved-profile name are eligible");
    }
    {
        Fixture f; f.rows = {f.secure(L"First fixture profile"), f.secure(L"Second fixture profile")};
        f.current_from(f.rows[1]); auto radio = f.radio(); CancelToken token; const auto result = radio->scan(f.ssid, token);
        check(result.networks.size() == 2 && result.networks[0].interface_key != result.networks[1].interface_key,
            "same-SSID saved profiles retain distinct opaque cache entries");
        check(result.networks.size() == 2 && radio->associate_profile(result.networks[1], token) == wifi::Result::joined &&
            f.captured_profile == L"Second fixture profile", "association uses the selected metadata profile instead of guessing from the SSID");
    }
    for (const std::string stop : {"cancel-before", "cancel-after-list", "cancel-final-gate", "other-physical", "unknown-physical", "permission"}) {
        Fixture f; auto radio = f.radio(); CancelToken token; const auto candidate = f.selected(*radio, token);
        wifi::Result expected = wifi::Result::cancelled;
        if (stop == "cancel-before") token.cancel();
        else if (stop == "cancel-after-list") f.before_list = [&](int call) { if (call == 2) token.cancel(); };
        else if (stop == "cancel-final-gate") f.gate = [&](int call) { if (call == 2) token.cancel(); return wifi::PhysicalState::disconnected; };
        else if (stop == "other-physical") { expected = wifi::Result::alreadyConnected; f.gate = [](int) { return wifi::PhysicalState::connected; }; }
        else if (stop == "unknown-physical") { expected = wifi::Result::connectionUnclear; f.gate = [](int) { return wifi::PhysicalState::unknown; }; }
        else { expected = wifi::Result::permissionRequired; f.list_error = ERROR_ACCESS_DENIED; }
        check(radio->associate_profile(candidate, token) == expected && f.connects == 0 && f.allocations.empty(),
            "cancellation, existing physical connections and metadata permission failures reject a saved-profile request before submission");
    }
    for (const std::string change : {"ssid", "profile", "security", "auth", "cipher", "other-interface", "query-permission", "connect-permission", "late-cancel", "connect-cancelled-permission", "accepted-only"}) {
        Fixture f; auto radio = f.radio(); CancelToken token; const auto candidate = f.selected(*radio, token);
        wifi::Result expected = wifi::Result::connectionUnclear;
        if (change == "ssid") set_ssid(f.current.wlanAssociationAttributes.dot11Ssid, "Other-fixture-WiFi");
        else if (change == "profile") set_name(f.current.strProfileName, L"Other fixture profile");
        else if (change == "security") f.current.wlanSecurityAttributes.bSecurityEnabled = FALSE;
        else if (change == "auth") f.current.wlanSecurityAttributes.dot11AuthAlgorithm = DOT11_AUTH_ALGO_RSNA;
        else if (change == "cipher") f.current.wlanSecurityAttributes.dot11CipherAlgorithm = DOT11_CIPHER_ALGO_TKIP;
        else if (change == "other-interface") f.other_connected = true;
        else if (change == "query-permission") { expected = wifi::Result::permissionRequired; f.query_error = ERROR_ACCESS_DENIED; }
        else if (change == "connect-permission") { expected = wifi::Result::permissionRequired; f.connect_error = ERROR_ACCESS_DENIED; }
        else if (change == "late-cancel") { expected = wifi::Result::cancelled; f.during_query = [&] { token.cancel(); }; }
        else if (change == "connect-cancelled-permission") {
            expected = wifi::Result::cancelled; f.connect_error = ERROR_ACCESS_DENIED; f.during_connect = [&] { token.cancel(); };
        }
        else { expected = wifi::Result::failed; f.accepted_pending = true; }
        check(radio->associate_profile(candidate, token) == expected && f.connects == 1 && f.allocations.empty(),
            "accepted requests, foreign connections, security changes, permission failures and late success cannot be reported as a verified join");
    }
    {
        Fixture f; auto radio = f.radio(); CancelToken token; const auto old = f.selected(*radio, token);
        (void)radio->scan(f.ssid, token);
        check(radio->associate_profile(old, token) == wifi::Result::cancelled && f.connects == 0,
            "a candidate from a previous scan cannot be replayed through the new cache generation");
    }
    for (const bool saved : {true, false}) {
        Fixture f; auto secure = f.secure(); if (!saved) secure.dwFlags = 0;
        f.rows = {f.open_row(), secure}; auto radio = f.radio(); CancelToken token;
        const auto result = wifi::recover(*radio, {{"fixture-id", f.ssid, "Fixture campus"}}, false, token);
        check((saved ? result == wifi::Result::joined && f.connects == 1 && f.captured_mode == wlan_connection_mode_profile
                     : result == wifi::Result::systemConnectionRequired && f.connects == 0) && f.allocations.empty(),
            "the actual recovery selector prefers a protected saved profile over stronger OPEN and never downgrades an unsaved protected target");
    }
    {
        Fixture f; f.rows = {f.open_row()}; f.current_from(f.rows[0]);
        auto radio = f.radio(); CancelToken token; const auto result = radio->scan(f.ssid, token);
        check(result.networks.size() == 1 && result.networks[0].open && !result.networks[0].saved_profile &&
            radio->associate_open(result.networks[0], token) == wifi::Result::joined && f.captured_mode == wlan_connection_mode_temporary_profile &&
            f.captured_ssid == f.ssid && f.captured_flags == 0, "OPEN/NONE recovery keeps its temporary manual profile and verifies the actual target");
    }
    {
        Fixture f; f.rows = {f.open_row()}; auto radio = f.radio(); CancelToken token; const auto result = radio->scan(f.ssid, token);
        f.rows.push_back(f.secure());
        check(result.networks.size() == 1 && radio->associate_open(result.networks[0], token) == wifi::Result::systemConnectionRequired && f.connects == 0,
            "a protected network discovered during final revalidation prevents a cached OPEN fallback in the same round");
    }
    {
        Fixture f; CancelToken token; token.cancel(); auto radio = f.radio();
        check(radio->scan(f.ssid, token).result == wifi::Result::cancelled && f.scans == 0 && f.lists == 0,
            "pre-cancelled scanning never calls an SDK operation");
        auto empty = win::make_wifi_native_radio_for_testing(f.guid, {});
        check(empty->physical_state() == wifi::PhysicalState::unknown && !empty->power_state() &&
            empty->scan(f.ssid, CancelToken{}).result == wifi::Result::failed,
            "missing injected operations fail closed without constructing a real WLAN handle or falling through to native calls");
    }
    std::printf("Native saved Wi-Fi profile recovery: %d checks, %d failures; no OS WLAN actions\n", checks, failures);
    return failures ? 1 : 0;
}
