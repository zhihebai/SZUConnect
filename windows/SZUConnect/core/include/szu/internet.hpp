// Optional user websites supply bounded recovery candidates; fresh campus proof still gates authentication.
#pragma once
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "szu/engine.hpp"

namespace szu {
struct WebsiteTarget {
    static constexpr std::size_t maximum_count = 12;
    std::string id;
    std::string url;
    bool enabled = true;
};
enum class WebsiteRecoverySignal { none, connectionFailure, campusRedirect };
struct WebsiteCheckResult {
    std::string state = "notRun";
    int status = 0;
    double elapsed = 0;
    WebsiteRecoverySignal recovery_signal = WebsiteRecoverySignal::none;
    bool passed() const { return state == "accessible" && status >= 200 && status < 300; }
    bool has_http_response() const;
};
struct WebsiteCheck {
    WebsiteTarget target;  // local UI only; never place this object in diagnostics or event logs
    WebsiteCheckResult result;
};
enum class InternetVerdict { basicPassed, allPassed, partial, unavailable, cancelled };
struct InternetCheckReport {
    ConnectivityReport base;
    std::vector<WebsiteCheck> websites;
    std::size_t selected_count = 0;
    bool cancelled = false;
    bool follow_system_proxy = true;  // strategy used by this report, never the current preference
    bool websites_deferred = false;
    InternetVerdict verdict() const;
    bool website_only_issues() const;
    bool has_recovery_evidence() const;
    bool has_campus_recovery_evidence() const;
    bool transport_recovered() const;
    std::size_t passed_count() const;
    std::size_t expected_count() const { return 2 + (base.mode == ProbeMode::complete ? selected_count : 0); }
};
std::string_view to_string(InternetVerdict value);

// Normalizes all rows, bounds the list to twelve, and rejects duplicate ids/URLs. No persistence.
std::optional<std::vector<WebsiteTarget>> validated_website_targets(const std::vector<WebsiteTarget>& targets);

// Independent local-only websites.json schema. Neither this type nor URLs enter history/export.
struct WebsitePreferences {
    std::vector<WebsiteTarget> targets;
    bool follow_system_proxy = true;
    static std::optional<WebsitePreferences> decode(std::string_view text);
    std::optional<std::string> encode() const;
};
class WebsitePreferencesState {
public:
    enum class ReadStatus { found, missing, failed };
    using Writer = std::function<bool(const std::string& data, bool preserve_existing)>;
    bool load(ReadStatus status, std::string_view data = {});
    bool save(const WebsitePreferences& next, const Writer& writer);
    const WebsitePreferences& current() const { return current_; }
    bool needs_preservation() const { return preserve_existing_; }
private:
    WebsitePreferences current_;
    bool preserve_existing_ = false;
};

// Decorates only probe(). discover()/attempt() retain their original implementation and guards.
// Each accepted full check uses a fixed configuration snapshot and <=12 parallel header checks.
class InternetCheckService final : public ConnectionService {
public:
    using WebsiteChecker = std::function<WebsiteCheckResult(const std::string&, bool, const CancelToken&)>;
    InternetCheckService(ConnectionService& base, WebsiteChecker checker);
    bool configure(const std::vector<WebsiteTarget>& targets, bool follow_system_proxy);
    void invalidate();
    void invalidate_website_path();
    std::optional<InternetCheckReport> latest() const;
    ConnectivityReport probe(ProbeMode mode, const CancelToken& token) override;
    ConnectivityReport probe_with_scope(ProbeMode mode, const CancelToken& token, bool prioritize_base, bool defer_websites) override;
    bool set_expected_physical_route(const std::optional<physical::Route>& route) override {
        return base_.set_expected_physical_route(route);
    }
    CampusDiscovery discover(CampusArea area, const CancelToken& token) override { return base_.discover(area, token); }
    LoginAttempt attempt(const Credentials& credentials, CampusArea area, const CancelToken& token) override {
        return base_.attempt(credentials, area, token);
    }
private:
    ConnectionService& base_;
    WebsiteChecker checker_;
    mutable std::mutex mutex_;
    std::vector<WebsiteTarget> targets_;
    bool follow_system_proxy_ = true;
    std::uint64_t generation_ = 0;
    CancelToken active_;
    CancelToken active_website_;
    std::uint64_t website_path_generation_ = 0;
    std::optional<InternetCheckReport> latest_;
};
}  // namespace szu
