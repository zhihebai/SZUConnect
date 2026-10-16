// Optional association only. SSIDs are local candidate names, never authentication evidence.
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "szu/engine.hpp"
#include "szu/netlabels.hpp"

namespace szu::wifi {
struct Network {
    std::string id, ssid, display_name;
    bool valid() const;
    bool operator==(const Network&) const = default;
};
struct Preferences {
    bool auto_join = false, power_on = false;
    std::set<std::string> excluded;
    // Mac schema-1 legacy allowlist is honored until a complete remembered context is available.
    std::optional<std::map<std::string, std::string>> legacy_selections;
    std::map<std::string, std::string> manual_names;
    static std::optional<Preferences> decode(const std::string&);
    std::optional<std::string> encode() const;
    bool selected(const Network&) const;
    bool migrate(const std::vector<Network>&);
};
std::vector<Network> candidates(const std::vector<MemoryRecord>&, const NetworkLabels&, const Preferences&);
// An unsaved OPEN/NONE profile. The display name exactly matches the original SSID.
std::optional<std::string> temporary_open_profile(const std::string& ssid);
enum class PhysicalState { connected, disconnected, unknown };
enum class Result { cancelled, alreadyConnected, notFound, wifiOff, poweredOn, joined,
                    systemConnectionRequired, permissionRequired, noSelection, unavailable, connectionUnclear, failed };
std::string_view status_key(Result);
struct ScanCandidate { std::string ssid, interface_key; int signal = 0; bool open = false; };
struct ScanResult { Result result = Result::notFound; std::vector<ScanCandidate> networks; };
class Radio {
public:
    virtual ~Radio() = default;
    virtual PhysicalState physical_state() = 0;
    virtual std::optional<bool> power_state() = 0;
    virtual Result power_on(const CancelToken&) = 0;
    virtual ScanResult scan(const std::string&, const CancelToken&) = 0;
    virtual Result associate_open(const ScanCandidate&, const CancelToken&) = 0;
};
Result recover(Radio&, const std::vector<Network>&, bool allow_power_on, const CancelToken&);
// Thread-confined scheduling state. Cancellation is a separate shared token, so a UI opt-out
// remains visible to a blocking platform scan immediately.
class Schedule {
public:
    struct Request { std::vector<Network> networks; bool power_on; CancelToken token; };
    void update(std::vector<Network> selected, bool enabled, bool power_on,
                bool allow_recovery, bool has_physical, bool sleeping);
    bool ready(double now) const;
    std::optional<Request> begin(double now);
    void finish(Result, double now);
    void cancel();
    std::string status() const;
    bool active() const { return active_; }
private:
    std::vector<Network> selected_;
    bool enabled_ = false, power_on_ = false, allowed_ = false, physical_ = true, sleeping_ = false, active_ = false;
    unsigned failures_ = 0;
    double next_ = 0;
    CancelToken token_;
    std::optional<Result> result_;
};
} // namespace szu::wifi
