#pragma once
#include "szu/wifi_recovery.hpp"
#include "test_support.hpp"

namespace wifitest {
struct Radio final : szu::wifi::Radio {
    szu::wifi::PhysicalState physical = szu::wifi::PhysicalState::disconnected;
    std::optional<bool> powered = true;
    szu::wifi::Result power_result = szu::wifi::Result::poweredOn;
    szu::wifi::ScanResult scan_result;
    szu::wifi::Result join_result = szu::wifi::Result::joined;
    int states = 0, powers = 0, scans = 0, joins = 0, profile_joins = 0;
    std::string profile_target;
    std::vector<std::string> scanned;
    std::function<void()> before_state, during_scan, before_join;
    szu::wifi::PhysicalState physical_state() override { ++states; if (before_state) before_state(); return physical; }
    std::optional<bool> power_state() override { return powered; }
    szu::wifi::Result power_on(const szu::CancelToken&) override { ++powers; return power_result; }
    szu::wifi::ScanResult scan(const std::string& ssid, const szu::CancelToken&) override {
        ++scans; scanned.push_back(ssid); if (during_scan) during_scan(); return scan_result;
    }
    szu::wifi::Result associate_open(const szu::wifi::ScanCandidate&, const szu::CancelToken&) override {
        ++joins; if (before_join) before_join(); return join_result;
    }
    szu::wifi::Result associate_profile(const szu::wifi::ScanCandidate& candidate, const szu::CancelToken&) override {
        ++profile_joins; profile_target = candidate.ssid; if (before_join) before_join(); return join_result;
    }
};
}
inline void run_wifi_recovery_tests(szutest::Harness& t) {
    using namespace szu;
    using namespace szu::wifi;
    t.begin("wifi-recovery-preferences");
    const Network one{"01234567", "SZU-fixture", "假校园 Wi-Fi"}, two{"89abcdef", "SZU-other", "另一假校园 Wi-Fi"};
    t.check(one.valid(), "valid local candidate");
    for (const auto& ssid : std::vector<std::string>{"", std::string(33, 'a'), "invalid\n", std::string("\xff")})
        t.check(!Network{"id", ssid, "label"}.valid(), "unsafe SSID is rejected rather than rewritten");
    const auto original = temporary_open_profile("SZU & <校园>\"'");
    t.check(original && original->find("<name>SZU &amp; &lt;校园&gt;&quot;&apos;</name>") != std::string::npos,
        "temporary connection name preserves the original UTF-8 SSID with XML escaping");
    const auto bytes = temporary_open_profile("SZU-fixture");
    t.check(bytes && bytes->find("<hex>535A552D66697874757265</hex>") != std::string::npos &&
        bytes->find("<connectionMode>manual</connectionMode>") != std::string::npos &&
        bytes->find("<authentication>open</authentication>") != std::string::npos &&
        bytes->find("temporary open campus") == std::string::npos && bytes->find("keyMaterial") == std::string::npos,
        "temporary profile keeps exact SSID bytes, OPEN/NONE manual semantics and no credential material");
    t.check(!temporary_open_profile("bad\n") && !temporary_open_profile(std::string(33, 'a')),
        "unsafe temporary profile names are rejected before XML generation");
    Preferences p;
    t.check(!p.auto_join && !p.power_on && p.selected(one), "fresh install defaults off, candidate consent remains separate");
    p.auto_join = true; p.excluded.insert(two.id); p.manual_names[one.id] = one.ssid;
    const auto text = p.encode();
    const auto decoded = text ? Preferences::decode(*text) : std::nullopt;
    t.check(decoded && decoded->auto_join && !decoded->power_on && decoded->selected(one) && !decoded->selected(two) &&
        decoded->manual_names == p.manual_names, "schema-1 round trip retains separate power permission and opt-outs");
    for (const auto& bad : std::vector<std::string>{"broken", "{}", "{\"schemaVersion\":2}", std::string(16385, ' '),
        R"({"schemaVersion":1,"autoJoinEnabled":true,"powerOnWhenDisconnected":false,"selections":[],"excludedNetworkIDs":["id","id"]})",
        R"({"schemaVersion":1,"autoJoinEnabled":true,"powerOnWhenDisconnected":false,"selections":[],"excludedNetworkIDs":[],"manualNetworkNames":{"id":"bad\n"}})"})
        t.check(!Preferences::decode(bad), "malformed or unsupported preferences fail closed");
    auto legacy = Preferences::decode("{\"schemaVersion\":1,\"autoJoinEnabled\":true,\"powerOnWhenDisconnected\":false,\"selections\":[{\"id\":\"" + one.id + "\",\"ssidDigest\":\"" + sha256_hex(one.ssid) + "\"}]}");
    t.check(legacy && legacy->selected(one) && !legacy->selected(two), "legacy exact id plus SSID digest selection retained");
    auto renamed = one; renamed.ssid = "unexpected";
    t.check(legacy && !legacy->selected(renamed), "renamed SSID cannot inherit old selection");
    t.check(legacy && !legacy->migrate({}), "empty context never broadens a legacy allowlist");
    t.check(legacy && legacy->migrate({one,two}) && legacy->selected(one) && !legacy->selected(two), "legacy migration preserves previous explicit choice");
    NetworkLabels labels;
    labels.update_automatic(one.id, {one.display_name,"wifi",one.ssid,false});
    labels.update_automatic(two.id, {"wired", "ethernet", {},false});
    MemoryRecord r1; r1.id = one.id; r1.proof_teaching = true;
    MemoryRecord r2; r2.id = two.id; r2.proof_dormitory = true;
    t.equal(candidates({r1,r2},labels,p),std::vector<Network>{one},"only remembered Wi-Fi labels become candidates");
    t.check(candidates({},labels,p).empty(), "orphaned labels and names cannot become candidates after forget");
    p.manual_names[one.id] = "Manual-fixture";
    t.equal(candidates({r1},labels,p).front().ssid,std::string("Manual-fixture"),"manual exact SSID is local selection metadata only");
    p.manual_names[two.id] = "not-wifi";
    t.equal(candidates({r1,r2},labels,p).size(),std::size_t(1),"manual name cannot transform remembered Ethernet into Wi-Fi");

    t.begin("wifi-recovery-physical-guards");
    for (const auto physical : {PhysicalState::connected,PhysicalState::unknown}) {
        wifitest::Radio radio; radio.physical = physical; radio.powered = false;
        const auto result = recover(radio,{one},true,CancelToken{});
        t.check(result == (physical == PhysicalState::connected ? Result::alreadyConnected : Result::connectionUnclear) &&
            radio.powers == 0 && radio.scans == 0 && radio.joins == 0, "existing or unknown physical state blocks every mutation");
    }
    {
        wifitest::Radio radio; CancelToken token; token.cancel();
        t.check(recover(radio,{one},true,token)==Result::cancelled && radio.states==0,"cancelled request never accesses radio");
    }
    {
        wifitest::Radio radio;
        const std::vector<Network> invalid_name{{"id","bad\n","label"}};
        t.check(recover(radio,invalid_name,true,CancelToken{})==Result::cancelled && radio.states==0,"invalid candidate never accesses radio");
        t.check(recover(radio,std::vector<Network>(9,one),true,CancelToken{})==Result::cancelled && radio.states==0,"over-limit requests never scan");
    }
    {
        wifitest::Radio radio; radio.powered = false;
        t.check(recover(radio,{one},false,CancelToken{})==Result::wifiOff && radio.powers==0 && radio.scans==0,"no power permission means no radio write");
        t.check(recover(radio,{},true,CancelToken{})==Result::poweredOn && radio.powers==1 && radio.scans==0,"power-on consent works independently of candidate names");
    }
    {
        wifitest::Radio radio; radio.powered = std::nullopt;
        t.check(recover(radio,{one},true,CancelToken{})==Result::unavailable && radio.scans==0,"unknown radio power fails closed");
    }
    {
        wifitest::Radio radio;
        t.check(recover(radio,{},false,CancelToken{})==Result::noSelection && radio.scans==0,"no selected target never scans");
        radio.scan_result = {Result::notFound,{{"unselected","radio",100,true},{one.ssid,"radio",20,false}}};
        t.check(recover(radio,{one},false,CancelToken{})==Result::systemConnectionRequired && radio.joins==0,"unexpected SSIDs and encrypted targets never associate");
    }
    {
        wifitest::Radio radio; radio.scan_result = {Result::permissionRequired,{}};
        t.check(recover(radio,{one},false,CancelToken{})==Result::permissionRequired && radio.joins==0,"location/access denial is explicit with no fallback authority");
    }
    {
        wifitest::Radio radio; radio.scan_result = {Result::notFound,{{one.ssid,"radio",20,true}}};
        t.check(recover(radio,{one},false,CancelToken{})==Result::joined && radio.joins==1,"exact open candidate can be associated once");
    }
    {
        wifitest::Radio radio; radio.scan_result = {Result::notFound,{{one.ssid,"radio",20,true}}};
        radio.during_scan = [&] { radio.physical = PhysicalState::connected; };
        t.check(recover(radio,{one},false,CancelToken{})==Result::alreadyConnected && radio.joins==0,"Ethernet/Wi-Fi appearing during scan prevents takeover");
    }
    {
        wifitest::Radio radio; CancelToken token; radio.scan_result = {Result::notFound,{{one.ssid,"radio",20,true}}};
        radio.during_scan = [&] { token.cancel(); };
        t.check(recover(radio,{one},false,token)==Result::cancelled && radio.joins==0,"opt-out/forget during scan prevents association");
    }
    {
        wifitest::Radio radio; radio.scan_result = {Result::notFound,{}};
        auto duplicate=one; duplicate.id="another-id";
        t.check(recover(radio,{one,duplicate,two},false,CancelToken{})==Result::notFound && radio.scans==2,"SSID deduplication scans each selected name once");
    }
    {
        wifitest::Radio radio; CancelToken token; radio.scan_result = {Result::notFound,{{one.ssid,"radio",20,true}}};
        radio.before_join = [&] { token.cancel(); };
        t.check(recover(radio,{one},false,token)==Result::cancelled,"late cancelled OS result cannot publish joined");
    }
    t.begin("wifi-recovery-system-profile");
    const ScanCandidate secure{one.ssid, "saved-fixture", 20, false, true, true};
    const ScanCandidate open_same_name{one.ssid, "open-fixture", 100, true};
    {
        wifitest::Radio radio; radio.scan_result = {Result::notFound, {open_same_name, secure}};
        t.check(recover(radio, {one}, false, CancelToken{}) == Result::joined && radio.profile_joins == 1 &&
            radio.profile_target == one.ssid && radio.joins == 0,
            "a selected secure network uses its system profile even when an open namesake has a stronger signal");
    }
    {
        wifitest::Radio radio; auto unsaved = secure; unsaved.saved_profile = false;
        radio.scan_result = {Result::notFound, {open_same_name, unsaved}};
        t.check(recover(radio, {one}, false, CancelToken{}) == Result::systemConnectionRequired &&
            radio.profile_joins == 0 && radio.joins == 0,
            "an unsaved protected network cannot fall back to an open namesake");
    }
    {
        wifitest::Radio radio; auto unselected = secure; unselected.ssid = two.ssid;
        radio.scan_result = {Result::notFound, {unselected}};
        t.check(recover(radio, {one}, false, CancelToken{}) == Result::notFound &&
            radio.profile_joins == 0 && radio.joins == 0, "a system profile for another SSID cannot expand the selected targets");
    }
    for (const auto& invalid : std::vector<ScanCandidate>{{one.ssid, "bad-profile", 100, true, true, true},
            {one.ssid, "bad-profile", 100, false, true, false}, {one.ssid, "bad-profile", 100, true, false, true}}) {
        wifitest::Radio radio; radio.scan_result = {Result::notFound, {invalid, open_same_name}};
        t.check(recover(radio, {one}, false, CancelToken{}) == Result::systemConnectionRequired &&
            radio.profile_joins == 0 && radio.joins == 0,
            "contradictory protected profile metadata cannot enter either connection path or trigger an open downgrade");
    }
    for (const auto physical : {PhysicalState::connected, PhysicalState::unknown}) {
        wifitest::Radio radio; radio.physical = physical; radio.scan_result = {Result::notFound, {secure}};
        t.check(recover(radio, {one}, false, CancelToken{}) ==
            (physical == PhysicalState::connected ? Result::alreadyConnected : Result::connectionUnclear) &&
            radio.scans == 0 && radio.profile_joins == 0, "saved profile recovery still yields to every existing or uncertain physical connection");
    }
    {
        wifitest::Radio radio; radio.powered = false; radio.scan_result = {Result::notFound, {secure}};
        t.check(recover(radio, {one}, false, CancelToken{}) == Result::wifiOff && radio.profile_joins == 0 && radio.powers == 0,
            "a saved profile grants no permission to power on Wi-Fi");
    }
    {
        wifitest::Radio radio; CancelToken token; radio.scan_result = {Result::notFound, {secure}};
        radio.during_scan = [&] { token.cancel(); };
        t.check(recover(radio, {one}, false, token) == Result::cancelled && radio.profile_joins == 0,
            "forgetting or excluding a target during scanning also prevents system profile association");
    }
    {
        wifitest::Radio radio; radio.scan_result = {Result::notFound, {secure}};
        radio.during_scan = [&] { radio.physical = PhysicalState::connected; };
        t.check(recover(radio, {one}, false, CancelToken{}) == Result::alreadyConnected && radio.profile_joins == 0,
            "another attachment appearing before system profile association prevents takeover");
    }
    {
        wifitest::Radio radio; CancelToken token; radio.scan_result = {Result::notFound, {secure}};
        radio.before_join = [&] { token.cancel(); };
        t.check(recover(radio, {one}, false, token) == Result::cancelled && radio.profile_joins == 1,
            "a late system profile completion cannot publish success after cancellation");
    }
    {
        wifitest::Radio radio; radio.scan_result = {Result::notFound, {secure, open_same_name}};
        radio.join_result = Result::failed;
        t.check(recover(radio, {one}, false, CancelToken{}) == Result::failed && radio.profile_joins == 1 && radio.joins == 0,
            "a failed saved profile never retries its open namesake");
    }
    {
        wifitest::Radio radio; radio.scan_result = {Result::notFound, {secure, open_same_name}};
        radio.join_result = Result::systemConnectionRequired;
        t.check(recover(radio, {one}, false, CancelToken{}) == Result::systemConnectionRequired &&
            radio.profile_joins == 1 && radio.joins == 0,
            "a missing or unsupported system profile keeps its actionable status without downgrading to open association");
    }

    t.begin("wifi-recovery-schedule");
    Schedule schedule;
    t.check(!schedule.ready(0) && !schedule.begin(0),"default schedule never runs");
    schedule.update({one},true,false,true,true,false);
    t.check(!schedule.ready(0) && !schedule.begin(0) && schedule.status()=="connected","connected context prevents finding other networks");
    schedule.update({one},true,false,true,false,false);
    t.check(schedule.ready(0) && schedule.ready(0) && !schedule.active() && schedule.status()=="waiting",
        "checking readiness neither admits work nor changes its display state");
    auto request=schedule.begin(0);
    t.check(request && schedule.status()=="searching" && !schedule.ready(1) && !schedule.begin(1),"one worker admission at a time");
    auto display_only=one; display_only.display_name="renamed local label";
    schedule.update({display_only},true,false,true,false,false);
    t.check(request && !request->token.cancelled() && schedule.active(),"display-only rename cannot cancel an association or change retry scheduling");
    schedule.finish(Result::notFound,0);
    t.check(!schedule.ready(1.999) && schedule.ready(2) && schedule.status()=="waiting",
        "readiness honors the retry boundary without clearing the previous result");
    double elapsed = 0;
    for (const double delay : {2, 5, 10, 20, 30, 60, 120, 300, 300}) {
        t.check(!schedule.begin(elapsed + delay - 0.001) && schedule.begin(elapsed + delay).has_value(),
            "transient radio retries begin promptly and remain bounded during long absences");
        elapsed += delay;
        schedule.finish(Result::notFound,elapsed);
    }
    for (const auto result : {Result::permissionRequired, Result::unavailable, Result::systemConnectionRequired}) {
        Schedule protected_schedule; protected_schedule.update({one},true,false,true,false,false);
        protected_schedule.begin(0); protected_schedule.finish(result,0);
        t.check(!protected_schedule.ready(14.999) && protected_schedule.ready(15),
            "permission, unavailable and system-configuration failures retain their conservative retry interval");
    }
    schedule.update({},true,true,true,false,false);
    auto powering=schedule.begin(600);
    t.check(powering && powering->power_on && powering->networks.empty(),"separate radio-on permission is independent of candidate selection");
    schedule.finish(Result::poweredOn,600);
    t.check(!schedule.begin(601),"powered-on recheck waits two seconds");
    auto running = schedule.begin(602);
    t.check(running.has_value(),"powered-on recheck begins at two seconds");
    schedule.update({},false,false,true,false,false);
    t.check(running && running->token.cancelled() && !schedule.ready(700) && !schedule.begin(700) && schedule.status()=="disabled","disabling revokes outstanding association authorization");
    schedule.update({one},true,false,false,false,false);
    t.check(!schedule.ready(800) && !schedule.begin(800) && schedule.status()=="paused","manual/observe or credential hold pauses finding networks");
    schedule.update({one},true,false,true,false,true);
    t.check(!schedule.ready(900) && !schedule.begin(900) && schedule.status()=="paused","sleep blocks radio activity");
    schedule.update({one},true,false,true,false,false);
    auto forgotten=schedule.begin(1000);
    schedule.update({},true,false,true,false,false);
    schedule.finish(Result::joined,1000);
    t.check(forgotten && forgotten->token.cancelled() && !schedule.begin(1000) && schedule.status()=="noSelection","forget suppresses candidate and late joined publication");
}
