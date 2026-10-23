// Portable early-launch contract; does not initialize any Windows service.
#pragma once
#include <string>
#include <string_view>
#include <vector>
namespace szu::app {
struct LaunchOptions {
    bool observe_only = false, background = false, offline_smoke = false, portable = false, uninstall = false;
    std::wstring smoke_output, ready_token, uninstall_worker_token;
    int smoke_timeout_ms = 10000;
    std::string error;
};
inline bool absolute_receipt_path(std::wstring_view p) {
    if (p.empty() || p.size() > 30000 || p.find_first_of(L"\r\n\t") != std::wstring_view::npos ||
        p.find(L'\0') != std::wstring_view::npos || p.find(L'"') != std::wstring_view::npos) return false;
    const bool drive = p.size() > 3 && ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) &&
        p[1] == L':' && (p[2] == L'\\' || p[2] == L'/');
    if (!drive) return false; // UNC/device paths can initiate SMB authentication and are not offline
    const auto last = p.find_last_of(L"\\/");
    return last != std::wstring_view::npos && last + 1 < p.size() && p.substr(last + 1).find(L':') == std::wstring_view::npos;
}
inline LaunchOptions parse_launch_options(const std::vector<std::wstring>& args) {
    LaunchOptions o; bool output = false, timeout = false;
    if (!args.empty() && args.front() == L"--uninstall") {
        if (args.size() != 1) o.error = "unknownOrDuplicateArgument";
        else o.uninstall = true;
        return o;
    }
    if (!args.empty() && args.front() == L"--uninstall-worker") {
        if (args.size() != 2) o.error = "unknownOrDuplicateArgument";
        else if (args[1].size() != 32 || args[1].find_first_not_of(L"0123456789abcdef") != std::wstring::npos)
            o.error = "invalidUninstallToken";
        else o.uninstall_worker_token = args[1];
        return o;
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == L"--observe-only") o.observe_only = true;
        else if (a == L"--background") o.background = true;
        else if (a == L"--portable" && !o.portable) o.portable = true;
        else if (a == L"--installation-ready" && o.ready_token.empty() && i + 1 < args.size()) {
            o.ready_token = args[++i];
            if (o.ready_token.size() != 32 || o.ready_token.find_first_not_of(L"0123456789abcdef") != std::wstring::npos) {
                o.error = "invalidInstallationToken"; break;
            }
        }
        else if (a == L"--offline-smoke" && !o.offline_smoke) o.offline_smoke = true;
        else if (a == L"--smoke-output" && !output && i + 1 < args.size()) { output = true; o.smoke_output = args[++i]; }
        else if (a == L"--smoke-timeout-ms" && !timeout && i + 1 < args.size()) {
            timeout = true; const auto& value = args[++i]; int n = 0;
            if (value.empty() || value.size() > 5) { o.error = "invalidTimeout"; break; }
            for (const auto c : value) {
                if (c < L'0' || c > L'9') { o.error = "invalidTimeout"; break; }
                n = n * 10 + static_cast<int>(c - L'0');
            }
            if (!o.error.empty() || n < 1000 || n > 30000) { o.error = "invalidTimeout"; break; }
            o.smoke_timeout_ms = n;
        } else { o.error = "unknownOrDuplicateArgument"; break; }
    }
    if (o.error.empty() && o.offline_smoke && (o.observe_only || o.background)) o.error = "conflictingModes";
    if (o.error.empty() && !o.ready_token.empty() && (o.observe_only || o.offline_smoke || o.portable)) o.error = "conflictingModes";
    if (o.error.empty() && o.offline_smoke && (!output || !absolute_receipt_path(o.smoke_output))) o.error = "invalidReceiptPath";
    if (o.error.empty() && !o.offline_smoke && (output || timeout)) o.error = "smokeOptionsRequireSmoke";
    return o;
}
}  // namespace szu::app
