#pragma once
#include <string>
#include <thread>
#include <utility>

void r11_presentation_tests(Harness& t) {
    t.begin("required-campus-presentation");
    EngineSnapshot state;
    state.mode = MonitoringMode::recover;
    state.area = CampusArea::dormitory;
    state.network_available = state.can_trust = state.has_saved = state.draft_complete = true;
    state.remembered = true;
    state.needs_network_memory_update = true;
    state.auto_login = AutoLoginState::awaitingProof;
    state.last_discovery = CampusDiscovery{Discovery::confirmed, Discovery::unconfirmed};
    state.required_campus_verification = Discovery::unconfirmed;
    t.check(!status::readiness_input_from(state).remembered &&
        status::campus_verification_key(state) == "env.campusVerification.unconfirmed",
        "old teaching memory and raw TLS identity cannot complete current dormitory setup or verification");
    state.trusted = true;
    state.auto_login = AutoLoginState::ready;
    const auto temporary = status::recovery_readiness(state);
    t.check(!status::readiness_input_from(state).remembered && temporary.items.at(1).key == "ready.network.trusted" &&
        temporary.title != "ready.title.ready",
        "temporary approval is presented separately from permanently saved current-scope eligibility");
    state.recovery_setup_has_network = true;
    state.needs_network_memory_update = false;
    state.required_campus_verification = Discovery::confirmed;
    t.check(status::readiness_input_from(state).remembered &&
        status::recovery_readiness(state).items.at(1).key == "ready.network.remembered",
        "only the effective durable scope supplies the remembered readiness item");
    state.recovery_setup_has_network = false;
    state.needs_selection = true;
    state.status = EngineStatus::unconfirmed;
    state.online = true;
    auto displayed = status::present(status::input_from(state));
    t.check(displayed.primary == "check" && displayed.secondary == "area" && displayed.headline == "campus.entry.pendingTitle" &&
        displayed.detail == "campus.entry.bothDetected" && !status::system_path_can_lead(state),
        "unresolved authentication keeps detection primary and entry choice optional without misreporting a failed campus service");
    for (const auto reason : {HoldReason::credentials, HoldReason::security, HoldReason::uncertain, HoldReason::userPaused}) {
        state.holds = {Hold{}}; state.holds.front().reason = reason;
        state.pause_reason = reason; state.auto_login = AutoLoginState::paused;
        state.paused = true; state.status = EngineStatus::paused;
        displayed = status::present(status::input_from(state));
        t.check(displayed.primary == "resume" && displayed.banner && displayed.banner->key.starts_with("hold.") &&
            !status::system_path_can_lead(state), "selection feedback preserves each existing protection hold");
    }
    state.holds.clear(); state.pause_reason.reset(); state.paused = false;
    state.auto_login = AutoLoginState::ready; state.status = EngineStatus::discovering; state.busy = true;
    displayed = status::present(status::input_from(state));
    t.check(displayed.primary == "cancel" && status::campus_verification_key(state) == "env.campusVerification.checking",
        "busy discovery retains a real cancel action and current checking label");
    state.busy = false; state.status = EngineStatus::unconfirmed; state.read_only = true;
    displayed = status::present(status::input_from(state));
    t.check(displayed.primary != "area" && displayed.chip == "auto.readOnly.chip",
        "read-only presentation never exposes mutable selection as its primary action");

    state = {};
    state.mode = MonitoringMode::recover;
    state.network_available = state.can_trust = state.has_saved = state.draft_complete = true;
    state.remembered = state.recovery_setup_has_network = state.online = true;
    state.auto_login = AutoLoginState::awaitingProof;
    state.status = EngineStatus::online;
    t.check(status::recovery_readiness(state).title == "ready.title.monitoring" &&
        status::campus_verification_hint_key(state) == "campus.verify.autoPendingHint",
        "remembered identified online recovery explains automatic verification without requesting manual action");
    state.busy = state.verifying_campus = state.auto_verifying_campus = true;
    state.status = EngineStatus::discovering;
    t.check(status::recovery_readiness(state).title == "ready.title.checking" &&
        status::recovery_readiness(state).items.at(1).key == "ready.network.knownChecking" &&
        status::campus_verification_hint_key(state) == "campus.verify.checking" &&
        status::present(status::input_from(state)).primary == "cancel",
        "automatic verification presents checking and a cancel action within its existing detection round");
    state.busy = state.verifying_campus = state.auto_verifying_campus = false;
    state.status = EngineStatus::online;
    state.last_discovery = CampusDiscovery{Discovery::unconfirmed, Discovery::unconfirmed};
    state.required_campus_verification = Discovery::unconfirmed;
    state.automatic_campus_verification_failed = state.automatic_campus_verification_pending = true;
    t.check(status::recovery_readiness(state).title == "ready.title.verificationFailed" &&
        status::campus_verification_key(state) == "env.campusVerification.unconfirmed" &&
        status::campus_verification_hint_key(state) == "campus.verify.autoFailed",
        "an actual automatic failure replaces checking with its retry explanation and never a stale confirmed label");
    for (const auto reason : {HoldReason::credentials, HoldReason::security, HoldReason::uncertain, HoldReason::userPaused}) {
        state.holds = {Hold{}}; state.holds.front().reason = reason;
        state.pause_reason = reason; state.paused = true; state.auto_login = AutoLoginState::paused;
        t.check(status::recovery_readiness(state).title == "ready.title.paused",
            "automatic failure never overrides credentials, TLS, uncertain-result or user-pause readiness");
    }
    state.holds.clear(); state.pause_reason.reset(); state.paused = false;
    state.auto_login = AutoLoginState::coolingDown; state.cooldown_remaining = 90;
    t.check(status::recovery_readiness(state).items.at(2).key == "ready.protection.cooldown" &&
        status::recovery_readiness(state).title != "ready.title.verificationFailed",
        "cooldown remains higher priority than the automatic-service retry state");
    state.cooldown_remaining.reset(); state.auto_login = AutoLoginState::needsAccount; state.has_saved = false;
    t.check(status::recovery_readiness(state).items.front().key == "ready.account.missing" &&
        status::recovery_readiness(state).title == "ready.title.attention",
        "automatic verification cannot conceal a missing saved account");
    state.has_saved = true; state.auto_login = AutoLoginState::ready;
    state.automatic_campus_verification_failed = state.automatic_campus_verification_pending = false;
    state.last_discovery = CampusDiscovery{Discovery::confirmed, Discovery::confirmed};
    state.required_campus_verification = Discovery::confirmed;
    t.check(status::recovery_readiness(state).title == "ready.title.ready" &&
        status::campus_verification_hint_key(state) == "campus.verify.confirmedHint",
        "successful automatic verification returns the remembered recovery status to confirmed");
    state.last_discovery.reset(); state.required_campus_verification = Discovery::notChecked;
    state.recovery_setup_has_network = false; state.trusted = true;
    t.check(status::campus_verification_hint_key(state) == "campus.verify.onlineHint",
        "one-time trust is not described as a saved network awaiting automatic service verification");
    state.recovery_setup_has_network = true; state.read_only = true;
    t.check(status::recovery_readiness(state).title == "ready.title.off" &&
        status::campus_verification_hint_key(state) == "campus.verify.onlineHint",
        "read-only presentation makes no automatic service-verification promise");
    state.read_only = false; state.remembered = false;
    t.check(status::campus_verification_hint_key(state) == "campus.verify.onlineHint",
        "a legacy teaching setup default without an actual saved network does not promise automatic service verification");
    state.remembered = true; state.remember_blocker = "unreliable";
    t.check(status::campus_verification_hint_key(state) == "campus.verify.onlineHint",
        "an unreliable current physical attachment never advertises automatic campus verification from historical memory alone");

    t.begin("current-campus-failure-phase-presentation");
    const std::pair<CampusVerificationIssue, const char*> phases[] = {
        {CampusVerificationIssue::dnsTimeout, "campus.verify.dnsTimeoutHint"},
        {CampusVerificationIssue::dns, "campus.verify.dnsHint"},
        {CampusVerificationIssue::dnsTls, "campus.verify.dnsTlsHint"},
        {CampusVerificationIssue::tls, "campus.verify.tlsHint"},
        {CampusVerificationIssue::connection, "campus.verify.connectionHint"},
        {CampusVerificationIssue::http, "campus.verify.httpHint"},
        {CampusVerificationIssue::page, "campus.verify.pageHint"},
        {CampusVerificationIssue::binding, "campus.verify.bindingHint"},
    };
    for (const auto& [issue, hint] : phases) {
        automaticcampustest::Fixture f;
        f.service.proofs[f.wire.interface_key] = {Discovery::unconfirmed, Discovery::notChecked, false, issue};
        const auto original_memory = f.memory_bytes;
        f.round(); const auto failed = f.engine->snapshot();
        t.check(failed.online && failed.last_discovery && failed.last_discovery->issue == issue &&
            status::campus_verification_hint_key(failed) == hint && failed.automatic_campus_verification_pending,
            "a current service failure travels through the engine snapshot into its specific retry explanation");
        t.check(f.writes == 0 && f.memory_bytes == original_memory && f.service.submissions.empty() &&
            !failed.trusted && failed.holds.empty(),
            "failure diagnostics do not change memory, grant trust, pause recovery or submit credentials");
        auto checking = failed; checking.verifying_campus = true;
        t.check(status::campus_verification_hint_key(checking) == "campus.verify.checking",
            "a new verification lifecycle takes priority over the previous failure phase");
        auto hidden = failed; hidden.network_available = false;
        t.check(status::campus_verification_hint_key(hidden) != hint,
            "unavailable attachments cannot display a previous network's detailed failure");
        hidden = failed; hidden.sleeping = true;
        t.check(status::campus_verification_hint_key(hidden) != hint,
            "sleeping attachments cannot display a previous live failure phase");
        f.service.proofs[f.wire.interface_key] = {Discovery::confirmed, Discovery::confirmed};
        f.advance(30); f.round(); const auto recovered = f.engine->snapshot();
        t.check(recovered.required_campus_confirmed && recovered.last_discovery &&
            recovered.last_discovery->issue == CampusVerificationIssue::none &&
            status::campus_verification_hint_key(recovered) == "campus.verify.confirmedHint" &&
            f.service.submissions.empty(),
            "the next successful ordinary verification replaces the detailed failure without a login");
    }
    for (const bool replace : {false, true}) for (const auto issue : {CampusVerificationIssue::dnsTimeout, CampusVerificationIssue::dnsTls}) {
        automaticcampustest::Fixture f;
        f.service.proofs[f.wire.interface_key] = {Discovery::unconfirmed, Discovery::notChecked, false,
            issue};
        automaticcampustest::Gate gate;
        f.service.during_discovery = [&](const std::string&) { gate.block(); };
        std::thread worker([&] { f.round(); });
        const bool entered = gate.wait();
        if (replace) { f.wire.identity = "phase-fixture-replacement-lease"; f.engine->receive_path(f.path()); }
        else f.engine->cancel();
        gate.release(); worker.join(); const auto after = f.engine->snapshot();
        t.check(entered && !gate.timed_out && !after.last_discovery &&
            status::campus_verification_hint_key(after) != "campus.verify.dnsTimeoutHint" &&
            status::campus_verification_hint_key(after) != "campus.verify.dnsTlsHint" &&
            !after.automatic_campus_verification_failed && f.service.submissions.empty(),
            "cancelled or replaced engine operations discard a deliberately late DNS diagnostic");
    }
}
