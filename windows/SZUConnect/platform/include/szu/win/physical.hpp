// Read-only physical-network capture and source/interface-bound DNS.
#pragma once
#include "szu/physical.hpp"

namespace szu { class CancelToken; }
namespace szu::win {

std::vector<physical::Route> capture_physical_candidates();
bool physical_route_current(const physical::Route& route);
std::vector<std::string> resolve_physical_a(const std::string& host, const physical::Route& route,
                                          int timeout_ms, const CancelToken& token);

}  // namespace szu::win
