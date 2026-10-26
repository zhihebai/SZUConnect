#pragma once
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace automaticcampustest {
using r11enginetest::Gate;
using r11enginetest::Service;
using r11enginetest::connectivity;
using r11enginetest::hinted;

struct Fixture {
    std::atomic<double> clock{1000}, wall{1800000000};
    Service service;
    physical::Route wire = r11enginetest::route("automatic-campus-wire", "ethernet", "1");
    physical::Route other = r11enginetest::route("automatic-campus-other", "wifi", "2");
    std::unique_ptr<ConnectionEngine> engine;
    std::string memory_bytes;
    int writes = 0;
    explicit Fixture(CampusArea area = CampusArea::automatic, std::string scope = "teaching",
                     MonitoringMode mode = MonitoringMode::recover, bool readonly = false, bool remembered = true) {
        wire.is_system_primary = true;
        service.reports[wire.interface_key] = service.reports[other.interface_key] = connectivity(true);
        service.proofs[wire.interface_key] = service.proofs[other.interface_key] = {Discovery::confirmed, Discovery::confirmed};
        ConnectionEngine::Hooks hooks;
        hooks.now = [this] { return clock.load(); }; hooks.wall_now = [this] { return wall.load(); };
        hooks.sleep = [](double) {}; hooks.persist_recovery = [](const std::string&) { return true; };
        hooks.random_hex = [] { return "00112233445566778899aabbccddeeff"; };
        hooks.persist_memory = [this](const std::string& value) { ++writes; memory_bytes = value; return true; };
        engine = std::make_unique<ConnectionEngine>(service, hooks, readonly);
        engine->restore_recovery(std::nullopt);
        engine->configure(mode, area, ProbeMode::complete, Credentials{"r11-fixture-account", "r11-fixture-password"});
        NetworkMemory memory("00112233445566778899aabbccddeeff");
        if (remembered) {
            memory.remember(wire.descriptor, scope, true, scope == "dormitory", wall - 100, "abcde101");
            memory.remember(other.descriptor, scope, true, scope == "dormitory", wall - 100, "abcde102");
        }
        memory_bytes = memory.encode(); engine->restore_memory(memory_bytes); engine->receive_path(path());
        advance(46); // Leave the attachment's intentionally quick initial observation window.
    }
    void advance(double seconds) { clock.fetch_add(seconds); wall.fetch_add(seconds); }
    PathSnapshot path(bool dual = false) const {
        PathSnapshot p; p.usable = p.link_active = p.requires_physical_binding = true;
        p.identity = "automatic-campus-fixture-attachment"; p.interfaces = "Fixture attachments";
        p.physical_candidates = dual ? std::vector<physical::Route>{wire, other} : std::vector<physical::Route>{wire};
        for (const auto& route : p.physical_candidates) p.descriptors.push_back(route.descriptor);
        return p;
    }
    void round() { engine->automatic_round(); }
    ConnectionEngine::CampusVerificationResult verify() {
        const auto request = engine->prepare_verify_campus();
        return request ? engine->verify_campus(*request) : ConnectionEngine::CampusVerificationResult::unavailable;
    }
};
}

inline void automatic_campus_verification_refresh_tests(Harness& t) {
    using namespace automaticcampustest;
    t.begin("automatic-campus-verification-periodic-refresh");
    {
        Fixture f; const auto memory = f.memory_bytes;
        f.round(); const auto delay = f.engine->next_check_delay();
        f.advance(899); f.round();
        t.check(f.service.discoveries.size() == 1 && f.engine->snapshot().required_campus_confirmed,
            "same-context successful verification remains deduplicated through 899 monotonic seconds");
        f.wall.fetch_add(86400); f.round();
        t.check(f.service.discoveries.size() == 1,
            "a wall-clock correction cannot advance the monotonic service-refresh deadline");
        f.advance(1); f.round();
        t.check(f.service.discoveries.size() == 2 && f.engine->snapshot().required_campus_confirmed &&
            f.service.submissions.empty() && f.writes == 0 && f.memory_bytes == memory && !f.engine->snapshot().trusted,
            "the next ordinary round at 900 seconds refreshes service proof without credentials, trust or persistent-memory changes");
        t.check(delay > 0 && f.engine->next_check_delay() > 0,
            "service refresh does not install a separate fast authentication schedule");
    }
    {
        Fixture f; const auto memory = f.memory_bytes;
        f.service.during_discovery = [&](const std::string&) { f.advance(17); };
        f.round(); f.service.during_discovery = {};
        f.advance(899); f.round();
        t.check(f.service.discoveries.size() == 1,
            "successful verification freshness starts at completion rather than consuming its own transport duration");
        f.advance(1); f.round();
        t.check(f.service.discoveries.size() == 2 && f.memory_bytes == memory && f.writes == 0 && f.service.submissions.empty(),
            "a slow successful verification becomes refreshable exactly 900 seconds after completion");
    }
    {
        Fixture f; const auto memory = f.memory_bytes;
        f.round(); f.advance(900);
        f.service.proofs[f.wire.interface_key] = {Discovery::unconfirmed, Discovery::notChecked};
        f.service.during_discovery = [&](const std::string&) { f.advance(6); };
        f.round(); f.service.during_discovery = {};
        const auto failed = f.engine->snapshot();
        t.check(f.service.discoveries.size() == 2 && failed.online && !failed.required_campus_confirmed &&
            failed.automatic_campus_verification_failed && failed.remembered && failed.holds.empty(),
            "a periodic refresh failure preserves connectivity and memory while exposing its failed service result");
        f.service.proofs[f.wire.interface_key] = {Discovery::confirmed, Discovery::confirmed};
        f.advance(29); f.round();
        t.check(f.service.discoveries.size() == 2,
            "a failed periodic refresh retains thirty full seconds of completion-based retry backoff");
        f.advance(1); f.round();
        t.check(f.service.discoveries.size() == 3 && f.engine->snapshot().required_campus_confirmed &&
            !f.engine->snapshot().automatic_campus_verification_failed && f.service.submissions.empty() &&
            f.writes == 0 && f.memory_bytes == memory,
            "an ordinary due retry can restore periodic verification without submitting credentials or extending remembered proof");
    }
    {
        Fixture f; const auto memory = f.memory_bytes;
        t.check(f.verify() == ConnectionEngine::CampusVerificationResult::confirmed,
            "explicit service verification initializes the same bounded freshness interval");
        f.advance(899); f.round();
        t.check(f.service.discoveries.size() == 1,
            "ordinary monitoring deduplicates a recent explicit service verification");
        f.advance(1); f.round();
        t.check(f.service.discoveries.size() == 2 && f.memory_bytes == memory && f.writes == 0 && f.service.submissions.empty(),
            "explicit service verification also refreshes after fifteen minutes without rewriting memory");
    }
    {
        Fixture f; f.round(); f.advance(900);
        const auto memory = f.memory_bytes;
        Gate gate; f.service.during_discovery = [&](const std::string&) { gate.block(); };
        std::thread worker([&] { f.round(); });
        const bool entered = gate.wait();
        t.check(entered, "a periodic refresh enters the cancellable independent-service stage");
        if (entered) f.engine->cancel();
        gate.release(); worker.join();
        t.check(!gate.timed_out && !f.engine->snapshot().required_campus_confirmed &&
            !f.engine->snapshot().verifying_campus && f.memory_bytes == memory && f.writes == 0 && f.service.submissions.empty(),
            "a cancelled periodic refresh cannot republish old success or its late response");
        f.service.during_discovery = {}; f.advance(60); f.round();
        t.check(f.service.discoveries.size() == 3 && f.engine->snapshot().required_campus_confirmed,
            "cancellation clears the old freshness interval so a later normal round obtains new service proof");
    }
    for (const auto next : {MonitoringMode::manual, MonitoringMode::observe}) {
        Fixture f; const auto memory = f.memory_bytes; f.round();
        f.engine->set_mode(next); f.advance(1800); f.round();
        t.check(f.service.discoveries.size() == 1 && f.service.submissions.empty() && f.memory_bytes == memory && f.writes == 0,
            "elapsed service-refresh time cannot start automatic verification outside automatic-recovery mode");
    }
}

inline void automatic_campus_verification_tests(Harness& t) {
    using namespace automaticcampustest;
    automatic_campus_verification_refresh_tests(t);
    t.begin("automatic-campus-verification-success-and-context");
    for (const auto area : {CampusArea::teaching, CampusArea::dormitory}) {
        Fixture f(area, std::string(to_string(area))); const auto memory = f.memory_bytes;
        f.round(); const auto state = f.engine->snapshot();
        t.check(state.online && state.required_campus_confirmed && !state.busy && !state.verifying_campus,
            "a healthy remembered teaching or dormitory attachment obtains current service proof in its own round");
        t.check(f.service.discoveries.size() == 1 && f.service.submissions.empty() && !state.trusted &&
            f.memory_bytes == memory && f.writes == 0, "automatic campus confirmation neither submits, grants trust nor rewrites remembered proof or expiry");
        f.advance(60); f.round(); f.round();
        t.check(f.service.discoveries.size() == 1 && f.service.submissions.empty(),
            "same-context successful proof is reused for display across later healthy monitoring rounds");
    }
    {
        Fixture f; const auto bytes = f.memory_bytes;
        t.check(f.engine->check() && f.engine->snapshot().required_campus_confirmed && f.service.discoveries.size() == 1,
            "manual connectivity checking in recover mode also confirms a remembered healthy attachment");
        t.check(f.service.submissions.empty() && f.writes == 0 && f.memory_bytes == bytes && !f.engine->snapshot().trusted,
            "a manual-owned automatic verification stage is still credential-free and cannot grant trust");
    }
    {
        Fixture f;
        t.check(f.verify() == ConnectionEngine::CampusVerificationResult::confirmed, "explicit service verification establishes valid same-context proof");
        f.round(); t.check(f.service.discoveries.size() == 1, "an explicit successful service verification participates in automatic deduplication");
        f.engine->set_area(CampusArea::dormitory);
        t.check(!f.engine->snapshot().required_campus_confirmed, "changing selected service invalidates old proof immediately");
        f.service.proofs[f.wire.interface_key].dormitory = Discovery::unconfirmed;
        f.round(); t.check(f.service.discoveries.size() == 2 && !f.engine->snapshot().required_campus_confirmed,
            "a changed area gets a fresh required-service verification rather than borrowing teaching proof");
    }
    {
        Fixture f; f.round();
        const auto before = f.service.discoveries.size(); f.engine->restore_memory(f.memory_bytes); f.round();
        t.check(f.service.discoveries.size() == before + 1, "a memory generation change cannot retain the old context's successful deduplication");
    }
    {
        Fixture f;
        f.service.during_probe = [&](const std::string&) { f.engine->restore_memory(f.memory_bytes); };
        f.round();
        t.check(f.service.discoveries.empty() && !f.engine->snapshot().required_campus_confirmed,
            "a memory generation changed during the internet probe cannot lend its authority to the already-started round");
        f.service.during_probe = {}; f.round();
        t.check(f.service.discoveries.size() == 1 && f.engine->snapshot().required_campus_confirmed && f.service.submissions.empty(),
            "a subsequent normal round can independently verify the replacement memory context");
    }
    {
        Fixture f; f.round(); const auto epoch = f.engine->snapshot().epoch;
        auto detached = f.path(); detached.usable = detached.link_active = false;
        detached.physical_candidates.clear(); detached.descriptors.clear(); f.engine->receive_path(detached);
        f.engine->receive_path(f.path());
        t.check(f.engine->snapshot().epoch != epoch && !f.engine->snapshot().required_campus_confirmed && !f.engine->snapshot().online,
            "a same-identity physical disconnect and reconnect drops the previous online and service proof");
        f.round(); t.check(f.service.discoveries.size() == 2 && f.engine->snapshot().required_campus_confirmed,
            "reattaching the same remembered cable performs fresh credential-free campus confirmation");
    }
    {
        Fixture f; f.round();
        f.wire.identity = "automatic-campus-fixture-new-lease";
        f.engine->receive_path(f.path());
        t.check(!f.engine->snapshot().required_campus_confirmed, "a new physical route lease cannot reuse old proof despite identical remembered features");
        f.round(); t.check(f.service.discoveries.size() == 2, "the new route lease receives its own service request");
    }
    {
        Fixture f; f.round();
        auto dual = f.path(true); dual.physical_candidates[0].is_system_primary = false; dual.physical_candidates[1].is_system_primary = true;
        f.engine->receive_path(dual); f.round();
        t.check(f.service.discoveries == std::vector<std::string>{f.wire.interface_key, f.other.interface_key} &&
            f.engine->snapshot().authentication_route->interface_key == f.other.interface_key && f.engine->snapshot().required_campus_confirmed,
            "a remembered dual-interface handoff probes and verifies the newly selected interface independently");
        t.check(f.service.submissions.empty() && f.writes == 0, "dual-interface service confirmation is not an authentication or memory update");
    }

    t.begin("automatic-campus-verification-backoff-and-health");
    {
        Fixture f; f.service.proofs[f.wire.interface_key] = {Discovery::unconfirmed, Discovery::unconfirmed};
        f.service.during_discovery = [&](const std::string&) { f.advance(17); };
        f.round(); const auto failed = f.engine->snapshot();
        t.check(failed.online && !failed.required_campus_confirmed && failed.automatic_campus_verification_failed &&
            failed.automatic_campus_verification_pending && !failed.busy,
            "automatic proof failure preserves the healthy internet result and exposes pending retry instead of green readiness");
        f.service.during_discovery = {}; f.service.proofs[f.wire.interface_key] = {Discovery::confirmed, Discovery::confirmed};
        f.round(); f.advance(29); f.round();
        t.check(f.service.discoveries.size() == 1 && f.engine->snapshot().online,
            "failure backoff starts at request completion and rejects retries before thirty full seconds");
        f.advance(1); f.round();
        t.check(f.service.discoveries.size() == 2 && f.engine->snapshot().required_campus_confirmed &&
            !f.engine->snapshot().automatic_campus_verification_failed && !f.engine->snapshot().automatic_campus_verification_pending,
            "the next ordinary monitoring round can retry at the thirty-second completion boundary");
        t.check(f.service.submissions.empty() && f.writes == 0, "failure and retry remain credential-free with unchanged persistent memory");
    }
    {
        Fixture f; f.engine->set_probe_mode(ProbeMode::quick);
        auto report = connectivity(true); report.microsoft.state = EndpointState::notCompleted;
        f.service.reports[f.wire.interface_key] = report; f.round();
        const auto state = f.engine->snapshot();
        t.check(state.required_campus_confirmed && f.service.discoveries.size() == 1 && state.last_report &&
            state.last_report->mode == ProbeMode::quick && state.last_report->microsoft.state == EndpointState::notCompleted,
            "quick success with a genuinely skipped second probe is healthy without fabricating a second success");
    }
    for (const auto failure : {EndpointState::timeout, EndpointState::dns, EndpointState::tls, EndpointState::redirect,
                              EndpointState::network, EndpointState::unexpectedResponse, EndpointState::cancelled}) {
        Fixture f; f.engine->set_probe_mode(ProbeMode::quick);
        auto report = connectivity(true); report.microsoft.state = failure; f.service.reports[f.wire.interface_key] = report;
        f.round(); t.check(f.service.discoveries.empty() && !f.engine->snapshot().required_campus_confirmed && f.service.submissions.empty(),
            "a real quick probe failure cannot masquerade as a skipped probe or start healthy-path service confirmation");
    }
    {
        Fixture f; auto report = connectivity(true); report.microsoft.state = EndpointState::timeout;
        f.service.reports[f.wire.interface_key] = report; f.round();
        t.check(f.service.discoveries.empty() && f.service.submissions.empty() && f.engine->snapshot().last_report->recoverable_partial(),
            "a complete partial first observation keeps the existing recovery threshold rather than taking the healthy auto-verification path");
        f.advance(6); f.round();
        t.check(f.service.discoveries.empty() && f.service.submissions.empty() && f.engine->snapshot().online,
            "repeated single-endpoint timeouts cannot prove campus-session expiry or force an automatic credential submission");
    }

    t.begin("automatic-campus-verification-eligibility-and-protection");
    for (const auto mode : {MonitoringMode::manual, MonitoringMode::observe}) {
        Fixture f(CampusArea::teaching, "teaching", mode); f.engine->check();
        t.check(f.engine->snapshot().online && f.service.discoveries.empty() && f.service.submissions.empty(),
            "manual and periodic-only modes do not add automatic campus-service discovery to a healthy check");
    }
    for (int excluded : {0, 1, 2, 3}) {
        Fixture f(CampusArea::teaching, "teaching", MonitoringMode::recover, excluded == 0, excluded != 1 && excluded != 2);
        if (excluded == 2) t.check(f.engine->trust_current_network() && f.engine->snapshot().trusted && !f.engine->snapshot().remembered,
            "the session-only fixture has actual temporary trust but no persistent campus-network qualification");
        if (excluded == 3) t.check(!f.engine->restore_memory("{\"schema\":999}") && f.engine->snapshot().memory_unreadable,
            "the malformed-memory fixture retains an explicit unreadable state before its healthy connectivity check");
        const auto bytes = f.memory_bytes; f.engine->check();
        t.check(f.service.discoveries.empty() && f.service.submissions.empty() && f.writes == 0 && f.memory_bytes == bytes,
            "read-only, unknown, session-trust-only and unreadable-memory cases never enter the automatic service stage");
    }
    for (const auto hold : {HoldReason::userPaused, HoldReason::credentials, HoldReason::security, HoldReason::uncertain}) {
        Fixture f;
        f.engine->set_probe_mode(ProbeMode::quick);
        auto report = connectivity(true); report.microsoft.state = EndpointState::notCompleted;
        f.service.reports[f.wire.interface_key] = report;
        const auto journal = "{\"schema\":1,\"generation\":1,\"holds\":[{\"reason\":" +
            std::to_string(static_cast<int>(hold)) + ",\"origin\":2}],\"notBefore\":" + std::to_string(f.wall.load() + 60) + "}";
        EngineSnapshot before;
        if (hold == HoldReason::uncertain) {
            // A complete connectivity recheck may settle an old uncertain result before this stage.
            // Install its independent protection after the healthy probe to isolate service-proof effects.
            f.service.during_discovery = [&](const std::string&) { f.engine->restore_recovery(journal); before = f.engine->snapshot(); };
        } else { f.engine->restore_recovery(journal); before = f.engine->snapshot(); }
        f.round(); const auto after = f.engine->snapshot();
        t.check(after.online && after.required_campus_confirmed && after.paused && after.pause_reason == hold &&
            after.holds.size() == before.holds.size() && after.cooldown_remaining == before.cooldown_remaining,
            "harmless service confirmation preserves user, password, security and uncertain-result holds plus their original cooldown");
        t.check(f.service.submissions.empty() && !after.trusted && f.writes == 0, "read-only service proof cannot clear authentication protection or grant trust");
    }
    {
        Fixture f(CampusArea::dormitory, "dormitory");
        f.service.proofs[f.wire.interface_key] = {Discovery::confirmed, Discovery::unconfirmed}; f.round();
        const auto state = f.engine->snapshot();
        t.check(state.last_discovery && state.last_discovery->teaching == Discovery::confirmed && !state.required_campus_confirmed &&
            state.required_campus_verification == Discovery::unconfirmed && state.automatic_campus_verification_failed,
            "raw teaching HTTPS identity is retained separately and cannot satisfy a required dormitory proof");
    }
    {
        Fixture f; auto report = connectivity(false);
        report.apple.state = report.microsoft.state = EndpointState::redirect;
        report.apple.campus_area_hint = CampusPortalHint::teaching; report.microsoft.campus_area_hint = CampusPortalHint::dormitory;
        f.service.reports[f.wire.interface_key] = report; f.engine->check();
        t.check(f.service.discoveries.empty() && f.service.submissions.empty() && !f.engine->snapshot().required_campus_confirmed,
            "conflicting current recovery hints cannot trigger the healthy confirmation path or submit credentials");
        t.check(f.verify() != ConnectionEngine::CampusVerificationResult::confirmed && !f.engine->snapshot().required_campus_confirmed,
            "both reachable campus pages cannot turn conflicting required-service evidence green");
        f.service.reports[f.wire.interface_key] = connectivity(true); f.round();
        t.check(f.engine->snapshot().required_campus_confirmed && f.service.discoveries.size() == 2 && f.service.submissions.empty(),
            "later healthy evidence uses a fresh service request rather than retaining the previous conflict");
    }
    {
        Fixture f; f.round();
        f.service.reports[f.wire.interface_key] = hinted(CampusPortalHint::teaching); f.advance(6); f.round();
        t.check(f.service.submissions.empty(), "past display proof does not remove the first failure recovery threshold");
        f.advance(6); f.round();
        t.check(f.service.discoveries.size() == 2 && f.service.submissions.size() == 1 &&
            f.service.targets == std::vector<CampusArea>{CampusArea::teaching} && f.engine->snapshot().online,
            "a later genuine expiry obtains fresh recovery proof and at most one fake teaching submission rather than reusing display proof");
    }

    t.begin("automatic-campus-verification-cancellation-ownership");
    for (int interruption = 0; interruption != 11; ++interruption) {
        Fixture f; Gate gate; const auto bytes = f.memory_bytes;
        f.service.during_discovery = [&](const std::string&) { gate.block(); };
        std::thread worker([&] { f.round(); });
        const bool entered = gate.wait(); t.check(entered, "automatic verification enters the bounded late-result service barrier");
        if (entered) {
            const auto running = f.engine->snapshot();
            t.check(running.online && running.busy && running.auto_verifying_campus && running.verifying_campus &&
                !running.required_campus_confirmed, "the real automatic operation publishes a visible checking lifecycle before completion");
            switch (interruption) {
            case 0: f.engine->cancel(); break;
            case 1: f.wire.identity = "automatic-campus-fixture-replaced-lease"; f.engine->receive_path(f.path()); break;
            case 2: f.engine->forget_current_network(); break;
            case 3: f.engine->forget_network("abcde101"); break;
            case 4: f.engine->forget_all_networks(); break;
            case 5: f.engine->set_area(CampusArea::dormitory); break;
            case 6: f.engine->set_sleeping(true); break;
            case 7: f.engine->set_mode(MonitoringMode::observe); break;
            case 8: f.engine->set_mode(MonitoringMode::manual); break;
            case 9: f.engine->revoke_trust(); break;
            case 10: f.engine->configure(MonitoringMode::manual, CampusArea::teaching, ProbeMode::complete,
                Credentials{"r11-fixture-account", "r11-fixture-password"}); break;
            }
            const auto cancelled = f.engine->snapshot();
            t.check(!cancelled.busy && !cancelled.auto_verifying_campus && !cancelled.verifying_campus &&
                cancelled.status != EngineStatus::checking && cancelled.status != EngineStatus::discovering,
                "cancel, lease, forget, area, sleep, mode or configuration change clears the in-progress automatic lifecycle immediately");
        }
        gate.release(); worker.join(); const auto after = f.engine->snapshot();
        t.check(!gate.timed_out && !after.required_campus_confirmed && !after.last_discovery && !after.auto_verifying_campus &&
            !after.automatic_campus_verification_failed && !after.automatic_campus_verification_pending &&
            f.service.submissions.empty() && !after.trusted,
            "a deliberately late service success cannot restore invalidated proof, automatic feedback, trust or authentication");
        if (interruption == 1) t.check(!after.online, "a late result on a replaced lease cannot restore the old attachment's online fact");
        if (interruption != 2 && interruption != 3 && interruption != 4)
            t.check(f.memory_bytes == bytes && f.writes == 0, "non-forget interruption leaves persistent network proof and expiry byte-for-byte unchanged");
    }
    for (const auto next : {MonitoringMode::manual, MonitoringMode::observe}) {
        Fixture f; Gate gate; f.service.during_discovery = [&](const std::string&) { gate.block(); };
        std::thread worker([&] { f.engine->check(); });
        const bool entered = gate.wait(); t.check(entered, "manual connectivity check reaches its automatically nested campus-verification stage");
        if (entered) {
            t.check(f.engine->snapshot().busy_kind == OperationKind::manual && f.engine->snapshot().auto_verifying_campus,
                "the nested stage remains owned by the original manual connectivity operation");
            f.engine->set_mode(next);
            t.check(!f.engine->snapshot().busy && !f.engine->snapshot().verifying_campus,
                "leaving recover mode cancels a nested automatic stage even when its owner is manual");
        }
        gate.release(); worker.join();
        t.check(!gate.timed_out && !f.engine->snapshot().last_discovery && !f.engine->snapshot().required_campus_confirmed &&
            f.service.submissions.empty(), "late manual-owned automatic proof cannot publish after mode change");
    }
    {
        Fixture f; Gate gate; const auto request = f.engine->prepare_verify_campus();
        f.service.during_discovery = [&](const std::string&) { gate.block(); };
        auto result = ConnectionEngine::CampusVerificationResult::unavailable;
        std::thread worker([&] { if (request) result = f.engine->verify_campus(*request); });
        const bool entered = gate.wait(); t.check(entered, "independent explicit campus verification reaches the same bounded transport barrier");
        if (entered) { f.engine->set_mode(MonitoringMode::manual); t.check(f.engine->snapshot().verifying_campus,
            "ordinary mode selection retains the existing independent explicit-verification semantics"); }
        gate.release(); worker.join();
        t.check(!gate.timed_out && result == ConnectionEngine::CampusVerificationResult::confirmed &&
            f.engine->snapshot().required_campus_confirmed && f.service.submissions.empty(),
            "explicit manual verification is not confused with an automatically nested stage");
    }
}
