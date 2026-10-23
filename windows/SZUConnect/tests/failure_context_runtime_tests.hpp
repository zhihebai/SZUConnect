#pragma once
#include "campus_verification_runtime_tests.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace failurecontextruntime {
class Fake final : public campusverificationruntime::Fake {
public:
    Fake() : campusverificationruntime::Fake("recover") {}
    szu::win::NetworkSnapshot capture_network() override {
        auto value = campusverificationruntime::Fake::capture_network();
        value.path.tunnel = value.tunnel.tunnel = true; value.tunnel_known = true;
        value.physical_diagnostics.interface_count = 3; value.physical_diagnostics.candidate_count = 1;
        value.physical_diagnostics.reject(szu::physical::CandidateRejection::missingDNS);
        return value;
    }
    szu::proxy::WindowsRaw read_proxy() override { szu::proxy::WindowsRaw value; value.proxy = "http=127.0.0.1:19876"; return value; }
    bool erase_credentials() override { return true; }
    szu::ConnectivityReport probe(szu::ProbeMode mode, const szu::CancelToken&) override {
        ++probes; auto value = szu::ConnectivityReport::simple(online, online); value.mode = mode;
        if (!online) {
            value.apple.state = value.microsoft.state = szu::EndpointState::redirect;
            value.apple.campus_area_hint = value.microsoft.campus_area_hint = szu::CampusPortalHint::teaching;
        }
        value.apple.elapsed = 1.234; value.microsoft.elapsed = 0.875; return value;
    }
    szu::CampusDiscovery discover(szu::CampusArea, const szu::CancelToken&) override {
        ++discoveries;
        while (block && !released) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++returned;
        return {confirmed ? szu::Discovery::confirmed : szu::Discovery::unconfirmed,
            confirmed ? szu::Discovery::confirmed : szu::Discovery::unconfirmed, false,
            confirmed ? szu::CampusVerificationIssue::none : szu::CampusVerificationIssue::dnsTls};
    }
    szu::LoginAttempt attempt(const szu::Credentials&, szu::CampusArea, const szu::CancelToken&) override {
        ++attempts;
        while (hold_attempt && !release_attempt) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        szu::LoginAttempt value; value.outcome = szu::Outcome::security; value.error = szu::PortalError::tls;
        value.credential_submission_started = false; return value;
    }
    std::atomic<bool> online{false}, hold_attempt{false}, release_attempt{false};
    std::atomic<int> attempts{0};
};
inline std::size_t context_count(const szu::app::AppController& app) {
    return static_cast<std::size_t>(std::count_if(app.events().items().begin(), app.events().items().end(),
        [](const szu::records::Event& event) { return event.context.has_value(); }));
}
inline std::string contextual_events(const szu::app::AppController& app) {
    auto list = szu::json::Value::array();
    const auto exported = szu::json::parse(app.diagnostic_export_text());
    if (exported && exported->find("events")) for (const auto& event : exported->find("events")->items())
        if (event.find("context")) list.push(event);
    return szu::json::serialize(list);
}
inline void drain(szu::app::AppController& app, const std::function<bool()>& complete) {
    wifiruntime::wait_for([&] { app.on_changed(); return complete(); }); app.on_changed();
}
}

template<class Check> void run_failure_context_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    using failurecontextruntime::Fake;
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get(); fake->hold_attempt = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services), [](UINT) { return true; }); app.start(); app.connect();
        failurecontextruntime::drain(app, [&] { return fake->attempts == 1; });
        app.on_timer(kTimerAuto); fake->release_attempt = true;
        failurecontextruntime::drain(app, [&] { return !app.snapshot().busy && app.next_check() && failurecontextruntime::context_count(app) == 1; });
        const auto saved_failure = failurecontextruntime::contextual_events(app);
        const auto event = std::find_if(app.events().items().begin(), app.events().items().end(), [](const records::Event& e) { return e.context.has_value(); });
        check(event != app.events().items().end() && event->failure && event->context->status == "security" &&
            event->context->recovery.paused && event->context->recovery.cooldown_remaining &&
            std::find(event->context->recovery.hold_reasons.begin(), event->context->recovery.hold_reasons.end(), HoldReason::security) != event->context->recovery.hold_reasons.end() &&
            event->context->physical_network && event->context->physical_network->interface_count == 3 &&
            event->context->physical_network->candidate_count == 1 && event->context->physical_collected_at &&
            *event->context->physical_collected_at <= event->context->collected_at && event->time == event->context->collected_at,
            "authentication preflight failure retains its actual protected result, observed physical diagnostics and separate collection time");
        fake->online = true; app.resume(); app.check();
        failurecontextruntime::drain(app, [&] { return !app.snapshot().busy && app.snapshot().online; });
        check(app.snapshot().online && failurecontextruntime::contextual_events(app) == saved_failure &&
            saved_failure.find("operationResult") != std::string::npos && saved_failure.find("missingDNS") != std::string::npos &&
            saved_failure.find("security") != std::string::npos && fake->attempts == 1,
            "restored connectivity does not overwrite or remove the original authentication failure scene from later JSON exports");
        app.shutdown();
        check(fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 && fake->evidence().production_storage_accesses == 0 &&
            fake->evidence().fake_auth_submissions == 0, "failure-scene collection changes no real network, credentials or authentication protections");
    }
    for (const bool remember : {false, true}) {
        auto services = std::make_unique<Fake>(); auto* fake = services.get(); fake->confirmed = false;
        AppController app(nullptr, RuntimeMode::normal, std::move(services), [](UINT) { return true; }); app.start();
        check(remember ? app.remember_network() : app.verify_campus_service(), "isolated service or remember operation admits its own independent verification");
        failurecontextruntime::drain(app, [&] { return !app.snapshot().busy && app.next_check() && failurecontextruntime::context_count(app) == 1; });
        const auto saved_failure = failurecontextruntime::contextual_events(app);
        check(saved_failure.find("campusVerification") != std::string::npos && saved_failure.find("dnsTls") != std::string::npos &&
            saved_failure.find("unconfirmed") != std::string::npos && app.health().items().empty() && fake->attempts == 0,
            "independent verification failures create a fixed anonymous failure scene even when no connectivity sample or login exists");
        fake->confirmed = true;
        check(remember ? app.remember_network() : app.verify_campus_service(), "verification can later succeed through its normal protected operation");
        failurecontextruntime::drain(app, [&] { return !app.snapshot().busy && app.next_check() &&
            app.memory_verification_message() == (remember ? "memory.verification.remembered" : "campus.verify.confirmed"); });
        check(failurecontextruntime::contextual_events(app) == saved_failure && fake->attempts == 0 && fake->probes == 0,
            "a successful later verification preserves the earlier issue and environment instead of replacing it with the healthy scene");
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get(); fake->block = true; fake->confirmed = false;
        AppController app(nullptr, RuntimeMode::normal, std::move(services), [](UINT) { return true; }); app.start(); app.connect();
        failurecontextruntime::drain(app, [&] { return fake->discoveries == 1; });
        const auto epoch = app.snapshot().epoch;
        for (int signal = 0; signal < 3; ++signal) { app.on_network_signal(); app.on_timer(kTimerNetwork); }
        check(app.snapshot().epoch == epoch && app.snapshot().busy,
            "repeated TUN notifications and local snapshot refresh retain ownership of the same physical discovery");
        app.on_timer(kTimerAuto); fake->released = true;
        failurecontextruntime::drain(app, [&] { return !app.snapshot().busy && app.next_check() && failurecontextruntime::context_count(app) == 1; });
        const auto scene = failurecontextruntime::contextual_events(app);
        check(failurecontextruntime::context_count(app) == 1 && scene.find("dnsTls") != std::string::npos &&
            scene.find("detected") != std::string::npos && app.snapshot().epoch == epoch && fake->attempts == 0,
            "same-attachment TUN notification churn cannot suppress a genuine failed verification scene");
        app.shutdown();
    }
    for (const std::string interruption : {"cancel", "network", "delete", "pause", "forget"}) {
        auto services = std::make_unique<Fake>(); auto* fake = services.get(); fake->block = true; fake->confirmed = false;
        AppController app(nullptr, RuntimeMode::normal, std::move(services), [](UINT) { return true; }); app.start(); app.connect();
        failurecontextruntime::drain(app, [&] { return fake->discoveries == 1; });
        app.on_timer(kTimerAuto);
        if (interruption == "cancel") app.cancel();
        else if (interruption == "network") { fake->changed = true; app.refresh_environment(); }
        else if (interruption == "delete") app.delete_account();
        else if (interruption == "pause") app.pause();
        else app.forget_all_networks();
        fake->released = true;
        failurecontextruntime::drain(app, [&] { return fake->returned == 1 && !app.snapshot().busy && app.next_check(); });
        check(app.next_check() && failurecontextruntime::context_count(app) == 0 && fake->attempts == 0,
            "cancel, changed attachment, deleted account, new user pause or forgotten memory cannot turn a late result into a new failure scene");
        app.shutdown();
    }
}
