#pragma once
#include <string>

inline void independent_verification_hint_tests(Harness& t) {
    using namespace automaticcampustest;
    const auto same_report = [](const EngineSnapshot& before, const EngineSnapshot& after) {
        return before.last_report && after.last_report && before.online == after.online &&
            before.last_report->mode == after.last_report->mode &&
            before.last_report->apple.state == after.last_report->apple.state &&
            before.last_report->apple.campus_area_hint == after.last_report->apple.campus_area_hint &&
            before.last_report->microsoft.state == after.last_report->microsoft.state &&
            before.last_report->microsoft.campus_area_hint == after.last_report->microsoft.campus_area_hint;
    };
    const auto conflict = [] {
        auto report = hinted(CampusPortalHint::dormitory);
        report.apple.state = EndpointState::redirect;
        report.apple.campus_area_hint = CampusPortalHint::teaching;
        return report;
    };
    t.begin("independent-verification-fresh-selection-hints");
    for (const auto area : {CampusArea::automatic, CampusArea::teaching, CampusArea::dormitory}) {
        Fixture f(area, "dormitory", MonitoringMode::manual);
        if (area != CampusArea::automatic) { f.service.reports[f.wire.interface_key] = conflict(); f.engine->check(); }
        const auto probes = f.service.probes.size();
        t.check(f.verify() == ConnectionEngine::CampusVerificationResult::confirmed && f.service.probes.size() == probes &&
            f.engine->snapshot().area == area && f.service.submissions.empty(),
            "no historical candidate hint or an explicitly fixed scope needs no extra selection probe or authentication");
    }
    {
        Fixture f(CampusArea::automatic, "teaching", MonitoringMode::manual);
        t.check(f.verify() == ConnectionEngine::CampusVerificationResult::confirmed && f.engine->snapshot().required_campus_confirmed,
            "the failed-new-discovery fixture begins with a successful independent confirmation");
        f.service.reports[f.wire.interface_key] = connectivity(false);
        f.service.proofs[f.wire.interface_key] = {Discovery::unconfirmed, Discovery::unconfirmed};
        f.engine->connect();
        t.check(f.engine->snapshot().last_discovery && f.engine->snapshot().last_discovery->teaching == Discovery::unconfirmed &&
            !f.engine->snapshot().required_campus_confirmed && f.service.submissions.empty(),
            "a new no-hint authentication discovery drops the old service confirmation before reporting an HTTPS failure");
    }
    for (const bool old_conflict : {false, true}) {
        Fixture f(CampusArea::automatic, "teaching", MonitoringMode::manual);
        f.service.reports[f.wire.interface_key] = old_conflict ? conflict() : hinted(CampusPortalHint::dormitory);
        f.engine->check();
        const auto before = f.engine->snapshot(); const auto bytes = f.memory_bytes;
        const auto probes = f.service.probes.size();
        f.service.reports[f.wire.interface_key] = connectivity(true);
        f.service.proofs[f.wire.interface_key] = {Discovery::confirmed, Discovery::unconfirmed, false};
        t.check(f.verify() == ConnectionEngine::CampusVerificationResult::confirmed,
            "an independent automatic service check replaces a previous dormitory or conflicting hint with same-operation evidence");
        const auto after = f.engine->snapshot();
        t.check(after.required_campus_confirmed && same_report(before, after) && after.area == CampusArea::automatic &&
            f.service.probes.size() == probes + 1 && f.service.probes.back() == f.wire.interface_key,
            "fresh verification uses the selected physical binding without rewriting the published result, mode or selected area");
        t.check(!after.trusted && f.memory_bytes == bytes && f.writes == 0 && f.service.submissions.empty(),
            "service confirmation remains display evidence without trust, memory writes or authentication");
        f.service.reports[f.wire.interface_key] = hinted(CampusPortalHint::dormitory);
        f.engine->check();
        t.check(!f.engine->snapshot().required_campus_confirmed && f.service.submissions.empty(),
            "a later new dormitory observation does not inherit the independent teaching-service confirmation");
    }
    for (int current = 0; current != 4; ++current) {
        Fixture f(CampusArea::automatic, "dormitory", MonitoringMode::manual);
        f.service.reports[f.wire.interface_key] = hinted(CampusPortalHint::teaching); f.engine->check();
        const auto before = f.engine->snapshot(); const auto bytes = f.memory_bytes;
        f.service.reports[f.wire.interface_key] = current == 0 ? conflict() : hinted(CampusPortalHint::dormitory);
        f.service.proofs[f.wire.interface_key] = current == 3 ? CampusDiscovery{Discovery::confirmed, Discovery::confirmed}
            : current == 2 ? CampusDiscovery{Discovery::unconfirmed, Discovery::confirmed}
                           : CampusDiscovery{Discovery::confirmed, Discovery::unconfirmed};
        const auto result = f.verify();
        t.check(result == (current == 3 ? ConnectionEngine::CampusVerificationResult::confirmed : ConnectionEngine::CampusVerificationResult::needsProof),
            "new conflicts, missing HTTPS identity and missing required dormitory service remain unconfirmed while current dual proof succeeds");
        t.check(same_report(before, f.engine->snapshot()) && f.memory_bytes == bytes && f.writes == 0 && f.service.submissions.empty(),
            "a new selection result cannot overwrite old connectivity data, expand memory or submit credentials");
    }
    for (const auto hold : {HoldReason::userPaused, HoldReason::credentials, HoldReason::security, HoldReason::uncertain}) {
        Fixture f(CampusArea::automatic, "teaching", MonitoringMode::manual);
        f.service.reports[f.wire.interface_key] = hinted(CampusPortalHint::dormitory); f.engine->check();
        f.engine->restore_recovery("{\"schema\":1,\"generation\":1,\"holds\":[{\"reason\":" +
            std::to_string(static_cast<int>(hold)) + ",\"origin\":2}],\"notBefore\":" + std::to_string(f.wall.load() + 300) + "}");
        const auto before = f.engine->snapshot(); const auto bytes = f.memory_bytes;
        f.service.reports[f.wire.interface_key] = connectivity(true);
        t.check(f.verify() == ConnectionEngine::CampusVerificationResult::confirmed,
            "independent fresh-hint verification can finish while an authentication protection is retained");
        const auto after = f.engine->snapshot();
        t.check(before.holds.size() == 1 && after.holds.size() == before.holds.size() && after.holds.front().reason == hold &&
            after.cooldown_remaining == before.cooldown_remaining && after.next_check_seconds == before.next_check_seconds &&
            same_report(before, after) && f.memory_bytes == bytes && f.writes == 0 && f.service.submissions.empty(),
            "fresh helper probes do not settle holds, shorten a long cooldown, advance monitoring or update stored memory");
    }
    for (const bool remember : {false, true}) for (int interruption = 0; interruption != 4; ++interruption) {
        Fixture f(CampusArea::automatic, "teaching", MonitoringMode::manual);
        f.service.reports[f.wire.interface_key] = hinted(CampusPortalHint::dormitory); f.engine->check();
        const auto bytes = f.memory_bytes;
        f.service.during_probe = [&](const std::string&) {
            if (interruption == 0) f.engine->cancel();
            else if (interruption == 1) { f.wire.identity += "-changed"; f.engine->receive_path(f.path()); }
            else if (interruption == 2) f.engine->set_sleeping(true);
            else { auto path = f.path(); path.physical_candidates.clear(); path.descriptors.clear(); f.engine->receive_path(path); }
        };
        if (remember) {
            t.check(f.engine->trust_and_remember() == ConnectionEngine::RememberResult::cancelled,
                "cancel, changed attachment, sleep or lost binding rejects a late hint probe before remembering");
        } else {
            t.check(f.verify() == ConnectionEngine::CampusVerificationResult::cancelled,
                "cancel, changed attachment, sleep or lost binding rejects a late hint probe before service confirmation");
        }
        t.check(f.service.discoveries.empty() && !f.engine->snapshot().required_campus_confirmed &&
            f.memory_bytes == bytes && f.writes == 0 && f.service.submissions.empty(),
            "an interrupted hint refresh cannot proceed to discovery or retain a late proof, write or login");
    }
    for (const bool current_conflict : {false, true}) {
        Fixture f(CampusArea::automatic, "teaching", MonitoringMode::manual, false, false);
        f.service.reports[f.wire.interface_key] = hinted(CampusPortalHint::dormitory); f.engine->check();
        const auto before = f.engine->snapshot(); const auto bytes = f.memory_bytes;
        f.service.reports[f.wire.interface_key] = current_conflict ? conflict() : connectivity(true);
        f.service.proofs[f.wire.interface_key] = {Discovery::confirmed, Discovery::unconfirmed, true};
        const auto result = f.engine->trust_and_remember();
        t.check(result == (current_conflict ? ConnectionEngine::RememberResult::needsProof : ConnectionEngine::RememberResult::remembered),
            "remembering uses current hints and client-bound teaching proof but cannot ignore a current conflict");
        t.check(same_report(before, f.engine->snapshot()) && f.service.probes.size() == 2 && f.service.submissions.empty(),
            "remembering refreshes exactly one fixed probe round without publishing it or authenticating");
        const auto records = f.engine->remembered_networks();
        t.check(current_conflict ? (f.writes == 0 && f.memory_bytes == bytes && records.empty())
            : (f.writes == 1 && records.size() == 1 && records.front().area == "teaching" && !records.front().proof_dormitory),
            "only a successful same-operation teaching proof creates its narrow memory scope; failed proof preserves bytes");
    }
}
