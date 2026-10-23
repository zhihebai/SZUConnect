#include "szu/win/proxy_yaml.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {
int checks = 0, failures = 0;
void check(bool passed, const char* message) {
    ++checks;
    if (!passed) { ++failures; std::printf("FAIL: %s\n", message); }
}

bool same(const YAML::Node& left, const YAML::Node& right) {
    if (left.Type() != right.Type()) return false;
    if (left.IsScalar()) return left.Scalar() == right.Scalar();
    if (left.IsNull()) return true;
    if (left.size() != right.size()) return false;
    if (left.IsSequence()) {
        for (std::size_t i = 0; i < left.size(); ++i) if (!same(left[i], right[i])) return false;
    } else if (left.IsMap()) {
        for (const auto& item : left) {
            const auto other = right[item.first.Scalar()];
            if (!other.IsDefined() || !same(item.second, other)) return false;
        }
    }
    return true;
}

void rejected(std::string_view input, std::string_view expected = "invalidConfig") {
    const auto result = szu::win::patch_mihomo_yaml(input);
    check(!result.ok() && result.error == expected, "invalid input returns its fixed category");
    check(result.text.empty() && result.changes.empty() && !result.changed,
          "rejected input exposes no configuration or partial changes");
    check(result.error.find("PRIVATE_SENTINEL") == std::string::npos,
          "parser diagnostics never contain supplied values");
}

void idempotent(const std::string& input) {
    const auto once = szu::win::patch_mihomo_yaml(input);
    check(once.ok(), "first transformation succeeds");
    if (!once.ok()) return;
    const auto twice = szu::win::patch_mihomo_yaml(once.text);
    check(twice.ok() && !twice.changed && twice.changes.empty() && twice.text == once.text,
          "second transformation preserves exact bytes without reporting changes");
}
}  // namespace

int main() {
    {
        const std::string input = R"(mode: rule
mixed-port: 7890
secret: PRIVATE_SENTINEL
proxy-providers:
  example: {type: http, url: 'https://example.invalid/subscription?key=PRIVATE_SENTINEL', interval: 3600}
proxies:
  - {name: '中文节点', type: socks5, server: 127.0.0.1, port: 10001, password: PRIVATE_SENTINEL}
rule-providers: {private: {type: file, path: ./private.yaml}}
rules:
  - DOMAIN,example.invalid,Existing
  - DOMAIN,net.szu.edu.cn,DIRECT
  - MATCH,Existing
  - IP-CIDR,172.30.255.42/32,DIRECT,no-resolve
x-future: {enabled: true, words: ['false', 'on', 'null', '001'], nested: {中文: 保留}}
dns:
  enhanced-mode: fake-ip
  nameserver: [system, 'https://example.invalid/dns-query']
  nameserver-policy: {'+.example.invalid': [system]}
  fallback-filter: {geoip: true, ipcidr: [240.0.0.0/4]}
  fake-ip-filter: ['*.lan', localhost]
tun:
  enable: true
  auto-route: true
  strict-route: true
  stack: mixed
  route-exclude-address: [192.0.2.0/24]
)";
        const auto result = szu::win::patch_mihomo_yaml(input);
        check(result.ok() && result.changed && result.changes == std::vector<std::string>{"rules", "dns", "tun"},
              "preview lists only the three fixed change categories");
        if (result.ok()) {
            const auto before = YAML::Load(input), after = YAML::Load(result.text);
            check(after["rules"].size() == 4 &&
                  after["rules"][0].Scalar() == "DOMAIN,net.szu.edu.cn,DIRECT" &&
                  after["rules"][1].Scalar() == "IP-CIDR,172.30.255.42/32,DIRECT,no-resolve" &&
                  after["rules"][2].Scalar() == "DOMAIN,example.invalid,Existing" &&
                  after["rules"][3].Scalar() == "MATCH,Existing",
                  "campus rules precede existing rules, deduplicate and preserve remaining order");
            for (const char* key : {"mixed-port", "secret", "proxy-providers", "proxies", "rule-providers", "x-future"})
                check(same(before[key], after[key]), "unrelated configuration retains parsed semantics");
            for (const auto& word : after["x-future"]["words"])
                check(word.Tag() == "!", "quoted boolean and number strings remain quoted strings");
            for (const char* key : {"nameserver", "nameserver-policy", "fallback-filter"})
                check(same(before["dns"][key], after["dns"][key]), "existing DNS policies and resolvers are retained");
            check(after["dns"]["fake-ip-filter"].size() == 3 &&
                  after["dns"]["fake-ip-filter"][2].Scalar() == "net.szu.edu.cn",
                  "default blacklist appends only the exact campus hostname");
            check(after["tun"]["strict-route"].as<bool>() &&
                  after["tun"]["route-exclude-address"].size() == 2 &&
                  after["tun"]["route-exclude-address"][1].Scalar() == "172.30.255.42/32",
                  "TUN adds only the fixed host route and preserves strict route");
            check(result.error.empty() && std::none_of(result.changes.begin(), result.changes.end(), [](const auto& value) {
                      return value.find("PRIVATE_SENTINEL") != std::string::npos;
                  }), "preview and error tokens never contain configuration secrets");
        }
        idempotent(input);
    }
    {
        const std::string input = R"(# Keep this exact formatting when already configured.
mode: rule
rules: ['DOMAIN,net.szu.edu.cn,DIRECT', 'IP-CIDR,172.30.255.42/32,DIRECT,no-resolve', 'MATCH,Existing']
dns: {enhanced-mode: fake-ip, fake-ip-filter-mode: blacklist, fake-ip-filter: [net.szu.edu.cn]}
tun: {enable: true, auto-route: true, route-exclude-address: [172.30.255.42/32], strict-route: false}
)";
        const auto result = szu::win::patch_mihomo_yaml(input);
        check(result.ok() && !result.changed && result.text == input && result.changes.empty(),
              "already configured YAML retains comments and original bytes");
    }
    {
        const std::string input = R"(rules: ['MATCH,Existing']
dns:
  enhanced-mode: fake-ip
  fake-ip-filter-mode: rule
  fake-ip-filter: ['DOMAIN-SUFFIX,szu.edu.cn,fake-ip', 'DOMAIN,net.szu.edu.cn,real-ip']
)";
        const auto result = szu::win::patch_mihomo_yaml(input);
        check(result.ok(), "rule-style fake-IP filters are supported");
        if (result.ok()) {
            const auto after = YAML::Load(result.text);
            check(after["dns"]["fake-ip-filter"].size() == 2 &&
                  after["dns"]["fake-ip-filter"][0].Scalar() == "DOMAIN,net.szu.edu.cn,real-ip" &&
                  after["dns"]["fake-ip-filter"][1].Scalar() == "DOMAIN-SUFFIX,szu.edu.cn,fake-ip",
                  "real-IP exception has priority without removing other filter rules");
        }
        idempotent(input);
    }
    {
        const auto no_tun = szu::win::patch_mihomo_yaml("mode: rule\ndns: {enhanced-mode: redir-host}\n");
        check(no_tun.ok() && no_tun.changes == std::vector<std::string>{"rules"},
              "missing optional DNS and TUN features are not enabled");
        if (no_tun.ok()) {
            const auto after = YAML::Load(no_tun.text);
            check(!after["tun"].IsDefined() && after["dns"]["enhanced-mode"].Scalar() == "redir-host",
                  "route patch does not add TUN or change DNS modes");
        }
        for (const char* body : {"tun: {enable: false, auto-route: true}\n", "tun: {enable: true, auto-route: false}\n"}) {
            const auto patch = szu::win::patch_mihomo_yaml(body);
            check(patch.ok() && patch.changes == std::vector<std::string>{"rules"},
                  "disabled TUN or auto-route does not acquire route exclusions");
        }
        const std::string disabled_dns = "dns: {enable: false, enhanced-mode: fake-ip, fake-ip-filter-mode: whitelist, fake-ip-filter: [example.invalid]}\n";
        const auto disabled = szu::win::patch_mihomo_yaml(disabled_dns);
        check(disabled.ok() && disabled.changes == std::vector<std::string>{"rules"} &&
              same(YAML::Load(disabled_dns)["dns"], YAML::Load(disabled.text)["dns"]),
              "disabled DNS retains its settings without editing or rejecting an inactive filter mode");
        idempotent("{}\n");
        idempotent("---\nmode: Rule\nrules: []\n...\n");
        idempotent("rules: [ 'DOMAIN, NET.SZU.EDU.CN, DIRECT', 'IP-CIDR, 172.30.255.42/32, DIRECT, no-resolve', 'MATCH,Existing']\n");
    }

    for (const char* invalid : {
             "", "null\n", "[]\n", "PRIVATE_SENTINEL\n", "---\n{}\n---\n{}\n",
             "mode: rule\nmode: global\n", "dns: {enhanced-mode: fake-ip, enhanced-mode: redir-host}\n",
             "mode: rule\n\"\\u006dode\": global\n",
             "x: {a: 1, 'a': 2}\n", "? [a,b]\n: PRIVATE_SENTINEL\n", "? {a: 1}\n: value\n",
             "? null\n: PRIVATE_SENTINEL\n", "x: !PRIVATE_SENTINEL value\n",
             "x: &anchor PRIVATE_SENTINEL\n", "x: &anchor {nested: value}\ny: *anchor\n",
             "x: &anchor [*anchor]\n", "<<: {mode: global}\n", "x: {<<: {nested: value}}\n",
             "rules: null\n", "rules: MATCH,Existing\n", "rules: [{rule: value}]\n",
             "tun: {enable: true, auto-route: true, route-exclude-address: invalid}\n",
             "tun: {enable: PRIVATE_SENTINEL, auto-route: true}\n", "x: [PRIVATE_SENTINEL\n"
         }) rejected(invalid);
    for (const char* mode : {"global", "direct", "script", "PRIVATE_SENTINEL"})
        rejected(std::string("mode: ") + mode + "\n", "unsupportedMode");
    for (const char* dns : {
             "dns: PRIVATE_SENTINEL\n", "dns: {enhanced-mode: [fake-ip]}\n",
             "dns: {enable: PRIVATE_SENTINEL, enhanced-mode: fake-ip}\n",
             "dns: {enhanced-mode: fake-ip, fake-ip-filter-mode: whitelist}\n",
             "dns: {enhanced-mode: fake-ip, fake-ip-filter-mode: PRIVATE_SENTINEL}\n",
             "dns: {enhanced-mode: fake-ip, fake-ip-filter: [null]}\n",
             "dns: {enhanced-mode: fake-ip, fake-ip-filter: PRIVATE_SENTINEL}\n"
         }) rejected(dns, "unsupportedDns");

    rejected(std::string("x: ") + std::string(1, '\0') + "PRIVATE_SENTINEL");
    rejected(std::string("x: ") + "\xc0\xaf");
    rejected(std::string("x: ") + "\xed\xa0\x80");
    rejected(std::string("x: ") + "\xf4\x90\x80\x80");
    rejected(std::string(szu::proxyassist::kMaxConfigBytes + 1, ' '), "tooLarge");
    std::string deep = "x: ";
    for (int i = 0; i < 60; ++i) deep += '[';
    deep += "PRIVATE_SENTINEL";
    for (int i = 0; i < 60; ++i) deep += ']';
    rejected(deep);
    std::string wide = "x: [";
    for (int i = 0; i < 16385; ++i) wide += (i ? ",0" : "0");
    wide += "]\n";
    rejected(wide);
    std::string mapping;
    for (int i = 0; i < 16385; ++i) mapping += "k" + std::to_string(i) + ": 0\n";
    rejected(mapping);
    std::string many_nodes = "x: [";
    for (int i = 0; i < 16384; ++i) many_nodes += (i ? ",[0,0,0]" : "[0,0,0]");
    many_nodes += "]\n";
    rejected(many_nodes);

    std::printf("Mihomo YAML transforms: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
