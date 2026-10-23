#include "installation.hpp"

#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winver.h>
#include <wincred.h>
#include <tlhelp32.h>
#include "shared_copy.hpp"
#include "szu/json.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "comctl32.lib")

namespace szu::app {
namespace {
namespace fs = std::filesystem;
constexpr wchar_t kWindowClass[] = L"SZUConnect.MainWindow.v1";
constexpr UINT kQuit = 40006; // Same existing tray command, including portable versions.
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunName[] = L"SZUConnect";
constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SZUConnect";
constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr char kRootIdentity[] = "SZUConnect.ManagedInstallation.v1\n";
constexpr char kManifestIdentity[] = "SZUConnect.ManagedPayload.v1\n";
constexpr wchar_t kManifestName[] = L"INSTALLATION-MANIFEST.txt";
constexpr wchar_t kDocumentsDirectory[] = L"开源许可";
constexpr wchar_t kLegacyDocumentsDirectory[] = L"说明与许可";
constexpr DWORD kWaitMs = 30000;
constexpr DWORD kCleanupReadyWaitMs = 20000;
constexpr std::size_t kDocumentLimit = 2 * 1024 * 1024;
const std::array<const wchar_t*, 4> kDocumentNames{
    L"README.md", L"LICENSE", L"THIRD-PARTY-NOTICES.md", L"curl-COPYING.txt"};

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : value(v == INVALID_HANDLE_VALUE ? nullptr : v) {}
    ~Handle() { if (value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    bool valid() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};
template<class T> struct ComPtr {
    T* value = nullptr;
    ~ComPtr() { if (value) value->Release(); }
    T** address() { return &value; }
    T* operator->() const { return value; }
};

fs::path executable_path() {
    std::vector<wchar_t> data(512);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, data.data(), static_cast<DWORD>(data.size()));
        if (!n) return {};
        if (n < data.size() - 1) return std::wstring(data.data(), n);
        if (data.size() >= 32768) return {};
        data.resize(data.size() * 2);
    }
}
bool equal_path(const fs::path& a, const fs::path& b) {
    const auto left = a.lexically_normal().native(), right = b.lexically_normal().native();
    return !left.empty() && !right.empty() && CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool path_exists(const fs::path& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
bool regular(const fs::path& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
bool plain_directory(const fs::path& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}
// Reject directory junctions as well as symlinks, including at ancestor boundaries.
bool safe_ancestors(const fs::path& path) {
    if (!path.is_absolute() || path.native().rfind(L"\\\\", 0) == 0) return false;
    for (const auto& part : path) if (part == L"." || part == L"..") return false;
    fs::path accumulated;
    for (const auto& part : path.lexically_normal()) {
        accumulated /= part;
        const DWORD attrs = GetFileAttributesW(accumulated.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    return true;
}
class DirectoryPins {
    struct Entry { fs::path path; HANDLE handle; };
    std::vector<Entry> entries_;
public:
    ~DirectoryPins() { for (const auto& entry : entries_) if (entry.handle) CloseHandle(entry.handle); }
    bool pin(const fs::path& path) {
        if (!safe_ancestors(path)) return false;
        fs::path accumulated;
        for (const auto& component : path.lexically_normal()) {
            accumulated /= component;
            if (!accumulated.is_absolute()) continue;
            if (!path_exists(accumulated)) break;
            if (std::any_of(entries_.begin(), entries_.end(), [&](const auto& e) { return e.handle && equal_path(e.path, accumulated); })) continue;
            HANDLE handle = CreateFileW(accumulated.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return false;
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) { CloseHandle(handle); return false; }
            entries_.push_back({accumulated, handle});
        }
        return true;
    }
    void release(const fs::path& path) {
        for (auto& entry : entries_) if (entry.handle && equal_path(entry.path, path)) { CloseHandle(entry.handle); entry.handle = nullptr; }
    }
    bool inherit(std::vector<HANDLE>& copies) const {
        for (const auto& entry : entries_) if (entry.handle) {
            HANDLE copy = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), entry.handle, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS)) return false;
            copies.push_back(copy);
        }
        return true;
    }
};
std::string read_bytes(const fs::path& path, std::size_t limit, bool& ok) {
    ok = false;
    if (!regular(path)) return {};
    std::error_code ec;
    const auto n = fs::file_size(path, ec);
    if (ec || n > limit) return {};
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::string bytes(static_cast<std::size_t>(n), '\0');
    if (n && !file.read(bytes.data(), static_cast<std::streamsize>(n))) return {};
    ok = true;
    return bytes;
}
bool write_new(const fs::path& path, const std::string& bytes) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.value || bytes.size() > MAXDWORD) return false;
    DWORD written = 0;
    return (bytes.empty() || WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) &&
        written == bytes.size() && FlushFileBuffers(file.value);
}
std::string sha256(const fs::path& path) {
    if (!regular(path)) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    DWORD length = 0, used = 0;
    bool ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof length, &used, 0) >= 0;
    std::vector<UCHAR> object(length);
    if (ok) ok = BCryptCreateHash(algorithm, &hash, object.data(), length, nullptr, 0, 0) >= 0;
    std::ifstream input(path, std::ios::binary);
    if (!input) ok = false;
    std::array<char, 65536> buffer{};
    while (ok && input) {
        input.read(buffer.data(), buffer.size());
        if (input.gcount() && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(input.gcount()), 0) < 0) ok = false;
    }
    if (input.bad()) ok = false;
    std::array<UCHAR, 32> digest{};
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto c : digest) { result.push_back(hex[c >> 4]); result.push_back(hex[c & 15]); }
    return result;
}
std::string sha256_handle(HANDLE file) {
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    DWORD length = 0, used = 0;
    bool ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof length, &used, 0) >= 0;
    std::vector<UCHAR> object(length);
    if (ok) ok = BCryptCreateHash(algorithm, &hash, object.data(), length, nullptr, 0, 0) >= 0;
    std::array<UCHAR, 65536> buffer{}; DWORD count = 0;
    while (ok) {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) { ok = false; break; }
        if (!count) break;
        if (BCryptHashData(hash, buffer.data(), count, 0) < 0) ok = false;
    }
    std::array<UCHAR, 32> digest{};
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    constexpr char hex[] = "0123456789abcdef"; std::string result;
    for (const auto c : digest) { result.push_back(hex[c >> 4]); result.push_back(hex[c & 15]); }
    return result;
}
std::wstring random_token() {
    std::array<UCHAR, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring token;
    for (const auto c : bytes) { token.push_back(hex[c >> 4]); token.push_back(hex[c & 15]); }
    return token;
}
bool valid_token(const std::wstring& token) {
    return token.size() == 32 && std::all_of(token.begin(), token.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
    });
}
fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &value)) || !value) return {};
    fs::path result(value);
    CoTaskMemFree(value);
    return result;
}
fs::path installation_root() {
    const auto base = known_folder(FOLDERID_LocalAppData);
    return base.empty() ? fs::path{} : base / L"Programs" / L"SZUConnect";
}
std::vector<fs::path> payload_files(const wchar_t* documents = kDocumentsDirectory) {
    std::vector<fs::path> names{L"SZUConnect.exe"};
    for (const auto* name : kDocumentNames) names.push_back(fs::path(documents) / name);
    return names;
}
std::string manifest_for(const fs::path& folder, const wchar_t* documents = kDocumentsDirectory) {
    std::string manifest = kManifestIdentity;
    // Fixed positions avoid accepting filenames from an untrusted on-disk manifest.
    for (const auto& name : payload_files(documents)) {
        const auto digest = sha256(folder / name);
        if (digest.empty()) return {};
        manifest += digest + "\n";
    }
    return manifest;
}
bool payload_inventory(const fs::path& folder, const wchar_t* documents = kDocumentsDirectory) {
    if (!safe_ancestors(folder) || !plain_directory(folder)) return false;
    const auto expected_files = payload_files(documents);
    std::set<fs::path> expected(expected_files.begin(), expected_files.end());
    expected.emplace(kManifestName);
    std::set<fs::path> actual;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec)) {
        const auto attrs = GetFileAttributesW(it->path().c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto relative = it->path().lexically_relative(folder);
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            if (relative != fs::path(documents)) return false;
        } else actual.emplace(relative);
    }
    return !ec && actual == expected;
}
const wchar_t* owned_payload_directory(const fs::path& folder) {
    const wchar_t* documents = nullptr;
    for (const auto* candidate : {kDocumentsDirectory, kLegacyDocumentsDirectory}) {
        if (payload_inventory(folder, candidate)) { documents = candidate; break; }
    }
    if (!documents) return nullptr;
    bool ok = false;
    const auto recorded = read_bytes(folder / kManifestName, 1024, ok);
    if (!ok) return nullptr;
    const auto actual = manifest_for(folder, documents);
    return !actual.empty() && recorded == actual ? documents : nullptr;
}
bool owned_payload(const fs::path& folder) {
    return owned_payload_directory(folder) != nullptr;
}
bool remove_owned_payload(const fs::path& folder) {
    const auto* documents = owned_payload_directory(folder);
    if (!documents) return false;
    // No recursive removal: each expected ordinary file is checked again.
    auto names = payload_files(documents); names.emplace_back(kManifestName);
    for (const auto& name : names) {
        const auto path = folder / name;
        if (!regular(path) || !DeleteFileW(path.c_str())) return false;
    }
    return RemoveDirectoryW((folder / documents).c_str()) && RemoveDirectoryW(folder.c_str());
}
bool root_identity(const fs::path& root) {
    bool ok = false;
    return read_bytes(root / L"installation.id", 128, ok) == kRootIdentity && ok;
}
bool root_inventory(const fs::path& root) {
    if (!plain_directory(root) || !safe_ancestors(root) || !root_identity(root)) return false;
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().native();
        if (name == L"installation.id") { if (!regular(it->path())) return false; }
        else if (name == L"current" || name == L"previous") { if (!owned_payload(it->path())) return false; }
        else return false;
    }
    return !ec;
}
bool move_directory(const fs::path& from, const fs::path& to) {
    return safe_ancestors(from) && safe_ancestors(to) && !path_exists(to) && MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH);
}
struct Payload {
    fs::path source;
    std::vector<std::pair<std::wstring, std::string>> documents;
};
bool document_input_valid(const Payload& payload) {
    if (!safe_ancestors(payload.source) || !regular(payload.source) || payload.documents.size() != kDocumentNames.size()) return false;
    std::set<std::wstring> names;
    for (const auto& [name, bytes] : payload.documents) {
        if (bytes.empty() || bytes.size() > kDocumentLimit ||
            std::find(kDocumentNames.begin(), kDocumentNames.end(), name) == kDocumentNames.end() || !names.insert(name).second) return false;
    }
    return true;
}
bool same_payload(const fs::path& installed, const Payload& payload) {
    const auto* documents = owned_payload_directory(installed);
    if (!documents || std::wstring_view(documents) != kDocumentsDirectory ||
        sha256(installed / L"SZUConnect.exe") != sha256(payload.source)) return false;
    for (const auto& [name, bytes] : payload.documents) {
        bool ok = false;
        if (read_bytes(installed / kDocumentsDirectory / name, kDocumentLimit, ok) != bytes || !ok) return false;
    }
    return true;
}
bool create_stage(const fs::path& stage, const Payload& payload) {
    if (!CreateDirectoryW(stage.c_str(), nullptr) || !CreateDirectoryW((stage / kDocumentsDirectory).c_str(), nullptr)) return false;
    if (!CopyFileW(payload.source.c_str(), (stage / L"SZUConnect.exe").c_str(), TRUE)) return false;
    for (const auto& [name, bytes] : payload.documents)
        if (!write_new(stage / kDocumentsDirectory / name, bytes)) return false;
    const auto manifest = manifest_for(stage);
    return !manifest.empty() && write_new(stage / kManifestName, manifest) && owned_payload(stage);
}
void cleanup_created_stage(const fs::path& stage) {
    // Only names created by this transaction, never remove_all or an arbitrary parent.
    if (!plain_directory(stage) || !safe_ancestors(stage)) return;
    auto names = payload_files(); names.emplace_back(kManifestName);
    for (const auto& name : names) if (regular(stage / name)) DeleteFileW((stage / name).c_str());
    RemoveDirectoryW((stage / kDocumentsDirectory).c_str());
    RemoveDirectoryW(stage.c_str());
}

struct TransactionCallbacks {
    std::function<bool()> reuse_running_current = [] { return false; };
    std::function<bool()> stop_old = [] { return true; };
    std::function<bool()> apply_links = [] { return true; };
    std::function<bool()> restore_links = [] { return true; };
    std::function<bool(const fs::path&)> launch_ready = [](const fs::path&) { return true; };
    std::function<bool()> stop_new = [] { return true; };
    std::function<void(const fs::path&)> restart_old = [](const fs::path&) {};
    int fail_at = 0;
};
struct TransactionResult { bool ok = false; std::string key; };
TransactionResult transact(const fs::path& root, const Payload& payload, TransactionCallbacks& callbacks) {
    if (!safe_ancestors(root) || !document_input_valid(payload)) return {false, "installation.unsafePath"};
    if (!path_exists(root)) {
        std::error_code ec;
        fs::create_directories(root, ec);
        if (ec || !plain_directory(root) || !safe_ancestors(root) || !write_new(root / L"installation.id", kRootIdentity))
            return {false, "installation.createFailed"};
    }
    if (!root_inventory(root)) return {false, "installation.unrecognizedFiles"};
    const auto current = root / L"current", previous = root / L"previous";
    // Recovery from interruption between the two atomic directory renames.
    if (!path_exists(current) && path_exists(previous) && !move_directory(previous, current)) return {false, "installation.restoreFailed"};
    if (same_payload(current, payload)) {
        if (callbacks.reuse_running_current()) return callbacks.launch_ready(current / L"SZUConnect.exe") ?
            TransactionResult{true, {}} : TransactionResult{false, "installation.startFailed"};
        if (!callbacks.stop_old()) return {false, "installation.closeFailed"};
        if (!callbacks.apply_links()) {
            const bool restored = callbacks.restore_links(); callbacks.restart_old(current / L"SZUConnect.exe");
            return {false, restored ? "installation.shortcutFailed" : "installation.restoreFailed"};
        }
        if (!callbacks.launch_ready(current / L"SZUConnect.exe")) {
            const bool restored = callbacks.restore_links();
            if (!callbacks.stop_new()) return {false, "installation.closeFailedBackupRetained"};
            callbacks.restart_old(current / L"SZUConnect.exe");
            return {false, restored ? "installation.startFailed" : "installation.restoreFailed"};
        }
        return {true, {}};
    }
    const auto token = random_token();
    if (token.empty()) return {false, "installation.randomFailed"};
    const auto stage = root / (L"stage-" + token);
    if (!create_stage(stage, payload)) { cleanup_created_stage(stage); return {false, "installation.copyFailed"}; }
    if (callbacks.fail_at == 1) { cleanup_created_stage(stage); return {false, "installation.copyFailed"}; }
    if (!callbacks.stop_old()) { cleanup_created_stage(stage); return {false, "installation.closeFailed"}; }
    const bool had_current = path_exists(current);
    if (path_exists(previous) && !remove_owned_payload(previous)) {
        cleanup_created_stage(stage); callbacks.restart_old(current / L"SZUConnect.exe");
        return {false, "installation.backupFailed"};
    }
    bool backed_up = false, published = false, links_applied = false;
    const auto rollback = [&](const char* key) -> TransactionResult {
        const bool links_restored = !links_applied || callbacks.restore_links();
        if (published && !callbacks.stop_new()) return {false, "installation.closeFailedBackupRetained"};
        if (published && !remove_owned_payload(current)) return {false, "installation.restoreFailed"};
        if (backed_up && !move_directory(previous, current)) return {false, "installation.restoreFailed"};
        cleanup_created_stage(stage);
        callbacks.restart_old(current / L"SZUConnect.exe");
        return {false, links_restored ? key : "installation.restoreFailed"};
    };
    if (had_current) {
        if (!move_directory(current, previous)) {
            cleanup_created_stage(stage); callbacks.restart_old(current / L"SZUConnect.exe");
            return {false, "installation.backupFailed"};
        }
        backed_up = true;
    }
    if (callbacks.fail_at == 2) return rollback("installation.backupFailed");
    if (!move_directory(stage, current)) return rollback("installation.replaceFailed");
    published = true;
    if (callbacks.fail_at == 3) return rollback("installation.replaceFailed");
    links_applied = true; // Partial link/Run writes also require restoration.
    if (!callbacks.apply_links()) return rollback("installation.shortcutFailed");
    if (!callbacks.launch_ready(current / L"SZUConnect.exe")) return rollback("installation.startFailed");
    return {true, {}};
}

bool szu_executable(const fs::path& path) {
    if (!safe_ancestors(path) || !regular(path)) return false;
    DWORD ignored = 0;
    const DWORD length = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!length || length > 128 * 1024) return false;
    std::vector<BYTE> info(length);
    if (!GetFileVersionInfoW(path.c_str(), 0, length, info.data())) return false;
    const auto string_value = [&](const wchar_t* key) {
        LPVOID value = nullptr; UINT size = 0;
        const auto query = std::wstring(L"\\StringFileInfo\\080404b0\\") + key;
        if (!VerQueryValueW(info.data(), query.c_str(), &value, &size) || !value || !size) return std::wstring{};
        return std::wstring(static_cast<wchar_t*>(value), wcsnlen(static_cast<wchar_t*>(value), size));
    };
    return string_value(L"OriginalFilename") == L"SZUConnect.exe" && string_value(L"InternalName") == L"SZUConnect" &&
        string_value(L"ProductName") == L"深大随连 SZUConnect（非深圳大学官方应用）";
}
using Version = std::array<unsigned short, 4>;
bool version_older(const Version& incoming, const Version& installed) { return incoming < installed; }
std::optional<Version> file_version(const fs::path& path) {
    if (!safe_ancestors(path) || !regular(path)) return std::nullopt;
    DWORD ignored = 0;
    const DWORD length = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!length || length > 128 * 1024) return std::nullopt;
    std::vector<BYTE> info(length);
    if (!GetFileVersionInfoW(path.c_str(), 0, length, info.data())) return std::nullopt;
    VS_FIXEDFILEINFO* value = nullptr; UINT size = 0;
    if (!VerQueryValueW(info.data(), L"\\", reinterpret_cast<LPVOID*>(&value), &size) || !value ||
        size < sizeof(VS_FIXEDFILEINFO) || value->dwSignature != VS_FFI_SIGNATURE) return std::nullopt;
    return Version{HIWORD(value->dwFileVersionMS), LOWORD(value->dwFileVersionMS), HIWORD(value->dwFileVersionLS), LOWORD(value->dwFileVersionLS)};
}
std::vector<BYTE> token_user(HANDLE process) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) return {};
    Handle token(raw);
    DWORD needed = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &needed);
    if (!needed || needed > 65536) return {};
    std::vector<BYTE> data(needed);
    if (!GetTokenInformation(token.value, TokenUser, data.data(), needed, &needed)) return {};
    return data;
}
bool same_user(HANDLE process) {
    const auto a = token_user(GetCurrentProcess()), b = token_user(process);
    return !a.empty() && !b.empty() && EqualSid(reinterpret_cast<const TOKEN_USER*>(a.data())->User.Sid,
        reinterpret_cast<const TOKEN_USER*>(b.data())->User.Sid);
}
fs::path process_path(HANDLE process) {
    std::vector<wchar_t> buffer(32768); DWORD n = static_cast<DWORD>(buffer.size());
    return QueryFullProcessImageNameW(process, 0, buffer.data(), &n) ? fs::path(std::wstring(buffer.data(), n)) : fs::path{};
}
struct ExistingProcess {
    DWORD pid = 0;
    fs::path path;
    bool found = false;
};
ExistingProcess existing_process() {
    ExistingProcess result;
    HWND window = FindWindowW(kWindowClass, nullptr);
    if (!window) return result;
    GetWindowThreadProcessId(window, &result.pid);
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, result.pid));
    if (!process.value || !same_user(process.value)) return result;
    result.path = process_path(process.value);
    result.found = szu_executable(result.path);
    return result;
}
bool stop_process(DWORD pid, const fs::path& expected) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!process.value) return GetLastError() == ERROR_INVALID_PARAMETER;
    if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) return true;
    if (!same_user(process.value) || !equal_path(process_path(process.value), expected) || !szu_executable(expected)) return false;
    HWND window = FindWindowW(kWindowClass, nullptr);
    DWORD owner = 0; if (window) GetWindowThreadProcessId(window, &owner);
    if (owner != pid || !PostMessageW(window, WM_COMMAND, kQuit, 0)) return false;
    return WaitForSingleObject(process.value, kWaitMs) == WAIT_OBJECT_0;
}
bool start_process(const fs::path& exe, const std::wstring& arguments, PROCESS_INFORMATION& process) {
    std::wstring command = L"\"" + exe.native() + L"\"";
    if (!arguments.empty()) command += L" " + arguments;
    STARTUPINFOW startup{}; startup.cb = sizeof startup;
    return CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(), &startup, &process) != FALSE;
}

struct RunSnapshot {
    bool exists = false, changed = false;
    DWORD type = REG_NONE;
    std::vector<BYTE> bytes, replacement;
    fs::path target;
};
RunSnapshot read_run() {
    RunSnapshot result;
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_ANY, &result.type, nullptr, &size) != ERROR_SUCCESS || size > 65536) return result;
    result.bytes.resize(size);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_ANY, &result.type, result.bytes.data(), &size) != ERROR_SUCCESS) return {};
    result.exists = true;
    if (result.type != REG_SZ || size % sizeof(wchar_t)) return result;
    std::wstring command(reinterpret_cast<const wchar_t*>(result.bytes.data()), size / sizeof(wchar_t));
    if (command.empty() || command.back() != L'\0') return result;
    command.pop_back();
    if (command.find(L'\0') != std::wstring::npos) return result;
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(command.c_str(), &argc);
    if (argv) {
        if ((argc == 1 || (argc == 2 && std::wstring(argv[1]) == L"--background")) && szu_executable(argv[0])) result.target = argv[0];
        LocalFree(argv);
    }
    return result;
}
std::vector<BYTE> wide_bytes(const std::wstring& text) {
    const auto* first = reinterpret_cast<const BYTE*>(text.c_str());
    return {first, first + (text.size() + 1) * sizeof(wchar_t)};
}
bool migrate_run(RunSnapshot& run, const fs::path& target) {
    if (run.target.empty()) return true; // Never enable startup on a new installation.
    const auto now = read_run();
    if (!now.exists || now.bytes != run.bytes || now.type != run.type) return false;
    run.replacement = wide_bytes(L"\"" + target.native() + L"\" --background");
    if (run.replacement == run.bytes) return true;
    const auto rc = RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName, REG_SZ, run.replacement.data(), static_cast<DWORD>(run.replacement.size()));
    run.changed = rc == ERROR_SUCCESS;
    return run.changed;
}
bool restore_run(RunSnapshot& run) {
    if (!run.changed) return true;
    const auto now = read_run();
    if (now.exists && now.type == REG_SZ && now.bytes == run.replacement)
        return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName, run.type, run.bytes.data(), static_cast<DWORD>(run.bytes.size())) == ERROR_SUCCESS;
    return false;
}

struct ShortcutChange { fs::path path; bool existed = false, changed = false; std::string before, after; };
fs::path shortcut_target(const fs::path& path) {
    if (!safe_ancestors(path) || !regular(path)) return {};
    ComPtr<IShellLinkW> link; ComPtr<IPersistFile> persist;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.address()))) ||
        FAILED(link->QueryInterface(IID_PPV_ARGS(persist.address()))) || FAILED(persist->Load(path.c_str(), STGM_READ))) return {};
    std::vector<wchar_t> target(32768);
    return SUCCEEDED(link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH)) ? fs::path(target.data()) : fs::path{};
}
bool write_shortcut(ShortcutChange& change, const fs::path& target) {
    if (!safe_ancestors(change.path)) return false;
    bool prior_ok = false;
    if (change.existed) {
        if (read_bytes(change.path, kDocumentLimit, prior_ok) != change.before || !prior_ok) return false;
    } else if (path_exists(change.path)) return false;
    ComPtr<IShellLinkW> link; ComPtr<IPersistFile> persist;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.address()))) ||
        FAILED(link->QueryInterface(IID_PPV_ARGS(persist.address())))) return false;
    if (change.existed && FAILED(persist->Load(change.path.c_str(), STGM_READ))) return false;
    if (FAILED(link->SetPath(target.c_str())) || FAILED(link->SetWorkingDirectory(target.parent_path().c_str())) ||
        FAILED(link->SetIconLocation(target.c_str(), 0))) return false;
    const auto temporary = change.path.parent_path() / (L".szu-link-" + random_token() + L".lnk");
    if (FAILED(persist->Save(temporary.c_str(), TRUE))) return false;
    bool ok = false; const auto bytes = read_bytes(temporary, kDocumentLimit, ok);
    if (change.existed) {
        if (read_bytes(change.path, kDocumentLimit, prior_ok) != change.before || !prior_ok) { DeleteFileW(temporary.c_str()); return false; }
    } else if (path_exists(change.path)) { DeleteFileW(temporary.c_str()); return false; }
    if (!ok || !MoveFileExW(temporary.c_str(), change.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str()); return false;
    }
    change.after = bytes; change.changed = true;
    return true;
}
bool restore_shortcut(ShortcutChange& change) {
    if (!change.changed) return true;
    bool ok = false;
    if (read_bytes(change.path, kDocumentLimit, ok) != change.after || !ok) return false; // Preserve concurrent user edits.
    if (!change.existed) return DeleteFileW(change.path.c_str()) != FALSE;
    const auto temporary = change.path.parent_path() / (L".szu-link-" + random_token() + L".lnk");
    if (write_new(temporary, change.before)) {
        if (MoveFileExW(temporary.c_str(), change.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        DeleteFileW(temporary.c_str());
    }
    return false;
}
bool collect_shortcuts(std::vector<ShortcutChange>& changes, const std::vector<fs::path>& old_paths, const fs::path& installed) {
    const auto programs = known_folder(FOLDERID_Programs), desktop = known_folder(FOLDERID_Desktop);
    if (programs.empty() || !safe_ancestors(programs) || !plain_directory(programs)) return false;
    for (const auto& folder : {programs, desktop}) {
        if (folder.empty() || !safe_ancestors(folder) || !plain_directory(folder)) continue;
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (_wcsicmp(it->path().extension().c_str(), L".lnk") != 0 || !regular(it->path())) continue;
            const auto target = shortcut_target(it->path());
            if (!std::any_of(old_paths.begin(), old_paths.end(), [&](const auto& p) { return equal_path(target, p); }) && !szu_executable(target)) continue;
            bool ok = false; const auto bytes = read_bytes(it->path(), kDocumentLimit, ok);
            if (!ok) return false;
            changes.push_back({it->path(), true, false, bytes, {}});
        }
        if (ec) return false;
    }
    const auto main_link = programs / L"深大随连.lnk";
    const bool found = std::any_of(changes.begin(), changes.end(), [&](const auto& change) { return equal_path(change.path, main_link); });
    if (!found) {
        if (path_exists(main_link)) {
            if (!equal_path(shortcut_target(main_link), installed)) return false;
            bool ok = false; const auto bytes = read_bytes(main_link, kDocumentLimit, ok);
            if (!ok) return false;
            changes.push_back({main_link, true, false, bytes, {}});
        } else changes.push_back({main_link, false, false, {}, {}});
    }
    return true;
}
struct RegistryValue {
    DWORD type = REG_NONE;
    std::vector<BYTE> bytes;
    bool operator==(const RegistryValue&) const = default;
};
using RegistryValues = std::map<std::wstring, RegistryValue>;
struct RegistrySnapshot {
    bool valid = false, exists = false;
    RegistryValues values;
    bool operator==(const RegistrySnapshot&) const = default;
};
#ifdef SZU_INSTALLATION_TESTING
struct RegistryTestState { RegistrySnapshot snapshot; int failure_at = 0, mutations = 0, reads = 0; };
thread_local RegistryTestState* registry_test_state = nullptr;
#endif
LSTATUS set_uninstall_value(const std::wstring& name, const RegistryValue& value) {
#ifdef SZU_INSTALLATION_TESTING
    if (registry_test_state) {
        if (++registry_test_state->mutations == registry_test_state->failure_at) return ERROR_ACCESS_DENIED;
        registry_test_state->snapshot.valid = true; registry_test_state->snapshot.exists = true;
        registry_test_state->snapshot.values[name] = value; return ERROR_SUCCESS;
    }
#endif
    return RegSetKeyValueW(HKEY_CURRENT_USER, kUninstallKey, name.c_str(), value.type, value.bytes.data(), static_cast<DWORD>(value.bytes.size()));
}
LSTATUS delete_uninstall_value(const std::wstring& name) {
#ifdef SZU_INSTALLATION_TESTING
    if (registry_test_state) {
        if (++registry_test_state->mutations == registry_test_state->failure_at) return ERROR_ACCESS_DENIED;
        registry_test_state->snapshot.values.erase(name); return ERROR_SUCCESS;
    }
#endif
    return RegDeleteKeyValueW(HKEY_CURRENT_USER, kUninstallKey, name.c_str());
}
LSTATUS delete_uninstall_key() {
#ifdef SZU_INSTALLATION_TESTING
    if (registry_test_state) {
        if (++registry_test_state->mutations == registry_test_state->failure_at) return ERROR_ACCESS_DENIED;
        registry_test_state->snapshot = {true, false, {}}; return ERROR_SUCCESS;
    }
#endif
    return RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
}
RegistrySnapshot read_uninstall_key() {
#ifdef SZU_INSTALLATION_TESTING
    if (registry_test_state) {
        if (registry_test_state->failure_at == -1 && ++registry_test_state->reads == 2)
            registry_test_state->snapshot.values[L"ConcurrentUserValue"] = {REG_SZ, wide_bytes(L"preserve")};
        return registry_test_state->snapshot;
    }
#endif
    RegistrySnapshot result;
    HKEY key = nullptr;
    const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_READ, &key);
    if (opened == ERROR_FILE_NOT_FOUND) { result.valid = true; return result; }
    if (opened != ERROR_SUCCESS) return result;
    struct CloseKey { HKEY value; ~CloseKey() { RegCloseKey(value); } } close{key};
    DWORD subkeys = 0, count = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &count, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS ||
        subkeys || count > 32) return result;
    for (DWORD index = 0; index < count; ++index) {
        std::array<wchar_t, 256> name{}; DWORD length = static_cast<DWORD>(name.size()), size = 65536, type = 0;
        std::vector<BYTE> bytes(size);
        if (RegEnumValueW(key, index, name.data(), &length, nullptr, &type, bytes.data(), &size) != ERROR_SUCCESS) return {};
        bytes.resize(size);
        result.values.emplace(std::wstring(name.data(), length), RegistryValue{type, std::move(bytes)});
    }
    result.valid = true; result.exists = true;
    return result;
}
RegistryValue registry_string(const std::wstring& value) { return {REG_SZ, wide_bytes(value)}; }
RegistryValue registry_dword(DWORD value) {
    std::vector<BYTE> bytes(sizeof value); std::memcpy(bytes.data(), &value, sizeof value);
    return {REG_DWORD, std::move(bytes)};
}
std::wstring uninstall_command(const fs::path& root) { return L"\"" + (root / L"current" / L"SZUConnect.exe").native() + L"\" --uninstall"; }
bool owned_uninstall_key(const RegistrySnapshot& snapshot, const fs::path& root) {
    if (!snapshot.valid) return false;
    if (!snapshot.exists) return true;
    const auto owner = snapshot.values.find(L"SZUConnectOwner"), command = snapshot.values.find(L"UninstallString");
    return owner != snapshot.values.end() && owner->second == registry_string(L"SZUConnect.ManagedUninstall.v1|" + root.native()) &&
        command != snapshot.values.end() && command->second == registry_string(uninstall_command(root));
}
struct UninstallRegistration { RegistrySnapshot before, after; bool changed = false; };
bool remove_registration_snapshot(const RegistrySnapshot& expected) {
    if (!expected.valid || read_uninstall_key() != expected) return false;
    if (!expected.exists) return true;
    RegistryValues deleted;
    auto step = expected;
    const auto restore_deleted = [&] {
        for (const auto& [name, value] : deleted) {
            const auto now = read_uninstall_key();
            if (!now.valid) continue;
            if (now.values.find(name) == now.values.end())
                set_uninstall_value(name, value);
        }
    };
    // Delete only values whose original bytes were checked; never recursively delete keys.
    for (const auto& [name, value] : expected.values) {
        const auto now = read_uninstall_key();
        if (now != step || delete_uninstall_value(name) != ERROR_SUCCESS) {
            restore_deleted(); return false;
        }
        deleted.emplace(name, value); step.values.erase(name);
    }
    const auto now = read_uninstall_key();
    if (now == step && now.valid && now.exists && now.values.empty() && delete_uninstall_key() == ERROR_SUCCESS) return true;
    restore_deleted(); return false;
}
bool apply_uninstall_registration(UninstallRegistration& change, const fs::path& root) {
    if (!change.before.valid || !owned_uninstall_key(change.before, root) || read_uninstall_key() != change.before) return false;
    const auto version = file_version(root / L"current" / L"SZUConnect.exe");
    if (!version) return false;
    auto desired = change.before.values;
    desired[L"DisplayName"] = registry_string(L"深大随连");
    desired[L"DisplayVersion"] = registry_string(std::to_wstring((*version)[0]) + L"." + std::to_wstring((*version)[1]) + L"." + std::to_wstring((*version)[2]));
    desired[L"Publisher"] = registry_string(L"zhihebai");
    desired[L"InstallLocation"] = registry_string(root.native());
    desired[L"DisplayIcon"] = registry_string((root / L"current" / L"SZUConnect.exe").native());
    desired[L"UninstallString"] = registry_string(uninstall_command(root));
    desired[L"SZUConnectOwner"] = registry_string(L"SZUConnect.ManagedUninstall.v1|" + root.native());
    desired[L"URLInfoAbout"] = registry_string(L"https://github.com/zhihebai/SZUConnect");
    desired[L"NoModify"] = registry_dword(1); desired[L"NoRepair"] = registry_dword(1);
    change.after = change.before;
    for (const auto& [name, value] : desired) {
        if (read_uninstall_key() != change.after) return false;
        if (set_uninstall_value(name, value) != ERROR_SUCCESS) return false;
        change.after.valid = true; change.after.exists = true; change.after.values[name] = value; change.changed = true;
    }
    return read_uninstall_key() == change.after;
}
bool restore_uninstall_registration(UninstallRegistration& change) {
    if (!change.changed) return true;
    if (!change.before.exists) return remove_registration_snapshot(change.after);
    if (read_uninstall_key() != change.after) return false;
    auto step = change.after;
    // Keep the existing owner and command intact while restoring changed fields.
    for (const auto& [name, value] : change.before.values) {
        const auto found = step.values.find(name);
        if (found != step.values.end() && found->second == value) continue;
        if (read_uninstall_key() != step || set_uninstall_value(name, value) != ERROR_SUCCESS) return false;
        step.values[name] = value;
    }
    for (const auto& [name, value] : change.after.values) {
        if (change.before.values.contains(name)) continue;
        if (read_uninstall_key() != step || delete_uninstall_value(name) != ERROR_SUCCESS) return false;
        step.values.erase(name);
    }
    return read_uninstall_key() == change.before;
}

Payload embedded_payload(HINSTANCE instance) {
    Payload payload; payload.source = executable_path();
    const std::array<int, 4> ids{kInstallReadmeResource, kInstallLicenseResource, kInstallNoticesResource, kInstallCurlResource};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(ids[i]), RT_RCDATA);
        const DWORD size = resource ? SizeofResource(instance, resource) : 0;
        HGLOBAL loaded = resource ? LoadResource(instance, resource) : nullptr;
        const void* bytes = loaded ? LockResource(loaded) : nullptr;
        if (!bytes || !size || size > kDocumentLimit) return {};
        payload.documents.emplace_back(kDocumentNames[i], std::string(static_cast<const char*>(bytes), size));
    }
    return payload;
}
const std::array<const wchar_t*, 9> kDataNames{L"settings.json", L"recovery.json", L"network-memory.json", L"network-labels.json",
    L"wifi-recovery.json", L"websites.json", L"events.json", L"health.json", L"proxy-assistant.backup"};
bool numeric_parts(std::wstring_view suffix, std::size_t minimum, std::size_t maximum) {
    std::size_t count = 0, length = 0;
    for (wchar_t c : suffix) {
        if (c == L'-') { if (!length) return false; ++count; length = 0; }
        else if (c >= L'0' && c <= L'9') { if (++length > 20) return false; }
        else return false;
    }
    return length && ++count >= minimum && count <= maximum;
}
bool owned_data_name(const std::wstring& name) {
    for (const auto* base : kDataNames) {
        const std::wstring_view full(name), prefix(base);
        if (full == prefix) return true;
        if (!full.starts_with(prefix)) continue;
        auto tail = full.substr(prefix.size());
        if (tail.starts_with(L".tmp-")) return numeric_parts(tail.substr(5), 2, 2);
        if (tail.starts_with(L".corrupt-")) {
            tail.remove_prefix(9);
            const auto temporary = tail.find(L".tmp-");
            if (temporary != std::wstring_view::npos) {
                if (!numeric_parts(tail.substr(temporary + 5), 2, 2)) return false;
                tail = tail.substr(0, temporary);
            }
            return numeric_parts(tail, 2, 3);
        }
    }
    return false;
}
struct FileDigest { fs::path path; std::string digest; };
bool collect_data(const fs::path& folder, std::vector<FileDigest>& files) {
    if (!safe_ancestors(folder)) return false;
    if (!path_exists(folder)) return true;
    if (!plain_directory(folder)) return false;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!regular(it->path()) || !owned_data_name(it->path().filename().native()) || files.size() >= 4096) return false;
        const auto digest = sha256(it->path());
        if (digest.empty()) return false;
        files.push_back({it->path(), digest});
    }
    return !ec;
}
struct SavedFile { fs::path path; std::string bytes; };
struct ProgramBackup { std::vector<fs::path> directories; std::vector<SavedFile> files; };
bool save_program(const fs::path& root, ProgramBackup& backup) {
    if (!root_inventory(root) || !owned_payload(root / L"current")) return false;
    backup.directories.push_back(root);
    bool ok = false;
    auto bytes = read_bytes(root / L"installation.id", 128, ok);
    if (!ok) return false;
    backup.files.push_back({root / L"installation.id", std::move(bytes)});
    for (const auto* name : {L"current", L"previous"}) {
        const auto folder = root / name;
        if (!path_exists(folder)) continue;
        const auto* documents = owned_payload_directory(folder);
        if (!documents) return false;
        backup.directories.push_back(folder); backup.directories.push_back(folder / documents);
        auto names = payload_files(documents); names.emplace_back(kManifestName);
        for (const auto& file : names) {
            auto content = read_bytes(folder / file, 128 * 1024 * 1024, ok);
            if (!ok) return false;
            backup.files.push_back({folder / file, std::move(content)});
        }
    }
    return true;
}
bool restore_program(const ProgramBackup& backup, DirectoryPins* pins = nullptr) {
    for (const auto& directory : backup.directories) {
        if (!safe_ancestors(directory) || (path_exists(directory) ? !plain_directory(directory) : !CreateDirectoryW(directory.c_str(), nullptr)) ||
            (pins && !pins->pin(directory))) return false;
    }
    for (const auto& file : backup.files) {
        if (!safe_ancestors(file.path)) return false;
        if (path_exists(file.path)) {
            bool ok = false;
            if (read_bytes(file.path, 128 * 1024 * 1024, ok) != file.bytes || !ok) return false;
        } else if (!write_new(file.path, file.bytes)) return false;
    }
    return true;
}
struct UninstallCallbacks {
    std::function<bool()> stop = [] { return true; };
    std::function<bool()> erase_credentials = [] { return false; };
    std::function<bool()> cleanup_links = [] { return true; };
    bool fail_after_first_payload_file = false;
};
bool handle_matches_bytes(HANDLE file, const std::string& bytes) {
    LARGE_INTEGER size{}, start{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart != static_cast<LONGLONG>(bytes.size()) || !SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) return false;
    std::array<char, 65536> buffer{}; std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD count = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(std::min(buffer.size(), bytes.size() - offset)), &count, nullptr) || !count ||
            std::memcmp(buffer.data(), bytes.data() + offset, count) != 0) return false;
        offset += count;
    }
    return true;
}
TransactionResult uninstall_transaction(const fs::path& root, const fs::path& data, bool remove_data, UninstallCallbacks& callbacks) {
    DirectoryPins pins;
    if (!pins.pin(root)) return {false, "uninstall.unsafe"};
    ProgramBackup backup;
    if (!save_program(root, backup)) return {false, "uninstall.unsafe"};
    for (const auto& directory : backup.directories) if (!pins.pin(directory)) return {false, "uninstall.unsafe"};
    if (!callbacks.stop()) return {false, "uninstall.exitRequired"};
    // Shutdown can flush records; inventory and snapshots are checked again afterwards.
    if (!root_inventory(root) || !owned_payload(root / L"current")) return {false, "uninstall.unsafe"};
    std::vector<HANDLE> program_files;
    struct CloseProgramFiles { std::vector<HANDLE>& files; ~CloseProgramFiles() { for (HANDLE h : files) if (h) CloseHandle(h); } } close_program{program_files};
    for (const auto& file : backup.files) {
        HANDLE handle = CreateFileW(file.path.c_str(), DELETE | GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return {false, "uninstall.failed"};
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY)) ||
            !handle_matches_bytes(handle, file.bytes)) { CloseHandle(handle); return {false, "uninstall.unsafe"}; }
        program_files.push_back(handle);
    }
    if (remove_data) {
        if (!pins.pin(data)) return {false, "uninstall.dataFailed"};
        std::vector<FileDigest> files;
        if (!collect_data(data, files)) return {false, "uninstall.dataFailed"};
        // Acquire delete rights before erasing the credential or touching any data.
        std::vector<HANDLE> locks;
        struct CloseFiles { std::vector<HANDLE>& files; ~CloseFiles() { for (HANDLE h : files) CloseHandle(h); } } close{locks};
        for (const auto& file : files) {
            if (!safe_ancestors(file.path) || sha256(file.path) != file.digest) return {false, "uninstall.dataFailed"};
            HANDLE handle = CreateFileW(file.path.c_str(), DELETE | GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return {false, "uninstall.dataFailed"};
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_READONLY)) ||
                sha256_handle(handle) != file.digest) {
                CloseHandle(handle); return {false, "uninstall.dataFailed"};
            }
            locks.push_back(handle);
        }
        if (!callbacks.erase_credentials()) return {false, "uninstall.dataFailed"};
        for (HANDLE handle : locks) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof disposition)) return {false, "uninstall.dataFailed"};
        }
        for (HANDLE handle : locks) CloseHandle(handle);
        locks.clear();
        pins.release(data);
        if (path_exists(data) && !RemoveDirectoryW(data.c_str())) return {false, "uninstall.dataFailed"};
    }
    const auto rollback = [&] {
        for (auto& handle : program_files) if (handle) { CloseHandle(handle); handle = nullptr; }
        return TransactionResult{false, restore_program(backup, &pins) ? "uninstall.failed" : "uninstall.restoreFailed"};
    };
    std::size_t removed = 0;
    for (std::size_t i = program_files.size(); i-- > 0;) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(program_files[i], FileDispositionInfo, &disposition, sizeof disposition))
            return rollback();
        CloseHandle(program_files[i]); program_files[i] = nullptr;
        if (callbacks.fail_after_first_payload_file && ++removed == 1)
            return rollback();
    }
    for (auto it = backup.directories.rbegin(); it != backup.directories.rend(); ++it) {
        pins.release(*it);
        if (!safe_ancestors(*it) || !RemoveDirectoryW(it->c_str())) return rollback();
    }
    if (!callbacks.cleanup_links()) {
        return rollback();
    }
    return {true, {}};
}
std::wstring localized(std::string_view key) {
    const auto text = copy::text(key);
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring value(static_cast<std::size_t>(std::max(0, count)), L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), value.data(), count);
    return value;
}
void uninstall_message(std::string_view key, bool error = true) {
    const auto text = localized(key), title = localized("uninstall.title");
    MessageBoxW(nullptr, text.c_str(), title.c_str(), MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}
bool confirm_uninstall(HINSTANCE instance, bool& remove_data) {
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    const auto title = localized("uninstall.title"), detail = localized("uninstall.detail"), checkbox = localized("uninstall.removeData");
    const auto confirm = localized("uninstall.confirm"), cancel = localized("action.cancel");
    const TASKDIALOG_BUTTON buttons[]{{IDOK, confirm.c_str()}, {IDCANCEL, cancel.c_str()}};
    TASKDIALOGCONFIG config{}; config.cbSize = sizeof config; config.hInstance = instance;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle = title.c_str(); config.pszMainInstruction = title.c_str(); config.pszContent = detail.c_str();
    config.pszVerificationText = checkbox.c_str(); config.cButtons = 2; config.pButtons = buttons; config.nDefaultButton = IDCANCEL;
    BOOL checked = FALSE; int selected = IDCANCEL;
    const auto result = TaskDialogIndirect(&config, &selected, nullptr, &checked);
    remove_data = checked != FALSE;
    return SUCCEEDED(result) && selected == IDOK;
}
bool remove_owned_run(const RunSnapshot& run, const fs::path& installed) {
    if (!run.exists || !equal_path(run.target, installed)) return true;
    const auto now = read_run();
    if (!now.exists || now.type != run.type || now.bytes != run.bytes) return true;
    RegistryValue approved; DWORD size = 65536;
    approved.bytes.resize(size);
    const bool has_approved = RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kRunName, RRF_RT_ANY,
        &approved.type, approved.bytes.data(), &size) == ERROR_SUCCESS;
    approved.bytes.resize(size <= 65536 ? size : 0);
    const auto result = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName);
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) return false;
    if (has_approved) {
        RegistryValue current; DWORD current_size = 65536; current.bytes.resize(current_size);
        const auto status = RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kRunName, RRF_RT_ANY,
            &current.type, current.bytes.data(), &current_size);
        current.bytes.resize(current_size <= 65536 ? current_size : 0);
        if (status == ERROR_SUCCESS && current == approved) {
            const auto removed = RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kRunName);
            if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) return false;
        }
    }
    return true;
}
bool remove_owned_shortcuts(const fs::path& installed) {
    for (const auto& folder : {known_folder(FOLDERID_Programs), known_folder(FOLDERID_Desktop)}) {
        if (folder.empty() || !safe_ancestors(folder) || !plain_directory(folder)) continue;
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            const auto path = it->path();
            if (_wcsicmp(path.extension().c_str(), L".lnk") != 0 || !regular(path) || !equal_path(shortcut_target(path), installed)) continue;
            bool ok = false; const auto before = read_bytes(path, kDocumentLimit, ok);
            if (!ok) return false;
            if (read_bytes(path, kDocumentLimit, ok) != before || !ok || !equal_path(shortcut_target(path), installed)) continue;
            if (!DeleteFileW(path.c_str())) return false;
        }
        if (ec) return false;
    }
    return true;
}
struct WorkerRequest {
    std::uint32_t magic = 0x535a5531, size = sizeof(WorkerRequest), pid = 0, child_pid = 0, flags = 0;
    std::uintptr_t parent_handle = 0;
    std::uintptr_t cleanup_done_handle = 0;
    LONG decision = 0;
};
bool handoff_committed(LONG decision) { return decision == 1; }
bool complete_handoff(WorkerRequest& request, bool accepted, const std::function<bool()>& signal) {
    InterlockedExchange(&request.decision, accepted ? 1 : -1);
    if (!signal()) { InterlockedExchange(&request.decision, -1); return false; }
    return accepted;
}
DWORD actual_parent_id() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.value) return 0;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof entry;
    if (!Process32FirstW(snapshot.value, &entry)) return 0;
    do { if (entry.th32ProcessID == GetCurrentProcessId()) return entry.th32ParentProcessID; } while (Process32NextW(snapshot.value, &entry));
    return 0;
}
fs::path temporary_parent() {
    const auto local = known_folder(FOLDERID_LocalAppData);
    return local.empty() ? fs::path{} : local / L"Temp";
}
bool valid_worker_path(const std::wstring& token, const fs::path& helper, const fs::path& parent) {
    return valid_token(token) && safe_ancestors(helper) && safe_ancestors(parent) &&
        equal_path(helper, parent / (L"SZUConnect-Uninstall-" + token) / L"SZUConnect.exe");
}
std::string utf8(const std::wstring& value) {
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(0, count)), '\0');
    if (count) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::wstring quote_argument(std::wstring_view value) {
    std::wstring quoted = L"\""; std::size_t slashes = 0;
    for (const auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        quoted.append(slashes * (c == L'\"' ? 2 : 1), L'\\'); slashes = 0;
        if (c == L'\"') quoted += L'\\';
        quoted += c;
    }
    quoted.append(slashes * 2, L'\\'); quoted += L'\"'; return quoted;
}
std::string file_identity(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) return {};
    char text[40]{};
    std::snprintf(text, sizeof text, "%08lX:%08lX:%08lX", info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow);
    return text;
}
bool worker_inventory(const fs::path& helper) {
    if (!plain_directory(helper.parent_path()) || !regular(helper) || !safe_ancestors(helper)) return false;
    std::error_code ec; std::size_t count = 0;
    for (fs::directory_iterator it(helper.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!equal_path(it->path(), helper) || !regular(it->path())) return false;
        ++count;
    }
    return !ec && count == 1;
}

// This is fixed program text. Paths, handles and result messages arrive only as
// JSON on an anonymous pipe; they never become PowerShell source or arguments.
constexpr wchar_t kCleanupScript[] = LR"PS(
$ProgressPreference = 'SilentlyContinue'
$ErrorActionPreference = 'Stop'
[Console]::InputEncoding = [Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$pins = @(); $target = $null; $folder = $null; $stream = $null
$worker = $null; $done = $null; $passed = $null; $ready = $false
$result = $null; $cleaned = $false; $cfg = $null
function WaitHandle([string] $value) {
    $item = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::AutoReset)
    $item.SafeWaitHandle = [Microsoft.Win32.SafeHandles.SafeWaitHandle]::new([IntPtr][long]$value, $true)
    return $item
}
function Inventory {
    $items = @([IO.Directory]::EnumerateFileSystemEntries($cfg.folder))
    if ($items.Count -ne 1 -or -not [string]::Equals($items[0], $cfg.helper, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unexpected temporary files'
    }
}
try {
    $line = [Console]::ReadLine()
    if (-not $line -or $line.Length -gt 16384) { throw 'Missing cleanup request' }
    $cfg = $line | ConvertFrom-Json
    if ($cfg.schema -ne 1 -or $cfg.token -cnotmatch '^[0-9a-f]{32}$') { throw 'Invalid cleanup request' }
    if ($cfg.done -ne '0') { $done = WaitHandle $cfg.done }
    if ($cfg.passed -ne '0') { $passed = WaitHandle $cfg.passed }
    $expected = [IO.Path]::Combine($cfg.parent, ('SZUConnect-Uninstall-' + $cfg.token))
    if (-not [string]::Equals($expected, $cfg.folder, [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals([IO.Path]::Combine($expected, 'SZUConnect.exe'), $cfg.helper, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Invalid cleanup location'
    }
    foreach ($pin in $cfg.pins) {
        $pins += [Microsoft.Win32.SafeHandles.SafeFileHandle]::new([IntPtr][long]$pin, $true)
    }
    $target = [Microsoft.Win32.SafeHandles.SafeFileHandle]::new([IntPtr][long]$cfg.fileHandle, $true)
    $folder = [Microsoft.Win32.SafeHandles.SafeFileHandle]::new([IntPtr][long]$cfg.folderHandle, $true)
    $worker = WaitHandle $cfg.workerHandle
    $native = @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class SZUCleanupNative {
    [StructLayout(LayoutKind.Sequential)] private struct Info {
        public uint Attributes, CreationLow, CreationHigh, AccessLow, AccessHigh, WriteLow, WriteHigh;
        public uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
    }
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern bool GetFileInformationByHandle(SafeFileHandle handle, out Info info);
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern bool SetFileInformationByHandle(SafeFileHandle handle, int kind, ref int value, uint size);
    [DllImport("kernel32.dll", EntryPoint="SetFileInformationByHandle", SetLastError=true)]
    private static extern bool SetDisposition(SafeFileHandle handle, int kind, ref byte value, uint size);
    public static string Identity(SafeFileHandle handle, bool directory) {
        Info info;
        if (!GetFileInformationByHandle(handle, out info)) throw new Win32Exception(Marshal.GetLastWin32Error());
        if ((info.Attributes & 0x400) != 0 || ((info.Attributes & 0x10) != 0) != directory || (!directory && info.Links != 1))
            throw new InvalidOperationException("Unexpected temporary object");
        return String.Format(System.Globalization.CultureInfo.InvariantCulture, "{0:X8}:{1:X8}:{2:X8}", info.Volume, info.IndexHigh, info.IndexLow);
    }
    public static void Delete(SafeFileHandle handle) {
        int flags = 0x13;
        if (SetFileInformationByHandle(handle, 21, ref flags, 4)) return;
        int error = Marshal.GetLastWin32Error();
        if (error != 1 && error != 50 && error != 87) throw new Win32Exception(error);
        byte delete = 1;
        if (!SetDisposition(handle, 4, ref delete, 1)) throw new Win32Exception(Marshal.GetLastWin32Error());
    }
}
'@
    Add-Type -TypeDefinition $native -ErrorAction Stop
    Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
    if ([SZUCleanupNative]::Identity($target, $false) -cne $cfg.fileIdentity -or
        [SZUCleanupNative]::Identity($folder, $true) -cne $cfg.folderIdentity) { throw 'Changed temporary object' }
    Inventory
    $stream = [IO.FileStream]::new($target, [IO.FileAccess]::Read)
    $stream.Position = 0
    $hash = [Security.Cryptography.SHA256]::Create()
    try { $digest = [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $hash.Dispose() }
    if ($digest -cne $cfg.digest) { throw 'Changed temporary image' }
    [Console]::WriteLine('SZU-CLEANUP-READY-1'); [Console]::Out.Flush(); $ready = $true
    $result = [pscustomobject]@{kind = 'cancel'; body = ''}
    $line = [Console]::ReadLine()
    if ($line -and $line.Length -le 16384) {
        $phase = $null
        try { $phase = $line | ConvertFrom-Json } catch { }
        if ($phase.stage -eq 'committed') {
            $result = [pscustomobject]@{kind = 'error'; body = [string]$cfg.incomplete}
            try {
                $line = [Console]::ReadLine()
                if (-not $line -or $line.Length -gt 16384) { throw 'Missing final result' }
                $final = $line | ConvertFrom-Json
                if ($final.commit -ne $true -or $final.kind -notin @('success', 'error', 'cancel')) { throw 'Invalid final result' }
                $result = $final
            } catch { }
        }
    }
    if (-not $worker.WaitOne(60000)) { throw 'Temporary process is still running' }
    Inventory
    if ([SZUCleanupNative]::Identity($target, $false) -cne $cfg.fileIdentity -or
        [SZUCleanupNative]::Identity($folder, $true) -cne $cfg.folderIdentity) { throw 'Changed temporary object' }
    [SZUCleanupNative]::Delete($target)
    $stream.Dispose(); $stream = $null; $target.Dispose(); $target = $null
    [SZUCleanupNative]::Delete($folder)
    $folder.Dispose(); $folder = $null
    $cleaned = -not [IO.Directory]::Exists($cfg.folder) -and -not [IO.File]::Exists($cfg.helper)
    if (-not $cleaned) { throw 'Temporary cleanup incomplete' }
} catch {
    if (-not $ready) { [Console]::WriteLine('SZU-CLEANUP-UNAVAILABLE-1'); [Console]::Out.Flush() }
} finally {
    if ($stream) { $stream.Dispose() }
    if ($target) { $target.Dispose() }
    if ($folder) { $folder.Dispose() }
    foreach ($pin in $pins) { $pin.Dispose() }
    if ($worker) { $worker.Dispose() }
    if ($cleaned -and $passed) { [void]$passed.Set() }
    if ($done) { [void]$done.Set(); $done.Dispose() }
    if ($passed) { $passed.Dispose() }
}
if ($ready -and $result -and $result.kind -ne 'cancel' -and $cfg.show) {
    $body = [string]$result.body
    if (-not $cleaned) {
        if ($body) { $body += [Environment]::NewLine + [Environment]::NewLine }
        $body += [string]$cfg.failure
    }
    if ($body) {
        $icon = [Windows.Forms.MessageBoxIcon]::Information
        if ($result.kind -eq 'error' -or -not $cleaned) { $icon = [Windows.Forms.MessageBoxIcon]::Warning }
        [void][Windows.Forms.MessageBox]::Show($body, [string]$cfg.title, [Windows.Forms.MessageBoxButtons]::OK, $icon)
    }
}
if ($cleaned) { exit 0 } else { exit 6 }
)PS";

enum class CleanupReadiness { notStarted, ready, unavailable, timeout, exited, closed, invalidOutput };
template<class Wait>
CleanupReadiness wait_cleanup_readiness(HANDLE output, HANDLE process, DWORD timeout_ms, Wait wait) {
    std::string line;
    bool exited = false;
    const auto start = GetTickCount64();
    while (GetTickCount64() - start < timeout_ms) {
        DWORD available = 0;
        if (!PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr)) return CleanupReadiness::closed;
        if (available) {
            char next = 0; DWORD count = 0;
            if (!ReadFile(output, &next, 1, &count, nullptr) || !count) return CleanupReadiness::closed;
            if (next == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line == "SZU-CLEANUP-READY-1") return CleanupReadiness::ready;
                if (line == "SZU-CLEANUP-UNAVAILABLE-1") return CleanupReadiness::unavailable;
                return CleanupReadiness::invalidOutput;
            }
            if (line.size() >= 1024) return CleanupReadiness::invalidOutput;
            line += next;
        } else if (exited) return CleanupReadiness::exited;
        else if (wait(process, 10) == WAIT_OBJECT_0) {
            // Exit can race with the last stdout write after PeekNamedPipe.
            // Drain that bounded protocol line before classifying the exit.
            exited = true;
        }
    }
    return CleanupReadiness::timeout;
}
CleanupReadiness wait_cleanup_readiness(HANDLE output, HANDLE process, DWORD timeout_ms) {
    return wait_cleanup_readiness(output, process, timeout_ms,
        [](HANDLE value, DWORD wait_ms) { return WaitForSingleObject(value, wait_ms); });
}

struct CleanupSession {
    Handle process, input, output, protected_image;
    std::string image_digest, image_identity;
    bool ready = false;
    CleanupReadiness readiness = CleanupReadiness::notStarted;
    bool matches_program_image(const fs::path& installed) const {
        if (!ready || !protected_image.value || image_digest.empty() || image_identity.empty() ||
            file_identity(protected_image.value) != image_identity || sha256_handle(protected_image.value) != image_digest) return false;
        return sha256(installed) == image_digest;
    }
    bool commit() {
        auto phase = json::Value::object(); phase.set("stage", json::Value::string("committed"));
        return ready && write(phase);
    }
    bool write(const json::Value& value) {
        if (!input.value) return false;
        const auto bytes = json::serialize(value) + "\n"; DWORD written = 0;
        return bytes.size() <= 16384 && WriteFile(input.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    }
    bool finish(std::string_view kind, const std::wstring& body = {}) {
        auto result = json::Value::object(); result.set("commit", json::Value::boolean(true));
        result.set("kind", json::Value::string(std::string(kind))); result.set("body", json::Value::string(utf8(body)));
        if (kind == "cancel") result.set("stage", json::Value::string("cancel"));
        const bool sent = ready && write(result);
        if (input.value) { CloseHandle(input.value); input.value = nullptr; }
        return sent;
    }
};
enum class CleanupFailureStage {
    requestValidation, workerInventory, directoryPins, directoryHandle, imageHandle, processHandle,
    imageDigest, imageIdentity, directoryIdentity, inheritedPins, inheritedHandle, stderrHandle,
    inputPipe, inputInheritance, outputPipe, outputInheritance, systemDirectory, cleanerImage,
    processAttributes, inheritedHandles, cleanerProcess, initialMessage, readiness,
    guardTransaction, forgedImageWrite, forgedImageDigest, forgedManifest, forgedInventory
};
bool cleanup_stage_failure(CleanupFailureStage stage, DWORD error) {
#ifdef SZU_INSTALLATION_TESTING
    const char* name = "unknown";
    switch (stage) {
        case CleanupFailureStage::requestValidation: name = "request-validation"; break;
        case CleanupFailureStage::workerInventory: name = "worker-inventory"; break;
        case CleanupFailureStage::directoryPins: name = "directory-pins"; break;
        case CleanupFailureStage::directoryHandle: name = "directory-handle"; break;
        case CleanupFailureStage::imageHandle: name = "image-handle"; break;
        case CleanupFailureStage::processHandle: name = "process-handle"; break;
        case CleanupFailureStage::imageDigest: name = "image-digest"; break;
        case CleanupFailureStage::imageIdentity: name = "image-identity"; break;
        case CleanupFailureStage::directoryIdentity: name = "directory-identity"; break;
        case CleanupFailureStage::inheritedPins: name = "inherited-pins"; break;
        case CleanupFailureStage::inheritedHandle: name = "inherited-handle"; break;
        case CleanupFailureStage::stderrHandle: name = "stderr-handle"; break;
        case CleanupFailureStage::inputPipe: name = "input-pipe"; break;
        case CleanupFailureStage::inputInheritance: name = "input-inheritance"; break;
        case CleanupFailureStage::outputPipe: name = "output-pipe"; break;
        case CleanupFailureStage::outputInheritance: name = "output-inheritance"; break;
        case CleanupFailureStage::systemDirectory: name = "system-directory"; break;
        case CleanupFailureStage::cleanerImage: name = "cleaner-image"; break;
        case CleanupFailureStage::processAttributes: name = "process-attributes"; break;
        case CleanupFailureStage::inheritedHandles: name = "inherited-handles"; break;
        case CleanupFailureStage::cleanerProcess: name = "cleaner-process"; break;
        case CleanupFailureStage::initialMessage: name = "initial-message"; break;
        case CleanupFailureStage::readiness: name = "readiness"; break;
        case CleanupFailureStage::guardTransaction: name = "guard-transaction"; break;
        case CleanupFailureStage::forgedImageWrite: name = "forged-image-write"; break;
        case CleanupFailureStage::forgedImageDigest: name = "forged-image-digest"; break;
        case CleanupFailureStage::forgedManifest: name = "forged-manifest"; break;
        case CleanupFailureStage::forgedInventory: name = "forged-inventory"; break;
    }
    std::printf("Cleanup fixture failure: stage=%s win32=%lu\n", name, error);
    std::fflush(stdout);
#else
    static_cast<void>(stage);
    static_cast<void>(error);
#endif
    return false;
}
#ifdef SZU_INSTALLATION_TESTING
bool cleaner_unavailable_for_test = false;
bool cleaner_stderr_prefix_for_test = false;
bool cleaner_stdout_reject_for_test = false;
#endif
bool begin_worker_cleanup(CleanupSession& session, const std::wstring& token, const fs::path& parent,
                          HANDLE done = nullptr, HANDLE passed = nullptr, bool show = true) {
#ifdef SZU_INSTALLATION_TESTING
    if (cleaner_unavailable_for_test) return false;
#endif
    const auto helper = executable_path(), folder = helper.parent_path();
    if (!valid_worker_path(token, helper, parent)) return cleanup_stage_failure(CleanupFailureStage::requestValidation, GetLastError());
    if (!worker_inventory(helper)) return cleanup_stage_failure(CleanupFailureStage::workerInventory, GetLastError());
    DirectoryPins pins;
    if (!pins.pin(parent)) return cleanup_stage_failure(CleanupFailureStage::directoryPins, GetLastError());
    Handle directory(CreateFileW(folder.c_str(), DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    const DWORD directory_error = directory.value ? ERROR_SUCCESS : GetLastError();
    Handle target(CreateFileW(helper.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    const DWORD image_error = target.value ? ERROR_SUCCESS : GetLastError();
    Handle worker(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId()));
    const DWORD process_error = worker.value ? ERROR_SUCCESS : GetLastError();
    if (!directory.value) return cleanup_stage_failure(CleanupFailureStage::directoryHandle, directory_error);
    if (!target.value) return cleanup_stage_failure(CleanupFailureStage::imageHandle, image_error);
    if (!worker.value) return cleanup_stage_failure(CleanupFailureStage::processHandle, process_error);
    if (!worker_inventory(helper)) return cleanup_stage_failure(CleanupFailureStage::workerInventory, GetLastError());
    const auto digest = sha256_handle(target.value);
    const DWORD digest_error = digest.empty() ? GetLastError() : ERROR_SUCCESS;
    const auto file_id = file_identity(target.value);
    const DWORD identity_error = file_id.empty() ? GetLastError() : ERROR_SUCCESS;
    const auto folder_id = file_identity(directory.value);
    const DWORD folder_error = folder_id.empty() ? GetLastError() : ERROR_SUCCESS;
    if (digest.empty()) return cleanup_stage_failure(CleanupFailureStage::imageDigest, digest_error);
    if (file_id.empty()) return cleanup_stage_failure(CleanupFailureStage::imageIdentity, identity_error);
    if (folder_id.empty()) return cleanup_stage_failure(CleanupFailureStage::directoryIdentity, folder_error);
    std::vector<HANDLE> inherited;
    struct CloseCopies { std::vector<HANDLE>& values; ~CloseCopies() { for (const auto value : values) CloseHandle(value); } } copies{inherited};
    auto inherit = [&](HANDLE value) {
        HANDLE copy = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), value, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            cleanup_stage_failure(CleanupFailureStage::inheritedHandle, GetLastError());
            return static_cast<HANDLE>(nullptr);
        }
        inherited.push_back(copy); return copy;
    };
    if (!pins.inherit(inherited)) return cleanup_stage_failure(CleanupFailureStage::inheritedPins, GetLastError());
    auto config = json::Value::object(), pin_values = json::Value::array();
    for (const auto value : inherited) pin_values.push(json::Value::string(std::to_string(reinterpret_cast<std::uintptr_t>(value))));
    config.set("pins", std::move(pin_values));
    const auto worker_copy = inherit(worker.value), file_copy = inherit(target.value), folder_copy = inherit(directory.value);
    const auto done_copy = done ? inherit(done) : nullptr, passed_copy = passed ? inherit(passed) : nullptr;
    if (!worker_copy || !file_copy || !folder_copy || (done && !done_copy) || (passed && !passed_copy)) return false;
    config.set("schema", json::Value::number(1)); config.set("token", json::Value::string(utf8(token)));
    config.set("parent", json::Value::string(utf8(parent.native()))); config.set("folder", json::Value::string(utf8(folder.native())));
    config.set("helper", json::Value::string(utf8(helper.native()))); config.set("digest", json::Value::string(digest));
    config.set("fileIdentity", json::Value::string(file_id)); config.set("folderIdentity", json::Value::string(folder_id));
    auto add_handle = [&](const char* name, HANDLE value) { config.set(name, json::Value::string(std::to_string(reinterpret_cast<std::uintptr_t>(value)))); };
    add_handle("workerHandle", worker_copy); add_handle("fileHandle", file_copy); add_handle("folderHandle", folder_copy);
    add_handle("done", done_copy); add_handle("passed", passed_copy);
    config.set("show", json::Value::boolean(show)); config.set("title", json::Value::string(utf8(localized("uninstall.title"))));
    config.set("failure", json::Value::string(utf8(localized("uninstall.temporaryRemaining"))));
    config.set("incomplete", json::Value::string(utf8(localized("uninstall.failed"))));
    SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE}; HANDLE child_input = nullptr, child_output = nullptr;
    Handle stderr_sink(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!stderr_sink.value) return cleanup_stage_failure(CleanupFailureStage::stderrHandle, GetLastError());
    const auto error_handle = stderr_sink.value;
    inherited.push_back(error_handle); stderr_sink.value = nullptr;
    if (!CreatePipe(&child_input, &session.input.value, &security, 16384)) return cleanup_stage_failure(CleanupFailureStage::inputPipe, GetLastError());
    Handle stdin_read(child_input);
    if (!SetHandleInformation(session.input.value, HANDLE_FLAG_INHERIT, 0)) return cleanup_stage_failure(CleanupFailureStage::inputInheritance, GetLastError());
    if (!CreatePipe(&session.output.value, &child_output, &security, 16384)) return cleanup_stage_failure(CleanupFailureStage::outputPipe, GetLastError());
    Handle stdout_write(child_output);
    if (!SetHandleInformation(session.output.value, HANDLE_FLAG_INHERIT, 0)) return cleanup_stage_failure(CleanupFailureStage::outputInheritance, GetLastError());
    inherited.push_back(stdin_read.value); stdin_read.value = nullptr;
    inherited.push_back(stdout_write.value); stdout_write.value = nullptr;
    const auto input_handle = inherited[inherited.size() - 2], output_handle = inherited.back();
    wchar_t system[MAX_PATH + 1]{};
    const auto system_count = GetSystemDirectoryW(system, MAX_PATH);
    if (!system_count || system_count >= MAX_PATH) return cleanup_stage_failure(CleanupFailureStage::systemDirectory, GetLastError());
    const auto executable = fs::path(system) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    if (!regular(executable) || !safe_ancestors(executable)) return cleanup_stage_failure(CleanupFailureStage::cleanerImage, GetLastError());
    SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<BYTE> attributes_bytes(size); auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_bytes.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) return cleanup_stage_failure(CleanupFailureStage::processAttributes, GetLastError());
    struct DeleteAttributes { LPPROC_THREAD_ATTRIBUTE_LIST value; ~DeleteAttributes() { DeleteProcThreadAttributeList(value); } } delete_attributes{attributes};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), inherited.size() * sizeof(HANDLE), nullptr, nullptr))
        return cleanup_stage_failure(CleanupFailureStage::inheritedHandles, GetLastError());
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof startup; startup.lpAttributeList = attributes;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input_handle; startup.StartupInfo.hStdOutput = output_handle;
    startup.StartupInfo.hStdError = error_handle;
    std::wstring script = kCleanupScript;
#ifdef SZU_INSTALLATION_TESTING
    if (cleaner_stderr_prefix_for_test)
        script = L"[Console]::Error.WriteLine('SZU-CLEANUP-FIXTURE-STDERR'); [Console]::Error.Flush();\n" + script;
    if (cleaner_stdout_reject_for_test)
        script = L"[void][Console]::ReadLine(); [Console]::Error.WriteLine('SZU-CLEANUP-READY-1'); [Console]::Error.Flush(); "
                 L"[Console]::WriteLine('SZU-CLEANUP-UNAVAILABLE-1'); [Console]::Out.Flush(); exit 6";
#endif
    auto command = quote_argument(executable.native()) + L" -NoLogo -NoProfile -NonInteractive -Command " + quote_argument(script);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, system, &startup.StartupInfo, &process)) return cleanup_stage_failure(CleanupFailureStage::cleanerProcess, GetLastError());
    Handle thread(process.hThread); session.process.value = process.hProcess;
    // The parent must not keep the child's pipe ends open, otherwise EOF would
    // never cancel a failed handoff.
    CloseHandle(inherited.back()); inherited.pop_back(); CloseHandle(inherited.back()); inherited.pop_back();
    if (!session.write(config)) return cleanup_stage_failure(CleanupFailureStage::initialMessage, GetLastError());
    // Cold PowerShell/module startup is allowed a bounded part of the parent's
    // 30-second handoff window. Only the validated stdout protocol grants READY.
    session.readiness = wait_cleanup_readiness(session.output.value, session.process.value, kCleanupReadyWaitMs);
    session.ready = session.readiness == CleanupReadiness::ready;
    if (!session.ready) {
        cleanup_stage_failure(CleanupFailureStage::readiness, GetLastError());
        CloseHandle(session.input.value); session.input.value = nullptr;
        WaitForSingleObject(session.process.value, 5000);
    } else {
        // The cleaner holds DELETE access and denies writes/renames until exit.
        // Reuse this exact image handle: a CRT path reopen does not share DELETE.
        session.protected_image.value = target.value; target.value = nullptr;
        session.image_digest = digest; session.image_identity = file_id;
    }
    return session.ready;
}
bool uninstall_worker_preflight(const fs::path& root, const RegistrySnapshot& registration, const CleanupSession& cleanup) {
    const auto installed = root / L"current" / L"SZUConnect.exe";
    return owned_uninstall_key(registration, root) && root_inventory(root) && cleanup.matches_program_image(installed);
}
bool spawn_uninstall_worker(const std::wstring& token, bool remove_data) {
    const auto parent = temporary_parent(), folder = parent / (L"SZUConnect-Uninstall-" + token);
    const auto source = executable_path(), helper = folder / L"SZUConnect.exe";
    if (!valid_token(token) || !safe_ancestors(parent) || !plain_directory(parent) || !safe_ancestors(folder) ||
        !CreateDirectoryW(folder.c_str(), nullptr)) return false;
    const auto cleanup = [&] {
        if (!path_exists(folder)) return;
        if (valid_worker_path(token, helper, parent) && worker_inventory(helper) && sha256(helper) == sha256(source)) {
            if (DeleteFileW(helper.c_str())) RemoveDirectoryW(folder.c_str());
        } else if (safe_ancestors(folder) && plain_directory(folder)) RemoveDirectoryW(folder.c_str());
    };
    if (!CopyFileW(source.c_str(), helper.c_str(), TRUE) || sha256(source).empty() || sha256(source) != sha256(helper)) { cleanup(); return false; }
    const auto map_name = L"Local\\SZUConnect.UninstallRequest.v1." + token;
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(WorkerRequest), map_name.c_str()));
    if (!mapping.value || GetLastError() == ERROR_ALREADY_EXISTS) { cleanup(); return false; }
    auto* request = static_cast<WorkerRequest*>(MapViewOfFile(mapping.value, FILE_MAP_WRITE, 0, 0, sizeof(WorkerRequest)));
    if (!request) { cleanup(); return false; }
    struct Unmap { void* pointer; ~Unmap() { UnmapViewOfFile(pointer); } } unmap{request};
    const auto event_name = L"Local\\SZUConnect.UninstallReady.v1." + token;
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
    const bool event_new = ready.value && GetLastError() != ERROR_ALREADY_EXISTS;
    const auto published_name = L"Local\\SZUConnect.UninstallPublished.v1." + token;
    Handle published(CreateEventW(nullptr, TRUE, FALSE, published_name.c_str()));
    const bool published_new = published.value && GetLastError() != ERROR_ALREADY_EXISTS;
    const auto decision_name = L"Local\\SZUConnect.UninstallDecision.v1." + token;
    Handle decision(CreateEventW(nullptr, TRUE, FALSE, decision_name.c_str()));
    const bool decision_new = decision.value && GetLastError() != ERROR_ALREADY_EXISTS;
    SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE};
    Handle cleanup_done(CreateEventW(&security, TRUE, FALSE, nullptr));
    Handle parent_handle(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, GetCurrentProcessId()));
    if (!event_new || !published_new || !decision_new || !cleanup_done.value || !parent_handle.value) { cleanup(); return false; }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<BYTE> attribute_bytes(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_bytes.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) { cleanup(); return false; }
    struct DeleteAttributes { LPPROC_THREAD_ATTRIBUTE_LIST value; ~DeleteAttributes() { DeleteProcThreadAttributeList(value); } } delete_attributes{attributes};
    HANDLE inherited[]{parent_handle.value, cleanup_done.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, nullptr, nullptr)) { cleanup(); return false; }
    *request = WorkerRequest{}; request->pid = GetCurrentProcessId(); request->flags = remove_data ? 1u : 0u;
    request->parent_handle = reinterpret_cast<std::uintptr_t>(parent_handle.value);
    request->cleanup_done_handle = reinterpret_cast<std::uintptr_t>(cleanup_done.value);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof startup; startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + helper.native() + L"\" --uninstall-worker " + token;
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT,
        nullptr, parent.c_str(), &startup.StartupInfo, &process)) { cleanup(); return false; }
    Handle thread(process.hThread), child(process.hProcess);
    request->child_pid = process.dwProcessId;
    const bool announced = SetEvent(published.value) != FALSE;
    const HANDLE waits[]{ready.value, child.value};
    const bool accepted = announced && WaitForMultipleObjects(2, waits, FALSE, kWaitMs) == WAIT_OBJECT_0;
    const bool committed = complete_handoff(*request, accepted, [&] { return SetEvent(decision.value) != FALSE; });
    if (!committed) {
        WaitForSingleObject(child.value, 5000);
        WaitForSingleObject(cleanup_done.value, 2000);
        cleanup();
    }
    return committed;
}
InstallationResult failed(const std::string& detail) {
    const char* key = "installation.failed";
    if (detail == "installation.busy" || detail == "installation.lockFailed") key = "installation.busy";
    else if (detail == "installation.unsafePath" || detail == "installation.unrecognizedFiles" || detail == "installation.invalidReadyToken") key = "installation.unsafe";
    else if (detail == "installation.closeFailed") key = "installation.exitRequired";
    else if (detail == "installation.restoreFailed" || detail == "installation.closeFailedBackupRetained") key = "installation.rollbackFailed";
    else if (detail == "installation.newerInstalled") key = "installation.newerInstalled";
    return {InstallationAction::failed, key};
}
} // namespace

std::string embedded_license_text(HINSTANCE instance) {
    std::string text;
    for (const int id : {kInstallNoticesResource, kInstallCurlResource}) {
        HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
        const DWORD size = resource ? SizeofResource(instance, resource) : 0;
        HGLOBAL loaded = resource ? LoadResource(instance, resource) : nullptr;
        const void* bytes = loaded ? LockResource(loaded) : nullptr;
        if (!bytes || !size || size > kDocumentLimit) return {};
        if (!text.empty()) text += "\n\n";
        text.append(static_cast<const char*>(bytes), size);
    }
    return text;
}

InstallationResult prepare_installation(HINSTANCE instance, const InstallationOptions& options) {
    if (options.observe_only || options.offline_smoke || options.portable) {
        return options.ready_token.empty() ? InstallationResult{InstallationAction::continueStartup, {}} : failed("installation.invalidReadyToken");
    }
    const auto root = installation_root(), source = executable_path(), installed = root / L"current" / L"SZUConnect.exe";
    if (root.empty() || source.empty() || !safe_ancestors(root)) return failed("installation.unsafePath");
    if (equal_path(source, installed)) {
        if (options.ready_token.empty()) {
            Handle gate(CreateMutexW(nullptr, FALSE, L"Local\\SZUConnect.Installation.v1"));
            if (!gate.value) return failed("installation.lockFailed");
            const auto status = WaitForSingleObject(gate.value, 0);
            // The installer owns restarting the app. Do not start an old image
            // while another process is replacing its directory.
            if (status == WAIT_TIMEOUT) return {InstallationAction::handled, {}};
            if (status != WAIT_OBJECT_0 && status != WAIT_ABANDONED) return failed("installation.lockFailed");
            if (!root_identity(root) || !owned_payload(root / L"current")) { ReleaseMutex(gate.value); return failed("installation.unrecognizedFiles"); }
            UninstallRegistration registration{read_uninstall_key()};
            const bool registered = apply_uninstall_registration(registration, root);
            if (!registered) restore_uninstall_registration(registration);
            ReleaseMutex(gate.value);
            if (!registered) return failed("installation.shortcutFailed");
        }
        // Unknown interruption debris is retained. It blocks a further upgrade,
        // but must not prevent the intact installed application from running.
        if (!root_identity(root) || !owned_payload(root / L"current")) return failed("installation.unrecognizedFiles");
        if (!options.ready_token.empty() && !valid_token(options.ready_token)) return failed("installation.invalidReadyToken");
        return {InstallationAction::continueStartup, {}};
    }
    if (!options.ready_token.empty()) return failed("installation.invalidReadyToken");
    const auto payload = embedded_payload(instance);
    if (!document_input_valid(payload)) return failed("installation.resourcesMissing");
    Handle mutex(CreateMutexW(nullptr, FALSE, L"Local\\SZUConnect.Installation.v1"));
    if (!mutex.value) return failed("installation.lockFailed");
    const auto lock_status = WaitForSingleObject(mutex.value, kWaitMs);
    if (lock_status != WAIT_OBJECT_0 && lock_status != WAIT_ABANDONED) return failed("installation.busy");
    struct Unlock {
        HANDLE value;
        bool held = true;
        void release() { if (held) { ReleaseMutex(value); held = false; } }
        ~Unlock() { release(); }
    } unlock{mutex.value};
    ComScope com;
    if (!com.valid()) return failed("installation.shortcutFailed");
    const auto old = existing_process();
    if (owned_payload(root / L"current")) {
        const auto incoming_version = file_version(source), installed_version = file_version(installed);
        if (!incoming_version || !installed_version) return failed("installation.unrecognizedFiles");
        if (version_older(*incoming_version, *installed_version)) return failed("installation.newerInstalled");
    }
    auto run = read_run();
    UninstallRegistration registration{read_uninstall_key()};
    if (!owned_uninstall_key(registration.before, root)) return failed("installation.unrecognizedFiles");
    std::vector<fs::path> old_paths{installed};
    if (old.found) old_paths.push_back(old.path);
    if (!run.target.empty()) old_paths.push_back(run.target);
    std::vector<ShortcutChange> shortcuts;
    if (!collect_shortcuts(shortcuts, old_paths, installed)) return failed("installation.shortcutFailed");
    const auto token = random_token();
    if (token.empty()) return failed("installation.randomFailed");
    const auto event_name = L"Local\\SZUConnect.InstallReady.v1." + token;
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
    if (!ready.value || GetLastError() == ERROR_ALREADY_EXISTS) return failed("installation.readyFailed");
    DWORD child_pid = 0;
    bool old_stopped = false;
    bool restart_requested = false;
    const bool unchanged = same_payload(root / L"current", payload);
    TransactionCallbacks callbacks;
    callbacks.reuse_running_current = [&] { return unchanged && old.found && equal_path(old.path, installed); };
    callbacks.stop_old = [&] {
        if (!old.found || (unchanged && equal_path(old.path, installed))) return true;
        old_stopped = stop_process(old.pid, old.path);
        return old_stopped;
    };
    callbacks.apply_links = [&] {
        if (!apply_uninstall_registration(registration, root)) return false;
        if (!migrate_run(run, installed)) return false;
        for (auto& change : shortcuts) if (!write_shortcut(change, installed)) return false;
        return true;
    };
    callbacks.restore_links = [&] {
        bool restored = true;
        for (auto it = shortcuts.rbegin(); it != shortcuts.rend(); ++it) restored = restore_shortcut(*it) && restored;
        const bool run_restored = restore_run(run);
        return restore_uninstall_registration(registration) && run_restored && restored;
    };
    callbacks.stop_new = [&] { return !child_pid || stop_process(child_pid, installed); };
    callbacks.restart_old = [&](const fs::path&) {
        // An ordinary managed launch must not see our own transaction lock.
        // Defer the restart until the directory/Run/link restoration is complete.
        restart_requested = old_stopped;
    };
    callbacks.launch_ready = [&](const fs::path& path) {
        // For identical bytes, activate an existing managed process without relaunching it.
        const auto running = existing_process();
        if (running.found && equal_path(running.path, path)) {
            HWND window = FindWindowW(kWindowClass, nullptr);
            if (window) PostMessageW(window, RegisterWindowMessageW(L"SZUConnect.Activate.v1"), 0, 0);
            return true;
        }
        PROCESS_INFORMATION process{};
        const auto arguments = L"--installation-ready " + token + (options.background ? L" --background" : L"");
        if (!start_process(path, arguments, process)) return false;
        Handle thread(process.hThread), child(process.hProcess); child_pid = process.dwProcessId;
        const HANDLE handles[]{ready.value, child.value};
        return WaitForMultipleObjects(2, handles, FALSE, kWaitMs) == WAIT_OBJECT_0 && WaitForSingleObject(child.value, 0) == WAIT_TIMEOUT;
    };
    const auto result = transact(root, payload, callbacks);
    if (restart_requested) {
        unlock.release();
        PROCESS_INFORMATION process{};
        if (!start_process(old.path, options.background ? L"--background" : L"", process)) return failed("installation.restoreFailed");
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
    return result.ok ? InstallationResult{InstallationAction::handled, {}} : failed(result.key);
}

bool signal_installation_ready(const std::wstring& token) {
    if (!valid_token(token)) return false;
    const auto root = installation_root();
    if (root.empty() || !equal_path(executable_path(), root / L"current" / L"SZUConnect.exe") || !owned_payload(root / L"current")) return false;
    const auto event_name = L"Local\\SZUConnect.InstallReady.v1." + token;
    Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str()));
    return ready.value && SetEvent(ready.value);
}

InstallationResult open_uninstaller() {
    const auto root = installation_root(), installed = root / L"current" / L"SZUConnect.exe";
    if (root.empty() || !equal_path(executable_path(), installed) || !root_inventory(root) || !owned_payload(root / L"current"))
        return {InstallationAction::failed, "uninstall.notInstalled"};
    PROCESS_INFORMATION process{};
    if (!start_process(installed, L"--uninstall", process)) return {InstallationAction::failed, "uninstall.failed"};
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return {InstallationAction::handled, {}};
}

int run_uninstaller(HINSTANCE instance, const std::wstring& worker_token) {
    const auto root = installation_root(), installed = root / L"current" / L"SZUConnect.exe", self = executable_path();
    if (worker_token.empty()) {
        if (root.empty() || !equal_path(self, installed) || !root_inventory(root) || !owned_payload(root / L"current")) {
            uninstall_message("uninstall.notInstalled"); return 6;
        }
        ComScope com;
        if (!com.valid()) { uninstall_message("uninstall.failed"); return 6; }
        bool remove_data = false;
        if (!confirm_uninstall(instance, remove_data)) return 0;
        const auto token = random_token();
        if (token.empty() || !spawn_uninstall_worker(token, remove_data)) {
            auto message = localized("uninstall.failed");
            if (valid_token(token) && path_exists(temporary_parent() / (L"SZUConnect-Uninstall-" + token))) message += L"\n\n" + localized("uninstall.temporaryRemaining");
            const auto title = localized("uninstall.title");
            MessageBoxW(nullptr, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
            return 6;
        }
        return 0;
    }
    // Internal requests carry no filesystem target and cannot enter ordinary runtime startup.
    if (!valid_worker_path(worker_token, self, temporary_parent())) return 6;
    const auto map_name = L"Local\\SZUConnect.UninstallRequest.v1." + worker_token;
    Handle mapping(OpenFileMappingW(FILE_MAP_READ, FALSE, map_name.c_str()));
    if (!mapping.value) return 6;
    const auto* mapped = static_cast<const WorkerRequest*>(MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, sizeof(WorkerRequest)));
    if (!mapped) return 6;
    struct UnmapRequest { const void* value; ~UnmapRequest() { UnmapViewOfFile(value); } } unmap{mapped};
    const auto published_name = L"Local\\SZUConnect.UninstallPublished.v1." + worker_token;
    Handle published(OpenEventW(SYNCHRONIZE, FALSE, published_name.c_str()));
    if (!published.value || WaitForSingleObject(published.value, kWaitMs) != WAIT_OBJECT_0) return 6;
    const WorkerRequest request = *mapped;
    if (request.magic != 0x535a5531 || request.size != sizeof(WorkerRequest) || request.flags > 1 || actual_parent_id() != request.pid ||
        request.child_pid != GetCurrentProcessId() || !request.pid || !request.parent_handle || !request.cleanup_done_handle) return 6;
    Handle parent(reinterpret_cast<HANDLE>(request.parent_handle));
    Handle cleanup_done(reinterpret_cast<HANDLE>(request.cleanup_done_handle));
    if (GetProcessId(parent.value) != request.pid || !same_user(parent.value) || !equal_path(process_path(parent.value), installed) ||
        !szu_executable(installed) || !root_inventory(root) || !owned_payload(root / L"current") ||
        sha256(self).empty() || sha256(self) != sha256(installed)) return 6;
    const auto event_name = L"Local\\SZUConnect.UninstallReady.v1." + worker_token;
    Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str()));
    const auto decision_name = L"Local\\SZUConnect.UninstallDecision.v1." + worker_token;
    Handle decision(OpenEventW(SYNCHRONIZE, FALSE, decision_name.c_str()));
    CleanupSession cleanup;
    // A usable exit-time cleaner is required before the installed parent can
    // commit and before credentials, data or program files can be touched.
    if (!begin_worker_cleanup(cleanup, worker_token, temporary_parent(), cleanup_done.value)) return 6;
    // Hold both handoff objects before publishing ready; the parent may exit immediately.
    if (!ready.value || !decision.value || !SetEvent(ready.value)) { cleanup.finish("cancel"); return 6; }
    if (WaitForSingleObject(decision.value, kWaitMs) != WAIT_OBJECT_0 || !handoff_committed(mapped->decision)) {
        cleanup.finish("cancel"); return 6;
    }
    if (!cleanup.commit()) { uninstall_message("uninstall.failed"); return 6; }
    if (WaitForSingleObject(parent.value, kWaitMs) != WAIT_OBJECT_0) {
        cleanup.finish("error", localized("uninstall.exitRequired"));
        return 6;
    }
    if (!handoff_committed(mapped->decision)) { cleanup.finish("cancel"); return 6; }
    std::string result_key;
    bool success = false;
    {
        Handle mutex(CreateMutexW(nullptr, FALSE, L"Local\\SZUConnect.Installation.v1"));
        const auto lock_status = mutex.value ? WaitForSingleObject(mutex.value, kWaitMs) : WAIT_FAILED;
        if (lock_status != WAIT_OBJECT_0 && lock_status != WAIT_ABANDONED) result_key = "uninstall.busy";
        else {
            struct Unlock { HANDLE value; ~Unlock() { ReleaseMutex(value); } } unlock{mutex.value};
            ComScope com;
            const auto registration = read_uninstall_key();
            const auto run = read_run();
            if (!com.valid() || !uninstall_worker_preflight(root, registration, cleanup)) result_key = "uninstall.unsafe";
            else {
                const auto old = existing_process();
                UninstallCallbacks callbacks;
                callbacks.stop = [&] {
                    return !old.found || (equal_path(old.path, installed) && stop_process(old.pid, old.path));
                };
                callbacks.erase_credentials = [] {
                    return CredDeleteW(L"local.SZUConnect.credentials", CRED_TYPE_GENERIC, 0) != FALSE || GetLastError() == ERROR_NOT_FOUND;
                };
                callbacks.cleanup_links = [&] {
                    if (!remove_owned_run(run, installed) || !remove_owned_shortcuts(installed)) return false;
                    // A concurrent edit is retained; never broaden ownership to another registration.
                    const auto now = read_uninstall_key();
                    return now != registration || remove_registration_snapshot(registration);
                };
                const auto data = known_folder(FOLDERID_LocalAppData) / L"SZUConnect";
                const auto result = uninstall_transaction(root, data, request.flags == 1, callbacks);
                success = result.ok; result_key = result.key;
            }
        }
    }
    const auto message = success ? localized("uninstall.done") + L"\n\n" + localized(request.flags == 1 ? "uninstall.cleared" : "uninstall.kept") :
        localized(result_key.empty() ? "uninstall.failed" : result_key);
    if (!cleanup.finish(success ? "success" : "error", message)) {
        const auto failure = message + L"\n\n" + localized("uninstall.temporaryRemaining"), title = localized("uninstall.title");
        MessageBoxW(nullptr, failure.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
    }
    return success ? 0 : 6;
}

#ifdef SZU_INSTALLATION_TESTING
namespace installation_testing {
Result run_transaction(const Hooks& hooks) {
    Result evidence;
    TransactionCallbacks callbacks;
    callbacks.fail_at = static_cast<int>(hooks.fail_at);
    const bool unchanged = same_payload(hooks.root / L"current", {hooks.source_executable, hooks.documents});
    callbacks.reuse_running_current = [&] { return unchanged && hooks.reuse_running_current; };
    callbacks.stop_old = [&] { ++evidence.stop_calls; return hooks.stop_old_succeeds; };
    callbacks.apply_links = [&] { ++evidence.apply_calls; return hooks.apply_links_succeeds; };
    callbacks.restore_links = [&] { ++evidence.restore_calls; return hooks.restore_links_succeeds; };
    callbacks.restart_old = [&](const fs::path&) { ++evidence.restart_calls; };
    callbacks.launch_ready = [&](const fs::path&) {
        if (unchanged && hooks.reuse_running_current) { ++evidence.activation_calls; return true; }
        ++evidence.launch_calls; return hooks.launch_succeeds && hooks.ready_succeeds;
    };
    const auto result = transact(hooks.root, {hooks.source_executable, hooks.documents}, callbacks);
    evidence.success = result.ok; evidence.error_key = result.key;
    return evidence;
}
bool owned_payload(const fs::path& path) { return szu::app::owned_payload(path); }
std::string file_sha256(const fs::path& path) { return sha256(path); }
bool version_older(const Version& incoming, const Version& installed) { return szu::app::version_older(incoming, installed); }
UninstallResult run_uninstall_transaction(const UninstallHooks& hooks) {
    UninstallResult evidence;
    UninstallCallbacks callbacks;
    callbacks.stop = [&] { ++evidence.stop_calls; return hooks.stop_succeeds; };
    callbacks.erase_credentials = [&] { ++evidence.erase_calls; return hooks.erase_succeeds; };
    callbacks.cleanup_links = [&] { ++evidence.cleanup_calls; return hooks.cleanup_succeeds; };
    callbacks.fail_after_first_payload_file = hooks.fail_after_first_payload_file;
    const auto result = uninstall_transaction(hooks.root, hooks.data_root, hooks.remove_data, callbacks);
    evidence.success = result.ok; evidence.error_key = result.key;
    return evidence;
}
bool data_filename_owned(const std::wstring& name) { return owned_data_name(name); }
bool worker_request_valid(const std::wstring& token, const fs::path& helper, const fs::path& parent) { return valid_worker_path(token, helper, parent); }
bool registration_failure_recoverable(const fs::path& root, int failure_at, bool restoring) {
    RegistrySnapshot before{true, true, {}};
    before.values[L"DisplayName"] = registry_string(L"深大随连");
    before.values[L"DisplayVersion"] = registry_string(L"0.10.1");
    before.values[L"SZUConnectOwner"] = registry_string(L"SZUConnect.ManagedUninstall.v1|" + root.native());
    before.values[L"UninstallString"] = registry_string(uninstall_command(root));
    RegistryTestState fake{before, failure_at};
    struct Reset { ~Reset() { registry_test_state = nullptr; } } reset;
    registry_test_state = &fake;
    if (!restoring) {
        const bool removed = remove_registration_snapshot(before);
        if (failure_at == -1) {
            auto expected = before; expected.values[L"ConcurrentUserValue"] = registry_string(L"preserve");
            return !removed && fake.snapshot == expected;
        }
        return !removed && fake.snapshot == before && owned_uninstall_key(fake.snapshot, root);
    }
    auto after = before; after.values[L"DisplayVersion"] = registry_string(L"1.0.0"); after.values[L"NoModify"] = registry_dword(1);
    fake.snapshot = after;
    UninstallRegistration change{before, after, true};
    const bool restored = restore_uninstall_registration(change);
    return !restored && owned_uninstall_key(fake.snapshot, root) && fake.snapshot.values[L"UninstallString"] == before.values[L"UninstallString"];
}
bool handoff_failure_is_cancelled(bool ready, bool signal_succeeds) {
    WorkerRequest request;
    Handle decision(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!decision.value) return false;
    const bool result = complete_handoff(request, ready, [&] { return signal_succeeds && SetEvent(decision.value); });
    // A late ready signal cannot change the committed decision after failure.
    Handle late_ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!late_ready.value || !SetEvent(late_ready.value)) return false;
    return !result && request.decision == -1 && !handoff_committed(request.decision) &&
        (signal_succeeds ? WaitForSingleObject(decision.value, 0) == WAIT_OBJECT_0 : WaitForSingleObject(decision.value, 0) == WAIT_TIMEOUT);
}
std::string cleanup_readiness_name(int value) {
    switch (static_cast<CleanupReadiness>(value)) {
        case CleanupReadiness::notStarted: return "preflight";
        case CleanupReadiness::ready: return "ready";
        case CleanupReadiness::unavailable: return "unavailable";
        case CleanupReadiness::timeout: return "timeout";
        case CleanupReadiness::exited: return "process-exited";
        case CleanupReadiness::closed: return "pipe-closed";
        case CleanupReadiness::invalidOutput: return "invalid-output";
    }
    return "fixture";
}
std::string cleanup_readiness_probe(HANDLE output, HANDLE process, DWORD timeout_ms) {
    return cleanup_readiness_name(static_cast<int>(wait_cleanup_readiness(output, process, timeout_ms)));
}
std::string cleanup_readiness_exit_race_probe(HANDLE output, HANDLE process, HANDLE writer, const std::string& final_output) {
    bool injected = false, wrote = false;
    const auto result = wait_cleanup_readiness(output, process, 1000, [&](HANDLE value, DWORD wait_ms) {
        if (!injected) {
            injected = true;
            DWORD count = 0;
            wrote = WriteFile(writer, final_output.data(), static_cast<DWORD>(final_output.size()), &count, nullptr) &&
                count == final_output.size() && SetEvent(value);
        }
        return WaitForSingleObject(value, wait_ms);
    });
    return injected && wrote ? cleanup_readiness_name(static_cast<int>(result)) : "fixture-write-failed";
}
std::string cleanup_fixture_exit_stage(DWORD exit_code) {
    return exit_code == 6 ? "before-ready" : exit_code >= 64 && exit_code <= 70
        ? cleanup_readiness_name(static_cast<int>(exit_code - 64)) : "fixture";
}
int run_cleanup_image_fixture(const std::string& mode, const std::wstring& token,
                              std::uintptr_t done_handle, std::uintptr_t passed_handle) {
    Handle done(reinterpret_cast<HANDLE>(done_handle)), passed(reinterpret_cast<HANDLE>(passed_handle));
    const auto self = executable_path(), parent = self.parent_path().parent_path(), base = parent.parent_path();
    std::error_code ec;
    const auto temp = fs::weakly_canonical(fs::temp_directory_path(ec), ec);
    const std::set<std::string> modes{"success", "stderr-prefix", "stderr-unavailable", "cancel", "disconnect", "malformed-phase", "lost-result", "late-unknown", "unknown", "locked", "unavailable", "junction",
        "guard-valid", "guard-foreign", "guard-forged", "guard-owner"};
    if (ec || !done.value || !passed.value || !modes.contains(mode) || !valid_token(token) ||
        parent.filename() != L"清理 $path ' & 空格" || !base.filename().native().starts_with(L"szu-installation-test-") ||
        !equal_path(base.parent_path(), temp) || !valid_worker_path(token, self, parent)) {
        cleanup_stage_failure(CleanupFailureStage::requestValidation, ec ? static_cast<DWORD>(ec.value()) : GetLastError());
        if (done.value) SetEvent(done.value);
        return 6;
    }
    const auto guard_root = base / L"guard-install";
    RegistrySnapshot registration{true, true, {}};
    const bool guard_mode = mode.starts_with("guard-");
    if (guard_mode) {
        Payload payload{self, {}};
        for (const auto* name : kDocumentNames) payload.documents.emplace_back(name, "Isolated guard license\n");
        TransactionCallbacks callbacks;
        const auto installed = transact(guard_root, payload, callbacks);
        const DWORD install_error = installed.ok ? ERROR_SUCCESS : GetLastError();
        if (!installed.ok) {
            cleanup_stage_failure(CleanupFailureStage::guardTransaction, install_error);
            std::printf("Cleanup fixture transaction: result=%s\n", installed.key.c_str());
            std::fflush(stdout);
            SetEvent(done.value); return 6;
        }
        registration.values[L"SZUConnectOwner"] = registry_string(L"SZUConnect.ManagedUninstall.v1|" + guard_root.native());
        registration.values[L"UninstallString"] = registry_string(uninstall_command(guard_root));
    }
    cleaner_unavailable_for_test = mode == "unavailable";
    cleaner_stderr_prefix_for_test = mode == "stderr-prefix";
    cleaner_stdout_reject_for_test = mode == "stderr-unavailable";
    CleanupSession cleanup;
    if (!begin_worker_cleanup(cleanup, token, parent, done.value, passed.value, false)) {
        SetEvent(done.value);
        return cleanup.readiness == CleanupReadiness::notStarted ? 6 : 64 + static_cast<int>(cleanup.readiness);
    }
    if (guard_mode) {
        if (mode == "guard-foreign" && !write_new(guard_root / L"keep.txt", "unrelated guard data\n")) return 7;
        if (mode == "guard-forged") {
            const auto installed_image = guard_root / L"current" / L"SZUConnect.exe";
            const auto original_digest = sha256(installed_image);
            if (original_digest.empty() || original_digest != cleanup.image_digest) {
                cleanup_stage_failure(CleanupFailureStage::forgedImageDigest, GetLastError()); return 7;
            }
            std::ofstream changed(installed_image, std::ios::binary | std::ios::app);
            changed << "forged fixture payload\n";
            changed.flush();
            const DWORD append_error = GetLastError();
            const bool appended = changed.good();
            changed.close();
            const DWORD append_close_error = GetLastError();
            if (!appended || changed.fail()) {
                cleanup_stage_failure(CleanupFailureStage::forgedImageWrite, appended ? append_close_error : append_error); return 7;
            }
            const auto forged_digest = sha256(installed_image);
            if (forged_digest.empty() || forged_digest == original_digest) {
                cleanup_stage_failure(CleanupFailureStage::forgedImageDigest, GetLastError()); return 7;
            }
            const auto manifest = manifest_for(guard_root / L"current");
            if (manifest.empty()) { cleanup_stage_failure(CleanupFailureStage::forgedManifest, GetLastError()); return 7; }
            std::ofstream updated(guard_root / L"current" / kManifestName, std::ios::binary | std::ios::trunc); updated << manifest;
            updated.flush();
            const DWORD manifest_error = GetLastError();
            const bool manifest_written = updated.good();
            updated.close();
            const DWORD manifest_close_error = GetLastError();
            if (!manifest_written || updated.fail()) {
                cleanup_stage_failure(CleanupFailureStage::forgedManifest, manifest_written ? manifest_close_error : manifest_error); return 7;
            }
            if (!root_inventory(guard_root)) { cleanup_stage_failure(CleanupFailureStage::forgedInventory, GetLastError()); return 7; }
        }
        if (mode == "guard-owner") registration.values[L"SZUConnectOwner"] = registry_string(L"foreign fixture owner");
        const bool accepted = uninstall_worker_preflight(guard_root, registration, cleanup);
        const bool expected = mode == "guard-valid";
        if (mode == "guard-foreign") {
            bool ok = false;
            if (read_bytes(guard_root / L"keep.txt", 128, ok) != "unrelated guard data\n" || !ok) return 7;
        }
        if (!cleanup.commit() || !cleanup.finish(accepted == expected ? "success" : "error")) return 7;
        return accepted == expected ? 0 : 7;
    }
    if (mode == "disconnect") return 0;
    if (mode == "malformed-phase") {
        DWORD count = 0;
        return WriteFile(cleanup.input.value, "{\n", 2, &count, nullptr) && count == 2 ? 0 : 6;
    }
    if (mode == "cancel") return cleanup.finish("cancel") ? 0 : 6;
    if (!cleanup.commit()) return 6;
    if (mode == "lost-result") return 0;
    if (mode == "late-unknown" && !write_new(self.parent_path() / L"keep.txt", "unrelated fixture data\n")) return 6;
    return cleanup.finish("success", L"Isolated cleanup result") ? 0 : 6;
}
}
#endif
} // namespace szu::app
