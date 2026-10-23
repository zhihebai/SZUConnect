#include "szu/win/wifi_recovery.hpp"
#include "szu/win/text.hpp"
#include "szu/win/network.hpp"
#ifdef SZU_WIFI_TESTING
#include "szu/win/wifi_recovery_testing.hpp"
#endif
#include <wlanapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#pragma comment(lib, "wlanapi.lib")
namespace szu::win {
namespace {
wifi::Result failure(DWORD status) {
    return status == ERROR_ACCESS_DENIED ? wifi::Result::permissionRequired : wifi::Result::failed;
}
struct ScanWait {
    HANDLE handle;
    GUID guid;
    std::atomic<int> completion{0}; // 1 complete, 2 failed
    bool registered = false;
    std::function<void()> unregister;
    static void WINAPI callback(WLAN_NOTIFICATION_DATA* data, void* context) {
        auto* self = static_cast<ScanWait*>(context);
        if (!self || !data || !(data->NotificationSource & WLAN_NOTIFICATION_SOURCE_ACM) ||
            std::memcmp(&data->InterfaceGuid, &self->guid, sizeof self->guid) != 0) return;
        if (data->NotificationCode == wlan_notification_acm_scan_complete) self->completion = 1;
        else if (data->NotificationCode == wlan_notification_acm_scan_fail) self->completion = 2;
    }
    ~ScanWait() {
        // Unregister on this worker, outside the callback. Windows waits for any callback to
        // return before releasing the context; no shared lock can deadlock that wait.
        if (registered && unregister) unregister();
    }
};
class NativeRadio final : public wifi::Radio {
public:
    NativeRadio() = default;
#ifdef SZU_WIFI_TESTING
    NativeRadio(const GUID& guid, WifiNativeTestHooks hooks)
        : handle_(reinterpret_cast<HANDLE>(1)), guid_(guid), test_hooks_(std::move(hooks)) {}
#endif
    ~NativeRadio() override {
#ifdef SZU_WIFI_TESTING
        if (test_hooks_) return;
#endif
        if (handle_) WlanCloseHandle(handle_, nullptr);
    }
    wifi::PhysicalState physical_state() override {
#ifdef SZU_WIFI_TESTING
        if (test_hooks_) return test_hooks_->physical_state ? test_hooks_->physical_state() : wifi::PhysicalState::unknown;
#endif
        // Share the platform's physical/TUN classification. Reading names is explicitly disabled,
        // so this gate cannot request location permission or confuse a virtual Ethernet with Wi-Fi.
        const auto snapshot = capture_network(false);
        if (snapshot.access == "unknown") return wifi::PhysicalState::unknown;
        if (snapshot.path.link_active || snapshot.other_physical_link_active) return wifi::PhysicalState::connected;
        // Association may be active before an IP address/link appears in IP Helper.
        if (!open()) return wifi::PhysicalState::unknown;
        WLAN_INTERFACE_INFO_LIST* interfaces = nullptr;
        if (WlanEnumInterfaces(handle_, nullptr, &interfaces) != ERROR_SUCCESS || !interfaces) return wifi::PhysicalState::unknown;
        auto state = wifi::PhysicalState::disconnected;
        for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
            const auto current = interfaces->InterfaceInfo[i].isState;
            if (current == wlan_interface_state_connected || current == wlan_interface_state_associating ||
                current == wlan_interface_state_authenticating || current == wlan_interface_state_disconnecting ||
                current == wlan_interface_state_discovering) state = wifi::PhysicalState::connected;
            else if (current != wlan_interface_state_disconnected && current != wlan_interface_state_not_ready)
                state = wifi::PhysicalState::unknown;
        }
        WlanFreeMemory(interfaces); return state;
    }
    std::optional<bool> power_state() override {
#ifdef SZU_WIFI_TESTING
        if (test_hooks_) return test_hooks_->power_state ? test_hooks_->power_state() : std::nullopt;
#endif
        if (!open()) return std::nullopt;
        WLAN_INTERFACE_INFO_LIST* interfaces = nullptr;
        if (WlanEnumInterfaces(handle_, nullptr, &interfaces) != ERROR_SUCCESS || !interfaces) return std::nullopt;
        bool found = false;
        for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i)
            if (interfaces->InterfaceInfo[i].isState == wlan_interface_state_disconnected ||
                interfaces->InterfaceInfo[i].isState == wlan_interface_state_not_ready) {
                guid_ = interfaces->InterfaceInfo[i].InterfaceGuid; found = true; break;
            }
        WlanFreeMemory(interfaces);
        if (!found) return std::nullopt;
        DWORD size = 0; void* raw = nullptr;
        const auto rc = WlanQueryInterface(handle_, &guid_, wlan_intf_opcode_radio_state, nullptr, &size, &raw, nullptr);
        if (rc != ERROR_SUCCESS || !raw || size < sizeof(WLAN_RADIO_STATE)) { if (raw) WlanFreeMemory(raw); return std::nullopt; }
        const auto* radios = static_cast<WLAN_RADIO_STATE*>(raw);
        bool on = false; phys_.clear();
        for (DWORD i = 0; i < std::min<DWORD>(radios->dwNumberOfPhys, WLAN_MAX_PHY_INDEX); ++i) {
            phys_.push_back(radios->PhyRadioState[i].dwPhyIndex);
            on |= radios->PhyRadioState[i].dot11SoftwareRadioState == dot11_radio_state_on &&
                radios->PhyRadioState[i].dot11HardwareRadioState == dot11_radio_state_on;
        }
        WlanFreeMemory(raw); return phys_.empty() ? std::nullopt : std::optional<bool>(on);
    }
    wifi::Result power_on(const CancelToken& token) override {
#ifdef SZU_WIFI_TESTING
        if (test_hooks_) return wifi::Result::unavailable;
#endif
        if (!handle_ || phys_.empty()) return wifi::Result::unavailable;
        for (const auto index : phys_) {
            if (token.cancelled()) return wifi::Result::cancelled;
            if (physical_state() != wifi::PhysicalState::disconnected) return wifi::Result::connectionUnclear;
            WLAN_PHY_RADIO_STATE state{}; state.dwPhyIndex = index; state.dot11SoftwareRadioState = dot11_radio_state_on;
            if (token.cancelled()) return wifi::Result::cancelled;
            const auto rc = WlanSetInterface(handle_, &guid_, wlan_intf_opcode_radio_state, sizeof state, &state, nullptr);
            if (rc != ERROR_SUCCESS) return failure(rc);
        }
        const auto powered = power_state();
        return powered && *powered ? wifi::Result::poweredOn : wifi::Result::wifiOff;
    }
    wifi::ScanResult scan(const std::string& ssid, const CancelToken& token) override {
        wifi::ScanResult out;
        candidates_.clear();
        if (token.cancelled()) { out.result = wifi::Result::cancelled; return out; }
        if (!handle_ || !NetworkLabels::valid_ssid(ssid)) { out.result = wifi::Result::unavailable; return out; }
        ScanWait wait{handle_, guid_};
        wait.unregister = [this] { register_notification(WLAN_NOTIFICATION_SOURCE_NONE, nullptr, nullptr); };
        const auto registered = register_notification(WLAN_NOTIFICATION_SOURCE_ACM, ScanWait::callback, &wait);
        if (registered != ERROR_SUCCESS) { out.result = failure(registered); return out; }
        wait.registered = true;
        DOT11_SSID target{}; target.uSSIDLength = static_cast<ULONG>(ssid.size()); std::memcpy(target.ucSSID, ssid.data(), ssid.size());
        if (token.cancelled()) { out.result = wifi::Result::cancelled; return out; }
        const auto rc = scan_target(target);
        if (token.cancelled()) { out.result = wifi::Result::cancelled; return out; }
        if (rc != ERROR_SUCCESS) { out.result = failure(rc); return out; }
        // Microsoft specifies scan_complete notification or a four-second timeout. Wake as
        // soon as the scan finishes, without a fixed delay on every target.
        for (int i = 0; i < 40 && !wait.completion && !token.cancelled(); ++i) wait_step(token);
        if (token.cancelled()) { out.result = wifi::Result::cancelled; return out; }
        if (wait.completion == 2) { out.result = wifi::Result::failed; return out; }
        WLAN_AVAILABLE_NETWORK_LIST* list = nullptr;
        const auto listed = available_networks(&list);
        if (token.cancelled()) { if (list) free_memory(list); out.result = wifi::Result::cancelled; return out; }
        if (listed != ERROR_SUCCESS || !list) { if (list) free_memory(list); out.result = failure(listed); return out; }
        if (list->dwNumberOfItems > 4096) { free_memory(list); out.result = wifi::Result::failed; return out; }
        for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
            const auto& network = list->Network[i];
            if (network.dot11Ssid.uSSIDLength != ssid.size() || std::memcmp(network.dot11Ssid.ucSSID, ssid.data(), ssid.size()) != 0) continue;
            if (network.dot11BssType != dot11_BSS_type_infrastructure) continue;
            const bool is_open = network.dot11BssType == dot11_BSS_type_infrastructure && !network.bSecurityEnabled &&
                network.dot11DefaultAuthAlgorithm == DOT11_AUTH_ALGO_80211_OPEN && network.dot11DefaultCipherAlgorithm == DOT11_CIPHER_ALGO_NONE;
            const bool protected_network = network.bSecurityEnabled || !is_open;
            const auto profile = profile_name(network.strProfileName);
            const bool saved = network.dot11BssType == dot11_BSS_type_infrastructure && network.bNetworkConnectable && network.bSecurityEnabled &&
                (network.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) && profile.has_value() &&
                !(network.dot11DefaultAuthAlgorithm == DOT11_AUTH_ALGO_80211_OPEN && network.dot11DefaultCipherAlgorithm == DOT11_CIPHER_ALGO_NONE);
            std::string key;
            if ((is_open && network.bNetworkConnectable) || saved) {
                key = "wlan-candidate-" + std::to_string(++candidate_sequence_);
                candidates_.emplace(key, Candidate{guid_, ssid, saved ? *profile : std::wstring{}, network.dot11DefaultAuthAlgorithm,
                    network.dot11DefaultCipherAlgorithm, saved});
            }
            out.networks.push_back({ssid, key, static_cast<int>(std::min<ULONG>(network.wlanSignalQuality, 100)),
                is_open && network.bNetworkConnectable, saved, protected_network});
        }
        free_memory(list);
        if (token.cancelled()) { candidates_.clear(); out.networks.clear(); out.result = wifi::Result::cancelled; }
        return out;
    }
    wifi::Result associate_open(const wifi::ScanCandidate& candidate, const CancelToken& token) override {
        if (!candidate.open || candidate.saved_profile || candidate.protected_network) return wifi::Result::cancelled;
        return associate(candidate, token, false);
    }
    wifi::Result associate_profile(const wifi::ScanCandidate& candidate, const CancelToken& token) override {
        if (candidate.open || !candidate.saved_profile || !candidate.protected_network) return wifi::Result::cancelled;
        return associate(candidate, token, true);
    }
private:
    struct Candidate {
        GUID guid;
        std::string ssid;
        std::wstring profile;
        DOT11_AUTH_ALGORITHM auth;
        DOT11_CIPHER_ALGORITHM cipher;
        bool saved_profile;
    };
    static std::optional<std::wstring> profile_name(const wchar_t* text) {
        const auto end = std::find(text, text + WLAN_MAX_NAME_LENGTH, L'\0');
        if (end == text || end == text + WLAN_MAX_NAME_LENGTH) return std::nullopt;
        return std::wstring(text, end);
    }
    static bool ssid_matches(const DOT11_SSID& actual, const std::string& expected) {
        return actual.uSSIDLength == expected.size() && expected.size() <= DOT11_SSID_MAX_LENGTH &&
            std::memcmp(actual.ucSSID, expected.data(), expected.size()) == 0;
    }
    static bool same_candidate(const WLAN_AVAILABLE_NETWORK& actual, const Candidate& expected) {
        if (!ssid_matches(actual.dot11Ssid, expected.ssid) || actual.dot11BssType != dot11_BSS_type_infrastructure ||
            !actual.bNetworkConnectable || actual.dot11DefaultAuthAlgorithm != expected.auth || actual.dot11DefaultCipherAlgorithm != expected.cipher ||
            static_cast<bool>(actual.bSecurityEnabled) != expected.saved_profile) return false;
        return !expected.saved_profile || ((actual.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) &&
            profile_name(actual.strProfileName) == std::optional<std::wstring>(expected.profile));
    }
    std::optional<wifi::Result> admission(const CancelToken& token) {
        if (token.cancelled()) return wifi::Result::cancelled;
        const auto physical = physical_state();
        if (token.cancelled()) return wifi::Result::cancelled;
        if (physical == wifi::PhysicalState::connected) return wifi::Result::alreadyConnected;
        if (physical != wifi::PhysicalState::disconnected) return wifi::Result::connectionUnclear;
        return std::nullopt;
    }
    wifi::Result associate(const wifi::ScanCandidate& selected, const CancelToken& token, bool saved) {
        if (!handle_ || token.cancelled() || !NetworkLabels::valid_ssid(selected.ssid)) return wifi::Result::cancelled;
        const auto cached = candidates_.find(selected.interface_key);
        if (cached == candidates_.end() || cached->second.ssid != selected.ssid || cached->second.saved_profile != saved ||
            std::memcmp(&cached->second.guid, &guid_, sizeof guid_) != 0) return wifi::Result::cancelled;
        const Candidate candidate = cached->second;
        if (const auto denied = admission(token)) return *denied;
        WLAN_AVAILABLE_NETWORK_LIST* list = nullptr;
        const auto listed = available_networks(&list);
        if (token.cancelled()) { if (list) free_memory(list); return wifi::Result::cancelled; }
        if (listed != ERROR_SUCCESS || !list) { if (list) free_memory(list); return failure(listed); }
        bool matches = false, protected_seen = false;
        if (list->dwNumberOfItems <= 4096)
            for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
                const auto& network = list->Network[i];
                matches = matches || same_candidate(network, candidate);
                protected_seen = protected_seen || (ssid_matches(network.dot11Ssid, candidate.ssid) &&
                    network.dot11BssType == dot11_BSS_type_infrastructure && (network.bSecurityEnabled ||
                    network.dot11DefaultAuthAlgorithm != DOT11_AUTH_ALGO_80211_OPEN || network.dot11DefaultCipherAlgorithm != DOT11_CIPHER_ALGO_NONE));
            }
        free_memory(list);
        if (!matches || (!saved && protected_seen)) return wifi::Result::systemConnectionRequired;
        // Existing profiles stay under Windows' credential/security policy. The
        // separate OPEN path still uses a temporary manual configuration only.
        const auto profile = saved ? std::optional<std::string>{} : wifi::temporary_open_profile(candidate.ssid);
        if (!saved && !profile) return wifi::Result::failed;
        const auto xml = profile ? widen(*profile) : std::wstring{};
        DOT11_SSID target{}; target.uSSIDLength = static_cast<ULONG>(candidate.ssid.size());
        std::memcpy(target.ucSSID, candidate.ssid.data(), candidate.ssid.size());
        WLAN_CONNECTION_PARAMETERS parameters{};
        parameters.wlanConnectionMode = saved ? wlan_connection_mode_profile : wlan_connection_mode_temporary_profile;
        parameters.strProfile = saved ? candidate.profile.c_str() : xml.c_str();
        parameters.pDot11Ssid = &target; parameters.dot11BssType = dot11_BSS_type_infrastructure;
        if (const auto denied = admission(token)) return *denied;
        if (token.cancelled()) return wifi::Result::cancelled;
        const auto rc = connect(parameters);
        if (token.cancelled()) return wifi::Result::cancelled;
        if (rc != ERROR_SUCCESS) return failure(rc);
        // An accepted request is not yet an established connection. Never claim it joined early.
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (token.cancelled()) return wifi::Result::cancelled;
            WLAN_INTERFACE_INFO_LIST* interfaces = nullptr;
            const auto enumerated = enum_interfaces(&interfaces);
            if (token.cancelled()) { if (interfaces) free_memory(interfaces); return wifi::Result::cancelled; }
            if (enumerated != ERROR_SUCCESS || !interfaces) { if (interfaces) free_memory(interfaces); return failure(enumerated); }
            if (interfaces->dwNumberOfItems > 256) { free_memory(interfaces); return wifi::Result::failed; }
            bool joined = false;
            bool other_connection = false;
            for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
                const auto& info = interfaces->InterfaceInfo[i];
                if (std::memcmp(&info.InterfaceGuid, &guid_, sizeof guid_) == 0)
                    joined = info.isState == wlan_interface_state_connected;
                else if (info.isState != wlan_interface_state_disconnected && info.isState != wlan_interface_state_not_ready)
                    other_connection = true;
            }
            free_memory(interfaces);
            if (other_connection) return wifi::Result::connectionUnclear;
            if (joined) return verify_connection(candidate, token);
            wait_step(token);
        }
        return wifi::Result::failed;
    }
    wifi::Result verify_connection(const Candidate& candidate, const CancelToken& token) {
        if (token.cancelled()) return wifi::Result::cancelled;
        DWORD size = 0; void* raw = nullptr;
        const auto queried = current_connection(&size, &raw);
        if (token.cancelled()) { if (raw) free_memory(raw); return wifi::Result::cancelled; }
        if (queried != ERROR_SUCCESS || !raw || size < sizeof(WLAN_CONNECTION_ATTRIBUTES)) {
            if (raw) free_memory(raw);
            return queried == ERROR_SUCCESS ? wifi::Result::connectionUnclear : failure(queried);
        }
        const auto& actual = *static_cast<const WLAN_CONNECTION_ATTRIBUTES*>(raw);
        const bool matches = actual.isState == wlan_interface_state_connected &&
            ssid_matches(actual.wlanAssociationAttributes.dot11Ssid, candidate.ssid) &&
            actual.wlanAssociationAttributes.dot11BssType == dot11_BSS_type_infrastructure &&
            static_cast<bool>(actual.wlanSecurityAttributes.bSecurityEnabled) == candidate.saved_profile &&
            actual.wlanSecurityAttributes.dot11AuthAlgorithm == candidate.auth &&
            actual.wlanSecurityAttributes.dot11CipherAlgorithm == candidate.cipher &&
            (!candidate.saved_profile || (actual.wlanConnectionMode == wlan_connection_mode_profile &&
                profile_name(actual.strProfileName) == std::optional<std::wstring>(candidate.profile)));
        free_memory(raw);
        if (token.cancelled()) return wifi::Result::cancelled;
        return matches ? wifi::Result::joined : wifi::Result::connectionUnclear;
    }
    DWORD register_notification(DWORD source, WLAN_NOTIFICATION_CALLBACK callback, void* context) {
#ifdef SZU_WIFI_TESTING
        if (test_hooks_) return test_hooks_->register_notification ? test_hooks_->register_notification(source, callback, context) : ERROR_NOT_SUPPORTED;
#endif
        return WlanRegisterNotification(handle_, source, TRUE, callback, context, nullptr, nullptr);
    }
    DWORD scan_target(DOT11_SSID target) {
#ifdef SZU_WIFI_TESTING
        if (test_hooks_) return test_hooks_->scan ? test_hooks_->scan(guid_, target) : ERROR_NOT_SUPPORTED;
#endif
        return WlanScan(handle_, &guid_, &target, nullptr, nullptr);
    }
    DWORD available_networks(WLAN_AVAILABLE_NETWORK_LIST** list) {
#ifdef SZU_WIFI_TESTING
        if (test_hooks_) return test_hooks_->available_networks ? test_hooks_->available_networks(guid_, list) : ERROR_NOT_SUPPORTED;
#endif
        return WlanGetAvailableNetworkList(handle_, &guid_, 0, nullptr, list);
    }
    DWORD connect(WLAN_CONNECTION_PARAMETERS parameters) {
#ifdef SZU_WIFI_TESTING
        if (test_hooks_) return test_hooks_->connect ? test_hooks_->connect(guid_, parameters) : ERROR_NOT_SUPPORTED;
#endif
        return WlanConnect(handle_, &guid_, &parameters, nullptr);
    }
    DWORD enum_interfaces(WLAN_INTERFACE_INFO_LIST** interfaces) {
#ifdef SZU_WIFI_TESTING
        if (test_hooks_) return test_hooks_->interfaces ? test_hooks_->interfaces(interfaces) : ERROR_NOT_SUPPORTED;
#endif
        return WlanEnumInterfaces(handle_, nullptr, interfaces);
    }
    DWORD current_connection(DWORD* size, void** attributes) {
#ifdef SZU_WIFI_TESTING
        if (test_hooks_) return test_hooks_->current_connection ? test_hooks_->current_connection(guid_, size, attributes) : ERROR_NOT_SUPPORTED;
#endif
        return WlanQueryInterface(handle_, &guid_, wlan_intf_opcode_current_connection, nullptr, size, attributes, nullptr);
    }
    void free_memory(void* memory) {
#ifdef SZU_WIFI_TESTING
        if (test_hooks_) { if (test_hooks_->free_memory) test_hooks_->free_memory(memory); return; }
#endif
        WlanFreeMemory(memory);
    }
    void wait_step(const CancelToken& token) {
#ifdef SZU_WIFI_TESTING
        if (test_hooks_) { if (test_hooks_->wait) test_hooks_->wait(token); return; }
#endif
        token.wait_for(0.1);
    }
    bool open() {
        if (handle_) return true;
        DWORD version = 0;
        return WlanOpenHandle(2, nullptr, &version, &handle_) == ERROR_SUCCESS && handle_;
    }
    HANDLE handle_ = nullptr;
    GUID guid_{};
    std::vector<DWORD> phys_;
    std::map<std::string, Candidate> candidates_;
    unsigned long long candidate_sequence_ = 0;
#ifdef SZU_WIFI_TESTING
    std::optional<WifiNativeTestHooks> test_hooks_;
#endif
};
}
#ifdef SZU_WIFI_TESTING
std::unique_ptr<wifi::Radio> make_wifi_native_radio_for_testing(const GUID& guid, WifiNativeTestHooks hooks) {
    return std::make_unique<NativeRadio>(guid, std::move(hooks));
}
#endif
wifi::Result recover_wifi(const std::vector<wifi::Network>& networks, bool power, const CancelToken& token) {
    NativeRadio radio; return wifi::recover(radio, networks, power, token);
}
}
