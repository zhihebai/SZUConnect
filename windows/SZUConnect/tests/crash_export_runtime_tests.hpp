#pragma once
#include "wifi_runtime_tests.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace crashexportfixture {
class Fake final : public wifiruntime::Fake {
public:
    Fake() {
        files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
    }
    szu::app::RuntimeMode mode() const override { return szu::app::RuntimeMode::normal; }
    szu::crash::Report crash_diagnostics(const szu::CancelToken& token) override {
        ++calls; started = true;
        while (blocked && !token.cancelled()) token.wait_for(0.001);
        finished = true;
        if (token.cancelled()) return {szu::crash::Status::cancelled, 0, {}};
        const double now = szu::app::AppController::wall_now();
        return {szu::crash::Status::found, now, {{now - 60, "1.0.0.34", 0xc0000005, 0x1234}}};
    }
    std::atomic<bool> blocked{false}, started{false}, finished{false};
    std::atomic<unsigned> calls{0};
};
inline std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
inline void wait(szu::app::AppController& app) {
    wifiruntime::wait_for([&] { app.on_changed(); return !app.diagnostics_export_running(); });
}
}

template<class Check> void run_crash_export_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    namespace fs = std::filesystem;
    std::error_code error;
    const auto directory = fs::temp_directory_path(error) / (L"SZUConnect-crash-export-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    check(!error && fs::create_directory(directory, error), "crash export tests create only an isolated temporary directory");
    if (error) return;
    const auto file = directory / L"report.json";
    const auto path = file.wstring();
    {
        auto services = std::make_unique<crashexportfixture::Fake>(); auto* fake = services.get(); fake->blocked = true; fake->physical_path = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services), {}, {},
            [&](HWND) -> std::optional<std::wstring> { return path; });
        app.start();
        const auto initial = json::parse(app.diagnostic_export_text());
        const auto initial_samples = json::serialize(*initial->find("samples"));
        check(app.export_diagnostics(nullptr) && app.diagnostics_export_running() &&
            app.diagnostics_export_feedback() == "records.exporting", "file selection starts a pending export instead of reporting immediate success");
        wifiruntime::wait_for([&] { return fake->started.load(); });
        check(!app.export_diagnostics(nullptr) && fake->calls == 1, "repeated export clicks cannot queue duplicate OS scans");
        app.set_probe(ProbeMode::complete); app.check();
        wifiruntime::wait_for([&] { app.on_changed(); return fake->evidence().fake_probe_calls > 0 && !app.snapshot().busy; });
        check(fake->evidence().fake_probe_calls > 0 && app.diagnostics_export_running(), "slow event collection does not block the connection worker");
        fake->blocked = false; crashexportfixture::wait(app);
        const auto report = json::parse(crashexportfixture::read(file));
        check(!app.diagnostics_export_running() && app.diagnostics_export_feedback() == "records.exportDone" && report &&
            report->find("crashes") && report->find("crashes")->find("status")->as_string() == "found" &&
            report->find("crashes")->find("records")->items().size() == 1, "completed asynchronous JSON includes exactly the sanitized crash metadata");
        check(report && report->find("settings")->find("probeMode")->as_string() == "quick" &&
            json::serialize(*report->find("samples")) == initial_samples && app.settings().probe == ProbeMode::complete,
            "settings and history are frozen at export selection rather than read later from a changing application");
        check(fake->calls == 1 && fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 &&
            fake->evidence().fake_auth_submissions == 0 && fake->evidence().production_storage_accesses == 0,
            "export uses the injected read-only collector and does not authenticate, access credentials or upload data");
        app.shutdown();
    }
    const std::string previous = crashexportfixture::read(file);
    {
        auto services = std::make_unique<crashexportfixture::Fake>(); auto* fake = services.get(); fake->blocked = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services), {}, {},
            [&](HWND) -> std::optional<std::wstring> { return path; }); app.start();
        check(app.export_diagnostics(nullptr), "cancellation fixture starts with an existing user-selected report");
        wifiruntime::wait_for([&] { return fake->started.load(); });
        app.cancel_diagnostics_export();
        check(app.diagnostics_export_feedback() == "records.exportCancelling", "cancel communicates that background export is stopping");
        crashexportfixture::wait(app);
        check(fake->finished && app.diagnostics_export_feedback() == "records.exportCancelled" && crashexportfixture::read(file) == previous,
            "cancelling collection preserves an existing report and never claims the cancelled report was saved");
        app.shutdown();
    }
    {
        auto services = std::make_unique<crashexportfixture::Fake>(); auto* fake = services.get(); fake->blocked = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services), {}, {},
            [&](HWND) -> std::optional<std::wstring> { return path; }); app.start(); app.export_diagnostics(nullptr);
        wifiruntime::wait_for([&] { return fake->started.load(); });
        const auto start = std::chrono::steady_clock::now(); app.shutdown();
        check(fake->finished && std::chrono::steady_clock::now() - start < std::chrono::seconds(2) &&
            crashexportfixture::read(file) == previous, "shutdown cancels and joins the dedicated collector without replacing an existing file");
    }
    {
        auto services = std::make_unique<crashexportfixture::Fake>(); auto* fake = services.get();
        const auto missing = (directory / L"missing" / L"report.json").wstring();
        AppController app(nullptr, RuntimeMode::normal, std::move(services), {}, {},
            [&](HWND) -> std::optional<std::wstring> { return missing; }); app.start(); app.export_diagnostics(nullptr); crashexportfixture::wait(app);
        check(app.diagnostics_export_feedback() == "records.exportFailed" && fake->calls == 1 && crashexportfixture::read(file) == previous,
            "a filesystem failure reports failure and leaves unrelated reports intact"); app.shutdown();
    }
    {
        auto services = std::make_unique<crashexportfixture::Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services), {}, {},
            [&](HWND) -> std::optional<std::wstring> { return path; }); app.start(); app.export_diagnostics(nullptr); crashexportfixture::wait(app);
        const auto report = json::parse(crashexportfixture::read(file));
        check(fake->calls == 0 && report && report->find("crashes") &&
            report->find("crashes")->find("status")->as_string() == "notCollected" && app.diagnostics_export_feedback() == "records.exportDone",
            "observe-only report exports explicitly mark system crash metadata as not collected without reading the OS"); app.shutdown();
    }
    {
        auto services = std::make_unique<crashexportfixture::Fake>(); auto* fake = services.get(); unsigned selections = 0;
        AppController app(nullptr, RuntimeMode::normal, std::move(services), {}, {},
            [&](HWND) -> std::optional<std::wstring> { ++selections; return std::nullopt; }); app.start();
        check(!app.export_diagnostics(nullptr) && selections == 1 && fake->calls == 0 && !app.diagnostics_export_running() &&
            app.diagnostics_export_feedback().empty(), "cancelling the save dialog starts no scan and does not display a successful export"); app.shutdown();
    }
    {
        unsigned selections = 0;
        AppController app(nullptr, RuntimeMode::offlineSmoke, {}, {}, {},
            [&](HWND) -> std::optional<std::wstring> { ++selections; return path; }); app.start();
        check(!app.export_diagnostics(nullptr) && selections == 0, "offline smoke never selects or writes a formal diagnostic file");
        check(app.diagnostic_export_text().find("\"crashes\"") == std::string::npos,
            "ordinary report inspection never initiates an implicit OS crash scan"); app.shutdown();
    }
    std::size_t remaining = 0;
    for (const auto& item : fs::directory_iterator(directory, error)) { (void)item; ++remaining; }
    check(!error && remaining == 1, "completed, cancelled and failed exports leave no intermediate files in the selected directory");
    fs::remove(file, error); fs::remove(directory, error);
    check(!error, "isolated report and temporary directory are removed after the regression");
}
