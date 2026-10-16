#include <cstdint>
#include <utility>
#include <string>

// Complete-check recovery regressions. All services are scripted; no live networking.
inline void partial_recovery_tests(Harness& t) {
    t.begin("partial-recovery");
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        f.engine->set_probe_mode(ProbeMode::complete);
        f.service.default_probe = "partial";
        f.service.attempts["teaching"] = "success";
        f.engine->connect();
        t.equal(f.service.submissions.size(), std::size_t(1),
                "ordinary manual connect recovers a fresh base-partial result without a stale force marker");
    }
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        f.engine->set_probe_mode(ProbeMode::complete);
        f.service.default_probe = "partial";
        f.service.attempts["teaching"] = "success";
        f.engine->automatic_round();
        t.check(f.engine->snapshot().online && f.service.attempt_log.empty(), "first complete partial preserves online fact without login");
        t.equal(f.engine->next_check_delay(), 5, "first complete partial confirms after five seconds");
        const auto p = status::present(status::input_from(f.engine->snapshot()));
        t.check(p.primary == "partialLogin" && p.secondary == "check", "partial result offers login plus a separate check");
        f.engine->automatic_round();
        t.check(f.service.discoveries == 1 && f.service.submissions.size() == 1, "second complete partial performs fresh discovery and one login");
        t.equal(f.service.probe_calls, 5, "partial post-login result uses all three read-only checks");
        f.clock += 1000;
        for (int i = 0; i < 4; ++i) f.engine->automatic_round();
        t.equal(f.service.attempt_log.size(), std::size_t(1), "persistent partial does not resubmit after cooldown expires");
    }
}

inline void partial_recovery_guard_tests(Harness& t) {
    t.begin("partial-budget");
    for (const std::string outcome : {"unavailable", "challengeRefused", "success"}) {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        f.engine->set_probe_mode(ProbeMode::complete);
        f.service.default_probe = "partial";
        f.service.attempts["teaching"] = outcome;
        f.engine->automatic_round(); f.engine->automatic_round();
        f.clock += 1000;
        f.engine->automatic_round(); f.engine->automatic_round();
        t.equal(f.service.attempt_log.size(), std::size_t(1), outcome + " consumes the episode regardless of whether credentials left");
        f.service.default_probe = "online"; f.engine->automatic_round();
        f.service.default_probe = "partial"; f.engine->automatic_round(); f.engine->automatic_round();
        t.equal(f.service.attempt_log.size(), std::size_t(1), "one full pass does not rearm " + outcome);
        f.engine->set_probe_mode(ProbeMode::quick); f.service.default_probe = "online";
        f.engine->automatic_round(); f.engine->automatic_round();
        f.engine->set_probe_mode(ProbeMode::complete); f.service.default_probe = "partial";
        f.engine->automatic_round(); f.engine->automatic_round();
        t.equal(f.service.attempt_log.size(), std::size_t(1), "quick success does not rearm " + outcome);
        f.service.default_probe = "online"; f.engine->check(); f.engine->check();
        f.service.default_probe = "partial"; f.engine->automatic_round(); f.engine->automatic_round();
        t.equal(f.service.attempt_log.size(), std::size_t(2), "two full passes rearm " + outcome);
    }
    for (const bool offline : {false, true}) {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        f.engine->set_probe_mode(ProbeMode::complete);
        f.service.default_probe = offline ? "offline" : "partial";
        f.service.attempts["teaching"] = "unavailable";
        if (offline) f.engine->connect(); else f.engine->connect_even_if_online();
        f.service.default_probe = "partial";
        f.engine->automatic_round(); f.engine->automatic_round();
        t.equal(f.service.attempt_log.size(), std::size_t(1), "manual/offline preflight spends automatic partial budget");
        f.engine->connect_even_if_online();
        t.equal(f.service.attempt_log.size(), std::size_t(2), "explicit partial login remains retryable after budget exhaustion");
    }
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        f.engine->set_probe_mode(ProbeMode::complete); f.service.default_probe = "partial";
        f.engine->automatic_round(); f.engine->automatic_round();
        f.engine->receive_path(recovery_path("new-wired"));
        f.engine->automatic_round();
        t.equal(f.service.attempt_log.size(), std::size_t(1), "new path still needs two new partial observations");
        f.engine->automatic_round();
        t.equal(f.service.attempt_log.size(), std::size_t(2), "actual path epoch rearms partial recovery");
        f.engine->set_draft({"new-fixture", "new-password"});
        t.equal(f.engine->save_draft(), ConnectionEngine::SaveResult::saved, "changed saved credential generation succeeds");
        f.engine->automatic_round(); f.engine->automatic_round();
        t.equal(f.service.attempt_log.size(), std::size_t(3), "saved credential generation rearms partial recovery");
    }
    t.begin("partial-protections");
    for (const auto reason : {HoldReason::userPaused, HoldReason::security, HoldReason::uncertain, HoldReason::credentials}) {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        f.engine->set_probe_mode(ProbeMode::complete); f.service.default_probe = "partial";
        const auto journal = "{\"schema\":1,\"generation\":1,\"holds\":[{\"reason\":" +
            std::to_string(static_cast<int>(reason)) + ",\"origin\":2}]}";
        t.check(f.engine->restore_recovery(journal), "partial protection fixture restores");
        f.engine->automatic_round(); f.engine->automatic_round();
        t.check(f.service.attempt_log.empty() && f.service.discoveries == 0 && f.engine->snapshot().pause_reason == reason,
            "partial preserves " + std::string(to_string(reason)) + " without discovery or login");
        if (reason == HoldReason::uncertain) {
            f.engine->set_probe_mode(ProbeMode::quick); f.engine->check();
            t.check(f.engine->paused() && f.engine->snapshot().last_report->mode == ProbeMode::complete &&
                f.engine->snapshot().probe_mode == ProbeMode::quick && f.service.submissions.empty(),
                "uncertain overrides quick for a read-only complete check; partial evidence keeps the hold");
            f.service.default_probe = "online"; f.engine->check();
            t.check(!f.engine->paused() && f.engine->snapshot().last_report->complete_all_passed() &&
                f.engine->snapshot().probe_mode == ProbeMode::quick && f.service.submissions.empty(),
                "complete success settles uncertainty without changing the quick preference or submitting");
        }
    }
    for (const bool read_only : {false, true}) {
        EngineFixture f(read_only ? MonitoringMode::recover : MonitoringMode::observe, CampusArea::teaching, true, read_only);
        f.engine->set_probe_mode(ProbeMode::complete); f.service.default_probe = "partial";
        f.engine->automatic_round(); f.engine->automatic_round(); f.engine->connect_even_if_online();
        const auto p = status::present(status::input_from(f.engine->snapshot()));
        t.check(f.service.attempt_log.empty() && f.service.discoveries == 0 && p.primary == "check" && !p.secondary,
            "read-only or observe partial never offers or runs authentication");
    }
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, false);
        f.engine->set_probe_mode(ProbeMode::complete); f.service.default_probe = "partial";
        f.engine->automatic_round(); f.engine->automatic_round();
        t.check(f.service.attempt_log.empty() && f.engine->snapshot().status == EngineStatus::needsAccount,
            "partial cannot login without saved credentials");
        t.check(status::present(status::input_from(f.engine->snapshot())).primary == "account", "partial without a draft still asks for an account");
    }
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        f.engine->set_probe_mode(ProbeMode::complete); f.service.default_probe = "partial";
        t.check(f.engine->restore_recovery(std::string("{\"schema\":1,\"generation\":1,\"holds\":[],\"notBefore\":1090}")), "cooldown fixture restores");
        f.engine->automatic_round(); f.engine->automatic_round();
        t.check(f.service.attempt_log.empty() && f.engine->snapshot().status == EngineStatus::coolingDown,
            "partial retains the original ninety-second cooldown");
        f.clock += 91; f.engine->automatic_round();
        t.equal(f.service.attempt_log.size(), std::size_t(1), "unspent partial episode may recover when existing cooldown ends");
    }
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::dormitory, true);
        f.engine->set_probe_mode(ProbeMode::complete); f.service.default_probe = "partial";
        f.service.discovery = {{{"teaching", "confirmed"}, {"dormitory", "confirmed"}}};
        f.engine->automatic_round(); f.engine->automatic_round();
        t.check(f.service.attempt_log.empty() && f.engine->snapshot().status == EngineStatus::needsTrust,
            "partial cannot invent dormitory network trust");
        f.engine->trust_current_network(); f.engine->automatic_round();
        t.equal(f.service.attempt_log.size(), std::size_t(1), "fresh discovery and explicit current-network trust admit partial recovery");
    }
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::automatic, true);
        f.engine->set_probe_mode(ProbeMode::complete); f.service.default_probe = "partial";
        f.engine->trust_current_network();
        f.service.discovery = {{{"teaching", "confirmed"}, {"dormitory", "unconfirmed"}}, {{"teaching", "confirmed"}, {"dormitory", "confirmed"}}};
        f.service.attempts["teaching"] = "challengeRefused"; f.service.attempts["dormitory"] = "success";
        f.engine->automatic_round(); f.engine->automatic_round();
        t.check(f.service.attempt_log.size() == 2 && f.service.submissions.size() == 1,
            "partial retains same-round no-submission fallback with one credential submission");
        f.clock += 1000; f.engine->automatic_round();
        t.equal(f.service.attempt_log.size(), std::size_t(2), "fallback also consumes the sustained partial episode");
    }
    for (const std::string change : {"cancel", "path", "forget"}) {
        for (const std::string phase : {"discovery", "attempt"}) {
            EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
            f.engine->set_probe_mode(ProbeMode::complete); f.service.default_probe = "partial";
            f.service.attempts["teaching"] = "success";
            bool cancel_visible = false;
            auto invalidate = [&] {
                const auto p = status::present(status::input_from(f.engine->snapshot()));
                cancel_visible = p.primary == "cancel";
                if (change == "cancel") f.engine->cancel();
                else if (change == "path") f.engine->receive_path(recovery_path("changed"));
                else f.engine->forget_all_networks();
            };
            if (phase == "discovery") f.service.during_discovery = invalidate;
            else f.service.during_attempt = invalidate;
            f.engine->automatic_round(); f.engine->automatic_round();
            t.check(cancel_visible, "partial " + phase + " presents cancel despite its retained online fact");
            t.check(!f.engine->snapshot().last_attempt && !f.engine->snapshot().last_success_area && !f.engine->snapshot().busy,
                change + " rejects late partial " + phase + " results");
            if (phase == "discovery") t.check(f.service.attempt_log.empty(), change + " during discovery prevents any attempt");
        }
    }
}

inline void partial_recovery_integration_tests(Harness& t) {
    t.begin("partial-aggregate");
    szutest::ScriptedService raw; raw.default_probe = "online";
    raw.discovery = {{{"teaching", "confirmed"}, {"dormitory", "confirmed"}}};
    raw.attempts["teaching"] = "unavailable";
    bool site_pass = false;
    InternetCheckService service(raw, [&](const std::string&, bool, const CancelToken&) {
        return WebsiteCheckResult{site_pass ? "accessible" : "timeout", site_pass ? 200 : 0, 0};
    });
    t.check(service.configure({{"fixture", "https://fixture.example/", true}}, true), "synthetic website configuration accepted");
    auto r = service.probe(ProbeMode::complete, CancelToken{});
    t.check(r.online() && r.all_passed() && r.complete_partial() && !r.complete_all_passed() &&
            r.complete_result == CompleteCheckResult::partial, "site failure carries only partial metadata while built-in results remain all-pass");
    t.check(r.website_only_issues() && !r.recoverable_partial() && service.latest()->website_only_issues(),
            "full base success with custom failure has website-only facts, not a campus recovery trigger");
    InternetCheckReport missing;
    missing.base = ConnectivityReport::simple(true, true); missing.base.mode = ProbeMode::complete;
    missing.selected_count = 1;
    t.check(missing.website_only_issues() && missing.verdict() == InternetVerdict::partial,
            "missing selected result is also a website-only issue when both base probes passed");
    auto cancelled = missing; cancelled.cancelled = true;
    t.check(!cancelled.website_only_issues(), "cancelled aggregate cannot supply website-only evidence");
    auto quick = missing; quick.base.mode = ProbeMode::quick;
    t.check(!quick.website_only_issues(), "quick mode cannot supply selected website evidence");
    site_pass = true; r = service.probe(ProbeMode::complete, CancelToken{});
    t.check(r.complete_all_passed() && !r.complete_partial(), "full metadata includes all selected website passes");
    r = service.probe(ProbeMode::quick, CancelToken{});
    t.check(!r.complete_result && !r.complete_all_passed() && !r.complete_partial(), "quick results cannot accidentally contain complete evidence");
    raw.default_probe = "offline"; r = service.probe(ProbeMode::complete, CancelToken{});
    t.check(r.complete_result == CompleteCheckResult::partial && !r.online() && !r.complete_partial(),
            "custom website success cannot impersonate built-in connectivity or login authorization");
    raw.default_probe = "online"; site_pass = false;
    double clock = 1000;
    ConnectionEngine::Hooks hooks; hooks.now = [&] { return clock; };
    ConnectionEngine engine(service, hooks);
    engine.configure(MonitoringMode::recover, CampusArea::teaching, ProbeMode::complete, Credentials{"fixture", "pw"});
    engine.receive_path(recovery_path());
    engine.automatic_round();
    t.check(engine.next_check_delay() != 5, "website-only failure does not schedule the five-second login confirmation");
    engine.automatic_round();
    t.check(raw.discoveries == 0 && raw.attempt_log.empty(), "two passing base probes with website failure never discover or authenticate");
    const auto website_presentation = status::present(status::input_from(engine.snapshot()));
    t.check(website_presentation.primary == "check" && website_presentation.secondary == "forceLogin" &&
            website_presentation.headline == "internet.websiteIssues.title" &&
            website_presentation.detail == "internet.websiteIssues.detail", "unclassified website-only partial offers check and the explicit confirmed force path");
    engine.connect();
    t.check(raw.attempt_log.empty() && status::present(status::input_from(engine.snapshot())).secondary == "forceLogin",
            "an explicit connect only offers the original secondary force path after its read-only check");
    engine.check();
    t.check(!engine.snapshot().offer_forced_login && status::present(status::input_from(engine.snapshot())).secondary == "forceLogin",
            "a read-only check does not mark an explicit request; noncandidate partial still offers confirmation");
    for (int i = 0; i < 4; ++i) { clock += 100; engine.automatic_round(); }
    t.check(raw.discoveries == 0 && raw.attempt_log.empty(), "sustained website-only failure never consumes a campus login round");
    const auto journal = "{\"schema\":1,\"generation\":1,\"holds\":[{\"reason\":" +
        std::to_string(static_cast<int>(HoldReason::uncertain)) + ",\"origin\":2}]}";
    t.check(engine.restore_recovery(journal), "aggregate uncertainty protection fixture restores");
    engine.check(); engine.automatic_round();
    t.check(engine.snapshot().pause_reason == HoldReason::uncertain && raw.attempt_log.empty(),
            "complete website-only partial cannot settle uncertain or authenticate again");
    site_pass = true; engine.check();
    t.check(!engine.paused(), "actual full all-pass can resolve uncertain under the original proof contract");
    raw.default_probe = "partial"; site_pass = false;
    engine.automatic_round();
    t.check(engine.next_check_delay() == 5 && raw.attempt_log.empty(),
            "base failure after a website-only episode still requires its first bounded confirmation");
    engine.automatic_round();
    t.check(raw.discoveries == 1 && raw.attempt_log.size() == 1,
            "base partial plus custom failure still admits the original single controlled recovery");

    t.begin("website-only-protections");
    for (const auto reason : {HoldReason::userPaused, HoldReason::security, HoldReason::credentials, HoldReason::uncertain}) {
        szutest::ScriptedService held_raw; held_raw.default_probe = "online";
        bool sites_ok = false;
        InternetCheckService held_service(held_raw, [&](const std::string&, bool, const CancelToken&) {
            return WebsiteCheckResult{sites_ok ? "accessible" : "tls", sites_ok ? 200 : 0, 0};
        });
        held_service.configure({{"held-site", "https://fixture.example/", true}}, true);
        ConnectionEngine held_engine(held_service, {});
        held_engine.configure(MonitoringMode::recover, CampusArea::teaching, ProbeMode::complete, Credentials{"fixture", "pw"});
        held_engine.receive_path(recovery_path());
        const auto hold_json = "{\"schema\":1,\"generation\":1,\"holds\":[{\"reason\":" +
            std::to_string(static_cast<int>(reason)) + ",\"origin\":2}]}";
        t.check(held_engine.restore_recovery(hold_json), "website-only hold fixture restores");
        held_engine.automatic_round(); held_engine.automatic_round();
        const auto held_snapshot = held_engine.snapshot();
        t.check(held_snapshot.pause_reason == reason && held_raw.discoveries == 0 && held_raw.attempt_log.empty(),
                "website-only failure preserves " + std::string(to_string(reason)) + " without campus operations");
        t.check(status::present(status::input_from(held_snapshot)).primary == "check",
                "website-only held result never turns the primary action into login");
        sites_ok = true; held_engine.check();
        t.check(reason == HoldReason::uncertain ? !held_engine.paused() : held_engine.snapshot().pause_reason == reason,
                "only actual all-selected pass may release uncertainty; other holds remain independent");
    }

    t.begin("website-only-cooldown");
    {
        szutest::ScriptedService cooling_raw; cooling_raw.default_probe = "online";
        InternetCheckService cooling_service(cooling_raw, [](const std::string&, bool, const CancelToken&) {
            return WebsiteCheckResult{"timeout", 0, 0};
        });
        cooling_service.configure({{"cooling-site", "https://fixture.example/", true}}, true);
        double cooling_clock = 1000; ConnectionEngine::Hooks cooling_hooks;
        cooling_hooks.now = [&] { return cooling_clock; };
        cooling_hooks.wall_now = [&] { return cooling_clock; };
        ConnectionEngine cooling(cooling_service, cooling_hooks);
        cooling.configure(MonitoringMode::recover, CampusArea::teaching, ProbeMode::complete, Credentials{"fixture", "pw"});
        cooling.receive_path(recovery_path());
        t.check(cooling.restore_recovery("{\"schema\":1,\"generation\":1,\"holds\":[],\"notBefore\":1090}"),
                "website-only cooldown fixture restores");
        cooling.automatic_round(); cooling.automatic_round();
        t.check(cooling.snapshot().cooldown_remaining == 90 && cooling_raw.attempt_log.empty(),
                "website-only failure neither resets the ninety-second cooldown nor submits");
    }
    t.begin("partial-memory");
    {
        memtest::Rig f;
        f.path("remembered", {mem_desc("a4:5e:60:11:22:33")}); f.discovery("confirmed", "confirmed");
        t.equal(f.e->trust_and_remember(), ConnectionEngine::RememberResult::remembered, "partial fixture remembers a campus attachment");
        f.path("new-epoch", {mem_desc("a4:5e:60:11:22:33")});
        f.e->set_probe_mode(ProbeMode::complete); f.svc.default_probe = "partial";
        f.discovery("unconfirmed", "confirmed"); f.svc.attempts["dormitory"] = "success";
        f.rounds(2);
        t.check(f.e->snapshot().remembered && !f.e->snapshot().trusted && f.svc.submissions.size() == 1 &&
                f.svc.attempt_log.front().first == "dormitory", "strong remembered network with fresh dormitory proof recovers partial after a network change");
    }
    {
        memtest::Rig f;
        f.path("remembered", {mem_desc("a4:5e:60:11:22:33")}); f.discovery("confirmed", "confirmed");
        f.e->trust_and_remember();
        f.path("new-tunnel", {mem_desc("a4:5e:60:11:22:33")}, true);
        f.e->set_probe_mode(ProbeMode::complete); f.svc.default_probe = "partial";
        f.discovery("unconfirmed", "confirmed"); f.rounds(2);
        t.check(f.svc.attempt_log.empty(), "partial with a tunnel still requires fresh dual-campus memory proof");
    }
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        f.engine->set_probe_mode(ProbeMode::complete); f.service.default_probe = "partial";
        f.service.attempts["teaching"] = "success";
        f.service.probes = {"partial", "partial", "partial", "offline", "offline"};
        f.engine->automatic_round(); f.engine->automatic_round();
        t.check(!f.engine->snapshot().online && f.engine->snapshot().status == EngineStatus::accepted,
            "later offline post-login probes replace an earlier partial online fact");
    }
}

inline void website_recovery_signal_tests(Harness& t) {
    t.begin("website-recovery-signals");
    t.check(WebsiteCheckResult{"network", 0, 0}.recovery_signal == WebsiteRecoverySignal::none,
        "legacy or unclassified website results default to no recovery candidate");
    for (const auto error : {win::HttpError::timeout, win::HttpError::dns}) {
        win::HttpResponse response; response.error = error;
        t.check(response.website_recovery_signal() == WebsiteRecoverySignal::connectionFailure,
            "precise timeout/DNS conversion supplies a connection candidate");
    }
    for (const auto error : {win::HttpError::none, win::HttpError::network, win::HttpError::tls,
                            win::HttpError::cancelled, win::HttpError::redirect, win::HttpError::tooLarge}) {
        win::HttpResponse response; response.error = error;
        t.check(response.website_recovery_signal() == WebsiteRecoverySignal::none,
            "HTTP/unknown/TLS/cancel/response conversion never infers a connection failure");
    }
    win::HttpResponse response; response.error = win::HttpError::network; response.connection_failure = true;
    t.check(response.website_recovery_signal() == WebsiteRecoverySignal::connectionFailure,
        "a precise WinHTTP connection error supplies a candidate");
    response.error = win::HttpError::cancelled;
    t.check(response.website_recovery_signal() == WebsiteRecoverySignal::none,
        "cancellation supersedes a coincident connection error");
    t.check(win::recovery_connection_error(12029) && win::recovery_connection_error(12030),
        "WinHTTP CANNOT_CONNECT and CONNECTION_ERROR are explicitly admitted");
    for (const std::uint32_t code : {0u, 12005u, 12007u, 12015u, 12017u, 12156u, 12175u, 99999u})
        t.check(!win::recovery_connection_error(code), "other WinHTTP codes do not fall through to recovery");
    response = {}; response.campus_redirect = true;
    for (const int code : {200, 299, 300, 302, 399, 400, 403, 599}) {
        response.status = code;
        t.check(response.website_recovery_signal() == (code >= 300 && code < 400 ?
            WebsiteRecoverySignal::campusRedirect : WebsiteRecoverySignal::none),
            "campus destination is a candidate only with an actual 3xx response");
    }
    response.status = 302; response.error = win::HttpError::tls;
    t.check(response.website_recovery_signal() == WebsiteRecoverySignal::none,
        "failed TLS never turns a retained status into a campus redirect");

    t.begin("website-campus-location");
    const std::string base = "https://fixture.example/path/check";
    for (const std::string location : {"https://net.szu.edu.cn", "HTTPS://NET.SZU.EDU.CN:443/",
         "https://net.szu.edu.cn/srun_portal_pc", "https://net.szu.edu.cn/?fixture=x#fragment",
         "http://172.30.255.42:801", "http://172.30.255.42:801/", "http://172.30.255.42:801/eportal",
         "//net.szu.edu.cn/srun_portal_pc"})
        t.check(website::is_campus_redirect(location, base), "only fixed portal homepages qualify");
    for (const std::string location : {"", "https://net.szu.edu.cn:444/", "http://net.szu.edu.cn/",
         "https://172.30.255.42:801/eportal", "http://172.30.255.42/eportal", "http://172.30.255.42:80/eportal",
         "http://172.30.255.42:801/eportal/portal/login", "https://net.szu.edu.cn/cgi-bin/srun_portal",
         "https://net.szu.edu.cn/srun_portal_pc/", "https://net.szu.edu.cn.evil.example/",
         "https://net.szu.edu.cn" "@evil.example/", "https://u:p@net.szu.edu.cn/", "https://@net.szu.edu.cn/",
         "https://net.szu.edu.cn/%73run_portal_pc", "https://net.szu.edu.cn/./", "/srun_portal_pc",
         "https://net.szu.edu.cn\\/", "https://net.szu.edu.cn/\n", "https://net.szu.edu.cn/\xE2\x80\x8B"})
        t.check(!website::is_campus_redirect(location, base), "nonportal or ambiguous Location cannot authorize recovery");
    t.check(website::is_campus_redirect("/srun_portal_pc?fixture=x", "https://net.szu.edu.cn/check") &&
            website::is_campus_redirect("eportal", "http://172.30.255.42:801/") &&
            website::is_campus_redirect("?fixture=x", "https://net.szu.edu.cn/"),
        "relative Location must resolve to the same exact portal origin and homepage");
    const std::string prefix = "https://net.szu.edu.cn/?fixture=";
    t.check(website::is_campus_redirect(prefix + std::string(2048 - prefix.size(), 'x'), base) &&
            !website::is_campus_redirect(prefix + std::string(2049 - prefix.size(), 'x'), base),
        "UTF-8 byte bound applies to the whole Location including ignored query");
    t.check(!website::is_campus_redirect(prefix + std::string(1000, 'x') + std::string(400, '\xE4') , base),
        "invalid UTF-8 is rejected before portal parsing");
    std::string multibyte = prefix;
    for (int i = 0; i < 700; ++i) multibyte += "校";
    t.check(!website::is_campus_redirect(multibyte, base), "multibyte query is bounded by UTF-8 bytes");

    t.begin("internet-recovery-facts");
    auto base_report = ConnectivityReport::simple(true, true); base_report.mode = ProbeMode::complete;
    InternetCheckReport report{base_report, {{{"a", "https://fixture.example/", true}, {"forbidden", 403, 0}}}, 1, false};
    t.check(report.verdict() == InternetVerdict::partial && !report.has_recovery_evidence() && report.transport_recovered(),
        "403 is a business failure with recovered transport, not a campus recovery candidate");
    for (const auto& pair : {std::pair{"authenticationRequired", 401}, std::pair{"authenticationRequired", 407},
                           std::pair{"httpStatus", 500}, std::pair{"redirected", 302}, std::pair{"accessible", 204}}) {
        report.websites[0].result = {pair.first, pair.second, 0};
        t.check(report.transport_recovered() && !report.has_recovery_evidence(),
            "a real noncampus HTTP response can rearm transport without triggering login");
    }
    for (const std::string state : {"timeout", "dns", "network", "tls", "invalidInput", "cancelled", "notRun"}) {
        report.websites[0].result = {state, 0, 0};
        t.check(!report.has_recovery_evidence() && !report.transport_recovered(),
            "classification defaults remain safe for " + state);
    }
    report.websites[0].result = {"timeout", 0, 0, WebsiteRecoverySignal::connectionFailure};
    t.check(report.has_recovery_evidence() && !report.transport_recovered(), "explicit connection evidence can recover but cannot rearm itself");
    report.websites[0].result = {"redirected", 302, 0, WebsiteRecoverySignal::campusRedirect};
    t.check(report.has_recovery_evidence() && !report.transport_recovered(), "campus redirect is a candidate, not a transport recovery");
    report.cancelled = true;
    t.check(!report.has_recovery_evidence() && !report.transport_recovered(), "cancelled aggregate supplies neither recovery fact");
    report.cancelled = false; report.base.mode = ProbeMode::quick;
    t.check(!report.has_recovery_evidence() && !report.transport_recovered(), "quick aggregate supplies neither selected-site fact");
    report.base = base_report; report.websites.clear();
    t.check(!report.has_recovery_evidence() && !report.transport_recovered(), "missing selected result cannot trigger or rearm");
    report.selected_count = 2;
    report.websites = {{{"a", "https://fixture.example/", true}, {"accessible", 200, 0}},
                       {{"a", "https://other.example/", true}, {"accessible", 200, 0}}};
    t.check(!report.transport_recovered(), "duplicate selected identities cannot prove a complete transport round");
    report.websites.clear(); report.selected_count = 0;
    report.base.microsoft.state = EndpointState::network;
    t.check(report.has_recovery_evidence() && !report.transport_recovered(), "one base failure retains the original bounded recovery candidate");
}

struct WebsiteRecoveryRig {
    szutest::ScriptedService raw;
    WebsiteCheckResult site{"timeout", 0, 0, WebsiteRecoverySignal::connectionFailure};
    InternetCheckService service;
    double clock = 1000;
    ConnectionEngine engine;
    static ConnectionEngine::Hooks hooks(double& clock) {
        ConnectionEngine::Hooks h; h.now = [&clock] { return clock; }; h.wall_now = [&clock] { return clock; }; return h;
    }
    WebsiteRecoveryRig(bool follow = true, bool tunnel = false)
        : service(raw, [this](const std::string&, bool, const CancelToken&) { return site; }), engine(service, hooks(clock)) {
        raw.default_probe = "online";
        raw.discovery = {{{"teaching", "confirmed"}, {"dormitory", "confirmed"}}};
        raw.attempts["teaching"] = "unavailable";
        service.configure({{"fixture-site", "https://fixture.example/", true}}, follow);
        engine.configure(MonitoringMode::recover, CampusArea::teaching, ProbeMode::complete, Credentials{"fixture", "pw"});
        auto path = recovery_path(); path.tunnel = tunnel; engine.receive_path(path);
    }
};

inline void website_recovery_engine_tests(Harness& t) {
    t.begin("website-candidate-episodes");
    for (const bool system : {false, true}) for (const bool tunnel : {false, true}) {
        WebsiteRecoveryRig f(system, tunnel);
        f.engine.automatic_round();
        t.check(f.raw.discoveries == 0 && f.engine.next_check_delay() == 5 &&
                status::present(status::input_from(f.engine.snapshot())).primary == "partialLogin",
            "real website connection evidence confirms at five seconds without vetoing proxy/TUN");
        f.engine.automatic_round();
        t.check(f.raw.discoveries == 1 && f.raw.attempt_log.size() == 1,
            "second candidate round still needs fresh campus proof and exactly one bounded attempt");
        f.clock += 1000;
        for (int i = 0; i < 4; ++i) f.engine.automatic_round();
        t.equal(f.raw.attempt_log.size(), std::size_t(1), "sustained timeout never rearms the spent episode");
        f.site = {"forbidden", 403, 0}; f.engine.check();
        f.site = {"timeout", 0, 0, WebsiteRecoverySignal::connectionFailure};
        f.engine.automatic_round(); f.engine.automatic_round();
        t.equal(f.raw.attempt_log.size(), std::size_t(1), "one HTTP business response cannot rearm");
        f.site = {"forbidden", 403, 0}; f.engine.check(); f.engine.check();
        t.check(f.engine.snapshot().last_report->transport_recovered() && !f.engine.snapshot().last_report->complete_all_passed(),
            "two HTTP business responses prove transport only, while the full verdict stays partial");
        f.site = {"timeout", 0, 0, WebsiteRecoverySignal::connectionFailure};
        f.engine.automatic_round(); f.engine.automatic_round();
        t.equal(f.raw.attempt_log.size(), std::size_t(2), "two transport rounds rearm exactly one future recovery episode");
    }
    for (const auto signal : {WebsiteRecoverySignal::connectionFailure, WebsiteRecoverySignal::campusRedirect}) {
        WebsiteRecoveryRig f;
        f.site = signal == WebsiteRecoverySignal::campusRedirect ? WebsiteCheckResult{"redirected", 302, 0, signal} :
                                                                  WebsiteCheckResult{"dns", 0, 0, signal};
        f.engine.connect();
        t.equal(f.raw.attempt_log.size(), std::size_t(1), "normal manual connect uses a fresh website recovery candidate");
    }
    for (const std::string result : {"allPassed", "business"}) {
        WebsiteRecoveryRig f;
        f.engine.check(); // the rendered recovery action was based on a real failure
        f.site = result == "allPassed" ? WebsiteCheckResult{"accessible", 200, 0} : WebsiteCheckResult{"forbidden", 403, 0};
        f.engine.connect();
        t.check(f.raw.attempt_log.empty() && f.engine.snapshot().offer_forced_login,
            "normal manual connect rereads " + result + " and offers confirmation without submitting");
        f.engine.connect();
        t.check(f.raw.attempt_log.empty(), "an old explicit force offer cannot change the next normal connect into force");
        f.engine.connect_even_if_online();
        t.equal(f.raw.attempt_log.size(), std::size_t(1), "the separately confirmed core force remains available");
    }
    {
        WebsiteRecoveryRig f; f.site = {"accessible", 200, 0}; f.engine.check();
        const auto p = status::present(status::input_from(f.engine.snapshot()));
        t.check(p.primary == "check" && !p.secondary, "ordinary full success never permanently exposes force login");
        f.site = {"forbidden", 403, 0}; f.engine.check();
        const auto business = status::present(status::input_from(f.engine.snapshot()));
        t.check(business.primary == "check" && business.secondary == "forceLogin",
            "noncandidate HTTP partial uses only check and an explicit force confirmation");
    }
    {
        WebsiteRecoveryRig f; f.raw.discovery = {{{"teaching", "unconfirmed"}, {"dormitory", "unconfirmed"}}};
        f.engine.automatic_round(); f.engine.automatic_round();
        t.check(f.raw.discoveries == 1 && f.raw.attempt_log.empty(), "website failure never replaces the fresh campus plan");
    }
    for (const auto reason : {HoldReason::userPaused, HoldReason::credentials, HoldReason::security, HoldReason::uncertain}) {
        WebsiteRecoveryRig f;
        const auto journal = "{\"schema\":1,\"generation\":1,\"holds\":[{\"reason\":" +
            std::to_string(static_cast<int>(reason)) + ",\"origin\":2}]}";
        t.check(f.engine.restore_recovery(journal), "website candidate protection fixture restores");
        f.engine.automatic_round(); f.engine.automatic_round();
        f.site = {"forbidden", 403, 0}; f.engine.check(); f.engine.check();
        t.check(f.engine.snapshot().pause_reason == reason && f.raw.attempt_log.empty(),
            "transport recovery never clears " + std::string(to_string(reason)) + " or submits");
        f.site = {"accessible", 200, 0}; f.engine.check();
        t.check(reason == HoldReason::uncertain ? !f.engine.paused() : f.engine.snapshot().pause_reason == reason,
            "only allPassed can settle uncertainty and other holds stay independent");
    }
    {
        WebsiteRecoveryRig f; f.raw.attempts["teaching"] = "uncertain";
        f.engine.automatic_round(); f.engine.automatic_round();
        f.site = {"forbidden", 403, 0}; f.engine.check(); f.engine.check();
        f.clock += 1000; f.site = {"timeout", 0, 0, WebsiteRecoverySignal::connectionFailure};
        f.engine.automatic_round(); f.engine.automatic_round();
        t.check(f.engine.snapshot().pause_reason == HoldReason::uncertain && f.raw.submissions.size() == 1,
            "rearmed budget cannot replay a submission that remains unknown");
    }
}

inline void website_recovery_boundary_tests(Harness& t) {
    t.begin("website-recovery-configuration-boundaries");
    for (const std::string boundary : {"websites", "probeMode", "monitoringMode", "cancel"}) {
        for (const int new_rounds : {1, 2}) {
            WebsiteRecoveryRig f;
            f.engine.automatic_round(); f.engine.automatic_round(); // spend one episode, without submission
            f.site = {"forbidden", 403, 0}; f.engine.check(); // one pre-boundary transport round
            if (boundary == "websites") {
                t.check(f.service.configure({{"new-fixture", "https://new.example/", true}}, false),
                    "changed website/strategy fixture is accepted");
                f.engine.reset_partial_confirmation();
            } else if (boundary == "probeMode") {
                f.engine.set_probe_mode(ProbeMode::quick); f.engine.set_probe_mode(ProbeMode::complete);
            } else if (boundary == "monitoringMode") {
                f.engine.set_mode(MonitoringMode::manual); f.engine.set_mode(MonitoringMode::recover);
            } else {
                f.raw.during_probe = [&] { f.engine.cancel(); };
                f.engine.check(); f.raw.during_probe = nullptr;
            }
            for (int i = 0; i < new_rounds; ++i) f.engine.check();
            f.site = {"timeout", 0, 0, WebsiteRecoverySignal::connectionFailure};
            f.clock += 1000;
            f.engine.automatic_round(); f.engine.automatic_round();
            t.equal(f.raw.attempt_log.size(), std::size_t(new_rounds == 2 ? 2 : 1),
                boundary + " requires two fresh transport rounds; a single new round cannot combine with the old one");
        }
    }
    {
        WebsiteRecoveryRig f;
        f.engine.automatic_round();
        f.service.configure({{"new-fixture", "https://new.example/", true}}, false);
        f.engine.reset_partial_confirmation();
        f.engine.automatic_round();
        t.check(f.raw.attempt_log.empty() && f.engine.next_check_delay() == 5,
            "a configuration boundary also breaks the two candidate observations");
        f.engine.automatic_round();
        t.equal(f.raw.attempt_log.size(), std::size_t(1), "same new configuration still admits its second fresh candidate");
    }
    {
        WebsiteRecoveryRig f;
        t.check(f.engine.restore_recovery("{\"schema\":1,\"generation\":1,\"holds\":[],\"notBefore\":1090}"),
            "website candidate cooldown fixture restores");
        f.engine.automatic_round(); f.engine.automatic_round();
        t.check(f.raw.discoveries == 0 && f.raw.attempt_log.empty() && f.engine.snapshot().cooldown_remaining == 90,
            "a real website candidate cannot bypass the ninety-second cooldown");
        f.engine.reset_partial_confirmation();
        t.check(f.engine.snapshot().cooldown_remaining == 90, "configuration reset preserves the exact cooldown");
        f.clock += 91; f.engine.automatic_round();
        t.check(f.raw.attempt_log.empty(), "candidate history remains broken after cooldown expiry");
        f.engine.automatic_round();
        t.equal(f.raw.attempt_log.size(), std::size_t(1), "unspent candidate may recover after cooldown with two fresh observations");
    }
    for (const bool read_only : {false, true}) {
        szutest::ScriptedService raw; raw.default_probe = "online";
        InternetCheckService svc(raw, [](const std::string&, bool, const CancelToken&) {
            return WebsiteCheckResult{"timeout", 0, 0, WebsiteRecoverySignal::connectionFailure};
        });
        svc.configure({{"fixture-site", "https://fixture.example/", true}}, true);
        ConnectionEngine engine(svc, {}, read_only);
        engine.configure(read_only ? MonitoringMode::recover : MonitoringMode::observe, CampusArea::teaching,
            ProbeMode::complete, Credentials{"fixture", "pw"});
        engine.receive_path(recovery_path());
        engine.automatic_round(); engine.automatic_round(); engine.connect(); engine.connect_even_if_online();
        const auto p = status::present(status::input_from(engine.snapshot()));
        t.check(raw.discoveries == 0 && raw.attempt_log.empty() && p.primary == "check" &&
                p.secondary != "forceLogin" && p.secondary != "partialLogin",
            "read-only/observe with concrete website evidence still cannot authenticate or offer an auth action");
    }
}
