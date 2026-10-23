import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

// This test executes only the project's generated script with synthetic config objects.
// --script-file accepts output from the native script exporter; the source fallback
// lets portable test jobs exercise the same raw-string body without Windows tools.
let body;
if (process.argv[2] === '--script-file' && process.argv[3]) {
  body = readFileSync(process.argv[3], 'utf8');
} else {
  const source = readFileSync(new URL('../core/src/proxy_assistant.cpp', import.meta.url), 'utf8');
  const literal = source.match(/R"JS\(([\s\S]*?)\)JS";/);
  assert.ok(literal, 'generated script raw string exists');
  body = literal[1];
}
assert.match(body, /^\/\/ SZUConnect campus direct configuration v1\r?\n/);
const main = new Function(`${body}\nreturn main;`)();
const domain = 'DOMAIN,net.szu.edu.cn,DIRECT';
const address = 'IP-CIDR,172.30.255.42/32,DIRECT,no-resolve';
let checks = 0;
function check(condition, description) { ++checks; assert.ok(condition, description); }
function deepFreeze(value) {
  if (value && typeof value === 'object') {
    for (const child of Object.values(value)) deepFreeze(child);
    Object.freeze(value);
  }
  return value;
}
function transform(input) {
  const before = JSON.stringify(input);
  deepFreeze(input);
  const output = main(input, 'synthetic-profile');
  check(JSON.stringify(input) === before, 'does not mutate the input');
  check(JSON.stringify(main(output, 'synthetic-profile')) === JSON.stringify(output), 'second transform is idempotent');
  return output;
}

const config = {
  mode: 'rule',
  rules: ['MATCH,fixture-proxy', domain, domain, address],
  dns: {
    enable: true, 'enhanced-mode': 'fake-ip',
    'fake-ip-filter': ['fixture.example.test', 'net.szu.edu.cn', 'net.szu.edu.cn'],
    nameserver: ['https://resolver.example.test/dns-query'],
    'nameserver-policy': { 'other.example.test': 'https://other-resolver.example.test/dns-query' },
  },
  tun: { enable: true, 'auto-route': true, 'strict-route': true, 'route-exclude-address': ['203.0.113.0/24', '172.30.255.42/32', '172.30.255.42/32'] },
  proxies: [{ name: 'fixture-proxy', type: 'socks5', server: 'fixture.example.test', password: 'not-a-real-password' }],
  unknown: { numericId: '18446744073709551615', nested: [true, null, '中文'] },
};
const output = transform(config);
check(output.rules.join('|') === [domain, address, 'MATCH,fixture-proxy'].join('|'), 'exact campus rules precede the original catch-all');
check(output.dns['fake-ip-filter'].join('|') === 'net.szu.edu.cn|fixture.example.test', 'blacklist filters one exact hostname');
check(output.dns.nameserver === config.dns.nameserver && output.dns['nameserver-policy'] === config.dns['nameserver-policy'], 'preserves global DNS and DNS policies');
check(output.tun['strict-route'] === true && output.tun['route-exclude-address'].join('|') === '203.0.113.0/24|172.30.255.42/32', 'keeps strict routing and all unrelated exclusions');
check(output.proxies === config.proxies && output.unknown === config.unknown, 'preserves proxy credentials and unknown values without inspecting them');

const ruleFilter = transform({ mode: 'rule', rules: [], dns: { 'enhanced-mode': 'fake-ip', 'fake-ip-filter-mode': 'rule', 'fake-ip-filter': ['DOMAIN-SUFFIX,example.test,fake-ip', 'DOMAIN,net.szu.edu.cn,real-ip'] } });
check(ruleFilter.dns['fake-ip-filter'].join('|') === 'DOMAIN,net.szu.edu.cn,real-ip|DOMAIN-SUFFIX,example.test,fake-ip', 'rule-mode exception precedes any later DNS rule');
const disabled = transform({ rules: [], dns: { enable: false, 'enhanced-mode': 'fake-ip', 'fake-ip-filter-mode': 'whitelist', 'fake-ip-filter': ['original.example.test'] }, tun: { enable: false, 'auto-route': true } });
check(disabled.dns['fake-ip-filter'][0] === 'original.example.test' && disabled.tun['route-exclude-address'] === undefined, 'disabled DNS and TUN are left intact');
const redirHost = transform({ rules: [], dns: { enable: true, 'enhanced-mode': 'redir-host' }, tun: { enable: true, 'auto-route': false, 'strict-route': true } });
check(redirHost.dns['fake-ip-filter'] === undefined && redirHost.tun['route-exclude-address'] === undefined, 'does not reconfigure non-fake DNS or manual TUN routing');
const defaults = transform({});
check(defaults.rules.join('|') === [domain, address].join('|') && Object.keys(defaults).length === 1, 'minimal config only gains the two rules');
const absentLists = transform({ mode: 'rule', dns: { 'enhanced-mode': 'fake-ip' }, tun: { enable: true, 'auto-route': true } });
check(absentLists.dns['fake-ip-filter'].join('|') === 'net.szu.edu.cn' && absentLists.tun['route-exclude-address'].join('|') === '172.30.255.42/32', 'creates only the necessary precise exception lists');

for (const [input, token] of [
  [null, 'invalidConfig'],
  [[], 'invalidConfig'],
  [{ mode: 'global', rules: [] }, 'unsupportedMode'],
  [{ mode: 'direct', rules: [] }, 'unsupportedMode'],
  [{ rules: 'invalid-list' }, 'invalidConfig'],
  [{ rules: [null] }, 'invalidConfig'],
  [{ rules: [], dns: [] }, 'invalidConfig'],
  [{ rules: [], dns: { enable: 'false', 'enhanced-mode': 'fake-ip' } }, 'unsupportedDns'],
  [{ rules: [], dns: { 'enhanced-mode': 'fake-ip', 'fake-ip-filter-mode': 'whitelist', 'fake-ip-filter': ['original'] } }, 'unsupportedDns'],
  [{ rules: [], dns: { 'enhanced-mode': 'fake-ip', 'fake-ip-filter-mode': '', 'fake-ip-filter': [] } }, 'unsupportedDns'],
  [{ rules: [], dns: { 'enhanced-mode': 'fake-ip', 'fake-ip-filter-mode': ['blacklist'] } }, 'unsupportedDns'],
  [{ rules: [], dns: { 'enhanced-mode': 'fake-ip', 'fake-ip-filter': {} } }, 'unsupportedDns'],
  [{ rules: [], dns: { 'enhanced-mode': 'fake-ip', 'fake-ip-filter': [false] } }, 'unsupportedDns'],
  [{ rules: [], tun: [] }, 'invalidConfig'],
  [{ rules: [], tun: { enable: 'true', 'auto-route': true } }, 'unsupportedTun'],
  [{ rules: [], tun: { enable: true, 'auto-route': 'true' } }, 'unsupportedTun'],
  [{ rules: [], tun: { enable: true, 'auto-route': true, 'route-exclude-address': 'invalid' } }, 'unsupportedTun'],
  [{ rules: [], tun: { enable: true, 'auto-route': true, 'route-exclude-address': [false] } }, 'unsupportedTun'],
]) {
  const before = JSON.stringify(input);
  deepFreeze(input);
  assert.throws(() => main(input, 'synthetic-profile'), { message: `SZUConnect: ${token}` });
  check(JSON.stringify(input) === before, 'failed transform never partially changes input');
}
check(!body.includes('PROCESS-NAME') && !body.includes('DOMAIN-SUFFIX,szu.edu.cn') && !body.includes('198.18.'), 'no broad process, university suffix or fake-IP-range bypass');
console.log(`proxy script behavior: ${checks} checks passed`);
