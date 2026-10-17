// Network memory: a revocable record of explicit approval for one campus network.
//
// Stores keyed hashes (HMAC-SHA256 with a per-install random salt, truncated to 128 bits) of
// stable network features: gateway hardware id, gateway address, subnet, DHCP server, DNS set, DNS suffix.
// Never stores, logs, exports or sends SSID, client IP, raw feature values or credentials.
// A different gateway at a remembered address or subnet is a conflict; elsewhere it is another network.
// A remembered network is only an eligibility: every recovery still needs a fresh proof of the campus
// service (see memory_verdict) and never overrides holds, cooldown or the one-submission rule.
// Mirrored 1:1 by macOS NetworkMemory.swift and tools/reference/memory_reference.py and verified against
// shared/fixtures/network-memory-cases.json.
#pragma once
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "szu/types.hpp"

namespace szu {

std::string sha256_hex(std::string_view data);
std::string hmac_sha256_hex(std::string_view key, std::string_view data);

enum class MatchLevel { none, weak, medium, strong, conflict };
std::string_view to_string(MatchLevel value);
std::optional<MatchLevel> match_level_from(std::string_view text);

// Raw, RAM-only description of the current physical attachment. Labels: gw (gateway hardware id),
// gwip, net (network address + prefix), dhcp, dns, sfx. Values are normalised lower-case text.
struct NetworkDescriptor {
    std::string kind;                        // wifi | ethernet | other (label only, not identity)
    std::map<std::string, std::string> raw;
    bool reliable() const;                   // gw, or gwip + net
    bool empty() const { return raw.empty(); }
};

struct MemoryRecord {
    std::string id;
    double created = 0;
    double verified = 0;
    std::string area;                        // dormitory | teaching | "" (area confirmed when remembered)
    bool proof_teaching = false;
    bool proof_dormitory = false;
    std::map<std::string, std::string> features;  // label -> 32 hex chars
};

// What a remembered network still has to prove before a submission is allowed.
enum class MemoryVerdict { notRemembered, conflict, awaitingProof, authorized };
std::string_view to_string(MemoryVerdict value);
// strong: the dormitory portal alone; medium, or strong while a tunnel / several interfaces make the
// physical path ambiguous: HTTPS campus identity AND the dormitory portal. weak/none never authorize.
MemoryVerdict memory_verdict(MatchLevel level, const CampusDiscovery& discovery, bool ambiguous);

class NetworkMemory {
public:
    static constexpr std::size_t kLimit = 8;
    static constexpr double kExpirySeconds = 180.0 * 86400.0;
    static constexpr double kTouchInterval = 3600.0;

    NetworkMemory() = default;
    explicit NetworkMemory(std::string salt_hex) : salt_(std::move(salt_hex)) {}
    // nullopt: corrupt or unknown schema. The caller keeps the file and runs without memory.
    static std::optional<NetworkMemory> decode(std::string_view text, double now);
    std::string encode() const;

    struct Match {
        MatchLevel level = MatchLevel::none;
        std::string record;
        std::string area;
    };
    static MatchLevel compare(const std::map<std::string, std::string>& stored, const std::map<std::string, std::string>& current);
    std::map<std::string, std::string> hashed(const NetworkDescriptor& descriptor) const;
    Match match(const NetworkDescriptor& descriptor, double now, const std::set<std::string>* exclude = nullptr,
                bool require_dormitory_proof = false) const;
    Match match_any(const std::vector<NetworkDescriptor>& descriptors, double now, const std::set<std::string>* exclude = nullptr,
                    bool require_dormitory_proof = false) const;
    std::vector<std::string> matching_ids(const std::vector<NetworkDescriptor>& descriptors, double now) const;  // medium or strong
    // Duplicates with identical evidence or the same known gateway; never joins records through a missing gateway.
    std::vector<std::string> equivalent_ids(const std::string& id) const;
    std::vector<std::string> merge_candidates(const NetworkDescriptor& descriptor, double now) const;

    // Adds or refreshes the record for a reliable descriptor. nullopt when the descriptor is not reliable.
    std::optional<std::string> remember(const NetworkDescriptor& descriptor, const std::string& area, bool proof_teaching,
                                        bool proof_dormitory, double now, const std::string& new_id);
    bool touch(const std::string& id, double now, const std::string& area);  // throttled; true when changed
    bool forget(const std::string& id);
    void forget_all() { records_.clear(); }

    const std::vector<MemoryRecord>& records() const { return records_; }
    std::size_t size() const { return records_.size(); }
    bool has_salt() const { return salt_.size() >= 16; }

private:
    void prune(double now);
    std::string salt_;
    std::vector<MemoryRecord> records_;
};

}  // namespace szu
