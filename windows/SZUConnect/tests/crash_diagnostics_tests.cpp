#include "szu/crash_diagnostics.hpp"
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

int main() {
    using namespace szu;
    int checks = 0, failures = 0;
    const auto check = [&](bool pass, const char* what) { ++checks; if (!pass) { ++failures; std::printf("FAIL: %s\n", what); } };
    constexpr double now = 1792713600;
    const auto good = crash::parse_record(now, now - 60, "1.0.0.34", "c0000005", "1234");
    check(good && good->fault_code == 0xc0000005 && good->module_offset == 0x1234, "fixed event fields accept a valid record");
    for (const std::string version : {"", "1..0", "1.2.3.4.5", "65536.0", "-1.0", "1.0 private-marker", "SZU-fixture", "1.0/secret"})
        check(!crash::parse_record(now, now - 1, version, "c0000005"), "untrusted version strings cannot enter the exported model");
    for (const std::string code : {"", "0", "0x00000000", "100000000", "-1", "c0000005 query", "secret"})
        check(!crash::parse_record(now, now - 1, "1.0.0", code), "exception codes are bounded nonzero hex values");
    for (const double time : {now + 1, now - crash::kLookbackSeconds - 1, 0.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        check(!crash::parse_record(now, time, "1.0.0", "c0000005"), "events outside the lookback window or malformed dates are excluded");
    for (const std::string offset : {"private-module", "10000001", "10000000000000000", "-1"}) {
        const auto record = crash::parse_record(now, now - 1, "1.0.0", "0xC0000005", offset);
        check(record && !record->module_offset, "untrusted or excessive module offsets are omitted without discarding the crash");
    }
    crash::Report report{crash::Status::found, now, {*good, *good}};
    auto text = json::serialize(crash::to_json(report));
    const auto parsed = json::parse(text);
    check(parsed && parsed->find("records")->items().size() == 1 && text.find("0xC0000005") != std::string::npos &&
        text.find("0x0000000000001234") != std::string::npos, "serialization normalizes hex and removes duplicate records");
    report.records.clear();
    for (unsigned i = 0; i < 12; ++i) report.records.push_back({now - i, "1.0.0.34", 0xc0000005, i});
    const auto bounded = crash::anonymous_report(report);
    check(bounded.records.size() == 5 && bounded.records.front().time == now && bounded.records.back().time == now - 4,
        "at most the five newest valid events are retained");
    report.records.push_back({now, "SSID-private", 0xc0000005, 0});
    report.records.push_back({now, "1.0.0", 0xc0000005, std::numeric_limits<std::uint64_t>::max()});
    const auto filtered = crash::anonymous_report(report);
    check(filtered.status == crash::Status::partial, "malformed synthetic events are reported as partial data");
    text = json::serialize(crash::to_json(report));
    check(text.find("SSID-private") == std::string::npos && text.find("moduleName") == std::string::npos &&
        text.find("appPath") == std::string::npos && text.find("pid") == std::string::npos, "the export model has no raw event or identity fields");
    for (auto status : {crash::Status::notCollected, crash::Status::none, crash::Status::accessDenied, crash::Status::unavailable, crash::Status::cancelled}) {
        report.status = status;
        check(crash::anonymous_report(report).records.empty(), "non-result status never retains stale crash records");
    }
    report.status = static_cast<crash::Status>(99);
    check(crash::anonymous_report(report).status == crash::Status::unavailable, "unknown statuses are bounded at the serialization boundary");
    report.status = crash::Status::found; report.collected_at = 0;
    check(crash::anonymous_report(report).status == crash::Status::unavailable && crash::anonymous_report(report).records.empty(),
        "missing collection time cannot publish undated crash data");
    report = {crash::Status::found, now, {}};
    check(crash::anonymous_report(report).status == crash::Status::none, "an empty successful scan is distinct from not collected");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
