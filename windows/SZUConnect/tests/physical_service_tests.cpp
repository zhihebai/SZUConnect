// Isolated production-service protocol/ownership tests. Every network response
// comes from an injected fixed-session fake. No account vault or OS route changes.
#include "szu/win/service.hpp"
#include "szu/portal_wire.hpp"

#include <atomic>
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
    struct Session final : FixedHttpSession {
        Backend& owner; physical::Route route; bool cookies; std::size_t id;
        std::function<bool()> current;
        Session(Backend& b, physical::Route r, bool c, std::size_t i, std::function<bool()> scope) : owner(b), route(std::move(r)), cookies(c), id(i), current(std::move(scope)) {}
        HttpResponse get(const HttpRequest& request, const CancelToken& token) override {
            const auto url = wire::parse_url(request.url);
            { std::lock_guard<std::mutex> lock(owner.mutex); owner.calls.push_back({route.identity, url ? url->path : "invalid", cookies, request.allow_cookies, id}); }
            // Deliberately allow scripts to return late success: the production
            // service must reject it independently of a cooperative transport.
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
}

int main() {
    Checks t;
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
    for (const auto area : {CampusArea::teaching, CampusArea::dormitory}) {
        Backend backend; WinHttpService service(backend.hooks()); service.set_expected_physical_route(route("A")); CancelToken token;
        backend.respond = [](const auto&, const auto&, const auto&) { HttpResponse r; r.error = HttpError::tls; return r; };
        const auto discovery = service.discover(CampusArea::automatic, token);
        const auto attempt = service.attempt(credentials, area, token);
        t.check(discovery.teaching == Discovery::unconfirmed && backend.count("/eportal") == 0, "TLS identity failure never downgrades discovery to HTTP dormitory");
        t.check(attempt.outcome == Outcome::security && !attempt.credential_submission_started &&
                backend.count("/eportal/portal/login") == 0 && backend.count("/cgi-bin/srun_portal") == 0, "TLS preflight failure cannot submit or create uncertain submission");
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
            HttpResponse r; r.error = HttpError::redirect; r.status = 302; r.campus_redirect = campus; r.request_started = true; return r;
        };
        const auto report = service.probe(mode, token);
        const bool stop = mode == ProbeMode::quick && campus;
        t.check(peer_cancelled == stop, "only quick fixed campus redirect stops the slower peer; unknown/complete do not");
        t.check(report.microsoft.state == EndpointState::redirect && report.apple.state == (stop ? EndpointState::notCompleted : EndpointState::success), "early-stop report never fabricates success or peer failure");
    }
    std::printf("Physical service isolation tests: %d checks, %d failures\n", t.count, t.failures);
    return t.failures ? 1 : 0;
}
