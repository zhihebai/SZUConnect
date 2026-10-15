// Entry point. Offline smoke dispatch precedes every formal runtime service.
#include "main_window.hpp"
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <cstdio>
#include "smoke_options.hpp"
#include "szu/win/system.hpp"
#include "installation.hpp"
#include "shared_copy.hpp"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0; std::vector<std::wstring> args;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    LocalFree(argv);
    const auto options = szu::app::parse_launch_options(args);
    if (!options.error.empty()) return 2;
    const auto installation = szu::app::prepare_installation(instance,
        {options.observe_only, options.offline_smoke, options.portable, options.background, options.ready_token});
    if (installation.action == szu::app::InstallationAction::handled) return 0;
    if (installation.action == szu::app::InstallationAction::failed) {
        const auto body = szu::win::widen(szu::copy::text(installation.error_key));
        const auto title = szu::win::widen(szu::copy::text("app.name"));
        MessageBoxW(nullptr, body.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
        return 5;
    }
    std::unique_ptr<szu::app::SmokeReceipt> receipt;
    if (options.offline_smoke) {
        receipt = std::make_unique<szu::app::SmokeReceipt>(options.smoke_output);
        if (!receipt->valid()) { std::fprintf(stderr, "offline-smoke: %s\n", receipt->error().c_str()); return 3; }
    }
    const std::wstring key = options.offline_smoke ? L"Local\\SZUConnect.OfflineSmoke.Instance.v1." + std::to_wstring(GetCurrentProcessId()) :
        options.observe_only ? L"Local\\SZUConnect.Observe.Instance.v1" : L"Local\\SZUConnect.Instance.v1";
    szu::win::SingleInstance guard(key.c_str());
    if (!guard.valid()) {
        if (receipt) { szu::app::SmokeResult r; r.failure = "instanceCreationFailed"; receipt->finish(r, nullptr); }
        return 4;
    }
    if (!guard.primary()) {
        if (receipt) { szu::app::SmokeResult r; r.failure = "instanceFailed"; receipt->finish(r, nullptr); return 4; }
        szu::win::SingleInstance::activate_existing(options.observe_only ? L"SZUConnect.Observe.MainWindow.v1" : szu::app::kWindowClass);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        if (receipt) { szu::app::SmokeResult r; r.failure = "comInitializationFailed"; receipt->finish(r, nullptr); }
        return 1;
    }
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    int code = 1;
    {
        szu::app::MainWindow window(instance, options.observe_only, options.background, options.offline_smoke, options.smoke_timeout_ms);
        if (window.create()) {
            if (!options.background) window.show();
            if (!options.ready_token.empty() && !szu::app::signal_installation_ready(options.ready_token))
                PostMessageW(window.host_window(), WM_COMMAND, szu::app::kCmdQuit, 0);
            code = window.run();
        }
        if (receipt) {
            auto result = window.smoke_result();
            if (code != 0 && result.failure.empty()) result.failure = "windowStartupFailed";
            if (!receipt->finish(result, window.runtime_evidence()) || !result.failure.empty() || !result.fake_auth_passed ||
                result.native_painted_pages.size() != 3 || !result.clean_shutdown) code = 1;
        }
    }
    CoUninitialize();
    return code;
}
