#pragma once
#include "network_recovery_schedule_runtime_tests.hpp"
#include <cmath>
#include <vector>

namespace autotimerruntime {
struct Arm {
    bool fail = false;
    std::vector<UINT> waits;
    std::vector<double> deadlines;
    bool operator()(UINT milliseconds) {
        waits.push_back(milliseconds);
        deadlines.push_back(szu::app::AppController::wall_now() + milliseconds / 1000.0);
        return !fail;
    }
};
}

template<class Check> void run_auto_timer_recovery_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    {
        auto services = std::make_unique<networkrecoveryscheduleruntime::Fake>(); auto* fake = services.get();
        autotimerruntime::Arm arm; arm.fail = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services), [&](UINT wait) { return arm(wait); });
        app.start();
        const auto epoch = app.snapshot().epoch;
        const auto files = fake->files;
        check(arm.waits.size() == 1 && !app.next_check(),
            "failed automatic timer allocation is not advertised as an armed monitoring deadline");
        app.on_timer(kTimerAuto);
        check(fake->probes == 0 && fake->discoveries == 0 && arm.waits.size() == 1,
            "a stale automatic timer callback cannot bypass a failed allocation's retained deadline");
        for (int message = 0; message < 8; ++message) {
            fake->tunnel = !fake->tunnel;
            app.on_network_signal(); app.on_timer(kTimerNetwork); app.on_changed();
        }
        check(arm.waits.size() == 1 && app.snapshot().epoch == epoch && !app.next_check(),
            "UI updates and same-attachment TUN notifications neither spin nor replace a failed timer's pending deadline");
        app.on_timer(kTimerWiFi);
        check(arm.waits.size() == 2 && !app.next_check() && fake->probes == 0 && fake->discoveries == 0,
            "one existing maintenance pulse makes one timer-allocation retry without issuing network work");
        arm.fail = false; app.on_timer(kTimerWiFi);
        check(arm.waits.size() == 3 && app.next_check() && arm.waits.back() == 1 && fake->probes == 0 &&
            fake->discoveries == 0 && fake->files == files && fake->evidence().fake_auth_submissions == 0,
            "timer allocation can recover at its overdue deadline without granting trust, changing data or authenticating");
        app.on_timer(kTimerAuto);
        networkrecoveryscheduleruntime::finish(app, [&] { return fake->probes == 1 && fake->returned == 1; });
        check(app.next_check() && fake->evidence().fake_auth_submissions == 0,
            "a recovered timer resumes the ordinary credential-free monitoring and service-verification path");
        app.shutdown();
    }
    {
        auto services = std::make_unique<networkrecoveryscheduleruntime::Fake>(); auto* fake = services.get();
        fake->constrained = true;
        autotimerruntime::Arm arm;
        AppController app(nullptr, RuntimeMode::normal, std::move(services), [&](UINT wait) { return arm(wait); });
        app.start(); arm.fail = true; app.on_timer(kTimerAuto);
        wifiruntime::wait_for([&] { return fake->probes == 1 && fake->returned == 1 && !app.snapshot().busy; });
        wifiruntime::wait_for([&] { app.on_changed(); return arm.waits.size() >= 2; });
        const double original = arm.deadlines.back();
        check(arm.waits.back() > 110000 && !app.next_check(),
            "long protected monitoring delay remains pending when its native timer cannot be created");
        const auto attempts = arm.waits.size();
        app.on_changed(); app.on_changed();
        check(arm.waits.size() == attempts,
            "failed long timer does not create a retry loop through ordinary change notifications");
        fake->block = true;
        check(app.verify_campus_service(), "independent campus verification can reserve a pending long timer");
        wifiruntime::wait_for([&] { return fake->discoveries == 2; });
        arm.fail = false; app.on_timer(kTimerWiFi);
        check(arm.waits.size() == attempts && !app.next_check(),
            "maintenance cannot recreate a timer suspended for independent verification");
        app.cancel(); fake->released = true;
        wifiruntime::wait_for([&] {
            app.on_changed(); return fake->returned == 2 && !app.snapshot().busy && app.next_check();
        });
        check(app.next_check() && *app.next_check() >= original - 0.02 && arm.waits.back() > 110000,
            "cancelled verification restores the original long pending deadline without shortening it");
        app.shutdown();
    }
    {
        auto services = std::make_unique<networkrecoveryscheduleruntime::Fake>(); auto* fake = services.get();
        fake->constrained = true;
        autotimerruntime::Arm arm;
        AppController app(nullptr, RuntimeMode::normal, std::move(services), [&](UINT wait) { return arm(wait); });
        app.start(); arm.fail = true; app.on_timer(kTimerAuto);
        wifiruntime::wait_for([&] { return fake->probes == 1 && fake->returned == 1 && !app.snapshot().busy; });
        wifiruntime::wait_for([&] { app.on_changed(); return arm.waits.size() >= 2; });
        const double original = arm.deadlines.back();
        arm.fail = false; app.on_timer(kTimerWiFi);
        check(app.next_check() && *app.next_check() >= original - 0.02 && arm.waits.back() > 110000 &&
            fake->probes == 1 && fake->discoveries == 1 && fake->evidence().fake_auth_submissions == 0,
            "maintenance recovery preserves the original long deadline instead of advancing the next check");
        app.shutdown();
    }
    for (const bool sleeping : {false, true}) {
        auto services = std::make_unique<networkrecoveryscheduleruntime::Fake>(); auto* fake = services.get();
        autotimerruntime::Arm arm; arm.fail = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services), [&](UINT wait) { return arm(wait); });
        app.start();
        if (sleeping) app.on_suspend(); else app.set_mode(MonitoringMode::manual);
        const auto attempts = arm.waits.size(); arm.fail = false;
        app.on_timer(kTimerWiFi); app.on_changed();
        check(arm.waits.size() == attempts && !app.next_check() && fake->probes == 0 &&
            fake->discoveries == 0 && fake->evidence().fake_auth_submissions == 0,
            "manual mode or sleep revokes pending timer retries without starting background operations");
        app.shutdown();
        app.on_timer(kTimerWiFi);
        check(arm.waits.size() == attempts && !app.next_check(),
            "a late maintenance callback after shutdown cannot resurrect the cancelled automatic timer");
    }
}
