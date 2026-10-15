#pragma once
#include <algorithm>
#include <set>
#include "wifi_runtime_tests.hpp"

namespace preservationruntime {
class Fake final : public wifiruntime::Fake {
public:
    std::set<std::wstring> unreadable;
    std::vector<std::wstring> writes, quarantined;
    bool preserve_logs = false;
    std::atomic<int> captures{0};
    szu::win::Storage::ReadStatus read(const wchar_t* name, std::string& out) override {
        if (unreadable.count(name)) return szu::win::Storage::ReadStatus::failed;
        return wifiruntime::Fake::read(name, out);
    }
    bool write(const wchar_t* name, const std::string& data) override {
        writes.emplace_back(name);
        return wifiruntime::Fake::write(name, data);
    }
    bool quarantine(const wchar_t* name) override {
        quarantined.emplace_back(name);
        if (!preserve_logs || !files.count(name)) return false;
        files[std::wstring(name) + L".fixture-backup"] = files.at(name);
        files.erase(name);
        return true;
    }
    szu::win::NetworkSnapshot capture_network() override {
        ++captures;
        return wifiruntime::Fake::capture_network();
    }
    bool wrote(const wchar_t* name) const {
        return std::find(writes.begin(), writes.end(), name) != writes.end();
    }
};
}

template<class Check> void run_settings_preservation_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    for (const std::string invalid : {std::string("{broken"), std::string("[]"),
            std::string(R"({"formatVersion":2,"monitoringMode":"recover"})"),
            std::string(R"({"formatVersion":"1","monitoringMode":"recover"})"),
            std::string(R"({"formatVersion":null})"), std::string(R"({"formatVersion":1.5})"),
            std::string(R"({"formatVersion":1,"monitoringMode":true})"),
            std::string(R"({"formatVersion":1,"area":"future"})"),
            std::string(R"({"formatVersion":1,"tutorial":{"status":false}})")}) {
        auto services = std::make_unique<preservationruntime::Fake>(); auto* fake = services.get();
        fake->files[L"settings.json"] = invalid;
        wifi::Preferences preferences; preferences.auto_join = true; preferences.power_on = true;
        fake->files[L"wifi-recovery.json"] = *preferences.encode();
        const auto memory = fake->files.at(L"network-memory.json"), wifi_original = fake->files.at(L"wifi-recovery.json");
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(!app.settings_writable() && app.settings().mode == MonitoringMode::manual &&
              app.snapshot().mode == MonitoringMode::manual && app.notice() == "error.settingsUnreadable",
              "unknown or damaged settings start conservatively and expose preservation status");
        app.set_mode(MonitoringMode::recover); app.set_mode(MonitoringMode::observe);
        app.set_appearance(Appearance::dark); app.set_probe(ProbeMode::complete); app.set_tutorial("completed");
        check(app.settings().mode == MonitoringMode::manual && app.settings().appearance == Appearance::dark &&
              app.settings().probe == ProbeMode::complete,
              "protected settings reject automatic modes while allowing session-only appearance and probe previews");
        const int captured = fake->captures;
        app.on_timer(kTimerAuto);
        wifiruntime::wait_for([&] { return fake->captures > captured; });
        app.on_timer(kTimerWiFi);
        check(app.wifi_status() == "disabled", "protected primary settings disable restored Wi-Fi recovery eligibility");
        app.shutdown();
        check(fake->files.at(L"settings.json") == invalid && fake->files.at(L"network-memory.json") == memory &&
              fake->files.at(L"wifi-recovery.json") == wifi_original && !fake->wrote(L"settings.json") && fake->quarantined.empty(),
              "protected settings and network approvals remain byte-for-byte intact without quarantine or overwrite");
        check(fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0 &&
              fake->evidence().fake_probe_calls == 0 && fake->evidence().network_calls == 0,
              "invalid settings cannot start automatic probing, authentication or restored Wi-Fi finding");
    }
    {
        auto services = std::make_unique<preservationruntime::Fake>(); auto* fake = services.get();
        const auto original = fake->files.at(L"settings.json"); fake->unreadable.insert(L"settings.json");
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        app.set_appearance(Appearance::light); app.set_mode(MonitoringMode::recover); app.shutdown();
        check(!app.settings_writable() && app.settings().mode == MonitoringMode::manual &&
              fake->files.at(L"settings.json") == original && !fake->wrote(L"settings.json") && fake->quarantined.empty(),
              "a settings read failure is preserved rather than replaced with defaults");
    }
    {
        auto services = std::make_unique<preservationruntime::Fake>(); auto* fake = services.get();
        const std::string legacy = R"({"monitoringMode":"observe","area":"teaching","probeMode":"complete","appearanceMode":"dark","tutorial":{"status":"skipped"}})";
        fake->files[L"settings.json"] = legacy;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(app.settings_writable() && app.settings().mode == MonitoringMode::observe && app.settings().area == CampusArea::teaching &&
              app.settings().probe == ProbeMode::complete && app.settings().appearance == Appearance::dark &&
              app.settings().tutorial == "skipped" && fake->files.at(L"settings.json") == legacy,
              "known legacy v1 fields load without a version or implicit rewrite");
        app.set_mode(MonitoringMode::manual); app.shutdown();
        check(json::parse(fake->files.at(L"settings.json"))->number_at("formatVersion") == 1 && fake->quarantined.empty(),
              "a deliberate valid legacy preference change saves the compatible v1 schema");
    }
    {
        auto services = std::make_unique<preservationruntime::Fake>(); auto* fake = services.get();
        fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual"})";
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const std::string future = R"({"formatVersion":2,"monitoringMode":"recover","future":"retained"})";
        fake->files[L"settings.json"] = future;
        app.set_appearance(Appearance::dark); app.shutdown();
        check(!app.settings_writable() && app.settings().mode == MonitoringMode::manual &&
              fake->files.at(L"settings.json") == future && !fake->wrote(L"settings.json") && fake->quarantined.empty(),
              "settings that become unknown after startup cannot be overwritten by a later preference change");
    }
    for (const wchar_t* damaged : {L"events.json", L"health.json"}) {
        for (const bool read_failure : {false, true}) {
            auto services = std::make_unique<preservationruntime::Fake>(); auto* fake = services.get();
            fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual"})";
            fake->files[damaged] = "damaged-fixture-original";
            if (read_failure) fake->unreadable.insert(damaged);
            AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
            app.set_mode(MonitoringMode::observe); app.on_changed(); app.on_timer(kTimerPersist); app.shutdown();
            const wchar_t* healthy = std::wstring(damaged) == L"events.json" ? L"health.json" : L"events.json";
            check(fake->files.at(damaged) == "damaged-fixture-original" && !fake->wrote(damaged) && fake->wrote(healthy),
                  "unreadable or unpreserved log remains intact while the independent healthy log still persists");
            check(read_failure ? fake->quarantined.empty() : fake->quarantined == std::vector<std::wstring>{damaged},
                  "read failures never quarantine and failed log quarantine does not authorize replacement");
        }
    }
    {
        auto services = std::make_unique<preservationruntime::Fake>(); auto* fake = services.get();
        fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual"})";
        fake->files[L"events.json"] = "damaged-fixture-original"; fake->preserve_logs = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        app.set_mode(MonitoringMode::observe); app.on_changed(); app.on_timer(kTimerPersist); app.shutdown();
        check(fake->files.at(L"events.json.fixture-backup") == "damaged-fixture-original" &&
              records::EventLog::from_json(fake->files.at(L"events.json")).has_value(),
              "successful log preservation permits a new valid log while retaining the damaged original");
    }
}
