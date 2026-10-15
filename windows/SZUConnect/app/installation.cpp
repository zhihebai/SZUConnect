#include "installation.hpp"

#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")

namespace szu::app {
namespace {
namespace fs = std::filesystem;
constexpr wchar_t kWindowClass[] = L"SZUConnect.MainWindow.v1";
constexpr UINT kQuit = 40006; // Same existing tray command, including portable versions.
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunName[] = L"SZUConnect";
constexpr char kRootIdentity[] = "SZUConnect.ManagedInstallation.v1\n";
constexpr char kManifestIdentity[] = "SZUConnect.ManagedPayload.v1\n";
constexpr wchar_t kManifestName[] = L"INSTALLATION-MANIFEST.txt";
constexpr DWORD kWaitMs = 30000;
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
std::vector<fs::path> payload_files() {
    std::vector<fs::path> names{L"SZUConnect.exe"};
    for (const auto* name : kDocumentNames) names.push_back(fs::path(L"说明与许可") / name);
    return names;
}
std::string manifest_for(const fs::path& folder) {
    std::string manifest = kManifestIdentity;
    // Fixed positions avoid accepting filenames from an untrusted on-disk manifest.
    for (const auto& name : payload_files()) {
        const auto digest = sha256(folder / name);
        if (digest.empty()) return {};
        manifest += digest + "\n";
    }
    return manifest;
}
bool payload_inventory(const fs::path& folder, bool require_manifest = true) {
    if (!safe_ancestors(folder) || !plain_directory(folder)) return false;
    const auto expected_files = payload_files();
    std::set<fs::path> expected(expected_files.begin(), expected_files.end());
    if (require_manifest) expected.emplace(kManifestName);
    std::set<fs::path> actual;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec)) {
        const auto attrs = GetFileAttributesW(it->path().c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto relative = it->path().lexically_relative(folder);
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            if (relative != fs::path(L"说明与许可")) return false;
        } else actual.emplace(relative);
    }
    return !ec && actual == expected;
}
bool owned_payload(const fs::path& folder) {
    if (!payload_inventory(folder)) return false;
    bool ok = false;
    const auto recorded = read_bytes(folder / kManifestName, 1024, ok);
    if (!ok) return false;
    const auto actual = manifest_for(folder);
    return !actual.empty() && recorded == actual;
}
bool remove_owned_payload(const fs::path& folder) {
    if (!owned_payload(folder)) return false;
    // No recursive removal: each expected ordinary file is checked again.
    auto names = payload_files(); names.emplace_back(kManifestName);
    for (const auto& name : names) {
        const auto path = folder / name;
        if (!regular(path) || !DeleteFileW(path.c_str())) return false;
    }
    return RemoveDirectoryW((folder / L"说明与许可").c_str()) && RemoveDirectoryW(folder.c_str());
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
    if (!owned_payload(installed) || sha256(installed / L"SZUConnect.exe") != sha256(payload.source)) return false;
    for (const auto& [name, bytes] : payload.documents) {
        bool ok = false;
        if (read_bytes(installed / L"说明与许可" / name, kDocumentLimit, ok) != bytes || !ok) return false;
    }
    return true;
}
bool create_stage(const fs::path& stage, const Payload& payload) {
    if (!CreateDirectoryW(stage.c_str(), nullptr) || !CreateDirectoryW((stage / L"说明与许可").c_str(), nullptr)) return false;
    if (!CopyFileW(payload.source.c_str(), (stage / L"SZUConnect.exe").c_str(), TRUE)) return false;
    for (const auto& [name, bytes] : payload.documents)
        if (!write_new(stage / L"说明与许可" / name, bytes)) return false;
    const auto manifest = manifest_for(stage);
    return !manifest.empty() && write_new(stage / kManifestName, manifest) && owned_payload(stage);
}
void cleanup_created_stage(const fs::path& stage) {
    // Only names created by this transaction, never remove_all or an arbitrary parent.
    if (!plain_directory(stage) || !safe_ancestors(stage)) return;
    auto names = payload_files(); names.emplace_back(kManifestName);
    for (const auto& name : names) if (regular(stage / name)) DeleteFileW((stage / name).c_str());
    RemoveDirectoryW((stage / L"说明与许可").c_str());
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
            ReleaseMutex(gate.value);
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
        if (!migrate_run(run, installed)) return false;
        for (auto& change : shortcuts) if (!write_shortcut(change, installed)) return false;
        return true;
    };
    callbacks.restore_links = [&] {
        bool restored = true;
        for (auto it = shortcuts.rbegin(); it != shortcuts.rend(); ++it) restored = restore_shortcut(*it) && restored;
        return restore_run(run) && restored;
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
}
#endif
} // namespace szu::app
