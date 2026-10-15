#include "tray.hpp"

#include <commctrl.h>

#include "../resources/resource.h"
#include "controller.hpp"
#include "shared_copy.hpp"

#pragma comment(lib, "comctl32.lib")

namespace szu::app {

namespace {
int state_index(const std::string& state) {
    if (state == "online") return 1;
    if (state == "attention") return 2;
    if (state == "paused") return 3;
    if (state == "busy") return 4;
    return 0;
}
std::wstring text(std::string_view key) { return win::widen(copy::text(key)); }
}  // namespace

std::string Tray::state_for(const EngineSnapshot& s) {
    if (s.busy) return "busy";
    if (s.paused && s.mode == MonitoringMode::recover) return "paused";
    if (s.online) return "online";
    switch (s.status) {
        case EngineStatus::idle:
        case EngineStatus::cancelled: return "idle";
        default: return s.last_report || s.last_attempt ? "attention" : "idle";
    }
}

HICON Tray::load(const std::string& state, bool dark) const {
    HICON icon = nullptr;
    const int id = IDI_TRAY_BASE + state_index(state) * 2 + (dark ? 1 : 0);
    if (FAILED(LoadIconMetric(instance_, MAKEINTRESOURCEW(id), LIM_SMALL, &icon))) icon = nullptr;
    return icon;
}

void Tray::add(HWND hwnd, HINSTANCE instance) {
    instance_ = instance;
    if (added_) remove();
    data_ = NOTIFYICONDATAW{};
    data_.cbSize = sizeof data_;
    data_.hWnd = hwnd;
    data_.uID = 1;
    data_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data_.uCallbackMessage = kMsgTray;
    data_.hIcon = load(state_, dark_);
    wcsncpy_s(data_.szTip, tooltip_.empty() ? text("app.name").c_str() : tooltip_.c_str(), _TRUNCATE);
    added_ = Shell_NotifyIconW(NIM_ADD, &data_) != FALSE;
    if (added_) {
        data_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data_);
    }
}

void Tray::update(const std::string& state, bool taskbar_dark, const std::wstring& tooltip) {
    if (!added_) return;
    if (state == state_ && taskbar_dark == dark_ && tooltip == tooltip_) return;
    state_ = state;
    dark_ = taskbar_dark;
    tooltip_ = tooltip;
    HICON previous = data_.hIcon;
    data_.hIcon = load(state_, dark_);
    data_.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(data_.szTip, tooltip_.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data_);
    if (previous) DestroyIcon(previous);
}

void Tray::remove() {
    if (!added_) return;
    Shell_NotifyIconW(NIM_DELETE, &data_);
    if (data_.hIcon) DestroyIcon(data_.hIcon);
    data_.hIcon = nullptr;
    added_ = false;
}

void Tray::show_menu(HWND hwnd, const EngineSnapshot& s, int x, int y) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kCmdOpen, text("tray.open").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (s.busy ? MF_GRAYED : 0), kCmdCheck, text("tray.check").c_str());
    AppendMenuW(menu, MF_STRING | (s.busy || s.read_only || !(s.has_saved || s.draft_complete) ? MF_GRAYED : 0), kCmdLogin,
                text("tray.login").c_str());
    if (s.mode == MonitoringMode::recover) {
        if (s.paused) AppendMenuW(menu, MF_STRING, kCmdResume, text("tray.resume").c_str());
        else AppendMenuW(menu, MF_STRING, kCmdPause, text("tray.pause").c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdQuit, text("tray.quit").c_str());
    SetMenuDefaultItem(menu, kCmdOpen, FALSE);
    SetForegroundWindow(hwnd);  // required so the menu closes when the user clicks elsewhere
    const UINT flags = TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN) | TPM_BOTTOMALIGN;
    TrackPopupMenuEx(menu, flags, x, y, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

}  // namespace szu::app
