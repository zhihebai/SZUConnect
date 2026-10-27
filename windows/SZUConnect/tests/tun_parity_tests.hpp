#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
// Real engine + decorator with controlled physical attachments; no OS networking or credentials.
struct TunParityService final : ConnectionService {
    std::optional<physical::Route> bound;
    std::vector<std::string> bindings, submissions;
    std::vector<ProbeMode> modes;
    ConnectivityReport response = ConnectivityReport::simple(false, false);
    CampusDiscovery proof{Discovery::confirmed, Discovery::confirmed};
    bool accepted = true;
    bool online_after_attempt = true;
    Outcome outcome = Outcome::success;
    int discoveries = 0;
    std::function<void()> during_binding, during_discovery;
    bool set_expected_physical_route(const std::optional<physical::Route>& route) override {
        bound = route; bindings.push_back(route ? route->interface_key : "system");
        const bool result = accepted;
        const auto callback = during_binding;
        if (callback) callback();
        return result;
    }
    ConnectivityReport probe(ProbeMode mode, const CancelToken&) override {
        modes.push_back(mode); auto r = response; r.mode = mode; return r;
    }
    CampusDiscovery discover(CampusArea, const CancelToken&) override {
        ++discoveries;
        if (during_discovery) during_discovery();
        return proof;
    }
    LoginAttempt attempt(const Credentials&, CampusArea, const CancelToken&) override {
        if (online_after_attempt) response = ConnectivityReport::simple(true, true);
        LoginAttempt reply; reply.outcome = outcome;
        reply.credential_submission_started = outcome != Outcome::unavailable && outcome != Outcome::challengeRefused;
        if (reply.credential_submission_started) submissions.push_back(bound ? bound->interface_key : "system");
        if (outcome == Outcome::rejected) reply.rejection = RejectionKind::password;
        return reply;
    }
};
struct TunParityFixture {
    double clock = 1000;
    double wall = 1800000000;
    TunParityService base;
    int website_calls = 0, memory_writes = 0, changes = 0;
    WebsiteCheckResult website;
    InternetCheckService service;
    std::unique_ptr<ConnectionEngine> engine;
    std::string persisted, memory_persisted;
    physical::Route wired, wifi;
    TunParityFixture() : service(base, [this](const auto&, bool, const auto&) {
        ++website_calls; return website;
    }) {
        wired.interface_key = "fixture-wired"; wired.interface_index = 12; wired.interface_luid = 12;
        wired.source_ipv4 = "192.0.2.7"; wired.addresses = {wired.source_ipv4}; wired.dns_servers = {"192.0.2.53"};
        wired.gateway_ipv4 = "192.0.2.1"; wired.identity = "fixture-wired-A"; wired.display_name = "Ethernet";
        wired.descriptor = mem_desc("a4:5e:60:93:01:01", "192.0.2.1", "192.0.2.0/24"); wired.descriptor.kind = "ethernet";
        wired.is_system_primary = true;
        wifi = wired; wifi.interface_key = "fixture-wifi"; wifi.interface_index = 1; wifi.interface_luid = 1;
        wifi.identity = "fixture-wifi-A"; wifi.display_name = "Wi-Fi"; wifi.descriptor.kind = "wifi";
        wifi.descriptor.raw["gw"] = "a4:5e:60:93:01:02"; wifi.is_system_primary = false;
        website.state = "timeout"; website.recovery_signal = WebsiteRecoverySignal::connectionFailure;
        service.configure({{"site", "https://parity.fixture.invalid/", true}}, true);
        ConnectionEngine::Hooks hooks;
        hooks.now = [this] { return clock; }; hooks.wall_now = [this] { return wall; };
        hooks.sleep = [](double) {}; hooks.random_hex = [] { return "00112233445566778899aabbccddeeff"; };
        hooks.persist_memory = [this](const auto& value) { ++memory_writes; memory_persisted = value; return true; };
        hooks.persist_recovery = [this](const auto& value) { persisted = value; return true; };
        hooks.changed = [this] { ++changes; };
        engine = std::make_unique<ConnectionEngine>(service, hooks, false);
        engine->restore_recovery(std::nullopt);
        engine->configure(MonitoringMode::recover, CampusArea::dormitory, ProbeMode::complete, Credentials{"fixture-user", "fixture-password"});
        NetworkMemory memory("00112233445566778899aabbccddeeff");
        memory.remember(wired.descriptor, "dormitory", true, true, wall, "1234abcd");
        memory.remember(wifi.descriptor, "dormitory", true, true, wall, "5678abcd");
        engine->restore_memory(memory.encode());
        engine->receive_path(path());
    }
    PathSnapshot path() const {
        PathSnapshot p; p.usable = true; p.identity = "fixture-global-topology"; p.link_active = true;
        p.tunnel = true; p.interfaces = "Ethernet + Wi-Fi";
        p.descriptors = {wired.descriptor, wifi.descriptor};
        p.requires_physical_binding = true; p.physical_candidates = {wired, wifi}; return p;
    }
    void advance(double seconds) { clock += seconds; wall += seconds; }
};
inline void bound_website_presentation_tests(Harness& t) {
    t.begin("bound-website-status-actions");
    for (const bool follow_system_proxy : {false, true}) {
        for (const auto& website : std::vector<WebsiteCheckResult>{
                 {"timeout", 0, 0, WebsiteRecoverySignal::connectionFailure},
                 {"dns", 0, 0, WebsiteRecoverySignal::connectionFailure},
                 {"forbidden", 403, 0}, {"redirected", 302, 0}}) {
            TunParityFixture f; f.base.response = ConnectivityReport::simple(true, true); f.website = website;
            f.service.configure({{"site", "https://status.fixture.invalid/", true}}, follow_system_proxy);
            f.engine->check();
            const auto snapshot = f.engine->snapshot(); const auto input = status::input_from(snapshot);
            const auto presentation = status::present(input);
            t.check(snapshot.authentication_route && snapshot.online && input.bound_website_only_issues && !input.recovery_evidence &&
                    presentation.primary == "check" && presentation.secondary == "network" &&
                    presentation.headline == "internet.websiteIssues.title" && f.base.submissions.empty(),
                "a bound optional-website issue preserves basic connectivity and offers checking and diagnostics instead of campus login");
            auto forced = snapshot; forced.offer_forced_login = true;
            t.check(status::present(status::input_from(forced)).primary == "check" &&
                    status::present(status::input_from(forced)).secondary == "network",
                "a previous force-login offer cannot turn an optional website problem into a campus-login suggestion");
            if (website.state == "timeout" || website.state == "dns") {
                auto portable = snapshot; portable.authentication_route.reset();
                t.check(!status::input_from(portable).bound_website_only_issues &&
                        status::present(status::input_from(portable)).primary == "partialLogin",
                    "unbound portable website recovery keeps its existing explicit login presentation");
                f.engine->connect();
                t.equal(f.base.submissions.size(), std::size_t(1),
                    "hiding a website-driven suggestion does not disable an explicitly requested manual login");
            }
        }
    }
    for (const bool portal_redirect : {false, true}) {
        TunParityFixture f; f.base.response = ConnectivityReport::simple(true, false);
        f.base.response.microsoft.state = portal_redirect ? EndpointState::redirect : EndpointState::timeout;
        if (portal_redirect) f.base.response.microsoft.campus_area_hint = CampusPortalHint::teaching;
        f.engine->check();
        const auto input = status::input_from(f.engine->snapshot()); const auto presentation = status::present(input);
        t.check(!input.bound_website_only_issues && input.recovery_evidence && presentation.primary == "partialLogin" &&
                presentation.secondary == "check",
            "a current physical portal redirect or partial base failure retains the campus recovery action");
    }
    {
        TunParityFixture f; f.base.response = ConnectivityReport::simple(true, true);
        f.website = {"redirected", 302, 0, WebsiteRecoverySignal::campusRedirect}; f.engine->check();
        const auto input = status::input_from(f.engine->snapshot());
        t.check(!input.bound_website_only_issues && status::present(input).primary == "partialLogin",
            "a current validated campus redirect remains a recovery candidate rather than an ordinary website-only issue");
    }
    {
        TunParityFixture f; f.base.response = ConnectivityReport::simple(true, true); f.engine->check();
        auto snapshot = f.engine->snapshot(); snapshot.status = EngineStatus::checking;
        t.check(status::present(status::input_from(snapshot)).primary == "cancel",
            "an older website-only result cannot replace the current busy cancellation action");
        snapshot.status = EngineStatus::paused; snapshot.pause_reason = HoldReason::userPaused;
        t.check(status::present(status::input_from(snapshot)).primary == "resume",
            "a website-only timeout does not replace an existing pause with a login suggestion");
        snapshot.status = EngineStatus::coolingDown;
        t.check(status::present(status::input_from(snapshot)).primary == "check",
            "a website-only timeout keeps the cooldown check action");
    }
}

inline void tun_attachment_reacquisition_tests(Harness& t) {
    t.begin("tun-attachment-reacquisition");
    {
        TunParityFixture f; f.base.response = ConnectivityReport::simple(true, true);
        t.check(f.engine->trust_current_network(), "stable physical fixture grants only explicit session approval");
        f.engine->check();
        const auto before = f.engine->snapshot();
        for (const bool tunnel : {false, true, false, true}) {
            auto path = f.path(); path.tunnel = tunnel; path.identity = "changed-virtual-default-route";
            path.usable = false; path.interfaces = "changed-system-interface-list";
            f.engine->receive_path(path);
            const auto after = f.engine->snapshot();
            t.check(after.epoch == before.epoch && after.trusted && after.remembered && after.required_campus_confirmed &&
                    after.authentication_route == before.authentication_route && after.discovery_revision == before.discovery_revision,
                "TUN/system-route changes preserve established session approval and fresh proof when the exact physical binding remains valid");
        }
        t.check(f.memory_writes == 0 && f.base.submissions.empty(),
            "stable virtual-route changes neither rewrite network memory nor authenticate");
    }
    for (const bool conflicting_snapshot : {false, true}) {
        TunParityFixture f; f.base.response = ConnectivityReport::simple(true, true);
        f.engine->trust_current_network(); f.engine->check();
        const auto original = f.engine->snapshot();
        const auto records = f.engine->remembered_networks();
        auto unavailable = f.path(); unavailable.physical_candidates.clear();
        if (conflicting_snapshot) {
            auto conflict = f.wired; conflict.source_ipv4 = "192.0.2.8"; conflict.addresses = {conflict.source_ipv4};
            unavailable.physical_candidates = {f.wired, conflict};
        }
        f.engine->receive_path(unavailable);
        const auto rejected = f.engine->snapshot();
        const auto probes = f.base.modes.size(); const int discoveries = f.base.discoveries;
        f.engine->automatic_round(); f.engine->automatic_round();
        t.check(rejected.epoch != original.epoch && !rejected.trusted && !rejected.remembered && !rejected.authentication_route &&
                !rejected.required_campus_confirmed && rejected.memory_count == records.size() &&
                f.base.modes.size() == probes && f.base.discoveries == discoveries && f.base.submissions.empty(),
            "missing or contradictory physical snapshots revoke only current authority and never reuse a cached route for network work");
        f.engine->receive_path(f.path());
        t.check(f.engine->snapshot().remembered && !f.engine->snapshot().trusted && !f.engine->snapshot().required_campus_confirmed &&
                f.engine->snapshot().authentication_route == original.authentication_route,
            "a stable returning candidate rematches durable evidence without inheriting session approval or old campus proof");
        f.engine->automatic_round();
        const auto current = f.engine->remembered_networks();
        const bool unchanged = records.size() == current.size() && std::equal(records.begin(), records.end(), current.begin(),
            [](const auto& a, const auto& b) {
                return a.id == b.id && a.created == b.created && a.verified == b.verified && a.area == b.area &&
                    a.proof_teaching == b.proof_teaching && a.proof_dormitory == b.proof_dormitory && a.features == b.features;
            });
        t.check(f.engine->snapshot().online && f.engine->snapshot().required_campus_confirmed && !f.engine->snapshot().trusted &&
                f.base.modes.size() == probes + 1 && f.base.discoveries == discoveries + 1 && f.base.submissions.empty() &&
                unchanged && f.memory_writes == 0,
            "returning healthy attachment gets new independent proof without re-remembering, renewing old memory or submitting credentials");
    }
    for (const std::string boundary : {"gateway", "subnet", "sleep", "forget"}) {
        TunParityFixture f; f.engine->trust_current_network();
        auto next = f.path(); next.physical_candidates = {f.wired};
        if (boundary == "gateway") {
            next.physical_candidates[0].descriptor.raw["gw"] = "a4:5e:60:93:01:99";
            next.physical_candidates[0].identity = "fixture-conflicting-gateway";
        } else if (boundary == "subnet") {
            next.physical_candidates[0].descriptor.raw = {{"net", "198.51.100.0/24"}, {"gwip", "198.51.100.1"}};
            next.physical_candidates[0].identity = "fixture-different-attachment";
        } else if (boundary == "sleep") {
            f.engine->set_sleeping(true); f.engine->set_sleeping(false);
        } else f.engine->forget_all_networks();
        f.engine->receive_path(next);
        t.check(!f.engine->snapshot().trusted && !f.engine->snapshot().required_campus_confirmed,
            boundary + ": a genuine boundary cannot inherit session approval or a previous campus verification");
        if (boundary != "sleep") {
            f.engine->automatic_round(); f.engine->automatic_round();
            t.check(!f.engine->snapshot().remembered && f.base.submissions.empty() && f.base.discoveries == 0,
                boundary + ": a different or explicitly forgotten attachment cannot recover eligibility from a private subnet or old record");
        } else {
            f.base.response = ConnectivityReport::simple(true, true); f.engine->automatic_round();
            t.check(f.engine->snapshot().remembered && !f.engine->snapshot().trusted && f.base.discoveries == 1 && f.base.submissions.empty(),
                "wake reacquires durable eligibility through a new physical snapshot and independent credential-free proof");
        }
    }
    for (const std::string boundary : {"cancel", "unknown", "gateway", "sleep", "forget"}) {
        TunParityFixture f; f.engine->automatic_round();
        f.base.during_discovery = [&] {
            if (boundary == "cancel") f.engine->cancel();
            else if (boundary == "sleep") f.engine->set_sleeping(true);
            else if (boundary == "forget") f.engine->forget_current_network();
            else {
                auto path = f.path();
                if (boundary == "unknown") path.physical_candidates.clear();
                else { path.physical_candidates[0].descriptor.raw["gw"] = "a4:5e:60:93:01:99";
                       path.physical_candidates[0].identity = "fixture-new-gateway"; }
                f.engine->receive_path(path);
            }
        };
        f.engine->automatic_round();
        t.check(f.base.submissions.empty() && !f.engine->snapshot().required_campus_confirmed,
            boundary + ": a late old discovery cannot authorize credentials after a cancelled or invalidated attachment");
    }
}

inline void tun_parity_tests(Harness& t) {
    bound_website_presentation_tests(t);
    tun_attachment_reacquisition_tests(t);
    t.begin("tun-physical-engine-parity");
    for (const bool wireless : {false, true}) {
        TunParityFixture f;
        f.engine->forget_all_networks();
        auto path = f.path(); path.physical_candidates = {wireless ? f.wifi : f.wired};
        f.engine->receive_path(path);
        f.engine->automatic_round();
        t.check(f.engine->snapshot().status == EngineStatus::confirming && f.engine->next_check_delay() == 1,
                "new untrusted attachment retains one prompt, credential-free failure confirmation");
        f.engine->automatic_round();
        const auto state = f.engine->snapshot();
        t.check(state.status == EngineStatus::needsTrust && state.auto_login == AutoLoginState::needsTrust &&
                !state.trusted && !state.remembered && state.can_trust && !state.cooldown_remaining,
                "two failed probes on an unknown bound attachment ask for trust without inventing campus identity");
        t.equal(f.engine->next_check_delay(), 30, "waiting for a trust decision uses normal cadence rather than a three-second recovery loop");
        t.check(f.base.discoveries == 0 && f.base.submissions.empty(),
                "unknown untrusted attachment cannot discover or submit through automatic recovery");
        f.engine->automatic_round();
        t.check(f.engine->snapshot().status == EngineStatus::needsTrust && f.engine->next_check_delay() == 30 &&
                f.base.discoveries == 0 && f.base.submissions.empty(),
                "repeated failed checks keep the same actionable trust state and quiet cadence");
        t.check(f.engine->trust_current_network(), "fixture grants explicit session trust on the current physical attachment");
        f.engine->automatic_round();
        t.check(f.engine->snapshot().online && f.base.discoveries == 1 && f.base.submissions.size() == 1 &&
                f.engine->snapshot().cooldown_remaining == 90.0,
                "explicit trust still requires fresh campus proof and permits one cooldown-protected submission");
        f.engine->automatic_round();
        t.equal(f.base.submissions.size(), std::size_t(1), "read-only post-recovery supplement never repeats the authentication");
    }
    {
        TunParityFixture f; f.engine->forget_all_networks();
        auto path = f.path(); path.physical_candidates = {f.wired}; f.engine->receive_path(path);
        f.base.response = ConnectivityReport::simple(true, true); f.engine->automatic_round();
        t.check(f.engine->snapshot().online && !f.engine->snapshot().trusted && !f.engine->snapshot().remembered &&
                f.base.discoveries == 0 && f.base.submissions.empty(),
                "successful basic connectivity on an unknown network neither requests login nor creates trust");
    }
    // Independent, settled physical interfaces must not share an artificial propagation wait.
    {
        TunParityFixture f;
        auto wired = f.path(); wired.physical_candidates = {f.wired};
        f.engine->receive_path(wired); f.engine->connect();
        auto state = f.engine->snapshot();
        t.check(state.online && state.auto_login == AutoLoginState::ready && state.cooldown_remaining == 90.0,
                "online physical path shows ready while its full duplicate-submit protection remains");
        auto wifi = f.path(); wifi.physical_candidates = {f.wifi};
        f.engine->receive_path(wifi); f.base.response = ConnectivityReport::simple(false, false);
        t.check(!f.engine->snapshot().cooldown_remaining && f.engine->next_check_delay() == 0,
                "settled Ethernet permits immediate independent Wi-Fi revalidation");
        f.engine->automatic_round();
        t.check(f.base.submissions.size() == 1 && f.engine->next_check_delay() == 1,
                "new Wi-Fi still requires two fresh failures before submission");
        f.engine->automatic_round();
        t.check(f.engine->snapshot().online && f.base.submissions == std::vector<std::string>{"fixture-wired", "fixture-wifi"},
                "fresh remembered Wi-Fi discovery can authenticate exactly once without waiting for Ethernet");
        f.engine->receive_path(wired); f.base.response = ConnectivityReport::simple(false, false);
        t.check(f.engine->snapshot().cooldown_remaining == 90.0 && f.engine->next_check_delay() == 0,
                "A-to-B-to-A preserves A deadline and still probes the new path immediately");
        f.engine->automatic_round(); f.engine->automatic_round();
        t.check(f.base.submissions.size() == 2 && f.engine->snapshot().status == EngineStatus::coolingDown,
                "switching back to A cannot replay authentication inside A deadline");
        f.advance(90); f.engine->automatic_round();
        t.check(f.base.submissions.size() == 3, "expired A deadline permits one fresh-proven authentication");
    }
    for (const auto failure : {Outcome::unavailable, Outcome::challengeRefused}) {
        TunParityFixture f; auto p = f.path(); p.physical_candidates = {f.wired};
        f.engine->receive_path(p); f.engine->connect(); f.advance(10);
        const auto deadline = json::parse(f.persisted)->number_at("notBefore");
        p.physical_candidates = {f.wifi}; f.engine->receive_path(p);
        f.base.response = ConnectivityReport::simple(false, false); f.base.outcome = failure; f.base.online_after_attempt = false;
        f.engine->automatic_round(); f.engine->automatic_round();
        t.check(!f.engine->snapshot().cooldown_remaining && json::parse(f.persisted)->number_at("notBefore") == deadline,
                "unsent independent-interface preflight restores settled scope and exact durable deadline");
        f.engine->receive_path(f.path());
        t.check(f.engine->snapshot().cooldown_remaining == 80.0,
                "unsent B preflight cannot extend or erase A's earlier deadline");
    }
    for (const bool wireless : {false, true}) {
        TunParityFixture f; auto p = f.path(); p.physical_candidates = {wireless ? f.wifi : f.wired};
        f.engine->receive_path(p); f.engine->connect();
        auto changed = p;
        changed.tunnel = !p.tunnel; changed.identity = "new-global-default-route";
        changed.physical_candidates.front().is_system_primary = !changed.physical_candidates.front().is_system_primary;
        f.engine->receive_path(changed);
        t.check(f.engine->snapshot().cooldown_remaining == 90.0,
                "TUN and primary-route metadata cannot renew a settled interface's submission permission");
        changed.physical_candidates.front().identity = "new-ssid-or-dhcp-lease";
        changed.physical_candidates.front().source_ipv4 = "192.0.2.8";
        changed.physical_candidates.front().addresses = {"192.0.2.8"};
        changed.physical_candidates.front().descriptor.raw["net"] = "192.0.2.0/25";
        f.engine->receive_path(changed);
        t.check(f.engine->snapshot().cooldown_remaining == 90.0,
                "same hardware interface with another SSID/subnet or DHCP lease retains its deadline");
        auto down = changed; down.usable = false; down.link_active = false; down.physical_candidates.clear();
        f.engine->receive_path(down); f.engine->receive_path(changed);
        t.check(f.engine->snapshot().cooldown_remaining == 90.0,
                "same physical interface disconnected and reattached cannot evade ninety-second protection");
    }
    for (const auto outcome : {Outcome::success, Outcome::uncertain, Outcome::cancelled, Outcome::security, Outcome::rejected}) {
        TunParityFixture f; auto p = f.path(); p.physical_candidates = {f.wired}; f.engine->receive_path(p);
        f.base.outcome = outcome; f.base.online_after_attempt = false; f.engine->connect();
        p.physical_candidates = {f.wifi}; f.engine->receive_path(p);
        f.engine->automatic_round(); f.engine->automatic_round();
        t.check(f.engine->snapshot().cooldown_remaining == 90.0 && f.base.submissions.size() == 1,
                "unverified success, uncertain, cancelled, TLS and credential failures keep global protection across interfaces");
    }
    {
        TunParityFixture f; f.engine->connect();
        const auto journal = f.persisted;
        TunParityFixture restored; restored.engine->restore_recovery(journal);
        auto p = restored.path(); p.physical_candidates = {restored.wifi}; restored.engine->receive_path(p);
        restored.engine->automatic_round(); restored.engine->automatic_round();
        t.check(restored.engine->snapshot().cooldown_remaining == 90.0 && restored.base.submissions.empty(),
                "restart retains conservative global deadline without persisting raw interface identities or scope proof");
    }

    {
        auto cancelled = ConnectivityReport::simple(true,true); cancelled.mode=ProbeMode::complete;
        cancelled.complete_result=CompleteCheckResult::cancelled;
        t.check(!cancelled.complete_base_passed(),"cancelled complete base successes cannot settle unknown submission");
    }
    for (const std::string change : {"tun", "aux", "primary", "global-unavailable"}) {
        TunParityFixture f; const auto epoch = f.engine->snapshot().epoch;
        t.check(f.engine->snapshot().remembered && !f.engine->snapshot().trusted,
                "known physical eligibility does not invent session trust");
        t.equal(f.engine->next_check_delay(), 0, "known attachment starts immediate recovery");
        f.engine->automatic_round();
        t.equal(f.engine->next_check_delay(), 1, "first new-path failure confirms after one second");
        f.base.during_discovery = [&] {
            auto p = f.path(); p.identity = "unrelated-global-change";
            if (change == "tun") p.tunnel = false;
            if (change == "aux") p.physical_candidates.pop_back();
            if (change == "primary") { p.physical_candidates[0].is_system_primary = false; p.physical_candidates[1].is_system_primary = true; }
            if (change == "global-unavailable") p.usable = false;
            f.engine->receive_path(p);
        };
        f.engine->automatic_round();
        if (change == "primary") {
            t.check(f.base.submissions.empty() && !f.engine->snapshot().online && f.engine->snapshot().epoch != epoch &&
                    f.engine->snapshot().authentication_route->interface_key == "fixture-wifi",
                    "discovery yields to a newly primary remembered attachment and discards the old response");
            f.base.during_discovery = {};
            f.engine->automatic_round(); f.engine->automatic_round();
            t.check(f.base.submissions == std::vector<std::string>{"fixture-wifi"} && f.engine->snapshot().online,
                    "the new primary finishes only after its own failure confirmation and fresh proof");
        } else {
            t.check(f.base.submissions == std::vector<std::string>{"fixture-wired"} && f.engine->snapshot().online,
                    change + ": selected physical operation finishes without restart or borrowed authority");
            t.equal(f.engine->snapshot().epoch, epoch, change + ": unrelated change does not reset authentication epoch");
        }
        t.equal(f.website_calls, 0, "priority recovery/verification never waits for a proxy website");
        t.equal(f.engine->next_check_delay(), 2, "one read-only full supplement follows successful recovery");
    }
    for (const bool sleep : {false, true}) {
        TunParityFixture f; f.base.response = ConnectivityReport::simple(true,true);
        f.engine->automatic_round();
        t.equal(f.engine->next_check_delay(), 2, "fixture has one pending website supplement");
        if (sleep) f.engine->set_sleeping(true);
        else { auto p = f.path(); p.usable = false; p.link_active = false; p.physical_candidates.clear(); f.engine->receive_path(p); }
        t.check(f.engine->next_check_delay() != 2, "disconnect or sleep clears pending supplement without empty two-second polling");
        t.check(!f.engine->automatic_round(), "no automatic operation starts while disconnected or asleep");
    }
    for (const std::string change : {"lease", "forget", "sleep"}) {
        TunParityFixture f; f.engine->automatic_round();
        f.base.during_discovery = [&] {
            if (change == "forget") f.engine->forget_current_network();
            else if (change == "sleep") f.engine->set_sleeping(true);
            else { auto p = f.path(); p.physical_candidates[0].identity = "changed-source-lease"; f.engine->receive_path(p); }
        };
        f.engine->automatic_round();
        t.check(f.base.submissions.empty(), change + ": obsolete campus proof never submits credentials");
    }
    {
        TunParityFixture f; f.base.response = ConnectivityReport::simple(true, true);
        f.engine->automatic_round(); f.engine->automatic_round();
        f.advance(100); for (int i=0;i<6;++i) f.engine->automatic_round();
        t.check(f.base.submissions.empty() && !f.engine->snapshot().trusted,
                "continuous proxy website failures never trigger automatic campus login or synthesize trust");
        t.check(f.website_calls > 0 && f.engine->snapshot().online,
                "read-only website diagnostics retain the successful physical base fact");
        t.check(f.engine->next_check_delay() >= 15, "supplement is consumed without a two-second monitoring loop");
    }
    {
        TunParityFixture f; f.advance(46);
        f.base.response = ConnectivityReport::simple(true, false); f.base.response.microsoft.state = EndpointState::redirect;
        f.base.response.microsoft.campus_area_hint = CampusPortalHint::teaching;
        f.base.response.microsoft.authentication_required = true;
        f.engine->automatic_round();
        t.equal(f.engine->next_check_delay(), 1, "a bound login-required redirect confirms promptly outside the priority window");
        f.engine->automatic_round();
        t.equal(f.base.submissions.size(), std::size_t(1), "read-only website supplement cannot steal the second authentication confirmation");
        t.equal(f.website_calls, 0, "true base failure is not blocked by optional websites");
    }
    {
        TunParityFixture f; auto p = f.path(); p.physical_candidates.clear(); f.engine->receive_path(p);
        t.check(!f.engine->snapshot().can_trust && !f.engine->snapshot().remembered,
                "missing bound route cannot reuse previous eligibility or queued trust");
        t.check(!f.engine->trust_current_network(), "queued trust after route loss cannot authorize missing physical binding");
        f.engine->automatic_round();f.engine->automatic_round();t.check(f.base.submissions.empty(), "no binding means no credentials");
    }
    {
        TunParityFixture f; f.base.accepted = false; f.engine->automatic_round(); f.engine->automatic_round();
        t.check(f.base.modes.empty() && f.base.submissions.empty(), "transport lease rejection occurs before base or campus proof");
    }
    {
        TunParityService base; base.response = ConnectivityReport::simple(true,true);
        std::mutex gate; std::condition_variable cv; bool entered=false, release=false;
        InternetCheckService checker(base,[&](const auto&,bool,const auto&) {
            std::unique_lock<std::mutex> lock(gate); entered=true;cv.notify_all();cv.wait(lock,[&]{return release;});
            WebsiteCheckResult r; r.state="redirected"; r.status=302; r.recovery_signal=WebsiteRecoverySignal::campusRedirect; return r;
        });
        checker.configure({{"late","https://late.fixture.invalid/",true}},true);
        ConnectivityReport result; CancelToken token;
        std::thread pending([&]{ result=checker.probe(ProbeMode::complete,token); });
        { std::unique_lock<std::mutex> lock(gate);cv.wait(lock,[&]{return entered;}); }
        checker.invalidate_website_path();
        { std::lock_guard<std::mutex> lock(gate);release=true; } cv.notify_all(); pending.join();
        t.check(!token.cancelled() && result.complete_base_passed(),"TUN website invalidation cannot cancel the current physical parent probe");
        const auto latest=checker.latest();
        t.check(latest && latest->websites.empty() && latest->websites_deferred &&
                !result.campus_recovery_evidence.value_or(true),
                "late old-path campus redirect is discarded from publication and authentication evidence");
    }
    for (const int reason : {0,1,3}) {
        TunParityFixture f;
        const std::string journal = "{\"schema\":1,\"generation\":1,\"notBefore\":1800000090,\"holds\":[{\"reason\":2,\"origin\":2},{\"reason\":"+std::to_string(reason)+",\"origin\":2}]}";
        t.check(f.engine->restore_recovery(journal), "fixture pending submission and independent hold restore");
        f.base.response = ConnectivityReport::simple(true,true); f.engine->automatic_round();
        auto state = f.engine->snapshot();
        t.check(state.holds.size()==1 && static_cast<int>(state.holds.front().reason)==reason,
                "complete bound base proof clears only uncertainty, not other holds");
        t.check(state.cooldown_remaining && *state.cooldown_remaining==90 && f.website_calls==0,
                "uncertain settlement keeps original ninety-second deadline and skips TUN websites");
        t.check(f.base.submissions.empty(), "settlement never submits credentials");
    }
    {
        TunParityFixture f; auto original = f.path();
        f.base.response = ConnectivityReport::simple(true,true);
        f.engine->automatic_round();
        f.base.response = ConnectivityReport::simple(true,false);
        f.base.response.microsoft.state=EndpointState::cancelled;
        f.service.invalidate_website_path();
        const auto retained = f.service.latest();
        t.check(retained && retained->base.all_passed() && retained->websites.empty() && retained->websites_deferred &&
                retained->verdict() == InternetVerdict::basicPassed && !retained->has_campus_recovery_evidence(),
                "TUN notification retains physical connectivity and discards stale website recovery evidence");
        f.engine->receive_path(original);f.engine->automatic_round();
        t.check(f.engine->snapshot().online, "website generation changes do not revoke an owned physical base probe");
    }

    t.begin("memory-physical-binding-rejection");
    for (const bool online : {false, true}) {
        TunParityFixture f;
        f.engine->restore_memory(std::nullopt);
        if (online) {
            f.base.response = ConnectivityReport::simple(true, true);
            f.engine->check();
        }
        const auto request = f.engine->prepare_remember();
        t.check(request && f.engine->snapshot().verifying_memory, "reliable physical path reserves fresh memory verification");
        f.base.accepted = false;
        const int before_changes = f.changes;
        const auto before_probes = f.base.modes.size();
        const int before_websites = f.website_calls;
        t.equal(f.engine->trust_and_remember(*request), ConnectionEngine::RememberResult::needsProof,
                "a rejected physical lease reports an unconfirmed campus proof");
        const auto state = f.engine->snapshot();
        t.check(!state.busy && !state.verifying_memory && state.can_remember && !state.last_discovery,
                "binding rejection releases the operation and permits a fresh explicit retry");
        t.equal(state.status, online ? EngineStatus::online : EngineStatus::unconfirmed,
                "binding rejection cannot leave a discovering status after the operation finishes");
        t.check(state.online == online && !state.trusted && !state.remembered && f.changes == before_changes + 1,
                "binding rejection preserves connectivity and publishes its final state without granting trust");
        t.check(f.base.discoveries == 0 && f.base.submissions.empty() && f.memory_writes == 0 &&
                f.base.modes.size() == before_probes && f.website_calls == before_websites,
                "binding rejection performs no campus discovery, authentication, memory write or website request");
    }
    for (const auto reason : {HoldReason::userPaused, HoldReason::security, HoldReason::uncertain, HoldReason::credentials}) {
        TunParityFixture f;
        f.engine->restore_memory(std::nullopt);
        const std::string journal = "{\"schema\":1,\"generation\":1,\"notBefore\":1800000090,\"holds\":[{\"reason\":" +
            std::to_string(static_cast<int>(reason)) + ",\"origin\":2}]}";
        t.check(f.engine->restore_recovery(journal), "binding fixture restores an authentication hold and full cooldown");
        t.check(f.engine->trust_current_network(), "independent explicit session trust is available before verification");
        const auto before = f.engine->snapshot();
        const std::string before_journal = f.persisted;
        f.base.accepted = false;
        t.equal(f.engine->trust_and_remember(), ConnectionEngine::RememberResult::needsProof,
                "a held authentication session can safely report a failed credential-free binding");
        const auto after = f.engine->snapshot();
        t.check(after.status == EngineStatus::paused && !after.busy && !after.verifying_memory && after.paused,
                "failed binding restores the visible hold instead of leaving a verification spinner");
        t.check(after.holds.size() == before.holds.size() && after.pause_reason == before.pause_reason &&
                after.cooldown_remaining == before.cooldown_remaining && after.cooldown_remaining == 90.0 && f.persisted == before_journal,
                "failed binding preserves every hold, the exact cooldown and its durable journal");
        t.check(after.trusted && !after.remembered && f.memory_writes == 0 && f.base.discoveries == 0 && f.base.submissions.empty(),
                "failed binding retains only independent session trust and never remembers or authenticates");
    }
    for (const bool accepted : {false, true}) {
        for (int interruption = 0; interruption < 3; ++interruption) {
            TunParityFixture f;
            f.engine->restore_memory(std::nullopt);
            f.base.accepted = accepted;
            const auto request = f.engine->prepare_remember();
            std::optional<ConnectionEngine::RememberRequest> successor;
            f.base.during_binding = [&] {
                if (interruption == 1) {
                    auto changed = f.path(); changed.physical_candidates = {f.wifi};
                    f.engine->receive_path(changed);
                } else {
                    f.engine->cancel();
                    if (interruption == 2) successor = f.engine->prepare_remember();
                }
            };
            t.equal(f.engine->trust_and_remember(*request), ConnectionEngine::RememberResult::cancelled,
                    "binding completion cannot revive a cancelled request or a previous physical attachment");
            const auto state = f.engine->snapshot();
            t.check(f.base.discoveries == 0 && f.base.submissions.empty() && f.memory_writes == 0 && !state.trusted && !state.remembered,
                    "ownership lost during binding stops before discovery, authentication and storage");
            if (interruption == 2) {
                t.check(successor && state.busy && state.verifying_memory && state.status == EngineStatus::discovering,
                        "a late binding result cannot finish or overwrite the successor's operation");
                f.base.during_binding = {};
                f.base.accepted = true;
                t.equal(f.engine->trust_and_remember(*successor), ConnectionEngine::RememberResult::remembered,
                        "the untouched successor obtains its own fresh proof and durable memory");
                t.check(f.base.discoveries == 1 && f.memory_writes == 1 && f.base.submissions.empty(),
                        "only the successor discovers and persists once without authenticating");
            } else {
                t.check(!state.busy && !state.verifying_memory && state.status ==
                        (interruption == 1 ? EngineStatus::idle : EngineStatus::cancelled),
                        "stale binding completion preserves the new-path or cancelled status");
            }
        }
    }
}
