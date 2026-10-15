#include "szu/win/system.hpp"

#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincred.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

#include "szu/portal_wire.hpp"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace szu::win {
namespace {
constexpr wchar_t kCredentialTarget[] = L"local.SZUConnect.credentials";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kRunValue[] = L"SZUConnect";

DWORD read_dword(const wchar_t* key, const wchar_t* value, DWORD fallback) {
    DWORD data = 0, size = sizeof data;
    if (RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size) == ERROR_SUCCESS) return data;
    return fallback;
}
}  // namespace

// ---------------- Credential Manager ----------------
CredentialVault::ReadStatus CredentialVault::read(Credentials& out) {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(kCredentialTarget, CRED_TYPE_GENERIC, 0, &credential)) {
        return GetLastError() == ERROR_NOT_FOUND ? ReadStatus::missing : ReadStatus::failed;
    }
    out.account = narrow(credential->UserName ? credential->UserName : L"");
    out.password.assign(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
    SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);
    return out.complete() ? ReadStatus::found : ReadStatus::failed;
}

bool CredentialVault::write(const Credentials& credentials) {
    if (!credentials.complete()) return false;
    std::wstring user = widen(credentials.account);
    std::string blob = credentials.password;
    std::wstring comment = L"深大随连：校园网账户，仅保存在本机";
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(kCredentialTarget);
    credential.Comment = comment.data();
    credential.UserName = user.data();
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
    credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;  // this user, this machine, never roamed
    const BOOL ok = CredWriteW(&credential, 0);        // replaces atomically; the old value survives failure
    wipe(blob);
    return ok != FALSE;
}

bool CredentialVault::remove() {
    return CredDeleteW(kCredentialTarget, CRED_TYPE_GENERIC, 0) != FALSE || GetLastError() == ERROR_NOT_FOUND;
}

// ---------------- launch at login ----------------
std::wstring executable_path() {
    std::vector<wchar_t> buffer(MAX_PATH);
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) return std::wstring(buffer.data(), length);
        buffer.resize(buffer.size() * 2);
    }
}

bool Autostart::enabled() {
    wchar_t value[2048] = {};
    DWORD size = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return false;
    const std::wstring command(value);
    const std::wstring exe = executable_path();
    return !exe.empty() && command.find(exe) != std::wstring::npos;
}

bool Autostart::approved() {
    BYTE data[12] = {};
    DWORD size = sizeof data;
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kRunValue, RRF_RT_REG_BINARY, nullptr, data, &size) != ERROR_SUCCESS) return true;
    return size == 0 || (data[0] & 0x01) == 0;  // 02/06 enabled, 03/07 disabled by the user
}

bool Autostart::set(bool enable) {
    if (!enable) {
        const LSTATUS rc = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
    }
    const std::wstring exe = executable_path();
    if (exe.empty()) return false;
    const std::wstring command = L"\"" + exe + L"\" --background";
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, command.c_str(),
                           static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

// ---------------- single instance ----------------
SingleInstance::SingleInstance(const wchar_t* key) {
    mutex_ = CreateMutexW(nullptr, FALSE, key);
    primary_ = mutex_ != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
}

SingleInstance::~SingleInstance() {
    if (mutex_) CloseHandle(mutex_);
}

UINT SingleInstance::activation_message() {
    static const UINT message = RegisterWindowMessageW(L"SZUConnect.Activate.v1");
    return message;
}

void SingleInstance::activate_existing(const wchar_t* window_class) {
    HWND existing = FindWindowW(window_class, nullptr);
    if (!existing) return;
    DWORD process = 0;
    GetWindowThreadProcessId(existing, &process);
    if (process) AllowSetForegroundWindow(process);
    PostMessageW(existing, activation_message(), 0, 0);
}

// ---------------- storage ----------------
Storage::Storage(Access access) : access_(access) {
    if (access_ == Access::disabled) return;
    PWSTR base = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, access_ == Access::writable ? KF_FLAG_CREATE : 0, nullptr, &base)) && base) {
        std::wstring folder = std::wstring(base) + L"\\SZUConnect";
        CoTaskMemFree(base);
        if (access_ == Access::readOnly || CreateDirectoryW(folder.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) folder_ = folder;
    }
}

Storage::ReadStatus Storage::read(const wchar_t* name, std::string& out) const {
    if (folder_.empty()) return ReadStatus::failed;
    const std::wstring path = folder_ + L"\\" + name;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? ReadStatus::missing : ReadStatus::failed;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > 8 * 1024 * 1024) {
        CloseHandle(file);
        return ReadStatus::failed;
    }
    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = out.empty() || ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read != out.size()) return ReadStatus::failed;
    return ReadStatus::ok;
}

bool Storage::write(const wchar_t* name, const std::string& data) const {
    if (access_ != Access::writable || folder_.empty()) return false;
    const std::wstring target = folder_ + L"\\" + name;
    const std::wstring temp = target + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = data.empty() || WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    ok = ok && written == data.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(temp.c_str());
    return ok != FALSE;
}

bool Storage::write_preserving_existing(const wchar_t* name, const std::string& data, bool preserve_existing) const {
    if (access_ != Access::writable) return false;
    if (!preserve_existing) return write(name, data);
    std::string original;
    const auto status = read(name, original);
    if (status == ReadStatus::failed) return false;  // unreadable/oversize original must not be overwritten
    if (status == ReadStatus::ok) {
        static std::atomic<unsigned long long> sequence{0};
        const std::wstring backup = std::wstring(name) + L".corrupt-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence);
        if (!write(backup.c_str(), original)) return false;
    }
    return write(name, data);  // write uses temp + atomic rename; original survives failure
}

bool Storage::quarantine(const wchar_t* name) const {
    if (access_ != Access::writable || folder_.empty()) return false;
    SYSTEMTIME t;
    GetSystemTime(&t);
    wchar_t stamp[32];
    std::swprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::wstring source = folder_ + L"\\" + name;
    const std::wstring target = source + L".corrupt-" + stamp;
    return MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
}

// ---------------- appearance ----------------
SystemAppearance read_appearance() {
    SystemAppearance a;
    const wchar_t* personalize = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    a.apps_dark = read_dword(personalize, L"AppsUseLightTheme", 1) == 0;
    a.taskbar_dark = read_dword(personalize, L"SystemUsesLightTheme", 0) == 0;
    HIGHCONTRASTW contrast{sizeof contrast};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0)) a.high_contrast = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    BOOL animation = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0)) a.reduce_motion = animation == FALSE;
    const DWORD scale = read_dword(L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", 100);
    a.text_scale = static_cast<float>(scale < 100 ? 100 : (scale > 225 ? 225 : scale)) / 100.0f;
    return a;
}

bool copy_to_clipboard(HWND owner, const std::string& utf8) {
    const std::wstring text = widen(utf8);
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (memory) {
        if (void* target = GlobalLock(memory)) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!ok) GlobalFree(memory);
    }
    CloseClipboard();
    return ok;
}

bool open_official_link(const std::string& url) {
    const auto parsed = wire::parse_url(url);
    if (!parsed || parsed->scheme != "https" || parsed->has_userinfo) return false;
    const std::string& host = parsed->host;
    const bool official = host == "szu.edu.cn" || (host.size() > 11 && host.compare(host.size() - 11, 11, ".szu.edu.cn") == 0) ||
                          host == "github.com";
    if (!official) return false;
    const std::wstring wide = widen(url);
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}


std::string random_hex(std::size_t bytes) {
    std::vector<unsigned char> buffer(bytes);
    if (bytes == 0 || BCryptGenRandom(nullptr, buffer.data(), static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char b : buffer) { out.push_back(digits[b >> 4]); out.push_back(digits[b & 15]); }
    return out;
}

}  // namespace szu::win
