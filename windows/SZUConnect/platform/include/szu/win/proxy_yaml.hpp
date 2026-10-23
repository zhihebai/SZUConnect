#pragma once

#include "szu/proxy_assistant.hpp"

namespace szu::win {

// Transforms a standalone Mihomo configuration in memory; performs no I/O.
proxyassist::Patch patch_mihomo_yaml(std::string_view text);

}  // namespace szu::win
