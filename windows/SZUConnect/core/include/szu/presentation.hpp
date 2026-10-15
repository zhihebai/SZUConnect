// First-screen status presenter and local records (events, health samples, diagnostics export).
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "szu/engine.hpp"
#include "szu/json.hpp"

namespace szu {

namespace status {
struct Input {
    std::string status = "idle";
    std::optional<std::string> diagnosis;
    std::string probe_mode = "quick";
    int passed_count = 0;
    std::string mode = "manual";
    bool read_only = false;
    bool has_draft = false;
    std::string auto_login = "off";
    std::optional<std::string> hold;
    bool offer_forced_login = false;
    bool complete_partial = false;
    bool website_only_issues = false;
    bool recovery_evidence = false;
    bool multiple_networks = false; // Windows physical-route guard, independent of session trust
};
struct Banner {
    std::string key;
    std::string tone;
    std::optional<std::string> action;
};
struct Presentation {
    std::string tone, headline, detail;
    std::optional<std::string> primary, secondary;
    std::string chip, chip_tone;
    std::optional<Banner> banner;
};
Presentation present(const Input& input);  // tools/reference/status_reference.py
Input input_from(const EngineSnapshot& snapshot);
// Current-connection service evidence only; connectivity, trust and network names are not proof.
std::string_view campus_verification_key(const EngineSnapshot& snapshot);
bool system_path_can_lead(const EngineSnapshot& snapshot); // never conceal authentication/protection states

// Auto-recovery readiness for the account, network and protection state.
// tools/reference/memory_reference.py + shared/fixtures/readiness-cases.json.
struct ReadinessInput {
    std::string mode = "manual";
    bool read_only = false, has_saved = false, unsaved_changes = false;
    std::string auto_login = "off";
    std::optional<std::string> hold;
    bool session_trusted = false, remembered = false, memory_conflict = false;
    bool network_identified = true, session_only_network = false;
    std::string campus_verification = "notChecked"; // current service evidence; never grants trust
};
struct ReadinessItem { std::string kind, key, tone; };
struct Readiness { std::string title, tone; std::vector<ReadinessItem> items; };
Readiness readiness(const ReadinessInput& input);
ReadinessInput readiness_input_from(const EngineSnapshot& snapshot);
}  // namespace status

namespace records {
std::string iso8601(double unix_seconds);                // "2026-09-29T08:30:00Z"
std::optional<double> parse_iso8601(std::string_view text);

struct Event {
    double time = 0;  // unix seconds
    std::string message;
    bool failure = false;
};

class EventLog {
public:
    static constexpr std::size_t kLimit = 300;
    static constexpr double kDedupeSeconds = 300;
    bool append(double now, std::string message, bool failure = false);  // false when deduplicated
    const std::vector<Event>& items() const { return items_; }
    json::Value to_json() const;
    static std::optional<EventLog> from_json(std::string_view text);  // nullopt: corrupt (quarantine it)
private:
    std::vector<Event> items_;
};

struct Sample {
    double time = 0;
    bool online = false;
    std::string mode;       // quick | complete
    std::string apple;      // EndpointState name
    std::string microsoft;
    double latency_ms = -1; // fastest successful endpoint, -1 when none
    int site_total = -1;    // selected sites included in a complete round (0...12), -1 unknown; never a URL
    int site_passed = -1;
};

// What the local history really covers: every retained sample and the newest ones drawn as the ribbon.
struct HealthRange {
    int count = 0;
    std::optional<double> first, last;
    int ribbon_count = 0;
    std::optional<double> ribbon_first, ribbon_last;
};

struct HealthSummary {
    int samples = 0;
    int online = 0;
    std::optional<double> median_latency_ms;
    int success_percent() const { return samples ? (online * 100 + samples / 2) / samples : 0; }
};

class HealthLog {
public:
    static constexpr std::size_t kLimit = 360;
    static constexpr double kWindowSeconds = 24 * 3600;
    void append(const Sample& sample);
    void prune(double now);
    const std::vector<Sample>& items() const { return items_; }
    HealthSummary summary(double now) const;
    HealthRange range(double now, std::size_t ribbon = 24) const;
    json::Value to_json() const;
    static std::optional<HealthLog> from_json(std::string_view text);
private:
    std::vector<Sample> items_;
};

Sample sample_from(const ConnectivityReport& report, double now, int site_total = -1, int site_passed = -1);

struct ExportContext {
    std::string version, build, platform;
    std::string mode, area, probe_mode;
    double now = 0;
};
// Credential-free, SSID-free diagnostics export (shared/schemas/diagnostics-export.schema.json).
json::Value diagnostics_export(const ExportContext& context, const HealthLog& health, const EventLog& events);
std::string diagnostics_summary_text(const ExportContext& context, const HealthLog& health);
}  // namespace records

}  // namespace szu
