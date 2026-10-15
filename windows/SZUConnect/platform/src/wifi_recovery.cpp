#include "szu/win/wifi_recovery.hpp"
#include "szu/win/text.hpp"
#include "szu/win/network.hpp"
#include <wlanapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

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
        if (registered) WlanRegisterNotification(handle, WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);
    }
};
class NativeRadio final : public wifi::Radio {
public:
    ~NativeRadio() override { if (handle_) WlanCloseHandle(handle_, nullptr); }
    wifi::PhysicalState physical_state() override {
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
        if (!handle_ || !NetworkLabels::valid_ssid(ssid)) { out.result = wifi::Result::unavailable; return out; }
        ScanWait wait{handle_, guid_};
        const auto registered = WlanRegisterNotification(handle_, WLAN_NOTIFICATION_SOURCE_ACM, TRUE, ScanWait::callback, &wait, nullptr, nullptr);
        if (registered != ERROR_SUCCESS) { out.result = failure(registered); return out; }
        wait.registered = true;
        DOT11_SSID target{}; target.uSSIDLength = static_cast<ULONG>(ssid.size()); std::memcpy(target.ucSSID, ssid.data(), ssid.size());
        const auto rc = WlanScan(handle_, &guid_, &target, nullptr, nullptr);
        if (rc != ERROR_SUCCESS) { out.result = failure(rc); return out; }
        // Microsoft specifies scan_complete notification or a four-second timeout. Wake as
        // soon as the scan finishes, without a fixed delay on every target.
        for (int i = 0; i < 40 && !wait.completion && !token.cancelled(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (token.cancelled()) { out.result = wifi::Result::cancelled; return out; }
        if (wait.completion == 2) { out.result = wifi::Result::failed; return out; }
        WLAN_AVAILABLE_NETWORK_LIST* list = nullptr;
        const auto listed = WlanGetAvailableNetworkList(handle_, &guid_, 0, nullptr, &list);
        if (listed != ERROR_SUCCESS || !list) { out.result = failure(listed); return out; }
        for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
            const auto& network = list->Network[i];
            if (network.dot11Ssid.uSSIDLength != ssid.size() || std::memcmp(network.dot11Ssid.ucSSID, ssid.data(), ssid.size()) != 0) continue;
            const bool is_open = network.dot11BssType == dot11_BSS_type_infrastructure && !network.bSecurityEnabled &&
                network.dot11DefaultAuthAlgorithm == DOT11_AUTH_ALGO_80211_OPEN && network.dot11DefaultCipherAlgorithm == DOT11_CIPHER_ALGO_NONE;
            out.networks.push_back({ssid, "selected-wlan", static_cast<int>(network.wlanSignalQuality), is_open && network.bNetworkConnectable});
        }
        WlanFreeMemory(list); return out;
    }
    wifi::Result associate_open(const wifi::ScanCandidate& candidate, const CancelToken& token) override {
        if (!handle_ || token.cancelled() || !candidate.open || candidate.interface_key != "selected-wlan" || !NetworkLabels::valid_ssid(candidate.ssid))
            return wifi::Result::cancelled;
        const auto physical = physical_state();
        if (physical == wifi::PhysicalState::connected) return wifi::Result::alreadyConnected;
        if (physical != wifi::PhysicalState::disconnected) return wifi::Result::connectionUnclear;
        // Temporary, manual, OPEN/NONE only: no saved profile, password or credential lookup.
        const auto profile = wifi::temporary_open_profile(candidate.ssid);
        if (!profile) return wifi::Result::failed;
        const auto xml = widen(*profile);
        WLAN_CONNECTION_PARAMETERS parameters{}; parameters.wlanConnectionMode = wlan_connection_mode_temporary_profile;
        parameters.strProfile = xml.c_str(); parameters.dot11BssType = dot11_BSS_type_infrastructure;
        const auto final_physical = physical_state();
        if (final_physical == wifi::PhysicalState::connected) return wifi::Result::alreadyConnected;
        if (final_physical != wifi::PhysicalState::disconnected) return wifi::Result::connectionUnclear;
        if (token.cancelled()) return wifi::Result::cancelled;
        const auto rc = WlanConnect(handle_, &guid_, &parameters, nullptr);
        if (rc != ERROR_SUCCESS) return failure(rc);
        // An accepted request is not yet an established connection. Never claim it joined early.
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (token.cancelled()) return wifi::Result::cancelled;
            WLAN_INTERFACE_INFO_LIST* interfaces = nullptr;
            if (WlanEnumInterfaces(handle_, nullptr, &interfaces) != ERROR_SUCCESS || !interfaces) return wifi::Result::failed;
            bool joined = false;
            for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i)
                joined |= std::memcmp(&interfaces->InterfaceInfo[i].InterfaceGuid, &guid_, sizeof guid_) == 0 &&
                    interfaces->InterfaceInfo[i].isState == wlan_interface_state_connected;
            WlanFreeMemory(interfaces);
            if (joined) return wifi::Result::joined;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return wifi::Result::failed;
    }
private:
    bool open() {
        if (handle_) return true;
        DWORD version = 0;
        return WlanOpenHandle(2, nullptr, &version, &handle_) == ERROR_SUCCESS && handle_;
    }
    HANDLE handle_ = nullptr;
    GUID guid_{};
    std::vector<DWORD> phys_;
};
}
wifi::Result recover_wifi(const std::vector<wifi::Network>& networks, bool power, const CancelToken& token) {
    NativeRadio radio; return wifi::recover(radio, networks, power, token);
}
}
