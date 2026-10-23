// Offline installation transactions use fake payloads in random test directories.
// Cleanup probes run copies of this test executable; they never start the app,
// inspect credentials, change registry values or use the network.
#include "installation.hpp"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
#include <thread>
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
    fs::path documents() const { return current() / L"开源许可"; }

    void use_legacy_documents() const {
        fs::rename(documents(), current() / L"说明与许可");
    }

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
void cleanup_readiness_probe(const std::string& output, DWORD delay_ms, DWORD timeout_ms, const std::string& expected) {
    NativeHandle input, writer, process;
    if (!CreatePipe(&input.value, &writer.value, nullptr, 16384)) throw std::runtime_error("cannot create readiness fixture pipe");
    process.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!process.value) throw std::runtime_error("cannot create readiness fixture event");
    bool wrote = false;
    const auto write = [&] {
        DWORD count = 0;
        wrote = WriteFile(writer.value, output.data(), static_cast<DWORD>(output.size()), &count, nullptr) && count == output.size();
    };
    const auto start = GetTickCount64();
    std::thread producer;
    if (expected != "timeout") producer = std::thread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        write();
    });
    const auto result = installation::cleanup_readiness_probe(input.value, process.value, timeout_ms);
    const auto elapsed = GetTickCount64() - start;
    if (producer.joinable()) producer.join(); else write();
    check(wrote && result == expected, "only a complete validated stdout token grants readiness within the local deadline");
    if (expected == "timeout") check(elapsed >= timeout_ms,
        "a late ready token cannot extend the fixed local readiness deadline");
}
void cleanup_exit_output_probe(const std::string& output, const std::string& expected, bool already_exited = false) {
    NativeHandle input, writer, process;
    if (!CreatePipe(&input.value, &writer.value, nullptr, 16384)) throw std::runtime_error("cannot create final-output fixture pipe");
    process.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!process.value) throw std::runtime_error("cannot create final-output fixture event");
    std::string result;
    if (already_exited) {
        DWORD count = 0;
        if (!WriteFile(writer.value, output.data(), static_cast<DWORD>(output.size()), &count, nullptr) || count != output.size() ||
            !SetEvent(process.value)) throw std::runtime_error("cannot prepare exited fixture output");
        writer.close();
        result = installation::cleanup_readiness_probe(input.value, process.value, 1000);
    } else {
        result = installation::cleanup_readiness_exit_race_probe(input.value, process.value, writer.value, output);
    }
    check(result == expected, "process exit drains the final bounded stdout line before strict readiness classification");
}

void cleanup_image_probe(const std::string& mode, bool expect_cleaned, bool expect_ready) {
    Fixture fixture;
    const auto parent = fixture.base / L"清理 $path ' & 空格";
    const std::wstring token = L"0123456789abcdef0123456789abcdef";
    auto folder = parent / (L"SZUConnect-Uninstall-" + token);
    const auto helper = folder / L"SZUConnect.exe";
    fs::create_directories(folder);
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error("cannot resolve test image");
    const auto source = fs::path(std::wstring(path.data(), length));
    if (!CopyFileW(source.c_str(), helper.c_str(), TRUE)) throw std::runtime_error("cannot copy test image");
    const auto expected_digest = installation::file_sha256(helper);
    if (mode == "unknown") write_file(folder / L"keep.txt", "unrelated fixture data\n");
    fs::path junction_target;
    if (mode == "junction") {
        junction_target = fixture.base / L"retain-target";
        fs::rename(folder, junction_target);
        if (!make_junction(folder, junction_target)) { skip_link("temporary filesystem cannot create a junction"); return; }
    }
    NativeHandle locked;
    if (mode == "locked") {
        locked.value = CreateFileW(helper.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (locked.value == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot lock isolated image");
    }
    SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE};
    NativeHandle done, passed;
    done.value = CreateEventW(&security, TRUE, FALSE, nullptr); passed.value = CreateEventW(&security, TRUE, FALSE, nullptr);
    if (!done.value || !passed.value) throw std::runtime_error("cannot create cleanup probe events");
    NativeHandle diagnostic_read, diagnostic_write, null_input;
    if (!CreatePipe(&diagnostic_read.value, &diagnostic_write.value, &security, 16384) ||
        !SetHandleInformation(diagnostic_read.value, HANDLE_FLAG_INHERIT, 0))
        throw std::runtime_error("cannot create cleanup diagnostic pipe");
    null_input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_input.value == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create cleanup fixture input");
    SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<BYTE> attributes_bytes(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_bytes.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) throw std::runtime_error("cannot initialize probe attributes");
    struct DeleteAttributes { LPPROC_THREAD_ATTRIBUTE_LIST value; ~DeleteAttributes() { DeleteProcThreadAttributeList(value); } } delete_attributes{attributes};
    HANDLE inherited[]{done.value, passed.value, diagnostic_write.value, null_input.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, nullptr, nullptr))
        throw std::runtime_error("cannot inherit probe events");
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof startup; startup.lpAttributeList = attributes;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_input.value;
    startup.StartupInfo.hStdOutput = diagnostic_write.value;
    startup.StartupInfo.hStdError = diagnostic_write.value;
    std::wstring command = L"\"" + helper.native() + L"\" --cleanup-image-fixture " + std::wstring(mode.begin(), mode.end()) + L" " + token + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(done.value)) + L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(passed.value));
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, fixture.temp_parent.c_str(), &startup.StartupInfo, &process)) throw std::runtime_error("cannot start isolated image");
    diagnostic_write.close(); null_input.close();
    NativeHandle child, thread; child.value = process.hProcess; thread.value = process.hThread;
    const auto started = GetTickCount64();
    check(WaitForSingleObject(child.value, 30000) == WAIT_OBJECT_0, "isolated mapped image exits normally without a forced stop");
    DWORD exit_code = STILL_ACTIVE;
    const DWORD expected_exit = expect_ready ? 0u : mode == "stderr-unavailable" ? 66u : 6u;
    const bool exit_read = GetExitCodeProcess(child.value, &exit_code) != FALSE;
    const DWORD exit_error = exit_read ? ERROR_SUCCESS : GetLastError();
    const bool expected_status = exit_read && exit_code == expected_exit;
    if (!expected_status) {
        std::printf("Cleanup fixture: mode=%s stage=%s exit=%lu expected=%lu win32=%lu elapsed_ms=%llu\n",
            mode.c_str(), installation::cleanup_fixture_exit_stage(exit_code).c_str(), exit_code, expected_exit, exit_error,
            static_cast<unsigned long long>(GetTickCount64() - started));
        std::fflush(stdout);
    }
    std::string diagnostic;
    DWORD available = 0;
    while (diagnostic.size() < 4096 && PeekNamedPipe(diagnostic_read.value, nullptr, 0, nullptr, &available, nullptr) && available) {
        std::array<char, 512> buffer{}; DWORD read = 0;
        const auto count = static_cast<DWORD>(std::min<std::size_t>({buffer.size(), available, 4096 - diagnostic.size()}));
        if (!ReadFile(diagnostic_read.value, buffer.data(), count, &read, nullptr) || !read) break;
        diagnostic.append(buffer.data(), read);
    }
    if (!expected_status && !diagnostic.empty()) {
        std::printf("%s", diagnostic.c_str());
        std::fflush(stdout);
    }
    check(expected_status, "cleanup readiness matches the expected preflight result");
    check(WaitForSingleObject(done.value, 15000) == WAIT_OBJECT_0, "exit-time cleanup reaches a verified completion event");
    check((WaitForSingleObject(passed.value, 0) == WAIT_OBJECT_0) == expect_cleaned, "cleanup success is reported only after actual deletion");
    locked.close();
    if (expect_cleaned) {
        check(!fs::exists(helper) && !fs::exists(folder), "both the exited image and its random temporary directory are removed");
    } else {
        check(fs::exists(helper) && installation::file_sha256(helper) == expected_digest, "unsafe or unavailable cleanup retains the exact isolated image");
        if (mode == "unknown" || mode == "late-unknown")
            check(read_file(folder / L"keep.txt") == "unrelated fixture data\n", "unrecognized files remain byte-identical");
        if (mode == "junction") check(fs::exists(junction_target / L"SZUConnect.exe") && reparse_point(folder), "junction target and link are untouched");
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 6 && std::string_view(argv[1]) == "--cleanup-image-fixture") {
        const std::string token_text = argv[3];
        return installation::run_cleanup_image_fixture(argv[2], std::wstring(token_text.begin(), token_text.end()),
            static_cast<std::uintptr_t>(std::strtoull(argv[4], nullptr, 10)),
            static_cast<std::uintptr_t>(std::strtoull(argv[5], nullptr, 10)));
    }
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

    test_case("legacy documentation layout upgrades even with identical bytes", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        fixture.use_legacy_documents();
        const auto before = snapshot(fixture.current());
        check(installation::owned_payload(fixture.current()), "legacy layout retains strict ownership verification");
        fixture.hooks.reuse_running_current = true;
        const auto result = installation::run_transaction(fixture.hooks);
        check(result.success, "legacy layout updates through the normal transaction");
        check_complete(fixture);
        check(!fs::exists(fixture.current() / L"说明与许可"), "current payload contains only the new documentation directory");
        check(snapshot(fixture.previous()) == before, "legacy payload is preserved byte for byte as the rollback backup");
        check(installation::owned_payload(fixture.previous()), "legacy rollback backup remains recognized");
        check(result.stop_calls == 1 && result.apply_calls == 1 && result.launch_calls == 1 && result.activation_calls == 0,
            "legacy layout cannot bypass replacement through the identical-payload activation path");
    });

    for (const auto point : {FailurePoint::after_backup, FailurePoint::after_publish, FailurePoint::none}) {
        const auto* name = point == FailurePoint::after_backup ? "legacy layout is restored after backup failure" :
            point == FailurePoint::after_publish ? "legacy layout is restored after publish failure" :
            "legacy layout is restored after readiness failure";
        test_case(name, [point] {
            Fixture fixture;
            if (!fixture.install()) return;
            fixture.use_legacy_documents();
            const auto before = snapshot(fixture.current());
            fixture.hooks.fail_at = point;
            fixture.hooks.ready_succeeds = point != FailurePoint::none;
            const auto result = installation::run_transaction(fixture.hooks);
            rejected(result);
            check(snapshot(fixture.current()) == before, "failed migration restores every legacy payload byte and path");
            check(installation::owned_payload(fixture.current()), "restored legacy layout retains valid ownership");
            check(!fs::exists(fixture.documents()), "failed migration does not leave a second documentation directory");
        });
    }

    test_case("legacy documentation backup rotates after a later upgrade", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        fixture.use_legacy_documents();
        check(installation::run_transaction(fixture.hooks).success, "legacy layout migrates before later upgrade");
        const auto migrated = snapshot(fixture.current());
        fixture.write_source("second");
        check(installation::run_transaction(fixture.hooks).success, "later upgrade safely rotates the recognized legacy backup");
        check_complete(fixture);
        check(snapshot(fixture.previous()) == migrated, "backup rotation retains the complete immediately preceding payload");
    });

    for (const bool mixed_layout : {true, false}) {
        test_case(mixed_layout ? "mixed documentation layouts are rejected" : "unknown legacy documentation is preserved", [mixed_layout] {
            Fixture fixture;
            if (!fixture.install()) return;
            if (mixed_layout) {
                for (const auto& [name, content] : fixture.hooks.documents)
                    write_file(fixture.current() / L"说明与许可" / name, content);
            } else {
                fixture.use_legacy_documents();
                write_file(fixture.current() / L"说明与许可" / L"user-notes.txt", "Keep this unknown document.\n");
            }
            const auto before = snapshot(fixture.hooks.root);
            const auto result = installation::run_transaction(fixture.hooks);
            rejected(result);
            check(snapshot(fixture.hooks.root) == before, "unrecognized documentation is preserved without replacement");
            check(result.stop_calls == 0 && result.apply_calls == 0 && result.launch_calls == 0,
                "unrecognized documentation is rejected before stopping or changing the application");
        });
    }

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

    test_case("uninstall preserves data unless removal is selected", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        const auto data = fixture.base / L"本机数据";
        write_file(data / L"settings.json", "saved offline settings\n");
        write_file(data / L"unrecognized-user-file.txt", "keep this user file\n");
        const auto before = snapshot(data);
        installation::UninstallHooks hooks{fixture.hooks.root, data};
        const auto result = installation::run_uninstall_transaction(hooks);
        check(result.success && result.stop_calls == 1 && result.cleanup_calls == 1, "default uninstall removes the owned program and integration");
        check(result.erase_calls == 0 && snapshot(data) == before, "default uninstall neither accesses credentials nor removes any data");
        check(!fs::exists(fixture.hooks.root), "successful uninstall removes only the owned installation root");
        check(fs::exists(fixture.hooks.source_executable), "downloaded executable remains user-owned");
    });
    test_case("explicit uninstall clears known data and fixed credential target", [] {
        Fixture fixture;
        if (!fixture.install()) return;
        const auto data = fixture.base / L"本机数据";
        for (const auto* name : {L"settings.json", L"recovery.json", L"network-memory.json", L"network-labels.json", L"wifi-recovery.json",
            L"websites.json", L"events.json", L"health.json", L"proxy-assistant.backup", L"events.json.corrupt-20261018-123456",
            L"settings.json.tmp-123-456", L"proxy-assistant.backup.tmp-123-456", L"websites.json.corrupt-123-456-1", L"websites.json.corrupt-123-456-1.tmp-123-789"})
            write_file(data / name, "isolated fake data only\n");
        installation::UninstallHooks hooks{fixture.hooks.root, data}; hooks.remove_data = true;
        const auto result = installation::run_uninstall_transaction(hooks);
        check(result.success && result.erase_calls == 1, "only explicit removal invokes the mock credential erasure once");
        check(!fs::exists(data) && !fs::exists(fixture.hooks.root), "explicit cleanup removes known data and owned program");
    });
    test_case("unknown data prevents destructive cleanup", [] {
        Fixture fixture; if (!fixture.install()) return;
        const auto data = fixture.base / L"本机数据";
        write_file(data / L"settings.json", "keep settings\n"); write_file(data / L"notes.txt", "keep unknown\n");
        const auto program = snapshot(fixture.hooks.root), before = snapshot(data);
        installation::UninstallHooks hooks{fixture.hooks.root, data}; hooks.remove_data = true;
        const auto result = installation::run_uninstall_transaction(hooks);
        check(!result.success && result.erase_calls == 0 && result.cleanup_calls == 0, "unknown data is rejected before erasure or integration removal");
        check(snapshot(data) == before && snapshot(fixture.hooks.root) == program, "unknown data and program remain available for retry");
    });
    test_case("credential erasure failure preserves program and data", [] {
        Fixture fixture; if (!fixture.install()) return;
        const auto data = fixture.base / L"本机数据"; write_file(data / L"settings.json", "keep settings\n");
        const auto program = snapshot(fixture.hooks.root), before = snapshot(data);
        installation::UninstallHooks hooks{fixture.hooks.root, data}; hooks.remove_data = true; hooks.erase_succeeds = false;
        const auto result = installation::run_uninstall_transaction(hooks);
        check(!result.success && result.erase_calls == 1 && result.cleanup_calls == 0, "failed erasure retains the uninstall registration for retry");
        check(snapshot(data) == before && snapshot(fixture.hooks.root) == program, "credential failure does not partially uninstall or clear settings");
    });
    test_case("data lock prevents credential erasure and program removal", [] {
        Fixture fixture; if (!fixture.install()) return;
        const auto data = fixture.base / L"本机数据"; write_file(data / L"events.json", "keep fake records\n");
        const auto program = snapshot(fixture.hooks.root);
        NativeHandle locked{CreateFileW((data / L"events.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
        if (locked.value == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot lock isolated data file");
        installation::UninstallHooks hooks{fixture.hooks.root, data}; hooks.remove_data = true;
        const auto result = installation::run_uninstall_transaction(hooks); locked.close();
        check(!result.success && result.erase_calls == 0 && snapshot(fixture.hooks.root) == program, "locked data fails preflight without erasing the account");
    });
    test_case("normal exit failure prevents every uninstall write", [] {
        Fixture fixture; if (!fixture.install()) return;
        const auto program = snapshot(fixture.hooks.root);
        installation::UninstallHooks hooks{fixture.hooks.root, fixture.base / L"data"}; hooks.remove_data = true; hooks.stop_succeeds = false;
        const auto result = installation::run_uninstall_transaction(hooks);
        check(!result.success && result.stop_calls == 1 && result.erase_calls == 0 && result.cleanup_calls == 0, "exit timeout does not force termination or remove integration");
        check(snapshot(fixture.hooks.root) == program, "failed normal shutdown leaves the full program intact");
    });
    test_case("uninstall rejects unrecognized program files and altered manifest", [] {
        Fixture fixture; if (!fixture.install()) return;
        write_file(fixture.current() / L"unknown.txt", "do not delete\n");
        const auto before = snapshot(fixture.hooks.root);
        installation::UninstallHooks hooks{fixture.hooks.root, fixture.base / L"data"};
        auto result = installation::run_uninstall_transaction(hooks);
        check(!result.success && result.stop_calls == 0 && snapshot(fixture.hooks.root) == before, "unknown program content cannot establish deletion ownership");
        fs::remove(fixture.current() / L"unknown.txt"); write_file(fixture.manifest(), "forged manifest\n");
        const auto forged = snapshot(fixture.hooks.root); result = installation::run_uninstall_transaction(hooks);
        check(!result.success && result.stop_calls == 0 && snapshot(fixture.hooks.root) == forged, "altered ownership manifest cannot authorize uninstall");
    });
    test_case("partial program deletion restores the working installation", [] {
        Fixture fixture; if (!fixture.install()) return;
        fixture.write_source("second"); check(installation::run_transaction(fixture.hooks).success, "isolated update creates owned previous payload");
        const auto before = snapshot(fixture.hooks.root);
        installation::UninstallHooks hooks{fixture.hooks.root, fixture.base / L"data"}; hooks.fail_after_first_payload_file = true;
        const auto result = installation::run_uninstall_transaction(hooks);
        check(!result.success && result.cleanup_calls == 0 && snapshot(fixture.hooks.root) == before, "injected deletion failure restores current, previous and manifests byte for byte");
        check(installation::owned_payload(fixture.current()), "restored current remains eligible for a normal retry");
    });
    test_case("integration failure restores the full owned program", [] {
        Fixture fixture; if (!fixture.install()) return;
        const auto before = snapshot(fixture.hooks.root);
        installation::UninstallHooks hooks{fixture.hooks.root, fixture.base / L"data"}; hooks.cleanup_succeeds = false;
        const auto result = installation::run_uninstall_transaction(hooks);
        check(!result.success && result.cleanup_calls == 1 && snapshot(fixture.hooks.root) == before, "integration failure retains a runnable uninstaller for retry");
    });
    test_case("legacy license directory can be uninstalled safely", [] {
        Fixture fixture; if (!fixture.install()) return; fixture.use_legacy_documents();
        installation::UninstallHooks hooks{fixture.hooks.root, fixture.base / L"data"};
        check(installation::run_uninstall_transaction(hooks).success && !fs::exists(fixture.hooks.root), "legacy fixed-file payload remains recognized for uninstall");
    });
    test_case("uninstall refuses a junction data root", [] {
        Fixture fixture; if (!fixture.install()) return;
        const auto data = fixture.base / L"data", outside = fixture.base / L"outside";
        write_file(outside / L"settings.json", "outside data remains untouched\n");
        if (!make_junction(data, outside)) { skip_link("temporary filesystem cannot create a junction"); return; }
        installation::UninstallHooks hooks{fixture.hooks.root, data}; hooks.remove_data = true;
        const auto result = installation::run_uninstall_transaction(hooks);
        check(!result.success && result.erase_calls == 0 && fs::exists(fixture.executable()), "junction data cannot trigger credentials or program removal");
        check(read_file(outside / L"settings.json") == "outside data remains untouched\n", "junction target is not modified");
    });
    test_case("data file suffixes and internal worker requests are strict", [] {
        for (const auto* name : {L"passwords.json", L"settings.json.bak", L"settings.json.tmp-1", L"settings.json.tmp-1-2-3",
            L"settings.json.corrupt-abc", L"settings.json.corrupt-1-2-extra", L"../settings.json", L"settings.json:stream"})
            check(!installation::data_filename_owned(name), "unrecognized data suffix cannot authorize deletion");
        const std::wstring token(32, L'a');
        Fixture fixture;
        check(!installation::worker_request_valid(token, fixture.base / L"SZUConnect.exe", fixture.base), "ordinary executable cannot act as a temporary worker");
        check(!installation::worker_request_valid(L"../../bad", fixture.base / L"SZUConnect.exe", fixture.base), "malformed nonce is rejected");
        check(szu::app::run_uninstaller(nullptr, token) != 0, "forged CLI worker exits silently before formal runtime services");
    });

    test_case("registry deletion failure restores owned registration for retry", [] {
        Fixture fixture;
        for (int failure = 1; failure <= 5; ++failure)
            check(installation::registration_failure_recoverable(fixture.hooks.root, failure, false),
                "each registry value deletion and final key deletion failure preserves the full registered uninstaller");
        check(installation::registration_failure_recoverable(fixture.hooks.root, -1, false),
            "concurrent registry additions abort cleanup and remain intact");
        for (int failure = 1; failure <= 2; ++failure)
            check(installation::registration_failure_recoverable(fixture.hooks.root, failure, true),
                "registration rollback never deletes the existing owner or uninstall command");
    });
    test_case("uncommitted handoff cannot become a late uninstall", [] {
        check(installation::handoff_failure_is_cancelled(false, true), "ready timeout commits cancellation even when the worker later signals ready");
        check(installation::handoff_failure_is_cancelled(false, false), "ready and decision signal failure preserve cancellation");
        check(installation::handoff_failure_is_cancelled(true, false), "failed decision signal cancels an otherwise ready worker");
    });
    test_case("cleanup readiness waits for bounded cold startup", [] {
        cleanup_readiness_probe("SZU-CLEANUP-READY-1\r\n", 120, 1000, "ready");
    });
    test_case("cleanup readiness rejects late and invalid protocol output", [] {
        cleanup_readiness_probe("SZU-CLEANUP-READY-1\n", 240, 50, "timeout");
        cleanup_readiness_probe("SZU-CLEANUP-UNAVAILABLE-1\n", 0, 1000, "unavailable");
        cleanup_readiness_probe("unrelated fixture output\nSZU-CLEANUP-READY-1\n", 0, 1000, "invalid-output");
        cleanup_readiness_probe(std::string(1025, 'x'), 0, 1000, "invalid-output");
    });
    test_case("cleanup readiness drains output that races with process exit", [] {
        cleanup_exit_output_probe("SZU-CLEANUP-READY-1\r\n", "ready");
        cleanup_exit_output_probe("SZU-CLEANUP-UNAVAILABLE-1\n", "unavailable");
        cleanup_exit_output_probe("unrelated fixture output\nSZU-CLEANUP-READY-1\n", "invalid-output");
        cleanup_exit_output_probe(std::string(1025, 'x'), "invalid-output");
        cleanup_exit_output_probe("", "process-exited");
        cleanup_exit_output_probe("SZU-CLEANUP-READY-1", "process-exited");
    });
    test_case("cleanup readiness drains buffered output from an exited process", [] {
        cleanup_exit_output_probe("SZU-CLEANUP-READY-1\n", "ready", true);
        cleanup_exit_output_probe("SZU-CLEANUP-UNAVAILABLE-1\r\n", "unavailable", true);
        cleanup_exit_output_probe("unrelated fixture output\n", "invalid-output", true);
    });
    test_case("running temporary image is cleaned after normal exit", [] { cleanup_image_probe("success", true, true); });
    test_case("startup stderr does not corrupt validated cleanup readiness", [] { cleanup_image_probe("stderr-prefix", true, true); });
    test_case("stderr ready text cannot override an unavailable stdout result", [] { cleanup_image_probe("stderr-unavailable", false, false); });
    test_case("cancelled handoff still cleans only its temporary image", [] { cleanup_image_probe("cancel", true, true); });
    test_case("disconnected precommit pipe cleans the abandoned image", [] { cleanup_image_probe("disconnect", true, true); });
    test_case("malformed precommit message cannot strand its image", [] { cleanup_image_probe("malformed-phase", true, true); });
    test_case("missing final result cannot prevent temporary cleanup", [] { cleanup_image_probe("lost-result", true, true); });
    test_case("foreign file introduced after ready blocks all cleanup", [] { cleanup_image_probe("late-unknown", false, true); });
    test_case("unknown temporary files fail cleanup preflight", [] { cleanup_image_probe("unknown", false, false); });
    test_case("occupied temporary image fails cleanup preflight", [] { cleanup_image_probe("locked", false, false); });
    test_case("unavailable system cleaner fails before uninstall", [] { cleanup_image_probe("unavailable", false, false); });
    test_case("temporary cleanup refuses a junction", [] { cleanup_image_probe("junction", false, false); });
    test_case("production preflight accepts protected identical helper after READY", [] { cleanup_image_probe("guard-valid", true, true); });
    test_case("production preflight rejects unknown installed files after READY", [] { cleanup_image_probe("guard-foreign", true, true); });
    test_case("production preflight rejects a forged installed image after READY", [] { cleanup_image_probe("guard-forged", true, true); });
    test_case("production preflight rejects a foreign registration owner after READY", [] { cleanup_image_probe("guard-owner", true, true); });

    std::printf("Installation transactions: %d cases, %d checks, %d failures, %d skipped reparse cases.\n",
        cases, checks, failures, skipped);
    return failures == 0 ? 0 : 1;
}
