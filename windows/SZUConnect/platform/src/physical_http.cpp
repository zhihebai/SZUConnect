// Source/interface-bound, fixed-target HTTP. No shell, proxy, OS cookie store or
// system DNS participates. TLS still authenticates the original domain name.
#include "szu/win/http.hpp"
#include "szu/win/physical.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <charconv>
#include <ctime>
#include <optional>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <vector>

#include "szu/environment.hpp"
#include "szu/portal_wire.hpp"

namespace szu::win {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kHeaderLimit = 65536, kBodyLimit = 1048576, kUrlLimit = 65536;

struct Target { std::string host, scheme; unsigned port = 0; };

bool target_for(const std::string& text, Target& target) {
    if (text.empty() || text.size() > kUrlLimit ||
        std::any_of(text.begin(), text.end(), [](unsigned char c) { return c <= 32 || c == 127; })) return false;
    const auto url = wire::parse_url(text);
    if (!url || url->has_userinfo || url->has_fragment) return false;
    target = {url->host, url->scheme, static_cast<unsigned>(url->port.value_or(url->scheme == "https" ? 443 : 80))};
    const auto& path = url->path;
    if (target.scheme == "https" && target.host == "net.szu.edu.cn" && target.port == 443)
        return ((!url->has_query && (path.empty() || path == "/" || path == "/srun_portal_pc")) ||
                path == "/cgi-bin/get_challenge" || path == "/cgi-bin/srun_portal");
    if (target.scheme == "http" && target.host == "172.30.255.42" && target.port == 801)
        return ((!url->has_query && (path.empty() || path == "/" || path == "/eportal")) || path == "/eportal/portal/login");
    if (target.scheme == "https" && target.host == "www.apple.com" && target.port == 443)
        return !url->has_query && path == "/library/test/success.html";
    if (target.scheme == "http" && target.host == "www.msftconnecttest.com" && target.port == 80)
        return !url->has_query && path == "/connecttest.txt";
    return false;
}

bool valid_request(const HttpRequest& request, const physical::Route& route, Target& target) {
    return request.proxy == ProxyMode::none && request.timeout_ms > 0 && request.timeout_ms <= 12000 &&
        request.max_bytes > 0 && request.max_bytes <= kBodyLimit && !route.identity.empty() &&
        route.interface_index && route.interface_luid && physical::usable_ipv4(route.source_ipv4) &&
        std::find(route.addresses.begin(), route.addresses.end(), route.source_ipv4) != route.addresses.end() &&
        target_for(request.url, target);
}

struct Easy {
    CURL* value = curl_easy_init();
    ~Easy() { if (value) curl_easy_cleanup(value); }
};
struct List {
    curl_slist* value = nullptr;
    ~List() { curl_slist_free_all(value); }
    bool add(const std::string& text) {
        auto* next = curl_slist_append(value, text.c_str());
        if (!next) return false;
        value = next; return true;
    }
};

struct Transfer {
    enum class HeaderPhase { awaitingStatus, interimHeaders, finalHeaders, trailers };
    const physical::Route& route;
    const CancelToken& token;
    const std::function<bool()>& current;
    std::size_t limit;
    std::string body, headers;
    std::size_t header_bytes = 0;
    bool too_large = false, malformed = false, request_started = false, stale = false;
    bool headers_only = false, header_stop = false;
    HeaderPhase header_phase = HeaderPhase::awaitingStatus;
    int header_status = 0;
};

bool source_current(Transfer& transfer) {
    try {
        if (transfer.token.cancelled() || !transfer.current()) { transfer.stale = true; return false; }
        // The route validator also checks the selected physical gateway/DNS lease.
        // Native checks independently require this exact LUID/index/address to exist.
        NET_LUID luid{}; luid.Value = transfer.route.interface_luid;
        NET_IFINDEX actual = 0;
        if (ConvertInterfaceLuidToIndex(&luid, &actual) != NO_ERROR || actual != transfer.route.interface_index) {
            transfer.stale = true; return false;
        }
        MIB_UNICASTIPADDRESS_ROW row{};
        InitializeUnicastIpAddressEntry(&row);
        row.InterfaceLuid = luid;
        row.Address.si_family = AF_INET;
        if (InetPtonA(AF_INET, transfer.route.source_ipv4.c_str(), &row.Address.Ipv4.sin_addr) != 1 ||
            GetUnicastIpAddressEntry(&row) != NO_ERROR || row.DadState != IpDadStatePreferred) {
            transfer.stale = true; return false;
        }
        return true;
    } catch (...) { transfer.stale = true; return false; } // Never unwind through libcurl's C callbacks.
}

curl_socket_t open_socket(void* opaque, curlsocktype purpose, curl_sockaddr* address) {
    auto& transfer = *static_cast<Transfer*>(opaque);
    if (!address || purpose != CURLSOCKTYPE_IPCXN || address->family != AF_INET || !source_current(transfer)) return CURL_SOCKET_BAD;
    const SOCKET socket_value = socket(address->family, address->socktype, address->protocol);
    if (socket_value == INVALID_SOCKET) return CURL_SOCKET_BAD;
    // IP_UNICAST_IF expects the IPv4 interface index in network byte order.
    const DWORD index = htonl(transfer.route.interface_index);
    sockaddr_in source{}; source.sin_family = AF_INET;
    if (setsockopt(socket_value, IPPROTO_IP, IP_UNICAST_IF, reinterpret_cast<const char*>(&index), sizeof(index)) != 0 ||
        InetPtonA(AF_INET, transfer.route.source_ipv4.c_str(), &source.sin_addr) != 1 ||
        bind(socket_value, reinterpret_cast<const sockaddr*>(&source), sizeof(source)) != 0 || !source_current(transfer)) {
        closesocket(socket_value); return CURL_SOCKET_BAD;
    }
    return static_cast<curl_socket_t>(socket_value);
}

int prerequisite(void* opaque, char*, char* local_ip, int, int) {
    auto& transfer = *static_cast<Transfer*>(opaque);
    if (!local_ip || transfer.route.source_ipv4 != local_ip || !source_current(transfer)) return CURL_PREREQFUNC_ABORT;
    // libcurl invokes this after connection/TLS and immediately before the first
    // HTTP bytes. From this point a failed submission is conservatively uncertain.
    transfer.request_started = true;
    return CURL_PREREQFUNC_OK;
}
int progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto& transfer = *static_cast<Transfer*>(opaque);
    try { return transfer.token.cancelled() || !transfer.current() ? 1 : 0; }
    catch (...) { transfer.stale = true; return 1; }
}

std::size_t write_body(char* bytes, std::size_t size, std::size_t count, void* opaque) {
    auto& transfer = *static_cast<Transfer*>(opaque);
    if (transfer.token.cancelled()) return 0;
    if ((size && count > std::numeric_limits<std::size_t>::max() / size) || size * count > transfer.limit - transfer.body.size()) {
        transfer.too_large = true; return 0;
    }
    try { transfer.body.append(bytes, size * count); } catch (...) { transfer.too_large = true; return 0; }
    return size * count;
}
int status_from_header(std::string_view line) {
    if (!line.starts_with("HTTP/") || !line.ends_with("\r\n")) return 0;
    const auto space = line.find(' ', 5);
    if (space == std::string_view::npos || space == 5) return 0;
    const auto version = line.substr(5, space - 5);
    if (version.front() < '0' || version.front() > '9' || version.back() < '0' || version.back() > '9' ||
        std::count(version.begin(), version.end(), '.') > 1 || !std::all_of(version.begin(), version.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; })) return 0;
    auto begin = space;
    while (begin < line.size() && line[begin] == ' ') ++begin;
    if (begin + 3 >= line.size() || !std::all_of(line.begin() + begin, line.begin() + begin + 3, [](char c) { return c >= '0' && c <= '9'; }) ||
        (line[begin + 3] != ' ' && line[begin + 3] != '\r')) return 0;
    const int status = (line[begin] - '0') * 100 + (line[begin + 1] - '0') * 10 + line[begin + 2] - '0';
    return status >= 100 && status <= 599 ? status : 0;
}
std::size_t write_header(char* bytes, std::size_t size, std::size_t count, void* opaque) {
    auto& transfer = *static_cast<Transfer*>(opaque);
    if (transfer.token.cancelled()) return 0;
    if ((size && count > std::numeric_limits<std::size_t>::max() / size) || size * count > kHeaderLimit - transfer.header_bytes) {
        transfer.too_large = true; return 0;
    }
    const auto length = size * count;
    transfer.header_bytes += length;
    if (std::memchr(bytes, '\0', length)) { transfer.malformed = true; return 0; }
    // Once the final header delimiter has arrived, every later callback is a
    // trailer. Count its bytes but never save it, parse a new status, reset the
    // final block or let it add/replace authentication cookies and Location.
    using Phase = Transfer::HeaderPhase;
    if (transfer.header_phase == Phase::trailers) return length;
    try {
        const std::string_view line(bytes, length);
        if (transfer.header_phase == Phase::awaitingStatus) {
            const int status = status_from_header(line);
            if (!status) { transfer.malformed = true; return 0; }
            transfer.header_status = status;
            transfer.headers.clear();
            transfer.headers.append(bytes, length);
            transfer.header_phase = status < 200 ? Phase::interimHeaders : Phase::finalHeaders;
            return length;
        }
        transfer.headers.append(bytes, length);
        if (line == "\r\n") {
            if (transfer.header_phase == Phase::interimHeaders) {
                transfer.header_phase = Phase::awaitingStatus;
            } else {
                transfer.header_phase = Phase::trailers;
                if (transfer.headers_only || (transfer.header_status >= 300 && transfer.header_status < 400)) {
                    transfer.header_stop = true; return 0;
                }
            }
        }
    } catch (...) { transfer.too_large = true; return 0; }
    return length;
}

CURLcode configure_security(CURL* handle) {
#define SZU_SET(option, setting) do { const auto code = curl_easy_setopt(handle, option, setting); if (code != CURLE_OK) return code; } while (0)
    SZU_SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SZU_SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SZU_SET(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    // Schannel uses native roots with strict revocation. AUTO_CLIENT_CERT,
    // NO_REVOKE and REVOKE_BEST_EFFORT are deliberately absent.
    SZU_SET(CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
    SZU_SET(CURLOPT_SSLCERT, static_cast<const char*>(nullptr));
    SZU_SET(CURLOPT_SSLKEY, static_cast<const char*>(nullptr));
    SZU_SET(CURLOPT_FOLLOWLOCATION, 0L);
    SZU_SET(CURLOPT_MAXREDIRS, 0L);
    SZU_SET(CURLOPT_UNRESTRICTED_AUTH, 0L);
    SZU_SET(CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_NONE));
    SZU_SET(CURLOPT_PROXYAUTH, static_cast<long>(CURLAUTH_NONE));
    SZU_SET(CURLOPT_NETRC, CURL_NETRC_IGNORED);
    SZU_SET(CURLOPT_PROXY, "");
    SZU_SET(CURLOPT_PRE_PROXY, "");
    SZU_SET(CURLOPT_NOPROXY, "*");
    SZU_SET(CURLOPT_NOSIGNAL, 1L);
    SZU_SET(CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    SZU_SET(CURLOPT_PROTOCOLS_STR, "http,https");
    SZU_SET(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    SZU_SET(CURLOPT_DNS_CACHE_TIMEOUT, 0L);
    SZU_SET(CURLOPT_FRESH_CONNECT, 1L);
    SZU_SET(CURLOPT_FORBID_REUSE, 1L);
    SZU_SET(CURLOPT_VERBOSE, 0L);
    return CURLE_OK;
#undef SZU_SET
}

HttpError classify(CURLcode code, const Transfer& transfer) {
    if (transfer.token.cancelled() || transfer.stale || code == CURLE_ABORTED_BY_CALLBACK) return HttpError::cancelled;
    if (transfer.too_large || code == CURLE_FILESIZE_EXCEEDED) return HttpError::tooLarge;
    if (transfer.malformed) return HttpError::network;
    if (transfer.header_stop && code == CURLE_WRITE_ERROR) return HttpError::none;
    switch (code) {
        case CURLE_OK: return HttpError::none;
        case CURLE_OPERATION_TIMEDOUT: return HttpError::timeout;
        case CURLE_COULDNT_RESOLVE_HOST: return HttpError::dns;
        case CURLE_SSL_CONNECT_ERROR: case CURLE_PEER_FAILED_VERIFICATION: case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER: case CURLE_SSL_CACERT_BADFILE: case CURLE_SSL_ISSUER_ERROR:
        case CURLE_SSL_PINNEDPUBKEYNOTMATCH: case CURLE_SSL_CLIENTCERT:
        case CURLE_SSL_ENGINE_NOTFOUND: case CURLE_SSL_ENGINE_SETFAILED: case CURLE_SSL_ENGINE_INITFAILED:
        case CURLE_SSL_CRL_BADFILE: case CURLE_SSL_INVALIDCERTSTATUS: case CURLE_SSL_SHUTDOWN_FAILED:
        case CURLE_USE_SSL_FAILED: return HttpError::tls;
        default: return HttpError::network;
    }
}

std::string lowercase(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
// Exactly one bounded Location from the final header section. Trailer headers,
// duplicate Locations and malformed/control-containing values never grant proof.
bool campus_location(const std::string& headers, const std::string& request_url) {
    std::string location;
    bool found = false;
    auto offset = headers.find("\r\n");
    if (offset == std::string::npos) return false;
    offset += 2;
    while (offset < headers.size()) {
        const auto end = headers.find("\r\n", offset);
        if (end == std::string::npos) return false;
        const auto line = headers.substr(offset, end - offset);
        if (line.empty()) break;
        const auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0 || line.front() == ' ' || line.front() == '\t') return false;
        if (lowercase(line.substr(0, colon)) == "location") {
            if (found) return false;
            location = trim(line.substr(colon + 1)); found = true;
        }
        offset = end + 2;
    }
    return found && location.size() <= 2048 && website::is_campus_redirect(location, request_url);
}

struct Cookie {
    std::string host, name, value, path;
    bool secure = false;
    std::optional<std::int64_t> expires;
};
std::int64_t wall_seconds() { return static_cast<std::int64_t>(std::time(nullptr)); }
bool cookie_path_matches(const std::string& path, const std::string& cookie_path) {
    return path.starts_with(cookie_path) && (path.size() == cookie_path.size() || cookie_path.ends_with('/') || path[cookie_path.size()] == '/');
}
std::string default_cookie_path(const std::string& path) {
    const auto last = path.rfind('/');
    return last == std::string::npos || last == 0 ? "/" : path.substr(0, last);
}
bool cookie_name(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c > 32 && c < 127 && std::string_view("()<>@,;:\\\"/[]?={}").find(static_cast<char>(c)) == std::string_view::npos;
    });
}
bool cookie_value(const std::string& value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= 33 && c < 127 && c != '\"' && c != ',' && c != ';' && c != '\\';
    });
}
std::optional<Cookie> parse_cookie(const std::string& text, const Target& target, const std::string& path, std::int64_t now) {
    if (text.empty() || text.size() > 8192 || std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; })) return std::nullopt;
    const auto first = text.find(';');
    const auto pair = trim(text.substr(0, first));
    const auto equal = pair.find('=');
    if (equal == std::string::npos) return std::nullopt;
    Cookie cookie{target.host, trim(pair.substr(0, equal)), trim(pair.substr(equal + 1)), default_cookie_path(path), false, std::nullopt};
    if (cookie.value.size() >= 2 && cookie.value.front() == '\"' && cookie.value.back() == '\"') cookie.value = cookie.value.substr(1, cookie.value.size() - 2);
    if (!cookie_name(cookie.name) || !cookie_value(cookie.value)) return std::nullopt;
    bool has_domain = false, has_path = false;
    std::optional<std::int64_t> maximum_age, expires;
    auto offset = first == std::string::npos ? text.size() : first + 1;
    while (offset < text.size()) {
        const auto end = text.find(';', offset);
        const auto attribute = trim(text.substr(offset, end == std::string::npos ? end : end - offset));
        const auto separator = attribute.find('=');
        const auto key = lowercase(trim(attribute.substr(0, separator)));
        const auto value = separator == std::string::npos ? std::string{} : trim(attribute.substr(separator + 1));
        if (key == "secure") cookie.secure = true;
        else if (key == "domain") {
            if (has_domain) return std::nullopt;
            has_domain = true;
            auto domain = lowercase(value); if (domain.starts_with('.')) domain.erase(0, 1);
            if (domain != target.host) return std::nullopt;
        } else if (key == "path") {
            if (has_path) return std::nullopt;
            has_path = true;
            if (!value.empty() && value.front() == '/') cookie.path = value;
        } else if (key == "max-age") {
            std::int64_t age = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), age);
            if (!value.empty() && parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) maximum_age = age;
        } else if (key == "expires") {
            const auto date = curl_getdate(value.c_str(), nullptr);
            if (date != -1) expires = static_cast<std::int64_t>(date);
        }
        if (end == std::string::npos) break;
        offset = end + 1;
    }
    if (cookie.secure && target.scheme != "https") return std::nullopt;
    if (cookie.name.starts_with("__Secure-") && !cookie.secure) return std::nullopt;
    if (cookie.name.starts_with("__Host-") && (!cookie.secure || has_domain || !has_path || cookie.path != "/")) return std::nullopt;
    if (maximum_age) {
        if (*maximum_age <= 0) cookie.expires = 0;
        else cookie.expires = *maximum_age > std::numeric_limits<std::int64_t>::max() - now ? std::numeric_limits<std::int64_t>::max() : now + *maximum_age;
    } else cookie.expires = expires;
    return cookie;
}
struct CookieJar {
    std::vector<Cookie> entries;
    void clear() { entries.clear(); }
    void expire(std::int64_t now) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Cookie& cookie) { return cookie.expires && *cookie.expires <= now; }), entries.end());
    }
    std::string header(const Target& target, const std::string& path, std::int64_t now) {
        expire(now);
        auto selected = entries;
        selected.erase(std::remove_if(selected.begin(), selected.end(), [&](const Cookie& cookie) {
            return cookie.host != target.host || (cookie.secure && target.scheme != "https") || !cookie_path_matches(path, cookie.path);
        }), selected.end());
        std::stable_sort(selected.begin(), selected.end(), [](const Cookie& a, const Cookie& b) { return a.path.size() > b.path.size(); });
        std::string value;
        for (const auto& cookie : selected) { if (!value.empty()) value += "; "; value += cookie.name + '=' + cookie.value; }
        return value;
    }
    bool update(const std::string& headers, const Target& target, const std::string& path, std::int64_t now) {
        expire(now);
        auto offset = headers.find("\r\n");
        if (offset == std::string::npos) return false;
        offset += 2;
        while (offset < headers.size()) {
            const auto end = headers.find("\r\n", offset);
            if (end == std::string::npos) return false;
            const auto line = headers.substr(offset, end - offset);
            if (line.empty()) break; // Trailers can never add or replace authentication cookies.
            const auto colon = line.find(':');
            if (colon == std::string::npos || !colon || line.front() == ' ' || line.front() == '\t') return false;
            if (lowercase(line.substr(0, colon)) == "set-cookie") {
                if (const auto next = parse_cookie(trim(line.substr(colon + 1)), target, path, now)) {
                    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Cookie& old) {
                        return old.host == next->host && old.name == next->name && old.path == next->path;
                    }), entries.end());
                    if (!next->expires || *next->expires > now) entries.push_back(*next);
                }
            }
            offset = end + 2;
        }
        auto size = [&] { std::size_t bytes = 0; for (const auto& cookie : entries) bytes += cookie.name.size() + cookie.value.size(); return bytes; };
        while (entries.size() > 64 || size() > 8192) entries.erase(entries.begin());
        return true;
    }
};
}  // namespace

struct PhysicalHttpSession::State {
    physical::Route route;
    bool cookies;
    std::function<bool()> scope_current;
    std::mutex mutex;  // serializes one auth cookie jar, never shared by probe sessions
    CookieJar jar;
    bool current() const { return scope_current && scope_current() && physical_route_current(route); }
};

PhysicalHttpSession::PhysicalHttpSession(physical::Route route, bool cookies, std::function<bool()> scope_current)
    : state_(std::make_unique<State>()) {
    state_->route = std::move(route); state_->cookies = cookies; state_->scope_current = std::move(scope_current);
}
PhysicalHttpSession::~PhysicalHttpSession() = default;

HttpResponse PhysicalHttpSession::get(const HttpRequest& request, const CancelToken& token) {
    HttpResponse response;
    const auto started = Clock::now();
    auto finish = [&](HttpError error) {
        response.error = error;
        response.elapsed = std::chrono::duration<double>(Clock::now() - started).count();
        if (error != HttpError::none && error != HttpError::redirect) response.body.clear();
        return response;
    };
    std::lock_guard<std::mutex> lock(state_->mutex);
    Target target;
    if (!valid_request(request, state_->route, target)) return finish(HttpError::network);
    if (token.cancelled() || !state_->current()) { state_->jar.clear(); return finish(HttpError::cancelled); }
    static std::once_flag initialized;
    static CURLcode initialization = CURLE_FAILED_INIT;
    std::call_once(initialized, [] { initialization = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (initialization != CURLE_OK) return finish(HttpError::network);
    std::vector<std::string> answers;
    const auto dns_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    const int dns_budget = static_cast<int>(request.timeout_ms - dns_elapsed);
    if (dns_budget <= 0) return finish(HttpError::timeout);
    try {
        if (target.host != "172.30.255.42") answers = resolve_physical_a(target.host, state_->route, dns_budget, token);
    } catch (const physical::Error& error) {
        if (token.cancelled() || !state_->current() || error.reason == physical::Failure::cancelled) {
            state_->jar.clear(); return finish(HttpError::cancelled);
        }
        return finish(error.reason == physical::Failure::timeout ? HttpError::timeout : HttpError::dns);
    } catch (...) { return finish(HttpError::dns); }
    if (token.cancelled() || !state_->current()) { state_->jar.clear(); return finish(HttpError::cancelled); }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    const auto remaining = static_cast<long>(request.timeout_ms - elapsed);
    if (remaining <= 0) return finish(HttpError::timeout);
    Easy easy;
    if (!easy.value) return finish(HttpError::network);
    const std::function<bool()> current = [this] { return state_->current(); };
    Transfer transfer{state_->route, token, current, request.max_bytes, {}, {}};
    transfer.headers_only = request.headers_only;
    List resolution, headers;
    if (target.host != "172.30.255.42") {
        if (answers.empty() || answers.size() > 16) return finish(HttpError::dns);
        std::string mapping = target.host + ':' + std::to_string(target.port) + ':';
        for (const auto& address : answers) {
            if (!physical::usable_ipv4(address)) return finish(HttpError::dns);
            if (mapping.back() != ':') mapping += ',';
            mapping += address;
        }
        if (!resolution.add(mapping)) return finish(HttpError::network);
    }
    if (!headers.add("Cache-Control: no-store") || !headers.add("Pragma: no-cache")) return finish(HttpError::network);
    CURLcode code = configure_security(easy.value);
#define SZU_SET(option, setting) do { if (code == CURLE_OK) code = curl_easy_setopt(easy.value, option, setting); } while (0)
    SZU_SET(CURLOPT_URL, request.url.c_str());
    SZU_SET(CURLOPT_HTTPGET, 1L);
    SZU_SET(CURLOPT_TIMEOUT_MS, remaining);
    SZU_SET(CURLOPT_CONNECTTIMEOUT_MS, remaining);
    SZU_SET(CURLOPT_WRITEFUNCTION, write_body); SZU_SET(CURLOPT_WRITEDATA, &transfer);
    SZU_SET(CURLOPT_HEADERFUNCTION, write_header); SZU_SET(CURLOPT_HEADERDATA, &transfer);
    SZU_SET(CURLOPT_OPENSOCKETFUNCTION, open_socket); SZU_SET(CURLOPT_OPENSOCKETDATA, &transfer);
    SZU_SET(CURLOPT_XFERINFOFUNCTION, progress); SZU_SET(CURLOPT_XFERINFODATA, &transfer);
    SZU_SET(CURLOPT_NOPROGRESS, 0L);
    SZU_SET(CURLOPT_PREREQFUNCTION, prerequisite); SZU_SET(CURLOPT_PREREQDATA, &transfer);
    SZU_SET(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(request.max_bytes));
    SZU_SET(CURLOPT_HTTPHEADER, headers.value);
    if (resolution.value) SZU_SET(CURLOPT_RESOLVE, resolution.value);
    const auto parsed_path = wire::parse_url(request.url)->path;
    const auto path = parsed_path.empty() ? std::string("/") : parsed_path;
    const std::string cookie_header = state_->cookies && request.allow_cookies ? state_->jar.header(target, path, wall_seconds()) : "";
    if (!cookie_header.empty()) SZU_SET(CURLOPT_COOKIE, cookie_header.c_str());
    if (code == CURLE_OK) code = curl_easy_perform(easy.value);
    response.request_started = transfer.request_started;
    HttpError error = classify(code, transfer);
    if (token.cancelled() || !state_->current()) { state_->jar.clear(); return finish(HttpError::cancelled); }
    if (error != HttpError::none) return finish(error);
    long status = 0;
    if (curl_easy_getinfo(easy.value, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK || status < 200 || status > 599 || transfer.headers.empty() ||
        transfer.header_phase != Transfer::HeaderPhase::trailers || status != transfer.header_status)
        return finish(HttpError::network);
    response.status = static_cast<int>(status);
    response.body = std::move(transfer.body);
    if (status >= 300 && status < 400) response.campus_redirect = campus_location(transfer.headers, request.url);
    if (state_->cookies && request.allow_cookies) {
        auto next = state_->jar;
        if (!next.update(transfer.headers, target, path, wall_seconds())) return finish(HttpError::network);
        // No asynchronous boundary between the final freshness test and commit.
        if (token.cancelled() || !state_->current()) { state_->jar.clear(); return finish(HttpError::cancelled); }
        state_->jar = std::move(next);
    }
#undef SZU_SET
    return finish(status >= 300 && status < 400 && !request.headers_only ? HttpError::redirect : HttpError::none);
}

#ifdef SZU_PHYSICAL_HTTP_TESTING
// Called only by the native Windows isolation target. It opens and binds a
// loopback socket, never connects it and never reads a credential or user file.
bool physical_http_native_socket_test(std::uint32_t index, std::uint64_t luid) {
    WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    physical::Route route; route.interface_index = index; route.interface_luid = luid; route.source_ipv4 = "127.0.0.1";
    CancelToken token;
    bool current_value = true;
    std::function<bool()> current = [&] { return current_value; };
    Transfer transfer{route, token, current, 1, {}, {}};
    curl_sockaddr address{}; address.family = AF_INET; address.socktype = SOCK_STREAM; address.protocol = IPPROTO_TCP;
    const auto socket_value = open_socket(&transfer, CURLSOCKTYPE_IPCXN, &address);
    bool result = socket_value != CURL_SOCKET_BAD;
    if (result) {
        DWORD actual = 0; int bytes = sizeof(actual);
        sockaddr_in local{}; int local_bytes = sizeof(local);
        result = getsockopt(socket_value, IPPROTO_IP, IP_UNICAST_IF, reinterpret_cast<char*>(&actual), &bytes) == 0 && actual == index &&
            getsockname(socket_value, reinterpret_cast<sockaddr*>(&local), &local_bytes) == 0 && local.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
        char good[] = "127.0.0.1", wrong[] = "127.0.0.2";
        result = result && prerequisite(&transfer, nullptr, good, 0, 0) == CURL_PREREQFUNC_OK &&
            prerequisite(&transfer, nullptr, wrong, 0, 0) == CURL_PREREQFUNC_ABORT;
        current_value = false;
        result = result && prerequisite(&transfer, nullptr, good, 0, 0) == CURL_PREREQFUNC_ABORT &&
            open_socket(&transfer, CURLSOCKTYPE_IPCXN, &address) == CURL_SOCKET_BAD;
        current_value = true; token.cancel();
        result = result && prerequisite(&transfer, nullptr, good, 0, 0) == CURL_PREREQFUNC_ABORT;
        closesocket(socket_value);
    }
    WSACleanup(); return result;
}
bool physical_http_header_stage_test() {
    physical::Route route; CancelToken token;
    const std::function<bool()> current = [] { return true; };
    Target campus{"net.szu.edu.cn", "https", 443};
    Transfer transfer{route, token, current, 3, {}, {}};
    auto line = [&](const char* text) { return write_header(const_cast<char*>(text), 1, std::strlen(text), &transfer); };
    const std::string final_headers = "HTTP/1.1 200 OK\r\nSet-Cookie: session=original; Path=/\r\nLocation: https://unrelated.invalid/\r\n\r\n";
    for (const auto* text : {"HTTP/1.1 200 OK\r\n", "Set-Cookie: session=original; Path=/\r\n", "Location: https://unrelated.invalid/\r\n", "\r\n"})
        if (line(text) != std::strlen(text)) return false;
    char body[] = "abc"; if (write_body(body, 1, 3, &transfer) != 3) return false;
    // libcurl verifies chunked trailer fields by colon, so this HTTP/-prefixed
    // field is legitimate input to the real header callback after the body.
    const std::string trailers = "HTTP/fake: x\r\nSet-Cookie: session=trailer; Path=/\r\nLocation: https://net.szu.edu.cn/srun_portal_pc\r\n\r\n";
    for (const auto* text : {"HTTP/fake: x\r\n", "Set-Cookie: session=trailer; Path=/\r\n", "Location: https://net.szu.edu.cn/srun_portal_pc\r\n", "\r\n"})
        if (line(text) != std::strlen(text)) return false;
    CookieJar jar;
    if (!jar.update(transfer.headers, campus, "/", 1) || jar.header(campus, "/cgi-bin/srun_portal", 2) != "session=original" ||
        transfer.headers != final_headers || transfer.header_bytes != final_headers.size() + trailers.size() ||
        campus_location(transfer.headers, "https://net.szu.edu.cn/srun_portal_pc")) return false;
    // A real 1xx block may precede a final response, but it contributes no cookie.
    Transfer interim{route, token, current, 3, {}, {}};
    for (const auto* text : {"HTTP/1.1 103 Early Hints\r\n", "Set-Cookie: hint=discard; Path=/\r\n", "\r\n",
                             "HTTP/1.1 100 Continue\r\n", "\r\n", "HTTP/1.1 200 OK\r\n",
                             "Set-Cookie: session=final; Path=/\r\n", "HTTP/fake: header-field\r\n", "\r\n"}) {
        if (write_header(const_cast<char*>(text), 1, std::strlen(text), &interim) != std::strlen(text)) return false;
    }
    CookieJar final_jar;
    if (!final_jar.update(interim.headers, campus, "/", 1) || final_jar.header(campus, "/", 2) != "session=final" ||
        !interim.headers.starts_with("HTTP/1.1 200 OK\r\n")) return false;
    // A later status-shaped trailer is also inert, regardless of its spelling.
    const char later[] = "HTTP/1.1 302 Fake\r\n";
    if (write_header(const_cast<char*>(later), 1, std::strlen(later), &interim) != std::strlen(later) ||
        !interim.headers.starts_with("HTTP/1.1 200 OK\r\n")) return false;
    // All ignored trailer bytes still count toward the same bounded budget.
    interim.header_bytes = kHeaderLimit;
    char x[] = "x";
    if (write_header(x, 1, 1, &interim) != 0 || !interim.too_large) return false;
    return true;
}
bool physical_http_native_policy_test() {
    if (!physical_http_header_stage_test()) return false;
    Target target;
    physical::Route route; route.interface_index = 1; route.interface_luid = 1; route.identity = "fixture";
    route.source_ipv4 = "192.0.2.7"; route.addresses = {route.source_ipv4};
    HttpRequest request; request.url = "https://net.szu.edu.cn/srun_portal_pc";
    if (!valid_request(request, route, target)) return false;
    for (const auto* url : {"https://net.szu.edu.cn:443/cgi-bin/get_challenge?callback=_", "http://172.30.255.42:801/eportal/portal/login?a=1",
                            "https://www.apple.com/library/test/success.html", "http://www.msftconnecttest.com/connecttest.txt"}) {
        request.url = url; if (!valid_request(request, route, target)) return false;
    }
    for (const auto* url : {"https://net.szu.edu.cn.evil.invalid/srun_portal_pc", "http://net.szu.edu.cn/srun_portal_pc",
                            "https://net.szu.edu.cn/srun_portal_pc?next=1", "https://net.szu.edu.cn/cgi-bin/../srun_portal_pc",
                            "https://net.szu.edu.cn/srun_portal_pc#x", "http://172.30.255.42/eportal", "https://www.apple.com/"}) {
        request.url = url; if (valid_request(request, route, target)) return false;
    }
    request.url = "https://net.szu.edu.cn/srun_portal_pc"; request.proxy = ProxyMode::system;
    if (valid_request(request, route, target)) return false;
    request.proxy = ProxyMode::none; request.timeout_ms = 12001;
    if (valid_request(request, route, target)) return false;
    request.timeout_ms = 12000; route.source_ipv4 = "192.0.2.8";
    if (valid_request(request, route, target)) return false;
    const std::string good = "HTTP/1.1 302 Found\r\nLocation: https://net.szu.edu.cn/srun_portal_pc\r\n\r\n";
    if (!campus_location(good, request.url) || !campus_location(good + "Location: https://evil.invalid/\r\n", request.url)) return false;
    if (campus_location("HTTP/1.1 302 Found\r\nLocation: https://net.szu.edu.cn/srun_portal_pc\r\nLocation: https://evil.invalid/\r\n\r\n", request.url)) return false;
    Target campus{"net.szu.edu.cn", "https", 443};
    CookieJar jar;
    if (!jar.update("HTTP/1.1 200 OK\r\nSet-Cookie: session=fixture; Path=/; Secure\r\n\r\nSet-Cookie: session=trailer; Path=/\r\n", campus, "/srun_portal_pc", 100) ||
        jar.header(campus, "/cgi-bin/get_challenge", 101) != "session=fixture" ||
        !jar.header(Target{"net.szu.edu.cn", "http", 80}, "/", 101).empty() ||
        !jar.header(Target{"www.apple.com", "https", 443}, "/", 101).empty()) return false;
    if (!jar.update("HTTP/1.1 200 OK\r\nSet-Cookie: other=x; Domain=.szu.edu.cn; Path=/\r\nSet-Cookie: limited=y; Path=/cgi-bin; Max-Age=2\r\n\r\n", campus, "/", 101) ||
        jar.entries.size() != 2 || jar.header(campus, "/cgi-bin/srun_portal", 102) != "limited=y; session=fixture" ||
        jar.header(campus, "/cgi-bin-other", 102) != "session=fixture" || jar.header(campus, "/cgi-bin/srun_portal", 103) != "session=fixture") return false;
    if (!jar.update("HTTP/1.1 200 OK\r\nSet-Cookie: session=x; Path=/; Max-Age=0\r\n\r\n", campus, "/", 104) || !jar.entries.empty()) return false;
    if (parse_cookie("__Host-invalid=x; Domain=net.szu.edu.cn; Path=/; Secure", campus, "/", 1) ||
        parse_cookie("__Secure-invalid=x; Path=/", campus, "/", 1) ||
        parse_cookie("bad=x\r\nInjected: fake", campus, "/", 1)) return false;
    CancelToken token; std::function<bool()> current = [] { return true; };
    Transfer transfer{route, token, current, 3, {}, {}};
    if (classify(CURLE_SSL_INVALIDCERTSTATUS, transfer) != HttpError::tls ||
        classify(CURLE_SSL_CRL_BADFILE, transfer) != HttpError::tls || classify(CURLE_PEER_FAILED_VERIFICATION, transfer) != HttpError::tls) return false;
    char abc[] = "abc", x[] = "x";
    if (write_body(abc, 1, 3, &transfer) != 3 || write_body(x, 1, 1, &transfer) != 0 || !transfer.too_large) return false;
    token.cancel(); if (progress(&transfer, 0, 0, 0, 0) == 0 || write_header(x, 1, 1, &transfer) != 0) return false;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return false;
    Easy easy;
    const auto* version = curl_version_info(CURLVERSION_NOW);
    if (!easy.value || !version || !version->ssl_version || std::string_view(version->ssl_version).find("Schannel") == std::string_view::npos ||
        configure_security(easy.value) != CURLE_OK) return false;

    return true;
}
#endif
}  // namespace szu::win
