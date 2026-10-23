// Windows ConnectionService: fixed campus requests use a bound physical channel;
// ordinary website and explicit system-proxy comparisons use WinHTTP.
#pragma once
#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "szu/engine.hpp"
#include "szu/internet.hpp"
#include "szu/environment.hpp"
#include "szu/win/http.hpp"

namespace szu::win {

using WebsiteResult = WebsiteCheckResult;

class WinHttpService final : public ConnectionService {
public:
    struct Hooks {
        std::function<bool(const physical::Route&)> route_current;
        // The final callback reports binding ownership, not physical lease validity.
        // Native sessions independently validate their route at every transport gate.
        std::function<std::unique_ptr<FixedHttpSession>(const physical::Route&, bool, std::function<bool()>)> session_factory;
        std::function<HttpResponse(const HttpRequest&, const CancelToken&)> unbound_get;
    };
    WinHttpService();
    // Physical hooks isolate bound transport; unbound_get also isolates explicit
    // system-path compatibility comparisons.
    explicit WinHttpService(Hooks hooks);
    bool set_expected_physical_route(const std::optional<physical::Route>& route) override;
    ConnectivityReport probe(ProbeMode mode, const CancelToken& token) override;
    CampusDiscovery discover(CampusArea area, const CancelToken& token) override;
    LoginAttempt attempt(const Credentials& credentials, CampusArea area, const CancelToken& token) override;
    LoginAttempt attempt_with_requirements(const Credentials& credentials, CampusArea area, const CancelToken& token,
                                          bool require_client_config) override;

    // The address must already be normalized by szu::website::normalize; kept in memory only.
    WebsiteResult check_website(const std::string& url, bool follow_system_proxy, const CancelToken& token);
    // Bounded (≈8 s), credential-free comparison for the compatibility diagnosis.
    compat::Input compatibility(bool system_proxy_on, const std::string& tunnel, const CancelToken& token,
                                bool include_current_path = false);

private:
    struct Binding;
    struct Scope;
    std::unique_ptr<Scope> scope(const CancelToken& token);
    std::unique_ptr<FixedHttpSession> session(const Scope& scope, bool cookies);
    bool current(const Scope& scope) const;
    HttpResponse get(FixedHttpSession& session, const HttpRequest& request, const Scope& scope, const CancelToken& token);
    LoginAttempt teaching(const Credentials& credentials, Scope& scope, bool require_client_config);
    LoginAttempt dormitory(const Credentials& credentials, Scope& scope);
    Hooks hooks_;
    std::mutex binding_mutex_;
    std::shared_ptr<Binding> binding_;
};

}  // namespace szu::win
