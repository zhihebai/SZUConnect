#include <algorithm>
#include <string>
#include <vector>

inline void recovery_latency_tests(Harness& t) {
    using namespace wiredtest;
    t.begin("recovery-monitoring-cadence");
    for (const std::string restriction : {"none", "readOnly", "observe", "paused", "account", "memory", "constrained"}) {
        Fixture f(CampusArea::automatic, "dormitory", restriction == "readOnly", restriction != "memory", restriction != "account");
        f.service.response = online();
        if (restriction == "observe") f.engine->set_mode(MonitoringMode::observe);
        if (restriction == "paused") f.engine->pause_auto_login();
        if (restriction == "constrained") { auto path = f.path(); path.constrained = true; f.engine->receive_path(path); }
        for (int round = 0; round < 4; ++round) f.round();
        const int delay = f.engine->next_check_delay();
        t.check(restriction == "none" ? delay > 0 && delay <= 15 : delay >= 30,
            "short monitoring cadence is restricted to eligible unconstrained automatic recovery: " + restriction);
        t.check(f.service.submissions.empty(), "healthy monitoring never authenticates: " + restriction);
    }

    t.begin("virtual-timer-quick-session-loss-latency");
    {
        Fixture f;
        f.engine->set_probe_mode(ProbeMode::quick);
        f.service.cancel_quick_peer = true;
        f.service.response = online();
        f.engine->automatic_round();
        const auto epoch = f.engine->snapshot().epoch;
        const double expired_at = f.clock;
        f.service.response = hint(CampusPortalHint::dormitory);
        f.service.response.apple.state = EndpointState::success;
        for (int round = 0; round < 5 && f.service.submissions.empty(); ++round) {
            const double delay = std::max(1, f.engine->next_check_delay());
            f.clock += delay; f.wall += delay;
            f.engine->automatic_round();
        }
        t.check(f.clock - expired_at <= 17 && f.service.submissions == std::vector<CampusArea>{CampusArea::dormitory},
            "a timer-driven base audit and independent confirmation recover a masked expired session within 17 virtual seconds");
        t.check(f.engine->snapshot().epoch == epoch && f.engine->snapshot().online && f.service.fixture_credentials,
            "the virtual recovery keeps its physical epoch, uses fake credentials and verifies the result");
        t.check(std::count(f.service.probe_modes.begin(), f.service.probe_modes.end(), ProbeMode::complete) >= 2 &&
            std::count(f.service.deferred_website_scopes.begin(), f.service.deferred_website_scopes.end(), true) >= 2,
            "quick monitoring still reads both fixed probes for confirmation without waiting for custom websites");
    }

    t.begin("cooldown-deadline-scheduling");
    {
        ConnectionPolicy policy;
        policy.attempt_started(100);
        t.check(policy.next_check_delay(false, 183.5, false) == 7 &&
            policy.next_check_delay(false, 189.7, false) == 1,
            "the next check rounds up the remaining cooldown instead of adding a fixed fifteen-second wait");
        t.check(policy.cooling_down(189.999) && !policy.cooling_down(190),
            "deadline-aware scheduling cannot shorten the ninety-second credential protection");
    }
    {
        Fixture f;
        f.engine->restore_recovery(std::string("{\"schema\":1,\"generation\":1,\"holds\":[],\"notBefore\":") +
            std::to_string(f.wall + 7) + "}");
        f.service.response = hint(CampusPortalHint::dormitory);
        f.service.response.apple.state = EndpointState::success;
        f.engine->automatic_round(); f.clock += 1; f.wall += 1; f.engine->automatic_round();
        const auto state = f.engine->snapshot();
        t.check(state.status == EngineStatus::coolingDown && state.auto_login == AutoLoginState::coolingDown &&
            state.cooldown_remaining && *state.cooldown_remaining > 5 && *state.cooldown_remaining <= 6 &&
            state.next_check_seconds == 6 && f.service.submissions.empty(),
            "a partial public success cannot hide the active cooldown or postpone its next check");
        f.clock += 6; f.wall += 6; f.engine->automatic_round();
        t.check(f.service.submissions.size() == 1 && f.engine->snapshot().online,
            "confirmed session loss can recover on the first check at the protected deadline");
    }
    t.begin("live-eligibility-after-discovery");
    for (const bool client_bound : {false, true}) {
        Fixture f(CampusArea::automatic,"teaching");
        f.service.response = ConnectivityReport::simple(false,false);
        f.service.proof.teaching_client_bound = client_bound;
        f.round();
        const auto original = f.saved();
        f.service.during_discovery = [&] { f.wall += NetworkMemory::kExpirySeconds + 1; };
        f.round();
        t.check(f.service.submissions.empty() && f.service.attempts.empty() &&
            f.engine->snapshot().status == EngineStatus::needsTrust && f.saved().verified == original.verified,
            "a fresh teaching CONFIG or remembered preference cannot renew eligibility that expired during discovery");
    }
}
