#include "szu/win/system_probe.hpp"
#include <mutex>
#include <thread>
#include "szu/portal_wire.hpp"

namespace szu::win {
namespace {
struct ChildToken {
    CancelToken token;
    CancelToken parent;
    std::uint64_t registration;
    explicit ChildToken(const CancelToken& value) : parent(value) {
        const CancelToken child = token;
        registration = parent.on_cancel([child] { child.cancel(); });
    }
    ~ChildToken() { parent.remove(registration); }
};
EndpointState endpoint_state(const HttpResponse& response, bool apple) {
    switch (response.error) {
        case HttpError::none:
            if (response.status >= 300 && response.status < 400) return EndpointState::redirect;
            return (apple ? wire::is_connectivity_success(response.body, response.status) :
                wire::is_microsoft_success(response.body, response.status)) ? EndpointState::success : EndpointState::unexpectedResponse;
        case HttpError::timeout: return EndpointState::timeout;
        case HttpError::dns: return EndpointState::dns;
        case HttpError::tls: return EndpointState::tls;
        case HttpError::cancelled: return EndpointState::cancelled;
        case HttpError::redirect: return EndpointState::redirect;
        case HttpError::tooLarge: return EndpointState::unexpectedResponse;
        case HttpError::network: return EndpointState::network;
    }
    return EndpointState::network;
}
}  // namespace

std::optional<ConnectivityReport> probe_system_connectivity(ProbeMode mode, const CancelToken& token, const SystemProbeGet& get) {
    if (token.cancelled() || !get) return std::nullopt;
    ConnectivityReport report; report.mode = mode;
    ChildToken apple_token(token), microsoft_token(token);
    std::mutex mutex;
    const auto run = [&](const char* url, bool apple) {
        HttpRequest request; request.url = url; request.proxy = ProxyMode::system;
        request.timeout_ms = 6000; request.max_bytes = wire::kMaxProbeBytes;
        const auto& child = apple ? apple_token.token : microsoft_token.token;
        const HttpResponse response = get(request, child);
        // A transport may finish after its handle was cancelled. That response cannot
        // replace the quick round's "not waited" result with late success or failure.
        std::lock_guard<std::mutex> lock(mutex);
        EndpointResult result{child.cancelled() ? EndpointState::cancelled : endpoint_state(response, apple), response.elapsed};
        (apple ? report.apple : report.microsoft) = result;
        if (mode == ProbeMode::quick && result.succeeded())
            (apple ? microsoft_token : apple_token).token.cancel();
    };
    std::thread apple(run, "https://www.apple.com/library/test/success.html", true);
    std::thread microsoft(run, "http://www.msftconnecttest.com/connecttest.txt", false);
    apple.join(); microsoft.join();
    if (token.cancelled()) return std::nullopt;
    if (mode == ProbeMode::quick && report.online()) {
        if (report.apple.state == EndpointState::cancelled) report.apple.state = EndpointState::notCompleted;
        if (report.microsoft.state == EndpointState::cancelled) report.microsoft.state = EndpointState::notCompleted;
    }
    // System-path redirects are display-only, even if an injected transport carries a hint.
    report.apple.campus_area_hint.reset(); report.microsoft.campus_area_hint.reset();
    return report;
}
}  // namespace szu::win
