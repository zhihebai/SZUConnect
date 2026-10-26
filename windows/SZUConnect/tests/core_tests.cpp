// Portable core tests: shared fixtures (same JSON files as macOS and the Python references)
// plus unit tests for the parser, portal wire rules, policy, engine and records.
// Usage: szu_core_tests [repository-root]
#include "szu/address_resolver.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <semaphore>
#include <mutex>
#include <thread>
#include <cmath>
#include <limits>
#include <cstdio>
#include <string>
#include <vector>

#include "../core/generated/shared_copy.hpp"
#include "szu/engine.hpp"
#include "szu/physical.hpp"
#include "szu/wifi_recovery.hpp"
#include "szu/internet.hpp"
#include "szu/environment.hpp"
#include "szu/proxy_assistant.hpp"
#include "szu/json.hpp"
#include "szu/netlabels.hpp"
#include "szu/policy.hpp"
#include "szu/portal_wire.hpp"
#include "szu/presentation.hpp"
#include "szu/srun.hpp"
#include "test_support.hpp"
#include "../platform/include/szu/win/http.hpp"
#include "../app/smoke_options.hpp"

using namespace szu;
using szutest::Harness;
using szutest::str;

namespace {

void launch_options_tests(Harness& t) {
    t.begin("launch-mode-isolation");
    using szu::app::parse_launch_options;
    const std::wstring token = L"0123456789abcdef0123456789abcdef";
    const auto uninstall = parse_launch_options({L"--uninstall"});
    t.check(uninstall.error.empty() && uninstall.uninstall && uninstall.uninstall_worker_token.empty() &&
        !uninstall.observe_only && !uninstall.background && !uninstall.offline_smoke && !uninstall.portable &&
        uninstall.ready_token.empty() && uninstall.smoke_output.empty(), "Public uninstall selects only maintenance mode");
    const auto worker = parse_launch_options({L"--uninstall-worker", token});
    t.check(worker.error.empty() && !worker.uninstall && worker.uninstall_worker_token == token &&
        !worker.observe_only && !worker.background && !worker.offline_smoke && !worker.portable &&
        worker.ready_token.empty() && worker.smoke_output.empty(), "Worker accepts only the opaque lowercase token");
    std::vector<std::vector<std::wstring>> conflicting = {
        {L"--observe-only"}, {L"--background"}, {L"--portable"},
        {L"--offline-smoke", L"--smoke-output", L"C:\\offline\\receipt.json"},
        {L"--installation-ready", token}, {L"--smoke-output", L"C:\\offline\\receipt.json"},
        {L"--smoke-timeout-ms", L"1000"}, {L"--uninstall"}, {L"--uninstall-worker", token},
        {L"--unknown"}, {L"C:\\arbitrary\\application"}, {L"\\\\server\\share"},
        {L"--uninstall-root", L"C:\\arbitrary\\application"}
    };
    for (const auto& mode : std::vector<std::vector<std::wstring>>{{L"--uninstall"}, {L"--uninstall-worker", token}}) {
        for (const auto& extra : conflicting) {
            auto after = mode;
            after.insert(after.end(), extra.begin(), extra.end());
            t.check(!parse_launch_options(after).error.empty(), "Uninstall rejects additional modes, paths and duplicate arguments");
            auto before = extra;
            before.insert(before.end(), mode.begin(), mode.end());
            t.check(!parse_launch_options(before).error.empty(), "Uninstall cannot follow an application or smoke mode");
        }
    }
    t.check(!parse_launch_options({L"--uninstall-worker"}).error.empty(), "Worker token is required");
    auto embedded_nul = std::wstring(32, L'a');
    embedded_nul[16] = L'\0';
    for (const auto& invalid : std::vector<std::wstring>{L"", std::wstring(31, L'a'), std::wstring(33, L'a'),
            std::wstring(32, L'A'), std::wstring(32, L'g'), std::wstring(32, L'\uFF41'), embedded_nul,
            L"--uninstall", L"C:\\arbitrary\\application", L"\\\\server\\share", L"0123456789abcdef0123456789abcde\nf"}) {
        const auto rejected = parse_launch_options({L"--uninstall-worker", invalid});
        t.check(!rejected.error.empty() && !rejected.uninstall && rejected.uninstall_worker_token.empty(),
            "Malformed worker tokens never select a maintenance operation");
    }
    const auto ordinary = parse_launch_options({});
    t.check(ordinary.error.empty() && !ordinary.uninstall && ordinary.uninstall_worker_token.empty(), "Ordinary startup remains the default");
    const auto observe = parse_launch_options({L"--observe-only", L"--background"});
    t.check(observe.error.empty() && observe.observe_only && observe.background && !observe.uninstall &&
        observe.uninstall_worker_token.empty(), "Read-only background startup remains separate");
    const auto smoke = parse_launch_options({L"--offline-smoke", L"--smoke-output", L"C:\\offline\\receipt.json"});
    t.check(smoke.error.empty() && smoke.offline_smoke && !smoke.uninstall && smoke.uninstall_worker_token.empty(),
        "Offline smoke keeps its existing isolated mode");
    const auto installed = parse_launch_options({L"--installation-ready", token, L"--background"});
    t.check(installed.error.empty() && installed.ready_token == token && installed.background && !installed.uninstall &&
        installed.uninstall_worker_token.empty(), "Installation handshake remains separate");
    for (const auto& invalid : std::vector<std::vector<std::wstring>>{
            {L"--offline-smoke"}, {L"--smoke-output", L"C:\\offline\\receipt.json"},
            {L"--offline-smoke", L"--observe-only", L"--smoke-output", L"C:\\offline\\receipt.json"},
            {L"--offline-smoke", L"--smoke-output", L"\\\\server\\share\\receipt.json"},
            {L"--installation-ready", token, L"--portable"}, {L"--portable", L"--portable"}}) {
        t.check(!parse_launch_options(invalid).error.empty(), "Existing startup and smoke restrictions remain enforced");
    }
}

void protocol_golden(Harness& t) {
    t.begin("protocol-golden");
    const auto g = szutest::load("shared/fixtures/protocol-golden.json");
    for (const auto& c : g.find("xencodeBase64")->items())
        t.equal(srun::xencode_base64(str(c, "data"), str(c, "key")), str(c, "expected"), "xencode " + str(c, "id"));
    for (const auto& c : g.find("hmacMD5")->items())
        t.equal(srun::hmac_md5_hex(str(c, "data"), str(c, "key")), str(c, "expected"), "hmac " + str(c, "id"));
    for (const auto& c : g.find("sha1")->items()) t.equal(srun::sha1_hex(str(c, "data")), str(c, "expected"), "sha1 " + str(c, "id"));
    for (const auto& c : g.find("payload")->items()) {
        const auto p = srun::payload({str(c, "account"), str(c, "password")}, str(c, "challenge"), str(c, "ip"), str(c, "acID"));
        t.equal(p.info_json, str(c, "infoJSON"), "payload infoJSON " + str(c, "id"));
        t.equal(p.info, str(c, "info"), "payload info " + str(c, "id"));
        if (c.find("hash")) t.equal(p.hash, str(c, "hash"), "payload hash " + str(c, "id"));
        if (c.find("checksum")) t.equal(p.checksum, str(c, "checksum"), "payload checksum " + str(c, "id"));
    }
    t.equal(srun::md5_hex(""), std::string("d41d8cd98f00b204e9800998ecf8427e"), "md5 empty");
    t.equal(srun::md5_hex("The quick brown fox jumps over the lazy dog"), std::string("9e107d9d372bb6826bd81d3542a419d6"), "md5 fox");
}

void rejection_cases(Harness& t) {
    t.begin("rejection-classification");
    const auto f = szutest::load("shared/fixtures/rejection-classification-cases.json");
    for (const auto& c : f.find("cases")->items())
        t.equal(std::string(to_string(wire::rejection_kind(*c.find("response")))), str(c, "kind"), str(c, "id"));
}

void website_cases(Harness& t) {
    t.begin("website-address");
    const auto f = szutest::load("shared/fixtures/website-address-cases.json");
    for (const auto& c : f.find("cases")->items()) {
        const auto got = website::normalize(str(c, "input"));
        if (c.find("ok")) t.check(got.ok() && got.url == str(c, "ok"), str(c, "id") + " -> " + got.url + got.error);
        else t.check(!got.ok() && got.error == str(c, "error"), str(c, "id") + " -> " + got.url + "|" + got.error);
    }
    t.equal(std::string(website::classify_status(204)), std::string("accessible"), "status 204");
    t.equal(std::string(website::classify_status(301)), std::string("redirected"), "status 301");
    t.equal(std::string(website::classify_status(407)), std::string("authenticationRequired"), "status 407");
    t.equal(std::string(website::classify_status(403)), std::string("forbidden"), "status 403");
    t.equal(std::string(website::classify_status(500)), std::string("httpStatus"), "status 500");
}

void route_cases(Harness& t) {
    t.begin("route-classification");
    const auto f = szutest::load("shared/fixtures/route-classification-cases.json");
    for (const auto& c : f.find("cases")->items()) {
        std::vector<routes::Interface> interfaces;
        const auto* ifs = c.find("interfaces");
        for (std::size_t i = 0; i < ifs->keys().size(); ++i) {
            const auto& v = ifs->items()[i];
            interfaces.push_back({ifs->keys()[i], v.bool_at("up").value_or(false), v.bool_at("running").value_or(true), str(v, "kind")});
        }
        std::vector<routes::Route> rs;
        for (const auto& r : c.find("routes")->items()) {
            routes::Route route;
            route.family = static_cast<int>(r.number_at("family").value_or(4));
            route.destination = str(r, "destination");
            route.prefix = static_cast<int>(r.number_at("prefix").value_or(0));
            route.interface_name = str(r, "interface");
            if (const auto* flags = r.find("flags"))
                for (const auto& flag : flags->items()) route.flags.push_back(flag.as_string());
            rs.push_back(route);
        }
        std::vector<std::string> views{"mac", "windows"};
        if (const auto* v = c.find("views")) {
            views.clear();
            for (const auto& x : v->items()) views.push_back(x.as_string());
        }
        for (const auto& view : views) {
            const auto e = routes::classify(rs, interfaces, view);
            t.check(e.tunnel == c.bool_at("tunnelEvidence").value_or(false) &&
                        e.count == static_cast<int>(c.number_at("evidenceCount").value_or(-1)),
                    str(c, "id") + "[" + view + "] count=" + std::to_string(e.count));
        }
    }
}

proxy::Snapshot snapshot_from(const json::Value& v) {
    proxy::Snapshot s;
    s.readable = v.bool_at("readable").value_or(false);
    s.http = v.bool_at("http").value_or(false);
    s.https = v.bool_at("https").value_or(false);
    s.socks = v.bool_at("socks").value_or(false);
    s.pac = v.bool_at("pac").value_or(false);
    s.auto_discovery = v.bool_at("autoDiscovery").value_or(false);
    if (const auto* e = v.find("exceptions"))
        for (const auto& x : e->items()) s.exceptions.push_back(x.as_string());
    return s;
}

bool summary_matches(const proxy::Summary& got, const json::Value& want) {
    std::vector<std::string> kinds;
    for (const auto& k : want.find("kinds")->items()) kinds.push_back(k.as_string());
    return got.state == str(want, "state") && got.kinds == kinds && got.campus_bypass == str(want, "campusBypass");
}

void proxy_cases(Harness& t) {
    t.begin("proxy-environment");
    const auto f = szutest::load("shared/fixtures/proxy-environment-cases.json");
    for (const auto& c : f.find("matcherCases")->items())
        t.equal(proxy::pattern_matches(str(c, "pattern"), str(c, "host")), c.bool_at("matches").value_or(false),
                "matcher " + str(c, "pattern") + " vs " + str(c, "host"));
    for (const auto& c : f.find("summaryCases")->items())
        t.check(summary_matches(proxy::summarize(snapshot_from(*c.find("snapshot"))), *c.find("expected")), "summary " + str(c, "id"));
    for (const auto& c : f.find("windowsRawCases")->items()) {
        const auto* r = c.find("raw");
        proxy::WindowsRaw raw;
        raw.readable = r->bool_at("readable").value_or(true);
        raw.proxy = str(*r, "proxy");
        raw.bypass = str(*r, "bypass");
        raw.auto_config_url = str(*r, "autoConfigUrl");
        raw.auto_detect = r->bool_at("autoDetect").value_or(false);
        t.check(summary_matches(proxy::summarize(proxy::parse_windows(raw)), *c.find("expected")), "windowsRaw " + str(c, "id"));
    }
}

void compat_cases(Harness& t) {
    t.begin("compat-diagnosis");
    const auto f = szutest::load("shared/fixtures/compat-diagnosis-cases.json");
    const auto* rules = f.find("directRules");
    t.equal(rules->items()[0].as_string(), std::string(compat::kDirectRules[0]), "rule 1 text");
    t.equal(rules->items()[1].as_string(), std::string(compat::kDirectRules[1]), "rule 2 text");
    for (const auto& c : f.find("cases")->items()) {
        const auto* in = c.find("input");
        compat::Input input;
        input.proxy = str(*in, "proxy", "unknown");
        input.tunnel = str(*in, "tunnel", "unknown");
        input.campus_direct = str(*in, "campusDirect", "notRun");
        input.campus_system = str(*in, "campusSystem", "notRun");
        input.public_direct = str(*in, "publicDirect", "notRun");
        input.public_system = str(*in, "publicSystem", "notRun");
        const auto got = compat::diagnose(input);
        const auto* want = c.find("expected");
        std::vector<std::string> recs;
        for (const auto& r : want->find("recommendations")->items()) recs.push_back(r.as_string());
        t.check(got.verdict == str(*want, "verdict") && got.recommendations == recs && got.offer_rules == want->bool_at("offerRules").value_or(false),
                str(c, "id") + " -> " + got.verdict);
        t.check(copy::contains("compat.verdict." + got.verdict + ".title"), "copy for verdict " + got.verdict);
        for (const auto& r : got.recommendations) t.check(copy::contains("compat.rec." + r), "copy for rec " + r);
    }
}

void status_cases(Harness& t) {
    t.begin("status-presenter");
    {
        EngineSnapshot s;
        s.status = EngineStatus::offline;
        t.check(status::system_path_can_lead(s), "system connectivity may explain an ordinary physical probe failure");
        for (const auto state : {EngineStatus::security, EngineStatus::rejected, EngineStatus::uncertain,
             EngineStatus::accepted, EngineStatus::coolingDown, EngineStatus::paused, EngineStatus::confirming}) {
            s.status = state;
            t.check(!status::system_path_can_lead(s), "system connectivity cannot conceal authentication or protection state");
        }
        s.status = EngineStatus::offline; s.paused = true;
        t.check(!status::system_path_can_lead(s), "system connectivity cannot conceal a manual pause");
        s.paused = false; s.cooldown_remaining = 90;
        t.check(!status::system_path_can_lead(s), "system connectivity cannot conceal a remaining cooldown");
        for (const auto prerequisite : {EngineStatus::needsTrust, EngineStatus::needsAccount}) {
            EngineSnapshot waiting; waiting.status = prerequisite;
            t.check(status::system_path_can_lead(waiting),
                "fresh system connectivity can lead while campus authentication waits for trust or an account");
            waiting.busy = true;
            t.check(!status::system_path_can_lead(waiting), "prerequisite display cannot conceal an active operation");
            waiting.busy = false; waiting.sleeping = true;
            t.check(!status::system_path_can_lead(waiting), "prerequisite display cannot conceal sleep");
            waiting.sleeping = false; waiting.paused = true;
            t.check(!status::system_path_can_lead(waiting), "prerequisite display cannot conceal an explicit pause");
            waiting.paused = false; waiting.cooldown_remaining = 90;
            t.check(!status::system_path_can_lead(waiting), "prerequisite display cannot conceal a submission cooldown");
            waiting.cooldown_remaining.reset();
            for (const auto reason : {HoldReason::userPaused, HoldReason::security, HoldReason::uncertain, HoldReason::credentials}) {
                Hold hold; hold.reason = reason; waiting.holds = {hold};
                t.check(!status::system_path_can_lead(waiting), "prerequisite display cannot conceal any remaining authentication hold");
            }
        }
        for (const auto state : {EngineStatus::checking, EngineStatus::discovering, EngineStatus::loggingIn,
             EngineStatus::verifying, EngineStatus::sleeping, EngineStatus::cancelled, EngineStatus::unavailable}) {
            EngineSnapshot protected_state; protected_state.status = state;
            t.check(!status::system_path_can_lead(protected_state), "system connectivity cannot replace an active, cancelled or failed authentication state");
        }
        s.cooldown_remaining.reset();
        s.network_available = true;
        s.online = true; s.trusted = true; s.remembered = true;
        t.equal(std::string(status::campus_verification_key(s)), std::string("env.campusVerification.notChecked"),
            "internet, trust and memory do not prove campus service identity");
        s.last_discovery = CampusDiscovery{Discovery::confirmed, Discovery::unconfirmed};
        s.area = CampusArea::dormitory;
        s.required_campus_verification = Discovery::unconfirmed;
        t.equal(std::string(status::campus_verification_key(s)), std::string("env.campusVerification.unconfirmed"),
            "teaching evidence does not verify a selected dormitory service");
        s.area = CampusArea::automatic;
        s.required_campus_verification = Discovery::confirmed;
        t.equal(std::string(status::campus_verification_key(s)), std::string("env.campusVerification.confirmed"),
            "automatic area reports current service verification");
        s.last_discovery = CampusDiscovery{Discovery::unconfirmed, Discovery::confirmed};
        s.required_campus_verification = Discovery::unconfirmed;
        for (const auto area : {CampusArea::automatic, CampusArea::dormitory, CampusArea::teaching}) {
            s.area = area;
            t.equal(std::string(status::campus_verification_key(s)), std::string("env.campusVerification.unconfirmed"),
                "unverified teaching TLS cannot be replaced by a dormitory HTTP response");
        }
        s.last_discovery = CampusDiscovery{Discovery::confirmed, Discovery::confirmed};
        s.area = CampusArea::dormitory;
        s.required_campus_verification = Discovery::confirmed;
        t.equal(std::string(status::campus_verification_key(s)), std::string("env.campusVerification.confirmed"),
            "selected dormitory service requires both fresh proofs");
        s.sleeping = true;
        t.equal(std::string(status::campus_verification_key(s)), std::string("env.campusVerification.notChecked"),
            "sleep cannot retain a verified service label");
        s.sleeping = false; s.network_available = false;
        t.equal(std::string(status::campus_verification_key(s)), std::string("env.campusVerification.notChecked"),
            "unavailable path cannot show verified service");
        s.network_available = true; s.last_discovery = CampusDiscovery{Discovery::notChecked, Discovery::notChecked};
        s.required_campus_verification = Discovery::notChecked;
        t.equal(std::string(status::campus_verification_key(s)), std::string("env.campusVerification.notChecked"),
            "not checked is distinct from unsuccessful verification");
    }
    {
        const auto signature = [](const status::Readiness& value) {
            std::string result = value.title + "|" + value.tone;
            for (const auto& item : value.items) result += "|" + item.kind + ":" + item.key + ":" + item.tone;
            return result;
        };
        EngineSnapshot remembered;
        remembered.mode = MonitoringMode::recover;
        remembered.status = EngineStatus::online;
        remembered.network_available = true;
        remembered.online = true;
        remembered.has_saved = true;
        remembered.remembered = true;
        remembered.recovery_setup_has_network = true;
        remembered.auto_login = AutoLoginState::awaitingProof;
        auto displayed = status::recovery_readiness(remembered);
        t.check(displayed.title == "ready.title.monitoring" && displayed.tone == "info" &&
            displayed.items.at(1).key == "ready.network.remembered" && displayed.items.at(1).tone == "info",
            "a configured online network is monitoring while fresh campus evidence is pending");
        t.equal(std::string(status::campus_verification_hint_key(remembered)), std::string("campus.verify.autoPendingHint"),
            "a remembered recover-mode attachment explains that fresh campus service verification runs automatically");
        t.check(remembered.auto_login == AutoLoginState::awaitingProof && !remembered.last_discovery && !remembered.trusted,
            "displaying monitoring never supplies campus evidence, session trust or recovery permission");
        for (const bool service_only : {false, true}) {
            auto checking = remembered;
            checking.verifying_memory = !service_only;
            checking.verifying_campus = service_only;
            checking.busy = true;
            checking.status = EngineStatus::discovering;
            displayed = status::recovery_readiness(checking);
            t.check(displayed.title == "ready.title.checking" && displayed.tone == "busy" &&
                displayed.items.at(1).key == "ready.network.knownChecking" && displayed.items.at(1).tone == "info",
                "remembering and service-only verification have a distinct cancellable progress presentation");
            t.equal(std::string(status::campus_verification_key(checking)), std::string("env.campusVerification.checking"),
                "current verification progress does not appear as an unchecked failure");
            t.equal(std::string(status::campus_verification_hint_key(checking)), std::string("campus.verify.checking"),
                "current campus verification has one progress explanation");
        }
        auto unsuccessful = remembered;
        unsuccessful.last_discovery = CampusDiscovery{Discovery::unconfirmed, Discovery::confirmed};
        unsuccessful.required_campus_verification = Discovery::unconfirmed;
        displayed = status::recovery_readiness(unsuccessful);
        t.check(displayed.title == "ready.title.verificationFailed" && displayed.tone == "warning" &&
            displayed.items.at(1).key == "ready.network.remembered" && displayed.items.at(1).tone == "info",
            "an unsuccessful service check preserves the remembered-network label without claiming verified identity");
        t.equal(std::string(status::campus_verification_hint_key(unsuccessful)), std::string("campus.verify.failedHint"),
            "failed service verification offers a retry explanation");
        displayed = status::recovery_readiness(remembered, true);
        t.check(displayed.title == "ready.title.verificationFailed" && displayed.tone == "warning",
            "a service-only check with no usable discovery still shows its failure");
        auto verified = remembered;
        verified.auto_login = AutoLoginState::ready;
        verified.last_discovery = CampusDiscovery{Discovery::confirmed, Discovery::confirmed};
        verified.required_campus_verification = Discovery::confirmed;
        displayed = status::recovery_readiness(verified);
        t.check(displayed.title == "ready.title.ready" && displayed.tone == "success" && displayed.items.at(1).tone == "success",
            "only current applicable campus proofs produce verified recovery readiness");
        t.equal(std::string(status::campus_verification_hint_key(verified)), std::string("campus.verify.confirmedHint"),
            "successful verification still explains that later recovery rechecks campus identity");
        auto offline = remembered;
        offline.online = false;
        offline.status = EngineStatus::offline;
        t.equal(std::string(status::campus_verification_hint_key(offline)), std::string("campus.verify.autoPendingHint"),
            "an offline remembered attachment explains its pending automatic verification without claiming internet access");
        for (const auto reason : {HoldReason::credentials, HoldReason::security, HoldReason::uncertain, HoldReason::userPaused}) {
            auto protected_state = remembered;
            protected_state.holds = {Hold{}};
            protected_state.holds.front().reason = reason;
            protected_state.pause_reason = reason;
            protected_state.paused = true;
            protected_state.auto_login = AutoLoginState::paused;
            t.equal(signature(status::recovery_readiness(protected_state)), signature(status::readiness(status::readiness_input_from(protected_state))),
                "remembered monitoring never overrides a password, TLS, uncertain-result or user pause");
            protected_state.paused = false;
            protected_state.auto_login = AutoLoginState::ready;
            t.equal(signature(status::recovery_readiness(protected_state)), signature(status::readiness(status::readiness_input_from(protected_state))),
                "even a scoped hold cannot be hidden by a remembered online network");
        }
        for (const auto state : {EngineStatus::security, EngineStatus::rejected, EngineStatus::uncertain, EngineStatus::paused,
             EngineStatus::coolingDown, EngineStatus::sleeping, EngineStatus::loggingIn, EngineStatus::verifying, EngineStatus::cancelled}) {
            auto protected_state = remembered;
            protected_state.status = state;
            t.equal(signature(status::recovery_readiness(protected_state)), signature(status::readiness(status::readiness_input_from(protected_state))),
                "monitoring presentation cannot replace an authentication, cancellation or protection status");
        }
        for (int condition = 0; condition < 6; ++condition) {
            auto protected_state = remembered;
            if (condition == 0) { protected_state.cooldown_remaining = 90; protected_state.auto_login = AutoLoginState::coolingDown; }
            if (condition == 1) { protected_state.has_saved = false; protected_state.auto_login = AutoLoginState::needsAccount; }
            if (condition == 2) protected_state.unsaved_changes = true;
            if (condition == 3) protected_state.read_only = true;
            if (condition == 4) protected_state.mode = MonitoringMode::observe;
            if (condition == 5) { protected_state.remembered = false; protected_state.recovery_setup_has_network = false; protected_state.auto_login = AutoLoginState::needsTrust; }
            t.equal(signature(status::recovery_readiness(protected_state)), signature(status::readiness(status::readiness_input_from(protected_state))),
                "cooldown, account setup, unsaved changes, read-only, observe and unknown networks retain their original readiness");
        }
    }
    const auto f = szutest::load("shared/status-fixtures.json");
    for (const auto& c : f.find("cases")->items()) {
        const auto* in = c.find("input");
        status::Input input;
        input.status = str(*in, "status");
        if (auto d = in->string_at("diagnosis")) input.diagnosis = *d;
        input.probe_mode = str(*in, "probeMode", "quick");
        input.passed_count = static_cast<int>(in->number_at("passedCount").value_or(0));
        input.mode = str(*in, "mode");
        input.read_only = in->bool_at("readOnly").value_or(false);
        input.has_draft = in->bool_at("hasDraft").value_or(false);
        input.auto_login = str(*in, "autoLogin");
        if (auto h = in->string_at("hold")) input.hold = *h;
        input.offer_forced_login = in->bool_at("offerForcedLogin").value_or(false);
        const auto p = status::present(input);
        const auto* want = c.find("expected");
        auto opt = [&](const char* key) -> std::optional<std::string> {
            const auto* v = want->find(key);
            if (!v || v->is_null()) return std::nullopt;
            return v->as_string();
        };
        bool ok = p.tone == str(*want, "tone") && p.headline == str(*want, "headline") && p.detail == str(*want, "detail") &&
                  p.primary == opt("primary") && p.secondary == opt("secondary") && p.chip == str(*want, "chip") &&
                  p.chip_tone == str(*want, "chipTone");
        const auto* banner = want->find("banner");
        if (!banner || banner->is_null()) ok = ok && !p.banner;
        else {
            std::optional<std::string> action;
            if (const auto* a = banner->find("action"); a && !a->is_null()) action = a->as_string();
            ok = ok && p.banner && p.banner->key == str(*banner, "key") && p.banner->tone == str(*banner, "tone") && p.banner->action == action;
        }
        t.check(ok, str(c, "id") + " " + str(c, "title"));
        for (const std::string& key : {p.headline, p.detail, p.chip}) t.check(copy::contains(key), "copy key " + key);
        if (p.banner) t.check(copy::contains(p.banner->key + ".title") && copy::contains(p.banner->key + ".detail"), "banner copy " + p.banner->key);
        for (const auto& a : {p.primary, p.secondary})
            if (a) t.check(copy::contains("action." + *a), "action copy " + *a);
    }
}

// ---------------- shared recovery scenarios on the real engine ----------------
struct Driver {
    szutest::ScriptedService service;
    double clock = 0;
    int network = 1;
    std::unique_ptr<ConnectionEngine> engine;

    explicit Driver(const json::Value& initial) {
        ConnectionEngine::Hooks hooks;
        hooks.now = [this] { return clock; };
        engine = std::make_unique<ConnectionEngine>(service, hooks);
        std::optional<Credentials> saved;
        if (const auto* s = initial.find("saved"); s && s->is_array())
            saved = Credentials{s->items()[0].as_string(), s->items()[1].as_string()};
        engine->configure(*mode_from(str(initial, "mode", "recover")), *area_from(str(initial, "area", "automatic")), ProbeMode::quick, saved);
        if (const auto* d = initial.find("draft"); d && d->is_array())
            engine->set_draft({d->items()[0].as_string(), d->items()[1].as_string()});
        path();
        if (initial.bool_at("trusted").value_or(false)) engine->trust_current_network();
    }
    void path() {
        PathSnapshot p;
        p.usable = true;
        p.identity = "fixture-network-" + std::to_string(network);
        p.link_active = true;
        engine->receive_path(p);
    }

    void apply(const json::Value& step) {
        const std::string action = str(step, "do");
        if (action == "configure") {
            if (const auto* p = step.find("probes"))
                for (const auto& x : p->items()) service.probes.push_back(x.as_string());
            if (auto d = step.string_at("defaultProbe")) service.default_probe = *d;
            if (const auto* d = step.find("discovery")) {
                service.discovery.clear();
                auto add = [&](const json::Value& v) {
                    service.discovery.push_back({{"teaching", str(v, "teaching", "unconfirmed")}, {"dormitory", str(v, "dormitory", "notChecked")}});
                };
                if (d->is_array()) for (const auto& v : d->items()) add(v);
                else add(*d);
            }
            if (const auto* a = step.find("attempts")) {
                service.attempts.clear();
                for (std::size_t i = 0; i < a->keys().size(); ++i) service.attempts[a->keys()[i]] = a->items()[i].as_string();
            }
        } else if (action == "automaticRound") {
            const int count = static_cast<int>(step.number_at("count").value_or(1));
            for (int i = 0; i < count; ++i) engine->automatic_round();
        } else if (action == "manualConnect") {
            engine->connect();
        } else if (action == "advance") {
            clock += step.number_at("seconds").value_or(0);
        } else if (action == "trust") {
            engine->trust_current_network();
        } else if (action == "revokeTrust") {
            engine->revoke_trust();
        } else if (action == "pathChange") {
            ++network;
            path();
        } else if (action == "sleepWake") {
            engine->set_sleeping(true);
            engine->set_sleeping(false);
            path();
        } else if (action == "saveCredentials") {
            engine->set_draft({str(step, "account"), str(step, "password")});
            engine->save_draft();
        } else if (action == "editDraft") {
            engine->set_draft({str(step, "account"), str(step, "password")});
        } else if (action == "revertDraft") {
            engine->revert_draft();
        } else if (action == "resume") {
            engine->resume_auto_login();
        } else if (action == "pause") {
            engine->pause_auto_login();
        } else if (action == "setMode") {
            engine->set_mode(*mode_from(str(step, "mode")));
        } else if (action == "setArea") {
            engine->set_area(*area_from(str(step, "area")));
        }
    }

    json::Value observe() const {
        const auto s = engine->snapshot();
        json::Value o = json::Value::object();
        auto pairs = [](const std::vector<std::pair<std::string, std::string>>& list) {
            json::Value out = json::Value::array();
            for (const auto& [a, b] : list) {
                json::Value item = json::Value::array();
                item.push(json::Value::string(a));
                item.push(json::Value::string(b));
                out.push(std::move(item));
            }
            return out;
        };
        o.set("submissions", pairs(service.submissions));
        o.set("attempts", pairs(service.attempt_log));
        o.set("paused", json::Value::boolean(s.paused));
        o.set("mode", json::Value::string(std::string(to_string(s.mode))));
        o.set("online", json::Value::boolean(s.online));
        std::vector<std::tuple<std::string, std::string, std::string>> holds;
        for (const auto& h : s.holds)
            holds.emplace_back(std::string(to_string(h.reason)), h.area ? std::string(to_string(*h.area)) : std::string(), h.sticky() ? "sticky" : "network");
        std::sort(holds.begin(), holds.end());
        json::Value list = json::Value::array();
        for (const auto& [reason, area, scope] : holds) {
            json::Value h = json::Value::object();
            h.set("reason", json::Value::string(reason));
            h.set("area", area.empty() ? json::Value() : json::Value::string(area));
            h.set("scope", json::Value::string(scope));
            list.push(std::move(h));
        }
        o.set("holdCount", json::Value::number(static_cast<double>(holds.size())));
        o.set("holds", std::move(list));
        o.set("autoLoginState", json::Value::string(std::string(to_string(s.auto_login))));
        o.set("lastSuccessArea", s.last_success_area ? json::Value::string(std::string(to_string(*s.last_success_area))) : json::Value());
        o.set("discoveries", json::Value::number(service.discoveries));
        o.set("submissionCount", json::Value::number(static_cast<double>(service.submissions.size())));
        return o;
    }
};

void auto_recovery_scenarios(Harness& t) {
    t.begin("auto-recovery");
    const auto f = szutest::load("shared/fixtures/auto-recovery-scenarios.json");
    std::vector<std::string> known;
    for (const auto& k : f.find("expectVocabulary")->items()) known.push_back(k.as_string());
    for (const auto& scenario : f.find("scenarios")->items()) {
        Driver driver(*scenario.find("initial"));
        int index = 0;
        for (const auto& step : scenario.find("steps")->items()) {
            ++index;
            driver.apply(step);
            if (str(step, "do") != "expect") continue;
            const json::Value observed = driver.observe();
            const json::Value* expect = step.find("expect");
            const json::Value& wanted = (expect && expect->is_object()) ? *expect : step;
            for (std::size_t i = 0; i < wanted.keys().size(); ++i) {
                const std::string& key = wanted.keys()[i];
                if (key == "do" || key == "note") continue;
                const json::Value* got = observed.find(key);
                if (!got) {
                    t.check(false, str(scenario, "id") + " step " + std::to_string(index) + ": unknown expectation " + key);
                    continue;
                }
                const std::string want_text = json::serialize(wanted.items()[i]);
                const std::string got_text = json::serialize(*got);
                t.check(want_text == got_text, str(scenario, "id") + " step " + std::to_string(index) + " " + key + ": want " + want_text + " got " + got_text);
            }
        }
    }
}

// ---------------- unit tests ----------------
void json_tests(Harness& t) {
    t.begin("json");
    for (const char* bad : {"", "{", "[1,]", "{\"a\":1,}", "01", "\"\\x\"", "\"a\nb\"", "\"\\ud800\"", "tru", "1e", "{\"a\" 1}", "[1] 2", "\xff"})
        t.check(!json::parse(bad), std::string("rejects ") + bad);
    std::string deep(70, '[');
    deep += std::string(70, ']');
    t.check(!json::parse(deep), "depth limit");
    const auto v = json::parse(R"({"a":[1,2.5,-3e2,true,false,null],"b":{"c":"\u6df1\u5927 \ud83d\ude00"},"a":"last"})");
    t.check(v.has_value(), "parses nested");
    if (v) {
        t.equal(v->string_at("a").value_or(""), std::string("last"), "duplicate key resolves to last");
        t.equal(v->find("b")->string_at("c").value_or(""), std::string("深大 😀"), "unicode escapes");
    }
    t.equal(json::quote("a\"b\\c/深\n"), std::string("\"a\\\"b\\\\c/深\\n\""), "quote");
    const auto round = json::parse(json::serialize(*v));
    t.check(round && json::serialize(*round) == json::serialize(*v), "serialize round-trip");
}

void wire_tests(Harness& t) {
    t.begin("portal-wire");
    struct AcCase { const char* html; const char* value; PortalError error; };
    const AcCase ac[] = {
        {"<html>no controller</html>", "", PortalError::protocolConflict},
        {"<script>var ac_id = 12;</script>", "12", PortalError::none},
        {"<script>var acid   : \"6\";</script>", "6", PortalError::none},
        {"{\"acid\":6,\"ac_id\":\"6\"}", "6", PortalError::none},
        {"<input type=\"hidden\" name=\"acid\" value=\"6\">", "6", PortalError::none},
        {"location.href='/srun_portal_pc?ac_id=5&theme=pro'", "5", PortalError::none},
        {"{\"ac_id\": \"7\"}", "7", PortalError::none},
        {"<input type=\"hidden\" name=\"ac_id\" value=\"3\">", "3", PortalError::none},
        {"ac_id=1; x; ac_id=2", "", PortalError::protocolConflict},
        {"acid:6; ac_id=1", "", PortalError::protocolConflict},
        {"{\"acid\":6,\"ac_id\":1}", "", PortalError::protocolConflict},
        {"ac_id=01; ac_id = 1", "1", PortalError::none},
        {"other_ac_id=9 data-ac_id=8 data-acid=6 xacid=7", "", PortalError::protocolConflict},
        {"ac_id=abc", "", PortalError::protocolConflict},
        {"acid:abc", "", PortalError::protocolConflict},
        {"acid:0", "", PortalError::protocolConflict},
        {"acid:123456789", "", PortalError::protocolConflict},
        {"ac_id=0", "", PortalError::protocolConflict},
        {"ac_id=123456789", "", PortalError::protocolConflict},
        {"<input name=\"ac_id\">", "", PortalError::protocolConflict},
        {"<input name=\"ac_id\" name=\"x\" value=\"4\">", "", PortalError::protocolConflict},
        {"AC_ID=8", "8", PortalError::none},
        {"ac_id=\"12", "", PortalError::protocolConflict},
        {"ac_idx=5", "", PortalError::protocolConflict},
    };
    for (const auto& c : ac) {
        const auto r = wire::ac_id(c.html);
        t.check(r.error == c.error && (c.error != PortalError::none || r.value == c.value), std::string("acID ") + c.html + " -> " + r.value + "/" + std::string(to_string(r.error)));
    }
    t.check(wire::ac_id("ac_id=1\xff").error == PortalError::response, "acID invalid utf8");
    t.check(wire::ac_id(std::string(wire::kMaxResponseBytes + 1, 'a')).error == PortalError::responseTooLarge, "acID too large");

    t.check(wire::object_from("jsonpReturn({\"error\":\"ok\"})").has_value(), "jsonpReturn");
    t.check(wire::object_from(" _({\"a\":1}); ").has_value(), "underscore callback with semicolon");
    t.check(!wire::object_from("cb({\"a\":1})"), "unknown callback");
    t.check(wire::object_from("{\"a\":1}").has_value(), "plain json");
    t.check(!wire::object_from("[1]"), "array is not an object");
    auto obj = [](const char* text) { return *json::parse(text); };
    t.check(wire::teaching_success(obj(R"({"error":"ok"})")), "teaching ok");
    t.check(wire::teaching_success(obj(R"({"error":"ok","st":1})")), "teaching ok st 1");
    t.check(wire::teaching_success(obj(R"({"st":"1"})")), "teaching st string");
    t.check(!wire::teaching_success(obj(R"({"error":"login_error"})")), "teaching error");
    t.check(!wire::teaching_success(obj(R"({})")), "teaching empty");
    t.check(!wire::teaching_success(obj(R"({"error":1})")), "teaching non-string");
    t.check(!wire::teaching_success(obj(R"({"error":"ok","st":0})")), "teaching st 0");
    for (const char* body : {R"({})", R"({"error":""})", R"({"error":"ok","res":"failed"})",
                              R"({"error":1})", R"({"error":"pending"})"}) {
        t.check(wire::teaching_verdict(obj(body)) == wire::LoginVerdict::uncertain,
                "ambiguous teaching acknowledgement stays uncertain");
    }
    for (const char* body : {R"({"error":"ok","res":"ok","st":1,"suc_msg":"ip_already_online_error"})",
                             R"({"error":"ok","error_msg":"ip_already_online_error"})",
                             R"({"error":"login_error","error_msg":"ip_already_online_error"})"}) {
        t.check(wire::teaching_verdict(obj(body)) == wire::LoginVerdict::uncertain && !wire::teaching_success(obj(body)),
                "Existing IP session proves neither credential acceptance nor rejection");
    }
    t.check(wire::teaching_verdict(obj(R"({"error":"login_error"})")) == wire::LoginVerdict::rejected,
            "explicit teaching refusal remains rejected");
    t.check(wire::teaching_verdict(obj(R"({"st":true})")) == wire::LoginVerdict::accepted,
            "legacy boolean positive status remains accepted");
    t.check(wire::teaching_verdict(obj(R"({"st":false})")) == wire::LoginVerdict::rejected,
            "legacy boolean negative status remains rejected");
    t.check(wire::dormitory_success(obj(R"({"result":0,"msg":""})")), "dorm result 0");
    t.check(wire::dormitory_success(obj(R"({"result":1,"msg":"认证成功"})")), "dorm msg success");
    t.check(wire::dormitory_success(obj(R"({"msg":"Portal协议认证成功！"})")), "dorm portal msg");
    t.check(!wire::dormitory_success(obj(R"({"msg":"认证失败"})")), "dorm failure word");
    t.check(!wire::dormitory_success(obj(R"({"result":0,"msg":"等待认证"})")), "dorm waiting");
    t.check(!wire::dormitory_success(obj(R"({"result":false})")), "dorm bool result");
    for (const char* body : {R"({})", R"({"result":1})", R"({"result":2,"msg":"认证成功"})",
                              R"({"result":false,"msg":"认证成功"})", R"({"result":0,"msg":"认证失败"})",
                              R"({"result":0,"msg":"等待认证"})"}) {
        t.check(wire::dormitory_verdict(obj(body)) == wire::LoginVerdict::uncertain,
                "ambiguous dormitory acknowledgement stays uncertain");
    }
    t.check(wire::dormitory_verdict(obj(R"({"result":1,"msg":"认证失败"})")) == wire::LoginVerdict::rejected,
            "explicit dormitory refusal remains rejected");
    const auto ch = wire::challenge(obj(R"({"error":"ok","res":"ok","challenge":"abc","client_ip":"10.0.0.2"})"));
    t.check(ch.error == PortalError::none && ch.challenge == "abc" && ch.ip == "10.0.0.2", "challenge ok");
    t.check(wire::challenge(obj(R"({"error":"not_online_error","challenge":"x","client_ip":"1"})")).error == PortalError::challengeRefused, "challenge refused");
    t.check(wire::challenge(obj(R"({"error":"ok","res":"fail"})")).error == PortalError::protocolConflict, "challenge conflict");
    t.check(wire::challenge(obj(R"({"error":"ok"})")).error == PortalError::protocolConflict, "challenge missing token");
    t.check(wire::challenge(obj(R"({})")).error == PortalError::protocolConflict, "challenge empty");

    t.check(wire::is_allowed("https://net.szu.edu.cn/"), "allow teaching home");
    t.check(!wire::is_allowed("https://net.szu.edu.cn/?a=1"), "home with query");
    t.check(wire::is_allowed("https://net.szu.edu.cn/cgi-bin/get_challenge?callback=_"), "allow challenge");
    t.check(!wire::is_allowed("https://user@net.szu.edu.cn/"), "reject userinfo");
    t.check(!wire::is_allowed("https://net.szu.edu.cn:8443/"), "reject port");
    t.check(!wire::is_allowed("http://net.szu.edu.cn/"), "reject http teaching");
    t.check(wire::is_allowed("http://172.30.255.42:801/eportal/portal/login?x=1"), "allow dorm login");
    t.check(!wire::is_allowed("http://172.30.255.42/eportal"), "dorm needs 801");
    t.check(!wire::is_allowed("https://evil.example/"), "reject other host");
    t.check(!wire::is_allowed("https://net.szu.edu.cn/#x"), "reject fragment");
    t.check(wire::is_allowed("https://www.apple.com/library/test/success.html"), "allow apple probe");
    t.check(wire::is_allowed("http://www.msftconnecttest.com/connecttest.txt"), "allow microsoft probe");
    const auto url = wire::make_url(wire::kDormitoryBase, "/eportal/portal/login", {{"user_password", "pass+&word"}, {"x", "a b/深"}});
    t.equal(url.value_or(""), std::string("http://172.30.255.42:801/eportal/portal/login?user_password=pass%2B%26word&x=a%20b%2F%E6%B7%B1"), "make_url encoding");
    t.check(!wire::make_url("https://evil.example", "/"), "make_url refuses unknown");
    t.check(wire::is_campus_homepage("<title>深圳大学</title> srun login", 200, "https://net.szu.edu.cn/"), "campus homepage");
    t.check(!wire::is_campus_homepage("<title>深圳大学</title> srun login", 302, "https://net.szu.edu.cn/"), "campus homepage status");
    t.check(!wire::is_campus_homepage("<title>深圳大学</title> login", 200, "https://net.szu.edu.cn/"), "campus homepage needs srun");
    t.check(wire::is_dormitory_homepage("<TITLE> ePortal </TITLE>", 200, "http://172.30.255.42:801/eportal", true), "dorm homepage");
    t.check(!wire::is_dormitory_homepage("<title>ePortal</title>", 200, "http://172.30.255.42:801/eportal", false), "dorm needs campus identity");
    t.check(wire::is_connectivity_success(" <HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>\n", 200), "apple success");
    t.check(wire::is_microsoft_success("Microsoft Connect Test", 200), "microsoft success");
    t.check(!wire::is_microsoft_success("Microsoft Connect Test", 302), "microsoft status");
    t.check(wire::outcome_for(PortalError::network, false) == Outcome::unavailable, "network before submission");
    t.check(wire::outcome_for(wire::after_submission(PortalError::network), true) == Outcome::uncertain, "network after submission");
    t.check(wire::outcome_for(PortalError::rejected, false) == Outcome::challengeRefused, "refusal before submission");
    t.check(wire::outcome_for(PortalError::tls, true) == Outcome::security, "tls is security");
}

void policy_tests(Harness& t) {
    t.begin("policy");
    ConnectionPolicy p;
    t.check(!p.begin(OperationKind::manual), "unavailable refuses");
    p.available = true;
    t.check(!p.begin(OperationKind::automatic), "automatic needs enable");
    p.automatic_enabled = true;
    const auto op = p.begin(OperationKind::automatic);
    t.check(op.has_value() && p.owns(*op), "begin owns");
    t.check(!p.begin(OperationKind::manual), "single active operation");
    p.cancel();
    t.check(!p.owns(*op) && !p.finish(*op), "cancel drops ownership");
    {
        const AuthenticationScope wired{"ethernet", "physical-12"}, wifi{"wifi", "physical-1"};
        ConnectionPolicy scopes;
        scopes.attempt_started(1000);
        t.equal(scopes.cooldown_remaining(1010, wifi).value_or(0), 80.0, "unknown submission keeps a global deadline on another interface");
        scopes.accepted(1000, wired);
        t.equal(scopes.cooldown_remaining(1010, wifi).value_or(0), 80.0, "accepted response still needs complete connectivity verification");
        scopes.verify_accepted_scope(wifi, 1010);
        t.check(scopes.cooldown_remaining(1010, wifi).has_value(), "a different interface cannot verify the accepted request");
        scopes.verify_accepted_scope(wired, 1010);
        t.check(!scopes.cooldown_remaining(1010, wifi), "settled wired success does not delay an independently bound Wi-Fi path");
        t.equal(scopes.cooldown_remaining(1010, wired).value_or(0), 80.0, "same physical interface still keeps its deadline");
        const auto older = scopes.cooldown_state();
        scopes.attempt_started(1010); scopes.restore_cooldown_state(older);
        t.check(!scopes.cooldown_remaining(1010, wifi) && scopes.cooldown_remaining(1010, wired) == 80.0,
            "definitely-unsent preflight restores both original deadline and settled scope");
        scopes.attempt_started(1010); scopes.accepted(1010, wifi); scopes.verify_accepted_scope(wifi, 1010);
        t.equal(scopes.cooldown_remaining(1020, wired).value_or(0), 70.0, "A-to-B-to-A retains A's original independent deadline");
        t.equal(scopes.cooldown_remaining(1020, wifi).value_or(0), 80.0, "new B submission owns its own ninety-second deadline");
        scopes.attempt_started(1020);
        t.equal(scopes.cooldown_remaining(1030, wired).value_or(0), 80.0, "an interrupted new request restores global protection");
        scopes.restore_cooldown(42, 1030);
        t.equal(scopes.cooldown_remaining(1030, wifi).value_or(0), 42.0, "persistent restoration never recovers RAM-only bypass eligibility");
    }
    t.equal(p.next_check_delay(false, 0, false), 30, "fresh cadence 30");
    p.observe_probe(true);
    p.observe_probe(true);
    t.equal(p.next_check_delay(false, 0, false), 45, "two stable rounds 45");
    p.observe_probe(true);
    t.equal(p.next_check_delay(false, 0, false), 60, "three stable rounds 60");
    t.equal(p.next_check_delay(true, 0, false), 120, "constrained 120");
    p.observe_probe(false);
    t.equal(p.next_check_delay(false, 0, false), 5, "first failure rechecks in 5");
    p.observe_probe(false);
    p.failed();
    t.equal(p.next_check_delay(false, 0, false), 60, "backoff 60 after one failed round");
    t.equal(p.next_check_delay(false, 0, true), 60, "paused 60");
    t.equal(p.next_check_delay(true, 0, true), 120, "paused constrained 120");
    p.attempt_started(100);
    t.equal(p.next_check_delay(false, 150, true), 15, "cooldown 15");
    p.resume();
    t.check(p.cooling_down(150) && !p.cooling_down(191), "resume keeps cooldown deadline");
    p.authorize();
    t.check(p.authorized(), "authorize when available");
    p.reset_for_path_change();
    t.check(!p.authorized(), "path change revokes trust");
    p.available = false;
    p.authorize();
    t.check(!p.authorized(), "cannot authorize unavailable path");

    ConnectionPolicy q;
    LoginAttempt rej;
    rej.outcome = Outcome::rejected;
    rej.rejection = RejectionKind::unspecified;  // areaMismatch remains network-scoped, even after repetition
    auto h1 = q.record_failure(true, CampusArea::teaching, 1, 5, rej);
    t.check(h1 && !h1->sticky(), "first candidate rejection is network-scoped");
    auto h2 = q.record_failure(true, CampusArea::teaching, 1, 5, rej);
    t.check(h2 && h2->sticky(), "second candidate rejection of unknown cause is sticky");
    ConnectionPolicy q2;
    rej.rejection = RejectionKind::areaMismatch;
    q2.record_failure(true, CampusArea::teaching, 1, 5, rej);
    auto m2 = q2.record_failure(true, CampusArea::teaching, 1, 5, rej);
    t.check(m2 && !m2->sticky() && m2->epoch == std::optional<std::uint64_t>(5), "repeated area mismatch stays network-scoped");
    q2.path_changed(6);
    t.check(q2.holds().empty(), "area mismatch disappears with the network");
    t.check(!q.record_failure(true, CampusArea::teaching, std::nullopt, 5, rej), "unsaved draft never holds");
    t.check(q.blocking(CampusArea::teaching, 1, 5) && !q.blocking(CampusArea::dormitory, 1, 5), "hold is area scoped");
    t.check(!q.blocking(CampusArea::teaching, 2, 5), "hold is generation scoped");
    q.credentials_changed(2);
    t.check(q.holds().empty(), "new credentials prune holds");
    LoginAttempt sec;
    sec.outcome = Outcome::security;
    q.record_failure(false, CampusArea::teaching, 2, 5, sec);
    t.check(q.global_hold(2, 5) && q.global_hold(std::nullopt, 9), "security hold is global");
    q.pause_by_user();
    t.check(q.global_hold(2, 5)->reason == HoldReason::userPaused, "user pause has priority");
    t.check(!q.settle_uncertain(), "no uncertain to settle");
    q.resume();
    t.check(q.holds().empty(), "resume clears holds");
    const CampusDiscovery both{Discovery::confirmed, Discovery::confirmed};
    t.check(plan(OperationKind::automatic, CampusArea::automatic, both, false, q, 1, 1).decision == PlanDecision::needsTrust, "auto dorm needs trust");
    t.check(plan(OperationKind::manual, CampusArea::automatic, both, false, q, 1, 1).target == CampusArea::dormitory, "manual dorm allowed");
    t.check(plan(OperationKind::automatic, CampusArea::teaching, {Discovery::unconfirmed, Discovery::notChecked}, true, q, 1, 1).decision == PlanDecision::unconfirmed, "unconfirmed teaching");
}

struct EngineFixture {
    szutest::ScriptedService service;
    double clock = 1000;
    std::vector<std::string> events;
    std::vector<ConnectivityReport> reports;
    std::unique_ptr<ConnectionEngine> engine;
    EngineFixture(MonitoringMode mode, CampusArea area, bool saved, bool read_only = false) {
        ConnectionEngine::Hooks hooks;
        hooks.now = [this] { return clock; };
        hooks.event = [this](const std::string& e) { events.push_back(e); };
        hooks.report = [this](const ConnectivityReport& r) { reports.push_back(r); };
        engine = std::make_unique<ConnectionEngine>(service, hooks, read_only);
        engine->configure(mode, area, ProbeMode::quick, saved ? std::optional<Credentials>(Credentials{"20260001", "pw"}) : std::nullopt);
        PathSnapshot p;
        p.usable = true;
        p.identity = "net-a";
        engine->receive_path(p);
        service.discovery = {{{"teaching", "confirmed"}, {"dormitory", "notChecked"}}};
    }
};

void engine_tests(Harness& t) {
    t.begin("engine");
    for (int boundary = 0; boundary < 5; ++boundary) {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        f.service.attempts["teaching"] = boundary == 4 ? "cancelled" : "success";
        f.service.during_attempt = [&] {
            if (boundary == 0) f.engine->cancel();
            if (boundary == 1) f.engine->set_sleeping(true);
            if (boundary == 2) { PathSnapshot changed; changed.usable = true; changed.identity = "net-b"; f.engine->receive_path(changed); }
            if (boundary == 3) f.engine->set_mode(MonitoringMode::observe);
        };
        f.engine->automatic_round(); f.engine->automatic_round();
        const auto cancelled_holds = f.engine->snapshot().holds;
        t.check(std::any_of(cancelled_holds.begin(), cancelled_holds.end(), [](const Hold& h) { return h.reason == HoldReason::uncertain; }),
                "Cancelled pending submission immediately holds in RAM across every boundary");
        f.service.during_attempt = nullptr;
        f.engine->set_sleeping(false);
        PathSnapshot back; back.usable = true; back.identity = "net-b"; f.engine->receive_path(back);
        f.engine->set_mode(MonitoringMode::recover);
        f.clock += 120;
        f.engine->automatic_round(); f.engine->automatic_round();
        t.check(f.service.submissions.size() == 1, "Expired cooldown cannot replay an unresolved cancelled submission");
        f.engine->set_probe_mode(ProbeMode::complete);
        f.service.probes = {"online"}; f.engine->check();
        t.check(!f.engine->snapshot().paused, "Complete all-pass check settles cancelled uncertainty");
    }

    {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        f.service.attempts["teaching"] = "success";
        f.service.probes = {"offline", "offline", "online"};
        f.engine->automatic_round();
        t.check(f.engine->snapshot().status == EngineStatus::confirming && f.service.submissions.empty(), "first failure only confirms");
        f.engine->automatic_round();
        const auto s = f.engine->snapshot();
        t.check(f.service.submissions.size() == 1 && s.online && s.status == EngineStatus::online, "second failure logs in once and verifies");
        t.check(s.auto_login == AutoLoginState::ready && s.cooldown_remaining.has_value(), "online presentation is ready while duplicate-submission deadline stays protected");
        f.service.probes = {"offline", "offline"};
        f.engine->automatic_round();
        f.engine->automatic_round();
        t.check(f.service.submissions.size() == 1 && f.engine->snapshot().status == EngineStatus::coolingDown, "no duplicate submission during cooldown");
        t.check(std::find(f.events.begin(), f.events.end(), std::string(copy::text("event.succeededTeaching"))) != f.events.end(), "success event recorded");
    }
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        f.service.attempts["teaching"] = "success";
        f.engine->check();
        t.check(f.service.attempt_log.empty() && f.engine->snapshot().status == EngineStatus::offline, "manual check never authenticates");
        t.check(!f.events.empty() && f.events.back() == copy::format("event.manualCheckFailed",
            {{"reason", std::string(copy::text("diagnosis.unreachable.title"))}}), "manual check records its diagnostic result");
    }
    {
        EngineFixture f(MonitoringMode::observe, CampusArea::teaching, true);
        f.service.attempts["teaching"] = "success";
        for (int i = 0; i < 4; ++i) f.engine->automatic_round();
        f.engine->connect();
        t.check(f.service.attempt_log.empty(), "observe mode never authenticates");
    }
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true, true);
        f.service.attempts["teaching"] = "success";
        f.engine->connect();
        for (int i = 0; i < 3; ++i) f.engine->automatic_round();
        t.check(f.service.attempt_log.empty(), "read-only session never authenticates");
        t.check(!f.engine->trust_current_network(), "read-only session cannot trust");
        f.engine->set_draft({"x", "y"});
        t.check(f.engine->save_draft() == ConnectionEngine::SaveResult::invalid, "read-only session cannot save");
    }
    {
        EngineFixture f(MonitoringMode::manual, CampusArea::teaching, true);
        f.service.attempts["teaching"] = "success";
        f.service.probes = {"online"};
        f.engine->connect();
        t.check(f.engine->snapshot().offer_forced_login && f.service.attempt_log.empty(), "online connect offers forced login");
        f.service.probes = {"online", "online"};
        f.engine->connect_even_if_online();
        t.check(f.service.submissions.size() == 1, "forced login submits once");
    }
    {
        EngineFixture f(MonitoringMode::manual, CampusArea::teaching, true);
        f.service.probes = {"online"};
        f.service.during_probe = [&] { f.engine->cancel(); };
        f.engine->connect();
        const auto s = f.engine->snapshot();
        t.check(s.status == EngineStatus::cancelled && !s.last_report.has_value() && !s.online, "cancelled probe cannot publish");
        f.service.during_probe = nullptr;
    }
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        f.service.attempts["teaching"] = "rejected:unspecified";
        f.engine->set_draft({"20269999", "other"});
        f.engine->connect();
        t.check(f.service.submissions.size() == 1 && f.engine->snapshot().holds.empty(), "unsaved draft rejection keeps saved account usable");
        t.check(f.engine->snapshot().status == EngineStatus::rejected, "rejection status");
    }
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        f.service.attempts["teaching"] = "success";
        f.service.default_probe = "offline";
        f.engine->connect();
        const auto s = f.engine->snapshot();
        t.check(s.status == EngineStatus::accepted && f.service.probe_calls == 4, "accepted after three read-only verification probes");
        f.engine->automatic_round();
        f.engine->automatic_round();
        t.check(f.service.submissions.size() == 1, "accepted state does not resubmit inside cooldown");
    }
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        f.service.attempts["teaching"] = "uncertain";
        f.engine->connect();
        t.check(f.engine->snapshot().paused && f.engine->snapshot().pause_reason == HoldReason::uncertain, "uncertain pauses");
        f.service.probes = {"online"};
        f.engine->check();
        t.check(!f.engine->snapshot().paused, "online settles uncertain");
    }
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::dormitory, true);
        t.check(f.engine->trust_current_network() && f.engine->snapshot().trusted, "trust with identity");
        PathSnapshot lost;
        f.engine->receive_path(lost);
        auto s = f.engine->snapshot();
        t.check(s.status == EngineStatus::noNetwork && !s.trusted && !s.network_available, "path loss revokes trust");
        PathSnapshot back;
        back.usable = true;
        back.identity = "net-a";
        f.engine->receive_path(back);
        s = f.engine->snapshot();
        t.check(s.status == EngineStatus::idle && !s.trusted && s.auto_login == AutoLoginState::needsTrust, "returning path needs trust again");
        f.engine->set_sleeping(true);
        t.check(f.engine->snapshot().status == EngineStatus::sleeping && !f.engine->connect(), "sleep refuses operations");
    }
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, false);
        f.engine->set_draft({"  20260002 ", "pw2"});
        t.check(f.engine->save_draft() == ConnectionEngine::SaveResult::saved && f.engine->snapshot().generation == std::optional<std::uint64_t>(1), "first save generation 1");
        t.check(f.engine->save_draft() == ConnectionEngine::SaveResult::unchanged, "same save is unchanged");
        t.equal(f.engine->snapshot().saved_account_hint, std::string("20••••02"), "masked hint");
        f.engine->set_draft({"", ""});
        t.check(f.engine->save_draft() == ConnectionEngine::SaveResult::invalid, "empty draft invalid");
    }
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        f.engine->pause_auto_login();
        auto s = f.engine->snapshot();
        t.check(s.paused && s.pause_reason == HoldReason::userPaused, "user pause");
        const auto p = status::present(status::input_from(s));
        t.equal(p.chip, std::string("auto.paused.chip"), "presenter chip from snapshot");
        f.engine->resume_auto_login();
        t.check(!f.engine->snapshot().paused, "resume");
    }
}

void records_tests(Harness& t) {
    t.begin("records");
    t.equal(records::iso8601(0), std::string("1970-01-01T00:00:00Z"), "epoch iso");
    t.equal(records::iso8601(1790000000), std::string("2026-09-21T14:13:20Z"), "2026 iso");
    for (double v : {0.0, 951782400.0, 1790000000.0, 4102444799.0})
        t.check(records::parse_iso8601(records::iso8601(v)) == v, "iso round trip " + std::to_string(v));
    t.check(!records::parse_iso8601("2026-13-01T00:00:00Z"), "invalid month");
    for (const auto text : {"2026/01-01T00:00:00Z", "2026-01-01T-1:00:00Z", "2026-01-01T00: 1:00Z",
                            "2026-01-01T00:00:+1Z", "2026-01-01T00:00:00x", "2026-01-01T00:00:00Zx"})
        t.check(!records::parse_iso8601(text), "reject malformed fixed-width timestamp");
    records::EventLog log;
    t.check(log.append(0, "a") && !log.append(100, "a") && log.append(400, "a"), "dedupe within 300s");
    for (int i = 0; i < 400; ++i) log.append(1000 + i, "m" + std::to_string(i));
    t.equal(log.items().size(), std::size_t(300), "event limit 300");
    const auto back = records::EventLog::from_json(json::serialize(log.to_json()));
    t.check(back && back->items().size() == 300 && back->items().back().message == "m399", "event log round trip");
    t.check(!records::EventLog::from_json("{\"events\":[{\"date\":1}]}"), "corrupt event log detected");
    records::HealthLog health;
    for (int i = 0; i < 500; ++i) {
        records::Sample s;
        s.time = 100000 + i * 60.0;
        s.online = i % 4 != 0;
        s.latency_ms = s.online ? 50 + i % 7 : -1;
        health.append(s);
    }
    t.equal(health.items().size(), std::size_t(360), "health limit 360");
    const auto sum = health.summary(100000 + 499 * 60.0);
    t.check(sum.samples == 360 && sum.success_percent() == 75 && sum.median_latency_ms.has_value(), "health summary");
    records::Sample old;
    old.time = 0;
    health.append(old);
    t.check(std::none_of(health.items().begin(), health.items().end(), [](const records::Sample& s) { return s.time == 0; }), "stale samples pruned");
    records::ExportContext ctx{"0.7.0", "15", "windows", "recover", "automatic", "quick", 100000 + 499 * 60.0};
    const std::string exported = json::serialize(records::diagnostics_export(ctx, health, log));
    t.check(exported.find("20260001") == std::string::npos && exported.find("pw-secret") == std::string::npos, "export has no credentials");
    t.check(exported.find("\"privacy\"") != std::string::npos && exported.find("\"formatVersion\":1") != std::string::npos, "export envelope");
    t.check(!records::diagnostics_summary_text(ctx, health).empty(), "summary text");
    t.check(records::diagnostics_summary_text(ctx, health).find("0.7.0 · windows") != std::string::npos &&
        records::diagnostics_summary_text(ctx, health).find("(15)") == std::string::npos,
        "public summary shows the product version without an internal build suffix");

    const auto schema = szutest::load("shared/schemas/diagnostics-export.schema.json");
    const auto* definitions = schema.find("$defs");
    const auto* physical_schema = definitions->find("physicalNetwork");
    const auto* allowed_reasons = physical_schema->find("properties")->find("rejections")->find("properties");
    t.check(physical_schema->bool_at("additionalProperties") == false &&
        physical_schema->find("properties")->find("rejections")->bool_at("additionalProperties") == false &&
        definitions->find("counter")->number_at("maximum") == 4096 &&
        definitions->find("reasonCounter")->number_at("minimum") == 1,
        "physical export schema excludes raw properties and bounds nonzero rejection counters");
    std::map<std::string, physical::CandidateRejection> reasons;
    for (int value = static_cast<int>(physical::CandidateRejection::systemSnapshotUnavailable);
            value <= static_cast<int>(physical::CandidateRejection::noIPv4Service); ++value) {
        const auto reason = static_cast<physical::CandidateRejection>(value);
        reasons.emplace(physical::to_string(reason), reason);
        t.check(allowed_reasons->find(physical::to_string(reason)) != nullptr, "every exported rejection has an explicit schema property");
    }
    t.equal(allowed_reasons->size(), reasons.size(), "schema accepts only the fixed Windows rejection vocabulary");
    const auto fixtures = szutest::load("shared/fixtures/physical-diagnostics-export-cases.json");
    for (const auto& fixture : fixtures.find("cases")->items()) {
        physical::CandidateDiagnostics diagnostics;
        diagnostics.interface_count = static_cast<std::size_t>(*fixture.number_at("interfaceCount"));
        diagnostics.candidate_count = static_cast<std::size_t>(*fixture.number_at("candidateCount"));
        const auto* counts = fixture.find("rejections");
        for (const auto& key : counts->keys())
            diagnostics.rejections[reasons.at(key)] = static_cast<unsigned>(*counts->number_at(key));
        diagnostics.rejections[static_cast<physical::CandidateRejection>(999)] = 4096;
        ctx.physical_network = diagnostics;
        const auto report = records::diagnostics_export(ctx, health, log);
        const auto* actual = report.find("physicalNetwork");
        const auto* expected = fixture.find("expected");
        t.check(actual && actual->size() == 3 && actual->number_at("physicalInterfaceCount") == expected->number_at("physicalInterfaceCount") &&
            actual->number_at("candidateCount") == expected->number_at("candidateCount") &&
            actual->find("rejections")->size() == expected->find("rejections")->size(), "physical export preserves bounded fixture counts: " + str(fixture, "id"));
        for (const auto& key : expected->find("rejections")->keys())
            t.check(actual->find("rejections")->number_at(key) == expected->find("rejections")->number_at(key), "fixed rejection survives export: " + key);
        t.equal(records::physical_identification_key(diagnostics, false), str(fixture, "detailKey"), "UI diagnostic reason uses the same sanitized evidence");
        t.check(records::diagnostics_summary_text(ctx, health).find(str(fixture, "summary")) != std::string::npos,
            "copied summary and JSON use the same bounded counters and whitelist");
        t.check(json::parse(json::serialize(report)).has_value(), "extended diagnostics remain valid JSON");
    }
    ctx.physical_network.reset();
    t.check(!records::diagnostics_export(ctx, health, log).find("physicalNetwork"), "older contexts without physical capture remain compatible");
    t.check(!records::diagnostics_export(ctx, health, log).find("environment") &&
        !records::diagnostics_export(ctx, health, log).find("recovery"), "older diagnostic contexts omit optional environment and recovery");
    records::DiagnosticEnvironment environment;
    environment.connection = "ethernet"; environment.system_proxy = "on"; environment.tun = "detected";
    ctx.environment = environment;
    records::DiagnosticRecovery recovery;
    recovery.state = "paused"; recovery.paused = recovery.network_available = recovery.network_remembered = true;
    recovery.hold_reasons = {HoldReason::security, HoldReason::userPaused, HoldReason::security, static_cast<HoldReason>(999)};
    recovery.cooldown_remaining = 12.01;
    ctx.recovery = recovery;
    auto report = records::diagnostics_export(ctx, health, log);
    t.check(report.find("environment")->string_at("connection") == "ethernet" &&
        report.find("environment")->string_at("systemProxy") == "on" && report.find("environment")->string_at("tun") == "detected" &&
        report.find("recovery")->find("holdReasons")->size() == 2 &&
        report.find("recovery")->find("holdReasons")->items()[0].as_string() == "userPaused" &&
        report.find("recovery")->number_at("cooldownRemainingSeconds") == 13,
        "diagnostics export current fixed environment, ordered unique protections and rounded cooldown");
    const auto copied = records::diagnostics_summary_text(ctx, health);
    t.check(copied.find(copy::format("records.diagnostics.environment", {{"access", std::string(copy::text("env.access.ethernet"))},
        {"proxy", std::string(copy::text("env.proxy.on"))}, {"tunnel", std::string(copy::text("env.tunnel.detected"))}})) != std::string::npos &&
        copied.find(std::string(copy::text("records.diagnostics.protection.userPaused"))) != std::string::npos &&
        copied.find(copy::format("records.diagnostics.cooldown", {{"seconds", "13"}})) != std::string::npos,
        "copied diagnostics describe the same anonymous environment, protections and cooldown");
    ctx.environment->connection = ctx.environment->system_proxy = ctx.environment->tun = "fixture-private-name-192.0.2.7";
    ctx.recovery->state = "fixture-private-scope";
    ctx.recovery->cooldown_remaining = 1e100;
    report = records::diagnostics_export(ctx, health, log);
    t.check(report.find("environment")->string_at("connection") == "unknown" &&
        report.find("environment")->string_at("systemProxy") == "unknown" && report.find("environment")->string_at("tun") == "unknown" &&
        report.find("recovery")->string_at("state") == "unknown" && report.find("recovery")->number_at("cooldownRemainingSeconds") == 86400 &&
        json::serialize(report).find("fixture-private") == std::string::npos &&
        records::diagnostics_summary_text(ctx, health).find("fixture-private") == std::string::npos,
        "diagnostic boundary rejects arbitrary environment and state strings and bounds the cooldown count");
    for (const double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        ctx.recovery->cooldown_remaining = invalid;
        t.check(!records::diagnostics_export(ctx, health, log).find("recovery")->find("cooldownRemainingSeconds"),
            "diagnostics omit absent, expired or nonfinite cooldown values");
    }
    ctx.environment = environment; ctx.environment->refreshing = true;
    report = records::diagnostics_export(ctx, health, log);
    t.check(report.find("environment")->bool_at("refreshing") == true &&
        report.find("environment")->string_at("connection") == "unknown" && report.find("environment")->string_at("systemProxy") == "unknown" &&
        report.find("environment")->string_at("tun") == "unknown", "pending environmental capture cannot export old values as current");
    const auto* schema_properties = schema.find("properties");
    t.check(schema_properties->find("environment")->bool_at("additionalProperties") == false &&
        schema_properties->find("recovery")->bool_at("additionalProperties") == false &&
        schema_properties->find("recovery")->find("properties")->find("holdReasons")->number_at("maxItems") == 4 &&
        schema_properties->find("recovery")->find("properties")->find("cooldownRemainingSeconds")->number_at("maximum") == 86400,
        "optional diagnostic schema bounds protections and cooldown and excludes raw environment fields");
}

void recovery_journal_tests(Harness& t) {
    t.begin("durable-recovery");
    szutest::ScriptedService service;
    service.discovery = {{{"teaching", "confirmed"}, {"dormitory", "notChecked"}}};
    service.attempts["teaching"] = "success";
    service.probes = {"offline", "online"};
    double clock = 0, wall = 1000;
    bool storage_ok = true;
    std::string stored;
    std::vector<std::string> writes;
    auto make = [&](std::optional<std::string> journal = std::nullopt) {
        ConnectionEngine::Hooks hooks;
        hooks.now = [&] { return clock; };
        hooks.wall_now = [&] { return wall; };
        hooks.persist_recovery = [&](const std::string& value) {
            if (!storage_ok) return false;
            stored = value; writes.push_back(value); return true;
        };
        auto engine = std::make_unique<ConnectionEngine>(service, hooks);
        engine->configure(MonitoringMode::recover, CampusArea::teaching, ProbeMode::quick, Credentials{"fixture-account", "fixture-password"});
        engine->restore_recovery(journal);
        PathSnapshot path; path.usable = true; path.identity = "fixture-network";
        engine->receive_path(path);
        return engine;
    };
    auto engine = make();
    engine->pause_auto_login();
    auto paused = make(stored);
    t.check(paused->paused() && paused->snapshot().pause_reason == HoldReason::userPaused, "Explicit pause survives process reconstruction");
    t.check(!paused->snapshot().trusted, "Recovery journal never restores network trust");
    paused->resume_auto_login();
    paused->connect();
    t.check(service.submissions.size() == 1, "Explicitly resumed engine submits once");
    t.check(stored.find("fixture-account") == std::string::npos && stored.find("fixture-password") == std::string::npos && stored.find("fixture-network") == std::string::npos,
            "Journal contains neither credentials nor network identity");
    auto restarted = make(stored);
    t.check(restarted->snapshot().cooldown_remaining.value_or(0) > 89, "Submission cooldown survives restart");
    restarted->automatic_round(); restarted->automatic_round();
    t.check(service.submissions.size() == 1, "Restart cannot bypass the cooldown");
    std::optional<std::string> pending;
    for (const auto& text : writes) {
        const auto v = json::parse(text);
        if (v) for (const auto& hold : v->find("holds")->items())
            if (hold.number_at("reason") == static_cast<int>(HoldReason::uncertain)) pending = text;
    }
    t.check(pending.has_value(), "A pending uncertain marker is durable before awaiting authentication");
    auto interrupted = make(pending);
    t.check(interrupted->paused() && interrupted->snapshot().pause_reason == HoldReason::uncertain,
            "Interrupted submission remains uncertain on next launch");
    auto corrupt = make(std::string("{broken"));
    t.check(corrupt->paused() && corrupt->snapshot().pause_reason == HoldReason::security, "Corrupt journal stops submission");
    const auto before = service.submissions.size();
    auto failed = make(); storage_ok = false;
    failed->connect();
    t.check(service.submissions.size() == before && failed->snapshot().pause_reason == HoldReason::security,
            "Storage failure stops credentials before transport");
    storage_ok = true;
    service.attempts["teaching"] = "rejected:password";
    auto rejected = make(); rejected->connect();
    auto rejected_again = make(stored);
    t.check(rejected_again->paused() && rejected_again->snapshot().pause_reason == HoldReason::credentials,
            "Password rejection survives a new process");
    service.default_probe = "online"; rejected_again->check();
    t.check(rejected_again->paused(), "Internet access alone never clears a persisted password rejection");
    clock = 200; wall = 1200;
    auto expired = make(stored);
    t.check(!expired->snapshot().cooldown_remaining, "Expired deadline is not revived on restart");
}

void internet_check_tests(Harness& t) {
    t.begin("integrated-internet-verdict");
    const WebsiteTarget enabled{"a", "https://alpha.example/", true};
    const WebsiteTarget disabled{"b", "https://beta.example/", false};
    auto base = ConnectivityReport::simple(true, true); base.mode = ProbeMode::complete;
    InternetCheckReport report{base, {}, 0, false};
    t.check(report.verdict() == InternetVerdict::allPassed, "Complete without sites still requires both base probes");
    report.selected_count = 1;
    t.check(report.verdict() == InternetVerdict::partial, "Missing selected result cannot become full success");
    report.websites.push_back({enabled, {"accessible", 200, 0.01}});
    t.check(report.verdict() == InternetVerdict::allPassed && report.passed_count() == 3 && report.expected_count() == 3,
        "Complete all-pass includes both base endpoints and every selected site");
    for (const char* state : {"redirected", "authenticationRequired", "forbidden", "httpStatus", "dns", "tls", "timeout", "network", "cancelled", "notRun"}) {
        report.websites[0].result = {state, 0, 0};
        t.check(report.verdict() == InternetVerdict::partial && report.base.online(), std::string("Site ") + state + " keeps core online and makes verdict partial");
    }
    report.websites[0].result = {"accessible", 302, 0};
    t.check(report.verdict() == InternetVerdict::partial, "Accessible token without 2xx is not accepted");
    report.base = ConnectivityReport::simple(false, false); report.base.mode = ProbeMode::complete;
    report.websites[0].result = {"accessible", 204, 0};
    t.check(report.verdict() == InternetVerdict::partial && !report.base.online(), "A custom site does not impersonate campus/base proof");
    report.websites[0].result = {"timeout", 0, 0};
    t.check(report.verdict() == InternetVerdict::unavailable, "No successful endpoint reports unavailable");
    report.cancelled = true;
    t.check(report.verdict() == InternetVerdict::cancelled, "Cancelled aggregate never claims success");
    report.cancelled = false;
    report.base.apple.state = report.base.microsoft.state = EndpointState::cancelled;
    t.check(report.verdict() == InternetVerdict::cancelled, "Both cancelled base endpoints preserve cancelled aggregate verdict");
    report.base = base; report.base.mode = ProbeMode::quick;
    t.check(report.verdict() == InternetVerdict::basicPassed && report.expected_count() == 2, "Quick remains basic even with selected sites");

    t.begin("integrated-internet-configuration");
    t.check(validated_website_targets({enabled, disabled}).has_value(), "Valid enabled/disabled rows retained");
    t.check(!validated_website_targets({enabled, enabled}), "Duplicate ids and URLs rejected");
    t.check(!validated_website_targets({enabled, {"a", "https://beta.example/", true}}), "Duplicate ids rejected independently");
    t.check(!validated_website_targets({enabled, {"x", "https://alpha.example/", false}}), "Duplicate normalized URL rejected even disabled");
    t.check(!validated_website_targets({{"a", "https://u:p@example.com/", true}}), "Userinfo rejected before transport");
    t.check(!validated_website_targets({{"a", "https://example.com/?token=secret", true}}), "Query secrets rejected before transport");
    t.check(!validated_website_targets({{"a", "file:///tmp/private", true}}), "Nonweb scheme rejected before transport");
    t.check(!validated_website_targets({{"", "https://example.com/", true}}), "Empty row identity rejected");
    std::vector<WebsiteTarget> twelve;
    for (int i = 1; i <= 12; ++i) twelve.push_back({"expanded-" + std::to_string(i), "https://expanded-" + std::to_string(i) + ".invalid/", true});
    t.check(validated_website_targets(twelve).has_value(), "Twelve unique selected rows accepted");
    auto thirteen = twelve; thirteen.push_back({"thirteen", "https://thirteen.invalid/", true});
    t.check(!validated_website_targets(thirteen), "A thirteenth configured row is rejected");
    const auto normalized = validated_website_targets({{"a", "alpha.example", true}});
    t.check(normalized && normalized->front().url == "https://alpha.example/", "Missing scheme normalized with existing contract");

    t.begin("integrated-internet-execution");
    szutest::ScriptedService raw; raw.default_probe = "online";
    std::atomic<int> calls{0}; std::atomic<bool> saw_system{false};
    InternetCheckService service(raw, [&](const std::string& url, bool follow, const CancelToken&) {
        ++calls; saw_system = follow;
        return WebsiteCheckResult{url == enabled.url ? "timeout" : "accessible", url == enabled.url ? 0 : 200, 0.01};
    });
    t.check(service.configure({enabled, disabled}, true), "Configuration accepted");
    const auto quick = service.probe(ProbeMode::quick, CancelToken{});
    t.check(quick.online() && calls == 0 && service.latest()->selected_count == 1 && service.latest()->websites.empty(), "Quick deliberately skips all selected websites");
    const auto complete = service.probe(ProbeMode::complete, CancelToken{});
    t.check(complete.all_passed() && calls == 1 && saw_system && service.latest()->verdict() == InternetVerdict::partial,
        "Full check waits for only enabled sites and leaves base all-pass intact");
    const auto captured_report = *service.latest();
    t.check(captured_report.follow_system_proxy, "Completed report captures system strategy");
    const WebsiteTarget preset{"preset", "https://preset.example/", false};
    const auto captured_base_calls = raw.probe_calls;
    const auto retains_report = [&] {
        const auto current = service.latest();
        return current && current->base.mode == captured_report.base.mode &&
            current->base.apple.state == captured_report.base.apple.state && current->base.microsoft.state == captured_report.base.microsoft.state &&
            current->base.apple.elapsed == captured_report.base.apple.elapsed && current->base.microsoft.elapsed == captured_report.base.microsoft.elapsed &&
            current->base.complete_result == captured_report.base.complete_result && current->selected_count == captured_report.selected_count &&
            current->follow_system_proxy == captured_report.follow_system_proxy && current->websites.size() == 1 &&
            current->websites.front().target.id == captured_report.websites.front().target.id &&
            current->websites.front().target.url == captured_report.websites.front().target.url &&
            current->websites.front().result.state == captured_report.websites.front().result.state &&
            current->websites.front().result.elapsed == captured_report.websites.front().result.elapsed;
    };
    t.check(service.configure({enabled, disabled, preset}, true) && retains_report() && calls == 1 && raw.probe_calls == captured_base_calls,
        "Adding an unselected preset retains the completed result, actual mode and request durations without starting requests");
    const std::vector<WebsiteTarget> edited_unselected_targets{{"other-preset", "https://other-preset.example/", false}, enabled};
    t.check(service.configure(edited_unselected_targets, true) && retains_report() && calls == 1 &&
        raw.probe_calls == captured_base_calls,
        "Removing, editing and moving only disabled rows cannot replace the selected results or start requests");
    const std::vector<WebsiteTarget> equivalent_selected_targets{{"a", "HTTPS://ALPHA.EXAMPLE:0443", true}};
    t.check(service.configure(equivalent_selected_targets, true) && retains_report() && calls == 1 &&
        raw.probe_calls == captured_base_calls, "Equivalent normalized selected targets preserve their existing report");
    raw.default_probe = "offline";
    const auto private_hints = service.probe_verification_hints(CancelToken{});
    t.check(!private_hints.online() && private_hints.mode == ProbeMode::quick && calls == 1 && service.latest() &&
        service.latest()->base.mode == captured_report.base.mode && service.latest()->base.apple.state == captured_report.base.apple.state &&
        service.latest()->base.microsoft.state == captured_report.base.microsoft.state &&
        service.latest()->base.apple.elapsed == captured_report.base.apple.elapsed &&
        service.latest()->websites.size() == captured_report.websites.size() &&
        service.latest()->websites.front().result.state == captured_report.websites.front().result.state &&
        service.latest()->websites.front().result.elapsed == captured_report.websites.front().result.elapsed &&
        service.latest()->follow_system_proxy == captured_report.follow_system_proxy,
        "independent fixed hints bypass website checks and retain published base results, timing, site cache and strategy");
    raw.default_probe = "online";
    t.check(!service.configure({enabled, enabled}, false) && service.latest().has_value(), "Invalid configuration does not disturb accepted results");
    service.configure({enabled}, false);
    t.check(!service.latest(), "Configuration change clears previous result");
    service.probe(ProbeMode::complete, CancelToken{});
    t.check(!saw_system, "Site proxy strategy is passed explicitly");
    t.check(!service.latest()->follow_system_proxy && captured_report.follow_system_proxy,
        "New direct result does not relabel an already opened system result");
    CancelToken stopped; stopped.cancel(); const auto before = calls.load();
    service.probe(ProbeMode::complete, stopped);
    t.check(calls == before && !service.latest(), "Pre-cancelled operation sends zero site requests and publishes no result");

    // Deterministic barrier proves concurrent bounded execution and stale suppression, without networking or sleeps.
    std::mutex gate_mutex; std::condition_variable gate_cv; int entered = 0; bool release = false;
    InternetCheckService delayed(raw, [&](const std::string&, bool, const CancelToken&) {
        std::unique_lock<std::mutex> lock(gate_mutex); ++entered; gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release; });
        return WebsiteCheckResult{"accessible", 200, 0};
    });
    delayed.configure(twelve, true);
    CancelToken parent;
    std::thread worker([&] { delayed.probe(ProbeMode::complete, parent); });
    bool all_entered;
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        all_entered = gate_cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered == 12; });
    }
    t.check(all_entered && entered == 12, "All twelve website checks execute concurrently within the accepted bound");
    delayed.configure({disabled}, true);
    t.check(parent.cancelled(), "Configuration replacement cancels the entire engine probe generation");
    { std::lock_guard<std::mutex> lock(gate_mutex); release = true; }
    gate_cv.notify_all(); worker.join();
    t.check(!delayed.latest(), "A checker that returns success after cancellation cannot publish stale success");
    delayed.probe(ProbeMode::complete, CancelToken{});
    t.check(delayed.latest() && delayed.latest()->selected_count == 0, "New round uses replacement selection snapshot");

    t.begin("integrated-internet-effective-selection");
    const WebsiteTarget second{"second", "https://second.example/", true};
    for (const auto& replacement : std::vector<std::vector<WebsiteTarget>>{
             {enabled, {"second", second.url, false}}, {enabled, {"renamed", second.url, true}},
             {enabled, {"second", "https://changed.example/", true}}, {second, enabled}, {enabled, second, {preset.id, preset.url, true}}}) {
        szutest::ScriptedService configured_base; configured_base.default_probe = "online";
        std::atomic<int> configured_calls{0};
        InternetCheckService configured(configured_base, [&](const std::string&, bool, const CancelToken&) {
            ++configured_calls; return WebsiteCheckResult{"accessible", 200, 0.25};
        });
        configured.configure({enabled, second}, true); configured.probe(ProbeMode::complete, CancelToken{});
        const int before_change = configured_calls.load();
        t.check(configured.latest() && configured.configure(replacement, true) && !configured.latest() && configured_calls == before_change,
            "Changing an enabled target, row identity, selected order or selection invalidates without issuing replacement requests");
    }
    for (const std::string change : {"disabledOnly", "enabled", "strategy", "websitePath", "cancel", "invalidate"}) {
        szutest::ScriptedService active_base; active_base.default_probe = "online";
        std::mutex active_mutex; std::condition_variable active_cv;
        bool active_entered = false, active_release = false;
        InternetCheckService active(active_base, [&](const std::string&, bool, const CancelToken&) {
            std::unique_lock lock(active_mutex); active_entered = true; active_cv.notify_all();
            active_cv.wait(lock, [&] { return active_release; });
            return WebsiteCheckResult{"accessible", 200, 0.125};
        });
        active.configure({enabled}, true);
        CancelToken active_parent;
        std::thread active_worker([&] { active.probe(ProbeMode::complete, active_parent); });
        bool reached_checker;
        { std::unique_lock lock(active_mutex); reached_checker = active_cv.wait_for(lock, std::chrono::seconds(2), [&] { return active_entered; }); }
        t.check(reached_checker && active.configure({preset, enabled}, true) && !active_parent.cancelled(),
            "Adding an unselected preset during a check preserves ownership of the current probe");
        const auto activity = active.activity();
        t.check(activity && activity->mode == ProbeMode::complete && !activity->websites_deferred && activity->websites.size() == 1 &&
            activity->websites.front().id == enabled.id && activity->websites.front().url == enabled.url,
            "An in-flight complete activity exposes only the owned, enabled website snapshot after disabled-only edits");
        t.check(!active.configure({enabled, enabled}, false) && !active_parent.cancelled(),
            "Invalid configuration cannot change the active strategy or cancel the accepted probe");
        if (change == "enabled" || change == "strategy") {
            const std::vector<WebsiteTarget> changed_targets{enabled, {preset.id, preset.url, true}};
            const bool reconfigured = change == "enabled" ? active.configure(changed_targets, true) :
                active.configure({enabled, preset}, false);
            t.check(reconfigured && active_parent.cancelled() && !active.activity(),
                "Changing the selected targets or proxy strategy cancels the owned probe instead of mixing configurations");
        } else if (change == "websitePath") {
            active.invalidate_website_path();
            t.check(!active.activity() && !active_parent.cancelled(),
                "Website-path invalidation revokes display scope without cancelling the physical base probe");
        } else if (change == "cancel") {
            active_parent.cancel();
            t.check(!active.activity(), "A cancelled request cannot retain an active display scope while transport finishes");
        } else if (change == "invalidate") {
            active.invalidate();
            t.check(!active.activity() && active_parent.cancelled(), "Full invalidation revokes and cancels the active display scope");
        }
        { std::lock_guard lock(active_mutex); active_release = true; }
        active_cv.notify_all(); active_worker.join();
        const auto active_report = active.latest();
        t.check(change == "websitePath" ? active_report && active_report->websites_deferred && active_report->websites.empty() :
            change != "disabledOnly" ? !active_report : active_report && active_report->selected_count == 1 &&
            active_report->base.mode == ProbeMode::complete && active_report->follow_system_proxy &&
            active_report->websites.size() == 1 && active_report->websites.front().target.id == enabled.id &&
            active_report->websites.front().result.elapsed == 0.125,
            "Only the unchanged effective selection may publish its original completed report after a concurrent configuration edit");
        t.check(!active.activity(), "Every completed, deferred or cancelled round clears only its own active display scope");
    }

    t.begin("integrated-internet-active-scope");
    for (const bool deferred : {false, true}) {
        szutest::ScriptedService scope_base; scope_base.default_probe = "online";
        std::binary_semaphore entered_scope{0}, finish_scope{0};
        scope_base.during_probe = [&] { entered_scope.release(); finish_scope.acquire(); };
        std::atomic<int> scope_site_calls{0};
        InternetCheckService scope_service(scope_base, [&](const std::string&, bool, const CancelToken&) {
            ++scope_site_calls; return WebsiteCheckResult{"accessible", 200, 0.125};
        });
        scope_service.configure({enabled, preset}, true);
        t.check(!scope_service.activity() && scope_base.probe_calls == 0 && scope_site_calls == 0,
            "Reading an idle display scope never starts base or website requests");
        const ProbeMode actual_mode = deferred ? ProbeMode::complete : ProbeMode::quick;
        CancelToken scope_parent;
        std::thread scope_worker([&] { scope_service.probe_with_scope(actual_mode, scope_parent, false, deferred); });
        const bool entered = entered_scope.try_acquire_for(std::chrono::seconds(2));
        const auto scope = scope_service.activity();
        t.check(entered && scope && scope->mode == actual_mode && scope->websites_deferred == deferred &&
            scope->websites.size() == (deferred ? 1u : 0u) && scope_site_calls == 0,
            "Actual quick scope has no website targets; explicitly deferred complete scope retains targets but never claims they are running");
        finish_scope.release(); scope_worker.join();
        const auto scope_report = scope_service.latest();
        t.check(!scope_service.activity() && scope_report && scope_report->base.mode == actual_mode && scope_site_calls == 0 &&
            scope_report->websites_deferred == deferred,
            "Finishing a quick or explicitly deferred complete request publishes only its actual report and clears its display activity");
    }
    {
        szutest::ScriptedService overlap_base; overlap_base.default_probe = "online";
        std::counting_semaphore<2> base_entered{0};
        std::binary_semaphore old_entered{0}, new_entered{0}, release_old{0}, release_new{0};
        overlap_base.during_probe = [&] { base_entered.release(); };
        std::atomic<int> overlap_calls{0};
        InternetCheckService overlap(overlap_base, [&](const std::string&, bool, const CancelToken&) {
            const bool old = ++overlap_calls == 1;
            (old ? old_entered : new_entered).release(); (old ? release_old : release_new).acquire();
            return WebsiteCheckResult{"accessible", 200, old ? 0.111 : 0.222};
        });
        overlap.configure({enabled}, true);
        CancelToken old_parent, new_parent;
        std::thread old_worker([&] { overlap.probe(ProbeMode::complete, old_parent); });
        const bool old_ready = old_entered.try_acquire_for(std::chrono::seconds(2)) && base_entered.try_acquire_for(std::chrono::seconds(2));
        t.check(old_ready, "The old activity fixture enters its actual transport before testing a replacement generation");
        if (!old_ready) { release_old.release(); old_worker.join(); }
        else {
            overlap.invalidate();
            std::thread new_worker([&] { overlap.probe(ProbeMode::complete, new_parent); });
            const bool new_ready = new_entered.try_acquire_for(std::chrono::seconds(2)) && base_entered.try_acquire_for(std::chrono::seconds(2));
            release_old.release(); old_worker.join();
            const auto new_activity = overlap.activity();
            t.check(new_ready && old_parent.cancelled() && !new_parent.cancelled() && new_activity &&
                new_activity->mode == ProbeMode::complete && new_activity->websites.size() == 1 && !overlap.latest(),
                "A revoked old request finishing late cannot clear a newer blocked request's display scope or publish old evidence");
            release_new.release(); new_worker.join();
            const auto newest = overlap.latest();
            t.check(!overlap.activity() && newest && newest->websites.size() == 1 && newest->websites.front().result.elapsed == 0.222,
                "Only the newest generation may publish and retire the matching active scope");
        }
    }

    t.begin("integrated-internet-engine-safety");
    raw.discovery = {{{"teaching", "confirmed"}, {"dormitory", "confirmed"}}};
    raw.attempts["teaching"] = "unavailable";
    double now = 1000;
    std::vector<std::string> events;
    records::HealthLog health;
    ConnectionEngine::Hooks hooks;
    hooks.now = [&] { return now; };
    hooks.event = [&](const std::string& text) { events.push_back(text); };
    hooks.report = [&](const ConnectivityReport& value) { health.append(records::sample_from(value, now)); };
    ConnectionEngine engine(service, hooks);
    engine.configure(MonitoringMode::recover, CampusArea::teaching, ProbeMode::complete, Credentials{"fixture-account", "fixture-password"});
    PathSnapshot path; path.usable = true; path.identity = "fixture-network";
    engine.receive_path(path); engine.trust_current_network();
    engine.automatic_round(); engine.automatic_round(); engine.check(); engine.connect();
    t.check(raw.submissions.empty() && raw.discoveries == 0 && raw.attempt_log.empty(),
        "Passing base probes with persistent website failure never run campus discovery or credential preflight");
    t.check(engine.snapshot().trusted && !engine.paused() && engine.snapshot().online,
        "Website-only failure keeps base online and existing trust without a login round");
    records::EventLog log; for (const auto& event : events) log.append(now, event, false);
    records::ExportContext ctx{"0.7.0", "16", "windows", "recover", "teaching", "complete", now};
    const auto exported = json::serialize(records::diagnostics_export(ctx, health, log));
    t.check(exported.find("alpha.example") == std::string::npos && exported.find("beta.example") == std::string::npos,
        "Integrated site addresses never enter persisted health/events or diagnostics export");
    raw.default_probe = "offline";
    raw.attempts["teaching"] = "rejected:password";
    engine.connect();
    t.check(raw.submissions.size() == 1 && engine.snapshot().pause_reason == HoldReason::credentials, "Real explicit password refusal still creates a credential hold");
    raw.default_probe = "online"; engine.check(); engine.automatic_round();
    t.check(raw.submissions.size() == 1 && engine.paused() && engine.snapshot().pause_reason == HoldReason::credentials,
        "Later internet/site results cannot clear a password hold or submit again");
    t.check(engine.snapshot().cooldown_remaining.value_or(0) > 89, "Original 90-second post-submission cooldown remains intact");
    engine.resume_auto_login(); engine.pause_auto_login(); engine.check();
    t.check(engine.snapshot().pause_reason == HoldReason::userPaused, "Selected-site outcome preserves explicit user pause");
}

void website_preferences_tests(Harness& t) {
    t.begin("website-preferences-validation");
    const WebsiteTarget a{"a", "https://example.com/", true};
    for (const std::string& id : {std::string(""), std::string(" \t "), std::string("\xE3\x80\x80"), std::string("\xE2\x80\x83"),
        std::string("row\n"), std::string("row\x7F"), std::string("row\xC2\x85"), std::string("row\xE2\x80\x8B"), std::string(129, 'x'), std::string("\xFF")})
        t.check(!validated_website_targets({{id, a.url, true}}), "Invalid blank/control/oversize/non-UTF8 identity rejected");
    t.check(validated_website_targets({{std::string(128, 'x'), a.url, true}}).has_value(), "128-byte ID accepted");
    t.check(validated_website_targets({{"网站", a.url, true}}).has_value(), "Ordinary Unicode identity accepted");
    for (const char* url : {"https://example.com", "https://example.com:443", "https://example.com:443/", "HTTPS://EXAMPLE.COM:0443/"}) {
        const auto clean = validated_website_targets({{"b", url, true}});
        t.check(clean && clean->front().url == a.url, "Equivalent HTTPS roots normalize to one address");
        t.check(!validated_website_targets({a, {"b", url, false}}), "Equivalent disabled/root/default-port duplicate rejected");
    }
    const auto plain = validated_website_targets({{"a", "http://example.com:80", true}});
    t.check(plain && plain->front().url == "http://example.com/", "HTTP default port is removed and root added");
    const auto other = validated_website_targets({{"a", "https://example.com:8443/path/", true}});
    t.check(other && other->front().url == "https://example.com:8443/path/", "Nondefault port and nonempty path preserved");
    t.check(!validated_website_targets({{"a", std::string(2048, 'a'), true}}), "Scheme/root expansion beyond 2048 bytes rejected");
    const auto bounded = validated_website_targets({{"a", std::string("https://example.com/") + std::string(2028, 'a'), true}});
    t.check(bounded && bounded->front().url.size() == 2048, "Exactly 2048 normalized bytes accepted");

    t.begin("website-preferences-codec");
    WebsitePreferences config{{a, {"b", "http://second.example:80", false}}, false};
    const auto encoded = config.encode();
    const auto decoded = encoded ? WebsitePreferences::decode(*encoded) : std::nullopt;
    t.check(decoded && decoded->targets.size() == 2 && !decoded->targets[1].enabled && !decoded->follow_system_proxy,
        "Roundtrip preserves disabled rows and explicit direct strategy");
    t.check(decoded && decoded->targets[1].url == "http://second.example/", "Durable bytes use normalized URLs");
    const auto legacy = WebsitePreferences::decode(R"({"schemaVersion":1,"targets":[]})");
    t.check(legacy && legacy->follow_system_proxy, "Missing optional strategy defaults to system like Swift");
    t.check(WebsitePreferences::decode(R"({"schemaVersion":1,"targets":[],"strategy":null})").has_value(), "Null optional strategy mirrors Swift decodeIfPresent");
    for (const char* text : {"{}", "[]", "{broken", R"({"schemaVersion":2,"targets":[]})", R"({"schemaVersion":1,"targets":[],"strategy":"proxy"})",
        R"({"schemaVersion":1,"targets":[],"strategy":3})", R"({"schemaVersion":1,"targets":{}})",
        R"({"schemaVersion":1,"targets":[{"id":"a","url":"https://example.com"}]})",
        R"({"schemaVersion":1,"targets":[{"id":"a","url":"https://example.com","enabled":"true"}]})"})
        t.check(!WebsitePreferences::decode(text), "Malformed schema/config rejected");
    t.check(!WebsitePreferences::decode(std::string(32769, ' ')), "32 KiB read bound enforced");

    std::vector<WebsiteTarget> expanded;
    for (int i = 1; i <= 12; ++i) expanded.push_back({"expanded-" + std::to_string(i), "https://expanded-" + std::to_string(i) + ".invalid/", true});
    const auto expanded_bytes = WebsitePreferences{expanded, true}.encode();
    const auto expanded_copy = expanded_bytes ? WebsitePreferences::decode(*expanded_bytes) : std::nullopt;
    t.check(expanded_copy && expanded_copy->targets.size() == 12 && expanded_copy->targets.back().id == "expanded-12",
        "Schema 1 round-trips all twelve selections with stable identity and order");
    const auto old_three = WebsitePreferences::decode(R"({"schemaVersion":1,"targets":[{"id":"a","url":"https://one.invalid/","enabled":true},{"id":"b","url":"https://two.invalid/","enabled":false},{"id":"c","url":"https://three.invalid/","enabled":true}]})");
    t.check(old_three && old_three->targets.size() == 3 && !old_three->targets[1].enabled && old_three->follow_system_proxy,
        "Legacy three-row schema 1 data preserves identities, disabled choices and default strategy");
    expanded.push_back({"thirteen", "https://thirteen.invalid/", true});
    t.check(!WebsitePreferences{expanded, true}.encode(), "A thirteenth row cannot be persisted");

    t.begin("website-preferences-transaction");
    WebsitePreferencesState state;
    int writes = 0;
    t.check(state.load(WebsitePreferencesState::ReadStatus::missing) && state.current().targets.empty() && state.current().follow_system_proxy,
        "Missing file creates empty/system defaults without writes");
    t.check(!state.load(WebsitePreferencesState::ReadStatus::found, "corrupt-original") && state.needs_preservation(),
        "Malformed read marks preservation; load does not call storage or move original");
    bool backup_ok = false, replace_ok = true;
    std::string disk = "corrupt-original", backup;
    WebsitePreferencesState::Writer writer = [&](const std::string& data, bool preserve) {
        ++writes;
        t.check(state.current().targets.empty() && state.current().follow_system_proxy, "Runtime configuration is old while storage is committing");
        if (preserve) {
            if (!backup_ok) return false;
            backup = disk;
        }
        if (!replace_ok) return false;
        disk = data;
        return true;
    };
    t.check(!state.save(config, writer) && disk == "corrupt-original" && backup.empty() && state.needs_preservation(),
        "Backup failure aborts before replacement and keeps runtime/corrupt flag");
    backup_ok = true; replace_ok = false;
    t.check(!state.save(config, writer) && disk == "corrupt-original" && backup == "corrupt-original" && state.needs_preservation(),
        "Atomic replace failure retains original, backup and runtime for safe retry");
    replace_ok = true;
    t.check(state.save(config, writer) && disk == *encoded && backup == "corrupt-original" && !state.needs_preservation(),
        "Explicit save commits canonical bytes only after preservation succeeds");
    t.check(state.current().targets.size() == 2 && !state.current().follow_system_proxy, "Successful durable write commits runtime once");
    const auto good_disk = disk;
    const auto no_write = writes;
    t.check(!state.save(WebsitePreferences{{a, a}, true}, writer) && writes == no_write && disk == good_disk,
        "Invalid new configuration never reaches writer or changes old state");
    bool received_preserve = true;
    t.check(!state.save(WebsitePreferences{{a}, true}, [&](const std::string&, bool preserve) { received_preserve = preserve; return false; }) &&
        !received_preserve && !state.current().follow_system_proxy && state.current().targets.size() == 2,
        "Normal write failure preserves previous runtime configuration and strategy");
    WebsitePreferencesState reloaded;
    t.check(reloaded.load(WebsitePreferencesState::ReadStatus::found, disk) && !reloaded.current().follow_system_proxy && reloaded.current().targets.size() == 2,
        "Process reconstruction restores choices without an implicit check or mode change");
    t.check(!reloaded.load(WebsitePreferencesState::ReadStatus::failed) && reloaded.needs_preservation() && reloaded.current().targets.size() == 2,
        "Unreadable reload preserves already active configuration and requires explicit repair");
}

void copy_tests(Harness& t) {
    t.begin("copy");
    t.check(copy::size() > 400, "copy table populated");
    t.equal(std::string(copy::text("app.store")), std::string("Windows 凭据管理器"), "windows variant selected");
    t.equal(copy::format("tutorial.progress", {{"current", "2"}, {"total", "4"}}), std::string("第 2 步，共 4 步"), "format placeholders");
    t.equal(std::string(copy::text("no.such.key")), std::string("no.such.key"), "unknown key visible");
    for (const char* key : {"action.check", "action.login", "action.partialLogin", "action.cancel", "status.online.quick", "event.failed", "hold.scope.network",
                            "compat.rules.copied", "about.creditsBody", "tutorial.welcome.title", "tray.quit"})
        t.check(copy::contains(key), std::string("copy has ") + key);
}

#include "memory_tests.hpp"
#include "netlabels_tests.hpp"
#include "path_recovery_tests.hpp"
#include "partial_recovery_tests.hpp"
#include "physical_tests.hpp"
#include "address_resolver_tests.hpp"
#include "tun_parity_tests.hpp"
#include "wired_session_recovery_tests.hpp"
#include "r11_parser_policy_tests.hpp"
#include "config_boundary_tests.hpp"
#include "r11_engine_sync_tests.hpp"
#include "automatic_campus_verification_tests.hpp"
#include "tun_long_running_recovery_tests.hpp"
#include "verification_hint_tests.hpp"
#include "teaching_page_tests.hpp"
#include "r11_presentation_tests.hpp"
#include "wifi_recovery_tests.hpp"
#include "proxy_assistant_tests.hpp"
#include "history_detail_tests.hpp"
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) szutest::root_dir = argv[1];
    Harness t;
    launch_options_tests(t);
    protocol_golden(t);
    rejection_cases(t);
    website_cases(t);
    route_cases(t);
    proxy_cases(t);
    compat_cases(t);
    test_proxy_assistant(t);
    status_cases(t);
    auto_recovery_scenarios(t);
    json_tests(t);
    wire_tests(t);
    policy_tests(t);
    engine_tests(t);
    records_tests(t);
    history_detail_tests(t);
    copy_tests(t);
    recovery_journal_tests(t);
    internet_check_tests(t);
    website_preferences_tests(t);
    memory_tests(t);
    netlabels_tests(t);
    path_recovery_tests(t);
    physical_tests(t);
    address_resolver_tests(t);
    tun_parity_tests(t);
    wired_session_recovery_tests(t);
    r11_parser_policy_tests(t);
    config_boundary_tests(t);
    r11_engine_sync_tests(t);
    automatic_campus_verification_tests(t);
    tun_long_running_recovery_tests(t);
    independent_verification_hint_tests(t);
    test_teaching_page_reuse(t);
    r11_presentation_tests(t);
    run_wifi_recovery_tests(t);
    partial_recovery_tests(t);
    partial_recovery_guard_tests(t);
    partial_recovery_integration_tests(t);
    website_recovery_signal_tests(t);
    website_recovery_engine_tests(t);
    website_recovery_boundary_tests(t);
    std::printf("SZUConnect core tests: %d checks, %d failures\n", t.checks, t.failures);
    return t.failures == 0 ? 0 : 1;
}
