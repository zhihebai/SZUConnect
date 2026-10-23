// Windows integration: Credential Manager, launch at login (HKCU Run), single instance,
// LocalAppData storage with atomic writes and quarantine, theme/accessibility settings,
// clipboard and opening official links.
#pragma once
#include <optional>
#include <string>

#include "szu/types.hpp"
#include "szu/win/text.hpp"

namespace szu::win {

// Generic credential "local.SZUConnect.credentials", persisted for this user on this machine only.
class CredentialVault {
public:
    enum class ReadStatus { found, missing, failed };
    static ReadStatus read(Credentials& out);
    static bool write(const Credentials& credentials);
    static bool remove();
};

class Autostart {
public:
    static bool enabled();         // HKCU Run value points at this executable
    static bool approved();        // not disabled in Settings > Apps > Startup / Task Manager
    static bool set(bool enable);  // never called on first run; only from the user's toggle
};

class SingleInstance {
public:
    explicit SingleInstance(const wchar_t* key = L"Local\\SZUConnect.Instance.v1");
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    bool primary() const { return primary_; }
    bool valid() const { return mutex_ != nullptr; }
    static void activate_existing(const wchar_t* window_class);
    static UINT activation_message();

private:
    HANDLE mutex_ = nullptr;
    bool primary_ = false;
};

class Storage {
public:
    enum class Access { writable, readOnly, disabled };
    explicit Storage(Access access = Access::writable);
    bool available() const { return !folder_.empty(); }
    const std::wstring& folder() const { return folder_; }
    enum class ReadStatus { ok, missing, failed };
    ReadStatus read(const wchar_t* name, std::string& out) const;
    bool write(const wchar_t* name, const std::string& data) const;
    bool quarantine(const wchar_t* name) const;
    // Explicit repair only: preserve a copy first, then atomically replace. Any failure leaves
    // the original file in place, unlike startup quarantine used by legacy log recovery.
    bool write_preserving_existing(const wchar_t* name, const std::string& data, bool preserve_existing) const;

private:
    std::wstring folder_;
    Access access_;
};

struct SystemAppearance {
    bool apps_dark = false;
    bool taskbar_dark = true;
    bool high_contrast = false;
    bool reduce_motion = false;
    float text_scale = 1.0f;  // Settings > Accessibility > Text size (1.0 – 2.25)
};
SystemAppearance read_appearance();

// 2*bytes lower-case hex characters from the OS random generator; empty on failure.
std::string random_hex(std::size_t bytes);

bool copy_to_clipboard(HWND owner, const std::string& utf8);
bool open_official_link(const std::string& url);  // https://*.szu.edu.cn/ allow-list only
std::wstring executable_path();

}  // namespace szu::win
