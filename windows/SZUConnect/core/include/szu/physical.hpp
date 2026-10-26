// RAM-only authority for one independently verified physical IPv4 attachment.
// Never persist these fields or use display names as network identity.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "szu/netmemory.hpp"

namespace szu::physical {

enum class CandidateRejection {
    systemSnapshotUnavailable, duplicateInterface, unsupportedInterface, inactiveLink,
    missingInterfaceIndex, missingSourceAddress, invalidSourceAddress, invalidSubnet,
    invalidGateway, missingDNS, invalidDNS, missingDescriptor, missingDefaultRoute,
    conflictingDefaultRoutes, gatewayMismatch, associatedDNSMismatch,
    invalidDHCPServer, unstableSnapshot, noIPv4Service
};

struct Route {
    std::string interface_key;
    std::uint32_t interface_index = 0;
    std::uint64_t interface_luid = 0;
    std::string source_ipv4;
    std::vector<std::string> addresses;
    std::vector<std::string> dns_servers;
    std::string gateway_ipv4;
    NetworkDescriptor descriptor;
    std::string identity;
    std::string display_name;
    bool is_system_primary = false;
    bool operator==(const Route& other) const;
};

// Selected-interface inputs only. Extra interfaces, TUN routes, route metrics and
// system-primary preferences cannot revoke an already source-bound operation.
struct Attachment {
    Route route;
    bool physical = false;
    bool up = false;
    std::vector<std::string> bound_default_gateways;
    std::optional<CandidateRejection> collection_failure;
};

bool usable_ipv4(std::string_view value);
bool source_in_network(std::string_view source, std::string_view network);
// Fixed anonymous counters only. Raw adapter records remain in Route and never
// enter exported diagnostics, including failures on an unrelated interface.
std::string_view to_string(CandidateRejection reason);
struct CandidateDiagnostics {
    std::size_t interface_count = 0, candidate_count = 0;
    std::map<CandidateRejection, unsigned> rejections;
    void reject(CandidateRejection reason, unsigned count = 1);
};
// Export boundary: discard unknown reasons and zero entries, clamp all counters.
CandidateDiagnostics anonymous_diagnostics(const CandidateDiagnostics& diagnostics);
struct CandidateReport {
    std::vector<Route> routes;
    CandidateDiagnostics diagnostics;
};
CandidateReport candidate_report(const std::vector<Attachment>& evidence);
CandidateReport stable_candidate_report(const std::vector<Attachment>& first, const std::vector<Attachment>& second);
std::vector<Route> candidates(const std::vector<Attachment>& evidence);
std::vector<Route> stable_candidates(const std::vector<Attachment>& first, const std::vector<Attachment>& second);

// Positive OS hardware evidence is required before an adapter can become a
// campus-authentication candidate. An Ethernet IfType or name alone is insufficient.
struct HardwareEvidence {
    std::uint64_t expected_luid = 0, actual_luid = 0;
    std::uint32_t expected_index = 0, actual_index = 0;
    bool readable = false, up = false, hardware_interface = false, filter_interface = false, endpoint_interface = false;
};
bool physical_hardware_confirmed(const HardwareEvidence& evidence);

// A separate Wi-Fi takeover gate for WWAN/other attachments. It never proves
// campus identity and cannot create a Route or a remembered-network eligibility.
struct OtherLinkEvidence {
    bool up = false;
    bool known_virtual = false;
    bool usable_source = false;
    bool details_readable = false;
    bool hardware_interface = false;
    bool connector_present = false;
    bool media_connected = false;
    bool media_disconnected = false;
};
bool other_physical_link_active(const OtherLinkEvidence& evidence);

enum class Failure { cancelled, timeout, unavailable, invalidResponse, security };
struct Error : std::runtime_error {
    explicit Error(Failure value);
    Failure reason;
};

using Packet = std::vector<std::uint8_t>;
bool allowed_host(std::string_view host);
Packet dns_query(std::string_view host, std::uint16_t transaction);
std::vector<std::string> dns_parse(const Packet& packet, std::string_view host, std::uint16_t transaction);
struct ResolvedAddresses {
    std::vector<std::string> addresses;
    double ttl_seconds = 0;
};
// Preserve a validated DNS lifetime until HTTP Age is deducted; the resolver caps reuse.
double dns_response_ttl(const Packet& packet, std::string_view host, std::uint16_t transaction);

struct ExchangeEvent {
    enum class Kind { packet, failed };
    Kind kind = Kind::packet;
    std::size_t server = 0;
    Packet data;
};
struct ExchangeHooks {
    std::function<double()> now;  // monotonic seconds
    std::function<bool(std::size_t, const Packet&)> send;
    std::function<std::vector<ExchangeEvent>(const std::vector<std::size_t>&, double)> receive;
    std::function<bool()> cancelled;
};
// One common deadline for all associated resolvers; initial send plus bounded
// retransmissions at 250/750 ms. No public/system DNS or TCP fallback.
std::vector<std::string> dns_exchange(std::string_view host, std::uint16_t transaction,
                                    std::size_t servers, double deadline, const ExchangeHooks& hooks);
ResolvedAddresses dns_exchange_with_ttl(std::string_view host, std::uint16_t transaction,
                                        std::size_t servers, double deadline, const ExchangeHooks& hooks);

}  // namespace szu::physical
