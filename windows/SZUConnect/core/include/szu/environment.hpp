// Read-only network-environment interpretation: website-check addresses, system proxy summary,
// compatibility diagnosis and tunnel-route evidence. Nothing here changes system settings.
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace szu {

namespace website {
struct Normalized {
    std::string url;    // normalized absolute URL when ok()
    std::string error;  // empty | empty | tooLong | unsupportedScheme | sensitiveComponents | invalid
    bool ok() const { return error.empty(); }
};
// Mirrors macOS WebsiteAddress.normalize and tools/reference/website_reference.py.
Normalized normalize(std::string_view input);
// Header-only check result for an HTTP status code: accessible | redirected | authenticationRequired | forbidden | httpStatus
std::string_view classify_status(int code);
// Portal-homepage-only redirect candidate. No destination or query is retained.
bool is_campus_redirect(std::string_view location, std::string_view request_url);
}  // namespace website

namespace proxy {
inline constexpr std::string_view kCampusHosts[] = {"net.szu.edu.cn", "172.30.255.42"};

struct Snapshot {
    bool readable = false;
    bool http = false, https = false, socks = false, pac = false, auto_discovery = false;
    std::vector<std::string> exceptions;
};
struct Summary {
    std::string state;                // unknown | off | on
    std::vector<std::string> kinds;   // http https socks pac autoDiscovery
    std::string campus_bypass;        // unknown | notApplicable | all | partial | none
};
struct WindowsRaw {  // WinHttpGetIEProxyConfigForCurrentUser fields
    bool readable = true;
    std::string proxy;
    std::string bypass;
    std::string auto_config_url;
    bool auto_detect = false;
};

bool pattern_matches(std::string_view pattern, std::string_view host);
Summary summarize(const Snapshot& snapshot);
Snapshot parse_windows(const WindowsRaw& raw);
}  // namespace proxy

namespace compat {
// Result tokens: reachable | tls | dns | timeout | network | cancelled | notRun
struct Input {
    std::string proxy = "unknown";   // on | off | unknown
    std::string tunnel = "unknown";  // detected | notDetected | unknown
    std::string campus_direct = "notRun";
    std::string campus_system = "notRun";
    std::string public_direct = "notRun";
    std::string public_system = "notRun";
};
struct Verdict {
    std::string verdict;
    std::vector<std::string> recommendations;
    bool offer_rules = false;
};
Verdict diagnose(const Input& input);
inline constexpr std::string_view kDirectRules[] = {"- DOMAIN,net.szu.edu.cn,DIRECT", "- IP-CIDR,172.30.255.42/32,DIRECT,no-resolve"};
std::string rules_text();  // the two rules joined with "\n"
}  // namespace compat

namespace routes {
struct Interface {
    std::string name;
    bool up = false;
    bool running = false;
    std::string kind;  // physical | tunnel | loopback | other
};
struct Route {
    int family = 4;
    std::string destination;  // textual address
    int prefix = 0;
    std::string interface_name;
    std::vector<std::string> flags;  // shared flag vocabulary (see route-classification-cases.json)
};
struct Evidence {
    bool tunnel = false;
    int count = 0;
};
// view: "mac" (routing socket dump) or "windows" (GetIpForwardTable2 rows)
Evidence classify(const std::vector<Route>& routes, const std::vector<Interface>& interfaces, std::string_view view);
bool is_tunnel_name(std::string_view name);
}  // namespace routes

}  // namespace szu
