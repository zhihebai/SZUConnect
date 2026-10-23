#include <string>
#include <vector>

inline void netlabels_tests(Harness& t) {
    t.begin("network-labels");
    NetworkLabels labels;
    const std::string one = "01234567", two = "89abcdef";
    t.equal(labels.display_name(one), std::string(copy::text("memory.item")), "old hash cannot invent a name");
    t.check(labels.update_automatic(one, {"SZU", "wifi", "SZU", false}), "capture real SSID metadata");
    t.equal(labels.display_name(one), std::string("SZU"), "actual SSID displayed");
    t.check(labels.rename(one, "宿舍 Wi-Fi"), "manual rename succeeds");
    t.equal(labels.entries().at(one).ssid.value_or(""), std::string("SZU"), "rename never changes SSID");
    t.check(labels.update_automatic(one, {"SZU-2", "wifi", "SZU-2", false}), "observed SSID updates");
    t.equal(labels.display_name(one), std::string("宿舍 Wi-Fi"), "automatic update preserves manual name");
    t.equal(labels.entries().at(one).ssid.value_or(""), std::string("SZU-2"), "automatic metadata updates actual SSID");
    t.check(!labels.update_automatic(one, {"Ethernet", "ethernet", {}, false}), "wired observation preserves known wifi");
    t.check(!labels.update_automatic(one, {NetworkLabels::fallback("wifi"), "wifi", {}, false}), "missing permission preserves metadata");
    t.check(labels.update_automatic(two, {"USB LAN", "ethernet", {}, false}), "ethernet system service label");
    t.equal(labels.display_name(two), std::string("USB LAN"), "ethernet service name displayed");
    t.check(!labels.rename(one, "\n\t "), "empty sanitized rename rejected");
    t.check(!labels.rename("wrong", "Bad ID"), "invalid record id rejected");
    t.check(!labels.update_automatic(one, {"name", "not-a-kind", {}, false}), "unknown network kind rejected");
    t.equal(NetworkLabels::clean_name("\xE2\x80\xAE" "Campus\n\t"), std::string("Campus"), "directional and control characters removed");
    t.equal(NetworkLabels::clean_name(std::string(100, 'x')).size(), std::size_t(80), "display name length capped");
    t.equal(NetworkLabels::valid_ssid(" Campus ").value_or(""), std::string(" Campus "), "exact SSID whitespace preserved");
    t.check(!NetworkLabels::valid_ssid("Campus\n"), "unsafe SSID rejected rather than changed");
    t.check(!NetworkLabels::valid_ssid(std::string(33, 'x')), "SSID length cap is bytes");
    t.check(!NetworkLabels::valid_ssid(std::string("\xFF")), "SSID must be valid UTF8");
    t.check(!NetworkLabels::decode("{\"schema\":2,\"labels\":{}}"), "future schema unavailable");
    t.check(!NetworkLabels::decode("broken"), "corrupt data unavailable");
    t.check(!NetworkLabels::decode(std::string(65537, ' ')), "oversized sidecar unavailable");
    const auto restored = NetworkLabels::decode(labels.encode());
    t.check(restored && restored->entries() == labels.entries(), "round trip preserves labels without trust data");
    t.check(labels.retain({one}) && !labels.entries().count(two), "forget prunes only forgotten metadata");
    t.check(!labels.retain({one}), "repeated prune is idempotent");
    // The presentation getter cannot label every old record from one current attachment.
    memtest::Rig rig(false);
    NetworkMemory memory("00112233445566778899aabbccddeeff");
    const auto dorm = mem_desc("a4:5e:60:11:22:33");
    const auto home = mem_desc("c0:ff:ee:11:22:33", "192.168.1.1", "192.168.1.0/24");
    memory.remember(dorm, "dormitory", true, true, rig.wall, one);
    memory.remember(home, "dormitory", true, true, rig.wall, two);
    rig.e->restore_memory(memory.encode());
    rig.path("current-dorm", {dorm});
    t.equal(rig.e->current_remembered_ids(), std::vector<std::string>{one}, "name binds only exact current remembered record");
    t.check(!rig.e->snapshot().trusted && rig.svc.submissions.empty() && rig.writes == 0, "metadata lookup grants no trust or writes");
    rig.path("multi", {dorm, home});
    t.check(rig.e->current_remembered_ids().empty(), "ambiguous multiple adapters cannot label a record");
    rig.path("unreliable", {NetworkDescriptor{}});
    t.check(rig.e->current_remembered_ids().empty(), "unreliable descriptor cannot label a record");
    rig.path("current-home", {home});
    t.equal(rig.e->current_remembered_ids(), std::vector<std::string>{two}, "second known attachment labels its own record");
    rig.e->set_sleeping(true);
    t.check(rig.e->current_remembered_ids().empty(), "sleep invalidates presentation binding");
    rig.e->set_sleeping(false);
    rig.path("current-home", {home});
    rig.fail_write = true;
    rig.e->forget_current_network();
    t.check(rig.e->current_remembered_ids().empty(), "forgotten record remains suppressed after write failure");
    t.check(labels.encode().find("salt") == std::string::npos && labels.encode().find("features") == std::string::npos, "sidecar contains no trust material");
}
