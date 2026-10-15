#pragma once
#include <memory>
#include <utility>
// Benign preferences round-trip through the real controller and an isolated store.
// The fake never constructs production storage, a vault, WLAN or network watchers.
template<class Check> void run_settings_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    auto services = std::make_unique<wifiruntime::Fake>(); auto* fake = services.get();
    fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
    AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
    app.set_area(CampusArea::dormitory); app.set_probe(ProbeMode::complete);
    app.set_appearance(Appearance::dark); app.set_tutorial("completed");
    app.set_mode(MonitoringMode::observe);
    check(app.settings().area == CampusArea::dormitory && app.settings().probe == ProbeMode::complete &&
          app.settings().appearance == Appearance::dark && app.settings().tutorial == "completed" &&
          app.settings().mode == MonitoringMode::observe, "normal preferences apply through controller setters");
    app.shutdown();
    auto persisted = fake->files;
    const auto encoded = json::parse(persisted.at(L"settings.json"));
    check(encoded && encoded->number_at("formatVersion") == 1 && encoded->string_at("monitoringMode") == "observe" &&
          encoded->string_at("area") == "dormitory" && encoded->string_at("probeMode") == "complete" &&
          encoded->string_at("appearanceMode") == "dark", "saved preferences retain the compatible version-one schema");
    check(fake->evidence().fake_auth_submissions == 0 && fake->evidence().network_calls == 0 &&
          fake->evidence().production_storage_accesses == 0 && fake->evidence().vault_accesses == 0,
          "preference round-trip never reaches formal data, credentials or authentication");

    auto restarted_services = std::make_unique<wifiruntime::Fake>(); auto* restarted_fake = restarted_services.get();
    restarted_fake->files = persisted;
    AppController restarted(nullptr, RuntimeMode::normal, std::move(restarted_services)); restarted.start();
    check(restarted.settings().area == CampusArea::dormitory && restarted.settings().probe == ProbeMode::complete &&
          restarted.settings().appearance == Appearance::dark && restarted.settings().tutorial == "completed" &&
          restarted.settings().mode == MonitoringMode::observe && restarted.snapshot().probe_mode == ProbeMode::complete,
          "a fresh controller restores every saved preference and the engine probe mode");
    restarted_fake->fail_write = true;
    restarted.set_appearance(Appearance::light);
    check(restarted.notice() == "error.writeFailed" && restarted_fake->files.at(L"settings.json") == persisted.at(L"settings.json"),
          "failed preference storage reports the error and retains previous durable settings");
    restarted_fake->fail_write = false;
    restarted.set_appearance(Appearance::system);
    const auto retried = json::parse(restarted_fake->files.at(L"settings.json"));
    check(retried && retried->string_at("appearanceMode") == "system", "preference storage can succeed after a transient write failure");
    restarted.shutdown();

    auto readonly_services = std::make_unique<wifiruntime::Fake>(); auto* readonly_fake = readonly_services.get();
    readonly_fake->files = persisted;
    AppController readonly(nullptr, RuntimeMode::observeOnly, std::move(readonly_services)); readonly.start();
    readonly.set_appearance(Appearance::light); readonly.set_probe(ProbeMode::quick);
    readonly.set_mode(MonitoringMode::recover); readonly.set_area(CampusArea::automatic); readonly.set_tutorial("skipped");
    readonly.shutdown();
    check(readonly_fake->files == persisted && readonly_fake->evidence().fake_auth_submissions == 0 &&
          readonly_fake->evidence().network_calls == 0 && readonly_fake->evidence().vault_accesses == 0 &&
          readonly_fake->evidence().production_storage_accesses == 0,
          "observe-only preference edits and shutdown leave all original fake files unchanged");
}
