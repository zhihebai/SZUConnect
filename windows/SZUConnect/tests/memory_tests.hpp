// Tests: crypto primitives, network-memory matching/verdicts/plan (shared fixtures), persistence and
// corruption handling, engine restore / revoke / tunnel / fallback scenarios, readiness, history range.
// Included by core_tests.cpp inside its anonymous namespace.
#include <cmath>
#include <cstdio>
#include <memory>

#include "szu/netmemory.hpp"

using szutest::Harness;

inline szu::NetworkDescriptor mem_descriptor(const szu::json::Value* v) {
    szu::NetworkDescriptor d;
    d.kind = "wifi";
    for (const char* k : {"gw", "gwip", "net", "dhcp", "dns", "sfx"})
        if (auto s = v->string_at(k)) d.raw[k] = *s;
    return d;
}
inline szu::NetworkDescriptor mem_desc(const char* gw, const char* gwip = "172.30.0.1", const char* net = "172.30.0.0/20") {
    szu::NetworkDescriptor d;
    d.kind = "ethernet";
    if (gw && *gw) d.raw["gw"] = gw;
    if (gwip && *gwip) d.raw["gwip"] = gwip;
    if (net && *net) d.raw["net"] = net;
    return d;
}
inline std::string mem_id(unsigned i) { char b[40]; std::snprintf(b, sizeof b, "%032x", 0x5000u + i * 31u); return b; }

inline void crypto_tests(Harness& t) {
    t.begin("crypto");
    t.equal(szu::sha256_hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256 empty");
    t.equal(szu::sha256_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256 abc");
    t.equal(szu::sha256_hex(std::string(1000, 'a')), std::string("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"), "sha256 multi-block");
    t.equal(szu::hmac_sha256_hex("Jefe", "what do ya want for nothing?"), std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "hmac rfc4231 #2");
    t.equal(szu::hmac_sha256_hex(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First"),
            std::string("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"), "hmac rfc4231 #6 (long key)");
}

inline void netmemory_fixture_tests(Harness& t) {
    t.begin("netmemory-fixtures");
    const auto doc = szutest::load("shared/fixtures/network-memory-cases.json");
    const std::string salt = szutest::str(doc, "salt");
    szu::NetworkMemory mem(salt);
    for (const auto& c : doc.find("match")->items()) {
        const auto level = szu::NetworkMemory::compare(mem.hashed(mem_descriptor(c.find("stored"))), mem.hashed(mem_descriptor(c.find("current"))));
        t.equal(std::string(szu::to_string(level)), szutest::str(c, "expected"), "match " + szutest::str(c, "id"));
    }
    for (const auto& c : doc.find("verdict")->items()) {
        szu::CampusDiscovery d;
        d.teaching = *szu::discovery_from(szutest::str(c, "teaching"));
        d.dormitory = *szu::discovery_from(szutest::str(c, "dormitory"));
        const auto verdict = szu::memory_verdict(*szu::match_level_from(szutest::str(c, "level")), d, c.bool_at("ambiguous").value_or(false));
        t.equal(std::string(szu::to_string(verdict)), szutest::str(c, "expected"), "verdict " + szutest::str(c, "id"));
    }
    for (const auto& c : doc.find("plan")->items()) {
        szu::ConnectionPolicy policy;
        for (const auto& area : c.find("blocked")->items())
            policy.add(szu::Hold{szu::HoldReason::credentials, *szu::area_from(area.as_string()), 1, std::nullopt, szu::HoldOrigin::explicitArea, "password"});
        const auto* disc = c.find("discovery");
        szu::CampusDiscovery d;
        d.teaching = *szu::discovery_from(szutest::str(*disc, "teaching"));
        d.dormitory = *szu::discovery_from(szutest::str(*disc, "dormitory"));
        const auto kind = szutest::str(c, "kind") == "manual" ? szu::OperationKind::manual : szu::OperationKind::automatic;
        const auto result = szu::plan(kind, *szu::area_from(szutest::str(c, "choice")), d, c.bool_at("trusted").value_or(false), policy, 1, 1,
                                      *szu::match_level_from(szutest::str(c, "memory")), c.bool_at("ambiguous").value_or(false));
        const auto* expected = c.find("expected");
        const std::string id = "plan " + szutest::str(c, "id");
        t.equal(std::string(szu::to_string(result.decision)), szutest::str(*expected, "decision"), id + " decision");
        if (const auto target = expected->string_at("target")) t.equal(std::string(szu::to_string(result.target)), *target, id + " target");
    }
    for (const auto& c : doc.find("hash")->items()) {
        szu::NetworkDescriptor d;
        d.raw[szutest::str(c, "label")] = szutest::str(c, "raw");
        t.equal(mem.hashed(d)[szutest::str(c, "label")], szutest::str(c, "expected"), "known-answer hash " + szutest::str(c, "label"));
    }
    const auto readiness = szutest::load("shared/fixtures/readiness-cases.json");
    for (const auto& c : readiness.find("cases")->items()) {
        const auto* in = c.find("input");
        szu::status::ReadinessInput input;
        input.mode = szutest::str(*in, "mode");
        input.read_only = in->bool_at("readOnly").value_or(false);
        input.has_saved = in->bool_at("hasSaved").value_or(false);
        input.unsaved_changes = in->bool_at("unsavedChanges").value_or(false);
        input.auto_login = szutest::str(*in, "autoLogin");
        if (const auto hold = in->string_at("hold")) input.hold = *hold;
        input.session_trusted = in->bool_at("sessionTrusted").value_or(false);
        input.remembered = in->bool_at("remembered").value_or(false);
        input.memory_conflict = in->bool_at("memoryConflict").value_or(false);
        input.network_identified = in->bool_at("networkIdentified").value_or(true);
        input.session_only_network = in->bool_at("sessionOnlyNetwork").value_or(false);
        input.campus_verification = in->string_at("campusVerification").value_or("notChecked");
        const auto got = szu::status::readiness(input);
        const auto* want = c.find("expected");
        const std::string id = "readiness " + szutest::str(c, "id");
        t.equal(got.title, szutest::str(*want, "title"), id + " title");
        t.equal(got.tone, szutest::str(*want, "tone"), id + " tone");
        const auto* items = want->find("items");
        t.equal(got.items.size(), items->size(), id + " item count");
        std::size_t index = 0;
        for (const auto& item : items->items()) {
            if (index >= got.items.size()) break;
            t.equal(got.items[index].key, szutest::str(item, "key"), id + " item key");
            t.equal(got.items[index].tone, szutest::str(item, "tone"), id + " item tone");
            ++index;
        }
    }
}

inline void netmemory_store_tests(Harness& t) {
    t.begin("netmemory-store");
    const double now = 1750000000.0;
    szu::NetworkMemory mem("00112233445566778899aabbccddeeff");
    const auto dorm = mem_desc("a4:5e:60:11:22:33");
    t.check(!mem.remember(mem_descriptor(szutest::load("shared/fixtures/network-memory-cases.json").find("match")->items().front().find("stored")), "", false, false, now, "zz").has_value(), "invalid id refused");
    szu::NetworkDescriptor weak;
    weak.raw["dns"] = "1.1.1.1";
    t.check(!mem.remember(weak, "dormitory", true, true, now, mem_id(1)).has_value(), "descriptor without gateway evidence is not remembered");
    const auto id = mem.remember(dorm, "dormitory", true, true, now, mem_id(1));
    t.check(id.has_value() && mem.size() == 1, "reliable descriptor remembered");
    t.equal(std::string(szu::to_string(mem.match(dorm, now).level)), std::string("strong"), "same network matches strongly");
    const auto again = mem.remember(mem_desc("a4:5e:60:11:22:33", "172.30.16.1", "172.30.16.0/20"), "dormitory", true, true, now + 10, mem_id(2));
    t.check(again && *again == *id && mem.size() == 1, "same gateway hardware refreshes the record instead of duplicating");
    t.equal(std::string(szu::to_string(mem.match(mem_desc("a4:5e:60:11:22:33", "172.30.16.1", "172.30.16.0/20"), now + 10).level)), std::string("strong"), "renumbered network keeps matching");
    t.equal(std::string(szu::to_string(mem.match(mem_desc("b8:27:eb:44:55:66", "172.30.16.1", "172.30.16.0/20"), now).level)), std::string("conflict"), "same address, other gateway: conflict");
    t.equal(std::string(szu::to_string(mem.match(mem_desc("b8:27:eb:44:55:66", "192.168.1.1", "192.168.1.0/24"), now).level)), std::string("none"), "other gateway elsewhere: just another network");
    // privacy: no raw value, SSID or client address in the stored text
    const std::string text = mem.encode();
    for (const char* raw : {"a4:5e:60", "172.30", "Wi-Fi", "ssid"}) t.check(text.find(raw) == std::string::npos, std::string("encoded memory hides ") + raw);
    const auto decoded = szu::NetworkMemory::decode(text, now);
    t.check(decoded && decoded->size() == 1 && decoded->encode() == text, "encode/decode round trip is byte-stable");
    t.equal(std::string(szu::to_string(decoded->match(mem_desc("a4:5e:60:11:22:33", "172.30.16.1", "172.30.16.0/20"), now).level)), std::string("strong"), "decoded record still matches");
    // suppression and expiry
    std::set<std::string> hidden{*id};
    t.equal(std::string(szu::to_string(mem.match(dorm, now + 10, &hidden).level)), std::string("none"), "suppressed record is ignored");
    t.equal(std::string(szu::to_string(mem.match(dorm, now + szu::NetworkMemory::kExpirySeconds + 3600).level)), std::string("none"), "expired record is ignored");
    t.check(!szu::NetworkMemory::decode(text, now + szu::NetworkMemory::kExpirySeconds + 3600)->size(), "expired record pruned on load");
    // throttled touch
    t.check(!mem.touch(*id, now + 20, "dormitory"), "touch within an hour is throttled");
    t.check(mem.touch(*id, now + 7200, "dormitory"), "touch after an hour refreshes");
    szu::NetworkMemory overlapping("00112233445566778899aabbccddeeff");
    const auto teaching = overlapping.remember(mem_desc("a4:5e:60:11:22:33", "", "172.30.0.0/20"),
                                               "teaching", true, false, now, mem_id(3));
    const auto dormitory = overlapping.remember(mem_desc("", "172.30.0.1", "172.30.0.0/20"),
                                                "dormitory", true, true, now, mem_id(4));
    const auto complete = mem_desc("a4:5e:60:11:22:33", "172.30.0.1", "172.30.0.0/20");
    t.check(overlapping.size() == 2 && overlapping.match_any({complete}, now).record == teaching.value_or(""),
            "stronger teaching-only record is the best display match");
    const auto dorm_match = overlapping.match_any({complete}, now, nullptr, true);
    t.check(dorm_match.record == dormitory.value_or("") && dorm_match.level == szu::MatchLevel::medium,
            "dormitory authorization chooses a weaker eligible record after filtering proofs");
    // limit of eight, oldest dropped
    szu::NetworkMemory many("00112233445566778899aabbccddeeff");
    for (unsigned i = 0; i < 10; ++i) {
        char gw[24]; std::snprintf(gw, sizeof gw, "aa:00:00:00:00:%02x", i);
        char ip[24]; std::snprintf(ip, sizeof ip, "10.%u.0.1", i);
        many.remember(mem_desc(gw, ip, ""), "", true, true, now + i, mem_id(10 + i));
    }
    t.equal(many.size(), static_cast<std::size_t>(8), "at most eight networks are remembered");
    t.equal(std::string(szu::to_string(many.match(mem_desc("aa:00:00:00:00:00", "10.0.0.1", ""), now + 10).level)), std::string("none"), "oldest record was dropped");
    t.check(many.forget(mem_id(19)) && many.size() == 7 && !many.forget(mem_id(19)), "forget removes exactly one record");
    many.forget_all();
    t.equal(many.size(), static_cast<std::size_t>(0), "forget_all clears everything");
    // corruption and unknown schema are refused, never partially accepted
    const std::string ok_salt = "00112233445566778899aabbccddeeff";
    const std::string good = "{\"schema\":1,\"salt\":\"" + ok_salt + "\",\"records\":[]}";
    t.check(szu::NetworkMemory::decode(good, now).has_value(), "empty valid file loads");
    for (const std::string& bad : {std::string("not json"), std::string("{\"schema\":2,\"salt\":\"" + ok_salt + "\",\"records\":[]}"),
                                   std::string("{\"schema\":1,\"salt\":\"abc\",\"records\":[]}"), std::string("{\"schema\":1,\"records\":[]}"),
                                   std::string("{\"schema\":1,\"salt\":\"" + ok_salt + "\",\"records\":[{\"id\":\"zz\"}]}"),
                                   std::string("{\"schema\":1,\"salt\":\"" + ok_salt + "\",\"records\":[{\"id\":\"" + mem_id(1) + "\",\"created\":1,\"verified\":1,\"proofTeaching\":true,\"proofDormitory\":true,\"features\":{\"gw\":\"short\"}}]}"),
                                   std::string("{\"schema\":1,\"salt\":\"" + ok_salt + "\",\"records\":[{\"id\":\"" + mem_id(1) + "\",\"created\":1,\"verified\":1,\"area\":\"moon\",\"proofTeaching\":true,\"proofDormitory\":true,\"features\":{}}]}")})
        t.check(!szu::NetworkMemory::decode(bad, now).has_value(), "corrupt memory refused: " + bad.substr(0, 40));
}

namespace memtest {
struct Rig {
    szutest::ScriptedService svc;
    double t = 1000, wall = 1750000000.0;
    std::string file;
    bool has_file = false, fail_write = false;
    int writes = 0;
    unsigned counter = 0;
    std::unique_ptr<szu::ConnectionEngine> e;
    szu::ConnectionEngine::Hooks hooks() {
        szu::ConnectionEngine::Hooks h;
        h.now = [this] { return t; };
        h.wall_now = [this] { return wall; };
        h.sleep = [](double) {};
        h.persist_memory = [this](const std::string& d) { if (fail_write) return false; file = d; has_file = true; ++writes; return true; };
        h.random_hex = [this] { char b[40]; std::snprintf(b, sizeof b, "%032x", 0x1000u + (++counter) * 977u); return std::string(b); };
        return h;
    }
    explicit Rig(bool restore = true, szu::CampusArea area = szu::CampusArea::automatic) {
        e = std::make_unique<szu::ConnectionEngine>(svc, hooks());
        e->configure(szu::MonitoringMode::recover, area, szu::ProbeMode::quick, szu::Credentials{"fixture-A", "pw-a"});
        if (restore) e->restore_memory(std::nullopt);
    }
    void path(const std::string& id, std::vector<szu::NetworkDescriptor> d, bool tunnel = false) {
        szu::PathSnapshot p;
        p.usable = true; p.identity = id; p.link_active = true; p.tunnel = tunnel; p.interfaces = "Wi-Fi"; p.descriptors = std::move(d);
        e->receive_path(p);
    }
    void discovery(const char* teaching, const char* dorm) { svc.discovery = {{{"teaching", teaching}, {"dormitory", dorm}}}; }
    void rounds(int n) { for (int i = 0; i < n; ++i) { e->automatic_round(); t += 1; } }
    // A first network visit: both services confirmed, the user chooses "trust and remember".
    szu::ConnectionEngine::RememberResult remember_dorm() {
        discovery("confirmed", "confirmed");
        path("id-1", {mem_desc("a4:5e:60:11:22:33")});
        rounds(2);
        return e->trust_and_remember();
    }
};
}  // namespace memtest

inline void memory_engine_tests(Harness& t) {
    using namespace szu;
    using memtest::Rig;
    t.begin("memory-engine");
    // ---- remember once, then restart / sleep / epoch change without another trust click ----
    Rig first;
    t.equal(first.e->snapshot().auto_login, AutoLoginState::ready, "before any dormitory evidence nothing needs trust");
    first.discovery("confirmed", "confirmed");
    first.path("id-1", {mem_desc("a4:5e:60:11:22:33")});
    first.rounds(2);
    t.equal(first.e->snapshot().auto_login, AutoLoginState::needsTrust, "unknown dormitory network needs the explicit choice");
    t.check(first.svc.submissions.empty(), "no submission before trust");
    t.check(first.e->snapshot().can_remember, "double proof + reliable descriptor: remembering is offered");
    t.equal(first.e->trust_and_remember(), ConnectionEngine::RememberResult::remembered, "trust and remember");
    t.check(first.has_file && first.file.find("a4:5e") == std::string::npos && first.file.find("172.30") == std::string::npos, "stored file has no raw network values");
    auto snap = first.e->snapshot();
    t.check(snap.trusted && snap.remembered && snap.memory_count == 1 && snap.auto_login == AutoLoginState::ready, "trusted + remembered + ready");

    {
        Rig online(false);
        online.e->restore_memory(first.file);
        online.path("id-1", {mem_desc("a4:5e:60:11:22:33")});
        online.svc.probes = {"online"}; online.e->check();
        auto state = online.e->snapshot();
        t.check(state.online && state.auto_login == AutoLoginState::ready, "online remembered network needs no user action");
        t.check(!state.trusted && !state.last_discovery && online.svc.submissions.empty(), "readiness does not grant trust or submit credentials");
        online.svc.probes = {"offline", "offline"}; online.discovery("unconfirmed", "notChecked"); online.rounds(2);
        t.check(!online.e->snapshot().online && online.e->snapshot().auto_login == AutoLoginState::awaitingProof && online.svc.submissions.empty(),
                "a later outage still requires fresh campus proof");
    }

    Rig restarted(false);
    t.check(restarted.e->restore_memory(first.file), "memory file restored after restart");
    restarted.svc.attempts["dormitory"] = "success";
    restarted.discovery("unconfirmed", "confirmed");  // teaching HTTPS unreachable; the dormitory portal answers
    restarted.path("id-restart", {mem_desc("a4:5e:60:11:22:33")});
    snap = restarted.e->snapshot();
    t.check(!snap.trusted && snap.remembered && snap.memory_level == MatchLevel::strong, "after restart: no session trust, strong memory");
    t.equal(snap.auto_login, AutoLoginState::awaitingProof, "before the fresh proof the state is 'awaiting proof', not 'needs trust'");
    restarted.rounds(2);
    t.equal(restarted.svc.submissions.size(), static_cast<std::size_t>(1), "remembered dormitory recovers with one submission");
    t.check(!restarted.svc.submissions.empty() && restarted.svc.submissions[0].first == "dormitory", "the submission targets the dormitory area");
    t.check(!restarted.e->snapshot().trusted, "recovery did not create session trust");
    restarted.t += 100;
    restarted.rounds(2);
    t.check(restarted.svc.submissions.size() <= 2, "cooldown still limits repeated submissions");

    // ---- sleep / wake: the path is rebuilt, memory comes back without a click ----
    Rig sleeper(false);
    sleeper.e->restore_memory(first.file);
    sleeper.discovery("confirmed", "confirmed");
    sleeper.path("id-a", {mem_desc("a4:5e:60:11:22:33")});
    sleeper.e->set_sleeping(true);
    t.check(!sleeper.e->snapshot().remembered, "asleep: nothing is trusted");
    sleeper.e->set_sleeping(false);
    sleeper.path("id-b", {mem_desc("a4:5e:60:11:22:33", "172.30.16.1", "172.30.16.0/20")});
    t.check(sleeper.e->snapshot().remembered, "wake with a renumbered lease: still recognised");

    // ---- other network with the same address: conflict, never reused ----
    Rig other(false);
    other.e->restore_memory(first.file);
    other.discovery("confirmed", "confirmed");
    other.svc.attempts["dormitory"] = "success";
    other.path("id-home", {mem_desc("b8:27:eb:44:55:66")});
    other.rounds(3);
    t.equal(other.e->snapshot().memory_level, MatchLevel::conflict, "different gateway hardware at the same address is a conflict");
    t.check(other.svc.submissions.empty(), "conflict: zero credential submissions");
    t.equal(other.e->snapshot().auto_login, AutoLoginState::needsTrust, "conflict falls back to the explicit choice");
    t.equal(other.e->snapshot().proof_wait, std::string("conflict"), "the reason is reported");
    t.equal(other.e->trust_and_remember(), ConnectionEngine::RememberResult::remembered, "the user may still trust the new network explicitly");
    t.equal(other.e->snapshot().memory_count, static_cast<std::size_t>(2), "the new network is a separate record");

    // ---- tunnel / several interfaces: memory survives, but both proofs are required ----
    Rig tun(false);
    tun.e->restore_memory(first.file);
    tun.svc.attempts["dormitory"] = "success";
    tun.discovery("unconfirmed", "confirmed");
    tun.path("id-tun", {mem_desc("a4:5e:60:11:22:33")}, true);
    tun.rounds(3);
    t.check(tun.e->snapshot().remembered, "a tunnel does not erase the remembered network");
    t.check(tun.svc.submissions.empty(), "tunnel + only the dormitory portal: conservative wait");
    t.equal(tun.e->snapshot().auto_login, AutoLoginState::awaitingProof, "waiting is visible as 'awaiting proof'");
    t.equal(tun.e->snapshot().proof_wait, std::string("tunnel"), "the tunnel is named as the reason");
    tun.discovery("confirmed", "confirmed");
    tun.rounds(1);
    t.equal(tun.svc.submissions.size(), static_cast<std::size_t>(1), "both proofs available: exactly one submission");
    Rig multi(false);
    multi.e->restore_memory(first.file);
    multi.svc.attempts["dormitory"] = "success";
    multi.discovery("unconfirmed", "confirmed");
    multi.path("id-multi", {mem_desc("a4:5e:60:11:22:33"), mem_desc("c0:ff:ee:00:00:01", "192.168.1.1", "192.168.1.0/24")});
    multi.rounds(3);
    t.check(multi.svc.submissions.empty() && multi.e->snapshot().remembered, "two live interfaces are ambiguous too: wait for both proofs");
    multi.discovery("confirmed", "confirmed");
    multi.rounds(3);
    t.check(multi.svc.submissions.empty() && multi.e->snapshot().auto_login == AutoLoginState::awaitingProof,
            "two physical exits cannot borrow one adapter's remembered eligibility even with both portal proofs");
    const auto multi_presentation = status::present(status::input_from(multi.e->snapshot()));
    t.check(!multi.e->snapshot().can_trust && multi_presentation.primary == std::optional<std::string>("network") &&
            multi_presentation.banner && multi_presentation.banner->key == "banner.multipleNetworks",
            "ambiguous egress explains the network change needed instead of offering ineffective trust");
    t.equal(multi.e->trust_and_remember(), ConnectionEngine::RememberResult::unreliable, "an ambiguous attachment cannot be remembered");

    // ---- revoking wins over an in-flight round ----
    Rig revoke(false);
    revoke.e->restore_memory(first.file);
    revoke.svc.attempts["dormitory"] = "success";
    revoke.discovery("confirmed", "confirmed");
    revoke.path("id-r", {mem_desc("a4:5e:60:11:22:33")});
    revoke.rounds(1);
    revoke.svc.during_probe = [&] { revoke.e->forget_current_network(); revoke.svc.during_probe = nullptr; };
    revoke.rounds(1);
    t.check(revoke.svc.submissions.empty(), "forget during a round: the stale round submits nothing");
    t.check(!revoke.e->snapshot().remembered && revoke.e->snapshot().memory_count == 0, "record removed");
    t.check(revoke.file.find("\"records\":[]") != std::string::npos, "the file no longer contains the record");
    revoke.rounds(3);
    t.check(revoke.svc.submissions.empty(), "after revoking, late rounds cannot restore it");

    // ---- write failures keep old data; revocation still holds this session ----
    Rig fail;
    fail.discovery("confirmed", "confirmed");
    fail.path("id-1", {mem_desc("a4:5e:60:11:22:33")});
    fail.rounds(2);
    fail.fail_write = true;
    t.equal(fail.e->trust_and_remember(), ConnectionEngine::RememberResult::failed, "remember reports a failed write");
    t.check(!fail.has_file && fail.e->snapshot().memory_count == 0 && fail.e->snapshot().memory_write_failed, "nothing half-written");
    // Trust is granted only after the memory record is saved successfully.
    t.check(!fail.e->snapshot().trusted, "a failed remember write never grants session trust");
    fail.fail_write = false;
    t.equal(fail.e->trust_and_remember(), ConnectionEngine::RememberResult::remembered, "retry after the storage recovers");
    const std::string kept = fail.file;
    fail.fail_write = true;
    fail.e->forget_current_network();
    t.check(fail.file == kept, "failed delete leaves the stored file untouched");
    t.check(!fail.e->snapshot().remembered, "but the record is suppressed for this session");

    // ---- unreadable / unknown schema: keep the file, no memory, clear reason ----
    Rig broken(false);
    t.check(!broken.e->restore_memory(std::string("{oops")), "corrupt file is reported");
    t.check(broken.e->snapshot().memory_unreadable && broken.e->snapshot().remember_blocker == "unreadable", "blocker: unreadable");
    t.check(!broken.e->restore_memory(std::string("{\"schema\":9,\"salt\":\"00112233445566778899aabbccddeeff\",\"records\":[]}")), "unknown schema is refused");
    t.equal(broken.e->trust_and_remember(), ConnectionEngine::RememberResult::unreadable, "cannot remember while unreadable");
    t.check(!broken.has_file, "the unreadable file is never overwritten");

    // ---- pre-submission fallback needs no session trust when the network is remembered; one submission per round ----
    Rig fallback(false);
    fallback.e->restore_memory(first.file);
    fallback.svc.discovery = {{{"teaching", "confirmed"}, {"dormitory", "unconfirmed"}}, {{"teaching", "confirmed"}, {"dormitory", "confirmed"}}};
    fallback.svc.attempts["teaching"] = "unavailable";
    fallback.svc.attempts["dormitory"] = "success";
    fallback.path("id-fb", {mem_desc("a4:5e:60:11:22:33")});
    fallback.rounds(2);
    t.equal(fallback.svc.attempt_log.size(), static_cast<std::size_t>(2), "teaching unavailable, then dormitory");
    t.equal(fallback.svc.submissions.size(), static_cast<std::size_t>(1), "only the dormitory attempt carried a password");
    Rig refused(false);
    refused.e->restore_memory(first.file);
    refused.svc.discovery = {{{"teaching", "confirmed"}, {"dormitory", "unconfirmed"}}, {{"teaching", "confirmed"}, {"dormitory", "confirmed"}}};
    refused.svc.attempts["teaching"] = "rejected:unspecified";
    refused.svc.attempts["dormitory"] = "success";
    refused.path("id-rf", {mem_desc("a4:5e:60:11:22:33")});
    refused.rounds(2);
    t.equal(refused.svc.submissions.size(), static_cast<std::size_t>(1), "a refusal after submission never triggers a second password in the same round");

    // ---- an area mismatch is a fact about the network, not a permanent hold ----
    Rig mismatch;
    mismatch.svc.attempts["teaching"] = "rejected:areaMismatch";
    mismatch.discovery("confirmed", "unconfirmed");
    mismatch.path("id-m1", {mem_desc("a4:5e:60:11:22:33")});
    mismatch.rounds(2);
    mismatch.t += 100;
    mismatch.rounds(2);
    snap = mismatch.e->snapshot();
    t.check(!snap.holds.empty(), "a hold exists on this network");
    bool sticky = false;
    for (const auto& h : snap.holds) sticky |= h.sticky();
    t.check(!sticky, "two area-mismatch refusals never become a sticky hold");
    mismatch.path("id-m2", {mem_desc("b8:27:eb:44:55:66")});
    t.check(mismatch.e->snapshot().holds.empty(), "another network starts clean");
    t.check(mismatch.e->snapshot().mode == MonitoringMode::recover, "the user's choice of automatic recovery is untouched");
}

// Offline end-to-end tests exercise ConnectionEngine itself; service callbacks emulate late
// network replies even after cancellation. All paths use fixture data and an in-memory writer.
inline void memory_verification_tests(Harness& t) {
    using namespace szu;
    using memtest::Rig;
    using R = ConnectionEngine::RememberResult;
    t.begin("memory-verification");
    const auto descriptor = mem_desc("a4:5e:60:11:22:33");
    for (const auto mode : {MonitoringMode::manual, MonitoringMode::observe, MonitoringMode::recover}) {
        for (const auto area : {CampusArea::teaching, CampusArea::dormitory, CampusArea::automatic}) {
            Rig r;
            r.e->configure(mode, area, ProbeMode::quick, std::nullopt);
            r.discovery("confirmed", "confirmed");
            r.path("fixture-online", {descriptor});
            r.svc.default_probe = "online";
            t.check(r.e->check() && r.e->snapshot().online, "ordinary online check succeeds without discovery");
            t.check(!r.e->snapshot().last_discovery && r.e->snapshot().can_remember, "reliable online path offers verification without old proof");
            t.equal(r.e->trust_and_remember(), R::remembered, "online credential-free remember works for every mode and selected area");
            t.check(r.svc.discoveries == 1 && r.svc.discovery_areas.front() ==
                    (area == CampusArea::teaching ? CampusArea::teaching : CampusArea::automatic),
                    "remember discovers only the selected campus scope");
            t.check(r.svc.attempt_log.empty() && r.svc.submissions.empty() && r.svc.probe_calls == 1,
                    "verification never accesses authentication or runs another Internet check");
            t.check(r.e->snapshot().trusted == (area != CampusArea::teaching) && r.e->snapshot().remembered
                    && r.e->snapshot().online && !r.e->snapshot().busy,
                    "only a dormitory scope grants session trust after persistence");
        }
    }
    {
        Rig r;
        r.e->set_area(CampusArea::teaching);
        r.path("fixture-teaching", {descriptor});
        r.discovery("confirmed", "notChecked");
        t.equal(r.e->trust_and_remember(), R::remembered, "explicit teaching needs only a fresh teaching proof");
        const auto saved = NetworkMemory::decode(r.file, r.wall);
        t.check(saved && saved->records().size() == 1 && saved->records()[0].area == "teaching"
                && saved->records()[0].proof_teaching && !saved->records()[0].proof_dormitory,
                "teaching record has no dormitory proof");
        t.check(!r.e->snapshot().trusted && r.e->snapshot().remembered && r.svc.submissions.empty(),
                "teaching memory does not grant session trust or submit credentials");
        r.e->set_area(CampusArea::dormitory);
        r.discovery("unconfirmed", "confirmed");
        r.rounds(2);
        t.equal(r.e->snapshot().auto_login, AutoLoginState::needsTrust,
                "strong teaching memory cannot authorize a fresh dormitory-only recovery");
        t.check(r.svc.submissions.empty(), "teaching-only memory never submits dormitory credentials");
    }
    {
        Rig r;
        t.equal(r.remember_dorm(), R::remembered, "fixture begins with a dormitory memory");
        r.e->set_area(CampusArea::teaching);
        r.discovery("confirmed", "notChecked");
        t.equal(r.e->trust_and_remember(), R::remembered, "teaching proof may refresh the same record");
        const auto saved = NetworkMemory::decode(r.file, r.wall);
        t.check(saved && saved->records().size() == 1 && !saved->records()[0].proof_dormitory
                && saved->records()[0].area == "teaching",
                "teaching refresh downgrades old dormitory proof instead of extending it");
    }
    {
        Rig r;
        t.equal(r.remember_dorm(), R::remembered, "touch fixture begins with a dormitory memory");
        const std::string original = r.file;
        r.t += NetworkMemory::kTouchInterval + 1;
        r.wall += NetworkMemory::kTouchInterval + 1;
        r.e->set_area(CampusArea::teaching);
        r.svc.attempts["teaching"] = "success";
        t.check(r.e->connect(), "fixture completes a manual teaching login after the touch interval");
        t.check(!r.svc.submissions.empty() && r.file == original,
                "teaching login cannot extend an old dormitory proof's lifetime or rewrite its area");
    }
    {
        Rig r;
        r.e->configure(MonitoringMode::manual, CampusArea::automatic, ProbeMode::quick, std::nullopt);
        r.path("fixture-offline", {descriptor});
        r.e->check();
        t.check(r.e->snapshot().can_remember && !r.e->snapshot().last_discovery, "ordinary offline check also offers fresh verification");
        r.discovery("confirmed", "confirmed");
        t.equal(r.e->trust_and_remember(), R::remembered, "offline check then remember needs no credentials");
        t.check(r.svc.attempt_log.empty() && !r.e->snapshot().online, "campus identity proof alone never claims Internet connectivity");
    }
    for (const auto incomplete : {Discovery::unconfirmed, Discovery::notChecked}) {
        for (const bool teaching : {true, false}) {
            Rig r;
            r.path("fixture-stale", {descriptor});
            r.discovery("confirmed", "confirmed");
            r.rounds(2);  // populate a previously successful double proof without granting trust
            const auto before = r.svc.discoveries;
            r.discovery(teaching ? std::string(to_string(incomplete)).c_str() : "confirmed",
                        teaching ? "confirmed" : std::string(to_string(incomplete)).c_str());
            const auto result = r.e->trust_and_remember();
            t.equal(result, teaching ? R::needsProof : R::remembered,
                    "automatic scope remembers a fresh teaching proof but never stale dormitory proof");
            t.check(r.svc.discoveries == before + 1 && r.has_file == !teaching && !r.e->snapshot().trusted
                    && r.e->snapshot().remembered == !teaching,
                    "only a fresh teaching proof may create teaching-only memory without session trust");
        }
    }
    {
        Rig r;
        r.path("fixture-trusted", {descriptor});
        r.discovery("confirmed", "confirmed");
        t.check(r.e->trust_current_network(), "independent user session trust is granted");
        r.fail_write = true;
        t.equal(r.e->trust_and_remember(), R::failed, "storage failure remains a failure with pre-existing session trust");
        t.check(r.e->snapshot().trusted && !r.e->snapshot().remembered && !r.has_file,
                "failed remembering preserves independently granted trust without creating a memory");
    }
    for (const bool trusted : {false, true}) {
        for (const bool online : {false, true}) {
            Rig r;
            r.path("fixture-non-campus", {descriptor});
            if (online) {
                r.svc.default_probe = "online";
                r.e->check();
            }
            if (trusted) t.check(r.e->trust_current_network(), "user may grant independent session trust before verification");
            r.discovery("unconfirmed", "notChecked");
            const auto before_probes = r.svc.probe_calls;
            t.equal(r.e->trust_and_remember(), R::needsProof, "non-campus service evidence cannot be remembered");
            const auto state = r.e->snapshot();
            t.check(state.trusted == trusted && !state.remembered && !r.has_file && r.writes == 0,
                    "unconfirmed campus service never creates network memory or additional trust");
            t.check(!state.busy && !state.verifying_memory && state.online == online && state.status ==
                    (online ? EngineStatus::online : EngineStatus::unconfirmed),
                    "failed memory verification finishes while retaining the independent Internet fact");
            t.check(state.last_discovery && state.last_discovery->teaching == Discovery::unconfirmed &&
                    r.svc.discoveries == 1 && r.svc.probe_calls == before_probes && r.svc.submissions.empty() && r.svc.attempt_log.empty(),
                    "negative campus proof remains visible without another Internet probe or credential access");
        }
    }
    for (int interruption = 0; interruption < 10; ++interruption) {
        for (const bool queued : {true, false}) {
            Rig r;
            r.path("fixture-A", {descriptor});
            r.discovery("confirmed", "confirmed");
            const auto request = r.e->prepare_remember();
            t.check(request.has_value() && r.e->snapshot().busy && r.e->snapshot().verifying_memory && !r.e->snapshot().can_remember,
                    "UI admission reserves the operation before worker execution");
            auto invalidate = [&] {
                switch (interruption) {
                    case 0: r.e->cancel(); break;
                    case 1: r.path("fixture-B", {descriptor}); r.path("fixture-A", {descriptor}); break;
                    case 2: r.e->set_sleeping(true); r.e->set_sleeping(false); r.path("fixture-A", {descriptor}); break;
                    case 3: r.e->forget_current_network(); break;
                    case 4: r.e->forget_all_networks(); break;
                    case 5: r.e->revoke_trust(); break;
                    case 6: r.e->restore_memory(std::nullopt); break;
                    case 7: r.path("fixture-A", {mem_desc("b8:27:eb:44:55:66")}); r.path("fixture-A", {descriptor}); break;
                    case 8: r.path("fixture-A", {descriptor}, true); r.path("fixture-A", {descriptor}); break;
                    default: r.e->set_area(CampusArea::teaching); break;
                }
            };
            if (queued) invalidate();
            else r.svc.during_discovery = invalidate;
            t.equal(r.e->trust_and_remember(*request), R::cancelled, "queue and in-flight invalidations reject late positive discovery");
            t.check(!r.has_file && !r.e->snapshot().trusted && !r.e->snapshot().remembered && r.svc.attempt_log.empty(),
                    "invalidation leaves no persisted or authorized result and never authenticates");
            t.equal(r.svc.discoveries, queued ? 0 : 1, "cancelled queued work never starts discovery");
        }
    }
    {
        Rig r;
        r.path("fixture-busy", {descriptor});
        r.discovery("confirmed", "confirmed");
        auto first = r.e->prepare_remember();
        t.equal(r.e->trust_and_remember(), R::busy, "remember rejects an occupied operation");
        t.check(!r.e->check() && !r.e->automatic_round(), "reserved memory verification excludes other workers");
        std::optional<ConnectionEngine::RememberRequest> successor;
        r.svc.during_discovery = [&] { r.e->cancel(); successor = r.e->prepare_remember(); };
        t.equal(r.e->trust_and_remember(*first), R::cancelled, "old positive reply cannot overtake a newer request");
        t.check(successor && r.e->snapshot().busy && !r.has_file, "stale finish cannot release the successor's operation");
        r.svc.during_discovery = nullptr;
        t.equal(r.e->trust_and_remember(*successor), R::remembered, "successor receives its own fresh discovery");
        t.equal(r.e->trust_and_remember(*successor), R::cancelled, "consumed request cannot be replayed");
        t.check(r.svc.discoveries == 2 && r.writes == 1, "replay creates neither another discovery nor another write");
    }
    for (int unavailable = 0; unavailable < 8; ++unavailable) {
        Rig r;
        PathSnapshot p;
        p.usable = true; p.identity = "fixture-path"; p.link_active = true; p.descriptors = {descriptor};
        switch (unavailable) {
            case 0: p.identity.reset(); break;
            case 1: p.link_active = false; break;
            case 2: p.usable = false; break;
            case 3: p.descriptors.clear(); break;
            case 4: p.descriptors = {mem_desc("", "", "")}; break;
            case 5: p.descriptors.push_back(descriptor); break;
            case 6: p.identity = std::string(); break;
            default: r.e->set_sleeping(true); break;
        }
        r.e->receive_path(p);
        t.check(!r.e->snapshot().can_remember && r.e->trust_and_remember() != R::remembered,
                "missing unique reliable live identity refuses verification");
        t.check(r.svc.discoveries == 0 && !r.has_file && !r.e->snapshot().trusted, "ineligible paths cannot discover, persist or authorize");
    }
    {
        Rig r;
        r.e = std::make_unique<ConnectionEngine>(r.svc, r.hooks(), true);
        r.e->restore_memory(std::nullopt);
        r.path("fixture-read-only", {descriptor});
        t.equal(r.e->trust_and_remember(), R::readOnly, "read-only session rejects verification");
        t.check(!r.e->snapshot().can_remember && r.svc.discoveries == 0 && !r.has_file, "read-only guard precedes all work");
    }
    for (const auto reason : {HoldReason::userPaused, HoldReason::security, HoldReason::uncertain, HoldReason::credentials}) {
        Rig r;
        auto h = r.hooks();
        h.persist_recovery = [](const std::string&) { return true; };
        r.e = std::make_unique<ConnectionEngine>(r.svc, h);
        r.e->configure(MonitoringMode::recover, CampusArea::automatic, ProbeMode::quick, Credentials{"fixture-A", "pw-a"});
        r.e->restore_memory(std::nullopt);
        r.path("fixture-held", {descriptor});
        const std::string journal = "{\"schema\":1,\"generation\":1,\"notBefore\":1750000090,\"holds\":[{\"reason\":" +
            std::to_string(static_cast<int>(reason)) + ",\"origin\":2}]}";
        t.check(r.e->restore_recovery(journal), "fixture restores a durable hold and a full cooldown");
        r.discovery("confirmed", "confirmed");
        const auto before = r.e->snapshot();
        t.equal(r.e->trust_and_remember(), R::remembered, "credential-free identity verification can run while authentication is held");
        const auto after = r.e->snapshot();
        t.check(after.holds.size() == before.holds.size() && after.pause_reason == before.pause_reason && after.paused,
                "verification preserves user, password, uncertain and security holds");
        t.check(after.cooldown_remaining == before.cooldown_remaining && after.cooldown_remaining.value_or(0) == 90,
                "verification preserves the exact 90-second cooldown");
        t.check(r.e->draft() == Credentials{"fixture-A", "pw-a"} && r.svc.attempt_log.empty(),
                "verification neither changes credentials nor authenticates");
        r.rounds(3);
        t.check(r.svc.attempt_log.empty(), "remembering cannot bypass a hold in later automatic rounds");
    }
    for (const auto reason : {HoldReason::userPaused, HoldReason::security, HoldReason::uncertain, HoldReason::credentials}) {
        Rig r;
        auto h = r.hooks();
        h.persist_recovery = [](const std::string&) { return true; };
        r.e = std::make_unique<ConnectionEngine>(r.svc, h);
        r.e->configure(MonitoringMode::recover, CampusArea::automatic, ProbeMode::quick, Credentials{"fixture-A", "pw-a"});
        r.e->restore_memory(std::nullopt);
        r.path("fixture-held-non-campus", {descriptor});
        const std::string journal = "{\"schema\":1,\"generation\":1,\"notBefore\":1750000090,\"holds\":[{\"reason\":" +
            std::to_string(static_cast<int>(reason)) + ",\"origin\":2}]}";
        t.check(r.e->restore_recovery(journal), "negative-proof fixture restores a hold and exact cooldown");
        r.discovery("unconfirmed", "notChecked");
        const auto before = r.e->snapshot();
        t.equal(r.e->trust_and_remember(), R::needsProof, "a failed campus proof does not remove an authentication hold");
        const auto after = r.e->snapshot();
        t.check(after.status == EngineStatus::paused && !after.busy && !after.verifying_memory && after.paused &&
                after.holds.size() == before.holds.size() && after.pause_reason == before.pause_reason &&
                after.cooldown_remaining == before.cooldown_remaining && after.cooldown_remaining == 90.0,
                "failed discovery retains the visible hold and original ninety-second protection");
        t.check(!after.trusted && !after.remembered && !r.has_file && r.writes == 0 &&
                r.e->draft() == Credentials{"fixture-A", "pw-a"} && r.svc.attempt_log.empty(),
                "failed discovery cannot change credentials, authorize, persist or authenticate");
    }
}

inline void records_range_tests(Harness& t) {
    using namespace szu;
    t.begin("records-range");
    records::HealthLog log;
    const double base = 1750000000.0;
    for (int i = 0; i < 30; ++i) {
        ConnectivityReport report = ConnectivityReport::simple(true, true);
        report.mode = ProbeMode::complete;
        log.append(records::sample_from(report, base + i * 60, 2, i % 2 ? 2 : 1));
    }
    const auto range = log.range(base + 29 * 60);
    t.check(range.count == 30 && range.first == base && range.last == base + 29 * 60, "range covers first and last retained sample");
    t.check(range.ribbon_count == 24 && range.ribbon_first == base + 6 * 60, "ribbon draws exactly the newest 24");
    t.check(!log.range(base + 3 * 24 * 3600).first.has_value(), "samples older than 24 h are outside the range");
    records::HealthLog capped;
    for (int i = 0; i < 400; ++i) capped.append(records::sample_from(ConnectivityReport::simple(true, false), base + i * 60));
    t.check(capped.items().size() == 360 && capped.items().front().time == base + 40 * 60, "at most 360 samples are kept, oldest dropped");
    const auto restored = records::HealthLog::from_json(json::serialize(log.to_json()));
    t.check(restored && restored->items().size() == 30 && restored->items()[1].site_total == 2 && restored->items()[1].site_passed == 2 && restored->items()[0].site_passed == 1,
            "aggregate site counts survive a round trip");
    auto expanded_report = ConnectivityReport::simple(true, true); expanded_report.mode = ProbeMode::complete;
    records::HealthLog expanded_history;
    expanded_history.append(records::sample_from(expanded_report, base, 12, 11));
    const auto expanded = records::HealthLog::from_json(json::serialize(expanded_history.to_json()));
    t.check(expanded && expanded->items().size() == 1 && expanded->items()[0].site_total == 12
            && expanded->items()[0].site_passed == 11,
            "Twelve-site partial evidence survives creation, serialization and restore");
    const auto too_many = records::sample_from(expanded_report, base, 13, 13);
    t.check(too_many.site_total == -1 && too_many.site_passed == -1, "Thirteen-site aggregate remains outside the bound");
    const std::string legacy = "{\"schemaVersion\":1,\"samples\":[{\"date\":\"2026-09-29T08:30:00Z\",\"online\":true,\"mode\":\"quick\",\"apple\":\"success\",\"microsoft\":\"notCompleted\"}]}";
    const auto old = records::HealthLog::from_json(legacy);
    t.check(old && old->items().size() == 1 && old->items()[0].site_total == -1, "history without website counts still loads");
    const std::string invalid = "{\"schemaVersion\":1,\"samples\":[{\"date\":\"2026-09-29T08:30:00Z\",\"online\":true,\"websiteTotal\":2,\"websitePassed\":3}]}";
    const auto bad = records::HealthLog::from_json(invalid);
    t.check(bad && bad->items()[0].site_total == -1 && bad->items()[0].site_passed == -1, "impossible counts are dropped, not trusted");
    const std::string exported = json::serialize(records::diagnostics_export(records::ExportContext{}, log, records::EventLog{}));
    t.check(exported.find("http") == std::string::npos && exported.find("websiteTotal") != std::string::npos, "export carries counts but no address");
}

inline void memory_tests(Harness& t) {
    crypto_tests(t);
    netmemory_fixture_tests(t);
    netmemory_store_tests(t);
    memory_engine_tests(t);
    memory_verification_tests(t);
    records_range_tests(t);
}
