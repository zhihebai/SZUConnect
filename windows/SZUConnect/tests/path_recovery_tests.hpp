#include <optional>
#include <string>
#include <utility>

// Network-path regression cases use only scripted services and fixture identities.
// Included after memory_tests.hpp, inside core_tests.cpp's anonymous namespace.
inline PathSnapshot recovery_path(std::optional<std::string> identity = "wired") {
    PathSnapshot p;
    p.usable = true;
    p.identity = std::move(identity);
    p.interfaces = "Ethernet";
    p.descriptors = {mem_desc("a4:5e:60:11:22:33")};
    return p;
}

inline void path_recovery_tests(Harness& t) {
    t.begin("path-recovery");
    // Repeated observations of an unknown but usable path must allow one round to finish.
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        auto p = recovery_path(std::nullopt);
        f.engine->receive_path(p);
        const auto epoch = f.engine->snapshot().epoch;
        f.service.during_probe = [&] { f.engine->receive_path(p); };
        f.service.during_discovery = [&] { f.engine->receive_path(p); };
        f.service.attempts["teaching"] = "success";
        f.service.probes = {"offline", "offline", "online"};
        f.engine->automatic_round(); f.engine->automatic_round();
        t.check(f.engine->snapshot().epoch == epoch, "identical unknown observations keep the current epoch");
        t.check(f.engine->snapshot().online && f.service.submissions.size() == 1,
                "identical unknown observations cannot cancel discovery or login");
        p.usable = false; p.link_active = false; p.descriptors.clear();
        f.engine->receive_path(p);
        const auto down_epoch = f.engine->snapshot().epoch;
        f.engine->receive_path(p); f.engine->receive_path(p);
        t.check(f.engine->snapshot().epoch == down_epoch, "repeated disconnected observations do not advance the epoch");
    }
    // Metered status affects cadence, but is not an identity/trust boundary.
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        auto p = recovery_path(); f.engine->receive_path(p); f.engine->trust_current_network();
        const auto epoch = f.engine->snapshot().epoch;
        p.constrained = true; p.interfaces = "Renamed Ethernet"; f.engine->receive_path(p);
        t.check(f.engine->snapshot().epoch == epoch && f.engine->snapshot().trusted,
                "metered and display-label metadata preserve a known identity and trust");
    }
    // Each material change cancels all old work, including a late successful transport reply.
    for (const std::string change : {"descriptor", "tunnel", "interfaces", "link", "availability", "identity"}) {
        for (const std::string phase : {"probe", "discovery", "attempt"}) {
            EngineFixture f(MonitoringMode::recover, CampusArea::automatic, true);
            auto p = recovery_path(change == "interfaces" ? std::nullopt : std::optional<std::string>("wired"));
            f.engine->receive_path(p); f.engine->trust_current_network();
            const auto epoch = f.engine->snapshot().epoch;
            f.service.discovery = {{{"teaching", "confirmed"}, {"dormitory", "confirmed"}}};
            f.service.attempts["dormitory"] = "success";
            auto mutate = [&] {
                if (change == "descriptor") p.descriptors = {mem_desc("b8:27:eb:44:55:66", "10.0.0.1", "10.0.0.0/24")};
                else if (change == "tunnel") p.tunnel = true;
                else if (change == "interfaces") p.interfaces = "Wi-Fi";
                else if (change == "link") p.link_active = false;
                else if (change == "availability") p.usable = false;
                else p.identity = "wifi";
                f.engine->receive_path(p);
            };
            if (phase == "probe") f.service.during_probe = mutate;
            else if (phase == "discovery") f.service.during_discovery = mutate;
            else f.service.during_attempt = mutate;
            f.engine->connect();
            const auto state = f.engine->snapshot();
            t.check(state.epoch == epoch + 1 && !state.busy && !state.trusted,
                    change + " during " + phase + " cancels the old epoch and trust");
            t.check(!state.online && !state.last_attempt && !state.last_success_area,
                    change + " during " + phase + " rejects late success");
            if (phase != "attempt")
                t.check(f.service.submissions.empty(), change + " before login never submits");
            else
                t.check(state.cooldown_remaining.value_or(0) == 90,
                        change + " after transport starts retains duplicate-submit protection");
        }
    }
    // A remembered Wi-Fi path can recover after a wired disconnect without restarting the app.
    {
        memtest::Rig f;
        auto wifi = recovery_path("remembered-wifi");
        wifi.interfaces = "Wi-Fi"; wifi.descriptors.front().kind = "wifi";
        f.e->receive_path(wifi); f.discovery("confirmed", "confirmed");
        t.equal(f.e->trust_and_remember(), ConnectionEngine::RememberResult::remembered, "fixture Wi-Fi is durably remembered");
        auto unknown = wifi; unknown.identity.reset();
        f.e->receive_path(unknown);
        t.check(!f.e->snapshot().remembered && !f.e->snapshot().trusted,
                "unresolved identity cannot reuse even matching descriptors for memory trust");
        unknown.identity = ""; f.e->receive_path(unknown);
        t.check(!f.e->snapshot().remembered && !f.e->snapshot().can_trust,
                "empty identity is also unresolved and cannot be trusted");
        auto wired = recovery_path("wired");
        wired.descriptors = {mem_desc("b8:27:eb:44:55:66", "10.0.0.1", "10.0.0.0/24")};
        f.e->receive_path(wired); f.e->trust_current_network();
        f.discovery("unconfirmed", "unconfirmed"); f.rounds(3);
        t.check(f.e->next_check_delay() >= 60, "old wired failures created backoff");
        auto down = wired; down.usable = false; down.link_active = false; down.descriptors.clear();
        f.e->receive_path(down); f.e->receive_path(wifi);
        t.check(f.e->snapshot().remembered && !f.e->snapshot().trusted && f.e->next_check_delay() == 30,
                "remembered Wi-Fi starts with fresh evidence and no wired session trust");
        f.discovery("unconfirmed", "confirmed"); f.svc.attempts["dormitory"] = "success";
        f.svc.probes = {"offline", "offline", "online"};
        f.e->automatic_round();
        t.check(f.e->snapshot().status == EngineStatus::confirming && f.svc.submissions.empty(),
                "first Wi-Fi failure does not reuse the wired failure streak");
        f.e->automatic_round();
        t.check(f.e->snapshot().online && f.svc.submissions.size() == 1,
                "second Wi-Fi failure rechecks the remembered portal and logs in once");
    }
    {
        memtest::Rig f;
        auto wired = recovery_path(); f.e->receive_path(wired); f.e->trust_current_network();
        auto wifi = wired; wifi.identity = "unknown-wifi"; wifi.interfaces = "Wi-Fi";
        wifi.descriptors = {mem_desc("b8:27:eb:44:55:66", "192.168.1.1", "192.168.1.0/24")};
        wifi.descriptors.front().kind = "wifi"; f.e->receive_path(wifi);
        f.discovery("confirmed", "confirmed"); f.rounds(2);
        t.check(!f.e->snapshot().trusted && !f.e->snapshot().remembered && f.svc.submissions.empty() &&
                f.e->snapshot().status == EngineStatus::needsTrust,
                "unknown Wi-Fi cannot inherit wired trust or submit automatically");
    }
    // Network-scoped holds expire on a descriptor-only change; durable protections do not.
    {
        EngineFixture f(MonitoringMode::recover, CampusArea::automatic, true);
        auto p = recovery_path(); f.engine->receive_path(p);
        f.service.attempts["teaching"] = "rejected:areaMismatch"; f.engine->connect();
        t.check(f.engine->snapshot().holds.size() == 1 && f.engine->snapshot().holds[0].epoch.has_value(),
                "fixture has a network-scoped candidate rejection");
        p.descriptors.front().raw["net"] = "172.30.16.0/20"; f.engine->receive_path(p);
        t.check(f.engine->snapshot().holds.empty() && f.engine->snapshot().cooldown_remaining.value_or(0) == 90,
                "new path releases its old temporary hold but preserves actual submission cooldown");
    }
    for (const auto reason : {HoldReason::userPaused, HoldReason::security, HoldReason::uncertain, HoldReason::credentials}) {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        const std::string journal = "{\"schema\":1,\"generation\":1,\"holds\":[{\"reason\":" +
            std::to_string(static_cast<int>(reason)) + ",\"origin\":2}],\"notBefore\":1090}";
        // The in-memory fixture's wall clock is monotonic too unless overridden.
        t.check(f.engine->restore_recovery(journal), "durable protection fixture restores");
        auto p = recovery_path("new-path"); f.engine->receive_path(p);
        t.check(f.engine->snapshot().holds.size() == 1 && f.engine->snapshot().holds.front().reason == reason,
                "path change preserves user, security, uncertain and credential protection");
    }
    t.begin("preflight-cooldown");
    for (const std::string outcome : {"unavailable", "challengeRefused"}) {
        szutest::ScriptedService service;
        double clock = 1000, wall = 1750000000;
        std::string journal;
        ConnectionEngine::Hooks hooks;
        hooks.now = [&] { return clock; }; hooks.wall_now = [&] { return wall; };
        hooks.persist_recovery = [&](const std::string& data) { journal = data; return true; };
        ConnectionEngine e(service, hooks);
        e.configure(MonitoringMode::recover, CampusArea::teaching, ProbeMode::quick, Credentials{"fixture", "pw"});
        e.restore_recovery(std::nullopt); e.receive_path(recovery_path());
        service.attempts["teaching"] = outcome; e.connect();
        t.check(service.submissions.empty() && !e.snapshot().cooldown_remaining,
                outcome + " before submission does not impose a new 90-second cooldown");
        auto stored = json::parse(journal);
        t.check(stored && !stored->find("notBefore") && stored->find("holds")->items().empty(),
                outcome + " clears only its unsubmitted durable reservation");
        service.discovery = {{{"teaching", "confirmed"}, {"dormitory", "notChecked"}}};
        service.attempts["teaching"] = "success"; service.probes = {"offline", "offline", "online"};
        e.automatic_round(); e.automatic_round();
        t.check(service.submissions.size() == 1 && e.snapshot().online,
                outcome + " can recover on the next confirmed rounds without restart");
        clock += 10; wall += 10;
        const auto original_deadline = json::parse(journal)->number_at("notBefore");
        service.attempts["teaching"] = outcome;
        service.during_attempt = [&] { clock += 5; wall += 5; };
        e.connect();
        t.check(e.snapshot().cooldown_remaining.value_or(0) == 75 &&
                json::parse(journal)->number_at("notBefore") == original_deadline,
                outcome + " restores the exact earlier submission deadline after elapsed preflight");
    }
    for (const std::string outcome : {"uncertain", "cancelled", "security", "rejected:password"}) {
        EngineFixture f(MonitoringMode::recover, CampusArea::teaching, true);
        f.service.attempts["teaching"] = outcome; f.engine->connect();
        t.check(f.engine->snapshot().cooldown_remaining.value_or(0) == 90,
                outcome + " cannot use the definitely-unsubmitted cooldown release");
    }
}
