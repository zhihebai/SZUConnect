#include "szu/crash_diagnostics.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <tuple>
#include <utility>

namespace szu::records { std::string iso8601(double unix_seconds); }
namespace szu::crash {
namespace {
bool valid_time(double time) { return std::isfinite(time) && time >= 946684800 && time < 4102444800.0; }
bool valid_version(std::string_view version) {
    if (version.empty() || version.size() > 23) return false;
    std::size_t start = 0, components = 0;
    for (;;) {
        const auto end = version.find('.', start);
        const auto part = version.substr(start, end == std::string_view::npos ? end : end - start);
        if (part.empty() || part.size() > 5 || ++components > 4) return false;
        unsigned value = 0;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || value > 65535) return false;
        if (end == std::string_view::npos) return true;
        start = end + 1;
    }
}
std::optional<std::uint64_t> hex_number(std::string_view text, std::size_t digits) {
    if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
    if (text.empty() || text.size() > digits) return std::nullopt;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}
bool valid_record(const Record& record, double now) {
    return valid_time(record.time) && record.time <= now && now - record.time <= kLookbackSeconds &&
        valid_version(record.version) && record.fault_code != 0;
}
}

std::string_view to_string(Status status) {
    switch (status) {
        case Status::notCollected: return "notCollected";
        case Status::found: return "found";
        case Status::none: return "none";
        case Status::accessDenied: return "accessDenied";
        case Status::cancelled: return "cancelled";
        case Status::partial: return "partial";
        case Status::unavailable: return "unavailable";
    }
    return "unavailable";
}

std::optional<Record> parse_record(double collected_at, double time, std::string_view version,
                                  std::string_view fault_code, std::optional<std::string_view> own_module_offset) {
    const auto code = hex_number(fault_code, 8);
    if (!valid_time(collected_at) || !code || !*code) return std::nullopt;
    Record record{time, std::string(version), static_cast<std::uint32_t>(*code), std::nullopt};
    if (!valid_record(record, collected_at)) return std::nullopt;
    if (own_module_offset) {
        const auto offset = hex_number(*own_module_offset, 16);
        if (offset && *offset <= kMaximumModuleOffset) record.module_offset = *offset;
    }
    return record;
}

Report anonymous_report(const Report& input) {
    Report result;
    result.status = input.status;
    if (to_string(input.status) == "unavailable") result.status = Status::unavailable;
    if (valid_time(input.collected_at)) result.collected_at = input.collected_at;
    if (result.status != Status::found && result.status != Status::partial) return result;
    if (!result.collected_at) { result.status = Status::unavailable; return result; }
    bool filtered = false;
    for (std::size_t i = 0; i < input.records.size() && i < 128; ++i) {
        auto record = input.records[i];
        if (!valid_record(record, result.collected_at)) { filtered = true; continue; }
        if (record.module_offset && *record.module_offset > kMaximumModuleOffset) record.module_offset.reset();
        result.records.push_back(std::move(record));
    }
    std::sort(result.records.begin(), result.records.end(), [](const Record& left, const Record& right) {
        return std::tie(left.time, left.version, left.fault_code, left.module_offset) >
            std::tie(right.time, right.version, right.fault_code, right.module_offset);
    });
    result.records.erase(std::unique(result.records.begin(), result.records.end(), [](const Record& left, const Record& right) {
        return std::tie(left.time, left.version, left.fault_code, left.module_offset) ==
            std::tie(right.time, right.version, right.fault_code, right.module_offset);
    }), result.records.end());
    if (result.records.size() > kMaximumRecords) result.records.resize(kMaximumRecords);
    if (filtered || input.records.size() > 128) result.status = Status::partial;
    else if (result.records.empty() && result.status == Status::found) result.status = Status::none;
    return result;
}

json::Value to_json(const Report& input) {
    const auto report = anonymous_report(input);
    auto root = json::Value::object();
    root.set("status", json::Value::string(std::string(to_string(report.status))));
    root.set("lookbackDays", json::Value::number(7));
    if (report.collected_at) root.set("collectedAt", json::Value::string(records::iso8601(report.collected_at)));
    auto records = json::Value::array();
    for (const auto& record : report.records) {
        auto item = json::Value::object();
        item.set("date", json::Value::string(records::iso8601(record.time)));
        item.set("version", json::Value::string(record.version));
        char code[11]; std::snprintf(code, sizeof code, "0x%08X", static_cast<unsigned>(record.fault_code));
        item.set("exceptionCode", json::Value::string(code));
        if (record.module_offset) {
            char offset[19]; std::snprintf(offset, sizeof offset, "0x%016llX", static_cast<unsigned long long>(*record.module_offset));
            item.set("moduleOffset", json::Value::string(offset));
        }
        records.push(std::move(item));
    }
    root.set("records", std::move(records));
    return root;
}
}  // namespace szu::crash
