#pragma once
#include "wifi_runtime_tests.hpp"
#include <set>

namespace logpersistenceruntime {
class Fake final : public wifiruntime::Fake {
public:
    Fake() {
        physical_path = true;
        files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
    }
    szu::ConnectivityReport probe(szu::ProbeMode mode, const szu::CancelToken&) override {
        ++probes;
        auto result = szu::ConnectivityReport::simple(true, true); result.mode = mode;
        return result;
    }
    bool write(const wchar_t* name, const std::string& data) override {
        ++writes[name]; bytes[name] += data.size();
        if (failing.contains(name)) return false;
        return wifiruntime::Fake::write(name, data);
    }
    std::atomic<int> probes{0};
    std::map<std::wstring, int> writes;
    std::map<std::wstring, std::size_t> bytes;
    std::set<std::wstring> failing;
};
}

template<class Check> void run_log_persistence_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    {
        auto services = std::make_unique<logpersistenceruntime::Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto round = [&] {
            const auto next = fake->probes.load() + 1;
            app.check();
            wifiruntime::wait_for([&] {
                app.on_changed();
                return fake->probes >= next && !app.snapshot().busy && app.internet_report().has_value();
            });
            app.on_changed(); app.on_timer(kTimerPersist);
        };
        round();
        const auto events = fake->writes[L"events.json"], health = fake->writes[L"health.json"];
        const auto stored_events = fake->files[L"events.json"];
        const auto event_bytes = fake->bytes[L"events.json"];
        for (int i = 0; i < 30; ++i) round();
        check(fake->writes[L"events.json"] == events && fake->bytes[L"events.json"] == event_bytes &&
            fake->files[L"events.json"] == stored_events && fake->writes[L"health.json"] == health + 30 &&
            app.health().items().size() == 31,
            "thirty unchanged online rounds persist every sample without encoding or rewriting the unchanged event log");
        const auto samples = fake->writes[L"health.json"];
        app.set_mode(MonitoringMode::observe); app.on_changed(); app.on_timer(kTimerPersist);
        check(fake->writes[L"events.json"] == events + 1 && fake->writes[L"health.json"] == samples,
            "an event-only change writes its own log without rewriting unchanged health history");
        const auto saved_health = fake->files[L"health.json"];
        fake->failing.insert(L"health.json");
        round();
        const auto failed_samples = app.health().items().size();
        check(fake->files[L"health.json"] == saved_health && failed_samples == 32,
            "a failed health write preserves the previous file and pending in-memory observations");
        fake->failing.clear();
        const auto events_before_retry = fake->writes[L"events.json"];
        app.on_timer(kTimerPersist);
        const auto restored = records::HealthLog::from_json(fake->files[L"health.json"]);
        check(restored && restored->items().size() == failed_samples &&
            fake->writes[L"events.json"] == events_before_retry,
            "a timer retry saves pending health without requiring another network operation or duplicating event writes");
        const auto health_before_event_failure = fake->writes[L"health.json"];
        fake->failing.insert(L"events.json");
        app.set_mode(MonitoringMode::manual); app.on_changed(); app.on_timer(kTimerPersist);
        check(fake->writes[L"health.json"] == health_before_event_failure,
            "an event write failure never dirties unrelated health history");
        fake->failing.clear();
        app.shutdown();
        const auto saved = records::EventLog::from_json(fake->files[L"events.json"]);
        check(saved && saved->items().size() == app.events().items().size(),
            "normal shutdown retries unsaved events after a transient write failure");
        check(fake->evidence().fake_auth_submissions == 0 && fake->evidence().network_calls == 0 &&
            fake->evidence().production_storage_accesses == 0,
            "log persistence regression never submits credentials or reaches production networking and storage");
        std::printf("Log persistence: 30 steady rounds, %d event writes, %d health writes\n",
            events_before_retry - events - 1, samples - health);
    }
    {
        auto services = std::make_unique<logpersistenceruntime::Fake>(); auto* fake = services.get();
        const std::string corrupt = "fixture unreadable event history";
        fake->files[L"events.json"] = corrupt;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        app.set_mode(MonitoringMode::observe); app.on_changed(); app.on_timer(kTimerPersist); app.shutdown();
        check(fake->writes[L"events.json"] == 0 && fake->files[L"events.json"] == corrupt,
            "an unreadable event file that cannot be quarantined is never overwritten by retry or shutdown");
    }
    {
        auto services = std::make_unique<logpersistenceruntime::Fake>(); auto* fake = services.get();
        const auto files = fake->files;
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services)); app.start(); app.check();
        wifiruntime::wait_for([&] {
            app.on_changed(); return fake->probes > 0 && !app.snapshot().busy && app.internet_report().has_value();
        });
        app.on_changed(); app.on_timer(kTimerPersist); app.shutdown();
        check(fake->writes.empty() && fake->files == files && fake->evidence().fake_auth_submissions == 0,
            "read-only detection, timer and shutdown do not persist logs or recovery records");
    }
}

