#pragma once
#include <optional>
#include <string>
#include <string_view>
#include "szu/crash_diagnostics.hpp"
#include "szu/engine.hpp"

namespace szu::win {
// Temporary event fields are matched locally; no system text or path is exported.
struct CrashEvent {
    std::wstring provider, app_name, app_path, module_name, module_path;
    unsigned event_id = 0;
    double time = 0;
    std::string version, fault_code, module_offset;
};
std::optional<crash::Record> filter_crash_event(const CrashEvent& event,
    std::wstring_view executable, double now);
crash::Report collect_crash_diagnostics(const CancelToken& token);
}  // namespace szu::win
