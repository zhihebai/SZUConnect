// Pure parsing, allow-listing and payload rules for the two campus portals.
// Shared rules with macOS PortalWire (PortalClient.swift). No I/O here:
// platform transports and tests supply response bytes and consume the results.
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "szu/json.hpp"
#include "szu/types.hpp"

namespace szu::wire {

inline constexpr std::string_view kTeachingBase = "https://net.szu.edu.cn";
inline constexpr std::string_view kDormitoryBase = "http://172.30.255.42:801";
inline constexpr std::string_view kAppleBase = "https://www.apple.com";
inline constexpr std::string_view kMicrosoftBase = "http://www.msftconnecttest.com";
inline constexpr std::size_t kMaxResponseBytes = 1024 * 1024;
inline constexpr std::size_t kMaxProbeBytes = 64 * 1024;

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::optional<int> port;
    std::string path;
    bool has_query = false;
    std::string query;
    bool has_fragment = false;
    bool has_userinfo = false;
};

std::optional<ParsedUrl> parse_url(std::string_view url);
// Only fixed public probes may supply a candidate. The Location and query are never returned.
std::optional<CampusPortalHint> campus_redirect_hint(std::string_view location, std::string_view probe_url);
bool campus_authentication_required(std::string_view location, std::string_view probe_url);
bool is_allowed(std::string_view url);
std::string percent_encode(std::string_view text);  // RFC 3986 unreserved characters pass through
using Query = std::vector<std::pair<std::string, std::string>>;
// Builds base + path (+ ?query); std::nullopt when the result is not allow-listed.
std::optional<std::string> make_url(std::string_view base, std::string_view path, const Query& query = {});

// JSON or JSONP ("_(...)" / "jsonpReturn(...)") object; std::nullopt means PortalError::response.
std::optional<json::Value> object_from(std::string_view body);
bool teaching_success(const json::Value& object);
bool dormitory_success(const json::Value& object);
enum class LoginVerdict { accepted, rejected, uncertain };
LoginVerdict teaching_verdict(const json::Value& object);
LoginVerdict dormitory_verdict(const json::Value& object);
bool is_dormitory_success_message(std::string_view message);
RejectionKind rejection_kind(const json::Value& object);

struct AcIdResult {
    std::string value;                    // Present only for one explicit, valid controller id
    PortalError error = PortalError::none;  // responseTooLarge | response | protocolConflict
};
AcIdResult ac_id(std::string_view html);

struct ClientConfiguration {
    std::string ac_id, ip;  // RAM-only literals; never stored or exported.
};
struct ClientConfigurationResult {
    std::optional<ClientConfiguration> value;
    bool present = false;
    PortalError error = PortalError::none;
};
// Reads one inline literal CONFIG without evaluating JavaScript. The caller must
// still require independent TLS campus identity and match the exact bound source.
ClientConfigurationResult client_configuration(std::string_view html, std::string_view controller);

struct TeachingPageResult {
    AcIdResult controller;
    ClientConfigurationResult configuration;  // Parsed only when the controller is valid.
};
// Validates both fields from this exact response, without scanning its controller twice.
TeachingPageResult teaching_page(std::string_view html);

struct ChallengeResult {
    std::string challenge;
    std::string ip;
    PortalError error = PortalError::none;  // challengeRefused (no "ok") | protocolConflict
};
ChallengeResult challenge(const json::Value& object);

bool is_connectivity_success(std::string_view body, int status);
bool is_microsoft_success(std::string_view body, int status);
bool is_campus_homepage(std::string_view body, int status, std::string_view url);
bool is_dormitory_homepage(std::string_view body, int status, std::string_view url, bool campus_confirmed);

bool is_transient(PortalError error);
bool should_fallback(PortalError error);
// Errors after a credential-bearing submission started cannot prove the server did nothing.
PortalError after_submission(PortalError error);
// Classifies a PortalError into the engine's outcome vocabulary.
Outcome outcome_for(PortalError error, bool submitted);

}  // namespace szu::wire
