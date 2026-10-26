#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

inline void session_expiry_recovery_tests(Harness& t) {
    using namespace wiredtest;
    const std::string probe = "http://www.msftconnecttest.com/connecttest.txt";
    auto expired = [&](std::string_view location, bool apple_online = false) {
        auto report = ConnectivityReport::simple(apple_online, false);
        report.mode = ProbeMode::complete;
        report.microsoft.state = EndpointState::redirect;
        report.microsoft.campus_area_hint = wire::campus_redirect_hint(location, probe);
        report.microsoft.authentication_required = wire::campus_authentication_required(location, probe);
        return report;
    };
    t.begin("dormitory-browser-portal-redirects");
    for (const auto* location : {
        "http://172.30.255.42/a70.htm?wlanuserip=192.0.2.7&wlanacname=&wlanacip=192.0.2.1",
        "http://172.30.255.42:80/a79.htm?wlanuserip=192.0.2.7&wlanacname=fixture",
        "http://172.30.255.42/a79.htm", "http://172.30.255.42/a70.htm"}) {
        t.equal(wire::campus_redirect_hint(location, probe), std::optional<CampusPortalHint>(CampusPortalHint::dormitory),
            "fixed dormitory browser pages provide an enumerated recovery candidate");
        t.check(!wire::is_allowed(location), "a browser redirect is never an allowed credential or follow-up target");
    }
    t.check(wire::campus_authentication_required("http://172.30.255.42/a70.htm", probe) &&
        !wire::campus_authentication_required("http://172.30.255.42/a79.htm", probe) &&
        !wire::campus_authentication_required("https://net.szu.edu.cn/srun_portal_success", probe),
        "a login-required redirect is distinct from an authentication success page");
    for (const auto* location : {
        "https://172.30.255.42/a70.htm", "http://172.30.255.42:81/a70.htm", "http://172.30.255.42:801/a79.htm",
        "http://172.30.255.42/a70.htm/", "http://172.30.255.42/%6170.htm", "http://172.30.255.42/a70.htm#x",
        "http://172.30.255.42.evil.invalid/a70.htm", "http://user@172.30.255.42/a79.htm",
        "http://172.30.255.42/a70.htm?x=%0a", "http://172.30.255.42/a70.htm?x=%255c",
        "http://172.30.255.42/a70.htm?x=%GG", "http://172.30.255.42/login", "http://172.30.255.42/logout"})
        t.check(!wire::campus_redirect_hint(location, probe), "unrecognized or malformed dormitory browser destinations supply no candidate");
    t.check(!wire::campus_redirect_hint("http://172.30.255.42/a70.htm", "https://website.fixture.invalid/"),
        "custom websites cannot contribute dormitory selection");

    t.begin("unchanged-link-session-expiry");
    for (const auto* location : {"http://172.30.255.42/a70.htm?wlanuserip=192.0.2.7", "https://net.szu.edu.cn/srun_portal_pc?ac_id=7"}) {
        Fixture f;
        const auto epoch = f.engine->snapshot().epoch;
        f.service.reports = {online(), expired(location), expired(location), online()};
        f.round(); f.round();
        t.check(f.service.submissions.empty(), "first session-expiry observation does not submit credentials");
        f.round();
        const auto expected = std::string_view(location).find("net.szu.edu.cn") != std::string_view::npos ? CampusArea::teaching : CampusArea::dormitory;
        t.check(f.service.submissions == std::vector<CampusArea>{expected} && f.engine->snapshot().online,
            "expired campus sessions recover once after confirmation without disconnecting the physical link");
        t.equal(f.engine->snapshot().epoch, epoch, "session recovery does not require a synthetic network change");
    }

    t.begin("quick-check-portal-evidence");
    {
        Fixture f; f.engine->set_probe_mode(ProbeMode::quick);
        f.service.reports = {expired("http://172.30.255.42:801/eportal", true), expired("http://172.30.255.42:801/eportal", true), online()};
        f.round(); f.round();
        t.check(f.service.submissions == std::vector<CampusArea>{CampusArea::dormitory} && f.engine->snapshot().online,
            "a reachable public endpoint cannot hide repeated bound campus redirects in quick checks");
        t.equal(f.engine->snapshot().probe_mode, ProbeMode::quick, "session recovery keeps the user's quick selection");
    }
    t.begin("quick-check-session-audit");
    {
        Fixture f; f.engine->set_probe_mode(ProbeMode::quick);
        f.service.response = online(); f.round();
        f.service.response = expired("http://172.30.255.42:801/eportal", true);
        f.clock = 1061; f.wall += 61; f.round();
        t.check(f.service.probe_modes.back() == ProbeMode::complete && f.service.submissions.empty(),
            "quick monitoring periodically waits for both base endpoints before confirming session loss");
        t.check(f.service.deferred_website_scopes.back(), "the periodic base audit never expands quick checks to custom websites");
        f.round();
        t.check(f.service.submissions == std::vector<CampusArea>{CampusArea::dormitory} && f.engine->snapshot().online,
            "a persistent captive redirect behind an allowed public endpoint admits one recovery");
        t.equal(f.engine->snapshot().probe_mode, ProbeMode::quick, "the audit does not save a different probe preference");
    }
    t.begin("session-audit-needs-expiry-evidence");
    for (const auto mode : {ProbeMode::quick, ProbeMode::complete}) for (const bool success_page : {false, true}) {
        Fixture f(CampusArea::automatic, "teaching"); f.engine->set_probe_mode(mode);
        f.service.response = online(); f.round();
        f.service.response = success_page ? expired("http://172.30.255.42/a79.htm", true) : ConnectivityReport::simple(true, false);
        for (int round = 0; round < 8; ++round) { f.clock += 61; f.wall += 61; f.round(); }
        t.check(f.service.submissions.empty() && f.service.attempts.empty(),
            "a periodic audit cannot force login from a single endpoint failure or a success-page redirect");
    }
    t.begin("symmetric-remembered-entry-selection");
    for (const auto area : {CampusArea::teaching, CampusArea::dormitory}) {
        Fixture f(CampusArea::automatic, std::string(to_string(area)));
        f.service.response = ConnectivityReport::simple(false, false);
        f.round(); f.round();
        t.check(f.service.submissions == std::vector<CampusArea>{area} && !f.engine->snapshot().needs_selection,
            "current valid remembered teaching and dormitory entries are symmetric when fresh redirects are absent");
    }
    for (const std::string boundary : {"expired", "revoked", "weak", "proof"}) {
        Fixture f;
        if (boundary == "expired") f.wall += NetworkMemory::kExpirySeconds + 1;
        if (boundary == "revoked") f.engine->forget_all_networks();
        if (boundary == "weak") f.route.descriptor.raw = {{"net", "192.0.2.0/24"}};
        if (boundary == "proof") {
            NetworkMemory memory("00112233445566778899aabbccddeeff");
            memory.remember(f.route.descriptor, "dormitory", true, false, f.wall, "b184abcd");
            f.engine->restore_memory(memory.encode());
        }
        f.engine->receive_path(f.path());
        f.service.response = ConnectivityReport::simple(false, false);
        f.round(); f.round();
        t.check(f.service.submissions.empty(), "remembered dormitory selection cannot borrow " + boundary + " authority");
    }
    {
        Fixture f; f.service.proof.teaching_client_bound = true;
        f.service.response = ConnectivityReport::simple(false, false);
        f.round(); f.round();
        t.check(f.service.submissions.empty() && f.engine->snapshot().last_attempt &&
            f.engine->snapshot().last_attempt->error == PortalError::protocolConflict,
            "a fresh exact-source teaching configuration outranks old dormitory memory and preserves its strict attempt requirement");
    }
    {
        Fixture f; f.service.response = online();
        const auto memory = f.saved(); f.round();
        t.check(f.engine->snapshot().required_campus_confirmed && !f.engine->snapshot().needs_selection,
            "automatic service verification and entry readiness agree for a known dormitory connection");
        const auto request = f.engine->prepare_verify_campus();
        t.check(request && f.engine->verify_campus(*request) == ConnectionEngine::CampusVerificationResult::confirmed &&
            !f.engine->snapshot().needs_selection && f.saved().area == memory.area && f.saved().verified == memory.verified,
            "independent verification reuses the same selection without changing preferred entry or proof age");
    }
    t.begin("quick-check-repeated-session-expiry");
    {
        Fixture f; f.engine->set_probe_mode(ProbeMode::quick);
        f.service.response = online(); f.round();
        for (int episode = 0; episode < 12; ++episode) {
            f.clock += 100; f.wall += 100;
            f.service.response = expired("http://172.30.255.42/a70.htm", true);
            f.round(); f.round();
            t.check(f.engine->snapshot().online && f.service.submissions.size() == static_cast<std::size_t>(episode + 1),
                "each later session expiry can recover after earlier recovery was independently verified");
            const auto deadline = f.engine->snapshot().cooldown_remaining;
            t.check(deadline && *deadline > 0, "successful session recovery retains its original cooldown");
            f.round(); f.round();
            t.check(f.service.submissions.size() == static_cast<std::size_t>(episode + 1),
                "healthy base confirmation rearms future recovery without authenticating again");
        }
    }
    t.begin("remembered-campus-handover-session-expiry");
    {
        Fixture f(CampusArea::automatic, "teaching");
        const auto teaching_route = f.route;
        auto dormitory_attachment = physical_attachment("dormitory-session-fixture");
        dormitory_attachment.route.interface_index = 9;
        dormitory_attachment.route.interface_luid = 900;
        dormitory_attachment.route.source_ipv4 = "192.0.3.17";
        dormitory_attachment.route.addresses = {"192.0.3.17"};
        dormitory_attachment.route.gateway_ipv4 = "192.0.3.1";
        dormitory_attachment.bound_default_gateways = {"192.0.3.1"};
        dormitory_attachment.route.dns_servers = {"192.0.3.53"};
        dormitory_attachment.route.descriptor.kind = "wifi";
        dormitory_attachment.route.descriptor.raw = {{"ssid", "fixture-dormitory"}, {"gw", "02:00:00:00:03:01"},
            {"gwip", "192.0.3.1"}, {"net", "192.0.3.0/24"}, {"dns", "192.0.3.53"}};
        const auto dormitory_route = physical::candidates({dormitory_attachment}).front();
        NetworkMemory memories("00112233445566778899aabbccddeeff");
        memories.remember(teaching_route.descriptor, "teaching", true, false, f.wall - 100, "a184abcd");
        memories.remember(dormitory_route.descriptor, "dormitory", true, true, f.wall - 100, "b184abcd");
        f.engine->restore_memory(memories.encode());
        f.service.response = online(); f.round();
        f.route = dormitory_route;
        f.engine->receive_path(f.path());
        t.check(f.engine->snapshot().remembered, "returning to an independently remembered dormitory keeps its own network eligibility");
        f.service.response = expired("http://172.30.255.42/a70.htm");
        f.round(); f.round();
        t.check(f.engine->snapshot().online && f.service.submissions == std::vector<CampusArea>{CampusArea::dormitory} &&
            f.service.attempt_bindings == std::vector<std::string>{dormitory_route.interface_key},
            "return from teaching to dormitory restores the campus session on the new physical binding without borrowing the teaching preference");
        f.route = teaching_route; f.engine->receive_path(f.path());
        f.service.response = expired("https://net.szu.edu.cn/srun_portal_pc");
        f.clock += 100; f.wall += 100; f.round(); f.round();
        t.check(f.engine->snapshot().online && f.service.submissions == std::vector<CampusArea>{CampusArea::dormitory, CampusArea::teaching},
            "returning to teaching again selects its own remembered entrance and fresh portal proof");
    }
    t.begin("session-expiry-protection-boundaries");
    {
        Fixture f;
        f.service.during_discovery = [] { throw std::runtime_error("fixture service failure"); };
        f.round(); f.round();
        t.check(!f.engine->snapshot().busy && f.service.submissions.empty() && f.engine->snapshot().holds.empty(),
            "an exception before submission releases operation ownership without inventing a credential rejection");
        f.service.during_discovery = {}; f.round();
        t.check(f.service.submissions.size() == 1 && f.engine->snapshot().online && !f.engine->snapshot().busy,
            "a later automatic round recovers after a failed service task without manual resume");
    }
    {
        Fixture f;
        f.service.during_attempt = [] { throw std::runtime_error("fixture submit interruption"); };
        f.round(); f.round();
        t.check(!f.engine->snapshot().busy && f.service.submissions.size() == 1 &&
            f.engine->snapshot().pause_reason == HoldReason::uncertain && f.engine->snapshot().cooldown_remaining,
            "an exception after the submission gate retains uncertainty and its original cooldown");
        f.service.during_attempt = {}; f.service.response = hint(CampusPortalHint::teaching);
        f.clock += 100; f.round();
        t.check(f.service.submissions.size() == 1 && f.engine->snapshot().pause_reason == HoldReason::uncertain,
            "a task exception cannot replay an unconfirmed submission after the cooldown ends");
    }
    for (const std::string condition : {"readOnly", "observe", "pause", "cooldown", "credentials", "unknown", "proof"}) {
        Fixture f(CampusArea::automatic, "dormitory", condition == "readOnly", condition != "unknown", condition != "credentials");
        f.engine->set_probe_mode(ProbeMode::quick);
        if (condition == "observe") f.engine->set_mode(MonitoringMode::observe);
        if (condition == "pause") f.engine->pause_auto_login();
        if (condition == "cooldown") f.engine->restore_recovery(std::string("{\"schema\":1,\"generation\":1,\"holds\":[],\"notBefore\":1800001000}"));
        if (condition == "proof") f.service.proof = {Discovery::unconfirmed, Discovery::confirmed};
        f.service.response = expired("http://172.30.255.42/a70.htm", true);
        f.round(); f.round();
        t.check(f.service.submissions.empty(), "a session-expiry hint cannot override " + condition);
    }
    for (const std::string boundary : {"cancel", "path", "forget"}) {
        Fixture f; f.engine->set_probe_mode(ProbeMode::quick);
        f.service.response = expired("http://172.30.255.42/a70.htm", true);
        f.service.during_discovery = [&] {
            if (boundary == "cancel") f.engine->cancel();
            else if (boundary == "forget") f.engine->forget_all_networks();
            else { auto changed = f.path(); changed.physical_candidates.clear(); f.engine->receive_path(changed); }
        };
        f.round(); f.round();
        t.check(f.service.submissions.empty(), "late session discovery cannot submit after " + boundary);
    }
}
