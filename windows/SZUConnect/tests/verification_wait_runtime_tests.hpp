#pragma once
#include "wifi_runtime_tests.hpp"

namespace verificationwait {
class Fake final : public wifiruntime::Fake {
public:
    Fake() {
        files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
    }
    szu::win::NetworkSnapshot capture_network() override {
        szu::win::NetworkSnapshot snapshot;
        snapshot.path.usable = snapshot.path.link_active = true;
        snapshot.path.identity = next_path ? "fixture-verification-next" : "fixture-verification-original";
        snapshot.path.descriptors = {szu::NetworkDescriptor{"wifi", {{"gwip", "192.0.2.1"}, {"net", "192.0.2.0/24"}}}};
        return snapshot;
    }
    szu::ConnectivityReport probe(szu::ProbeMode mode, const szu::CancelToken& token) override {
        ++probes;
        auto report = szu::ConnectivityReport::simple(online, online);
        report.mode = mode;
        if (token.cancelled()) report.apple.state = report.microsoft.state = szu::EndpointState::cancelled;
        return report;
    }
    szu::LoginAttempt attempt(const szu::Credentials&, szu::CampusArea, const szu::CancelToken&) override {
        ++evidence().fake_auth_submissions;
        szu::LoginAttempt result; result.outcome = szu::Outcome::success; return result;
    }
    std::atomic<int> probes{0};
    std::atomic<bool> online{false}, next_path{false};
};
}

template<class Check> void run_verification_wait_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    using Clock = std::chrono::steady_clock;
    for (const int phase : {2, 3}) {
        auto services = std::make_unique<verificationwait::Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.connect();
        const auto deadline = Clock::now() + std::chrono::seconds(4);
        while (fake->probes < phase && Clock::now() < deadline) { app.on_changed(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        check(fake->probes == phase && app.snapshot().status == EngineStatus::verifying,
              "fake authentication reaches its scheduled two/five-second verification wait");
        const auto started = Clock::now();
        app.cancel(); fake->online = true;
        wifiruntime::wait_for([&] {
            if (fake->probes == phase) app.check();
            return fake->probes > phase && !app.snapshot().busy;
        });
        const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
        check(elapsed < 0.5 && fake->probes == phase + 1 && app.snapshot().online,
              "cancelled verification wait releases the worker for a fresh check within 500 ms");
        check(fake->evidence().fake_auth_submissions == 1 && app.snapshot().cooldown_remaining.value_or(0) > 80,
              "cancelling a wait neither resubmits credentials nor clears the accepted-login cooldown");
        std::printf("Verification wait cancellation (%d-second interval): %.1f ms\n", phase == 2 ? 2 : 5, elapsed * 1000);
        app.shutdown();
        check(fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 && fake->evidence().production_storage_accesses == 0,
              "verification timing test uses only injected network, credentials and storage");
    }
    for (const bool sleep : {false, true}) {
        auto services = std::make_unique<verificationwait::Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.connect();
        wifiruntime::wait_for([&] { app.on_changed(); return fake->probes >= 2; });
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        const auto started = Clock::now();
        if (sleep) app.on_suspend();
        else { fake->next_path = true; app.refresh_environment(); }
        if (!sleep) {
            fake->online = true;
            wifiruntime::wait_for([&] {
                if (fake->probes == 2) app.check();
                return fake->probes > 2 && !app.snapshot().busy;
            });
        }
        app.shutdown();
        const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
        check(elapsed < 0.5 && fake->probes == (sleep ? 2 : 3) && fake->evidence().fake_auth_submissions == 1,
              "sleep or changed physical path wakes the verification wait without any old-path recheck");
    }
    CancelToken cancelled; cancelled.cancel();
    const auto started = Clock::now();
    check(cancelled.wait_for(5) && std::chrono::duration<double>(Clock::now() - started).count() < 0.1,
          "a token cancelled before entering a wait returns immediately");
    CancelToken active;
    check(!active.wait_for(0.01), "an uncancelled wait expires normally without cancelling its operation");
}
