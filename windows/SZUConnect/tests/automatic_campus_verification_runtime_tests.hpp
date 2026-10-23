#pragma once
#include "campus_verification_runtime_tests.hpp"
#include <condition_variable>
#include <mutex>

namespace automaticcampusruntime {
class Fake final : public campusverificationruntime::Fake {
public:
    Fake() : campusverificationruntime::Fake("recover") {}
    szu::CampusDiscovery discover(szu::CampusArea, const szu::CancelToken& token) override {
        ++discoveries;
        {
            std::unique_lock lock(gate_mutex);
            active_token = token; entered = true; gate_changed.notify_all();
            if (hold && !gate_changed.wait_for(lock, std::chrono::seconds(5), [&] { return released_gate; })) timed_out = true;
        }
        ++returned;
        // Intentionally publish success even after cancellation; ownership is enforced by the engine.
        return {confirmed ? szu::Discovery::confirmed : szu::Discovery::unconfirmed,
                confirmed ? szu::Discovery::confirmed : szu::Discovery::unconfirmed};
    }
    bool wait() {
        std::unique_lock lock(gate_mutex);
        return gate_changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
    }
    void release() { std::lock_guard lock(gate_mutex); released_gate = true; gate_changed.notify_all(); }
    void arm() { std::lock_guard lock(gate_mutex); hold = true; entered = released_gate = false; }
    bool await_cancellation() {
        std::optional<szu::CancelToken> token;
        { std::lock_guard lock(gate_mutex); token = active_token; }
        return token && token->wait_for(2);
    }
    bool hold = false;
    std::atomic<bool> timed_out{false};
private:
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    std::optional<szu::CancelToken> active_token;
    bool entered = false, released_gate = false;
};
inline void drain(szu::app::AppController& app, const std::function<bool()>& done) {
    wifiruntime::wait_for([&] { app.on_changed(); return done(); });
    app.on_changed();
}
}

template<class Check> void run_automatic_campus_verification_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    for (const bool confirmed : {true, false}) {
        auto services = std::make_unique<automaticcampusruntime::Fake>(); auto* fake = services.get();
        fake->hold = true; fake->confirmed = confirmed;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto memory = fake->files[L"network-memory.json"], labels = fake->files[L"network-labels.json"];
        check(status::campus_verification_hint_key(app.snapshot()) == "campus.verify.autoPendingHint",
            "remembered recover-mode controller explains that its next healthy check will verify campus services automatically");
        app.check(); const bool entered = fake->wait();
        check(entered, "a normal recover-mode controller reaches the actual automatically nested service stage");
        if (entered) {
            app.on_changed(); const auto running = app.snapshot(); const auto ready = status::recovery_readiness(running);
            check(running.online && running.verifying_campus && running.auto_verifying_campus &&
                app.memory_verification_message() == "campus.verify.checking" && ready.title == "ready.title.checking" &&
                ready.tone == "busy" && status::campus_verification_key(running) == "env.campusVerification.checking",
                "engine state reaches Controller feedback and presenter as campus verification in progress while internet remains online");
            check(status::campus_verification_hint_key(running) == "campus.verify.checking" &&
                running.busy_kind == OperationKind::manual && fake->evidence().fake_auth_submissions == 0,
                "automatic confirmation remains owned by the original manual connectivity check and never presents it as credential recovery");
        }
        fake->release();
        automaticcampusruntime::drain(app, [&] { return fake->returned == 1 && !app.snapshot().busy && app.last_checked(); });
        const auto state = app.snapshot(); const auto ready = status::recovery_readiness(state);
        if (confirmed) {
            check(state.online && state.required_campus_confirmed && !state.auto_verifying_campus &&
                app.memory_verification_message().empty() && status::campus_verification_key(state) == "env.campusVerification.confirmed" &&
                status::campus_verification_hint_key(state) == "campus.verify.confirmedHint",
                "successful automatic confirmation clears transient feedback and exposes the actual verified service state through the presenter");
        } else {
            check(state.online && !state.required_campus_confirmed && state.automatic_campus_verification_failed &&
                state.automatic_campus_verification_pending && app.memory_verification_message() == "campus.verify.autoFailed" &&
                status::campus_verification_hint_key(state) == "campus.verify.autoFailed" && ready.title == "ready.title.verificationFailed" &&
                ready.tone == "warning", "failed automatic confirmation preserves online internet and displays a non-green automatic retry explanation");
        }
        check(!fake->timed_out && fake->files[L"network-memory.json"] == memory && fake->files[L"network-labels.json"] == labels &&
            app.network_name(fake->id) == "Fixture laboratory cable" && !state.trusted &&
            fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0,
            "the Controller automatic stage preserves remembered proof, expiry, local remark and trust with zero fake authentication or Wi-Fi actions");
        const int probes = fake->probes, discoveries = fake->discoveries;
        app.set_probe(ProbeMode::complete); app.set_probe(ProbeMode::quick);
        check(fake->probes == probes && fake->discoveries == discoveries,
            "changing the next scope does not issue or retry an automatic campus verification");
        app.check();
        automaticcampusruntime::drain(app, [&] { return fake->probes > probes && !app.snapshot().busy; });
        check(fake->discoveries == discoveries && fake->evidence().fake_auth_submissions == 0,
            "another immediate Controller check respects success deduplication or failed-request backoff without launching credential recovery");
        app.shutdown();
        check(fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 &&
            fake->evidence().production_storage_accesses == 0 && fake->evidence().watchers_started == 0,
            "automatic feedback integration uses injected storage, credentials and network services only");
    }
    {
        auto services = std::make_unique<automaticcampusruntime::Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(app.remember_network(), "saved-memory feedback fixture performs a genuine isolated remember operation");
        automaticcampusruntime::drain(app, [&] { return fake->returned == 1 && !app.snapshot().busy &&
            app.memory_verification_message() == "memory.verification.remembered"; });
        check(app.memory_verification_message() == "memory.verification.remembered",
            "a successful explicit remember exposes its own completed feedback before the automatic round");
        const auto memory = fake->files[L"network-memory.json"];
        app.set_mode(MonitoringMode::manual); app.set_mode(MonitoringMode::recover);
        fake->confirmed = false; fake->arm(); app.check(); const bool entered = fake->wait();
        check(entered && app.memory_verification_message() == "campus.verify.checking",
            "automatic checking replaces stale remember-success feedback instead of leaving the old success over the new work");
        fake->release(); automaticcampusruntime::drain(app, [&] { return fake->returned == 2 && !app.snapshot().busy; });
        check(!fake->timed_out && app.memory_verification_message() == "campus.verify.autoFailed" &&
            fake->files[L"network-memory.json"] == memory && fake->evidence().fake_auth_submissions == 0,
            "automatic failure and retry feedback cannot be concealed by a previous remember-success message");
        app.shutdown();
    }
    for (const auto next : {MonitoringMode::manual, MonitoringMode::observe}) {
        auto services = std::make_unique<automaticcampusruntime::Fake>(); auto* fake = services.get(); fake->hold = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto memory = fake->files[L"network-memory.json"];
        app.check(); const bool entered = fake->wait(); check(entered, "manual-owned automatic Controller operation reaches its cancellation barrier");
        if (entered) {
            app.set_mode(next);
            check(!app.snapshot().busy && !app.snapshot().verifying_campus && app.memory_verification_message().empty(),
                "changing monitoring mode immediately clears a manual-owned automatic service operation and its checking feedback");
        }
        fake->release(); automaticcampusruntime::drain(app, [&] { return fake->returned == 1 && !app.snapshot().busy; });
        check(!fake->timed_out && !app.snapshot().last_discovery && !app.snapshot().required_campus_confirmed &&
            app.memory_verification_message().empty() && fake->files[L"network-memory.json"] == memory &&
            fake->evidence().fake_auth_submissions == 0,
            "late Controller success cannot revive automatically nested proof or feedback after leaving recover mode");
        app.shutdown();
    }
    {
        auto services = std::make_unique<automaticcampusruntime::Fake>(); auto* fake = services.get(); fake->hold = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto memory = fake->files[L"network-memory.json"];
        app.check(); const bool entered = fake->wait(); check(entered, "Controller cancel fixture reaches its automatically nested campus verification");
        if (entered) {
            app.cancel();
            check(!app.snapshot().busy && !app.snapshot().verifying_campus &&
                app.memory_verification_message() == "campus.verify.cancelled",
                "cancelling automatic campus verification uses campus-service feedback rather than memory-save cancellation feedback");
        }
        fake->release(); automaticcampusruntime::drain(app, [&] { return fake->returned == 1 && !app.snapshot().busy; });
        check(!fake->timed_out && !app.snapshot().last_discovery && !app.snapshot().required_campus_confirmed &&
            (app.memory_verification_message().empty() || app.memory_verification_message() == "campus.verify.cancelled") &&
            fake->files[L"network-memory.json"] == memory && fake->evidence().fake_auth_submissions == 0,
            "late automatic confirmation cannot overwrite the cancelled Controller feedback or mutate network memory");
        app.shutdown();
    }
    {
        auto services = std::make_unique<automaticcampusruntime::Fake>(); auto* fake = services.get(); fake->hold = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto memory = fake->files[L"network-memory.json"];
        app.check(); const bool entered = fake->wait(); check(entered, "maintenance shutdown fixture reaches its owned campus verification");
        std::atomic<bool> cancelled{false};
        std::thread release_after_cancel([&] { cancelled = fake->await_cancellation(); fake->release(); });
        app.shutdown(); release_after_cancel.join(); app.on_changed();
        check(cancelled && !fake->timed_out && !app.snapshot().busy && !app.snapshot().verifying_campus &&
            !app.snapshot().last_discovery && !app.snapshot().required_campus_confirmed &&
            app.memory_verification_message().empty() && fake->files[L"network-memory.json"] == memory &&
            fake->evidence().fake_auth_submissions == 0,
            "maintenance shutdown cancels the real service token, waits for its late result and never publishes stale automatic proof or feedback");
    }
    {
        auto services = std::make_unique<automaticcampusruntime::Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services)); app.start(); const auto files = fake->files;
        app.check(); automaticcampusruntime::drain(app, [&] { return fake->probes > 0 && !app.snapshot().busy; });
        check(fake->probes > 0 && fake->discoveries == 0 && fake->credential_reads == 0 && fake->files == files &&
            !app.snapshot().auto_verifying_campus && !app.snapshot().automatic_campus_verification_failed &&
            fake->evidence().fake_auth_submissions == 0,
            "read-only Controller can inspect connectivity without automatic campus discovery, credential reads or formal data writes");
        app.shutdown();
    }
}
