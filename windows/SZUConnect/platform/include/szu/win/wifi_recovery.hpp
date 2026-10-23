#pragma once
#include "szu/wifi_recovery.hpp"
namespace szu::win {
// Explicit opt-in only. Reads visible network/profile-name metadata and lets Windows
// reconnect an existing profile; never reads profile content/passwords or changes profiles,
// disconnects an interface, elevates privileges, or changes VPN/DNS/routes.
wifi::Result recover_wifi(const std::vector<wifi::Network>&, bool allow_power_on, const CancelToken&);
}
