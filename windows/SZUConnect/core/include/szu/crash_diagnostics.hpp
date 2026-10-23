#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "szu/json.hpp"

namespace szu::crash {
inline constexpr std::size_t kMaximumRecords = 5;
inline constexpr double kLookbackSeconds = 7 * 24 * 3600;
inline constexpr std::uint64_t kMaximumModuleOffset = 256 * 1024 * 1024;

enum class Status { notCollected, found, none, accessDenied, unavailable, cancelled, partial };
std::string_view to_string(Status status);

struct Record {
    double time = 0;
    std::string version;
    std::uint32_t fault_code = 0;
    std::optional<std::uint64_t> module_offset;
};
struct Report {
    Status status = Status::notCollected;
    double collected_at = 0;
    std::vector<Record> records;
};

// OS paths are checked by the platform collector and never enter this model.
std::optional<Record> parse_record(double collected_at, double time, std::string_view version,
    std::string_view fault_code, std::optional<std::string_view> own_module_offset = std::nullopt);
Report anonymous_report(const Report& input);
json::Value to_json(const Report& input);
}  // namespace szu::crash
