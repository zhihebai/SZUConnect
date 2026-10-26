#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wiredtest {
inline ConnectivityReport hint(CampusPortalHint area, std::optional<CampusPortalHint> other = std::nullopt) {
    auto result = ConnectivityReport::simple(false, false);
    result.mode = ProbeMode::complete;
    result.microsoft.state = EndpointState::redirect;
    result.microsoft.campus_area_hint = area;
    if (other) { result.apple.state = EndpointState::redirect; result.apple.campus_area_hint = other; }
    return result;
}
inline ConnectivityReport online() {
    auto result = ConnectivityReport::simple(true, true); result.mode = ProbeMode::complete; return result;
}
struct Service final : ConnectionService {
    std::optional<physical::Route> bound;
    std::deque<ConnectivityReport> reports;
    ConnectivityReport response = hint(CampusPortalHint::teaching);
    CampusDiscovery proof{Discovery::confirmed, Discovery::confirmed};
    Outcome outcome = Outcome::success;
    RejectionKind rejection = RejectionKind::password;
    std::vector<CampusArea> attempts, submissions, discoveries;
    std::vector<std::string> probe_bindings, discovery_bindings, attempt_bindings;
    std::vector<ProbeMode> probe_modes;
    std::vector<bool> deferred_website_scopes;
    std::function<void()> during_probe, during_discovery, during_attempt;
    bool binding_available = true, fixture_credentials = true;
    bool set_expected_physical_route(const std::optional<physical::Route>& value) override { bound = value; return binding_available; }
    ConnectivityReport probe(ProbeMode mode, const CancelToken&) override {
        probe_modes.push_back(mode);
        probe_bindings.push_back(bound ? bound->interface_key : "system");
        auto result = response;
        if (!reports.empty()) { result = reports.front(); reports.pop_front(); }
        result.mode = mode;
        if (during_probe) during_probe();
        return result;
    }
    ConnectivityReport probe_with_scope(ProbeMode mode, const CancelToken& token, bool, bool defer_websites) override {
        deferred_website_scopes.push_back(defer_websites);
        return probe(mode, token);
    }
    CampusDiscovery discover(CampusArea area, const CancelToken&) override {
        discoveries.push_back(area); discovery_bindings.push_back(bound ? bound->interface_key : "system");
        auto result = proof;
        if (area == CampusArea::teaching) result.dormitory = Discovery::notChecked;
        if (during_discovery) during_discovery();
        return result;
    }
    LoginAttempt attempt(const Credentials& credentials, CampusArea area, const CancelToken&) override {
        fixture_credentials = fixture_credentials && credentials.account == "wired-session-fixture-account" && credentials.password == "wired-session-fixture-password";
        attempts.push_back(area); attempt_bindings.push_back(bound ? bound->interface_key : "system");
        LoginAttempt result; result.outcome = outcome; result.rejection = rejection;
        result.credential_submission_started = outcome != Outcome::unavailable && outcome != Outcome::challengeRefused && outcome != Outcome::security;
        if (result.credential_submission_started) submissions.push_back(area);
        if (outcome == Outcome::success) response = online();
        if (during_attempt) during_attempt();
        return result;
    }
};
struct Fixture {
    double clock = 1000, wall = 1800000000;
    int memory_writes = 0;
    std::string persisted_memory, journal;
    Service service;
    physical::Route route;
    std::unique_ptr<ConnectionEngine> engine;
    MemoryRecord original;
    explicit Fixture(CampusArea choice = CampusArea::automatic, std::string preference = "dormitory", bool read_only = false,
                     bool remembered = true, bool credentials = true) {
        route = physical::candidates({physical_attachment("wired-session-fixture")}).front();
        route.descriptor.raw["gw"] = "a4:5e:60:91:19:01";
        route.identity = "wired-session-fixture-A";
        ConnectionEngine::Hooks hooks;
        hooks.now = [this] { return clock; }; hooks.wall_now = [this] { return wall; };
        hooks.sleep = [](double) {};
        hooks.random_hex = [] { return "00112233445566778899aabbccddeeff"; };
        hooks.persist_memory = [this](const std::string& value) { ++memory_writes; persisted_memory = value; return true; };
        hooks.persist_recovery = [this](const std::string& value) { journal = value; return true; };
        engine = std::make_unique<ConnectionEngine>(service, hooks, read_only);
        engine->restore_recovery(std::nullopt);
        engine->configure(MonitoringMode::recover, choice, ProbeMode::complete,
            credentials ? std::optional<Credentials>(Credentials{"wired-session-fixture-account", "wired-session-fixture-password"}) : std::nullopt);
        NetworkMemory memory("00112233445566778899aabbccddeeff");
        if (remembered) { memory.remember(route.descriptor, preference, true, true, wall - 7200, "b184abcd"); original = memory.records().front(); }
        engine->restore_memory(memory.encode()); engine->receive_path(path());
    }
    PathSnapshot path() const {
        PathSnapshot result; result.usable = result.link_active = result.requires_physical_binding = true;
        result.identity = "wired-session-global"; result.tunnel = true; result.interfaces = "Fixture Ethernet";
        result.descriptors = {route.descriptor}; result.physical_candidates = {route}; return result;
    }
    void round() { engine->automatic_round(); clock += 31; wall += 31; }
    MemoryRecord saved() const { return engine->remembered_networks().front(); }
};
}  // namespace wiredtest

inline void wired_session_recovery_tests(Harness& t) {
    using namespace wiredtest;
    t.begin("wired-fixed-probe-redirect-classification");
    const std::string apple = "https://www.apple.com/library/test/success.html";
    const std::string microsoft = "http://www.msftconnecttest.com/connecttest.txt";
    for (const auto& location : {"http://net.szu.edu.cn/", "http://net.szu.edu.cn:80/srun_portal_phone", "https://net.szu.edu.cn", "https://net.szu.edu.cn/", "https://net.szu.edu.cn:443/srun_portal_pc?ac_id=7", "HTTPS://NET.SZU.EDU.CN/", "//net.szu.edu.cn/"})
        t.equal(wire::campus_redirect_hint(location, apple), std::optional<CampusPortalHint>(CampusPortalHint::teaching), "fixed HTTPS teaching homepage hint");
    for (const auto& location : {"http://172.30.255.42:801", "http://172.30.255.42:801/", "http://172.30.255.42:801/eportal?ignored=value"})
        t.equal(wire::campus_redirect_hint(location, microsoft), std::optional<CampusPortalHint>(CampusPortalHint::dormitory), "fixed HTTP dormitory homepage hint");
    for (const std::string location : {"http://net.szu.edu.cn:443/", "https://net.szu.edu.cn:80/", "https://net.szu.edu.cn:444/", "https://net.szu.edu.cn.evil.invalid/", "https://net.szu.edu.cn./", "https://user@net.szu.edu.cn/", "https://%6eet.szu.edu.cn/", "https://net.szu.edu.cn/%73run_portal_pc", "https://net.szu.edu.cn/a/../", "https://net.szu.edu.cn//", "https://net.szu.edu.cn/#", "https://net.szu.edu.cn/?x=%00", "https://net.szu.edu.cn/?x=%255c", "https://net.szu.edu.cn/?x=%GG", "https://net.szu.edu.cn/\n", "https://net.szu.edu.cn\\evil.invalid/", "http://172.30.255.42/eportal", "https://172.30.255.42:801/", "http://172.30.255.42:801/eportal/portal/login", "https://net.szu.edu.cn/cgi-bin/srun_portal"})
        t.check(!wire::campus_redirect_hint(location, microsoft), "non-homepage or abnormal destination has no candidate hint");
    for (const auto& source : {"https://custom.fixture.invalid/", "https://net.szu.edu.cn/", "https://www.apple.com/", "https://www.apple.com/library/test/success.html?changed=1", "http://www.msftconnecttest.com:81/connecttest.txt"})
        t.check(!wire::campus_redirect_hint("https://net.szu.edu.cn/", source), "custom website/system URLs cannot select a protocol");
    const CampusDiscovery both{Discovery::confirmed, Discovery::confirmed};
    t.check(resolve_campus_routing(CampusArea::automatic, hint(CampusPortalHint::teaching), both, false).choice == CampusArea::automatic,
        "a system-route hint never chooses the physical authentication protocol");
    t.check(resolve_campus_routing(CampusArea::automatic, hint(CampusPortalHint::dormitory),
        {Discovery::confirmed, Discovery::unconfirmed}, true).decision == RoutingDecision::unconfirmed,
        "dormitory hints require a separately verified applicable dormitory service");
    {
        Service base; base.response = ConnectivityReport::simple(false, false);
        InternetCheckService websites(base, [](const std::string&, bool, const CancelToken&) {
            WebsiteCheckResult result; result.state = "redirected"; result.status = 302; result.recovery_signal = WebsiteRecoverySignal::campusRedirect; return result;
        });
        websites.configure({{"fixture", "https://website.fixture.invalid/", true}}, true);
        const auto report = websites.probe(ProbeMode::complete, CancelToken{});
        t.check(!report.apple.campus_area_hint && !report.microsoft.campus_area_hint &&
            resolve_campus_routing(CampusArea::automatic, report, both, true).choice == CampusArea::automatic,
            "a custom website's campus recovery signal cannot choose a protocol");
    }

    t.begin("wired-unchanged-link-session-recovery");
    {
        Fixture f;
        const auto epoch = f.engine->snapshot().epoch;
        f.service.reports = {online(), hint(CampusPortalHint::teaching), hint(CampusPortalHint::teaching), online()};
        f.round(); t.check(f.engine->snapshot().online && f.service.attempts.empty(), "initially online cable does not authenticate");
        f.round(); t.check(f.service.attempts.empty() && f.engine->snapshot().status != EngineStatus::loggingIn, "one expired-session failure only confirms");
        const double verified_at = f.wall;
        f.round();
        t.check(f.engine->snapshot().online && f.service.attempts == std::vector<CampusArea>{CampusArea::teaching} && f.service.submissions == f.service.attempts,
            "two failures with both homepages reachable and teaching hint submit teaching once then verify online");
        t.check(f.engine->snapshot().epoch == epoch && f.service.fixture_credentials && !f.engine->snapshot().trusted,
            "same physical epoch recovers using fake credentials without creating session trust");
        const auto all_bound = [&](const auto& values) { return !values.empty() && std::all_of(values.begin(), values.end(), [&](const auto& value) { return value == f.route.interface_key; }); };
        t.check(all_bound(f.service.probe_bindings) && all_bound(f.service.discovery_bindings) && all_bound(f.service.attempt_bindings),
            "probe, independent discovery, authentication and verification use one physical binding");
        const auto saved = f.saved();
        t.check(saved.id == f.original.id && saved.features == f.original.features && saved.proof_teaching && saved.proof_dormitory && saved.area == "teaching",
            "accepted teaching changes preference while retaining identity features and proof flags");
        t.equal(saved.verified, verified_at, "fresh proof of both homepages may refresh their shared verification age");
    }
    {
        Fixture f; f.service.proof.dormitory = Discovery::notChecked;
        f.round(); f.round();
        const auto saved = f.saved();
        t.check(f.service.submissions == std::vector<CampusArea>{CampusArea::teaching} && f.engine->snapshot().online,
            "teaching-only independent proof supports one teaching recovery");
        t.check(saved.area == "teaching" && saved.created == f.original.created && saved.verified == f.original.verified &&
            saved.id == f.original.id && saved.features == f.original.features && saved.proof_dormitory,
            "teaching acceptance cannot renew historical dormitory proof or replace identity/features");
        t.check(f.engine->snapshot().auto_login == AutoLoginState::ready,
            "a teaching preference retaining old dormitory proof does not display a spurious dormitory-proof prerequisite");
    }
    for (const auto outcome : {Outcome::unavailable, Outcome::challengeRefused}) {
        Fixture f; f.service.outcome = outcome;
        f.round(); f.round();
        t.check(f.service.attempts == std::vector<CampusArea>{CampusArea::teaching} && f.service.submissions.empty() && f.service.discoveries.size() == 1,
            "hinted teaching preflight/challenge refusal never falls back to dormitory");
        t.equal(f.saved().area, std::string("dormitory"), "homepage discovery and unsent attempt preserve previous preference");
    }
    for (const auto choice : {CampusArea::teaching, CampusArea::dormitory}) {
        Fixture f(choice); f.service.response = hint(CampusPortalHint::teaching, CampusPortalHint::dormitory);
        f.round(); f.round();
        t.equal(f.service.submissions, std::vector<CampusArea>{choice}, "explicit protocol choice takes priority over conflicting hints");
    }
    {
        Fixture f; f.service.reports = {hint(CampusPortalHint::teaching), ConnectivityReport::simple(false, false), online()};
        f.round(); f.round();
        t.check(f.service.submissions.empty() && f.service.attempts.empty() && f.engine->snapshot().needs_selection,
            "without a current candidate dual-service discovery requires selection and never reuses the previous round's teaching hint");
    }
    {
        Fixture f(CampusArea::automatic, "teaching");
        f.service.response = hint(CampusPortalHint::dormitory, CampusPortalHint::teaching);
        f.round(); f.round();
        t.check(f.service.submissions.empty() && f.service.attempts.empty() && f.engine->snapshot().conflicting_campus_hints &&
            f.service.discoveries == std::vector<CampusArea>{CampusArea::automatic},
            "current hint conflicts outrank remembered teaching preference and historical dormitory proof without any submission");
    }
    for (const std::string condition : {"conflict", "identity", "unknown", "credentials", "readOnly"}) {
        Fixture f(CampusArea::automatic, "dormitory", condition == "readOnly", condition != "unknown", condition != "credentials");
        if (condition == "conflict") f.service.response = hint(CampusPortalHint::teaching, CampusPortalHint::dormitory);
        if (condition == "identity") f.service.proof.teaching = Discovery::unconfirmed;
        f.round(); f.round();
        t.check(f.service.submissions.empty(), condition + " cannot gain authentication authority from a redirect");
        if (condition == "conflict") t.check(f.engine->snapshot().conflicting_campus_hints, "conflicting current hints are exposed explicitly");
        if (condition == "readOnly") {
            t.equal(f.memory_writes, 0, "read-only fixture writes no memory");
            t.check(f.journal.empty(), "read-only fixture writes no recovery journal");
        }
    }
    for (const auto reason : {HoldReason::credentials, HoldReason::security, HoldReason::uncertain, HoldReason::userPaused}) {
        Fixture f;
        f.engine->restore_recovery("{\"schema\":1,\"generation\":1,\"holds\":[{\"reason\":" + std::to_string(static_cast<int>(reason)) + ",\"origin\":2}]}");
        f.round(); f.round();
        t.check(f.service.submissions.empty() && !f.engine->snapshot().holds.empty(), "credential security uncertain and user holds remain effective");
    }
    {
        Fixture f;
        f.round(); f.round();
        f.service.response = hint(CampusPortalHint::teaching);
        f.engine->automatic_round(); f.engine->automatic_round();
        t.equal(f.service.submissions.size(), std::size_t(1), "accepted session retains cooldown protection against a repeated submission");
    }
    for (const std::string phase : {"probe", "discovery", "attempt"}) {
        for (const bool change_path : {false, true}) {
            Fixture f;
            const auto mutate = [&] {
                if (change_path) { auto path = f.path(); path.physical_candidates.front().identity = "wired-session-fixture-B"; f.engine->receive_path(path); }
                else f.engine->cancel();
            };
            if (phase == "probe") f.service.during_probe = mutate;
            if (phase == "discovery") f.service.during_discovery = mutate;
            if (phase == "attempt") f.service.during_attempt = mutate;
            f.round(); f.round();
            t.check(!f.engine->snapshot().last_attempt && !f.engine->snapshot().last_success_area && !f.engine->snapshot().online,
                "late " + phase + " response cannot publish after cancellation or physical change");
            if (phase != "attempt") t.check(f.service.submissions.empty(), "cancellation/path change before submission sends nothing");
            else t.check(f.engine->snapshot().cooldown_remaining.has_value(), "late submission preserves unknown-result cooldown");
        }
    }
    {
        Fixture f; f.service.outcome = Outcome::rejected; f.service.rejection = RejectionKind::areaMismatch;
        f.round(); f.round();
        const auto holds = f.engine->snapshot().holds;
        t.check(holds.size() == 1 && holds.front().origin == HoldOrigin::candidate && holds.front().epoch.has_value(),
            "hint-derived teaching failure retains the original automatic selection's candidate hold");
    }
    {
        Fixture f(CampusArea::automatic, "teaching");
        f.service.response = ConnectivityReport::simple(false, false);
        f.service.outcome = Outcome::rejected; f.service.rejection = RejectionKind::areaMismatch;
        f.round(); f.round();
        const auto before = f.engine->snapshot();
        t.check(before.holds.size() == 1 && before.holds.front().origin == HoldOrigin::candidate &&
            before.holds.front().area == CampusArea::teaching && before.holds.front().epoch == before.epoch &&
            !before.holds.front().sticky() && before.holds.front().rejection == "areaMismatch",
            "remembered teaching pin retains the original automatic area's nonsticky candidate classification after regional refusal");
        t.check(f.service.submissions == std::vector<CampusArea>{CampusArea::teaching} &&
            f.service.discoveries == std::vector<CampusArea>{CampusArea::teaching},
            "regional refusal of a remembered teaching pin neither discovers nor submits a second protocol");
        const auto saved = f.saved();
        t.check(saved.id == f.original.id && saved.features == f.original.features && saved.area == "teaching" &&
            saved.proof_teaching && saved.proof_dormitory && saved.verified == f.original.verified && f.memory_writes == 0,
            "regional refusal preserves the teaching preference, identity, feature scope and historical dormitory proof age");
        auto next = f.path();
        next.identity = "wired-session-other-global";
        next.physical_candidates.front().identity = "wired-session-other-attachment";
        next.physical_candidates.front().descriptor.raw["gw"] = "a4:5e:60:91:19:02";
        next.descriptors = {next.physical_candidates.front().descriptor};
        f.engine->receive_path(next);
        t.check(f.engine->snapshot().epoch != before.epoch && f.engine->snapshot().holds.empty() &&
            f.service.submissions.size() == 1,
            "a different physical attachment releases a remembered teaching area's network-scoped refusal without another submission");
    }

    t.begin("wired-campus-service-only-check");
    {
        Fixture f; f.engine->pause_auto_login();
        const auto before = f.engine->snapshot(); const auto memory = f.saved(); const auto journal = f.journal;
        auto request = f.engine->prepare_verify_campus();
        t.check(request.has_value() && f.engine->snapshot().verifying_campus, "remembered cable offers a cancellable service-only action even while login is paused");
        if (request) t.equal(f.engine->verify_campus(*request), ConnectionEngine::CampusVerificationResult::confirmed, "service-only check independently confirms current campus homepages");
        const auto after = f.engine->snapshot();
        t.check(f.service.attempts.empty() && f.service.probe_bindings.empty() && f.memory_writes == 0 && f.journal == journal &&
            !after.trusted && after.holds.size() == before.holds.size() && after.next_check_seconds == before.next_check_seconds &&
            f.saved().verified == memory.verified && f.saved().area == memory.area,
            "service-only check changes no credentials, trust, memory scope/age, protection, detection report or recovery cadence");
    }
    for (const bool change_path : {false, true}) {
        Fixture f; auto request = f.engine->prepare_verify_campus();
        f.service.during_discovery = [&] {
            if (change_path) { auto path = f.path(); path.physical_candidates.front().identity = "changed-fixture"; f.engine->receive_path(path); }
            else f.engine->cancel();
        };
        if (request) t.equal(f.engine->verify_campus(*request), ConnectionEngine::CampusVerificationResult::cancelled, "late service-only evidence is discarded after cancel or path change");
        t.check(!f.engine->snapshot().last_discovery && f.memory_writes == 0 && f.service.attempts.empty(), "cancelled service check retains no late proof and performs no writes/login");
    }
    {
        Fixture f; f.engine->automatic_round();
        const auto before = f.engine->snapshot();
        auto request = f.engine->prepare_verify_campus();
        if (request) f.engine->verify_campus(*request);
        const auto after = f.engine->snapshot();
        t.check(before.status == EngineStatus::confirming && before.next_check_seconds == 1 && after.status == before.status &&
            after.next_check_seconds == before.next_check_seconds && f.service.attempts.empty(),
            "service-only completion preserves a rapid confirmation delay rather than resetting to an immediate round");
        request = f.engine->prepare_verify_campus();
        f.service.during_discovery = [&] { f.engine->restore_memory(std::nullopt); };
        if (request) t.equal(f.engine->verify_campus(*request), ConnectionEngine::CampusVerificationResult::cancelled,
            "memory replacement invalidates a service-only request");
        t.check(!f.engine->snapshot().busy && !f.engine->snapshot().verifying_campus,
            "memory replacement cannot strand a service-only operation busy");
    }
    for (const bool read_only : {false, true}) {
        Fixture f(CampusArea::automatic, "dormitory", read_only, false);
        t.check(!f.engine->prepare_verify_campus(), "unknown/read-only network cannot use remembered-service verification");
    }
    {
        Fixture f; f.service.response = online(); f.engine->automatic_round();
        const auto report = f.engine->snapshot().last_report; const auto calls = f.service.probe_bindings.size();
        f.engine->set_probe_mode(ProbeMode::quick); f.engine->set_probe_mode(ProbeMode::complete);
        const auto after = f.engine->snapshot();
        t.check(after.last_report && report && after.last_report->mode == report->mode && after.last_report->apple.state == report->apple.state &&
            f.service.probe_bindings.size() == calls, "switching quick/complete changes preference without requests or rewriting prior results");
    }

    t.begin("network-label-merge-preserves-presentation");
    {
        NetworkLabels labels;
        labels.update_automatic("1234abcd", {"Wired fixture", "ethernet", std::nullopt, false});
        labels.update_automatic("5678abcd", {"Fixture WLAN", "wifi", "Fixture WLAN", false});
        labels.rename("5678abcd", "My fixture network");
        t.equal(labels.preserve_merged_metadata("1234abcd", {"5678abcd"}), NetworkLabels::MergeResult::updated,
            "duplicate consolidation preserves a removed record's manual name and Wi-Fi label");
        const auto surviving = labels.entries().at("1234abcd");
        t.check(surviving.manual_name && surviving.display_name == "My fixture network" && surviving.ssid == "Fixture WLAN" &&
            labels.entries().count("5678abcd") == 1, "metadata preservation keeps source entries until memory commit");
        labels.rename("5678abcd", "Different fixture name");
        const auto before = labels.encode();
        t.equal(labels.preserve_merged_metadata("1234abcd", {"5678abcd"}), NetworkLabels::MergeResult::conflict,
            "different manual names cannot be silently discarded during consolidation");
        t.equal(labels.encode(), before, "conflicting label merge is atomic");
        labels.update_automatic("9abcabcd", {"Other fixture WLAN", "wifi", "Other fixture WLAN", false});
        t.equal(labels.preserve_merged_metadata("1234abcd", {"9abcabcd"}), NetworkLabels::MergeResult::conflict,
            "different remembered Wi-Fi names are not guessed during a merge");
    }
}
