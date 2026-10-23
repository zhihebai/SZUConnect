#pragma once
#include "campus_verification_runtime_tests.hpp"
#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace historydetailruntime {
class Fake final : public campusverificationruntime::Fake {
public:
    Fake() : campusverificationruntime::Fake("manual") {}
    szu::win::NetworkSnapshot capture_network() override {
        if (flip_proxy_on_capture.exchange(false)) proxy_off = true;
        auto value = campusverificationruntime::Fake::capture_network();
        value.access = changed ? "multiple" : "ethernet";
        value.tunnel.tunnel = value.path.tunnel = !changed;
        value.tunnel_known = true; return value;
    }
    szu::proxy::WindowsRaw read_proxy() override {
        szu::proxy::WindowsRaw value;
        if (!changed && !proxy_off) value.proxy = "http=127.0.0.1:19876";
        return value;
    }
    szu::ConnectivityReport probe(szu::ProbeMode mode, const szu::CancelToken&) override {
        ++probes;
        auto report = szu::ConnectivityReport::simple(false, false); report.mode = mode;
        report.apple = {szu::EndpointState::timeout, 1.234, std::nullopt};
        report.microsoft = {szu::EndpointState::tls, 0.875, std::nullopt}; return report;
    }
    std::atomic<bool> flip_proxy_on_capture{false}, proxy_off{false};
};
}

template<class Check> void run_history_detail_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    auto services = std::make_unique<historydetailruntime::Fake>(); auto* fake = services.get();
    AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
    const auto started = AppController::wall_now();
    app.check();
    // Deliberately leave kMsgChanged unconsumed until the UI has moved to another network and mode.
    wifiruntime::wait_for([&] { return fake->probes > 0 && !app.snapshot().busy && app.internet_report().has_value(); });
    const auto completed = AppController::wall_now();
    check(app.health().items().empty(), "completed worker reports wait in their isolated inbox before UI consumption");
    fake->changed = true; app.set_area(CampusArea::dormitory); app.set_mode(MonitoringMode::observe);
    app.refresh_environment(); app.on_changed();
    const auto& items = app.health().items();
    check(items.size() == 1 && items[0].apple == "timeout" && items[0].microsoft == "tls" &&
        items[0].apple_ms == 1234 && items[0].microsoft_ms == 875 && items[0].diagnosis == "security" &&
        items[0].time >= started && items[0].time <= completed && items[0].context &&
        items[0].context->started_at >= started && items[0].context->started_at <= items[0].time &&
        items[0].context->operation == "manual" && items[0].context->monitoring_mode == "manual" &&
        items[0].context->area == "automatic" && items[0].context->environment.connection == "ethernet" &&
        items[0].context->environment.system_proxy == "on" && items[0].context->environment.tun == "detected",
        "Controller seals actual failure durations, completion time and original operation environment before later UI state changes");
    check(!app.last_checked() && app.settings().mode == MonitoringMode::observe && app.snapshot().area == CampusArea::dormitory &&
        fake->evidence().fake_auth_submissions == 0,
        "an old-epoch history sample cannot backfill the new connection's current check time or trigger authentication");
    check(std::any_of(app.events().items().begin(), app.events().items().end(), [&](const records::Event& event) {
        return event.failure && event.time >= started && event.time <= completed;
    }), "a delayed failure event retains its emission time instead of the later UI receipt time");
    app.on_timer(kTimerPersist);
    const auto saved = fake->files.find(L"health.json");
    const auto stored = saved == fake->files.end() ? std::string() : saved->second;
    const auto report = json::parse(app.diagnostic_export_text());
    const auto copied = app.diagnostic_summary_text();
    check(!stored.empty() && report && report->find("samples") && report->find("samples")->size() == 1 &&
        report->find("samples")->items()[0].number_at("appleMilliseconds") == 1234 &&
        report->find("samples")->items()[0].find("context")->string_at("area") == "automatic" &&
        copied.find("1234 ms") != std::string::npos && copied.find("875 ms") != std::string::npos,
        "persisted health, detailed JSON and copied failure summary all use the same sealed report");
    bool private_absent = true;
    const auto output = app.diagnostic_export_text();
    for (const auto* marker : {"Fixture Ethernet", "Fixture laboratory cable", "192.0.2.", "127.0.0.1", "19876", "fixture-first-cable"})
        private_absent = private_absent && stored.find(marker) == std::string::npos && output.find(marker) == std::string::npos && copied.find(marker) == std::string::npos;
    check(private_absent && fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 &&
        fake->evidence().production_storage_accesses == 0 && fake->evidence().fake_wifi_calls == 0,
        "historical context stays anonymous and uses no formal network, storage, credentials or Wi-Fi services");
    app.shutdown();
    auto replay = std::make_unique<historydetailruntime::Fake>(); auto* replay_fake = replay.get();
    replay_fake->files = fake->files; replay_fake->changed = true;
    const auto before = replay_fake->files;
    AppController reloaded(nullptr, RuntimeMode::observeOnly, std::move(replay)); reloaded.start();
    const auto reloaded_report = json::parse(reloaded.diagnostic_export_text());
    check(reloaded.health().items().size() == 1 && reloaded_report &&
        json::serialize(*reloaded_report->find("samples")) == json::serialize(*report->find("samples")) &&
        reloaded.diagnostic_summary_text().find("1234 ms") != std::string::npos && replay_fake->files == before &&
        replay_fake->probes == 0 && replay_fake->evidence().fake_auth_submissions == 0 && replay_fake->evidence().vault_accesses == 0,
        "read-only restart preserves and exports historical context without resampling, rewriting or contacting formal services");
    reloaded.shutdown();
    {
        auto automatic_services = std::make_unique<historydetailruntime::Fake>(); auto* automatic = automatic_services.get();
        AppController automatic_app(nullptr, RuntimeMode::normal, std::move(automatic_services), [](UINT) { return true; });
        automatic_app.start(); automatic_app.set_mode(MonitoringMode::recover);
        automatic->flip_proxy_on_capture = true;
        automatic_app.on_timer(kTimerAuto);
        wifiruntime::wait_for([&] { return automatic->probes > 0 && !automatic_app.snapshot().busy && automatic_app.internet_report().has_value(); });
        automatic_app.on_changed();
        const auto& automatic_history = automatic_app.health().items();
        check(automatic->proxy_off && automatic_app.proxy_summary().state == "on" && automatic_history.size() == 1 &&
            automatic_history[0].context && automatic_history[0].context->operation == "automatic" &&
            automatic_history[0].context->environment.system_proxy == "unknown" &&
            automatic_history[0].context->environment.connection == "ethernet" &&
            automatic_history[0].context->environment.tun == "detected" &&
            automatic->evidence().fake_auth_submissions == 0 && automatic->evidence().network_calls == 0,
            "a fresh automatic attachment snapshot never labels its round with queued UI proxy state that changed before worker capture");
        automatic_app.shutdown();
    }
}
