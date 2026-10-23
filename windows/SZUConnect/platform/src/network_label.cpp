#include "szu/win/network.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <roapi.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <cstring>
#endif

namespace szu::win {
std::optional<std::string> connected_ssid(std::uint64_t interface_luid) {
#if defined(_WIN32)
    // GetConnectedSsid is Microsoft's SSID-only alternative to location-gated WLAN APIs.
    // Never scan, request access or fall back to a different network profile.
    NET_LUID luid{};
    luid.Value = interface_luid;
    GUID guid{};
    if (ConvertInterfaceLuidToGuid(&luid, &guid) != NO_ERROR) return std::nullopt;
    const HRESULT initialized = RoInitialize(RO_INIT_SINGLETHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return std::nullopt;
    std::optional<std::string> result;
    try {
        using namespace winrt::Windows::Networking::Connectivity;
        for (const auto& profile : NetworkInformation::GetConnectionProfiles()) {
            const auto adapter = profile.NetworkAdapter();
            if (!adapter || !profile.IsWlanConnectionProfile() ||
                profile.GetNetworkConnectivityLevel() == NetworkConnectivityLevel::None) continue;
            const auto adapter_id = adapter.NetworkAdapterId();
            if (std::memcmp(&adapter_id, &guid, sizeof guid) != 0) continue;
            const auto details = profile.WlanConnectionProfileDetails();
            if (details) result = NetworkLabels::valid_ssid(winrt::to_string(details.GetConnectedSsid()));
            break;
        }
    } catch (const winrt::hresult_error&) { /* Metadata unavailable; no logs or permission requests. */ }
    if (SUCCEEDED(initialized)) RoUninitialize();
    return result;
#else
    // Non-Windows syntax checks do not validate the real SDK branch above.
    (void)interface_luid;
    return std::nullopt;
#endif
}
}  // namespace szu::win
