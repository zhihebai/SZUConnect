#pragma once
#include "szu/wifi_recovery.hpp"
namespace szu::win {
// Only invoked by the normal runtime after an explicit opt-in. Does not read profiles/passwords,
// save a profile, disconnect an interface, elevate privileges, or change VPN/DNS/routes.
wifi::Result recover_wifi(const std::vector<wifi::Network>&, bool allow_power_on, const CancelToken&);
}
