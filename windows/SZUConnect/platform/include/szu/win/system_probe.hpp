// Independent display probes use the system proxy and never provide campus identity or protocol hints.
#pragma once
#include <functional>
#include <optional>
#include "szu/win/http.hpp"

namespace szu::win {
using SystemProbeGet = std::function<HttpResponse(const HttpRequest&, const CancelToken&)>;
std::optional<ConnectivityReport> probe_system_connectivity(ProbeMode, const CancelToken&, const SystemProbeGet&);
}  // namespace szu::win
