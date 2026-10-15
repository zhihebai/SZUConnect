// Notification-area icon (Shell_NotifyIcon v4) with light/dark taskbar variants and a context menu.
#pragma once
#include "szu/win/text.hpp"
#include <shellapi.h>

#include <string>

#include "szu/engine.hpp"

namespace szu::app {

enum TrayCommand : UINT { kCmdOpen = 40001, kCmdCheck, kCmdLogin, kCmdPause, kCmdResume, kCmdQuit };

class Tray {
public:
    void add(HWND hwnd, HINSTANCE instance);
    void update(const std::string& state, bool taskbar_dark, const std::wstring& tooltip);  // idle|online|attention|paused|busy
    void remove();
    void show_menu(HWND hwnd, const EngineSnapshot& snapshot, int x, int y);
    static std::string state_for(const EngineSnapshot& snapshot);

private:
    HICON load(const std::string& state, bool dark) const;
    NOTIFYICONDATAW data_{};
    HINSTANCE instance_ = nullptr;
    bool added_ = false;
    std::string state_ = "idle";
    bool dark_ = true;
    std::wstring tooltip_;
};

}  // namespace szu::app
