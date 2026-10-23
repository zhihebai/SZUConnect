// Per-user installation bootstrap. Read-only/offline launches never enter this boundary.
#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
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
std::string embedded_license_text(HINSTANCE instance);
InstallationResult open_uninstaller();
int run_uninstaller(HINSTANCE instance, const std::wstring& worker_token);

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
struct UninstallHooks {
    std::filesystem::path root, data_root;
    bool remove_data = false, stop_succeeds = true, erase_succeeds = true;
    bool cleanup_succeeds = true, fail_after_first_payload_file = false;
};
struct UninstallResult {
    bool success = false;
    std::string error_key;
    int stop_calls = 0, erase_calls = 0, cleanup_calls = 0;
};
UninstallResult run_uninstall_transaction(const UninstallHooks& hooks);
bool data_filename_owned(const std::wstring& name);
bool worker_request_valid(const std::wstring& token, const std::filesystem::path& helper,
                          const std::filesystem::path& temporary_parent);
bool registration_failure_recoverable(const std::filesystem::path& root, int failure_at, bool restoring);
bool handoff_failure_is_cancelled(bool ready, bool signal_succeeds);
std::string cleanup_readiness_probe(HANDLE output, HANDLE process, DWORD timeout_ms);
std::string cleanup_readiness_exit_race_probe(HANDLE output, HANDLE process, HANDLE writer, const std::string& final_output);
std::string cleanup_fixture_exit_stage(DWORD exit_code);
// Only the test executable dispatches this mode, inside its own random fixture.
int run_cleanup_image_fixture(const std::string& mode, const std::wstring& token,
                              std::uintptr_t done_handle, std::uintptr_t passed_handle);
}
#endif
