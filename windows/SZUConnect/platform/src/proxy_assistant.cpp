#include "szu/win/proxy_assistant.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <aclapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "szu/json.hpp"
#include "szu/win/text.hpp"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

namespace szu::win {
namespace {
namespace fs = std::filesystem;
constexpr wchar_t kBackupName[] = L"proxy-assistant.backup";
constexpr std::string_view kBackupMagic = "SZUConnect.ProxyBackup.v1\n";
constexpr std::size_t kBackupLimit = 4 * 1024 * 1024;
constexpr char kEntropy[] = "local.SZUConnect.ProxyAssistant.v1";

struct Secret {
    std::vector<BYTE> bytes;
    ~Secret() { if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size()); }
};
struct LocalBlob {
    DATA_BLOB value{};
    ~LocalBlob() { if (value.pbData) { SecureZeroMemory(value.pbData, value.cbData); LocalFree(value.pbData); } }
};
struct SecurityDescriptor {
    PSECURITY_DESCRIPTOR value = nullptr;
    ~SecurityDescriptor() { if (value) LocalFree(value); }
};
struct WipeOnExit {
    std::string& value;
    ~WipeOnExit() { wipe(value); }
};
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept { close(); value = std::exchange(other.value, INVALID_HANDLE_VALUE); return *this; }
    void close() { if (value != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(value, INVALID_HANDLE_VALUE)); }
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
};
struct Identity {
    DWORD volume = 0, high = 0, low = 0;
    bool operator==(const Identity&) const = default;
};
bool equal_path(const fs::path& a, const fs::path& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool valid_utf16(std::wstring_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto value = static_cast<unsigned>(text[i]);
        if (value >= 0xdc00 && value <= 0xdfff) return false;
        if (value >= 0xd800 && value <= 0xdbff) {
            if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff) return false;
        }
    }
    return true;
}
std::optional<fs::path> local_path(const std::wstring& input, bool allow_root = false) {
    if (input.size() < 3 || input.size() > 32760 || !valid_utf16(input) || input.find(L'\0') != std::wstring::npos ||
        !((input[0] >= L'A' && input[0] <= L'Z') || (input[0] >= L'a' && input[0] <= L'z')) ||
        input[1] != L':' || (input[2] != L'\\' && input[2] != L'/') || input.find(L':', 2) != std::wstring::npos) return {};
    std::wstring normal = input;
    std::replace(normal.begin(), normal.end(), L'/', L'\\');
    const fs::path path(normal);
    if (!path.is_absolute() || (!allow_root && path.filename().empty())) return {};
    for (const auto& component : path.relative_path()) {
        const auto part = component.native();
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return {};
        if (std::any_of(part.begin(), part.end(), [](wchar_t c) { return c < 32 || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|'; })) return {};
        auto device = part.substr(0, part.find(L'.'));
        while (!device.empty() && device.back() == L' ') device.pop_back();
        for (auto& c : device) if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
        if (device == L"CON" || device == L"PRN" || device == L"AUX" || device == L"NUL" ||
            device == L"CONIN$" || device == L"CONOUT$" || device == L"CLOCK$" ||
            (device.size() == 4 && (device.starts_with(L"COM") || device.starts_with(L"LPT")) &&
                ((device[3] >= L'0' && device[3] <= L'9') || device[3] == L'\u00b9' || device[3] == L'\u00b2' || device[3] == L'\u00b3'))) return {};
    }
    const auto drive = path.root_path().native();
    const auto type = GetDriveTypeW(drive.c_str());
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) return {};
    return path;
}
std::optional<fs::path> final_path(HANDLE file) {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const auto size = GetFinalPathNameByHandleW(file, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!size || size > 32760) return {};
        if (size >= buffer.size()) { buffer.resize(size + 1); continue; }
        std::wstring path(buffer.data(), size);
        if (!path.starts_with(L"\\\\?\\") || path.starts_with(L"\\\\?\\UNC\\")) return {};
        return local_path(path.substr(4), true);
    }
}
class Pins {
    std::vector<Handle> handles_;
public:
    bool pin(const fs::path& directory) {
        fs::path path;
        for (const auto& component : directory) {
            path /= component;
            if (!path.is_absolute()) continue;
            Handle handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            const auto resolved = handle ? final_path(handle.value) : std::nullopt;
            // Admit canonical long names only. Expanding a selected alias here
            // must not accidentally hide an ancestor's reparse-point mapping.
            if (!handle || !GetFileInformationByHandle(handle.value, &info) ||
                !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !resolved || !equal_path(path, *resolved)) return false;
            handles_.push_back(std::move(handle));
        }
        return true;
    }
};
bool ordinary(HANDLE file, const fs::path& path, Identity* id = nullptr, bool writable = true) {
    BY_HANDLE_FILE_INFORMATION info{};
    const auto resolved = final_path(file);
    constexpr DWORD rejected = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE |
        FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_OFFLINE;
    if (!GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & rejected) || info.nNumberOfLinks != 1 ||
        (writable && (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY)) || !resolved || !equal_path(path, *resolved)) return false;
    if (id) *id = {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
    return true;
}
Handle open_file(const fs::path& path, bool deleting = false, bool replacing = false) {
    return Handle(CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL | (deleting ? DELETE : 0), FILE_SHARE_READ | (replacing ? FILE_SHARE_DELETE : 0),
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
}
bool read_all(HANDLE file, std::string& out, std::size_t limit) {
    LARGE_INTEGER size{}, zero{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > limit ||
        !SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return false;
    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD count = 0;
    return (out.empty() || ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &count, nullptr)) && count == out.size();
}
bool write_all(HANDLE file, std::string_view data) {
    DWORD count = 0;
    return (data.empty() || WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &count, nullptr)) &&
        count == data.size() && FlushFileBuffers(file);
}
std::string digest(std::string_view text) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string result;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    DWORD object_size = 0, received = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof object_size, &received, 0) >= 0) {
        std::vector<BYTE> object(object_size);
        std::array<BYTE, 32> bytes{};
        if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0 &&
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())), static_cast<ULONG>(text.size()), 0) >= 0 &&
            BCryptFinishHash(hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) >= 0) {
            constexpr char hex[] = "0123456789abcdef";
            for (const auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
        }
        if (hash) BCryptDestroyHash(hash);
        if (!object.empty()) SecureZeroMemory(object.data(), object.size());
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}
bool text_encoding(std::string_view value) {
    return json::valid_utf8(value) && std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 && c != '\n' && c != '\r' && c != '\t'; });
}
fs::path temporary_path(const fs::path& parent) {
    std::array<BYTE, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring name = L".szu-proxy-";
    for (auto byte : random) { name += hex[byte >> 4]; name += hex[byte & 15]; }
    return parent / (name + L".tmp");
}
fs::path backup_temporary_path(const fs::path& target) {
    static std::atomic<unsigned long long> counter{GetTickCount64()};
    return fs::path(target.native() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(counter.fetch_add(1, std::memory_order_relaxed)));
}
bool rename_from_handle(HANDLE source, const fs::path& target, bool replace) {
    const auto name = target.native();
    const auto bytes = name.size() * sizeof(wchar_t);
    std::vector<BYTE> buffer(sizeof(FILE_RENAME_INFO) + bytes);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    // Keep the target deny-write handle open throughout the atomic replacement.
    info->Flags = FILE_RENAME_FLAG_POSIX_SEMANTICS | (replace ? FILE_RENAME_FLAG_REPLACE_IF_EXISTS : 0);
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(bytes);
    std::memcpy(info->FileName, name.data(), bytes);
    return SetFileInformationByHandle(source, FileRenameInfoEx, info, static_cast<DWORD>(buffer.size())) != FALSE;
}
bool remove_handle(HANDLE file) {
    FILE_DISPOSITION_INFO info{TRUE};
    return SetFileInformationByHandle(file, FileDispositionInfo, &info, sizeof info) != FALSE;
}
struct PendingRemoval {
    HANDLE file;
    bool committed = false;
    ~PendingRemoval() { if (!committed) remove_handle(file); }
};
struct Backup {
    fs::path path;
    proxyassist::Format format{};
    std::string original, before, after;
    ~Backup() { wipe(original); }
};
void append_u32(std::vector<BYTE>& out, std::size_t value) {
    const auto n = static_cast<std::uint32_t>(value);
    for (unsigned i = 0; i != 4; ++i) out.push_back(static_cast<BYTE>(n >> (i * 8)));
}
void append(std::vector<BYTE>& out, std::string_view value) {
    append_u32(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}
std::optional<std::string> next(std::string_view data, std::size_t& offset) {
    if (offset > data.size() || data.size() - offset < 4) return {};
    std::uint32_t size = 0;
    for (unsigned i = 0; i != 4; ++i) size |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset++])) << (i * 8);
    if (size > data.size() - offset) return {};
    std::string value(data.substr(offset, size)); offset += size; return value;
}
DATA_BLOB entropy() {
    return {static_cast<DWORD>(sizeof kEntropy), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
}
std::string encrypt(const Backup& backup) {
    Secret plain;
    plain.bytes.insert(plain.bytes.end(), kBackupMagic.begin(), kBackupMagic.end());
    plain.bytes.push_back(static_cast<BYTE>(backup.format));
    append(plain.bytes, narrow(backup.path.native())); append(plain.bytes, backup.before);
    append(plain.bytes, backup.after); append(plain.bytes, backup.original);
    DATA_BLOB input{static_cast<DWORD>(plain.bytes.size()), plain.bytes.data()}, extra = entropy();
    LocalBlob output;
    if (!CryptProtectData(&input, L"SZUConnect proxy configuration backup", &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output.value)) return {};
    std::string result(reinterpret_cast<char*>(output.value.pbData), output.value.cbData);
    return result;
}
bool decrypt(std::string_view encrypted, Backup& backup) {
    if (encrypted.empty() || encrypted.size() > kBackupLimit) return false;
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()), reinterpret_cast<BYTE*>(const_cast<char*>(encrypted.data()))}, extra = entropy();
    LocalBlob output;
    if (!CryptUnprotectData(&input, nullptr, &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output.value)) return false;
    const std::string_view plain(reinterpret_cast<char*>(output.value.pbData), output.value.cbData);
    if (!plain.starts_with(kBackupMagic) || plain.size() <= kBackupMagic.size()) return false;
    std::size_t offset = kBackupMagic.size();
    const auto format = static_cast<unsigned char>(plain[offset++]);
    auto path = next(plain, offset), before = next(plain, offset), after = next(plain, offset), original = next(plain, offset);
    const auto digest_text = [](const std::string& value) { return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }); };
    const auto selected = path && json::valid_utf8(*path) ? local_path(widen(*path)) : std::nullopt;
    const bool ok = format <= static_cast<unsigned char>(proxyassist::Format::singBoxJson) && selected && before && after && original &&
        offset == plain.size() && original->size() <= proxyassist::kMaxConfigBytes && text_encoding(*original) &&
        digest_text(*before) && digest_text(*after) && digest(*original) == *before;
    if (ok) { backup.path = *selected; backup.format = static_cast<proxyassist::Format>(format); backup.before = std::move(*before); backup.after = std::move(*after); backup.original = std::move(*original); }
    if (original) wipe(*original);
    if (path) wipe(*path);
    return ok;
}
bool extension_matches(const fs::path& path, proxyassist::Format format) {
    auto ext = path.extension().native();
    for (auto& c : ext) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return format == proxyassist::Format::clashVergeScript ? ext == L".js" :
        format == proxyassist::Format::mihomoYaml ? ext == L".yaml" || ext == L".yml" :
        format == proxyassist::Format::singBoxJson && ext == L".json";
}
bool replace_locked(HANDLE original, const fs::path& path, std::string_view text,
                    const Identity& expected_identity, std::string_view expected_digest) {
    constexpr auto security = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    SecurityDescriptor descriptor;
    PSID owner = nullptr, group = nullptr;
    PACL dacl = nullptr;
    if (GetSecurityInfo(original, SE_FILE_OBJECT, security, &owner, &group, &dacl, nullptr, &descriptor.value) != ERROR_SUCCESS) return false;
    SECURITY_ATTRIBUTES attributes{sizeof attributes, descriptor.value, FALSE};
    const auto temp = temporary_path(path.parent_path());
    if (temp.empty()) return false;
    Handle file(CreateFileW(temp.c_str(), GENERIC_WRITE | GENERIC_READ | DELETE | READ_CONTROL | WRITE_DAC | WRITE_OWNER, FILE_SHARE_READ, &attributes,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return false;
    PendingRemoval cleanup{file.value};
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    BY_HANDLE_FILE_INFORMATION old_info{};
    bool metadata = GetSecurityDescriptorControl(descriptor.value, &control, &revision) &&
        SetSecurityInfo(file.value, SE_FILE_OBJECT,
            security | (control & SE_DACL_PROTECTED ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
            owner, group, dacl, nullptr) == ERROR_SUCCESS && GetFileInformationByHandle(original, &old_info);
    if (metadata) {
        SecurityDescriptor copied;
        PSID copied_owner = nullptr, copied_group = nullptr;
        PACL copied_dacl = nullptr;
        SECURITY_DESCRIPTOR_CONTROL copied_control{};
        metadata = GetSecurityInfo(file.value, SE_FILE_OBJECT, security, &copied_owner, &copied_group, &copied_dacl, nullptr, &copied.value) == ERROR_SUCCESS &&
            GetSecurityDescriptorControl(copied.value, &copied_control, &revision) && owner && copied_owner && group && copied_group &&
            EqualSid(owner, copied_owner) && EqualSid(group, copied_group) &&
            static_cast<bool>(control & SE_DACL_PROTECTED) == static_cast<bool>(copied_control & SE_DACL_PROTECTED) &&
            ((!dacl && !copied_dacl) || (dacl && copied_dacl && dacl->AclSize == copied_dacl->AclSize &&
                std::memcmp(dacl, copied_dacl, dacl->AclSize) == 0));
    }
    if (metadata) {
        const auto attrs = old_info.dwFileAttributes &
            (FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
        FILE_BASIC_INFO basic{};
        basic.FileAttributes = attrs ? attrs : FILE_ATTRIBUTE_NORMAL;
        metadata = SetFileInformationByHandle(file.value, FileBasicInfo, &basic, sizeof basic) != FALSE;
    }
    const bool written = metadata && write_all(file.value, text);
    Identity current_identity;
    std::string current;
    WipeOnExit clear_current{current};
    const bool unchanged = written && ordinary(original, path, &current_identity) && current_identity == expected_identity &&
        read_all(original, current, proxyassist::kMaxConfigBytes) && digest(current) == expected_digest;
    const bool replaced = unchanged && rename_from_handle(file.value, path, true);
    cleanup.committed = replaced;
    return replaced;
}
proxyassist::Result error(std::string token, bool changed = false) { return {std::move(token), false, changed}; }
}  // namespace

struct ProxyConfigTransaction::Impl {
    fs::path root, backup;
    fs::path selected;
    proxyassist::Format format{};
    Identity identity;
    std::string original, patched, before, after;
    bool ready = false;
    Hooks hooks;
    mutable std::mutex mutex;
    explicit Impl(std::wstring directory, Hooks injected) : hooks(std::move(injected)) {
        const auto valid = local_path(directory);
        if (valid) { root = *valid; backup = root / kBackupName; }
    }
    ~Impl() { clear(); }
    void clear() { wipe(original); wipe(patched); selected.clear(); before.clear(); after.clear(); ready = false; }
    bool backup_exists() const {
        Pins pins;
        return !backup.empty() && pins.pin(root) && GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    std::string read_backup(Backup& record, Handle& file, Pins& pins, bool deleting = false) const {
        if (root.empty() || !pins.pin(root)) return "backupFailed";
        file = open_file(backup, deleting);
        if (!file) return GetLastError() == ERROR_FILE_NOT_FOUND ? "backupMissing" : "backupFailed";
        if (!ordinary(file.value, backup)) return "backupCorrupt";
        std::string encrypted;
        if (!read_all(file.value, encrypted, kBackupLimit) || !decrypt(encrypted, record)) return "backupCorrupt";
        return {};
    }
    bool publish_backup(const Backup& record) const {
        const auto bytes = encrypt(record);
        if (bytes.empty()) return false;
        if (hooks.before_backup_create) hooks.before_backup_create();
        const auto temp = backup_temporary_path(backup);
        if (temp.empty()) return false;
        Handle file(CreateFileW(temp.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr));
        if (!file) return false;
        PendingRemoval cleanup{file.value};
        // Validate the created object, before writing, rather than trusting a
        // second path lookup. Its entry keeps the pinned root nonempty until the
        // backup is published, so that root cannot become a new junction.
        if (!ordinary(file.value, temp)) return false;
        const bool published = write_all(file.value, bytes) && rename_from_handle(file.value, backup, false);
        cleanup.committed = published;
        return published;
    }
};

ProxyConfigTransaction::ProxyConfigTransaction(std::wstring data_root, Hooks hooks) : impl_(std::make_unique<Impl>(std::move(data_root), std::move(hooks))) {}
ProxyConfigTransaction::~ProxyConfigTransaction() = default;

proxyassist::Preview ProxyConfigTransaction::prepare(const std::wstring& input, proxyassist::Format format, const Transform& transform) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->clear();
    proxyassist::Preview preview; preview.format = format;
    const auto fail = [&](const char* token) { preview.error = token; impl_->clear(); return preview; };
    try {
        const auto path = local_path(input);
        if (!path || impl_->root.empty()) return fail("invalidPath");
        preview.filename = narrow(path->filename().native());
        if (!extension_matches(*path, format) || equal_path(*path, impl_->backup)) return fail("unsupportedFile");
        Pins pins;
        if (!pins.pin(path->parent_path()) || !pins.pin(impl_->root)) return fail("invalidPath");
        const auto attrs = GetFileAttributesW(path->c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return fail("unsupportedFile");
        auto file = open_file(*path);
        if (!file) return fail("readFailed");
        if (!ordinary(file.value, *path, &impl_->identity)) return fail("unsupportedFile");
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.value, &size)) return fail("readFailed");
        if (size.QuadPart > proxyassist::kMaxConfigBytes) return fail("tooLarge");
        if (!read_all(file.value, impl_->original, proxyassist::kMaxConfigBytes)) return fail("readFailed");
        if (!text_encoding(impl_->original)) return fail("encoding");
        const bool bom = impl_->original.starts_with("\xef\xbb\xbf");
        const std::string_view text(impl_->original);
        auto patch = transform(bom ? text.substr(3) : text);
        WipeOnExit clear_patch{patch.text};
        if (!patch.ok()) { preview.error = patch.error; impl_->clear(); return preview; }
        if (!text_encoding(patch.text)) return fail("invalidConfig");
        if (patch.text.starts_with("\xef\xbb\xbf")) patch.text.erase(0, 3);
        impl_->patched = bom ? std::string("\xef\xbb\xbf") + patch.text : patch.text;
        if (impl_->patched.size() > proxyassist::kMaxConfigBytes) return fail("tooLarge");
        impl_->before = digest(impl_->original); impl_->after = digest(impl_->patched);
        if (impl_->before.empty() || impl_->after.empty()) return fail("unavailable");
        if (impl_->backup_exists()) {
            Backup previous; Handle prior; Pins backup_pins;
            const auto status = impl_->read_backup(previous, prior, backup_pins);
            if (!status.empty()) return fail(status.c_str());
            if (!equal_path(previous.path, *path) || previous.after != impl_->before || impl_->before != impl_->after) return fail("backupExists");
        }
        impl_->selected = *path; impl_->format = format; impl_->ready = true;
        preview.changes = std::move(patch.changes); preview.changed = impl_->before != impl_->after; preview.ready = true;
        return preview;
    } catch (...) { return fail("invalidConfig"); }
}

proxyassist::Result ProxyConfigTransaction::apply() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->ready) return error("unavailable");
    try {
        Pins pins;
        if (!pins.pin(impl_->selected.parent_path()) || !pins.pin(impl_->root)) return error("invalidPath");
        auto file = open_file(impl_->selected, true, true);
        Identity identity;
        std::string current;
        WipeOnExit clear_current{current};
        if (!file || !ordinary(file.value, impl_->selected, &identity) || !read_all(file.value, current, proxyassist::kMaxConfigBytes)) return error("changed");
        const bool matches = identity == impl_->identity && digest(current) == impl_->before;
        wipe(current);
        if (!matches) return error("changed");
        if (impl_->before == impl_->after) { impl_->clear(); return {{}, true, false}; }
        if (impl_->backup_exists()) return error("backupExists");
        Backup backup; backup.path = impl_->selected; backup.format = impl_->format;
        backup.original = impl_->original; backup.before = impl_->before; backup.after = impl_->after;
        if (!impl_->publish_backup(backup)) return error("backupFailed");
        if (!replace_locked(file.value, impl_->selected, impl_->patched, impl_->identity, impl_->before)) return error("writeFailed");
        impl_->clear(); return {{}, true, true};
    } catch (...) { return error("writeFailed"); }
}

proxyassist::Result ProxyConfigTransaction::restore() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->clear();
    try {
        Backup backup; Handle backup_file; Pins pins;
        const auto status = impl_->read_backup(backup, backup_file, pins, true);
        if (!status.empty()) return error(status);
        if (!pins.pin(backup.path.parent_path())) return error("invalidPath");
        auto target = open_file(backup.path, true, true);
        Identity identity;
        std::string current;
        WipeOnExit clear_current{current};
        if (!target || !ordinary(target.value, backup.path, &identity) || !read_all(target.value, current, proxyassist::kMaxConfigBytes)) return error("restoreConflict");
        const auto hash = digest(current); wipe(current);
        if (hash != backup.after && hash != backup.before) return error("restoreConflict");
        const bool changed = hash != backup.before;
        if (changed && !replace_locked(target.value, backup.path, backup.original, identity, hash)) return error("writeFailed");
        if (!remove_handle(backup_file.value)) return error("cleanupPending", changed);
        return {{}, true, changed};
    } catch (...) { return error("backupCorrupt"); }
}

proxyassist::Result ProxyConfigTransaction::discard_backup() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->clear();
    try {
        Backup backup; Handle file; Pins pins;
        const auto status = impl_->read_backup(backup, file, pins, true);
        if (!status.empty()) return error(status);
        if (!remove_handle(file.value)) return error("backupFailed");
        return {{}, true, true};
    } catch (...) { return error("backupCorrupt"); }
}

void ProxyConfigTransaction::clear_preview() { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->clear(); }
bool ProxyConfigTransaction::has_backup() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->backup_exists(); }

}  // namespace szu::win
