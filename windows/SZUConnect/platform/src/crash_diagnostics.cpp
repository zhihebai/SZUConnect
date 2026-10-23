#include "szu/win/crash_diagnostics.hpp"
#include <windows.h>
#include <winevt.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cwchar>
#include <vector>

namespace szu::win {
namespace {
struct EventHandle {
    EVT_HANDLE value = nullptr;
    ~EventHandle() { if (value) EvtClose(value); }
};
std::optional<std::wstring> absolute_path(std::wstring_view input) {
    if (input.size() > 32760 || input.empty() || input.find(L'\0') != std::wstring_view::npos) return std::nullopt;
    if (input.starts_with(L"\\\\?\\")) input.remove_prefix(4);
    if (input.size() < 4 || !((input[0] >= L'A' && input[0] <= L'Z') || (input[0] >= L'a' && input[0] <= L'z')) ||
        input[1] != L':' || input[2] != L'\\' || input.find(L':', 2) != std::wstring_view::npos) return std::nullopt;
    const std::wstring owned(input);
    const DWORD needed = GetFullPathNameW(owned.c_str(), 0, nullptr, nullptr);
    if (!needed || needed > 32768) return std::nullopt;
    std::wstring result(needed, L'\0');
    const DWORD written = GetFullPathNameW(owned.c_str(), needed, result.data(), nullptr);
    if (!written || written >= needed) return std::nullopt;
    result.resize(written);
    return result;
}
bool equal_path(std::wstring_view left, std::wstring_view right) {
    const auto a = absolute_path(left), b = absolute_path(right);
    return a && b && CompareStringOrdinal(a->data(), static_cast<int>(a->size()), b->data(), static_cast<int>(b->size()), TRUE) == CSTR_EQUAL;
}
std::wstring_view basename(std::wstring_view path) { return path.substr(path.find_last_of(L"\\/") + 1); }
bool equal_name(std::wstring_view left, std::wstring_view right) {
    return !left.empty() && left.size() < 260 && right.size() < 260 &&
        CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}
std::optional<std::wstring> string_value(const EVT_VARIANT& value) {
    if (value.Type != EvtVarTypeString || !value.StringVal) return std::nullopt;
    const auto length = wcsnlen_s(value.StringVal, 32761);
    if (length > 32760) return std::nullopt;
    return std::wstring(value.StringVal, length);
}
std::string ascii_value(const EVT_VARIANT& value) {
    const auto text = string_value(value);
    if (!text || text->size() > 64) return {};
    std::string result;
    for (wchar_t ch : *text) { if (ch < 0x20 || ch > 0x7e) return {}; result.push_back(static_cast<char>(ch)); }
    return result;
}
double unix_file_time(std::uint64_t value) {
    constexpr std::uint64_t epoch = 116444736000000000ULL;
    return value >= epoch ? static_cast<double>(value - epoch) / 10000000.0 : 0;
}
std::optional<CrashEvent> render(EVT_HANDLE context, EVT_HANDLE handle) {
    DWORD needed = 0, count = 0;
    EvtRender(context, handle, EvtRenderEventValues, 0, nullptr, &needed, &count);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed > 64 * 1024 || !needed) return std::nullopt;
    std::vector<std::max_align_t> buffer((needed + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    if (!EvtRender(context, handle, EvtRenderEventValues, static_cast<DWORD>(buffer.size() * sizeof(std::max_align_t)), buffer.data(), &needed, &count) || count != 8)
        return std::nullopt;
    const auto* values = reinterpret_cast<const EVT_VARIANT*>(buffer.data());
    CrashEvent event; event.provider = L"Application Error"; event.event_id = 1000;
    if (values[0].Type == EvtVarTypeFileTime) event.time = unix_file_time(values[0].FileTimeVal);
    else if (values[0].Type == EvtVarTypeString) {
        // Some event providers expose the selected attribute as ISO UTC text.
        const auto date = string_value(values[0]);
        SYSTEMTIME st{}; int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        if (date && swscanf_s(date->c_str(), L"%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) == 6) {
            st.wYear = static_cast<WORD>(year); st.wMonth = static_cast<WORD>(month); st.wDay = static_cast<WORD>(day);
            st.wHour = static_cast<WORD>(hour); st.wMinute = static_cast<WORD>(minute); st.wSecond = static_cast<WORD>(second);
            FILETIME ft{};
            if (SystemTimeToFileTime(&st, &ft)) event.time = unix_file_time((static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
        }
    }
    event.app_name = string_value(values[1]).value_or(L"");
    event.version = ascii_value(values[2]);
    event.app_path = string_value(values[3]).value_or(L"");
    event.fault_code = ascii_value(values[4]);
    event.module_name = string_value(values[5]).value_or(L"");
    event.module_path = string_value(values[6]).value_or(L"");
    event.module_offset = ascii_value(values[7]);
    return event;
}
}

std::optional<crash::Record> filter_crash_event(const CrashEvent& event, std::wstring_view executable, double now) {
    if (event.provider != L"Application Error" || event.event_id != 1000 ||
        !equal_name(event.app_name, basename(executable)) || !equal_path(event.app_path, executable)) return std::nullopt;
    const bool own_module = equal_name(event.module_name, basename(executable)) && equal_path(event.module_path, executable);
    return crash::parse_record(now, event.time, event.version, event.fault_code,
        own_module ? std::optional<std::string_view>(event.module_offset) : std::nullopt);
}

crash::Report collect_crash_diagnostics(const CancelToken& token) {
    crash::Report report;
    report.collected_at = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (token.cancelled()) { report.status = crash::Status::cancelled; return report; }
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) { report.status = crash::Status::unavailable; return report; }
    executable.resize(length);
    const auto name = basename(executable);
    if (!absolute_path(executable) || name.empty() || name.size() > 100 || !std::all_of(name.begin(), name.end(), [](wchar_t ch) {
            return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'-' || ch == L'_' || ch == L' ';
        })) { report.status = crash::Status::unavailable; return report; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    const std::wstring query = L"*[System[Provider[@Name='Application Error'] and EventID=1000 and TimeCreated[timediff(@SystemTime) <= 604800000]] and EventData[Data[@Name='AppName']='" + std::wstring(name) + L"']]";
    EventHandle results{EvtQuery(nullptr, L"Application", query.c_str(), EvtQueryChannelPath | EvtQueryReverseDirection)};
    if (!results.value) { report.status = GetLastError() == ERROR_ACCESS_DENIED ? crash::Status::accessDenied : crash::Status::unavailable; return report; }
    const wchar_t* paths[] = {L"Event/System/TimeCreated/@SystemTime", L"Event/EventData/Data[@Name='AppName']",
        L"Event/EventData/Data[@Name='AppVersion']", L"Event/EventData/Data[@Name='AppPath']", L"Event/EventData/Data[@Name='ExceptionCode']",
        L"Event/EventData/Data[@Name='ModuleName']", L"Event/EventData/Data[@Name='ModulePath']", L"Event/EventData/Data[@Name='FaultingOffset']"};
    EventHandle context{EvtCreateRenderContext(8, paths, EvtRenderContextValues)};
    if (!context.value) { report.status = crash::Status::unavailable; return report; }
    report.status = crash::Status::none;
    unsigned scanned = 0;
    while (scanned < 256 && report.records.size() < crash::kMaximumRecords && std::chrono::steady_clock::now() < deadline && !token.cancelled()) {
        EVT_HANDLE next = nullptr; DWORD returned = 0;
        if (!EvtNext(results.value, 1, &next, 100, 0, &returned)) {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_ITEMS) break;
            if (error == ERROR_TIMEOUT) continue;
            report.status = error == ERROR_ACCESS_DENIED && report.records.empty() ? crash::Status::accessDenied : crash::Status::partial;
            break;
        }
        EventHandle event_handle{next}; ++scanned;
        const auto event = render(context.value, next);
        if (!event) { report.status = crash::Status::partial; continue; }
        if (const auto record = filter_crash_event(*event, executable, report.collected_at)) report.records.push_back(*record);
        else if (equal_path(event->app_path, executable) && equal_name(event->app_name, name)) report.status = crash::Status::partial;
    }
    if (token.cancelled()) { report.status = crash::Status::cancelled; report.records.clear(); }
    else if (scanned >= 256 || (std::chrono::steady_clock::now() >= deadline && report.records.size() < crash::kMaximumRecords)) report.status = crash::Status::partial;
    else if (!report.records.empty() && report.status == crash::Status::none) report.status = crash::Status::found;
    return crash::anonymous_report(report);
}
}  // namespace szu::win
