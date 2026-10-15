#pragma once
#define IDI_APP 1
// Tray icons: 100 + state * 2 + (dark taskbar ? 1 : 0); states: idle, online, attention, paused, busy
#define IDI_TRAY_BASE 100
#define IDI_TRAY_IDLE_LIGHT 100
#define IDI_TRAY_IDLE_DARK 101
#define IDI_TRAY_ONLINE_LIGHT 102
#define IDI_TRAY_ONLINE_DARK 103
#define IDI_TRAY_ATTENTION_LIGHT 104
#define IDI_TRAY_ATTENTION_DARK 105
#define IDI_TRAY_PAUSED_LIGHT 106
#define IDI_TRAY_PAUSED_DARK 107
#define IDI_TRAY_BUSY_LIGHT 108
#define IDI_TRAY_BUSY_DARK 109
// Static documents travel with the executable for single-file installation.
#define IDR_INSTALL_README 401
#define IDR_INSTALL_LICENSE 402
#define IDR_INSTALL_NOTICES 403
#define IDR_INSTALL_CURL_LICENSE 404
