// Bounded Windows HTTP transports. Fixed campus/probe requests use the source-bound
// channel below; ordinary websites use WinHTTP. Both retain TLS, cancellation and limits.
#pragma once
#include <string>
#include <functional>
#include <memory>
#include <optional>

#include "szu/physical.hpp"

#include "szu/internet.hpp"

namespace szu::win {

enum class ProxyMode {
    none,   // WINHTTP_ACCESS_TYPE_NO_PROXY: what SZUConnect itself uses for probes and the portals
    system  // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY: what browsers following the system proxy do
};

enum class HttpError { none, timeout, dns, tls, network, cancelled, redirect, tooLarge };
enum class HttpFailureStage { none, binding, dns, transport };

// Stable WinHTTP CANNOT_CONNECT / CONNECTION_ERROR values. Unknown network errors
// remain non-candidates; this pure predicate also runs in portable offline tests.
constexpr bool recovery_connection_error(std::uint32_t code) { return code == 12029 || code == 12030; }

struct HttpRequest {
    std::string url;               // must be absolute http(s)
    ProxyMode proxy = ProxyMode::none;
    int timeout_ms = 6000;         // hard deadline for the whole request
    std::size_t max_bytes = 64 * 1024;
    bool headers_only = false;     // stop after the status line and headers
    bool allow_cookies = false;    // only inside one authentication session
};

struct HttpResponse {
    HttpError error = HttpError::none;
    int status = 0;
    std::string body;
    double elapsed = 0;            // seconds
    bool connection_failure = false;
    bool campus_redirect = false;  // destination classified in RAM, never retained or followed
    // Only the fixed, source-bound Apple/Microsoft transport sets this enum.
    // System-path and custom-website responses never select a campus protocol.
    std::optional<CampusPortalHint> campus_area_hint;
    // True only after the native pre-send gate passed. A failure before this
    // gate proves no credential-bearing HTTP request was started.
    bool request_started = false;
    HttpFailureStage failure_stage = HttpFailureStage::none;  // Anonymous fixed-transport phase, not response evidence.
    bool authentication_required = false;
    bool ok() const { return error == HttpError::none; }
    WebsiteRecoverySignal website_recovery_signal() const {
        if (error == HttpError::timeout || error == HttpError::dns ||
            (error == HttpError::network && connection_failure)) return WebsiteRecoverySignal::connectionFailure;
        if (error == HttpError::none && status >= 300 && status < 400 && campus_redirect)
            return WebsiteRecoverySignal::campusRedirect;
        return WebsiteRecoverySignal::none;
    }
};

// A session groups requests that may share an in-memory cookie jar (one authentication attempt).
class HttpSession {
public:
    explicit HttpSession(ProxyMode proxy, bool cookies);
    ~HttpSession();
    HttpSession(const HttpSession&) = delete;
    HttpSession& operator=(const HttpSession&) = delete;
    HttpResponse get(const HttpRequest& request, const CancelToken& token);
    bool valid() const { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;  // HINTERNET
    bool cookies_ = false;
};

// Fixed campus/probe requests use an independently bound physical attachment.
// The interface permits isolated protocol tests without starting native networking.
class FixedHttpSession {
public:
    virtual ~FixedHttpSession() = default;
    virtual HttpResponse get(const HttpRequest&, const CancelToken&) = 0;
};

class PhysicalHttpSession final : public FixedHttpSession {
public:
    // The callback checks binding ownership; the transport independently validates
    // the full physical lease before, during and after every request.
    PhysicalHttpSession(physical::Route route, bool cookies, std::function<bool()> binding_current);
    ~PhysicalHttpSession();
    PhysicalHttpSession(const PhysicalHttpSession&) = delete;
    PhysicalHttpSession& operator=(const PhysicalHttpSession&) = delete;
    HttpResponse get(const HttpRequest& request, const CancelToken& token) override;
private:
    struct State;
    std::unique_ptr<State> state_;
};

// One-shot helper for ordinary user-selected websites. Never use for fixed
// campus authentication or primary connectivity probes.
HttpResponse http_get(const HttpRequest& request, const CancelToken& token);

}  // namespace szu::win
