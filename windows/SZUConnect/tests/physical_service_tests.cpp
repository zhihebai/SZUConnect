// Isolated production-service protocol/ownership tests. Every network response
// comes from an injected transport fake. No account vault or OS route changes.
#include "szu/win/service.hpp"
#include "szu/portal_wire.hpp"
#include "szu/presentation.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

// Portable builds run the same service logic. These fail-closed link guards are
// never its injected backend; any accidental native fall-through fails the test.
#ifndef _WIN32
namespace szu::win {
struct PhysicalHttpSession::State {};
PhysicalHttpSession::PhysicalHttpSession(physical::Route, bool, std::function<bool()>) { throw std::logic_error("unexpected native session"); }
PhysicalHttpSession::~PhysicalHttpSession() = default;
HttpResponse PhysicalHttpSession::get(const HttpRequest&, const CancelToken&) { throw std::logic_error("unexpected native request"); }
bool physical_route_current(const physical::Route&) { throw std::logic_error("unexpected native lease read"); }
HttpResponse http_get(const HttpRequest&, const CancelToken&) { throw std::logic_error("unexpected unbound request"); }
}  // namespace szu::win
#endif

namespace {
using namespace szu;
using namespace szu::win;
struct Checks {
    int count = 0, failures = 0;
    void check(bool value, const char* what) { ++count; if (!value) { ++failures; std::printf("FAIL: %s\n", what); } }
};
physical::Route route(const std::string& id) {
    physical::Route value;
    value.identity = id; value.interface_key = "fixture-" + id;
    value.interface_index = id == "A" ? 7 : 9; value.interface_luid = value.interface_index;
    value.source_ipv4 = id == "A" ? "192.0.2.7" : "192.0.2.9";
    value.addresses = {value.source_ipv4}; value.dns_servers = {"192.0.2.53"};
    return value;
}
HttpResponse success(const std::string& body) {
    HttpResponse value; value.status = 200; value.body = body; value.request_started = true; return value;
}
constexpr const char* kHome = "<html>Shenzhen University srun login<script>var ac_id=12;</script></html>";
constexpr const char* kDorm = "<html><title>ePortal</title></html>";
HttpResponse normal(const HttpRequest& request) {
    const auto url = wire::parse_url(request.url);
    if (!url) throw std::logic_error("invalid fake request");
    if (url->path == "/srun_portal_pc") return success(kHome);
    if (url->path == "/eportal") return success(kDorm);
    if (url->path == "/cgi-bin/get_challenge") return success(R"({"error":"ok","res":"ok","challenge":"fixture-challenge","client_ip":"192.0.2.7"})");
    if (url->path == "/cgi-bin/srun_portal") return success(R"({"error":"ok","res":"ok"})");
    if (url->path == "/eportal/portal/login") return success(R"({"result":0,"msg":""})");
    if (url->host == "www.apple.com") return success("<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    if (url->host == "www.msftconnecttest.com") return success("Microsoft Connect Test");
    throw std::logic_error("unplanned fake target");
}
struct Call { std::string identity, path; bool cookies = false, request_cookies = false; std::size_t session = 0; };
struct Backend {
    std::mutex mutex;
    std::vector<Call> calls;
    std::vector<bool> cookie_sessions;
    std::atomic<bool> lease_current{true};
    std::function<HttpResponse(const HttpRequest&, const CancelToken&, const std::function<bool()>&)> respond;
    std::function<HttpResponse(const physical::Route&, const HttpRequest&, const CancelToken&, const std::function<bool()>&)> bound_respond;
    struct Session final : FixedHttpSession {
        Backend& owner; physical::Route route; bool cookies; std::size_t id;
        std::function<bool()> current;
        Session(Backend& b, physical::Route r, bool c, std::size_t i, std::function<bool()> scope) : owner(b), route(std::move(r)), cookies(c), id(i), current(std::move(scope)) {}
        HttpResponse get(const HttpRequest& request, const CancelToken& token) override {
            const auto url = wire::parse_url(request.url);
            { std::lock_guard<std::mutex> lock(owner.mutex); owner.calls.push_back({route.identity, url ? url->path : "invalid", cookies, request.allow_cookies, id}); }
            // Deliberately allow scripts to return late success: the production
            // service must reject it independently of a cooperative transport.
            if (owner.bound_respond) return owner.bound_respond(route, request, token, current);
            return owner.respond ? owner.respond(request, token, current) : normal(request);
        }
    };
    WinHttpService::Hooks hooks() {
        return {
            [this](const physical::Route&) { return lease_current.load(); },
            [this](const physical::Route& route, bool cookies, std::function<bool()> current) -> std::unique_ptr<FixedHttpSession> {
                std::size_t id;
                { std::lock_guard<std::mutex> lock(mutex); id = cookie_sessions.size(); cookie_sessions.push_back(cookies); }
                return std::make_unique<Session>(*this, route, cookies, id, std::move(current));
            }
        };
    }
    int count(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex); int n = 0; for (const auto& c : calls) if (c.path == path) ++n; return n;
    }
};
struct Gate {
    std::mutex mutex; std::condition_variable cv; bool entered = false, released = false;
    void block() { std::unique_lock<std::mutex> lock(mutex); entered = true; cv.notify_all(); cv.wait(lock, [&] { return released; }); }
    bool wait() { std::unique_lock<std::mutex> lock(mutex); return cv.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }); }
    void release() { std::lock_guard<std::mutex> lock(mutex); released = true; cv.notify_all(); }
};

void verify_website_transport(Checks& t) {
    constexpr const char* url = "https://website.fixture.invalid/";
    for (const bool bound : {false, true}) for (const bool follow_system_proxy : {false, true}) {
        Backend backend; auto hooks = backend.hooks(); std::vector<HttpRequest> ordinary;
        hooks.unbound_get = [&](const HttpRequest& request, const CancelToken&) {
            ordinary.push_back(request); auto response = success("unused fixture body"); response.elapsed = 0.125; return response;
        };
        WinHttpService service(std::move(hooks));
        if (bound) service.set_expected_physical_route(route("A"));
        const auto result = service.check_website(url, follow_system_proxy, CancelToken{});
        t.check(ordinary.size() == 1 && ordinary.front().url == url && ordinary.front().headers_only &&
                !ordinary.front().allow_cookies && ordinary.front().timeout_ms == 8000 && ordinary.front().max_bytes <= 65536 &&
                ordinary.front().proxy == (follow_system_proxy ? ProxyMode::system : ProxyMode::none),
            "each website strategy sends one bounded cookie-free ordinary request with its explicit proxy policy");
        t.check(backend.calls.empty() && backend.cookie_sessions.empty() && result.passed() && result.elapsed == 0.125,
            "system and no-system-proxy website requests never borrow the bound campus transport or require its route");
    }
    for (const bool follow_system_proxy : {false, true}) {
        Backend backend; auto hooks = backend.hooks(); int calls = 0;
        hooks.unbound_get = [&](const auto&, const auto&) { ++calls; return success("unused fixture body"); };
        WinHttpService service(std::move(hooks)); CancelToken token; token.cancel();
        const auto result = service.check_website(url, follow_system_proxy, token);
        t.check(calls == 0 && backend.calls.empty() && result.state == "cancelled" && result.status == 0 &&
                result.recovery_signal == WebsiteRecoverySignal::none,
            "pre-cancelled website checks do not enter either native or injected transport");
    }
    for (const bool follow_system_proxy : {false, true}) for (const bool portal_redirect : {false, true}) {
        Backend backend; auto hooks = backend.hooks(); Gate gate;
        hooks.unbound_get = [&](const auto&, const auto&) {
            gate.block(); auto response = success("late fixture body");
            if (portal_redirect) { response.status = 302; response.campus_redirect = true; response.campus_area_hint = CampusPortalHint::teaching; }
            return response;
        };
        WinHttpService service(std::move(hooks)); CancelToken token; WebsiteResult result;
        std::thread running([&] { result = service.check_website(url, follow_system_proxy, token); });
        const bool entered = gate.wait(); token.cancel(); gate.release(); running.join();
        t.check(entered && result.state == "cancelled" && result.status == 0 && !result.passed() &&
                result.recovery_signal == WebsiteRecoverySignal::none && backend.calls.empty(),
            "late website success or campus redirect after cancellation supplies neither an accessible result nor recovery evidence");
    }
}

void verify_compatibility_current_path(Checks& t) {
    constexpr const char* campus = "https://net.szu.edu.cn/srun_portal_pc";
    constexpr const char* public_url = "https://www.apple.com/library/test/success.html";
    for (const bool system_proxy : {false, true}) for (const bool include_current : {false, true}) {
        Backend backend;
        std::mutex requests_mutex;
        std::vector<HttpRequest> bound_requests, unbound_requests;
        backend.respond = [&](const HttpRequest& request, const auto&, const auto&) {
            { std::lock_guard<std::mutex> lock(requests_mutex); bound_requests.push_back(request); }
            return normal(request);
        };
        auto hooks = backend.hooks();
        hooks.unbound_get = [&](const HttpRequest& request, const CancelToken&) {
            { std::lock_guard<std::mutex> lock(requests_mutex); unbound_requests.push_back(request); }
            return normal(request);
        };
        WinHttpService service(std::move(hooks)); service.set_expected_physical_route(route("A"));
        const auto input = service.compatibility(system_proxy, "detected", CancelToken{}, include_current);
        const bool compare_current = system_proxy || include_current;
        t.check(bound_requests.size() == 2 && unbound_requests.size() == (compare_current ? 2u : 0u),
            "explicit current-path comparison covers pure TUN without changing ordinary proxy-off checks");
        t.check(input.proxy == (system_proxy ? "on" : "off") && input.tunnel == "detected",
            "requesting the current path does not relabel the actual system proxy setting");
        t.check(input.campus_direct == "reachable" && input.public_direct == "reachable" &&
                input.campus_system == (compare_current ? "reachable" : "notRun") &&
                input.public_system == (compare_current ? "reachable" : "notRun"),
            "fixed and current-path compatibility results stay in their own columns");
        const auto bounded_public_head = [&](const HttpRequest& request, ProxyMode proxy) {
            return (request.url == campus || request.url == public_url) && request.headers_only &&
                !request.allow_cookies && request.proxy == proxy && request.timeout_ms == 8000 && request.max_bytes <= 65536;
        };
        t.check(std::all_of(bound_requests.begin(), bound_requests.end(), [&](const auto& request) { return bounded_public_head(request, ProxyMode::none); }) &&
                std::all_of(unbound_requests.begin(), unbound_requests.end(), [&](const auto& request) { return bounded_public_head(request, ProxyMode::system); }),
            "compatibility sends only bounded credential-free cookie-free HEADs to the two fixed public URLs");
        t.check(backend.count("/cgi-bin/get_challenge") == 0 && backend.count("/cgi-bin/srun_portal") == 0 &&
                backend.count("/eportal") == 0 && backend.count("/eportal/portal/login") == 0 &&
                std::none_of(backend.cookie_sessions.begin(), backend.cookie_sessions.end(), [](bool enabled) { return enabled; }),
            "TUN comparison does not create authentication sessions or issue challenge or login requests");
    }
    {
        Backend backend; auto hooks = backend.hooks(); std::atomic<int> unbound_calls{0};
        hooks.unbound_get = [&](const auto&, const auto&) { ++unbound_calls; return success("late fixture"); };
        WinHttpService service(std::move(hooks)); service.set_expected_physical_route(route("A"));
        CancelToken token; token.cancel();
        const auto input = service.compatibility(false, "detected", token, true);
        t.check(backend.calls.empty() && unbound_calls == 0 && input.campus_direct == "cancelled" &&
                input.public_direct == "cancelled" && input.campus_system == "cancelled" && input.public_system == "cancelled",
            "pre-cancelled TUN comparisons do not reach either injected transport");
    }
    {
        Backend backend; auto hooks = backend.hooks(); Gate gate;
        hooks.unbound_get = [&](const auto&, const auto&) { gate.block(); return success("late fixture"); };
        WinHttpService service(std::move(hooks)); service.set_expected_physical_route(route("A"));
        CancelToken token; compat::Input input;
        std::thread running([&] { input = service.compatibility(false, "detected", token, true); });
        const bool entered = gate.wait(); token.cancel(); gate.release(); running.join();
        t.check(entered && input.campus_system == "cancelled" && input.public_system == "cancelled" &&
                compat::diagnose(input).verdict == "cancelled",
            "cancellation rejects late current-path success without promoting it into a compatibility result");
    }
    {
        Backend backend; auto hooks = backend.hooks();
        backend.respond = [&](const HttpRequest& request, const auto&, const auto&) {
            if (request.url == campus) { HttpResponse response; response.error = HttpError::tls; return response; }
            return normal(request);
        };
        hooks.unbound_get = [&](const HttpRequest& request, const auto&) {
            if (request.url == campus) { HttpResponse response; response.error = HttpError::tls; return response; }
            return normal(request);
        };
        WinHttpService service(std::move(hooks)); service.set_expected_physical_route(route("A"));
        const auto input = service.compatibility(false, "detected", CancelToken{}, true);
        t.check(input.campus_direct == "tls" && input.campus_system == "tls" && input.public_direct == "reachable" &&
                input.public_system == "reachable" && compat::diagnose(input).verdict == "campusTLS",
            "TLS errors retain security priority during a pure-TUN current-path comparison");
        t.check(backend.count("/eportal") == 0 && backend.count("/cgi-bin/srun_portal") == 0 &&
                backend.count("/eportal/portal/login") == 0,
            "a TLS failure never downgrades to a dormitory page or authentication request");
    }
}

std::string configured_home(const std::string& controller, const std::string& ip) {
    return "<html>Shenzhen University srun login<script>const CONFIG = {ip:'" + ip + "',acid:" + controller + "};</script></html>";
}

void verify_source_bound_wired_handover(Checks& t) {
    const Credentials credentials{"fixture-account", "not-a-real-password"};
    auto wifi = route("A"), wired = route("B");
    wifi.descriptor.kind = "wifi"; wired.descriptor.kind = "ethernet";
    wifi.gateway_ipv4 = wired.gateway_ipv4 = "192.0.2.1";
    wired.dns_servers = {"192.0.2.54"};
    wifi.descriptor.raw = {{"gwip", wifi.gateway_ipv4}, {"net", "192.0.2.0/24"}, {"dns", "192.0.2.53"}};
    wired.descriptor.raw = {{"gwip", wired.gateway_ipv4}, {"net", "192.0.2.0/24"}, {"dns", "192.0.2.54"}};
    const auto response_for = [](const physical::Route& selected, const HttpRequest& request) {
        const auto url = wire::parse_url(request.url);
        if (url->path == "/srun_portal_pc")
            return success(configured_home(selected.identity == "A" ? "6" : "12", selected.source_ipv4));
        if (url->path == "/cgi-bin/get_challenge")
            return success("{\"error\":\"ok\",\"res\":\"ok\",\"challenge\":\"fixture-challenge\",\"client_ip\":\"" +
                selected.source_ipv4 + "\"}");
        return normal(request);
    };
    {
        Backend backend; WinHttpService service(backend.hooks());
        std::vector<physical::Route> actual_routes;
        bool matching_submissions = true;
        backend.bound_respond = [&](const auto& selected, const auto& request, const auto&, const auto&) {
            actual_routes.push_back(selected);
            const auto url = wire::parse_url(request.url);
            if (url->path == "/cgi-bin/srun_portal") {
                const auto controller = selected.identity == "A" ? "6" : "12";
                matching_submissions = matching_submissions && url->query.find("ac_id=" + std::string(controller)) != std::string::npos &&
                    url->query.find("ip=" + selected.source_ipv4) != std::string::npos;
            }
            return response_for(selected, request);
        };
        for (const auto& selected : {wifi, wired}) {
            t.check(service.set_expected_physical_route(selected), "source-specific handover pins the requested physical attachment");
            const auto discovery = service.discover(CampusArea::teaching, CancelToken{});
            const auto attempt = service.attempt_with_requirements(credentials, CampusArea::teaching, CancelToken{}, true);
            t.check(discovery.teaching == Discovery::confirmed && discovery.teaching_client_bound &&
                attempt.outcome == Outcome::success && attempt.credential_submission_started,
                "both Wi-Fi and Ethernet accept only their own newly read CONFIG and challenge source through the production service");
        }
        bool exact_routes = actual_routes.size() == 8;
        for (std::size_t i = 0; i < actual_routes.size(); ++i)
            exact_routes = exact_routes && actual_routes[i] == (i < 4 ? wifi : wired);
        t.check(exact_routes && matching_submissions && backend.count("/cgi-bin/get_challenge") == 2 &&
            backend.count("/cgi-bin/srun_portal") == 2 && backend.count("/eportal") == 0 &&
            backend.count("/eportal/portal/login") == 0,
            "a completed Wi-Fi round cannot lend its source, associated DNS, controller or service result to the next Ethernet round");
        t.check(backend.cookie_sessions == std::vector<bool>({false, true, false, true}) &&
            backend.calls.size() == 8 && backend.calls[0].session != backend.calls[4].session &&
            backend.calls[1].session != backend.calls[5].session,
            "each attachment receives fresh discovery and authentication sessions rather than reusing the old lease or cookie scope");
    }
    for (const bool stale_config : {false, true}) {
        Backend backend; WinHttpService service(backend.hooks());
        std::vector<physical::Route> actual_routes;
        backend.bound_respond = [&](const auto& selected, const auto& request, const auto&, const auto&) {
            actual_routes.push_back(selected);
            const auto path = wire::parse_url(request.url)->path;
            if (selected == wired && ((stale_config && path == "/srun_portal_pc") ||
                (!stale_config && path == "/cgi-bin/get_challenge"))) return response_for(wifi, request);
            return response_for(selected, request);
        };
        service.set_expected_physical_route(wifi);
        t.check(service.discover(CampusArea::teaching, CancelToken{}).teaching_client_bound,
            "the previous Wi-Fi supplies its own valid source-bound discovery before the rejection fixture switches attachments");
        service.set_expected_physical_route(wired);
        const auto discovery = service.discover(CampusArea::teaching, CancelToken{});
        const auto attempt = service.attempt_with_requirements(credentials, CampusArea::teaching, CancelToken{}, true);
        t.check(discovery.teaching == Discovery::confirmed && discovery.teaching_client_bound == !stale_config &&
            attempt.outcome == Outcome::security && attempt.error == PortalError::protocolConflict &&
            !attempt.credential_submission_started && backend.count("/cgi-bin/srun_portal") == 0 &&
            backend.count("/cgi-bin/get_challenge") == (stale_config ? 0 : 1) && backend.count("/eportal") == 0,
            "an Ethernet CONFIG or challenge containing the old Wi-Fi source fails at the proper pre-submission boundary without a dormitory fallback");
        t.check(actual_routes.size() >= 3 && actual_routes.front() == wifi &&
            std::all_of(actual_routes.begin() + 1, actual_routes.end(), [&](const auto& selected) { return selected == wired; }),
            "rejecting stale Wi-Fi page content never switches the Ethernet request back to the old route or associated resolver lease");
    }
    for (const bool challenge_stage : {false, true}) {
        Backend backend; WinHttpService service(backend.hooks()); CancelToken token; Gate gate;
        backend.bound_respond = [&](const auto& selected, const auto& request, const auto&, const auto&) {
            const auto path = wire::parse_url(request.url)->path;
            if (selected == wifi && path == (challenge_stage ? "/cgi-bin/get_challenge" : "/srun_portal_pc")) gate.block();
            return response_for(selected, request);
        };
        service.set_expected_physical_route(wifi);
        LoginAttempt old;
        std::thread worker([&] { old = service.attempt_with_requirements(credentials, CampusArea::teaching, token, true); });
        const bool entered = gate.wait();
        const bool pinned = service.set_expected_physical_route(wired);
        gate.release(); worker.join();
        t.check(entered && pinned && old.outcome == Outcome::cancelled && !old.credential_submission_started &&
            backend.count("/cgi-bin/srun_portal") == 0,
            "repinning Wi-Fi to Ethernet rejects a late source-matched homepage or challenge before any credential-bearing request");
        const auto discovery = service.discover(CampusArea::teaching, CancelToken{});
        const auto attempt = service.attempt_with_requirements(credentials, CampusArea::teaching, CancelToken{}, true);
        t.check(discovery.teaching_client_bound && attempt.outcome == Outcome::success &&
            backend.count("/cgi-bin/srun_portal") == 1 && backend.calls.back().identity == wired.identity &&
            backend.count("/eportal") == 0 && backend.count("/eportal/portal/login") == 0,
            "the replacement Ethernet can complete one independent strong-source attempt after stale Wi-Fi work is discarded");
    }
}

void verify_client_configuration(Checks& t) {
    const Credentials credentials{"fixture-account", "not-a-real-password"};
    for (const bool selected_source : {false, true}) {
        Backend backend; WinHttpService service(backend.hooks());
        auto selected = route("A"); selected.addresses.push_back("192.0.2.99");
        service.set_expected_physical_route(selected);
        backend.respond = [selected_source](const auto& request, const auto&, const auto&) {
            if (wire::parse_url(request.url)->path == "/srun_portal_pc")
                return success(configured_home("12", selected_source ? "192.0.2.7" : "192.0.2.99"));
            return normal(request);
        };
        const auto discovery = service.discover(CampusArea::automatic, CancelToken{});
        t.check(discovery.teaching == Discovery::confirmed && discovery.dormitory == Discovery::confirmed &&
                discovery.teaching_client_bound == selected_source,
                "CONFIG candidate matches the exact selected source, never a second address on that interface");
        const auto result = service.attempt_with_requirements(credentials, CampusArea::teaching, CancelToken{}, true);
        t.check(selected_source ? result.outcome == Outcome::success :
                result.outcome == Outcome::security && !result.credential_submission_started,
                "source-bound CONFIG requirement reaches the real attempt implementation");
        t.check(backend.count("/cgi-bin/get_challenge") == (selected_source ? 1 : 0) &&
                backend.count("/cgi-bin/srun_portal") == (selected_source ? 1 : 0),
                "non-bound CONFIG stops before challenge and password-derived submission");
    }
    const std::vector<std::string> invalid_pages{
        kHome,
        "<html>Shenzhen University srun login<script>var ac_id=12; /* var CONFIG={ip:'192.0.2.7',acid:12}; */</script></html>",
        "<html>Shenzhen University srun login<script>var CONFIG={ip:'192.0.2.7',acid:12};var CONFIG={ip:'192.0.2.7',acid:12};</script></html>",
        "<html>Shenzhen University srun login<script>var CONFIG={ip:'192.0.2.7',ip:'192.0.2.7',acid:12};</script></html>",
        "<html>Shenzhen University srun login<script>var CONFIG={ip:'192.0.2.7',acid:12};CONFIG.ip='192.0.2.9';</script></html>",
        "<html>Shenzhen University srun login<script src='fixture.js'>var CONFIG={ip:'192.0.2.7',acid:12};</script></html>",
        "<html>Shenzhen University srun login<script type='application/json'>var CONFIG={ip:'192.0.2.7',acid:12};</script></html>"
    };
    for (const auto& html : invalid_pages) {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A"));
        backend.respond = [html](const auto& request, const auto&, const auto&) {
            if (wire::parse_url(request.url)->path == "/srun_portal_pc") return success(html);
            return normal(request);
        };
        const auto discovery = service.discover(CampusArea::teaching, CancelToken{});
        t.check(discovery.teaching == Discovery::confirmed && !discovery.teaching_client_bound,
                "independent campus identity does not turn missing or unsafe CONFIG into a bound candidate");
        const auto result = service.attempt_with_requirements(credentials, CampusArea::teaching, CancelToken{}, true);
        t.check(result.outcome == Outcome::security && !result.credential_submission_started &&
                backend.count("/cgi-bin/get_challenge") == 0 && backend.count("/cgi-bin/srun_portal") == 0,
                "strong CONFIG attempts reject missing, duplicate, mutated or non-inline declarations before challenge");
    }
    {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A"));
        int home_reads = 0; bool fresh_controller = false, exact_account = false;
        backend.respond = [&](const auto& request, const auto&, const auto&) {
            const auto url = wire::parse_url(request.url);
            if (url->path == "/srun_portal_pc") return success(configured_home(++home_reads == 1 ? "6" : "12", "192.0.2.7"));
            if (url->path == "/cgi-bin/srun_portal") {
                fresh_controller = url->query.find("ac_id=12") != std::string::npos;
                exact_account = url->query.find("username=fixture-account&") != std::string::npos || url->query.ends_with("username=fixture-account");
            }
            return normal(request);
        };
        InternetCheckService decorated(service, [](const auto&, bool, const auto&) { return WebsiteCheckResult{}; });
        t.check(decorated.set_expected_physical_route(route("A")) &&
                decorated.discover(CampusArea::automatic, CancelToken{}).teaching_client_bound,
                "the website decorator preserves a fresh bound discovery from the physical service");
        const auto result = decorated.attempt_with_requirements(credentials, CampusArea::teaching, CancelToken{}, true);
        t.check(result.outcome == Outcome::success && home_reads == 2 && fresh_controller && exact_account &&
                backend.count("/cgi-bin/srun_portal") == 1,
                "attempt re-reads the controller through the production decorator and submits the unchanged account once");
    }
    for (const bool change_config : {false, true}) {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A"));
        int reads = 0;
        backend.respond = [&](const auto& request, const auto&, const auto&) {
            const auto url = wire::parse_url(request.url);
            if (url->path == "/srun_portal_pc") {
                ++reads;
                return success(change_config && reads > 1 ? std::string(kHome) : configured_home("12", "192.0.2.7"));
            }
            if (!change_config && url->path == "/cgi-bin/get_challenge")
                return success(R"({"error":"ok","res":"ok","challenge":"fixture-challenge","client_ip":"192.0.2.99"})");
            return normal(request);
        };
        t.check(service.discover(CampusArea::automatic, CancelToken{}).teaching_client_bound,
                "the first discovery supplies a real source-bound CONFIG candidate");
        InternetCheckService decorated(service, [](const auto&, bool, const auto&) { return WebsiteCheckResult{}; });
        const auto result = decorated.attempt_with_requirements(credentials, CampusArea::teaching, CancelToken{}, true);
        t.check(result.outcome == Outcome::security && !result.credential_submission_started &&
                backend.count("/cgi-bin/srun_portal") == 0 && backend.count("/eportal/portal/login") == 0 &&
                backend.count("/cgi-bin/get_challenge") == (change_config ? 0 : 1),
                "changed CONFIG and mismatching challenge sources stop at their respective stage without protocol fallback");
    }
    {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A"));
        backend.respond = [](const auto&, const auto&, const auto&) { HttpResponse result; result.error = HttpError::tls; return result; };
        const auto discovery = service.discover(CampusArea::automatic, CancelToken{});
        const auto result = service.attempt_with_requirements(credentials, CampusArea::teaching, CancelToken{}, true);
        t.check(discovery.teaching == Discovery::unconfirmed && !discovery.teaching_client_bound &&
                result.outcome == Outcome::security && !result.credential_submission_started && backend.count("/eportal") == 0,
                "CONFIG cannot survive TLS failure or cause a plaintext discovery downgrade");
    }
    for (const bool replace : {false, true}) {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A"));
        CancelToken token; Gate gate;
        backend.respond = [&](const auto& request, const auto&, const auto&) {
            if (wire::parse_url(request.url)->path == "/srun_portal_pc") { gate.block(); return success(configured_home("12", "192.0.2.7")); }
            return normal(request);
        };
        CampusDiscovery discovery;
        std::thread running([&] { discovery = service.discover(CampusArea::automatic, token); });
        const bool entered = gate.wait();
        if (replace) service.set_expected_physical_route(route("B")); else token.cancel();
        gate.release(); running.join();
        t.check(entered && discovery.teaching == Discovery::unconfirmed && !discovery.teaching_client_bound &&
                backend.count("/cgi-bin/get_challenge") == 0 && backend.count("/eportal") == 0,
                "cancelled or replaced discovery discards late CONFIG and never passes its authority onward");
    }
}

void verify_config_boundary_requests(Checks& t) {
    const Credentials credentials{"fixture-account", "not-a-real-password"};
    const std::string literal = "<script>var CONFIG={acid:12,ip:'192.0.2.7'};</script>";
    const std::vector<std::string> rejected{
        "<script>var CONFIG={acid:00012,ip:'192.0.2.7'};</script>",
        "<script>var CONFIG={acid:12,ip:'192.0.2.7',extra:{01:1,1:2}};</script>",
        "<script>var CONFIG={acid:12,ip:'192.0.2.7',extra:1/* gap */.2};</script>",
        literal + "<script>window?.['window']?.[key].ip='192.0.2.9';</script>",
        literal + "<script>((self))?.[key].ip='192.0.2.9';</script>",
        "<script><!-- <script></script>" + literal + " --> </script>",
        "<math>" + literal + "</math>",
        "<frameset></frameset>" + literal,
        literal + "<svg><template><script>window[key].ip='192.0.2.9';</script></template></svg>",
        literal + "<math><template><script>window[key].ip='192.0.2.9';</script></template></math>",
        literal + "<?fixture value=\"><script>window[key].ip='192.0.2.9';</script>\">",
        literal + "<!fixture value=\"><script>window[key].ip='192.0.2.9';</script>\">",
        literal + "<!-- x --!><script>window[key].ip='192.0.2.9';</script><!-- y -->",
        literal + "<script>// fixture comment\xE2\x80\xA8window[key].ip='192.0.2.9';</script>"
    };
    for (const auto& fragment : rejected) {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A"));
        backend.respond = [fragment](const auto& request, const auto&, const auto&) {
            if (wire::parse_url(request.url)->path == "/srun_portal_pc") return success(std::string(kHome) + fragment);
            return normal(request);
        };
        const auto discovery = service.discover(CampusArea::automatic, CancelToken{});
        t.check(discovery.teaching == Discovery::confirmed && discovery.dormitory == Discovery::confirmed &&
                !discovery.teaching_client_bound,
                "a valid campus service page does not authorize an ambiguous CONFIG candidate");
        const auto attempt = service.attempt_with_requirements(credentials, CampusArea::teaching, CancelToken{}, true);
        t.check(attempt.outcome == Outcome::security && !attempt.credential_submission_started &&
                backend.count("/cgi-bin/get_challenge") == 0 && backend.count("/cgi-bin/srun_portal") == 0 &&
                backend.count("/eportal/portal/login") == 0,
                "numeric, computed or HTML ambiguity stops strong preflight before account-bearing requests");
    }
    for (const std::string& fragment : std::vector<std::string>{
            "<script>var CONFIG={acid:'00012',ip:'192.0.2.7',extra:{0:0,'01':1},decimal:-1.2};</script>",
            literal + "<svg><path d='M0 0'/></svg><math><mi>x</mi></math>",
            literal + "<template><svg>" + literal + "</svg><frameset></frameset></template>",
            literal + "<script>const key=0;const values=[1,2];values[key]=1;window?.['unrelated'];</script>"}) {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A"));
        backend.respond = [fragment](const auto& request, const auto&, const auto&) {
            if (wire::parse_url(request.url)->path == "/srun_portal_pc") return success(std::string(kHome) + fragment);
            return normal(request);
        };
        t.check(service.discover(CampusArea::automatic, CancelToken{}).teaching_client_bound,
                "supported literal pages remain source-bound through production discovery");
        const auto attempt = service.attempt_with_requirements(credentials, CampusArea::teaching, CancelToken{}, true);
        t.check(attempt.outcome == Outcome::success && attempt.credential_submission_started &&
                backend.count("/cgi-bin/get_challenge") == 1 && backend.count("/cgi-bin/srun_portal") == 1 &&
                backend.count("/eportal/portal/login") == 0,
                "supported CONFIG pages retain the single teaching challenge and credential submission");
    }
}

void verify_discovery_failure_phases(Checks& t) {
    struct FailureCase { HttpError error; HttpFailureStage stage; int status; CampusVerificationIssue issue; };
    const FailureCase cases[] = {
        {HttpError::timeout, HttpFailureStage::dns, 0, CampusVerificationIssue::dnsTimeout},
        {HttpError::dns, HttpFailureStage::dns, 0, CampusVerificationIssue::dns},
        {HttpError::dns, HttpFailureStage::none, 0, CampusVerificationIssue::dns},
        {HttpError::timeout, HttpFailureStage::transport, 0, CampusVerificationIssue::connection},
        {HttpError::timeout, HttpFailureStage::none, 0, CampusVerificationIssue::connection},
        {HttpError::network, HttpFailureStage::binding, 0, CampusVerificationIssue::binding},
        {HttpError::network, HttpFailureStage::transport, 0, CampusVerificationIssue::connection},
        {HttpError::tls, HttpFailureStage::transport, 0, CampusVerificationIssue::tls},
        {HttpError::tls, HttpFailureStage::dns, 0, CampusVerificationIssue::dnsTls},
        {HttpError::tls, HttpFailureStage::none, 0, CampusVerificationIssue::tls},
        {HttpError::redirect, HttpFailureStage::none, 302, CampusVerificationIssue::page},
        {HttpError::tooLarge, HttpFailureStage::transport, 0, CampusVerificationIssue::page},
        {HttpError::none, HttpFailureStage::none, 403, CampusVerificationIssue::http},
        {HttpError::none, HttpFailureStage::none, 200, CampusVerificationIssue::page},
    };
    for (const auto& test : cases) {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A"));
        backend.respond = [test](const auto&, const auto&, const auto&) {
            HttpResponse value; value.error = test.error; value.failure_stage = test.stage;
            value.status = test.status; value.body = "unrelated fixture page"; return value;
        };
        const auto failed = service.discover(CampusArea::automatic, CancelToken{});
        t.check(failed.teaching == Discovery::unconfirmed && failed.dormitory == Discovery::notChecked &&
            !failed.teaching_client_bound && failed.issue == test.issue && backend.calls.size() == 1,
            "failed independent HTTPS identity preserves only its precise anonymous phase and never downgrades to HTTP");
        backend.respond = {};
        const auto recovered = service.discover(CampusArea::automatic, CancelToken{});
        t.check(recovered.teaching == Discovery::confirmed && recovered.dormitory == Discovery::confirmed &&
            recovered.issue == CampusVerificationIssue::none && backend.count("/cgi-bin/get_challenge") == 0 &&
            backend.count("/cgi-bin/srun_portal") == 0 && backend.count("/eportal/portal/login") == 0,
            "fresh successful discovery clears old diagnostics without sending any account-bearing request");

        auto attachment = route("A");
        attachment.descriptor.kind = "ethernet";
        attachment.descriptor.raw = {{"gw", "02:00:00:00:00:07"}, {"gwip", "192.0.2.254"}, {"net", "192.0.2.0/24"}};
        double now = 1000, wall = 1800000000; int writes = 0;
        ConnectionEngine::Hooks hooks;
        hooks.now = [&] { return now; }; hooks.wall_now = [&] { return wall; };
        hooks.persist_recovery = [](const std::string&) { return true; };
        hooks.persist_memory = [&](const std::string&) { ++writes; return true; };
        hooks.random_hex = [] { return "00112233445566778899aabbccddeeff"; };
        ConnectionEngine engine(service, hooks);
        engine.restore_recovery(std::nullopt);
        engine.configure(MonitoringMode::recover, CampusArea::teaching, ProbeMode::complete,
            Credentials{"fixture-account", "not-a-real-password"});
        NetworkMemory memory("00112233445566778899aabbccddeeff");
        memory.remember(attachment.descriptor, "teaching", true, false, wall - 100, "abcde107");
        const auto memory_bytes = memory.encode(); engine.restore_memory(memory_bytes);
        PathSnapshot path; path.identity = "phase-fixture-attachment";
        path.usable = path.link_active = path.requires_physical_binding = true;
        path.physical_candidates = {attachment}; path.descriptors = {attachment.descriptor};
        engine.receive_path(path);
        backend.respond = [test](const auto&, const auto&, const auto&) {
            HttpResponse value; value.error = test.error; value.failure_stage = test.stage;
            value.status = test.status; value.body = "unrelated fixture page"; return value;
        };
        const auto request = engine.prepare_verify_campus();
        const auto outcome = request ? engine.verify_campus(*request) : ConnectionEngine::CampusVerificationResult::unavailable;
        const auto snapshot = engine.snapshot();
        const char* hint = test.issue == CampusVerificationIssue::dnsTimeout ? "campus.verify.dnsTimeoutHint" :
            test.issue == CampusVerificationIssue::dns ? "campus.verify.dnsHint" :
            test.issue == CampusVerificationIssue::dnsTls ? "campus.verify.dnsTlsHint" :
            test.issue == CampusVerificationIssue::tls ? "campus.verify.tlsHint" :
            test.issue == CampusVerificationIssue::binding ? "campus.verify.bindingHint" :
            test.issue == CampusVerificationIssue::page ? "campus.verify.pageHint" :
            test.issue == CampusVerificationIssue::http ? "campus.verify.httpHint" : "campus.verify.connectionHint";
        t.check(request && outcome == ConnectionEngine::CampusVerificationResult::needsProof && snapshot.last_discovery &&
            snapshot.last_discovery->issue == test.issue && status::campus_verification_hint_key(snapshot) == hint &&
            !snapshot.trusted && snapshot.holds.empty() && writes == 0,
            "production discovery maps an injected transport failure through a real engine operation into the displayed reason");
        t.check(backend.count("/cgi-bin/get_challenge") == 0 && backend.count("/cgi-bin/srun_portal") == 0 &&
            backend.count("/eportal/portal/login") == 0,
            "the full service-to-engine diagnostic pipeline never submits credentials or requests a challenge");
    }
    for (const auto& test : cases) {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A"));
        backend.respond = [test](const auto& request, const auto&, const auto&) {
            if (wire::parse_url(request.url)->path == "/srun_portal_pc") return normal(request);
            HttpResponse value; value.error = test.error; value.failure_stage = test.stage;
            value.status = test.status; value.body = "unrelated fixture page"; return value;
        };
        const auto failed = service.discover(CampusArea::automatic, CancelToken{});
        t.check(failed.teaching == Discovery::confirmed && failed.dormitory == Discovery::unconfirmed &&
            failed.issue == test.issue && backend.count("/srun_portal_pc") == 1 && backend.count("/eportal") == 1,
            "a dormitory-service failure remains distinct from the successful independent HTTPS identity");
        const auto teaching = service.discover(CampusArea::teaching, CancelToken{});
        t.check(teaching.teaching == Discovery::confirmed && teaching.dormitory == Discovery::notChecked &&
            teaching.issue == CampusVerificationIssue::none && backend.count("/eportal") == 1,
            "teaching-only verification does not inherit an earlier dormitory failure");
    }
    for (const bool replace : {false, true}) for (const bool in_dormitory : {false, true})
        for (const bool dns_tls : {false, true}) {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A"));
        Gate gate; CancelToken token;
        backend.respond = [&](const auto& request, const auto&, const auto&) {
            const bool dormitory = wire::parse_url(request.url)->path == "/eportal";
            if (dormitory != in_dormitory) return normal(request);
            gate.block(); HttpResponse failure; failure.error = dns_tls ? HttpError::tls : HttpError::timeout;
            failure.failure_stage = HttpFailureStage::dns; return failure;
        };
        CampusDiscovery result;
        std::thread worker([&] { result = service.discover(CampusArea::automatic, token); });
        const bool entered = gate.wait();
        if (replace) service.set_expected_physical_route(route("B")); else token.cancel();
        gate.release(); worker.join();
        t.check(entered && result.teaching == Discovery::unconfirmed && result.dormitory == Discovery::notChecked &&
            result.issue == CampusVerificationIssue::none && !result.teaching_client_bound,
            "cancelled or replaced discovery discards both a late DNS failure and any intermediate identity result");
    }
    {
        Backend backend; WinHttpService service(backend.hooks());
        t.check(service.discover(CampusArea::automatic, CancelToken{}).issue == CampusVerificationIssue::binding &&
            backend.calls.empty(), "an absent physical binding has its own reason and makes no request");
        CancelToken token; token.cancel();
        t.check(service.discover(CampusArea::automatic, token).issue == CampusVerificationIssue::none &&
            backend.calls.empty(), "pre-cancellation does not manufacture a missing-binding failure");
    }
}
}

void verify_session_expiry_recovery(Checks& t) {
    for (const auto target : {CampusArea::teaching, CampusArea::dormitory})
    for (const auto mode : {ProbeMode::quick, ProbeMode::complete})
    for (const bool tunnel : {false, true}) {
        Backend backend; WinHttpService physical_service(backend.hooks());
        std::atomic<bool> expired{false};
        std::atomic<int> website_calls{0};
        backend.respond = [&](const HttpRequest& request, const CancelToken& token, const auto&) {
            const auto url = wire::parse_url(request.url);
            if (url->host == "www.msftconnecttest.com" && expired) {
                token.wait_for(0.01);
                HttpResponse response; response.status = 302; response.error = HttpError::redirect;
                response.campus_area_hint = wire::campus_redirect_hint(target == CampusArea::dormitory
                    ? "http://172.30.255.42/a70.htm?wlanuserip=192.0.2.7"
                    : "https://net.szu.edu.cn/srun_portal_pc?ac_id=12", request.url);
                response.authentication_required = true;
                return response;
            }
            if (url->path == "/eportal/portal/login" || url->path == "/cgi-bin/srun_portal") expired = false;
            return normal(request);
        };
        InternetCheckService service(physical_service, [&](const auto&, bool, const auto&) {
            ++website_calls; return WebsiteCheckResult{"accessible", 200, 0};
        });
        service.configure({{"fixture-site", "https://website.fixture.invalid/", true}}, true);
        auto attachment = route("A");
        attachment.descriptor.kind = "wifi";
        attachment.descriptor.raw = {{"ssid", "fixture-campus"}, {"gw", "02:00:00:00:00:07"},
            {"gwip", "192.0.2.254"}, {"net", "192.0.2.0/24"}};
        double now = 1000, wall = 1800000000;
        ConnectionEngine::Hooks hooks;
        hooks.now = [&] { return now; }; hooks.wall_now = [&] { return wall; };
        hooks.sleep = [](double) {};
        hooks.persist_recovery = [](const std::string&) { return true; };
        hooks.persist_memory = [](const std::string&) { return true; };
        hooks.random_hex = [] { return "00112233445566778899aabbccddeeff"; };
        ConnectionEngine engine(service, hooks);
        engine.restore_recovery(std::nullopt);
        engine.configure(MonitoringMode::recover, CampusArea::automatic, mode, Credentials{"fixture-account", "not-a-real-password"});
        NetworkMemory memory("00112233445566778899aabbccddeeff");
        memory.remember(attachment.descriptor, std::string(to_string(target)), true, true, wall - 100, "abcde107");
        engine.restore_memory(memory.encode());
        PathSnapshot path; path.identity = "session-expiry-attachment"; path.tunnel = tunnel;
        path.usable = path.link_active = path.requires_physical_binding = true;
        path.physical_candidates = {attachment}; path.descriptors = {attachment.descriptor};
        engine.receive_path(path); engine.automatic_round();
        const auto epoch = engine.snapshot().epoch;
        t.check(engine.snapshot().online && backend.count("/eportal/portal/login") == 0 && backend.count("/cgi-bin/srun_portal") == 0,
            "healthy remembered Wi-Fi does not submit credentials with or without a tunnel");
        expired = true; now += 61; wall += 61;
        engine.automatic_round();
        t.check(backend.count("/eportal/portal/login") == 0 && backend.count("/cgi-bin/srun_portal") == 0,
            "one expired-session observation still needs confirmation");
        now += 6; wall += 6; engine.automatic_round();
        t.check(engine.snapshot().online && engine.snapshot().epoch == epoch && !expired &&
            backend.count(target == CampusArea::dormitory ? "/eportal/portal/login" : "/cgi-bin/srun_portal") == 1 &&
            backend.count(target == CampusArea::dormitory ? "/cgi-bin/srun_portal" : "/eportal/portal/login") == 0,
            "production transport, website decorator and engine recover an unchanged-link session with one correctly routed fake submission");
        t.check(backend.count("/a70.htm") == 0 && backend.count("/a79.htm") == 0 &&
            engine.snapshot().probe_mode == mode && (mode != ProbeMode::quick || website_calls == 0),
            "browser redirects are not followed, quick settings are preserved and base audits do not query custom websites");
        t.check(engine.snapshot().cooldown_remaining.has_value(), "service recovery keeps the post-submission cooldown");
    }
}

int main() {
    Checks t;
    verify_website_transport(t);
    verify_compatibility_current_path(t);
    verify_config_boundary_requests(t);
    verify_discovery_failure_phases(t);
    verify_source_bound_wired_handover(t);
    verify_session_expiry_recovery(t);
    const Credentials credentials{"fixture-account", "not-a-real-password"};
    {
        Backend backend; WinHttpService service(backend.hooks()); CancelToken token;
        const auto attempt = service.attempt(credentials, CampusArea::teaching, token);
        t.check(attempt.outcome == Outcome::cancelled && !attempt.credential_submission_started, "missing expected physical route cannot send credentials");
        t.check(service.discover(CampusArea::automatic, token).teaching == Discovery::unconfirmed, "missing route cannot borrow a campus proof");
        const auto report = service.probe(ProbeMode::quick, token);
        t.check(report.apple.state == EndpointState::cancelled && report.microsoft.state == EndpointState::cancelled && backend.calls.empty(), "missing route never falls back to unbound probes");
    }
    {
        Backend backend; WinHttpService service(backend.hooks()); CancelToken token;
        t.check(service.set_expected_physical_route(route("A")), "current selected route pins");
        const auto discovery = service.discover(CampusArea::teaching, token);
        t.check(discovery.teaching == Discovery::confirmed && discovery.dormitory == Discovery::notChecked && backend.count("/eportal") == 0, "teaching-only discovery cannot grant dormitory scope");
        const auto attempt = service.attempt(credentials, CampusArea::teaching, token);
        t.check(attempt.outcome == Outcome::success && backend.count("/cgi-bin/srun_portal") == 1, "teaching performs one submission after fresh homepage and challenge");
        service.probe(ProbeMode::complete, token);
        bool auth_cookies = true, probe_cookies = false;
        for (const auto& call : backend.calls) {
            if (call.path.starts_with("/cgi-bin/") || (call.path == "/srun_portal_pc" && call.cookies)) auth_cookies &= call.cookies && call.request_cookies;
            if (call.path == "/connecttest.txt" || call.path == "/library/test/success.html") probe_cookies |= call.cookies || call.request_cookies;
        }
        t.check(auth_cookies && !probe_cookies, "authentication and probes never share cookie-enabled sessions");
        t.check(service.attempt(credentials, CampusArea::automatic, token).error == PortalError::forbiddenTarget, "service refuses ambiguous authentication area");
    }
    for (const auto area : {CampusArea::teaching, CampusArea::dormitory})
        for (const auto stage : {HttpFailureStage::none, HttpFailureStage::dns, HttpFailureStage::transport}) {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A")); CancelToken token;
        backend.respond = [stage](const auto&, const auto&, const auto&) {
            HttpResponse r; r.error = HttpError::tls; r.failure_stage = stage; return r;
        };
        const auto discovery = service.discover(CampusArea::automatic, token);
        const auto attempt = service.attempt(credentials, area, token);
        t.check(discovery.teaching == Discovery::unconfirmed && backend.count("/eportal") == 0, "TLS identity failure never downgrades discovery to HTTP dormitory");
        t.check(attempt.outcome == Outcome::security && attempt.error == PortalError::tls && !attempt.credential_submission_started &&
                backend.count("/eportal/portal/login") == 0 && backend.count("/cgi-bin/srun_portal") == 0, "TLS preflight failure cannot submit or create uncertain submission");
    }
    {
        Backend backend;
        auto hooks = backend.hooks();
        WinHttpService* active = nullptr;
        bool repin_during_read = false;
        hooks.route_current = [&](const physical::Route&) {
            if (repin_during_read) { repin_during_read = false; active->set_expected_physical_route(route("B")); }
            return true;
        };
        WinHttpService service(std::move(hooks)); active = &service;
        service.set_expected_physical_route(route("A")); repin_during_read = true;
        const auto discovery = service.discover(CampusArea::automatic, CancelToken{});
        t.check(discovery.teaching == Discovery::unconfirmed && backend.calls.empty() && backend.cookie_sessions.empty(),
            "binding replacement during session lease validation cannot construct or use a stale session");
    }
    {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A")); CancelToken token;
        const auto attempt = service.attempt(credentials, CampusArea::dormitory, token);
        t.check(attempt.outcome == Outcome::success && backend.calls.size() == 3 && backend.calls[0].path == "/srun_portal_pc" &&
            backend.calls[1].path == "/eportal" && backend.calls[2].path == "/eportal/portal/login", "dormitory session freshly proves HTTPS campus then local page before its sole submission");
    }
    for (const bool sent : {false, true}) {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A")); CancelToken token;
        backend.respond = [sent](const auto& request, const auto&, const auto&) {
            if (wire::parse_url(request.url)->path == "/cgi-bin/srun_portal") {
                HttpResponse r; r.error = HttpError::timeout; r.request_started = sent; return r;
            }
            return normal(request);
        };
        const auto attempt = service.attempt(credentials, CampusArea::teaching, token);
        t.check(attempt.credential_submission_started == sent && attempt.outcome == (sent ? Outcome::uncertain : Outcome::unavailable), "native pre-send evidence distinguishes preflight failure from unknown submission");
        t.check(backend.count("/cgi-bin/srun_portal") == 1, "credential-bearing request never retries after timeout");
    }
    for (const bool sent : {false, true}) {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A")); CancelToken token;
        backend.respond = [sent, &token](const auto& request, const auto&, const auto&) {
            if (wire::parse_url(request.url)->path == "/cgi-bin/srun_portal") {
                token.cancel(); HttpResponse r; r.error = HttpError::cancelled; r.request_started = sent; return r;
            }
            return normal(request);
        };
        const auto attempt = service.attempt(credentials, CampusArea::teaching, token);
        t.check(attempt.credential_submission_started == sent && attempt.outcome == (sent ? Outcome::uncertain : Outcome::cancelled), "cancel before HTTP gate is unsent; cancel after gate remains uncertain");
        t.check(backend.count("/cgi-bin/srun_portal") == 1, "cancellation cannot trigger second submission");
    }
    {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A")); CancelToken token; Gate gate;
        backend.respond = [&](const auto& request, const auto&, const auto&) {
            if (wire::parse_url(request.url)->path == "/srun_portal_pc") gate.block(); return normal(request);
        };
        LoginAttempt attempt;
        std::thread running([&] { attempt = service.attempt(credentials, CampusArea::teaching, token); });
        const bool entered = gate.wait();
        auto cosmetic = route("A"); cosmetic.display_name = "display changed"; cosmetic.is_system_primary = true;
        const bool accepted = service.set_expected_physical_route(cosmetic);
        gate.release(); running.join();
        t.check(entered && accepted && attempt.outcome == Outcome::success, "TUN/system-primary/display changes do not revoke the same bound lease");
    }
    {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A")); CancelToken token; Gate gate;
        backend.respond = [&](const auto& request, const auto&, const auto&) {
            if (wire::parse_url(request.url)->path == "/srun_portal_pc") gate.block(); return normal(request);
        };
        LoginAttempt attempt;
        std::thread running([&] { attempt = service.attempt(credentials, CampusArea::teaching, token); });
        const bool entered = gate.wait(); service.set_expected_physical_route(route("B")); gate.release(); running.join();
        t.check(entered && attempt.outcome == Outcome::cancelled && !attempt.credential_submission_started && backend.count("/cgi-bin/srun_portal") == 0, "new pin cancels old session and rejects even late homepage success");
        backend.respond = {};
        t.check(service.attempt(credentials, CampusArea::teaching, token).outcome == Outcome::success && backend.calls.back().identity == "B", "new session uses only newly pinned route");
        t.check(backend.cookie_sessions.size() == 2, "old session cannot restore its authentication cookies into the new pin");
    }
    {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A")); CancelToken token;
        backend.respond = [&](const auto& request, const auto&, const auto&) { backend.lease_current = false; return normal(request); };
        const auto attempt = service.attempt(credentials, CampusArea::teaching, token);
        t.check(attempt.outcome == Outcome::cancelled && backend.count("/cgi-bin/get_challenge") == 0, "actual selected lease mutation rejects late response even before path watcher repins");
        t.check(!service.set_expected_physical_route(route("B")), "stale requested lease is rejected");
        backend.lease_current = true; backend.respond = {};
        const auto report = service.probe(ProbeMode::quick, token);
        t.check(!report.online(), "rejected pin cannot silently recover by borrowing a newly current route");
        t.check(service.set_expected_physical_route(route("B")) && service.probe(ProbeMode::quick, token).online(), "explicit fresh repin admits only the requested route");
        service.set_expected_physical_route(std::nullopt);
        t.check(!service.probe(ProbeMode::quick, token).online(), "cleared expected lease stays fail-closed");
    }
    // Synchronize both endpoints so cancellation is checked while the slower
    // peer is in flight, not merely before it has started.
    for (const auto mode : {ProbeMode::quick, ProbeMode::complete}) for (const bool campus : {false, true}) {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A")); CancelToken token;
        std::atomic<bool> apple_entered{false}, peer_cancelled{false};
        backend.respond = [&](const auto& request, const CancelToken& peer, const auto&) {
            if (wire::parse_url(request.url)->host == "www.apple.com") {
                apple_entered = true;
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(120);
                while (!peer.cancelled() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                peer_cancelled = peer.cancelled();
                if (peer.cancelled()) { HttpResponse r; r.error = HttpError::cancelled; return r; }
                return normal(request);
            }
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!apple_entered && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            HttpResponse r; r.error = HttpError::redirect; r.status = 302; r.campus_redirect = campus;
            if (campus) r.campus_area_hint = CampusPortalHint::teaching;
            r.request_started = true; return r;
        };
        const auto report = service.probe(mode, token);
        const bool stop = mode == ProbeMode::quick && campus;
        t.check(peer_cancelled == stop, "only quick fixed campus redirect stops the slower peer; unknown/complete do not");
        t.check(report.microsoft.state == EndpointState::redirect && report.apple.state == (stop ? EndpointState::notCompleted : EndpointState::success), "early-stop report never fabricates success or peer failure");
        t.check(report.microsoft.campus_area_hint == (campus ? std::optional{CampusPortalHint::teaching} : std::nullopt),
            "fixed physical probe retains only the typed portal selection hint");
    }
    {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A"));
        backend.respond = [](const auto& request, const auto&, const auto&) {
            HttpResponse response; response.status = 302; response.error = HttpError::redirect;
            response.campus_area_hint = wire::parse_url(request.url)->host == "www.apple.com" ? CampusPortalHint::teaching : CampusPortalHint::dormitory;
            return response;
        };
        const auto report = service.probe(ProbeMode::complete, CancelToken{});
        t.check(report.apple.campus_area_hint == CampusPortalHint::teaching && report.microsoft.campus_area_hint == CampusPortalHint::dormitory,
            "one physical complete round keeps conflicting fixed-probe hints visible to engine policy");
        t.check(backend.count("/srun_portal_pc") == 0 && backend.count("/cgi-bin/srun_portal") == 0 && backend.count("/eportal/portal/login") == 0,
            "a redirect hint alone performs no identity request or credential submission");
        backend.respond = [](const auto&, const auto&, const auto&) {
            HttpResponse response = success("Microsoft Connect Test"); response.campus_area_hint = CampusPortalHint::dormitory;
            return response;
        };
        const auto successful = service.probe(ProbeMode::complete, CancelToken{});
        t.check(!successful.apple.campus_area_hint && !successful.microsoft.campus_area_hint,
            "nonredirect response cannot transport a campus selection hint");
    }
    {
        Backend backend; auto hooks = backend.hooks(); WinHttpService* active = nullptr;
        std::atomic<bool> replace_on_snapshot{false};
        hooks.route_current = [&](const physical::Route&) {
            if (replace_on_snapshot.exchange(false)) active->set_expected_physical_route(route("B"));
            return true;
        };
        WinHttpService service(std::move(hooks)); active = &service; service.set_expected_physical_route(route("A"));
        backend.respond = [&](const auto&, const auto&, const auto&) {
            replace_on_snapshot = true;
            HttpResponse response; response.status = 302; response.error = HttpError::redirect;
            response.campus_area_hint = CampusPortalHint::teaching; return response;
        };
        const auto report = service.probe(ProbeMode::complete, CancelToken{});
        t.check(report.apple.state == EndpointState::cancelled && report.microsoft.state == EndpointState::cancelled &&
            !report.apple.campus_area_hint && !report.microsoft.campus_area_hint,
            "replacement during postresponse snapshot cannot publish an old physical binding hint");
    }
    for (const bool replace_binding : {false, true}) {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A")); CancelToken token; Gate gate;
        backend.respond = [&](const auto&, const auto&, const auto&) {
            gate.block(); HttpResponse response; response.status = 302; response.error = HttpError::redirect;
            response.campus_area_hint = CampusPortalHint::teaching; return response;
        };
        ConnectivityReport report;
        std::thread running([&] { report = service.probe(ProbeMode::complete, token); });
        const bool entered = gate.wait();
        if (replace_binding) service.set_expected_physical_route(route("B")); else token.cancel();
        gate.release(); running.join();
        t.check(entered && report.apple.state == EndpointState::cancelled && report.microsoft.state == EndpointState::cancelled &&
            !report.apple.campus_area_hint && !report.microsoft.campus_area_hint,
            "cancelled or replaced physical round discards late fixed-probe hints");
    }
    verify_client_configuration(t);
    std::printf("Physical service isolation tests: %d checks, %d failures\n", t.count, t.failures);
    return t.failures ? 1 : 0;
}
