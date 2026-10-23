#pragma once
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace environmentruntime {
class Fake final : public wifiruntime::Fake {
public:
    Fake() {
        files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
        files.erase(L"network-memory.json");
        files.erase(L"network-labels.json");
    }
    szu::win::NetworkSnapshot capture_network() override {
        ++captures;
        szu::win::NetworkSnapshot result;
        result.access = readable ? "ethernet" : "unknown";
        result.tunnel_known = readable;
        result.path.usable = true;
        result.path.link_active = true;
        result.path.identity = "fixture-environment-path";
        return result;
    }
    szu::proxy::WindowsRaw read_proxy() override {
        ++proxy_reads;
        szu::proxy::WindowsRaw result; result.readable = readable;
        return result;
    }
    szu::ConnectivityReport probe(szu::ProbeMode mode, const szu::CancelToken&) override {
        ++probes;
        auto report = szu::ConnectivityReport::simple(true, true); report.mode = mode;
        return report;
    }
    std::optional<szu::ConnectivityReport> system_connectivity(szu::ProbeMode mode, const szu::CancelToken&) override {
        ++system_probes;
        auto report = szu::ConnectivityReport::simple(true, true); report.mode = mode; return report;
    }
    szu::CampusDiscovery discover(szu::CampusArea, const szu::CancelToken&) override {
        ++discoveries; return {};
    }
    bool write(const wchar_t* name, const std::string& value) override {
        ++writes; return wifiruntime::Fake::write(name, value);
    }
    bool readable = true;
    std::atomic<int> captures{0}, proxy_reads{0}, probes{0}, system_probes{0}, discoveries{0}, writes{0};
};
}

template<class Check> void run_environment_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    for (const auto mode : {RuntimeMode::normal, RuntimeMode::observeOnly}) {
        auto services = std::make_unique<environmentruntime::Fake>(); auto* fake = services.get();
        AppController app(nullptr, mode, std::move(services)); app.start();
        const auto writes = fake->writes.load();
        const auto captures = fake->captures.load(), proxy_reads = fake->proxy_reads.load();
        check(!app.environment_refreshed(), "connection refresh feedback starts empty instead of implying a user refresh");
        app.refresh_environment();
        const auto refreshed = app.environment_refreshed();
        check(refreshed && !app.environment_refresh_incomplete() && fake->captures == captures + 1 && fake->proxy_reads == proxy_reads + 1,
              "explicit connection refresh rereads metadata and provides a completion timestamp");
        fake->readable = false; app.refresh_environment();
        check(app.environment_refreshed() && *app.environment_refreshed() >= *refreshed && app.environment_refresh_incomplete(),
              "unreadable local connection fields have partial-refresh feedback rather than a success claim");
        fake->readable = true; app.refresh_environment();
        check(!app.environment_refresh_incomplete() && fake->probes == 0 && fake->system_probes == 0 &&
              fake->discoveries == 0 && fake->writes == writes && !app.internet_report() && !app.system_connectivity() &&
              fake->evidence().fake_auth_submissions == 0,
              "metadata refresh neither invents connectivity results nor sends probes, credentials or configuration writes");
        app.check();
        wifiruntime::wait_for([&] { return app.internet_report().has_value() && app.system_connectivity().has_value() && !app.snapshot().busy; });
        check(fake->probes == 1 && fake->system_probes == 1 && fake->discoveries == 0 &&
              fake->evidence().fake_auth_submissions == 0 && app.environment_refreshed(),
              "explicit check refreshes both displayed connectivity paths without authentication in normal and read-only mode");
        app.shutdown();
    }
}
