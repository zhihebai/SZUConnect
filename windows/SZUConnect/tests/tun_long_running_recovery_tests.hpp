#pragma once
#include "automatic_campus_verification_tests.hpp"

inline void tun_long_running_recovery_tests(Harness& t) {
    using namespace automaticcampustest;
    t.begin("tun-gateway-neighbour-observation-reacquisition");
    {
        wiredtest::Fixture f(CampusArea::dormitory, "dormitory");
        const auto canonical_route = [&] {
            physical::Attachment evidence;
            evidence.physical = evidence.up = true;
            evidence.route = f.route;
            evidence.bound_default_gateways = {f.route.gateway_ipv4};
            return physical::stable_candidate_report({evidence}, {evidence}).routes.front();
        };
        f.route = canonical_route();
        f.engine->receive_path(f.path());
        f.service.response = wiredtest::online();
        t.check(f.engine->trust_current_network(), "gateway-observation fixture starts with explicit session approval");
        f.round();
        const auto stable = f.route;
        const auto original = f.saved();
        const auto epoch = f.engine->snapshot().epoch;
        t.check(f.engine->snapshot().remembered && f.engine->snapshot().memory_level == MatchLevel::strong &&
            f.engine->snapshot().required_campus_confirmed && f.memory_writes == 0,
            "a canonical physical lease with gateway hardware begins with independent service proof and an unchanged saved record");

        f.route.descriptor.raw.erase("gw");
        f.route = canonical_route();
        const auto missing = f.route;
        f.engine->receive_path(f.path());
        auto state = f.engine->snapshot();
        t.check(state.epoch != epoch && !state.trusted && state.remembered && state.memory_level == MatchLevel::medium &&
            !state.required_campus_confirmed && f.route != stable,
            "a missing neighbour observation creates a fresh lease and revokes session proof without deleting remembered eligibility");
        t.check(missing.interface_key == stable.interface_key && missing.interface_index == stable.interface_index &&
            missing.interface_luid == stable.interface_luid && missing.source_ipv4 == stable.source_ipv4 &&
            missing.addresses == stable.addresses && missing.dns_servers == stable.dns_servers &&
            missing.gateway_ipv4 == stable.gateway_ipv4 && missing.descriptor.raw.at("gwip") == stable.descriptor.raw.at("gwip") &&
            missing.descriptor.raw.at("net") == stable.descriptor.raw.at("net") &&
            missing.descriptor.raw.at("dhcp") == stable.descriptor.raw.at("dhcp"),
            "the neighbour-only transition preserves the exact physical source, associated DNS, gateway, subnet and DHCP values");
        f.service.proof = {Discovery::unconfirmed, Discovery::notChecked};
        f.round();
        t.check(f.engine->snapshot().remembered && f.engine->snapshot().automatic_campus_verification_failed &&
            f.service.submissions.empty() && f.memory_writes == 0,
            "a failed service read on the replacement lease remains retryable without rewriting network memory");
        f.service.proof = {Discovery::confirmed, Discovery::confirmed};
        f.engine->receive_path(f.path());
        f.round();
        t.check(f.engine->snapshot().remembered && f.engine->snapshot().required_campus_confirmed &&
            !f.engine->snapshot().trusted && f.engine->snapshot().holds.empty() && f.service.submissions.empty() && f.memory_writes == 0,
            "the next due round can reverify the still-missing-neighbour lease without asking the user to remember it again");

        f.route.descriptor.raw["gw"] = stable.descriptor.raw.at("gw");
        f.route = canonical_route();
        f.engine->receive_path(f.path());
        f.round();
        state = f.engine->snapshot();
        const auto restored = f.saved();
        t.check(f.route == stable && state.memory_level == MatchLevel::strong && state.remembered &&
            state.required_campus_confirmed && !state.trusted && state.holds.empty() && f.service.submissions.empty(),
            "the original neighbour returning obtains its own fresh service proof and restores the original strong memory match");
        t.check(restored.id == original.id && restored.area == original.area && restored.features == original.features && restored.created == original.created &&
            restored.verified == original.verified && restored.proof_teaching == original.proof_teaching &&
            restored.proof_dormitory == original.proof_dormitory && f.memory_writes == 0,
            "neighbour disappearance and return preserve record identity, features, proof flags and the original proof age byte-for-byte");

        f.route.descriptor.raw["gw"] = "02:00:00:00:00:99";
        f.route = canonical_route();
        f.engine->receive_path(f.path());
        f.service.response = wiredtest::hint(CampusPortalHint::dormitory);
        f.round(); f.round();
        state = f.engine->snapshot();
        t.check(state.memory_level == MatchLevel::conflict && !state.remembered && !state.trusted &&
            state.status == EngineStatus::needsTrust && f.service.submissions.empty() && f.service.attempts.empty() &&
            f.saved().id == original.id && f.saved().features == original.features && f.saved().verified == original.verified &&
            f.memory_writes == 0,
            "a different positive gateway hardware address remains a conflict and cannot borrow the original network's automatic-login authority");
    }
    t.begin("tun-long-running-service-failure-and-recovery");
    {
        Fixture f;
        const auto memory = f.memory_bytes;
        const auto original = f.engine->remembered_networks().front();
        const auto epoch = f.engine->snapshot().epoch;
        f.service.proofs[f.wire.interface_key] = {Discovery::unconfirmed, Discovery::notChecked};
        f.service.during_discovery = [&](const std::string&) { f.advance(6); };
        for (int round = 0; round < 600; ++round) {
            auto path = f.path(); path.tunnel = true;
            path.identity = round % 2 ? "fixture-tun-default-a" : "fixture-tun-default-b";
            path.interfaces = round % 2 ? "Fixture TUN" : "Fixture physical + TUN";
            path.usable = round % 3 != 0;
            f.engine->receive_path(path);
            f.round();
            const auto state = f.engine->snapshot();
            t.check(state.online && state.remembered && state.epoch == epoch && !state.busy && !state.trusted &&
                !state.required_campus_confirmed && state.automatic_campus_verification_failed && state.holds.empty(),
                "hundreds of timed-out service checks preserve the physical attachment, remembered eligibility and future monitoring");
            t.check(f.service.discoveries.size() == static_cast<std::size_t>(round + 1) && f.service.attempts.empty() &&
                f.service.submissions.empty() && f.memory_bytes == memory && f.writes == 0,
                "each due service retry is fresh, credential-free and cannot rewrite or extend network memory");
            t.check(f.engine->next_check_delay() > 0 && f.engine->next_check_delay() <= 120,
                "a long sequence of service failures cannot turn the normal monitoring delay into a permanent stop");
            f.advance(60);
        }
        f.service.during_discovery = {};
        f.service.proofs[f.wire.interface_key] = {Discovery::confirmed, Discovery::confirmed};
        f.round();
        t.check(f.engine->snapshot().required_campus_confirmed && !f.engine->snapshot().automatic_campus_verification_failed &&
            f.service.discoveries.size() == 601 && f.service.submissions.empty() && f.memory_bytes == memory,
            "a later valid service response recovers automatically after hundreds of failed checks without asking to remember again");
        for (int round = 0; round < 600; ++round) {
            f.advance(60); f.round();
            t.check(f.service.discoveries.size() == static_cast<std::size_t>(601 + (round + 1) / 15) &&
                f.engine->snapshot().required_campus_confirmed && f.service.submissions.empty() && f.writes == 0 && f.memory_bytes == memory,
                "hours of healthy monitoring refresh service verification every fifteen minutes without authenticating or extending memory");
        }
        t.check(f.service.discoveries.size() == 641 && f.service.submissions.empty() && f.writes == 0 &&
            f.memory_bytes == memory && f.engine->snapshot().epoch == epoch,
            "long healthy monitoring completes forty bounded periodic refreshes on the unchanged physical attachment");

        f.service.reports[f.wire.interface_key] = hinted(CampusPortalHint::teaching);
        f.advance(60); f.round();
        t.check(f.service.submissions.empty(), "a later real expiry still requires a second failed observation after long healthy monitoring");
        f.advance(5); f.round();
        const auto recovered = f.engine->snapshot();
        const auto saved = f.engine->remembered_networks().front();
        t.check(recovered.online && recovered.epoch == epoch && !recovered.trusted && recovered.holds.empty() &&
            f.service.submissions == std::vector<std::string>{f.wire.interface_key} &&
            f.service.targets == std::vector<CampusArea>{CampusArea::teaching},
            "same-link session expiry after a long DNS outage obtains fresh proof, submits fake credentials once and verifies online");
        t.check(saved.id == original.id && saved.features == original.features && saved.created == original.created &&
            saved.proof_teaching == original.proof_teaching && saved.proof_dormitory == original.proof_dormitory,
            "successful recovery retains the network identifier, original features and proof flags");
        f.service.reports[f.wire.interface_key] = hinted(CampusPortalHint::teaching);
        for (int event = 0; event < 100; ++event) {
            auto path = f.path(); path.tunnel = event % 2 == 0;
            path.identity = event % 2 ? "fixture-tun-late-a" : "fixture-tun-late-b";
            f.engine->receive_path(path); f.advance(0.1); f.round();
        }
        t.check(f.service.submissions.size() == 1 && f.engine->snapshot().cooldown_remaining.has_value(),
            "a notification burst after recovery cannot shorten the original cooldown or trigger a second submission");
    }
    t.begin("tun-long-running-protection-and-isolation");
    for (const auto reason : {HoldReason::credentials, HoldReason::security, HoldReason::uncertain, HoldReason::userPaused}) {
        Fixture f; const auto memory = f.memory_bytes;
        f.service.reports[f.wire.interface_key] = connectivity(false);
        f.engine->restore_recovery("{\"schema\":1,\"generation\":1,\"holds\":[{\"reason\":" +
            std::to_string(static_cast<int>(reason)) + ",\"origin\":2}]}");
        for (int round = 0; round < 300; ++round) {
            auto path = f.path(); path.tunnel = round % 2 == 0;
            path.identity = round % 2 ? "fixture-protected-tun-a" : "fixture-protected-tun-b";
            f.engine->receive_path(path); f.round(); f.advance(120);
        }
        const auto state = f.engine->snapshot();
        t.check(state.holds.size() == 1 && state.holds.front().reason == reason && state.remembered &&
            f.service.attempts.empty() && f.service.submissions.empty() && f.memory_bytes == memory && f.writes == 0,
            "hours of failed TUN monitoring cannot clear password, security, unknown-result or user pause protection");
    }
    {
        Fixture f(CampusArea::automatic, "teaching", MonitoringMode::recover, true);
        const auto memory = f.memory_bytes;
        for (int round = 0; round < 300; ++round) {
            f.service.reports[f.wire.interface_key] = round % 2 ? connectivity(true) : hinted(CampusPortalHint::teaching);
            f.round(); f.advance(120);
        }
        t.check(f.service.discoveries.empty() && f.service.attempts.empty() && f.service.submissions.empty() &&
            f.writes == 0 && f.memory_bytes == memory && !f.engine->snapshot().trusted,
            "long read-only monitoring never verifies remembered authority, writes data or submits credentials");
    }
    for (const bool replace_binding : {false, true}) {
        Fixture f; const auto memory = f.memory_bytes;
        f.service.proofs[f.wire.interface_key] = {Discovery::unconfirmed, Discovery::notChecked};
        for (int round = 0; round < 300; ++round) { f.round(); f.advance(60); }
        f.service.proofs[f.wire.interface_key] = {Discovery::confirmed, Discovery::confirmed};
        f.service.during_discovery = [&](const std::string&) {
            if (replace_binding) { f.wire.identity = "fixture-long-run-replacement-lease"; f.engine->receive_path(f.path()); }
            else f.engine->cancel();
        };
        f.round();
        t.check(!f.engine->snapshot().required_campus_confirmed && f.service.submissions.empty() && f.memory_bytes == memory && f.writes == 0,
            "late service success after hundreds of failures cannot survive cancellation or replacement of its physical binding");
        f.service.during_discovery = {}; f.advance(60); f.round();
        t.check(f.engine->snapshot().required_campus_confirmed && f.service.submissions.empty() && f.memory_bytes == memory,
            "the next valid monitoring round can recover after cancellation without adopting its late proof");
    }
}
