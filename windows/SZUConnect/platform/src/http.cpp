#include "szu/win/http.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

#include "szu/portal_wire.hpp"
#include "szu/environment.hpp"
#include "szu/win/text.hpp"

#pragma comment(lib, "winhttp.lib")

namespace szu::win {
namespace {

constexpr wchar_t kAgent[] = L"SZUConnect/1.1.0 (Windows)";
static_assert(recovery_connection_error(ERROR_WINHTTP_CANNOT_CONNECT));
static_assert(recovery_connection_error(ERROR_WINHTTP_CONNECTION_ERROR));

HttpError map_error(DWORD code) {
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT: return HttpError::timeout;
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return HttpError::dns;
        case ERROR_WINHTTP_OPERATION_CANCELLED: return HttpError::cancelled;
        case ERROR_WINHTTP_REDIRECT_FAILED: return HttpError::redirect;
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
        case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_REVOKED:
        case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
        case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
            return HttpError::tls;
        default:
            return HttpError::network;
    }
}

HINTERNET open_session(ProxyMode proxy) {
    const DWORD access = proxy == ProxyMode::none ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    HINTERNET session = WinHttpOpen(kAgent, access, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return nullptr;
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // older builds reject the TLS 1.3 bit
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
    return session;
}

// Closes the request handle exactly once, from whichever thread gets there first.
struct HandleGuard {
    std::mutex mutex;
    HINTERNET request = nullptr;
    HINTERNET connect = nullptr;
    bool closed = false;
    void close() {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed) return;
        closed = true;
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        // Handles remain immutable once callbacks can run; closed gates one-time release.
    }
    ~HandleGuard() { close(); }
    bool is_closed() {
        std::lock_guard<std::mutex> lock(mutex);
        return closed;
    }
};

class Deadline {
public:
    Deadline(int ms, HandleGuard& guard) : guard_(guard) {
        thread_ = std::thread([this, ms] {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return done_; })) {
                expired_ = true;
                lock.unlock();
                guard_.close();
            }
        });
    }
    ~Deadline() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    bool expired() {
        std::lock_guard<std::mutex> lock(mutex_);
        return expired_;
    }

private:
    HandleGuard& guard_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    bool expired_ = false;
    std::thread thread_;
};

HttpResponse perform(HINTERNET session, bool cookies, const HttpRequest& request, const CancelToken& token) {
    HttpResponse response;
    const auto started = std::chrono::steady_clock::now();
    auto finish = [&](HttpError error) {
        response.error = error;
        response.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return response;
    };
    auto finish_error = [&](DWORD code) {
        response.connection_failure = recovery_connection_error(code);
        return finish(map_error(code));
    };
    if (token.cancelled()) return finish(HttpError::cancelled);
    const auto url = wire::parse_url(request.url);
    if (!session || !url || url->has_userinfo || url->has_fragment || (url->scheme != "http" && url->scheme != "https"))
        return finish(HttpError::network);
    const bool secure = url->scheme == "https";
    const INTERNET_PORT port = static_cast<INTERNET_PORT>(url->port.value_or(secure ? 443 : 80));
    std::wstring object = widen(url->path.empty() ? "/" : url->path);
    if (url->has_query) object += L"?" + widen(url->query);

    auto lifetime = std::make_shared<HandleGuard>();
    HandleGuard& guard = *lifetime;
    guard.connect = WinHttpConnect(session, widen(url->host).c_str(), port, 0);
    if (!guard.connect) return finish_error(GetLastError());
    guard.request = WinHttpOpenRequest(guard.connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!guard.request) {
        const DWORD code = GetLastError();
        guard.close();
        return finish_error(code);
    }
    DWORD features = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_KEEP_ALIVE;
    if (!cookies || !request.allow_cookies) features |= WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(guard.request, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof features);
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(guard.request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    WinHttpSetOption(guard.request, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof autologon);
    const int phase = request.timeout_ms;
    WinHttpSetTimeouts(guard.request, phase, phase, phase, phase);

    const std::uint64_t registration = token.on_cancel([lifetime] { lifetime->close(); });
    Deadline deadline(request.timeout_ms, guard);
    auto fail = [&](DWORD code) {
        response.connection_failure = recovery_connection_error(code);
        HttpError error = map_error(code);
        if (token.cancelled()) error = HttpError::cancelled;
        else if (deadline.expired()) error = HttpError::timeout;
        else if (guard.is_closed()) error = HttpError::cancelled;
        token.remove(registration);
        guard.close();
        return finish(error);
    };

    const wchar_t headers[] = L"Cache-Control: no-store\r\nPragma: no-cache\r\nAccept: */*";
    response.request_started = true;  // WinHTTP cannot expose a later, source-bound pre-send gate.
    if (!WinHttpSendRequest(guard.request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return fail(GetLastError());
    if (!WinHttpReceiveResponse(guard.request, nullptr)) return fail(GetLastError());

    DWORD status = 0, size = sizeof status;
    if (!WinHttpQueryHeaders(guard.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &size, WINHTTP_NO_HEADER_INDEX))
        return fail(GetLastError());
    response.status = static_cast<int>(status);
    if (status >= 300 && status < 400) {
        // Classify one bounded Location in RAM. Missing/oversized/invalid values are
        // not candidates, and the original header never enters HttpResponse or logs.
        DWORD location_bytes = 0;
        WinHttpQueryHeaders(guard.request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                            nullptr, &location_bytes, WINHTTP_NO_HEADER_INDEX);
        if (location_bytes >= sizeof(wchar_t) && location_bytes <= 4098 && location_bytes % sizeof(wchar_t) == 0) {
            std::vector<wchar_t> location(location_bytes / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(guard.request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                   location.data(), &location_bytes, WINHTTP_NO_HEADER_INDEX)) {
                const auto end = std::find(location.begin(), location.end(), L'\0');
                // A terminator must not truncate an embedded control into a valid portal prefix.
                if (std::all_of(end, location.end(), [](wchar_t c) { return c == L'\0'; }))
                    response.campus_redirect = website::is_campus_redirect(
                        narrow(std::wstring_view(location.data(), static_cast<std::size_t>(end - location.begin()))), request.url);
            }
        }
    }
    if (request.headers_only || (status >= 300 && status < 400)) {
        token.remove(registration);
        guard.close();
        return finish(status >= 300 && status < 400 && !request.headers_only ? HttpError::redirect : HttpError::none);
    }
    DWORD declared = 0;
    size = sizeof declared;
    if (WinHttpQueryHeaders(guard.request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &declared, &size, WINHTTP_NO_HEADER_INDEX) &&
        declared > request.max_bytes) {
        token.remove(registration);
        guard.close();
        return finish(HttpError::tooLarge);
    }
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(guard.request, &available)) return fail(GetLastError());
        if (available == 0) break;
        if (response.body.size() + available > request.max_bytes) {
            token.remove(registration);
            guard.close();
            return finish(HttpError::tooLarge);
        }
        const std::size_t offset = response.body.size();
        response.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(guard.request, response.body.data() + offset, available, &read)) return fail(GetLastError());
        response.body.resize(offset + read);
        if (read == 0) break;
    }
    token.remove(registration);
    guard.close();
    if (token.cancelled()) return finish(HttpError::cancelled);
    return finish(HttpError::none);
}

}  // namespace

HttpSession::HttpSession(ProxyMode proxy, bool cookies) : handle_(open_session(proxy)), cookies_(cookies) {}

HttpSession::~HttpSession() {
    if (handle_) WinHttpCloseHandle(static_cast<HINTERNET>(handle_));  // drops the in-memory cookie jar
}

HttpResponse HttpSession::get(const HttpRequest& request, const CancelToken& token) {
    return perform(static_cast<HINTERNET>(handle_), cookies_, request, token);
}

HttpResponse http_get(const HttpRequest& request, const CancelToken& token) {
    HttpSession session(request.proxy, false);
    return session.get(request, token);
}

}  // namespace szu::win
