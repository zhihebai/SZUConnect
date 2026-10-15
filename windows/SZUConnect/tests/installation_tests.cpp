// Offline installation transactions use only fake payloads in a random test directory.
// They never start an app, inspect credentials, change registry values or use the network.
#include "installation.hpp"

#include <windows.h>
#include <winioctl.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace installation = szu::app::installation_testing;
using FailurePoint = installation::Hooks::FailurePoint;

namespace {
int checks = 0;
int failures = 0;
int cases = 0;
int skipped = 0;
const char* active_case = "setup";

struct NativeHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~NativeHandle() { close(); }
    void close() {
        if (value != INVALID_HANDLE_VALUE) { CloseHandle(value); value = INVALID_HANDLE_VALUE; }
    }
};

void check(bool passed, const char* message) {
    ++checks;
    if (!passed) {
        ++failures;
        std::printf("FAIL [%s]: %s\n", active_case, message);
    }
}

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) throw std::runtime_error("cannot write isolated test payload");
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read isolated test payload");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool reparse_point(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// A junction must be removed as a link. Never descend into a reparse target, even
// while cleaning up a test: it need not be inside the randomly created directory.
void remove_test_tree(const fs::path& path) noexcept {
    try {
        std::error_code ec;
        const auto status = fs::symlink_status(path, ec);
        if (ec || status.type() == fs::file_type::not_found) return;
        if (reparse_point(path) || fs::is_symlink(status)) {
            if (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(path.c_str());
            else DeleteFileW(path.c_str());
            return;
        }
        if (fs::is_directory(status)) {
            for (const auto& child : fs::directory_iterator(path)) remove_test_tree(child.path());
        }
        fs::remove(path, ec);
    } catch (...) {
        // A failed cleanup must not broaden the deletion target.
    }
}

struct Fixture {
    fs::path base;
    fs::path temp_parent;
    installation::Hooks hooks;

    Fixture() {
        temp_parent = fs::weakly_canonical(fs::temp_directory_path());
        std::random_device random;
        for (int attempt = 0; attempt != 32; ++attempt) {
            const auto time = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            const auto suffix = (static_cast<std::uint64_t>(random()) << 32) ^ random() ^ time;
            wchar_t name[80]{};
            std::swprintf(name, std::size(name), L"szu-installation-test-%016llx", static_cast<unsigned long long>(suffix));
            base = temp_parent / name;
            if (fs::create_directory(base)) break;
            base.clear();
        }
        if (base.empty()) throw std::runtime_error("cannot create random test directory");
        hooks.root = base / L"安装目录 with spaces";
        hooks.source_executable = base / L"下载目录 with spaces" / L"新应用.exe";
        hooks.documents = {
            {L"README.md", "# Offline test application\n"},
            {L"LICENSE", "Offline test license\n"},
            {L"THIRD-PARTY-NOTICES.md", "Offline test notices\n"},
            {L"curl-COPYING.txt", "Offline test dependency license\n"}
        };
        write_source("first");
    }

    ~Fixture() {
        // Verify the full absolute cleanup target belongs to this fixture.
        if (!base.empty() && base.is_absolute() && base.parent_path() == temp_parent &&
            base.filename().native().starts_with(L"szu-installation-test-") && !reparse_point(base)) {
            remove_test_tree(base);
        }
    }

    void write_source(const std::string& version) {
        write_file(hooks.source_executable, "MZ\nOFFLINE INSTALLATION TEST PAYLOAD\n" + version + "\n");
    }

    fs::path current() const { return hooks.root / L"current"; }
    fs::path previous() const { return hooks.root / L"previous"; }
    fs::path executable() const { return current() / L"SZUConnect.exe"; }
    fs::path manifest() const { return current() / L"INSTALLATION-MANIFEST.txt"; }
    fs::path documents() const { return current() / L"说明与许可"; }

    bool install() {
        const auto result = installation::run_transaction(hooks);
        check(result.success, "initial isolated installation succeeds");
        return result.success;
    }
};

using Snapshot = std::map<fs::path, std::string>;

Snapshot snapshot(const fs::path& root) {
    Snapshot files;
    if (!fs::exists(root)) return files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (reparse_point(entry.path())) throw std::runtime_error("unexpected reparse point in snapshot");
        if (entry.is_regular_file()) files.emplace(entry.path().lexically_relative(root), read_file(entry.path()));
    }
    return files;
}

void rejected(const installation::Result& result) {
    check(!result.success, "unsafe or failed transaction is rejected");
    check(!result.error_key.empty(), "rejection has a nonempty error key");
}

void check_complete(const Fixture& fixture) {
    check(installation::owned_payload(fixture.current()), "current payload has a valid ownership manifest");
    check(fs::is_regular_file(fixture.hooks.root / L"installation.id"), "installation ownership marker exists");
    check(fs::is_regular_file(fixture.manifest()), "payload manifest exists");
    check(read_file(fixture.executable()) == read_file(fixture.hooks.source_executable), "installed executable is the supplied payload");
    for (const auto& [name, content] : fixture.hooks.documents) {
        check(read_file(fixture.documents() / name) == content, "embedded document is installed without alteration");
    }
}

void test_case(const char* name, const std::function<void()>& body) {
    active_case = name;
    ++cases;
    const auto before = failures;
    const auto skipped_before = skipped;
    try { body(); }
    catch (const std::exception& error) {
        ++failures;
        std::printf("FAIL [%s]: isolated setup or transaction exception: %s\n", name, error.what());
    }
    catch (...) {
        ++failures;
        std::printf("FAIL [%s]: unexpected isolated exception\n", name);
    }
    std::printf("%s: %s\n", failures != before ? "FAIL" : skipped != skipped_before ? "SKIP" : "PASS", name);
}

// Directory junctions do not require enabling Developer Mode or requesting
// privileges. If the temporary filesystem cannot create one, explicitly skip.
bool make_junction(const fs::path& link, const fs::path& target) {
    fs::create_directories(link.parent_path());
    if (!fs::create_directory(link)) return false;
    const HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory == INVALID_HANDLE_VALUE) { fs::remove(link); return false; }

    struct JunctionData {
        DWORD tag;
        WORD data_size;
        WORD reserved;
        WORD substitute_offset;
        WORD substitute_size;
        WORD print_offset;
        WORD print_size;
        wchar_t paths[4096];
    } data{};
    const std::wstring print_name = fs::absolute(target).native();
    const std::wstring substitute = L"\\??\\" + print_name;
    bool created = false;
    if (substitute.size() + print_name.size() + 2 < std::size(data.paths)) {
        data.tag = IO_REPARSE_TAG_MOUNT_POINT;
        data.substitute_size = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
        data.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
        data.print_size = static_cast<WORD>(print_name.size() * sizeof(wchar_t));
        std::memcpy(data.paths, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
        std::memcpy(reinterpret_cast<std::byte*>(data.paths) + data.print_offset,
            print_name.c_str(), (print_name.size() + 1) * sizeof(wchar_t));
        data.data_size = static_cast<WORD>(8 + data.print_offset + data.print_size + sizeof(wchar_t));
        DWORD returned = 0;
        created = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, &data,
            static_cast<DWORD>(8 + data.data_size), nullptr, 0, &returned, nullptr) != FALSE;
    }
    CloseHandle(directory);
    if (!created) fs::remove(link);
    return created;
}

void skip_link(const char* reason) {
    ++skipped;
    std::printf("SKIP [%s]: %s; no security settings were changed\n", active_case, reason);
}

void interrupted_upgrade(FailurePoint point, bool launch, bool ready) {
    Fixture fixture;
    if (!fixture.install()) return;
    const auto before = snapshot(fixture.current());
    const auto marker = read_file(fixture.hooks.root / L"installation.id");
    fixture.write_source("second");
    fixture.hooks.documents.front().second = "# Updated offline documentation\n";
    fixture.hooks.fail_at = point;
    fixture.hooks.launch_succeeds = launch;
    fixture.hooks.ready_succeeds = ready;
    const auto result = installation::run_transaction(fixture.hooks);
    rejected(result);
    check(snapshot(fixture.current()) == before, "failure restores every prior payload byte");
    check(installation::owned_payload(fixture.current()), "restored payload remains recognizable and owned");
    check(read_file(fixture.hooks.root / L"installation.id") == marker, "failure preserves installation identity");
    if (fs::exists(fixture.previous())) {
        check(snapshot(fixture.previous()) == before, "any retained backup contains the complete prior payload");
    }
    if (point == FailurePoint::none) {
        check(result.stop_calls == 1 && result.apply_calls == 1 && result.launch_calls == 1,
            "launch or readiness failure follows stop, link migration and launch");
        check(result.restore_calls == 1 && result.restart_calls == 1,
            "launch or readiness failure restores old links and restarts the old payload");
    }
}
}  // namespace

int main() {
    test_case("read-only and offline entry points bypass installation", [] {
        // These branches return before resolving a real installation directory,
        // inspecting a process or reading any per-user system configuration.
        for (int mode = 0; mode != 3; ++mode) {
            szu::app::InstallationOptions options;
            options.observe_only = mode == 0;
            options.offline_smoke = mode == 1;
            options.portable = mode == 2;
            const auto result = szu::app::prepare_installation(nullptr, options);
            check(result.action == szu::app::InstallationAction::continueStartup,
                "observe-only, offline-smoke and portable modes continue without installation");
            options.ready_token = L"invalid-offline-token";
            check(szu::app::prepare_installation(nullptr, options).action == szu::app::InstallationAction::failed,
                "bypass modes cannot carry an installation readiness token");
        }
        check(!szu::app::signal_installation_ready(L"invalid-offline-token"),
            "invalid readiness token is rejected before resolving a real installation");
    });

    test_case("SHA-256 has known file results", [] {
        Fixture fixture;
        const auto path = fixture.base / L"hash-input.txt";
        write_file(path, "abc");
        check(installation::file_sha256(path) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 matches the standard abc vector");
        write_file(path, "");
        check(installation::file_sha256(path) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "SHA-256 matches the standard empty file vector");
        check(installation::file_sha256(fixture.base / L"missing.txt").empty(), "unreadable file has no fabricated digest");
    });

    test_case("version guard compares all four components", [] {
        using Version = std::array<unsigned short, 4>;
        const Version installed{0, 10, 1, 32};
        check(!installation::version_older(installed, installed), "same version permits payload updates without being considered a downgrade");
        check(installation::version_older(Version{0, 10, 1, 31}, installed), "lower build number is older");
        check(!installation::version_older(Version{0, 10, 1, 33}, installed), "higher build number is newer");
        check(installation::version_older(Version{0, 10, 0, 65535}, installed), "patch takes priority over build");
        check(!installation::version_older(Version{0, 10, 2, 0}, installed), "higher patch takes priority over build");
        check(installation::version_older(Version{0, 9, 65535, 65535}, installed), "minor takes priority over patch and build");
        check(!installation::version_older(Version{0, 11, 0, 0}, installed), "higher minor takes priority over remaining components");
        check(installation::version_older(Version{0, 65535, 65535, 65535}, Version{1, 0, 0, 0}), "major takes priority over remaining components");
        check(!installation::version_older(Version{1, 0, 0, 0}, installed), "higher major is newer");
    });

    test_case("first installation handles Unicode and spaces", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        check_complete(fixture);
        check(!fs::exists(fixture.previous()), "first installation does not fabricate an old payload");
    });

    test_case("identical installation does not restart or rewrite", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        const auto before = snapshot(fixture.current());
        const auto modified = fs::last_write_time(fixture.executable());
        fixture.hooks.reuse_running_current = true;
        fixture.hooks.launch_succeeds = false;
        fixture.hooks.ready_succeeds = false;
        const auto result = installation::run_transaction(fixture.hooks);
        check(result.success, "identical payload succeeds without invoking failed launch hooks");
        check(snapshot(fixture.current()) == before, "identical payload keeps installed bytes");
        check(fs::last_write_time(fixture.executable()) == modified, "identical payload keeps executable modification time");
        check(!fs::exists(fixture.previous()), "identical payload does not create a redundant backup");
        check(result.activation_calls == 1, "identical payload activates the existing managed instance");
        check(result.stop_calls == 0 && result.apply_calls == 0 && result.restore_calls == 0 &&
            result.launch_calls == 0 && result.restart_calls == 0, "existing managed instance is never stopped, rewritten or relaunched");
    });

    test_case("same payload still migrates a legacy or absent instance", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        const auto before = snapshot(fixture.current());
        const auto modified = fs::last_write_time(fixture.executable());
        const auto result = installation::run_transaction(fixture.hooks);
        check(result.success, "same bytes without a managed instance migrate successfully");
        check(result.stop_calls == 1 && result.apply_calls == 1 && result.launch_calls == 1,
            "legacy or absent instance follows stop, migrate links and launch");
        check(result.activation_calls == 0 && result.restore_calls == 0 && result.restart_calls == 0,
            "successful legacy migration does not use managed activation or rollback");
        check(snapshot(fixture.current()) == before && fs::last_write_time(fixture.executable()) == modified,
            "legacy migration preserves an identical managed payload without copying it");
        check(!fs::exists(fixture.previous()), "same-byte legacy migration does not create a redundant payload backup");
    });

    for (const bool launch_failure : {true, false}) {
        test_case(launch_failure ? "same payload legacy launch failure restores links" : "same payload legacy readiness failure restores links", [launch_failure] {
            Fixture fixture;
            if (!fixture.install()) return;
            const auto before = snapshot(fixture.current());
            fixture.hooks.launch_succeeds = !launch_failure;
            fixture.hooks.ready_succeeds = launch_failure;
            const auto result = installation::run_transaction(fixture.hooks);
            rejected(result);
            check(snapshot(fixture.current()) == before, "failed same-byte migration preserves the existing managed payload");
            check(result.stop_calls == 1 && result.apply_calls == 1 && result.launch_calls == 1,
                "same-byte failure follows the real migration sequence");
            check(result.restore_calls == 1 && result.restart_calls == 1 && result.activation_calls == 0,
                "same-byte failure restores links and restarts the old instance");
        });
    }

    test_case("failure to stop old instance cancels before replacement", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        const auto before = snapshot(fixture.current());
        fixture.write_source("second");
        fixture.hooks.stop_old_succeeds = false;
        const auto result = installation::run_transaction(fixture.hooks);
        rejected(result);
        check(result.stop_calls == 1 && result.apply_calls == 0 && result.launch_calls == 0 &&
            result.restore_calls == 0 && result.restart_calls == 0, "failed normal exit cannot proceed to link changes or launch");
        check(snapshot(fixture.current()) == before, "old instance exit failure preserves every installed payload byte");
        fixture.hooks.stop_old_succeeds = true;
        check(installation::run_transaction(fixture.hooks).success, "canceled exit permits a later clean upgrade attempt");
    });

    test_case("partial link migration failure restores links and old payload", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        const auto before = snapshot(fixture.current());
        fixture.write_source("second");
        fixture.hooks.apply_links_succeeds = false;
        const auto result = installation::run_transaction(fixture.hooks);
        rejected(result);
        check(result.stop_calls == 1 && result.apply_calls == 1 && result.launch_calls == 0,
            "failed link migration cannot launch the new application");
        check(result.restore_calls == 1 && result.restart_calls == 1,
            "partial link migration is restored before restarting the old payload");
        check(snapshot(fixture.current()) == before, "partial link migration failure preserves every old payload byte");
    });

    test_case("link restoration failure still restores old payload", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        const auto before = snapshot(fixture.current());
        fixture.write_source("second");
        fixture.hooks.launch_succeeds = false;
        fixture.hooks.restore_links_succeeds = false;
        const auto result = installation::run_transaction(fixture.hooks);
        rejected(result);
        check(result.restore_calls == 1, "failed link restoration is attempted once");
        check(snapshot(fixture.current()) == before && installation::owned_payload(fixture.current()),
            "link restoration failure does not leave the new payload installed");
    });

    for (const bool bad_marker : {true, false}) {
        test_case(bad_marker ? "managed reuse cannot bypass corrupt ownership" : "managed reuse cannot bypass unknown root files", [bad_marker] {
            Fixture fixture;
            if (!fixture.install()) return;
            if (bad_marker) write_file(fixture.hooks.root / L"installation.id", "unknown installation identity\n");
            else write_file(fixture.hooks.root / L"user-notes.txt", "Keep this unknown root file.\n");
            const auto before = snapshot(fixture.hooks.root);
            fixture.hooks.reuse_running_current = true;
            const auto result = installation::run_transaction(fixture.hooks);
            rejected(result);
            check(result.activation_calls == 0 && result.stop_calls == 0 && result.apply_calls == 0 && result.launch_calls == 0,
                "unrecognized root cannot activate, stop or migrate an otherwise identical payload");
            check(snapshot(fixture.hooks.root) == before, "managed reuse preserves the unrecognized installation exactly");
        });
    }

    test_case("managed reuse cannot bypass incomplete embedded documents", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        const auto before = snapshot(fixture.hooks.root);
        fixture.hooks.documents.pop_back();
        fixture.hooks.reuse_running_current = true;
        const auto result = installation::run_transaction(fixture.hooks);
        rejected(result);
        check(result.activation_calls == 0 && result.stop_calls == 0 && result.apply_calls == 0 && result.launch_calls == 0,
            "incomplete incoming licenses cannot use the identical-payload fast path");
        check(snapshot(fixture.hooks.root) == before, "incomplete incoming licenses preserve installed files");
    });

    test_case("upgrade replaces complete payload and keeps prior version", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        const auto before = snapshot(fixture.current());
        const auto marker = read_file(fixture.hooks.root / L"installation.id");
        fixture.write_source("second");
        fixture.hooks.documents.front().second = "# Updated offline documentation\n";
        const auto result = installation::run_transaction(fixture.hooks);
        check(result.success, "changed payload upgrades successfully");
        check_complete(fixture);
        check(snapshot(fixture.previous()) == before, "previous contains the complete original executable and documents");
        check(installation::owned_payload(fixture.previous()), "previous is a valid owned payload");
        check(read_file(fixture.hooks.root / L"installation.id") == marker, "upgrade preserves installation identity");
    });

    test_case("document-only change upgrades the payload", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        const auto before = snapshot(fixture.current());
        fixture.hooks.documents.back().second += "Additional offline license notice\n";
        check(installation::run_transaction(fixture.hooks).success, "changed bundled document is not mistaken for an identical package");
        check_complete(fixture);
        check(snapshot(fixture.previous()) == before, "document-only upgrade retains the complete earlier payload");
    });

    test_case("subsequent upgrade rotates only the owned backup", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        fixture.write_source("second");
        check(installation::run_transaction(fixture.hooks).success, "second payload installs successfully");
        const auto second = snapshot(fixture.current());
        fixture.write_source("third");
        check(installation::run_transaction(fixture.hooks).success, "third payload installs successfully");
        check_complete(fixture);
        check(snapshot(fixture.previous()) == second, "latest complete old payload replaces the earlier owned backup");
    });

    test_case("interrupted rename recovers from complete previous", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        const auto before = snapshot(fixture.current());
        fs::rename(fixture.current(), fixture.previous());
        fixture.write_source("second");
        check(installation::run_transaction(fixture.hooks).success, "missing current is recovered before a new upgrade");
        check_complete(fixture);
        check(snapshot(fixture.previous()) == before, "recovered prior payload remains available after the upgrade");
    });

    test_case("stage failure restores old payload", [] { interrupted_upgrade(FailurePoint::after_stage, true, true); });
    test_case("backup failure restores old payload", [] { interrupted_upgrade(FailurePoint::after_backup, true, true); });
    test_case("publish failure restores old payload", [] { interrupted_upgrade(FailurePoint::after_publish, true, true); });
    test_case("launch failure restores old payload", [] { interrupted_upgrade(FailurePoint::none, false, true); });
    test_case("readiness failure restores old payload", [] { interrupted_upgrade(FailurePoint::none, true, false); });

    const std::array first_failures{
        std::pair{FailurePoint::after_stage, "first stage failure"},
        std::pair{FailurePoint::after_backup, "first backup failure"},
        std::pair{FailurePoint::after_publish, "first publish failure"},
        std::pair{FailurePoint::none, "first launch failure"},
        std::pair{FailurePoint::none, "first readiness failure"}
    };
    for (std::size_t index = 0; index < first_failures.size(); ++index) {
        const auto [point, name] = first_failures[index];
        test_case(name, [point, index] {
            Fixture fixture;
            fixture.hooks.fail_at = point;
            fixture.hooks.launch_succeeds = index != 3;
            fixture.hooks.ready_succeeds = index != 4;
            rejected(installation::run_transaction(fixture.hooks));
            check(!installation::owned_payload(fixture.current()), "failed first installation does not masquerade as a working installation");
            check(!fs::exists(fixture.executable()), "failed first installation leaves no runnable partial executable");
            check(read_file(fixture.hooks.source_executable).find("first") != std::string::npos,
                "failed first installation preserves the downloaded source");
            fixture.hooks.fail_at = FailurePoint::none;
            fixture.hooks.launch_succeeds = true;
            fixture.hooks.ready_succeeds = true;
            check(installation::run_transaction(fixture.hooks).success, "failed first installation permits a clean retry");
            check_complete(fixture);
        });
    }

    test_case("unrecognized root content is preserved", [] {
        Fixture fixture;
        const auto notes = fixture.hooks.root / L"user-notes.txt";
        write_file(notes, "Unknown user file: keep exactly.\n");
        const auto before = snapshot(fixture.hooks.root);
        rejected(installation::run_transaction(fixture.hooks));
        check(snapshot(fixture.hooks.root) == before, "unrecognized root is not rewritten or cleaned");
    });

    test_case("unknown file alongside owned current prevents replacement", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        write_file(fixture.hooks.root / L"user-notes.txt", "Keep this unknown root file.\n");
        const auto before = snapshot(fixture.hooks.root);
        fixture.write_source("second");
        rejected(installation::run_transaction(fixture.hooks));
        check(snapshot(fixture.hooks.root) == before, "recognized ownership cannot authorize deleting unknown root content");
    });

    test_case("unknown file within current prevents replacement", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        write_file(fixture.current() / L"user-note.txt", "Keep this unknown payload file.\n");
        const auto before = snapshot(fixture.current());
        fixture.write_source("second");
        rejected(installation::run_transaction(fixture.hooks));
        check(snapshot(fixture.current()) == before, "unknown current file and old payload are preserved");
    });

    test_case("unknown backup content prevents backup rotation", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        fixture.write_source("second");
        check(installation::run_transaction(fixture.hooks).success, "second payload installs successfully");
        write_file(fixture.previous() / L"unknown-owner-file.txt", "Keep this backup file.\n");
        const auto before = snapshot(fixture.hooks.root);
        fixture.write_source("third");
        rejected(installation::run_transaction(fixture.hooks));
        check(snapshot(fixture.hooks.root) == before, "unknown backup content and current are both preserved");
    });

    for (const bool executable : {true, false}) {
        test_case(executable ? "modified executable is preserved" : "modified bundled document is preserved", [executable] {
            Fixture fixture;
            if (!fixture.install()) return;
            write_file(executable ? fixture.executable() : fixture.documents() / fixture.hooks.documents.front().first,
                "Locally modified content: keep exactly.\n");
            const auto before = snapshot(fixture.current());
            check(!installation::owned_payload(fixture.current()), "modified payload is no longer considered safely owned");
            fixture.write_source("second");
            rejected(installation::run_transaction(fixture.hooks));
            check(snapshot(fixture.current()) == before, "modified payload is not overwritten");
        });
    }

    for (const bool missing : {false, true}) {
        test_case(missing ? "missing manifest prevents replacement" : "corrupt manifest prevents replacement", [missing] {
            Fixture fixture;
            if (!fixture.install()) return;
            if (missing) fs::remove(fixture.manifest());
            else write_file(fixture.manifest(), "not a recognized installation manifest\n");
            const auto before = snapshot(fixture.current());
            check(!installation::owned_payload(fixture.current()), "unrecognized manifest cannot establish ownership");
            fixture.write_source("second");
            rejected(installation::run_transaction(fixture.hooks));
            check(snapshot(fixture.current()) == before, "unrecognized manifest and payload are kept intact");
        });
    }

    test_case("corrupt installation marker prevents replacement", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        write_file(fixture.hooks.root / L"installation.id", "unknown installation identity\n");
        const auto before = snapshot(fixture.hooks.root);
        fixture.write_source("second");
        rejected(installation::run_transaction(fixture.hooks));
        check(snapshot(fixture.hooks.root) == before, "unrecognized installation identity is not overwritten");
    });

    test_case("missing source does not change installed payload", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        const auto before = snapshot(fixture.current());
        fs::remove(fixture.hooks.source_executable);
        rejected(installation::run_transaction(fixture.hooks));
        check(snapshot(fixture.current()) == before, "missing downloaded executable preserves the working installation");
    });

    test_case("directory source is rejected", [] {
        Fixture fixture;
        fs::remove(fixture.hooks.source_executable);
        fs::create_directory(fixture.hooks.source_executable);
        rejected(installation::run_transaction(fixture.hooks));
        check(!fs::exists(fixture.executable()), "directory source cannot create an installed executable");
    });

    test_case("document traversal is rejected without outside writes", [] {
        Fixture fixture;
        const auto sentinel = fixture.base / L"outside-sentinel.txt";
        write_file(sentinel, "Outside target remains untouched.\n");
        fixture.hooks.documents.front() = {L"../../../../outside-sentinel.txt", "Do not publish this.\n"};
        rejected(installation::run_transaction(fixture.hooks));
        check(read_file(sentinel) == "Outside target remains untouched.\n", "document traversal cannot overwrite an outside file");
        check(!installation::owned_payload(fixture.current()), "invalid document path cannot form an installed payload");
    });

    test_case("root junction is rejected without traversing target", [] {
        Fixture fixture;
        const auto target = fixture.base / L"outside root target";
        write_file(target / L"sentinel.txt", "Outside root target remains untouched.\n");
        const auto before = snapshot(target);
        if (!make_junction(fixture.hooks.root, target)) { skip_link("temporary filesystem cannot create a junction"); return; }
        rejected(installation::run_transaction(fixture.hooks));
        check(reparse_point(fixture.hooks.root), "root junction itself is retained");
        check(snapshot(target) == before, "root junction target is never modified");
    });

    test_case("current junction is rejected without traversing target", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        const auto target = fixture.base / L"outside current target";
        fs::rename(fixture.current(), target);
        const auto before = snapshot(target);
        if (!make_junction(fixture.current(), target)) { skip_link("temporary filesystem cannot create a junction"); return; }
        fixture.write_source("second");
        check(!installation::owned_payload(fixture.current()), "current junction is not treated as an owned payload");
        rejected(installation::run_transaction(fixture.hooks));
        check(reparse_point(fixture.current()), "current junction itself is retained");
        check(snapshot(target) == before, "current junction target is never modified");
    });

    test_case("nested documentation junction is rejected", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        const auto target = fixture.base / L"outside documentation target";
        fs::rename(fixture.documents(), target);
        const auto before = snapshot(target);
        if (!make_junction(fixture.documents(), target)) { skip_link("temporary filesystem cannot create a junction"); return; }
        fixture.write_source("second");
        check(!installation::owned_payload(fixture.current()), "nested junction does not establish payload ownership");
        rejected(installation::run_transaction(fixture.hooks));
        check(reparse_point(fixture.documents()), "nested junction itself is retained");
        check(snapshot(target) == before, "nested documentation junction target is never modified");
    });

    test_case("backup junction is rejected without replacing target", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        const auto target = fixture.base / L"outside backup target";
        write_file(target / L"sentinel.txt", "Outside backup target remains untouched.\n");
        const auto before = snapshot(fixture.current());
        const auto outside_before = snapshot(target);
        if (!make_junction(fixture.previous(), target)) { skip_link("temporary filesystem cannot create a junction"); return; }
        fixture.write_source("second");
        rejected(installation::run_transaction(fixture.hooks));
        check(snapshot(fixture.current()) == before, "backup junction does not disturb current payload");
        check(reparse_point(fixture.previous()), "backup junction itself is retained");
        check(snapshot(target) == outside_before, "backup junction target is never modified");
    });

    test_case("source ancestor junction is rejected", [] {
        Fixture fixture;
        const auto source_parent = fixture.hooks.source_executable.parent_path();
        const auto target = fixture.base / L"outside source directory";
        fs::rename(source_parent, target);
        const auto before = snapshot(target);
        if (!make_junction(source_parent, target)) { skip_link("temporary filesystem cannot create a junction"); return; }
        rejected(installation::run_transaction(fixture.hooks));
        check(snapshot(target) == before, "source ancestor junction target is never modified");
        check(!fs::exists(fixture.executable()), "source ancestor junction cannot form an installed payload");
    });

    test_case("locked current directory aborts without partial replacement", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        const auto before = snapshot(fixture.current());
        NativeHandle locked{CreateFileW(fixture.current().c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
        if (locked.value == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot lock isolated current directory");
        fixture.write_source("second");
        const auto result = installation::run_transaction(fixture.hooks);
        locked.close();
        rejected(result);
        check(snapshot(fixture.current()) == before, "a failed directory rename retains every prior payload byte");
        check(installation::owned_payload(fixture.current()), "locked payload remains a valid installation");
        check(installation::run_transaction(fixture.hooks).success, "replacement succeeds after the isolated lock is released");
        check_complete(fixture);
    });

    test_case("source symbolic link is rejected", [] {
        Fixture fixture;
        const auto target = fixture.base / L"outside source.exe";
        fs::rename(fixture.hooks.source_executable, target);
        const auto before = read_file(target);
        std::error_code ec;
        fs::create_symlink(target, fixture.hooks.source_executable, ec);
        if (ec) { skip_link("current user cannot create a symbolic link"); return; }
        rejected(installation::run_transaction(fixture.hooks));
        check(read_file(target) == before, "source symbolic link target is never modified");
        check(!fs::exists(fixture.executable()), "source symbolic link cannot form an installed payload");
    });

    std::printf("Installation transactions: %d cases, %d checks, %d failures, %d skipped reparse cases.\n",
        cases, checks, failures, skipped);
    return failures == 0 ? 0 : 1;
}
