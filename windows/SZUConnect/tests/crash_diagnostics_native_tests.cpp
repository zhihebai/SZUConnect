#include "szu/win/crash_diagnostics.hpp"
#include "szu/json.hpp"
#include <chrono>
#include <cstdio>
#include <string>

int main() {
    using namespace szu;
    int checks = 0, failures = 0;
    const auto check = [&](bool pass, const char* what) { ++checks; if (!pass) { ++failures; std::printf("FAIL: %s\n", what); } };
    constexpr double now = 1792713600;
    const std::wstring executable = L"C:\\Fixture\\Programs\\SZUConnect\\current\\SZUConnect.exe";
    win::CrashEvent good{L"Application Error", L"SZUConnect.exe", executable, L"SZUConnect.exe", executable,
        1000, now - 60, "1.0.0.34", "c0000005", "1234"};
    const auto own = win::filter_crash_event(good, executable, now);
    check(own && own->module_offset == 0x1234, "own executable crash retains a bounded module offset");
    auto event = good; event.app_path = L"c:\\fixture\\programs\\szuconnect\\current\\szuconnect.exe";
    check(win::filter_crash_event(event, executable, now).has_value(), "Windows path matching is case insensitive");
    for (const std::wstring path : {L"C:\\Other\\SZUConnect.exe", L"C:\\Fixture\\Programs\\SZUConnect\\current\\SZUConnect.exe:secret",
            L"\\\\server\\SZUConnect.exe", L"SZUConnect.exe", L"\\\\.\\C:\\SZUConnect.exe"}) {
        event = good; event.app_path = path;
        check(!win::filter_crash_event(event, executable, now), "same-looking names in other, relative or device paths are excluded");
    }
    event = good; event.app_name = L"SZUConnect-other.exe";
    check(!win::filter_crash_event(event, executable, now), "the application name and exact path must both match");
    event = good; event.provider = L"Windows Error Reporting";
    check(!win::filter_crash_event(event, executable, now), "WER informational events are not counted as crashes");
    event = good; event.event_id = 1001;
    check(!win::filter_crash_event(event, executable, now), "only Application Error 1000 events qualify");
    event = good; event.module_name = L"private-module.dll"; event.module_path = L"C:\\Private\\private-module.dll";
    const auto other_module = win::filter_crash_event(event, executable, now);
    check(other_module && !other_module->module_offset, "foreign module names and offsets are not exported");
    const std::wstring downloaded = L"C:\\Fixture\\Downloads\\SZUConnect-1.0.0-win-x64.exe";
    event = good; event.app_name = L"SZUConnect-1.0.0-win-x64.exe"; event.app_path = downloaded;
    event.module_name = event.app_name; event.module_path = downloaded;
    check(win::filter_crash_event(event, downloaded, now).has_value(), "a version-named executable matches its own exact process path");
    check(!win::filter_crash_event(event, executable, now), "downloaded executable events do not enter a different installed executable scan");
    const auto text = json::serialize(crash::to_json({crash::Status::found, now, {*own, *other_module}}));
    check(text.find("Fixture") == std::string::npos && text.find("Private") == std::string::npos &&
        text.find("SZUConnect.exe") == std::string::npos && text.find("module.dll") == std::string::npos,
        "no process path, name or module name crosses into JSON");
    CancelToken cancelled; cancelled.cancel();
    check(win::collect_crash_diagnostics(cancelled).status == crash::Status::cancelled, "pre-cancelled collection never starts an event query");
    const auto start = std::chrono::steady_clock::now();
    const auto report = win::collect_crash_diagnostics(CancelToken{});
    check(std::chrono::steady_clock::now() - start < std::chrono::seconds(5) && report.records.size() <= 5,
        "the native read-only scan completes within its finite budget without manufacturing a crash");
    check(report.status == crash::Status::found || report.status == crash::Status::none || report.status == crash::Status::partial ||
        report.status == crash::Status::accessDenied || report.status == crash::Status::unavailable,
        "OS access and unavailable data are explicit scan results");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
