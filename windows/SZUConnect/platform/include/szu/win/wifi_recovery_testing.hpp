#pragma once
#ifdef SZU_WIFI_TESTING
#include <winsock2.h>
#include <windows.h>
#include <wlanapi.h>
#include <functional>
#include <memory>
#include <optional>
#include "szu/wifi_recovery.hpp"

namespace szu::win {
struct WifiNativeTestHooks {
    std::function<wifi::PhysicalState()> physical_state;
    std::function<std::optional<bool>()> power_state;
    std::function<DWORD(DWORD, WLAN_NOTIFICATION_CALLBACK, void*)> register_notification;
    std::function<DWORD(const GUID&, const DOT11_SSID&)> scan;
    std::function<DWORD(const GUID&, WLAN_AVAILABLE_NETWORK_LIST**)> available_networks;
    std::function<DWORD(const GUID&, const WLAN_CONNECTION_PARAMETERS&)> connect;
    std::function<DWORD(WLAN_INTERFACE_INFO_LIST**)> interfaces;
    std::function<DWORD(const GUID&, DWORD*, void**)> current_connection;
    std::function<void(void*)> free_memory;
    std::function<void(const CancelToken&)> wait;
};
// No native handle is opened. Every operation is injected; missing hooks fail closed.
std::unique_ptr<wifi::Radio> make_wifi_native_radio_for_testing(const GUID&, WifiNativeTestHooks);
}
#endif
