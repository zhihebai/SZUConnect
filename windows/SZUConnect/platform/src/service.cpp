// Protocol flow mirrors macOS PortalClient.swift (itself adapted from ATMLuck/AutoSZUWeb src/web.cpp).
#include "szu/win/service.hpp"

#include <chrono>
#include <mutex>
#include <thread>

#include "szu/portal_wire.hpp"
#include "szu/win/physical.hpp"
#include "szu/srun.hpp"

namespace szu::win {
namespace {

constexpr int kProbeTimeoutMs = 6000;
constexpr int kAuthTimeoutMs = 12000;
constexpr int kWebsiteTimeoutMs = 8000;

PortalError portal_error(const HttpResponse& r) {
    switch (r.error) {
        case HttpError::none: return r.status == 200 ? PortalError::none : PortalError::httpStatus;
        case HttpError::timeout: return PortalError::timeout;
        case HttpError::dns: return PortalError::dns;
        case HttpError::tls: return PortalError::tls;
        case HttpError::redirect: return PortalError::redirect;
        case HttpError::tooLarge: return PortalError::responseTooLarge;
        case HttpError::network: return PortalError::network;
        case HttpError::cancelled: return PortalError::network;
    }
    return PortalError::network;
}

EndpointState endpoint_state(const HttpResponse& r, bool content_ok) {
    switch (r.error) {
        case HttpError::none:
            if (r.status >= 300 && r.status < 400) return EndpointState::redirect;
            return content_ok ? EndpointState::success : EndpointState::unexpectedResponse;
        case HttpError::redirect: return EndpointState::redirect;
        case HttpError::timeout: return EndpointState::timeout;
        case HttpError::dns: return EndpointState::dns;
        case HttpError::tls: return EndpointState::tls;
        case HttpError::cancelled: return EndpointState::cancelled;
        case HttpError::tooLarge: return EndpointState::unexpectedResponse;
        case HttpError::network: return EndpointState::network;
    }
    return EndpointState::network;
}

std::string token_for(const HttpResponse& r) {
    switch (r.error) {
        case HttpError::none:
            if (r.status >= 200 && r.status < 300) return "reachable";
            if (r.status >= 300 && r.status < 400) return "redirect";
            if (r.status == 401 || r.status == 407) return "authentication";
            return "httpStatus";
        case HttpError::redirect: return "redirect";
        case HttpError::tooLarge: return "httpStatus";
        case HttpError::tls: return "tls";
        case HttpError::dns: return "dns";
        case HttpError::timeout: return "timeout";
        case HttpError::cancelled: return "cancelled";
        case HttpError::network: return "network";
    }
    return "network";
}

LoginAttempt outcome(PortalError error, bool submitted, RejectionKind rejection = RejectionKind::unspecified) {
    LoginAttempt a;
    a.error = error;
    a.credential_submission_started = submitted;
    a.outcome = wire::outcome_for(error, submitted);
    a.rejection = rejection;
    return a;
}

LoginAttempt cancelled() {
    LoginAttempt a;
    a.outcome = Outcome::cancelled;
    a.credential_submission_started = false;
    return a;
}

// A child token that follows its parent and can also be cancelled on its own.
struct ChildToken {
    CancelToken token;
    const CancelToken& parent;
    std::uint64_t registration;
    explicit ChildToken(const CancelToken& p) : parent(p) {
        CancelToken copy = token;
        registration = parent.on_cancel([copy] { copy.cancel(); });
    }
    ~ChildToken() { parent.remove(registration); }
};

}  // namespace

struct WinHttpService::Binding {
    std::optional<physical::Route> route;
    CancelToken lifetime;
};
struct WinHttpService::Scope {
    std::shared_ptr<Binding> binding;
    CancelToken token;
    CancelToken parent;
    std::uint64_t parent_registration = 0, binding_registration = 0;
    Scope(std::shared_ptr<Binding> b, const CancelToken& p) : binding(std::move(b)), parent(p) {
        const CancelToken copy = token;
        parent_registration = parent.on_cancel([copy] { copy.cancel(); });
        if (binding) binding_registration = binding->lifetime.on_cancel([copy] { copy.cancel(); });
        if (!binding || !binding->route) token.cancel();
    }
    ~Scope() {
        parent.remove(parent_registration);
        if (binding) binding->lifetime.remove(binding_registration);
    }
};

WinHttpService::WinHttpService() : WinHttpService(Hooks{
    physical_route_current,
    [](const physical::Route& route, bool cookies, std::function<bool()> current) {
        return std::make_unique<PhysicalHttpSession>(route, cookies, std::move(current));
    }}) {}
WinHttpService::WinHttpService(Hooks hooks) : hooks_(std::move(hooks)), binding_(std::make_shared<Binding>()) {}

bool WinHttpService::set_expected_physical_route(const std::optional<physical::Route>& route) {
    // Rejecting a lease keeps that requested lease pinned and fail-closed. It
    // never restores an older route, searches a different NIC or uses system DNS.
    const bool current = !route || (hooks_.route_current && hooks_.route_current(*route));
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        if (binding_->route != route || binding_->lifetime.cancelled() || !current) {
            const auto old = binding_;
            // Old scopes lose authority before the new pin becomes visible.
            old->lifetime.cancel();
            binding_ = std::make_shared<Binding>();
            binding_->route = route;
            if (!current) binding_->lifetime.cancel();
        }
    }
    return current;
}
std::unique_ptr<WinHttpService::Scope> WinHttpService::scope(const CancelToken& token) {
    std::shared_ptr<Binding> binding;
    { std::lock_guard<std::mutex> lock(binding_mutex_); binding = binding_; }
    return std::make_unique<Scope>(std::move(binding), token);
}
std::unique_ptr<FixedHttpSession> WinHttpService::session(const Scope& scope, bool cookies) {
    if (!scope.binding || !scope.binding->route || scope.token.cancelled() || !hooks_.route_current || !hooks_.session_factory) return nullptr;
    const auto binding = scope.binding;
    const auto current = [binding] {
        return !binding->lifetime.cancelled() && binding->route.has_value();
    };
    if (!current() || !hooks_.route_current(*binding->route) || !current()) return nullptr;
    // The transport checks the exact physical lease between ownership checks.
    // Keeping this callback ownership-only avoids repeating the same double snapshot.
    return hooks_.session_factory(*binding->route, cookies, current);
}

bool WinHttpService::current(const Scope& scope) const {
    return scope.binding && scope.binding->route && !scope.token.cancelled() &&
        !scope.binding->lifetime.cancelled() && hooks_.route_current && hooks_.route_current(*scope.binding->route);
}
HttpResponse WinHttpService::get(FixedHttpSession& fixed, const HttpRequest& request, const Scope& scope, const CancelToken& token) {
    HttpResponse response;
    if (token.cancelled() || !current(scope)) { response.error = HttpError::cancelled; return response; }
    response = fixed.get(request, token);
    if (token.cancelled() || !current(scope)) { response.error = HttpError::cancelled; response.body.clear(); }
    return response;
}

ConnectivityReport WinHttpService::probe(ProbeMode mode, const CancelToken& token) {
    ConnectivityReport report;
    report.mode = mode;
    const auto operation = scope(token);
    std::mutex mutex;
    ChildToken apple_token(operation->token), microsoft_token(operation->token);
    auto run = [&](const char* url, bool apple) {
        HttpRequest request;
        request.url = url;
        request.timeout_ms = kProbeTimeoutMs;
        request.max_bytes = wire::kMaxProbeBytes;
        auto fixed = session(*operation, false); // probes never see an authentication cookie jar
        HttpResponse r;
        if (fixed) r = get(*fixed, request, *operation, apple ? apple_token.token : microsoft_token.token);
        else r.error = HttpError::cancelled;
        const bool ok = r.ok() && (apple ? wire::is_connectivity_success(r.body, r.status) : wire::is_microsoft_success(r.body, r.status));
        EndpointResult result{endpoint_state(r, ok), r.elapsed};
        std::lock_guard<std::mutex> lock(mutex);
        (apple ? report.apple : report.microsoft) = result;
        if (mode == ProbeMode::quick && (result.succeeded() || (result.state == EndpointState::redirect && r.campus_redirect)))
            (apple ? microsoft_token : apple_token).token.cancel();
    };
    std::thread a(run, "https://www.apple.com/library/test/success.html", true);
    std::thread m(run, "http://www.msftconnecttest.com/connecttest.txt", false);
    a.join(); m.join();
    // A peer cancelled after proven online/portal evidence is not a failed probe.
    // Complete mode always waits for both; unknown redirects never stop its peer.
    if (mode == ProbeMode::quick && !operation->token.cancelled()) {
        if (report.apple.state == EndpointState::cancelled) report.apple.state = EndpointState::notCompleted;
        if (report.microsoft.state == EndpointState::cancelled) report.microsoft.state = EndpointState::notCompleted;
    }
    if (operation->token.cancelled() || !operation->binding->route || !hooks_.route_current(*operation->binding->route)) {
        report.apple.state = EndpointState::cancelled; report.microsoft.state = EndpointState::cancelled;
    }
    return report;
}

CampusDiscovery WinHttpService::discover(CampusArea area, const CancelToken& token) {
    CampusDiscovery discovery{Discovery::unconfirmed, Discovery::notChecked};
    const auto operation = scope(token);
    auto fixed = session(*operation, false);
    if (!fixed) return discovery;
    HttpRequest identity;
    identity.url = "https://net.szu.edu.cn/srun_portal_pc";
    identity.timeout_ms = kProbeTimeoutMs;
    identity.max_bytes = wire::kMaxResponseBytes;
    const HttpResponse home = get(*fixed, identity, *operation, operation->token);
    // TLS/identity/lease failure is terminal; never downgrade to the HTTP portal.
    if (operation->token.cancelled() || !home.ok() || !wire::is_campus_homepage(home.body, home.status, identity.url)) return discovery;
    discovery.teaching = Discovery::confirmed;
    if (area == CampusArea::teaching) return discovery;
    HttpRequest dorm;
    dorm.url = "http://172.30.255.42:801/eportal";
    dorm.timeout_ms = kProbeTimeoutMs;
    dorm.max_bytes = wire::kMaxResponseBytes;
    const HttpResponse page = get(*fixed, dorm, *operation, operation->token);
    if (operation->token.cancelled()) return {Discovery::unconfirmed, Discovery::notChecked};
    discovery.dormitory = page.ok() && wire::is_dormitory_homepage(page.body, page.status, dorm.url, true) ? Discovery::confirmed
                                                                                                         : Discovery::unconfirmed;
    return discovery;
}

LoginAttempt WinHttpService::attempt(const Credentials& credentials, CampusArea area, const CancelToken& token) {
    if (!credentials.complete()) return outcome(PortalError::invalidCredentials, false);
    const auto operation = scope(token);
    if (operation->token.cancelled()) return cancelled();
    if (area == CampusArea::dormitory) return dormitory(credentials, *operation);
    if (area == CampusArea::teaching) return teaching(credentials, *operation);
    return outcome(PortalError::forbiddenTarget, false); // engine must choose a single concrete area
}

LoginAttempt WinHttpService::teaching(const Credentials& credentials, Scope& operation) {
    auto fixed = session(operation, true); // fresh RAM-only jar for this attempt
    if (!fixed) return cancelled();
    const auto& token = operation.token;
    auto get = [&](const std::string& url, bool retry) {
        HttpRequest request;
        request.url = url;
        request.timeout_ms = kAuthTimeoutMs;
        request.max_bytes = wire::kMaxResponseBytes;
        request.allow_cookies = true;
        HttpResponse r = this->get(*fixed, request, operation, token);
        if (retry && !token.cancelled() && wire::is_transient(portal_error(r))) {
            // Only pre-submission credential-free pages/challenges are retried.
            for (int step = 0; step < 16 && !token.cancelled(); ++step) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!token.cancelled()) r = this->get(*fixed, request, operation, token);
        }
        return r;
    };
    std::string ac_id;
    const std::string home_url = "https://net.szu.edu.cn/srun_portal_pc";
    const HttpResponse home = get(home_url, true);
    if (token.cancelled() || home.error == HttpError::cancelled) return cancelled();
    if (const PortalError e = portal_error(home); e != PortalError::none) return outcome(e, false);
    if (!wire::is_campus_homepage(home.body, home.status, home_url)) return outcome(PortalError::protocolConflict, false);
    const auto parsed = wire::ac_id(home.body);
    if (parsed.error != PortalError::none) return outcome(parsed.error, false);
    ac_id = parsed.value;
    const auto challenge_url = wire::make_url(wire::kTeachingBase, "/cgi-bin/get_challenge",
                                              {{"callback", "_"}, {"username", credentials.account}, {"ip", ""}});
    if (!challenge_url) return outcome(PortalError::forbiddenTarget, false);
    const HttpResponse challenge_response = get(*challenge_url, true);
    if (token.cancelled() || challenge_response.error == HttpError::cancelled) return cancelled();
    if (const PortalError e = portal_error(challenge_response); e != PortalError::none) return outcome(e, false);
    const auto challenge_object = wire::object_from(challenge_response.body);
    if (!challenge_object) return outcome(PortalError::protocolConflict, false);
    const auto challenge = wire::challenge(*challenge_object);
    if (challenge.error != PortalError::none) return outcome(challenge.error, false);
    const auto payload = srun::payload(credentials, challenge.challenge, challenge.ip, ac_id);
    const auto submit_url = wire::make_url(wire::kTeachingBase, "/cgi-bin/srun_portal",
                                           {{"ac_id", ac_id}, {"action", "login"}, {"callback", "_"}, {"chksum", payload.checksum},
                                            {"double_stack", "0"}, {"info", payload.info}, {"ip", challenge.ip}, {"n", "200"},
                                            {"name", "Windows"}, {"os", "Windows"}, {"password", "{MD5}" + payload.hash},
                                            {"type", "1"}, {"username", credentials.account}});
    if (!submit_url) return outcome(PortalError::forbiddenTarget, false);
    if (token.cancelled()) return cancelled();
    const HttpResponse submitted = get(*submit_url, false); // one credential-bearing request, never retry
    if (submitted.error == HttpError::cancelled || token.cancelled())
        return submitted.request_started ? outcome(PortalError::authenticationUncertain, true) : cancelled();
    if (const PortalError e = portal_error(submitted); e != PortalError::none)
        return outcome(submitted.request_started ? wire::after_submission(e) : e, submitted.request_started);
    const auto object = wire::object_from(submitted.body);
    if (!object) return outcome(PortalError::authenticationUncertain, true);
    if (wire::teaching_verdict(*object) == wire::LoginVerdict::accepted) {
        LoginAttempt ok; ok.outcome = Outcome::success; ok.message = "event.succeededTeaching"; return ok;
    }
    if (wire::teaching_verdict(*object) == wire::LoginVerdict::rejected)
        return outcome(PortalError::rejected, true, wire::rejection_kind(*object));
    return outcome(PortalError::authenticationUncertain, true);
}

LoginAttempt WinHttpService::dormitory(const Credentials& credentials, Scope& operation) {
    auto fixed = session(operation, true);
    if (!fixed) return cancelled();
    const auto& token = operation.token;
    // This exact bound session must freshly prove both HTTPS campus identity and
    // the dormitory page before a plaintext dormitory credential can be sent.
    HttpRequest identity;
    identity.url = "https://net.szu.edu.cn/srun_portal_pc";
    identity.timeout_ms = kAuthTimeoutMs; identity.max_bytes = wire::kMaxResponseBytes; identity.allow_cookies = true;
    const auto home = get(*fixed, identity, operation, token);
    if (token.cancelled() || home.error == HttpError::cancelled) return cancelled();
    if (const auto e = portal_error(home); e != PortalError::none) return outcome(e, false);
    if (!wire::is_campus_homepage(home.body, home.status, identity.url)) return outcome(PortalError::protocolConflict, false);
    identity.url = "http://172.30.255.42:801/eportal";
    const auto page = get(*fixed, identity, operation, token);
    if (token.cancelled() || page.error == HttpError::cancelled) return cancelled();
    if (const auto e = portal_error(page); e != PortalError::none) return outcome(e, false);
    if (!wire::is_dormitory_homepage(page.body, page.status, identity.url, true)) return outcome(PortalError::protocolConflict, false);
    const auto url = wire::make_url(wire::kDormitoryBase, "/eportal/portal/login",
                                    {{"login_method", "1"}, {"user_account", ",0," + credentials.account}, {"user_password", credentials.password}});
    if (!url) return outcome(PortalError::forbiddenTarget, false);
    HttpRequest request;
    request.url = *url; request.timeout_ms = kAuthTimeoutMs; request.max_bytes = wire::kMaxResponseBytes; request.allow_cookies = true;
    if (token.cancelled()) return cancelled();
    const auto submitted = get(*fixed, request, operation, token);
    if (submitted.error == HttpError::cancelled || token.cancelled())
        return submitted.request_started ? outcome(PortalError::authenticationUncertain, true) : cancelled();
    if (const auto e = portal_error(submitted); e != PortalError::none)
        return outcome(submitted.request_started ? wire::after_submission(e) : e, submitted.request_started);
    const auto object = wire::object_from(submitted.body);
    if (!object) return outcome(PortalError::authenticationUncertain, true);
    if (wire::dormitory_verdict(*object) == wire::LoginVerdict::accepted) {
        LoginAttempt ok; ok.outcome = Outcome::success; ok.message = "event.succeededDormitory"; return ok;
    }
    if (wire::dormitory_verdict(*object) == wire::LoginVerdict::rejected)
        return outcome(PortalError::rejected, true, wire::rejection_kind(*object));
    return outcome(PortalError::authenticationUncertain, true);
}

WebsiteResult WinHttpService::check_website(const std::string& url, bool follow_system_proxy, const CancelToken& token) {
    HttpRequest request;
    request.url = url;
    request.proxy = follow_system_proxy ? ProxyMode::system : ProxyMode::none;
    request.timeout_ms = kWebsiteTimeoutMs;
    request.headers_only = true;
    const HttpResponse r = http_get(request, token);
    WebsiteResult result;
    result.elapsed = r.elapsed;
    result.status = r.status;
    result.recovery_signal = r.website_recovery_signal();
    switch (r.error) {
        case HttpError::none: result.state = std::string(website::classify_status(r.status)); break;
        case HttpError::redirect: result.state = "redirected"; break;
        case HttpError::dns: result.state = "dns"; break;
        case HttpError::tls: result.state = "tls"; break;
        case HttpError::timeout: result.state = "timeout"; break;
        case HttpError::cancelled: result.state = "cancelled"; break;
        default: result.state = "network"; break;
    }
    return result;
}

compat::Input WinHttpService::compatibility(bool system_proxy_on, const std::string& tunnel, const CancelToken& token) {
    compat::Input input;
    input.proxy = system_proxy_on ? "on" : "off";
    input.tunnel = tunnel;
    const auto operation = scope(token);
    std::mutex mutex;
    auto run = [&](const char* url, ProxyMode proxy, std::string& out) {
        HttpRequest request;
        request.url = url;
        request.proxy = proxy;
        request.timeout_ms = kWebsiteTimeoutMs;
        request.headers_only = true;
        HttpResponse response;
        if (proxy == ProxyMode::none) {
            auto fixed = session(*operation, false);
            if (fixed) response = get(*fixed, request, *operation, operation->token);
            else response.error = HttpError::cancelled;
        } else response = http_get(request, token);
        const std::string token_text = token_for(response);
        std::lock_guard<std::mutex> lock(mutex);
        out = token_text;
    };
    const char* campus = "https://net.szu.edu.cn/srun_portal_pc";
    const char* pub = "https://www.apple.com/library/test/success.html";
    std::thread t1(run, campus, ProxyMode::none, std::ref(input.campus_direct));
    std::thread t2(run, pub, ProxyMode::none, std::ref(input.public_direct));
    std::thread t3, t4;
    if (system_proxy_on) {
        t3 = std::thread(run, campus, ProxyMode::system, std::ref(input.campus_system));
        t4 = std::thread(run, pub, ProxyMode::system, std::ref(input.public_system));
    }
    t1.join();
    t2.join();
    if (t3.joinable()) t3.join();
    if (t4.joinable()) t4.join();
    return input;
}

}  // namespace szu::win
