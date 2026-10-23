#include "szu/win/proxy_assistant.hpp"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <winioctl.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using szu::win::ProxyConfigTransaction;
using szu::proxyassist::Format;
int checks = 0, failures = 0, unavailable = 0;
void check(bool value, const char* label) { ++checks; if (!value) { ++failures; std::printf("FAIL: %s\n", label); } }
fs::path existing_long_path(const std::wstring& input) {
    const auto required = GetLongPathNameW(input.c_str(), nullptr, 0);
    if (!required || required > 32768) return {};
    std::wstring buffer(required, L'\0');
    const auto length = GetLongPathNameW(input.c_str(), buffer.data(), required);
    if (!length || length >= required) return {};
    buffer.resize(length);
    fs::path path(buffer);
    while (path.has_relative_path() && path.filename().empty()) path = path.parent_path();
    return path;
}
fs::path fixture_temp() {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (!length || length >= buffer.size()) return {};
    // TEMP may contain a profile's 8.3 alias. Expand names only; reparse points
    // remain visible to the production transaction's strict directory checks.
    return existing_long_path(std::wstring(buffer.data(), length));
}
std::string read(const fs::path& path) { std::ifstream f(path, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
bool write(const fs::path& path, std::string_view text) { std::ofstream f(path, std::ios::binary | std::ios::trunc); f.write(text.data(), static_cast<std::streamsize>(text.size())); return f.good(); }
constexpr std::string_view kMarker = "\n// fixture direct configuration\n";
szu::proxyassist::Patch transform(std::string_view text) {
    szu::proxyassist::Patch patch;
    patch.text = std::string(text);
    patch.changed = !text.ends_with(kMarker);
    if (patch.changed) patch.text += kMarker;
    patch.changes = {"directRules"};
    return patch;
}
struct Fixture {
    fs::path root, data, client, target;
    std::string original = "// fixture-token-private\r\nfunction main(config) { return config; }\r\n";
    Fixture() {
        const auto temp = fixture_temp();
        if (temp.empty()) throw 1;
        root = temp / (L"SZUProxyFixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) +
            L"-" + std::to_wstring(++sequence));
        data = root / L"managed"; client = root / L"client"; target = client / L"global.js";
        fs::create_directories(data); fs::create_directories(client);
        if (!write(target, original)) throw 2;
    }
    ~Fixture() {
        // Fixtures use only one explicitly named temporary subtree. Never follow an
        // unexpected reparse point or remove a path outside that exact parent.
        const auto temp = fixture_temp();
        const auto attrs = GetFileAttributesW(root.c_str());
        if (temp.empty() || root.parent_path().lexically_normal() != temp ||
            !root.filename().native().starts_with(L"SZUProxyFixture-") || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return;
        std::error_code ec;
        for (fs::recursive_directory_iterator i(root, fs::directory_options::none, ec), end; !ec && i != end; ++i) {
            const auto flags = GetFileAttributesW(i->path().c_str());
            if (flags & FILE_ATTRIBUTE_REPARSE_POINT) return;
            if (flags & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(i->path().c_str(), flags & ~FILE_ATTRIBUTE_READONLY);
        }
        fs::remove_all(root, ec);
    }
    static inline unsigned sequence = 0;
};
class TemporaryEnvironment {
    static std::optional<std::wstring> read_value(const wchar_t* name) {
        const auto required = GetEnvironmentVariableW(name, nullptr, 0);
        if (!required) return {};
        std::wstring text(required, L'\0');
        const auto length = GetEnvironmentVariableW(name, text.data(), required);
        if (!length || length >= required) return {};
        text.resize(length); return text;
    }
    std::optional<std::wstring> tmp_ = read_value(L"TMP"), temp_ = read_value(L"TEMP");
public:
    bool ready = false;
    explicit TemporaryEnvironment(const std::wstring& path) {
        const bool tmp_set = SetEnvironmentVariableW(L"TMP", path.c_str()) != FALSE;
        const bool temp_set = SetEnvironmentVariableW(L"TEMP", path.c_str()) != FALSE;
        ready = tmp_set && temp_set;
    }
    ~TemporaryEnvironment() {
        SetEnvironmentVariableW(L"TMP", tmp_ ? tmp_->c_str() : nullptr);
        SetEnvironmentVariableW(L"TEMP", temp_ ? temp_->c_str() : nullptr);
    }
};

class DenyCreate {
    fs::path path_;
    PSECURITY_DESCRIPTOR original_ = nullptr;
    PACL original_acl_ = nullptr;
    bool protected_ = false;
public:
    bool ready = false;
    explicit DenyCreate(fs::path path) : path_(std::move(path)) {
        if (GetNamedSecurityInfoW(const_cast<LPWSTR>(path_.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &original_acl_, nullptr, &original_) != ERROR_SUCCESS) return;
        SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
        if (!GetSecurityDescriptorControl(original_, &control, &revision)) return;
        protected_ = (control & SE_DACL_PROTECTED) != 0;
        BYTE world[SECURITY_MAX_SID_SIZE]{}; DWORD size = sizeof world;
        if (!CreateWellKnownSid(WinWorldSid, nullptr, world, &size)) return;
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_ADD_FILE;
        access.grfAccessMode = DENY_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(world);
        PACL updated = nullptr;
        if (SetEntriesInAclW(1, &access, original_acl_, &updated) != ERROR_SUCCESS) return;
        ready = SetNamedSecurityInfoW(const_cast<LPWSTR>(path_.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr) == ERROR_SUCCESS;
        LocalFree(updated);
    }
    ~DenyCreate() {
        if (ready) SetNamedSecurityInfoW(const_cast<LPWSTR>(path_.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | (protected_ ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
            nullptr, nullptr, original_acl_, nullptr);
        if (original_) LocalFree(original_);
    }
};
std::wstring security(const fs::path& path, bool protect = false) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL acl = nullptr;
    constexpr auto requested = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    if (GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, requested, nullptr, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS) return {};
    if (protect) {
        const auto result = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
        LocalFree(descriptor);
        return result == ERROR_SUCCESS ? security(path) : std::wstring{};
    }
    LPWSTR text = nullptr;
    const auto converted = ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1, requested, &text, nullptr);
    const auto value = converted ? std::wstring(text) : std::wstring{};
    if (text) LocalFree(text);
    LocalFree(descriptor);
    return value;
}
bool junction(const fs::path& link, const fs::path& target, DWORD access = GENERIC_WRITE, bool create = true) {
    if (create && !CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE file = CreateFileW(link.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) { if (create) RemoveDirectoryW(link.c_str()); return false; }
    const std::wstring substitute = L"\\??\\" + target.native(), print = target.native();
    const auto payload = (substitute.size() + print.size() + 2) * sizeof(wchar_t);
    std::vector<BYTE> bytes(16 + payload);
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    const WORD length = static_cast<WORD>(8 + payload), reserved = 0, sub_offset = 0;
    const WORD sub_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    const WORD print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t)), print_length = static_cast<WORD>(print.size() * sizeof(wchar_t));
    std::memcpy(bytes.data(), &tag, 4); std::memcpy(bytes.data() + 4, &length, 2); std::memcpy(bytes.data() + 6, &reserved, 2);
    std::memcpy(bytes.data() + 8, &sub_offset, 2); std::memcpy(bytes.data() + 10, &sub_length, 2);
    std::memcpy(bytes.data() + 12, &print_offset, 2); std::memcpy(bytes.data() + 14, &print_length, 2);
    std::memcpy(bytes.data() + 16, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(bytes.data() + 16 + print_offset, print.c_str(), (print.size() + 1) * sizeof(wchar_t));
    DWORD done = 0;
    const bool result = DeviceIoControl(file, FSCTL_SET_REPARSE_POINT, bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, 0, &done, nullptr) != FALSE;
    CloseHandle(file);
    if (!result && create) RemoveDirectoryW(link.c_str());
    return result;
}
bool isolated_data_path(const fs::path& data) {
    const auto temp = fixture_temp();
    const auto root = data.parent_path();
    return !temp.empty() && data.filename() == L"managed" &&
        root.parent_path().lexically_normal() == temp &&
        root.filename().native().starts_with(L"SZUProxyFixture-") &&
        !(GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool restore_in_new_process(const fs::path& data) {
    if (!isolated_data_path(data)) return false;
    std::vector<wchar_t> module(32768);
    const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!length || length >= module.size()) return false;
    const std::wstring exe(module.data(), length);
    std::wstring command = L"\"" + exe + L"\" --restore-test \"" + data.native() + L"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return false;
    CloseHandle(process.hThread);
    const auto waited = WaitForSingleObject(process.hProcess, 15000);
    DWORD code = 1;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    else { TerminateProcess(process.hProcess, 2); WaitForSingleObject(process.hProcess, 5000); }
    CloseHandle(process.hProcess);
    return waited == WAIT_OBJECT_0 && code == 0;
}
void run() {
    {
        Fixture parent;
        const auto long_temp = parent.root / L"ordinary long temporary directory";
        fs::create_directory(long_temp);
        const auto required = GetShortPathNameW(long_temp.c_str(), nullptr, 0);
        std::wstring short_temp(required, L'\0');
        const auto length = required ? GetShortPathNameW(long_temp.c_str(), short_temp.data(), required) : 0;
        if (length && length < required) short_temp.resize(length); else short_temp.clear();
        if (!short_temp.empty() && short_temp != long_temp.native()) {
            TemporaryEnvironment environment(short_temp);
            check(environment.ready, "set only the native test process TEMP and TMP to an existing 8.3 alias");
            if (environment.ready) {
                Fixture aliased;
                check(aliased.root.parent_path() == long_temp, "fixture expands the actual TEMP 8.3 ancestor to its existing long name");
                ProxyConfigTransaction tx(aliased.data.native());
                check(tx.prepare(aliased.target.native(), Format::clashVergeScript, transform).ready && tx.apply().success &&
                    restore_in_new_process(aliased.data) && read(aliased.target) == aliased.original && !tx.has_backup(),
                    "short TEMP environments support canonical preview atomic apply and cross-process encrypted recovery");
                const auto selected_required = GetShortPathNameW(aliased.target.c_str(), nullptr, 0);
                std::wstring selected_alias(selected_required, L'\0');
                const auto selected_length = selected_required ? GetShortPathNameW(aliased.target.c_str(), selected_alias.data(), selected_required) : 0;
                if (selected_length && selected_length < selected_required) selected_alias.resize(selected_length); else selected_alias.clear();
                check(!selected_alias.empty() && selected_alias != aliased.target.native() &&
                    tx.prepare(selected_alias, Format::clashVergeScript, transform).error == "invalidPath" && !tx.has_backup() &&
                    read(aliased.target) == aliased.original,
                    "product admission rejects a directly supplied short alias without changing the selected long-name file");
                check(tx.prepare(aliased.target.native(), Format::clashVergeScript, transform).ready,
                    "the same actual file remains supported through its canonical long path");
            }
        } else { ++unavailable; std::printf("UNAVAILABLE: filesystem did not expose a distinct 8.3 fixture alias\n"); }
    }
    {
        Fixture f;
        const auto acl = security(f.target, true);
        check(!acl.empty(), "capture protected original owner/group/DACL");
        {
            ProxyConfigTransaction tx(f.data.native());
            const auto preview = tx.prepare(f.target.native(), Format::clashVergeScript, transform);
            check(preview.ready && preview.changed && preview.error.empty() && preview.filename == "global.js", "prepare returns only filename and fixed changes");
            check(read(f.target) == f.original && !tx.has_backup(), "preview makes no target or backup changes");
            const auto result = tx.apply();
            check(result.success && result.changed && read(f.target) == f.original + std::string(kMarker), "apply atomically installs the prepared bytes");
            check(security(f.target) == acl, "apply preserves original owner group and exact protected DACL");
            const auto encrypted = read(f.data / L"proxy-assistant.backup");
            check(tx.has_backup() && !encrypted.empty() && encrypted.find("fixture-token-private") == std::string::npos &&
                encrypted.find("global.js") == std::string::npos, "backup is DPAPI encrypted and contains no plaintext token or path");
            const auto again = tx.prepare(f.target.native(), Format::clashVergeScript, transform);
            check(again.ready && !again.changed, "repeated application to the same bytes is admitted as already applied");
            const auto repeated = tx.apply();
            check(repeated.success && !repeated.changed && encrypted == read(f.data / L"proxy-assistant.backup"), "already-applied operation does not rewrite the first backup");
            const auto other = f.client / L"other.js"; write(other, f.original);
            check(tx.prepare(other.native(), Format::clashVergeScript, transform).error == "backupExists" && read(other) == f.original &&
                encrypted == read(f.data / L"proxy-assistant.backup"), "another target cannot overwrite pending recovery material");
        }
        ProxyConfigTransaction restarted(f.data.native());
        check(restarted.has_backup(), "a new transaction detects the same user's persistent DPAPI backup");
        check(restore_in_new_process(f.data) && read(f.target) == f.original && !restarted.has_backup(), "a new native process decrypts restores and clears only its own backup");
        check(security(f.target) == acl, "restore preserves the original owner group and DACL");
    }
    {
        Fixture f; f.original.insert(0, "\xef\xbb\xbf"); write(f.target, f.original);
        ProxyConfigTransaction tx(f.data.native()); bool saw_bom = false;
        const auto preview = tx.prepare(f.target.native(), Format::clashVergeScript, [&](std::string_view text) { saw_bom = text.starts_with("\xef\xbb\xbf"); return transform(text); });
        check(preview.ready && !saw_bom && tx.apply().success && read(f.target).starts_with("\xef\xbb\xbf"), "UTF-8 BOM is preserved while the pure transform receives decoded UTF-8 bytes");
        check(tx.restore().success && read(f.target) == f.original, "restore keeps exact original BOM and CRLF bytes");
    }
    {
        Fixture f;
        const auto neighbour = f.client / L"\ufffd.js";
        check(write(neighbour, f.original), "create the same-byte valid replacement-character neighbour");
        ProxyConfigTransaction tx(f.data.native());
        for (const auto surrogate : {0xd800, 0xdfff}) {
            const auto malformed = f.client / (std::wstring(1, static_cast<wchar_t>(surrogate)) + L".js");
            check(write(malformed, f.original), "create only an isolated NTFS malformed-UTF16 filename fixture");
            bool transformed = false;
            const auto preview = tx.prepare(malformed.native(), Format::clashVergeScript, [&](std::string_view text) {
                transformed = true; return transform(text);
            });
            check(preview.error == "invalidPath" && !transformed && !tx.has_backup() &&
                read(malformed) == f.original && read(neighbour) == f.original,
                "unpaired UTF-16 is rejected before transform and never redirects recovery to its same-hash neighbour");
        }
        const auto paired = f.client / (std::wstring(L"paired-") + static_cast<wchar_t>(0xd83d) + static_cast<wchar_t>(0xde00) + L".js");
        check(write(paired, f.original) && tx.prepare(paired.native(), Format::clashVergeScript, transform).ready &&
            tx.apply().success && tx.restore().success && read(paired) == f.original && read(neighbour) == f.original,
            "a valid surrogate pair round-trips through the encrypted recovery path without changing neighbours");
    }
    {
        Fixture f;
        const auto redirected = f.root / L"redirected"; fs::create_directory(redirected);
        bool injected = false;
        ProxyConfigTransaction tx(f.data.native(), {{[&] {
            injected = junction(f.data, redirected, FILE_WRITE_ATTRIBUTES, false);
        }}});
        check(tx.prepare(f.target.native(), Format::clashVergeScript, transform).ready && !injected,
            "preview neither invokes write-race injection nor creates a backup");
        const auto result = tx.apply();
        check(injected && result.error == "backupFailed" && read(f.target) == f.original && fs::is_empty(redirected),
            "a minimum-write-attributes junction race cannot redirect backup bytes or modify the selected config");
        check(RemoveDirectoryW(f.data.c_str()) && CreateDirectoryW(f.data.c_str(), nullptr), "remove only the isolated injected junction and restore its empty directory");
    }
    {
        Fixture f; ProxyConfigTransaction tx(f.data.native());
        check(tx.prepare(f.target.native(), Format::clashVergeScript, transform).ready, "prepare concurrent-edit fixture");
        write(f.target, "// fixture user edit");
        check(tx.apply().error == "changed" && read(f.target) == "// fixture user edit" && !tx.has_backup(), "an edit after preview prevents any write or backup");
    }
    {
        Fixture f; ProxyConfigTransaction tx(f.data.native());
        tx.prepare(f.target.native(), Format::clashVergeScript, transform);
        const auto replacement = f.client / L"replacement.js"; write(replacement, f.original);
        fs::remove(f.target); fs::rename(replacement, f.target);
        check(tx.apply().error == "changed" && !tx.has_backup(), "a different file identity with equal original bytes is rejected");
    }
    {
        Fixture f; ProxyConfigTransaction tx(f.data.native()); tx.prepare(f.target.native(), Format::clashVergeScript, transform);
        HANDLE lock = CreateFileW(f.target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(lock != INVALID_HANDLE_VALUE, "create independent deny-delete file lock");
        const auto result = tx.apply();
        check(!result.success && read(f.target) == f.original && !tx.has_backup(), "a conflicting external handle keeps the original file untouched");
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    }
    {
        Fixture f; ProxyConfigTransaction tx(f.data.native());
        SetFileAttributesW(f.target.c_str(), FILE_ATTRIBUTE_READONLY);
        check(!tx.prepare(f.target.native(), Format::clashVergeScript, transform).ready && read(f.target) == f.original, "read-only targets are rejected");
        SetFileAttributesW(f.target.c_str(), FILE_ATTRIBUTE_NORMAL);
        check(tx.prepare(f.target.native(), Format::clashVergeScript, transform).ready, "prepare late read-only fixture");
        SetFileAttributesW(f.target.c_str(), FILE_ATTRIBUTE_READONLY);
        check(tx.apply().error == "changed" && read(f.target) == f.original && !tx.has_backup(), "a target made read-only after preview is not replaced");
        SetFileAttributesW(f.target.c_str(), FILE_ATTRIBUTE_NORMAL);
        const auto link = f.client / L"hard-link.js";
        check(CreateHardLinkW(link.c_str(), f.target.c_str(), nullptr) != FALSE, "create same-volume hard-link fixture");
        check(!tx.prepare(f.target.native(), Format::clashVergeScript, transform).ready && !tx.prepare(link.native(), Format::clashVergeScript, transform).ready,
            "both names of a multiple-link target are rejected");
        fs::remove(link);
        const auto linked_parent = f.root / L"junction.js";
        if (junction(linked_parent, f.client)) {
            check(tx.prepare((linked_parent / L"global.js").native(), Format::clashVergeScript, transform).error == "invalidPath", "junction ancestors are rejected before reading client content");
            check(tx.prepare(linked_parent.native(), Format::clashVergeScript, transform).error == "unsupportedFile", "a selected reparse point is rejected even with a matching extension");
            RemoveDirectoryW(linked_parent.c_str());
        } else { ++unavailable; std::printf("UNAVAILABLE: local junction fixture creation\n"); }
    }
    {
        Fixture f; ProxyConfigTransaction tx(f.data.native());
        for (const auto& path : {L"\\\\localhost\\C$\\config.js", L"\\\\?\\C:\\config.js", L"C:\\NUL.js", L"C:\\NUL .js", L"C:\\COM\u00b9.js", L"C:\\CONOUT$.js", L"C:\\temp\\..\\config.js"})
            check(tx.prepare(path, Format::clashVergeScript, transform).error == "invalidPath", "UNC device reserved and traversal paths are rejected without I/O");
        check(tx.prepare(f.target.native() + L":stream", Format::clashVergeScript, transform).error == "invalidPath", "alternate data streams are rejected");
        check(tx.prepare(f.target.native(), Format::singBoxJson, transform).error == "unsupportedFile", "selected format and file extension must agree");
        write(f.target, std::string(szu::proxyassist::kMaxConfigBytes + 1, 'a'));
        check(tx.prepare(f.target.native(), Format::clashVergeScript, transform).error == "tooLarge", "oversized originals are rejected before transform");
        write(f.target, std::string("\xff\xfe"));
        check(tx.prepare(f.target.native(), Format::clashVergeScript, transform).error == "encoding", "UTF-16 and invalid UTF-8 are not silently rewritten");
    }
    {
        Fixture f; ProxyConfigTransaction tx(f.data.native()); tx.prepare(f.target.native(), Format::clashVergeScript, transform);
        DenyCreate denied(f.data); check(denied.ready, "restrict backup-directory creation in the isolated fixture");
        if (denied.ready) check(tx.apply().error == "backupFailed" && read(f.target) == f.original && !tx.has_backup(), "backup write failure leaves original bytes untouched");
    }
    {
        Fixture f; ProxyConfigTransaction tx(f.data.native()); tx.prepare(f.target.native(), Format::clashVergeScript, transform);
        DenyCreate denied(f.client); check(denied.ready, "restrict target-directory creation in the isolated fixture");
        if (denied.ready) {
            check(tx.apply().error == "writeFailed" && read(f.target) == f.original && tx.has_backup(), "target write failure retains original bytes and encrypted recovery material");
            const auto restored = tx.restore();
            check(restored.success && !restored.changed && !tx.has_backup(), "a failed replacement's backup can be cleared after confirming original bytes");
        }
    }
    {
        Fixture f; ProxyConfigTransaction tx(f.data.native()); tx.prepare(f.target.native(), Format::clashVergeScript, transform);
        check(tx.apply().success, "install rollback-conflict fixture");
        const auto saved = read(f.data / L"proxy-assistant.backup");
        write(f.target, "// fixture subsequent user edit");
        check(tx.restore().error == "restoreConflict" && read(f.target) == "// fixture subsequent user edit" &&
            read(f.data / L"proxy-assistant.backup") == saved, "undo never overwrites subsequent user edits or its original backup");
        check(tx.prepare(f.target.native(), Format::clashVergeScript, transform).error == "backupExists", "reapplying after edits cannot replace the oldest backup");
    }
    {
        Fixture f; ProxyConfigTransaction tx(f.data.native());
        check(tx.discard_backup().error == "backupMissing", "discard reports an absent backup without touching a selected file");
        const auto other_data = f.data / L"opaque.data"; write(other_data, "fixture retained app data");
        tx.prepare(f.target.native(), Format::clashVergeScript, transform);
        check(tx.apply().success, "install the explicit-discard fixture");
        write(f.target, "// fixture subsequent user edit");
        HANDLE target_lock = CreateFileW(f.target.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(target_lock != INVALID_HANDLE_VALUE, "hold the edited external target exclusively during explicit discard");
        const auto discarded = tx.discard_backup();
        if (target_lock != INVALID_HANDLE_VALUE) CloseHandle(target_lock);
        check(discarded.success && discarded.changed && !tx.has_backup() &&
            read(f.target) == "// fixture subsequent user edit" && read(other_data) == "fixture retained app data",
            "explicit discard deletes only the validated own backup without reading the locked target or other app data");
        const auto backup = f.data / L"proxy-assistant.backup";
        write(backup, "fixture foreign file");
        check(tx.discard_backup().error == "backupCorrupt" && read(backup) == "fixture foreign file" &&
            read(other_data) == "fixture retained app data", "discard preserves an unknown file even at its fixed backup filename");
        fs::remove(backup);
        const auto other = f.root / L"other"; fs::create_directory(other); write(other / L"marker", "fixture other retained");
        if (junction(backup, other)) {
            check(!tx.discard_backup().success && read(other / L"marker") == "fixture other retained" &&
                read(f.target) == "// fixture subsequent user edit", "discard rejects a selected backup reparse point and preserves its destination");
            RemoveDirectoryW(backup.c_str());
        } else { ++unavailable; std::printf("UNAVAILABLE: discard reparse fixture creation\n"); }
    }
    {
        Fixture f; ProxyConfigTransaction tx(f.data.native()); tx.prepare(f.target.native(), Format::clashVergeScript, transform);
        check(tx.apply().success, "install encrypted-backup integrity fixture");
        const auto applied = read(f.target); auto bytes = read(f.data / L"proxy-assistant.backup");
        if (!bytes.empty()) bytes[bytes.size() / 2] ^= 1;
        const auto backup = f.data / L"proxy-assistant.backup";
        HANDLE file = CreateFileW(backup.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        check(file != INVALID_HANDLE_VALUE && WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size() && SetEndOfFile(file), "corrupt only the isolated existing hidden backup fixture");
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        check(tx.restore().error == "backupCorrupt" && read(f.target) == applied && tx.has_backup(), "corrupt DPAPI backup is retained and never used for restoration");
    }
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--restore-test") {
        const fs::path data(argv[2]);
        if (!isolated_data_path(data)) return 2;
        ProxyConfigTransaction transaction(data.native());
        const auto restored = transaction.restore();
        return restored.success && restored.changed && !transaction.has_backup() ? 0 : 1;
    }
    try { run(); } catch (...) { check(false, "isolated filesystem fixture raised an unexpected error"); }
    std::printf("Proxy configuration file transactions: %d checks, %d failures, %d unavailable fixtures; temporary data and DPAPI only\n", checks, failures, unavailable);
    return failures ? 1 : 0;
}
