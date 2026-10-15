// Per-user installation bootstrap. Read-only/offline launches never enter this boundary.
#pragma once
#include <windows.h>
#include <array>
#include <string>

namespace szu::app {
inline constexpr int kInstallReadmeResource = 401;
inline constexpr int kInstallLicenseResource = 402;
inline constexpr int kInstallNoticesResource = 403;
inline constexpr int kInstallCurlResource = 404;

enum class InstallationAction { continueStartup, handled, failed };
struct InstallationOptions {
    bool observe_only = false;
    bool offline_smoke = false;
    bool portable = false;
    bool background = false;
    std::wstring ready_token;
};
struct InstallationResult {
    InstallationAction action = InstallationAction::failed;
    std::string error_key;
};
InstallationResult prepare_installation(HINSTANCE instance, const InstallationOptions& options);
bool signal_installation_ready(const std::wstring& token);

}

#ifdef SZU_INSTALLATION_TESTING
#include <filesystem>
#include <utility>
#include <vector>
namespace szu::app::installation_testing {
struct Hooks {
    std::filesystem::path root, source_executable;
    std::vector<std::pair<std::wstring, std::string>> documents;
    enum class FailurePoint { none, after_stage, after_backup, after_publish };
    FailurePoint fail_at = FailurePoint::none;
    bool launch_succeeds = true, ready_succeeds = true;
    bool reuse_running_current = false, stop_old_succeeds = true;
    bool apply_links_succeeds = true, restore_links_succeeds = true;
};
struct Result {
    bool success = false;
    std::string error_key;
    int stop_calls = 0, apply_calls = 0, restore_calls = 0;
    int launch_calls = 0, restart_calls = 0, activation_calls = 0;
};
Result run_transaction(const Hooks& hooks);
bool owned_payload(const std::filesystem::path& path);
std::string file_sha256(const std::filesystem::path& path);
bool version_older(const std::array<unsigned short, 4>& incoming, const std::array<unsigned short, 4>& installed);
}
#endif
