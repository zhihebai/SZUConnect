#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

void test_proxy_assistant(Harness& t) {
    using szu::proxyassist::patch_clash_verge_script;
    using szu::proxyassist::patch_sing_box;
    t.begin("proxy-assistant-v2rayn-rules");
    const auto v2rayn = szu::proxyassist::v2rayn_rules();
    const auto import_rules = szu::json::parse(v2rayn, {.max_depth = 8, .max_bytes = 1024});
    t.check(import_rules && import_rules->is_array() && import_rules->size() == 2,
        "v2rayN clipboard payload contains exactly two bounded routing rules");
    if (import_rules && import_rules->is_array() && import_rules->size() == 2) {
        for (std::size_t i = 0; i < 2; ++i) {
            const auto& rule = import_rules->items()[i];
            const auto* enabled = rule.find("enabled"); const auto* type = rule.find("ruleType");
            const auto* target = rule.find(i == 0 ? "domain" : "ip");
            const std::vector<std::string> keys{"remarks", "enabled", "ruleType", "outboundTag", i == 0 ? "domain" : "ip"};
            t.check(rule.is_object() && rule.size() == keys.size() &&
                    std::all_of(keys.begin(), keys.end(), [&](const auto& key) { return rule.find(key) != nullptr; }) &&
                    enabled && enabled->is_bool() && enabled->as_bool() &&
                    type && type->is_number() && type->as_number() == 1 && rule.string_at("outboundTag") == "direct",
                "import rules use enabled boolean, numeric Routing enum, fixed direct outbound and only the supported fields");
            t.check(target && target->is_array() && target->size() == 1 && target->items()[0].is_string() &&
                    target->items()[0].as_string() == (i == 0 ? "full:net.szu.edu.cn" : "172.30.255.42/32") &&
                    !rule.find(i == 0 ? "ip" : "domain"),
                "the only matches are the exact campus domain and the single dormitory host");
            t.equal(rule.string_at("remarks").value_or(""), i == 0 ? std::string("校园域名直连") : std::string("校园宿舍直连"),
                "imported rule remarks are fixed project labels");
        }
    }
    for (const auto* forbidden : {"MATCH", "subscription", "password", "credential", "token", "https://", "http://", "DOMAIN-SUFFIX"})
        t.check(v2rayn.find(forbidden) == std::string::npos, "clipboard rules contain no fallback, subscription, credentials or remote source");
    t.equal(szu::proxyassist::v2rayn_rules(), v2rayn, "repeated clipboard generation is deterministic and independent of local data");
    t.begin("proxy-assistant-scripts");
    const auto generated = szu::proxyassist::clash_verge_script();
    for (const auto& original : std::vector<std::string>{"", " \r\n\t", "// unchanged template\n",
        "function main(config) { return config; }", "function main(config, profileName) { return config; }",
        "/* default */ function /* name */ main ( config , profileName ) { return /* same object */ config } ;"}) {
        const auto patch = patch_clash_verge_script(original);
        t.check(patch.ok() && patch.changed && patch.text == generated, "Empty/default script is replaced by the owned script");
        t.equal(patch.changes, std::vector<std::string>({"rules", "dns", "tun"}), "Script preview uses only fixed change categories");
        const auto repeated = patch_clash_verge_script(patch.text);
        t.check(repeated.ok() && !repeated.changed && repeated.text == patch.text && repeated.changes.empty(), "Owned script is byte-idempotent");
    }
    for (const auto& original : std::vector<std::string>{";", "function main(config) { return config; }; evil()",
        "function main(config) { config.rules = []; return config; }", "function main(config) { return {...config}; }",
        "const main = config => config", "function main(config, name) {return config;}", "/* unterminated",
        "function main(config){return config;} // \nalert('private-value')", "\"function main(config){return config;}\"",
        "// comment\xe2\x80\xa8" "custom()", "// comment\xe2\x80\xa9" "custom()",
        "function main(config){return\nconfig;}", "function main(config){return/*\n*/config;}"}) {
        const auto patch = patch_clash_verge_script(original);
        t.check(!patch.ok() && !patch.changed && patch.error == "unsupportedScript" && patch.text == original && patch.changes.empty(),
            "Custom scripts are preserved and never wrapped or evaluated");
    }
    t.check(generated.find("DOMAIN,net.szu.edu.cn,DIRECT") != std::string::npos &&
        generated.find("IP-CIDR,172.30.255.42/32,DIRECT,no-resolve") != std::string::npos,
        "Script contains the two exact campus destinations");
    t.check(generated.find("DOMAIN-SUFFIX,szu.edu.cn") == std::string::npos && generated.find("PROCESS-NAME") == std::string::npos &&
        generated.find("198.18.") == std::string::npos && generated.find("strict-route\": false") == std::string::npos,
        "Script contains no broad process, university, fake-IP range or strict-route bypass");
    const auto big_script = std::string(szu::proxyassist::kMaxConfigBytes + 1, ' ');
    const auto too_large_script = patch_clash_verge_script(big_script);
    t.check(too_large_script.error == "tooLarge" && !too_large_script.changed && too_large_script.text == big_script,
        "Script limit returns fixed error without changing bytes");
    t.equal(patch_clash_verge_script(std::string(1, static_cast<char>(0xff))).error, std::string("invalidConfig"), "Script rejects invalid UTF-8");

    t.begin("proxy-assistant-sing-box");
    const std::string modern = R"JSON({
 "log":{"level":"warn"},
 "opaque": {"large":18446744073709551615,"decimal":1.230000,"escaped":"\u4e2d\u6587","token":"fixture-only"},
 "outbounds":[{"type":"direct","tag":"local-egress","bind_interface":"fixture-interface"},{"type":"selector","tag":"proxy","outbounds":["local-egress"]}],
 "dns":{"servers":[{"type":"fakeip","tag":"fake"},{"type":"local","tag":"os-resolver"}],"rules":[{"domain_suffix":["example.test"],"action":"route","server":"fake"}],"final":"fake"},
 "route":{"rules":[{"clash_mode":"Global","action":"route","outbound":"proxy"}],"final":"proxy","auto_detect_interface":true},
 "inbounds":[{"type":"tun","tag":"tunnel","auto_route":true,"strict_route":true,"route_exclude_address":["203.0.113.0/24"]}]
})JSON";
    const auto patch = patch_sing_box(modern);
    t.check(patch.ok() && patch.changed, "Modern fake-IP config is supported");
    t.equal(patch.changes, std::vector<std::string>({"rules", "dns", "tun"}), "Preview reports the actual three changed sections");
    t.check(patch.text.find(R"JSON("opaque": {"large":18446744073709551615,"decimal":1.230000,"escaped":"\u4e2d\u6587","token":"fixture-only"})JSON") != std::string::npos,
        "Unknown integer, decimal, escape and secret-like fields retain original bytes");
    const auto value = szu::json::parse(patch.text);
    t.check(value && value->is_object(), "Output is valid strict JSON");
    if (value) {
        const auto* route = value->find("route");
        const auto* rules = route ? route->find("rules") : nullptr;
        t.check(rules && rules->is_array() && rules->size() == 3, "Existing route rules are retained behind the two fixed rules");
        if (rules && rules->size() == 3) {
            t.equal(rules->items()[0].string_at("outbound").value_or(""), std::string("local-egress"), "Uses the actual direct outbound tag");
            t.equal(rules->items()[0].string_at("action").value_or(""), std::string("route"), "Typed schema uses the native route action");
            const auto* domains = rules->items()[0].find("domain");
            t.check(domains && domains->size() == 1 && domains->items()[0].as_string() == "net.szu.edu.cn", "Domain match is exact, not a university suffix");
            const auto* cidrs = rules->items()[1].find("ip_cidr");
            t.check(cidrs && cidrs->size() == 1 && cidrs->items()[0].as_string() == "172.30.255.42/32", "IP rule is a single host route");
            t.equal(rules->items()[2].string_at("outbound").value_or(""), std::string("proxy"), "Other routing decisions are preserved");
        }
        t.check(route && route->string_at("final").value_or("") == "proxy" && route->bool_at("auto_detect_interface").value_or(false),
            "Does not replace the global route or interface setting");
        const auto* dns = value->find("dns");
        const auto* dns_rules = dns ? dns->find("rules") : nullptr;
        t.check(dns && dns->string_at("final").value_or("") == "fake" && dns_rules && dns_rules->size() == 2,
            "Global DNS and existing rules are retained");
        if (dns_rules && dns_rules->size() == 2) {
            t.equal(dns_rules->items()[0].string_at("server").value_or(""), std::string("os-resolver"), "Fake-IP exception uses the existing real resolver tag");
            t.equal(dns_rules->items()[0].string_at("action").value_or(""), std::string("route"), "Typed DNS uses the native route action");
        }
        const auto* inbounds = value->find("inbounds");
        if (inbounds && inbounds->size() == 1) {
            const auto& tun = inbounds->items()[0];
            const auto* exclusions = tun.find("route_exclude_address");
            t.check(tun.bool_at("strict_route").value_or(false) && exclusions && exclusions->size() == 2,
                "TUN retains strict routing and the existing exclusions");
            if (exclusions && exclusions->size() == 2) {
                t.equal(exclusions->items()[0].as_string(), std::string("203.0.113.0/24"), "Existing TUN exclusion remains first");
                t.equal(exclusions->items()[1].as_string(), std::string("172.30.255.42/32"), "Only the exact dormitory address is added");
            }
        }
    }
    const auto repeated = patch_sing_box(patch.text);
    t.check(repeated.ok() && !repeated.changed && repeated.text == patch.text && repeated.changes.empty(), "Second sing-box patch is byte-idempotent");

    const auto legacy = patch_sing_box(R"JSON({"outbounds":[{"type":"direct","tag":"not-called-direct"}],"dns":{"servers":[{"address":"fakeip","tag":"fake"},{"address":"local","tag":"system-dns"}],"fakeip":{"enabled":true},"rules":[]},"route":{"rules":[]}})JSON");
    t.check(legacy.ok() && legacy.changed && legacy.text.find("\"action\"") == std::string::npos, "Legacy DNS keeps the legacy action-free syntax");
    t.check(legacy.text.find("\"outbound\":\"not-called-direct\"") != std::string::npos && legacy.text.find("\"server\":\"system-dns\"") != std::string::npos,
        "Legacy schema also uses the actual outbound and DNS tags");
    t.check(patch_sing_box(legacy.text).ok() && !patch_sing_box(legacy.text).changed, "Legacy result is idempotent");

    for (const auto& original : std::vector<std::string>{
        R"JSON({"outbounds":[{"type":"direct","tag":"出口"}]})JSON",
        R"JSON({"outbounds":[{"type":"direct","tag":"real-egress"}],"route":{"final":"real-egress"}})JSON",
        R"JSON({"outbounds":[{"type":"direct","tag":"real-egress"}],"route":{"rules":[]}})JSON",
        R"JSON({"outbounds":[{"type":"direct","tag":"real-egress"}],"dns":{"servers":[{"type":"local"}]}})JSON",
        R"JSON({"outbounds":[{"type":"direct","tag":"real-egress"}],"dns":{"servers":[{"type":"","address":"fakeip","tag":"f"},{"type":"","address":"local","tag":"l"}]}})JSON",
        R"JSON({"outbounds":[{"type":"direct","tag":"real-egress"}],"dns":{"servers":[{"type":"https","tag":"remote","server":"resolver.example.test"}]}})JSON",
        R"JSON({"outbounds":[{"type":"direct","tag":"real-egress"}],"dns":{"servers":[{"type":"fakeip","tag":"fake"},{"type":"dhcp","tag":"dhcp-dns","interface":"fixture"}]},"inbounds":[{"type":"tun","auto_route":true,"strict_route":true}]})JSON",
        R"JSON({"outbounds":[{"type":"direct","tag":"real-egress"}],"inbounds":[{"type":"tun","auto_route":false,"strict_route":true}]})JSON"
    }) {
        const auto changed = patch_sing_box(original);
        t.check(changed.ok() && changed.changed && szu::json::parse(changed.text).has_value(), "Supported schema adds precise rules without unrelated replacements");
        const auto again = patch_sing_box(changed.text);
        t.check(again.ok() && !again.changed && again.text == changed.text, "Supported schema remains idempotent");
    }

    const std::string existing = R"JSON({"outbounds":[{"type":"direct","tag":"exit"}],"route":{"rules":[{"domain":["net.szu.edu.cn"],"outbound":"exit"},{"ip_cidr":["172.30.255.42/32"],"outbound":"exit"},{"domain":["net.szu.edu.cn"],"outbound":"exit","network":"tcp"},{"domain":"net.szu.edu.cn","outbound":"exit"},{"ip_cidr":"172.30.255.42/32","outbound":"exit"}]}})JSON";
    const auto dedup = patch_sing_box(existing);
    const auto dedup_json = szu::json::parse(dedup.text);
    const auto* dedup_route = dedup_json ? dedup_json->find("route") : nullptr;
    const auto* dedup_rules = dedup_route ? dedup_route->find("rules") : nullptr;
    t.check(dedup.ok() && dedup.changed && dedup_rules && dedup_rules->size() == 3, "Exact owned duplicates are removed while constrained rules remain");
    t.check(dedup.text.find("\"network\":\"tcp\"") != std::string::npos, "A conditional user rule is never mistaken for an owned duplicate");

    t.begin("proxy-assistant-refusals");
    struct Refusal { const char* input; const char* error; };
    const std::vector<Refusal> refusals{
        {"", "invalidConfig"}, {"[]", "invalidConfig"}, {"{}", "unsupportedOutbound"},
        {R"JSON({"outbounds":[]})JSON", "unsupportedOutbound"},
        {R"JSON({"outbounds":[{"type":"direct"}]})JSON", "unsupportedOutbound"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"},{"type":"direct","tag":"b"}]})JSON", "unsupportedOutbound"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"},{"type":"block","tag":"a"}]})JSON", "unsupportedOutbound"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a","detour":"proxy"}]})JSON", "unsupportedOutbound"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a","override_address":"example.test"}]})JSON", "unsupportedOutbound"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a","override_port":443}]})JSON", "unsupportedOutbound"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a\nb"}]})JSON", "invalidConfig"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"route":[]})JSON", "invalidConfig"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"route":{"rules":{}}})JSON", "invalidConfig"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"route":{"rules":["not-a-rule"]}})JSON", "invalidConfig"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"dns":[]})JSON", "unsupportedDns"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"dns":{"servers":{}}})JSON", "unsupportedDns"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"dns":{"servers":[{"type":"fakeip","tag":"f"}]}})JSON", "unsupportedDns"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"dns":{"servers":[{"type":"future-unknown-dns","tag":"x"}]}})JSON", "unsupportedDns"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"dns":{"servers":[{"type":"fakeip","tag":"f"},{"type":"local"}]}})JSON", "unsupportedDns"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"dns":{"servers":[{"type":"fakeip","tag":"f"},{"type":"local","tag":"l1"},{"type":"local","tag":"l2"}]}})JSON", "unsupportedDns"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"dns":{"servers":[{"type":"fakeip","tag":"f"},{"address":"local","tag":"l"}]}})JSON", "unsupportedDns"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"dns":{"servers":[{"type":"local","address":"local","tag":"l"}]}})JSON", "unsupportedDns"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"dns":{"servers":[{"type":"fakeip","tag":"f"},{"type":"local","tag":"f"}]}})JSON", "unsupportedDns"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"dns":{"servers":[{"type":"fakeip","tag":"f"},{"type":"local","tag":"l","detour":"proxy"}]}})JSON", "unsupportedDns"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"dns":{"fakeip":{"enabled":"true"}}})JSON", "unsupportedDns"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"inbounds":"tun"})JSON", "invalidConfig"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"inbounds":[{"type":"tun","auto_route":true}]})JSON", "unsupportedTun"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"inbounds":[{"type":"tun","auto_route":"true"}]})JSON", "unsupportedTun"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"inbounds":[{"type":"tun","auto_route":true,"inet4_route_exclude_address":[]}]})JSON", "unsupportedTun"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"inbounds":[{"type":"tun","auto_route":true,"route_exclude_address":[0]}]})JSON", "unsupportedTun"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"route":{},"route":{}})JSON", "invalidConfig"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"route":{},"r\u006fute":{}})JSON", "invalidConfig"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],"unknown":{"secret":"a","secret":"b"}})JSON", "invalidConfig"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}],})JSON", "invalidConfig"},
        {R"JSON({/* comment */"outbounds":[{"type":"direct","tag":"a"}]})JSON", "invalidConfig"},
        {R"JSON({"outbounds":[{"type":"direct","tag":"a"}]};alert("fixture"))JSON", "invalidConfig"}
    };
    for (const auto& item : refusals) {
        const auto rejected = patch_sing_box(item.input);
        t.check(!rejected.ok() && !rejected.changed && rejected.error == item.error && rejected.text == item.input && rejected.changes.empty(),
            "Unsupported or ambiguous config is preserved with a fixed error token");
    }
    const auto giant = std::string(szu::proxyassist::kMaxConfigBytes + 1, ' ');
    const auto huge = patch_sing_box(giant);
    t.check(huge.error == "tooLarge" && huge.text == giant && !huge.changed, "Config input size is bounded before parsing");
    std::string nested = "{\"outbounds\":[{\"type\":\"direct\",\"tag\":\"a\"}],\"nested\":";
    nested += std::string(60, '[') + "0" + std::string(60, ']') + "}";
    t.equal(patch_sing_box(nested).error, std::string("invalidConfig"), "Excessive nesting is rejected");
    t.equal(patch_sing_box(std::string("{\"x\":\"") + static_cast<char>(0xff) + "\"}").error,
        std::string("invalidConfig"), "Invalid Unicode is rejected");
    std::string too_many = "{\"outbounds\":[{\"type\":\"direct\",\"tag\":\"a\"}],\"items\":[0";
    for (int i = 0; i < 33000; ++i) too_many += ",0";
    too_many += "]}";
    t.equal(patch_sing_box(too_many).error, std::string("invalidConfig"), "Config node count is bounded");
}
