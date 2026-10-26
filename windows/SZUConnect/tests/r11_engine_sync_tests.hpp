#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace r11enginetest {
inline ConnectivityReport connectivity(bool online) {
    auto report = ConnectivityReport::simple(online, online);
    report.mode = ProbeMode::complete;
    return report;
}
inline ConnectivityReport hinted(CampusPortalHint hint) {
    auto report = connectivity(false);
    report.microsoft.state = EndpointState::redirect;
    report.microsoft.campus_area_hint = hint;
    return report;
}
inline physical::Route route(std::string key, std::string kind, std::string suffix) {
    physical::Route result;
    const bool first = suffix == "1";
    result.interface_key = std::move(key); result.interface_index = suffix == "1" ? 1 : 2;
    result.interface_luid = result.interface_index;
    result.source_ipv4 = first ? "192.0.2.7" : "198.51.100.7";
    result.addresses = {result.source_ipv4}; result.dns_servers = {first ? "192.0.2.53" : "198.51.100.53"};
    result.gateway_ipv4 = first ? "192.0.2.1" : "198.51.100.1";
    result.identity = "r11-fixture-" + suffix; result.display_name = "Fixture " + kind;
    result.descriptor.kind = std::move(kind);
    result.descriptor.raw = {{"gw", "02:00:00:00:00:0" + suffix}, {"gwip", result.gateway_ipv4},
        {"net", first ? "192.0.2.0/24" : "198.51.100.0/24"}};
    return result;
}
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false, timed_out = false;
    void block() {
        std::unique_lock lock(mutex); entered = true; changed.notify_all();
        if (!changed.wait_for(lock, std::chrono::seconds(10), [&] { return released; })) timed_out = true;
    }
    bool wait() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
    }
    void release() { std::lock_guard lock(mutex); released = true; changed.notify_all(); }
};
struct Service final : ConnectionService {
    std::mutex mutex;
    std::optional<physical::Route> bound;
    std::map<std::string, ConnectivityReport> reports;
    std::map<std::string, CampusDiscovery> proofs;
    std::vector<std::string> probes, discoveries, attempts, submissions;
    std::vector<CampusArea> targets;
    std::vector<bool> config_requirements;
    std::function<void(const std::string&)> during_probe, during_discovery, during_preflight, during_attempt;
    LoginAttempt reply{Outcome::success, RejectionKind::unspecified, PortalError::none, "", true};
    bool config_support = true;
    bool set_expected_physical_route(const std::optional<physical::Route>& value) override {
        std::lock_guard lock(mutex); bound = value; return value.has_value();
    }
    ConnectivityReport probe(ProbeMode mode, const CancelToken&) override {
        std::string key; ConnectivityReport result; std::function<void(const std::string&)> callback;
        { std::lock_guard lock(mutex); key = bound->interface_key; result = reports.at(key); probes.push_back(key); callback = during_probe; }
        result.mode = mode;
        if (callback) callback(key);
        return result;
    }
    CampusDiscovery discover(CampusArea area, const CancelToken&) override {
        std::string key; CampusDiscovery result; std::function<void(const std::string&)> callback;
        { std::lock_guard lock(mutex); key = bound->interface_key; result = proofs.at(key); discoveries.push_back(key); callback = during_discovery; }
        if (area == CampusArea::teaching) result.dormitory = Discovery::notChecked;
        if (callback) callback(key);
        return result;
    }
    LoginAttempt attempt(const Credentials& credentials, CampusArea area, const CancelToken& token) override {
        return attempt_with_requirements(credentials, area, token, false);
    }
    LoginAttempt attempt_with_requirements(const Credentials& credentials, CampusArea area, const CancelToken& token, bool required) override {
        std::string key; LoginAttempt result; std::function<void(const std::string&)> preflight, callback;
        {
            std::lock_guard lock(mutex); key = bound->interface_key; attempts.push_back(key); targets.push_back(area);
            config_requirements.push_back(required); result = reply; preflight = during_preflight; callback = during_attempt;
            if (!credentials.complete() || credentials.account != "r11-fixture-account" || credentials.password != "r11-fixture-password" ||
                (required && !config_support)) { result.outcome = Outcome::security; result.credential_submission_started = false; }
        }
        if (preflight) preflight(key);
        if (token.cancelled()) {
            result.outcome = Outcome::cancelled;
            result.credential_submission_started = false;
            return result;
        }
        {
            std::lock_guard lock(mutex);
            if (result.credential_submission_started) submissions.push_back(key);
            if (result.outcome == Outcome::success) reports[key] = connectivity(true);
        }
        if (callback) callback(key);
        if (token.cancelled()) result.outcome = result.credential_submission_started ? Outcome::uncertain : Outcome::cancelled;
        return result;
    }
};
struct Fixture {
    double clock = 1000, wall = 1800000000;
    Service service;
    physical::Route wifi = route("r11-wifi", "wifi", "1"), wired = route("r11-wired", "ethernet", "2");
    std::unique_ptr<ConnectionEngine> engine;
    int writes = 0; bool write_ok = true;
    std::string memory_bytes;
    explicit Fixture(CampusArea choice = CampusArea::automatic, std::string memory_scope = "teaching",
                     bool remember_wired = true, bool read_only = false) {
        wifi.is_system_primary = true;
        service.reports[wifi.interface_key] = connectivity(true);
        service.reports[wired.interface_key] = hinted(CampusPortalHint::teaching);
        service.proofs[wifi.interface_key] = service.proofs[wired.interface_key] = {Discovery::confirmed, Discovery::confirmed};
        ConnectionEngine::Hooks hooks;
        hooks.now = [this] { return clock; }; hooks.wall_now = [this] { return wall; }; hooks.sleep = [](double) {};
        hooks.random_hex = [] { return "00112233445566778899aabbccddeeff"; };
        hooks.persist_recovery = [](const std::string&) { return true; };
        hooks.persist_memory = [this](const std::string& value) { ++writes; if (write_ok) memory_bytes = value; return write_ok; };
        engine = std::make_unique<ConnectionEngine>(service, hooks, read_only);
        engine->restore_recovery(std::nullopt);
        engine->configure(MonitoringMode::recover, choice, ProbeMode::complete, Credentials{"r11-fixture-account", "r11-fixture-password"});
        NetworkMemory memory("00112233445566778899aabbccddeeff");
        memory.remember(wifi.descriptor, memory_scope, true, memory_scope == "dormitory", wall - 100, "abcde001");
        if (remember_wired) memory.remember(wired.descriptor, memory_scope, true, memory_scope == "dormitory", wall - 100, "abcde002");
        memory_bytes = memory.encode(); engine->restore_memory(memory_bytes); engine->receive_path(path(false));
    }
    PathSnapshot path(bool dual = true, bool reverse = false) const {
        PathSnapshot result; result.usable = result.link_active = result.requires_physical_binding = true;
        result.identity = "r11-fixture-topology"; result.interfaces = "Fixture attachments";
        auto a = wifi, b = wired; a.is_system_primary = !dual; b.is_system_primary = dual;
        result.physical_candidates = dual ? std::vector<physical::Route>{a, b} : std::vector<physical::Route>{a};
        if (reverse) std::reverse(result.physical_candidates.begin(), result.physical_candidates.end());
        for (const auto& item : result.physical_candidates) result.descriptors.push_back(item.descriptor);
        return result;
    }
    void round() { engine->automatic_round(); clock += 6; wall += 6; }
    void wired_only() { auto p = path(); p.physical_candidates.erase(std::remove_if(p.physical_candidates.begin(), p.physical_candidates.end(),
        [&](const auto& item) { return item.interface_key != wired.interface_key; }), p.physical_candidates.end()); p.descriptors = {wired.descriptor}; engine->receive_path(p); }
};
}

inline void r11_engine_sync_tests(Harness& t) {
    using namespace r11enginetest;
    t.begin("r11-dual-physical-handoff");
    for (const bool reverse : {false, true}) {
        Fixture f; f.round();
        t.check(f.engine->snapshot().online && f.engine->snapshot().required_campus_confirmed &&
            f.wifi.source_ipv4 != f.wired.source_ipv4 && f.wifi.gateway_ipv4 != f.wired.gateway_ipv4 &&
            f.wifi.descriptor.raw.at("net") != f.wired.descriptor.raw.at("net"),
            "original Wi-Fi has its own online report and campus proof on a different source, gateway and test subnet");
        const auto memory = f.memory_bytes;
        auto path = f.path(true, reverse); path.tunnel = true;
        f.engine->receive_path(path);
        t.check(!f.engine->snapshot().online && !f.engine->snapshot().last_report &&
            !f.engine->snapshot().last_discovery && !f.engine->snapshot().trusted &&
            f.engine->snapshot().authentication_route->interface_key == f.wired.interface_key,
            "new remembered primary wired path under TUN discards the old Wi-Fi report, service proof and session trust");
        f.round(); f.round();
        t.check(f.service.submissions == std::vector<std::string>{f.wired.interface_key} && f.engine->snapshot().online &&
            std::count(f.service.probes.begin(), f.service.probes.end(), f.wifi.interface_key) == 1 &&
            std::count(f.service.probes.begin(), f.service.probes.end(), f.wired.interface_key) == 3 &&
            f.service.probes.back() == f.wired.interface_key && f.memory_bytes == memory,
            "each enumeration order observes two wired failures, submits once and verifies only the wired source without borrowing Wi-Fi or rewriting memory");
        t.check(f.service.targets == std::vector<CampusArea>{CampusArea::teaching} &&
            f.service.discoveries == std::vector<std::string>{f.wifi.interface_key, f.wired.interface_key},
            "current wired teaching redirect requires its own fresh service proof despite the earlier successful Wi-Fi proof");
    }
    for (const bool reverse : {false, true}) {
        Fixture f(CampusArea::automatic, "teaching", false); f.round();
        t.check(f.engine->trust_current_network(), "unknown-wire handoff fixture has explicit trust for the old remembered Wi-Fi");
        const auto original_epoch = f.engine->snapshot().epoch;
        const auto memory = f.memory_bytes;
        f.service.probes.clear(); f.service.discoveries.clear();
        auto path = f.path(true, reverse); path.tunnel = true;
        f.engine->receive_path(path);
        const auto selected = f.engine->snapshot();
        t.check(selected.authentication_route && selected.authentication_route->interface_key == f.wired.interface_key &&
            selected.epoch != original_epoch && !selected.remembered && !selected.trusted && !selected.online &&
            !selected.last_report && !selected.last_discovery && selected.memory_count == 1,
            "an unknown primary wire replaces secondary Wi-Fi for observation without inheriting its online result, remembered eligibility or trust");
        f.round(); f.round();
        const auto state = f.engine->snapshot();
        t.check(!state.remembered && !state.online && !state.trusted && state.status == EngineStatus::needsTrust &&
            f.service.probes == std::vector<std::string>{f.wired.interface_key, f.wired.interface_key} &&
            f.service.discoveries.empty() && f.service.submissions.empty() && f.memory_bytes == memory && f.writes == 0,
            "the unknown primary gets independent read-only observations while automatic proof and credentials remain gated on its own qualification");
        t.check(f.engine->trust_and_remember() == ConnectionEngine::RememberResult::remembered &&
            f.engine->snapshot().remembered && !f.engine->snapshot().trusted && f.service.submissions.empty() &&
            !f.service.discoveries.empty() && std::all_of(f.service.discoveries.begin(), f.service.discoveries.end(),
                [&](const auto& key) { return key == f.wired.interface_key; }),
            "explicitly remembering the new wire verifies and saves its own teaching eligibility without session trust, disconnecting Wi-Fi or sending credentials");
        const auto saved = f.engine->remembered_networks();
        const auto old = std::find_if(saved.begin(), saved.end(), [](const MemoryRecord& item) { return item.id == "abcde001"; });
        const auto original = NetworkMemory::decode(memory, f.wall);
        t.check(saved.size() == 2 && original && old != saved.end() && old->features == original->records().front().features &&
            old->verified == original->records().front().verified && old->proof_teaching == original->records().front().proof_teaching &&
            old->proof_dormitory == original->records().front().proof_dormitory,
            "remembering a different primary source preserves the old Wi-Fi features, proof flags and expiry independently");
        f.round(); f.round();
        t.check(f.service.submissions == std::vector<std::string>{f.wired.interface_key} && f.engine->snapshot().online,
            "only the explicitly remembered wired source can then complete one automatic teaching recovery and wired recheck");
    }
    for (const bool reverse : {false, true}) {
        Fixture f(CampusArea::automatic, "teaching", false); f.round();
        auto path = f.path(true, reverse); path.tunnel = true;
        f.engine->receive_path(path); f.round(); f.round();
        const auto epoch = f.engine->snapshot().epoch;
        for (auto& candidate : path.physical_candidates)
            candidate.is_system_primary = candidate.interface_key == f.wifi.interface_key;
        f.engine->receive_path(path);
        t.check(f.engine->snapshot().authentication_route &&
            f.engine->snapshot().authentication_route->interface_key == f.wifi.interface_key &&
            f.engine->snapshot().epoch != epoch && !f.engine->snapshot().online && !f.engine->snapshot().last_report &&
            !f.engine->snapshot().last_discovery && f.engine->snapshot().remembered && !f.engine->snapshot().trusted,
            "switching primary back to the still-present remembered Wi-Fi rematches only its memory and clears the old wire observation");
        f.round();
        t.check(f.engine->snapshot().online && f.engine->snapshot().required_campus_confirmed &&
            f.service.probes.back() == f.wifi.interface_key && f.service.discoveries.back() == f.wifi.interface_key &&
            f.service.submissions.empty(),
            "a primary-only handoff back to Wi-Fi requires its own new report and campus proof without an unnecessary credential submission");
    }
    {
        Fixture f(CampusArea::automatic, "teaching", false); f.round();
        const auto epoch = f.engine->snapshot().epoch;
        auto path = f.path();
        for (auto& candidate : path.physical_candidates) candidate.is_system_primary = false;
        f.engine->receive_path(path);
        t.check(f.engine->snapshot().authentication_route &&
            f.engine->snapshot().authentication_route->interface_key == f.wifi.interface_key &&
            f.engine->snapshot().epoch == epoch && f.engine->snapshot().online && f.service.submissions.empty(),
            "without a primary observation the presence of an unknown Ethernet candidate alone cannot replace the known attachment");
    }
    for (const bool reverse : {false, true}) {
        Fixture f; f.round();
        const auto memory = f.memory_bytes;
        f.service.proofs[f.wired.interface_key] = {Discovery::unconfirmed, Discovery::unconfirmed};
        auto path = f.path(true, reverse); path.tunnel = true;
        f.engine->receive_path(path); f.round(); f.round();
        const auto state = f.engine->snapshot();
        t.check(state.authentication_route && state.authentication_route->interface_key == f.wired.interface_key &&
            !state.online && !state.required_campus_confirmed && state.last_discovery &&
            state.last_discovery->teaching == Discovery::unconfirmed && f.service.submissions.empty() &&
            f.service.discoveries == std::vector<std::string>{f.wifi.interface_key, f.wired.interface_key} &&
            f.memory_bytes == memory && f.writes == 0,
            "a known primary wire with failed fresh identity cannot borrow an earlier successful Wi-Fi service proof or update either memory");
    }
    for (const auto hold : {HoldReason::credentials, HoldReason::security, HoldReason::uncertain, HoldReason::userPaused}) {
        Fixture f; f.round();
        f.engine->restore_recovery("{\"schema\":1,\"generation\":1,\"holds\":[{\"reason\":" +
            std::to_string(static_cast<int>(hold)) + ",\"origin\":2}],\"notBefore\":1800000096}");
        const auto before = f.engine->snapshot(); const auto memory = f.memory_bytes;
        auto path = f.path(); path.tunnel = true;
        f.engine->receive_path(path);
        const auto selected = f.engine->snapshot();
        t.check(selected.authentication_route && selected.authentication_route->interface_key == f.wired.interface_key &&
            selected.epoch != before.epoch && selected.paused && selected.pause_reason == hold &&
            selected.cooldown_remaining == before.cooldown_remaining,
            "choosing a new primary neither clears an existing global hold nor resets its active submission deadline");
        f.round(); f.round();
        const auto after = f.engine->snapshot();
        t.check(after.paused && after.pause_reason == hold && after.cooldown_remaining && before.cooldown_remaining &&
            *after.cooldown_remaining == *before.cooldown_remaining - 12 && f.service.submissions.empty() &&
            f.memory_bytes == memory && f.writes == 0,
            "primary handoff and read-only rounds preserve credential, security, uncertain and user pauses with the original remaining cooldown");
    }
    for (const bool discovery : {false, true}) {
        Fixture f; f.service.reports[f.wifi.interface_key] = hinted(CampusPortalHint::teaching);
        f.round();
        Gate gate;
        auto block = [&](const std::string& key) { if (key == f.wifi.interface_key) gate.block(); };
        if (discovery) f.service.during_discovery = block; else f.service.during_probe = block;
        std::thread old([&] { f.engine->automatic_round(); });
        t.check(gate.wait(), "the requested interface-specific probe/discovery barrier is reached before its test deadline");
        f.engine->receive_path(f.path());
        t.check(f.engine->snapshot().authentication_route->interface_key == f.wired.interface_key && !f.engine->snapshot().busy,
            "read-only probe or discovery yields to a new remembered primary attachment");
        gate.release(); old.join();
        t.check(!gate.timed_out, "the interface handoff releases its fixture barrier before its test deadline");
        t.check(!f.engine->snapshot().online && !f.engine->snapshot().last_discovery && f.service.submissions.empty(),
            "a cancelled old-interface callback cannot publish proof or submit for the new attachment");
        f.round(); f.round();
        t.check(f.service.submissions == std::vector<std::string>{f.wired.interface_key}, "the new attachment starts with its own two failures and fresh proof");
    }
    {
        Fixture f; f.wired_only(); f.round();
        const auto epoch = f.engine->snapshot().epoch;
        Gate gate;
        f.service.during_discovery = [&](const std::string& key) { if (key == f.wired.interface_key) gate.block(); };
        std::thread old([&] { f.engine->automatic_round(); });
        t.check(gate.wait(), "wired discovery reaches its bounded removal barrier");
        f.engine->receive_path(f.path(false));
        gate.release(); old.join();
        t.check(!gate.timed_out && f.engine->snapshot().epoch != epoch &&
            f.engine->snapshot().authentication_route->interface_key == f.wifi.interface_key &&
            !f.engine->snapshot().online && !f.engine->snapshot().last_discovery && f.service.submissions.empty(),
            "removing the wire during discovery discards its late proof while the remembered Wi-Fi remains attached");
        f.round();
        t.check(f.engine->snapshot().online && f.engine->snapshot().last_report->all_passed() &&
            f.service.probes.back() == f.wifi.interface_key && f.service.submissions.empty(),
            "the surviving Wi-Fi publishes only its own new online result without a credential submission");
    }
    for (const bool remember_wired : {false, true}) {
        Fixture f(CampusArea::automatic, "teaching", remember_wired);
        f.service.reports[f.wifi.interface_key] = hinted(CampusPortalHint::teaching); f.round();
        const auto epoch = f.engine->snapshot().epoch;
        bool before_submission = false;
        f.service.during_preflight = [&](const std::string& key) {
            before_submission = key == f.wifi.interface_key && f.service.submissions.empty();
            f.engine->receive_path(f.path());
        };
        f.round();
        t.check(before_submission && f.engine->snapshot().epoch == epoch &&
            f.engine->snapshot().authentication_route->interface_key == f.wifi.interface_key &&
            f.service.attempts == std::vector<std::string>{f.wifi.interface_key} &&
            f.service.submissions == std::vector<std::string>{f.wifi.interface_key} && f.engine->snapshot().online,
            "a remembered or unknown new primary before the credential submission gate keeps the original preflight route and permits only its one submission");
    }
    {
        Fixture f; f.wired_only(); f.engine->trust_current_network(); f.round();
        bool entered_unsent = false;
        f.service.during_preflight = [&](const std::string&) {
            entered_unsent = f.service.submissions.empty();
            f.engine->revoke_trust();
        };
        f.round();
        const auto state = f.engine->snapshot();
        t.check(entered_unsent && !state.trusted && !state.busy && f.service.submissions.empty() && state.cooldown_remaining.has_value(),
            "revoking trust during credential preflight cancels the real token before any fake credential submission");
        f.service.during_preflight = {}; f.round(); f.round();
        t.check(f.service.submissions.empty(), "revoking an in-flight preflight cannot create a fresh automatic submission quota");
    }
    {
        Fixture f; f.wired_only(); f.engine->trust_current_network(); f.round();
        f.service.reply.outcome = Outcome::uncertain;
        f.service.during_attempt = [&](const std::string&) { f.engine->revoke_trust(); };
        f.round();
        const auto state = f.engine->snapshot();
        t.check(f.service.submissions.size() == 1 && !state.trusted && !state.busy && state.paused &&
            state.pause_reason == HoldReason::uncertain && state.cooldown_remaining.has_value(),
            "revoking trust after the credential gate retains unknown-result protection and the existing deadline");
        f.service.during_attempt = {}; f.service.reports[f.wifi.interface_key] = connectivity(false);
        f.engine->receive_path(f.path(false)); f.clock += 100; f.wall += 100; f.round(); f.round();
        t.check(f.service.submissions.size() == 1 && f.engine->snapshot().paused &&
            f.engine->snapshot().pause_reason == HoldReason::uncertain,
            "a different remembered attachment and elapsed cooldown cannot bypass the unknown result left by trust revocation");
    }
    {
        Fixture f; f.service.reports[f.wifi.interface_key] = hinted(CampusPortalHint::teaching); f.round();
        f.service.during_attempt = [&](const std::string&) { f.engine->receive_path(f.path()); };
        f.round();
        t.check(f.engine->snapshot().authentication_route->interface_key == f.wifi.interface_key && f.service.submissions.size() == 1 &&
            f.engine->snapshot().online, "credential preflight, submission and confirmation keep their original still-present route despite a new primary");
    }
    {
        Fixture f; f.service.reports[f.wifi.interface_key] = hinted(CampusPortalHint::teaching); f.round();
        f.service.reply.outcome = Outcome::uncertain;
        f.service.during_attempt = [&](const std::string&) { f.wired_only(); };
        f.round(); f.service.during_attempt = {}; f.round(); f.round();
        t.check(f.service.submissions.size() == 1 && f.engine->snapshot().paused &&
            f.engine->snapshot().pause_reason == HoldReason::uncertain,
            "removal during a credential request preserves the unknown-result hold and never creates a second submission quota");
    }
    t.begin("r11-config-routing-and-memory-scope");
    {
        Fixture f(CampusArea::automatic, "dormitory"); f.wired_only();
        f.service.reports[f.wired.interface_key] = connectivity(false); f.service.proofs[f.wired.interface_key].teaching_client_bound = true;
        f.round(); f.round();
        t.check(f.service.targets == std::vector<CampusArea>{CampusArea::teaching} && f.service.config_requirements == std::vector<bool>{true},
            "a source-bound CONFIG candidate reaches the immutable strong requirement at the actual service call");
    }
    {
        Fixture f(CampusArea::automatic, "dormitory"); f.wired_only();
        f.service.reports[f.wired.interface_key] = connectivity(false); f.service.proofs[f.wired.interface_key].teaching_client_bound = true;
        f.service.config_support = false; f.round(); f.round();
        t.check(f.service.submissions.empty() && f.engine->snapshot().status == EngineStatus::security,
            "a transport unable to satisfy the strong CONFIG requirement fails closed without trying another protocol");
    }
    {
        Fixture f(CampusArea::automatic, "dormitory"); f.engine->receive_path(f.path());
        f.service.reports[f.wired.interface_key] = connectivity(false);
        f.service.proofs[f.wired.interface_key] = {Discovery::unconfirmed, Discovery::confirmed};
        f.round(); f.round();
        t.check(f.service.submissions.empty() && !f.engine->snapshot().required_campus_confirmed,
            "two physical attachments without a tunnel retain the independent HTTPS identity requirement");
    }
    for (const auto hold : {HoldReason::credentials, HoldReason::security, HoldReason::uncertain, HoldReason::userPaused}) {
        Fixture f(CampusArea::automatic, "dormitory"); f.wired_only();
        f.service.reports[f.wired.interface_key] = connectivity(false);
        f.service.proofs[f.wired.interface_key].teaching_client_bound = true;
        f.engine->restore_recovery("{\"schema\":1,\"generation\":1,\"holds\":[{\"reason\":" +
            std::to_string(static_cast<int>(hold)) + ",\"origin\":2}]}");
        f.round(); f.round();
        t.check(f.service.submissions.empty() && f.engine->snapshot().paused,
            "a valid CONFIG cannot remove a credential, security, uncertain or user pause");
    }
    {
        Fixture f(CampusArea::automatic, "dormitory"); f.wired_only();
        f.service.reports[f.wired.interface_key] = connectivity(false);
        f.service.proofs[f.wired.interface_key].teaching_client_bound = true;
        f.engine->restore_recovery("{\"schema\":1,\"generation\":1,\"holds\":[],\"notBefore\":1800000090}");
        f.round(); f.round();
        t.check(f.service.submissions.empty() && f.engine->snapshot().cooldown_remaining.has_value(),
            "a source-bound CONFIG cannot bypass an existing duplicate-submission deadline");
    }
    {
        Fixture f(CampusArea::automatic, "teaching"); f.wired_only();
        f.service.reports[f.wired.interface_key] = hinted(CampusPortalHint::dormitory); f.round(); f.round();
        auto state = f.engine->snapshot();
        t.check(state.remembered && state.needs_network_memory_update && !state.recovery_setup_has_network &&
            state.can_remember && state.can_trust && f.service.submissions.empty(),
            "current dormitory evidence outranks a teaching preference and exposes the missing persistent scope without deleting the old record");
        t.check(f.engine->trust_current_network(), "temporary trust is available for the current attachment");
        state = f.engine->snapshot();
        t.check(state.trusted && state.needs_network_memory_update && !state.recovery_setup_has_network && state.can_remember,
            "temporary trust does not pretend that the persistent setup has been updated");
        t.check(f.engine->trust_and_remember() == ConnectionEngine::RememberResult::remembered,
            "temporary trust can still be followed by a permanent scope update");
        state = f.engine->snapshot();
        t.check(state.remembered && !state.needs_network_memory_update && state.recovery_setup_has_network,
            "only a successful permanent write completes the current network setup");
    }
    {
        Fixture f(CampusArea::automatic, "teaching"); f.wired_only();
        f.service.reports[f.wired.interface_key] = hinted(CampusPortalHint::dormitory);
        f.service.proofs[f.wired.interface_key].dormitory = Discovery::unconfirmed; f.round(); f.round();
        const auto state = f.engine->snapshot();
        t.check(state.last_discovery->teaching == Discovery::confirmed && !state.required_campus_confirmed &&
            state.required_campus_verification == Discovery::unconfirmed && f.service.submissions.empty(),
            "raw HTTPS teaching identity cannot stand in for the dormitory proof required by the current hint");
    }
    {
        Fixture f(CampusArea::automatic, "teaching"); f.wired_only();
        f.service.reports[f.wired.interface_key] = connectivity(false); f.service.proofs[f.wired.interface_key].teaching_client_bound = true;
        f.engine->check();
        t.check(f.engine->trust_and_remember() == ConnectionEngine::RememberResult::remembered,
            "CONFIG-backed memory verification succeeds without a credential request");
        const auto records = f.engine->remembered_networks();
        const auto wired = std::find_if(records.begin(), records.end(), [&](const MemoryRecord& item) { return item.id == "abcde002"; });
        t.check(wired != records.end() && wired->area == "teaching" && !wired->proof_dormitory && f.service.submissions.empty(),
            "both reachable homepages do not expand a selected teaching CONFIG memory to dormitory scope");
    }
    {
        Fixture f(CampusArea::automatic, "teaching"); f.wired_only(); f.service.reports[f.wired.interface_key] = connectivity(false);
        f.engine->connect();
        t.check(!f.engine->snapshot().needs_selection && f.service.submissions == std::vector<std::string>{f.wired.interface_key} &&
            f.service.targets == std::vector<CampusArea>{CampusArea::teaching},
            "manual recovery uses the same current valid teaching preference as automatic service verification");
        const bool trusted_after_login = f.engine->snapshot().trusted;
        f.service.reports[f.wired.interface_key] = connectivity(true); f.engine->check();
        t.check(!f.engine->snapshot().needs_selection && f.engine->snapshot().trusted == trusted_after_login && f.service.submissions.size() == 1,
            "complete basic success preserves entry readiness without adding authentication");
    }
    {
        Fixture f(CampusArea::dormitory, "teaching"); f.wired_only(); f.write_ok = false;
        const auto bytes = f.memory_bytes;
        t.check(f.engine->trust_and_remember() == ConnectionEngine::RememberResult::failed && f.memory_bytes == bytes &&
            !f.engine->snapshot().recovery_setup_has_network, "failed persistent scope update retains the original bytes and incomplete setup");
        f.engine->forget_current_network();
        t.check(!f.engine->snapshot().remembered && !f.engine->snapshot().trusted,
            "failed forget persistence still revokes this session's recovery qualification");
    }
    {
        Fixture f(CampusArea::dormitory, "dormitory"); f.wired_only();
        const auto memory = f.memory_bytes; const int writes = f.writes;
        const auto revision = f.engine->snapshot().discovery_revision;
        const auto request = f.engine->prepare_verify_campus();
        t.check(request.has_value() && f.engine->snapshot().discovery_revision > revision,
            "a reserved service-only operation publishes a distinct discovery lifecycle");
        if (request) t.check(f.engine->verify_campus(*request) == ConnectionEngine::CampusVerificationResult::confirmed,
            "service-only verification uses the selected scope and fresh proof");
        t.check(f.memory_bytes == memory && f.writes == writes && !f.engine->snapshot().trusted && f.service.submissions.empty(),
            "service-only verification cannot write memory, grant trust or submit credentials");
        f.engine->set_area(CampusArea::teaching);
        t.check(!f.engine->snapshot().last_discovery && f.engine->snapshot().required_campus_verification == Discovery::notChecked,
            "changing the selected area immediately invalidates old verification context");
    }
    {
        Fixture f(CampusArea::dormitory, "dormitory"); f.wired_only();
        f.engine->restore_recovery("{\"schema\":1,\"generation\":1,\"holds\":[],\"notBefore\":1800000090}");
        const auto before = f.engine->snapshot(); const auto memory = f.memory_bytes; const auto writes = f.writes;
        const auto request = f.engine->prepare_verify_campus();
        f.service.during_discovery = [&](const std::string&) { f.engine->cancel(); };
        t.check(request && f.engine->verify_campus(*request) == ConnectionEngine::CampusVerificationResult::cancelled,
            "service-only cancellation invalidates its result while a longer old deadline is active");
        const auto after = f.engine->snapshot();
        t.check(after.cooldown_remaining == before.cooldown_remaining && after.next_check_seconds >= before.next_check_seconds &&
            f.memory_bytes == memory && f.writes == writes && !after.last_discovery && f.service.submissions.empty(),
            "cancelling verification preserves the old deadline, cadence, memory and zero-submission boundary");
    }
    {
        Fixture f(CampusArea::automatic, "teaching", true, true); f.wired_only(); f.round(); f.round();
        t.check(f.service.submissions.empty() && !f.engine->prepare_remember() && !f.engine->prepare_verify_campus() && f.writes == 0,
            "read-only mode remains isolated from memory, credentials and campus verification actions");
    }
}
