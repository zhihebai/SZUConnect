#include "CampusPhysicalCurl.h"
#if TARGET_OS_OSX
#include <curl/curl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

struct SZUCampusCancellation {
    std::atomic<bool> cancelled{false};
    std::mutex multiMutex;
    CURLM *activeMulti = nullptr;
};
namespace {
constexpr size_t headerLimit = 65536;
constexpr size_t bodyLimit = 1048576;
constexpr size_t urlLimit = 65536;
struct Target { std::string host; std::string scheme; unsigned port = 0; };

bool ipv4(const std::string &value) {
    in_addr address{};
    return !value.empty() && inet_pton(AF_INET, value.c_str(), &address) == 1;
}
bool extractTarget(const char *text, Target &target) {
    if (!text || strnlen(text, urlLimit + 1) > urlLimit || strpbrk(text, "\r\n")) return false;
    CURLU *url = curl_url();
    if (!url) return false;
    char *scheme = nullptr, *host = nullptr, *port = nullptr, *path = nullptr;
    char *user = nullptr, *password = nullptr, *fragment = nullptr, *query = nullptr;
    const bool parsed = curl_url_set(url, CURLUPART_URL, text, CURLU_DISALLOW_USER) == CURLUE_OK;
    bool allowed = false;
    if (parsed && curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
        curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
        curl_url_get(url, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) == CURLUE_OK &&
        curl_url_get(url, CURLUPART_PATH, &path, 0) == CURLUE_OK) {
        const bool hasUser = curl_url_get(url, CURLUPART_USER, &user, 0) == CURLUE_OK;
        const bool hasPassword = curl_url_get(url, CURLUPART_PASSWORD, &password, 0) == CURLUE_OK;
        const bool hasFragment = curl_url_get(url, CURLUPART_FRAGMENT, &fragment, 0) == CURLUE_OK;
        const bool hasQuery = curl_url_get(url, CURLUPART_QUERY, &query, 0) == CURLUE_OK;
        target.host = host; target.scheme = scheme;
        target.port = static_cast<unsigned>(strtoul(port, nullptr, 10));
        const std::string location(path);
        if (!hasUser && !hasPassword && !hasFragment) {
            if (target.scheme == "https" && target.host == "net.szu.edu.cn" && target.port == 443) {
                allowed = ((!hasQuery && (location == "/" || location == "/srun_portal_pc")) ||
                    location == "/cgi-bin/get_challenge" || location == "/cgi-bin/srun_portal");
            } else if (target.scheme == "http" && target.host == "172.30.255.42" && target.port == 801) {
                allowed = ((!hasQuery && (location == "/" || location == "/eportal")) ||
                    location == "/eportal/portal/login");
            } else if (target.scheme == "https" && target.host == "www.apple.com" && target.port == 443) {
                allowed = !hasQuery && location == "/library/test/success.html";
            } else if (target.scheme == "http" && target.host == "www.msftconnecttest.com" && target.port == 80) {
                allowed = !hasQuery && location == "/connecttest.txt";
            }
        }
    }
    curl_free(scheme); curl_free(host); curl_free(port); curl_free(path);
    curl_free(user); curl_free(password); curl_free(fragment); curl_free(query); curl_url_cleanup(url);
    return allowed;
}
bool validAnswers(const char *text) {
    if (!text || !*text || strnlen(text, 1025) > 1024) return false;
    const std::string values(text);
    size_t offset = 0, count = 0;
    do {
        const size_t end = values.find(',', offset);
        if (!ipv4(values.substr(offset, end == std::string::npos ? end : end - offset)) || ++count > 16) return false;
        if (end == std::string::npos) return true;
        offset = end + 1;
    } while (offset < values.size());
    return false;
}
bool validate(const SZUCampusRequest *request, Target &target) {
    return request && request->source_ipv4 && ipv4(request->source_ipv4) &&
        request->interface_name && *request->interface_name && strnlen(request->interface_name, IF_NAMESIZE) < IF_NAMESIZE &&
        request->route_is_current && request->interface_index != 0 && std::isfinite(request->timeout_seconds) &&
        request->timeout_seconds > 0 && request->timeout_seconds <= 12 &&
        request->response_limit > 0 && request->response_limit <= bodyLimit &&
        extractTarget(request->url, target) &&
        (target.host == "172.30.255.42" ? (!request->resolved_addresses || !*request->resolved_addresses) : validAnswers(request->resolved_addresses)) &&
        (!request->cookie || (strnlen(request->cookie, 16385) <= 16384 && !strpbrk(request->cookie, "\r\n")));
}
struct Transfer {
    SZUCampusCancellation *token;
    uint32_t index;
    const char *source;
    const char *interfaceName;
    int (*routeIsCurrent)(void *);
    void *routeContext;
    size_t limit;
    std::vector<unsigned char> body;
    std::string headers;
    size_t headerBytes = 0;
    bool tooLarge = false;
    bool malformed = false;
};
bool isCancelled(const Transfer &transfer) { return transfer.token->cancelled.load(std::memory_order_acquire); }
size_t writeBody(char *bytes, size_t size, size_t count, void *opaque) {
    auto &transfer = *static_cast<Transfer *>(opaque);
    if (isCancelled(transfer)) return 0;
    if (size && count > std::numeric_limits<size_t>::max() / size) { transfer.tooLarge = true; return 0; }
    const size_t length = size * count;
    if (length > transfer.limit - transfer.body.size()) { transfer.tooLarge = true; return 0; }
    try { transfer.body.insert(transfer.body.end(), bytes, bytes + length); }
    catch (...) { transfer.tooLarge = true; return 0; }
    return length;
}
size_t writeHeader(char *bytes, size_t size, size_t count, void *opaque) {
    auto &transfer = *static_cast<Transfer *>(opaque);
    if (isCancelled(transfer)) return 0;
    if (size && count > std::numeric_limits<size_t>::max() / size) { transfer.tooLarge = true; return 0; }
    const size_t length = size * count;
    if (length > headerLimit - transfer.headerBytes) { transfer.tooLarge = true; return 0; }
    transfer.headerBytes += length;
    if (memchr(bytes, '\0', length)) { transfer.malformed = true; return 0; }
    try {
        // A new HTTP status line replaces an interim block, but all bytes still count.
        if (length >= 5 && memcmp(bytes, "HTTP/", 5) == 0) transfer.headers.clear();
        transfer.headers.append(bytes, length);
    } catch (...) { transfer.tooLarge = true; return 0; }
    return length;
}
int progress(void *opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return isCancelled(*static_cast<Transfer *>(opaque)) ? 1 : 0;
}
bool sourceIsCurrent(const Transfer &transfer) {
    if (isCancelled(transfer) || !transfer.routeIsCurrent(transfer.routeContext) ||
        if_nametoindex(transfer.interfaceName) != transfer.index) return false;
    ifaddrs *records = nullptr;
    if (getifaddrs(&records) != 0) return false;
    bool found = false;
    for (ifaddrs *item = records; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
            strcmp(item->ifa_name, transfer.interfaceName) != 0 || !(item->ifa_flags & IFF_UP)) continue;
        char address[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(item->ifa_addr)->sin_addr, address, sizeof(address)) &&
            strcmp(address, transfer.source) == 0) { found = true; break; }
    }
    freeifaddrs(records); return found;
}
int prereq(void *opaque, char *, char *localIP, int, int) {
    auto &transfer = *static_cast<Transfer *>(opaque);
    return localIP && strcmp(localIP, transfer.source) == 0 && sourceIsCurrent(transfer)
        ? CURL_PREREQFUNC_OK : CURL_PREREQFUNC_ABORT;
}
curl_socket_t openSocket(void *opaque, curlsocktype purpose, curl_sockaddr *address) {
    auto &transfer = *static_cast<Transfer *>(opaque);
    if (!sourceIsCurrent(transfer) || purpose != CURLSOCKTYPE_IPCXN || address->family != AF_INET) return CURL_SOCKET_BAD;
    const int socketFD = socket(address->family, address->socktype, address->protocol);
    if (socketFD < 0) return CURL_SOCKET_BAD;
    // CURLOPT_NOSIGNAL leaves SIGPIPE handling to the caller. A failed TLS/socket
    // write must remain a request error, including on Launch Services launches.
    const int noSigPipe = 1;
    if (setsockopt(socketFD, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe)) != 0) {
        close(socketFD); return CURL_SOCKET_BAD;
    }
    const unsigned index = transfer.index;
    if (setsockopt(socketFD, IPPROTO_IP, IP_BOUND_IF, &index, sizeof(index)) != 0) {
        close(socketFD); return CURL_SOCKET_BAD;
    }
    sockaddr_in source{};
    source.sin_len = sizeof(source); source.sin_family = AF_INET;
    if (inet_pton(AF_INET, transfer.source, &source.sin_addr) != 1 ||
        bind(socketFD, reinterpret_cast<sockaddr *>(&source), sizeof(source)) != 0 || !sourceIsCurrent(transfer)) {
        close(socketFD); return CURL_SOCKET_BAD;
    }
    return socketFD;
}
int classify(CURLcode code, const Transfer &transfer) {
    if (isCancelled(transfer) || code == CURLE_ABORTED_BY_CALLBACK) return SZU_CAMPUS_CANCELLED;
    if (transfer.tooLarge) return SZU_CAMPUS_TOO_LARGE;
    if (transfer.malformed) return SZU_CAMPUS_RESPONSE;
    switch (code) {
    case CURLE_OK: return SZU_CAMPUS_OK;
    case CURLE_OPERATION_TIMEDOUT: return SZU_CAMPUS_TIMEOUT;
    case CURLE_COULDNT_RESOLVE_HOST: return SZU_CAMPUS_DNS;
    case CURLE_SSL_CONNECT_ERROR: case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM: case CURLE_SSL_CIPHER: case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ISSUER_ERROR: case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
        return SZU_CAMPUS_TLS;
    case CURLE_WEIRD_SERVER_REPLY: case CURLE_UNSUPPORTED_PROTOCOL: case CURLE_URL_MALFORMAT:
        return SZU_CAMPUS_RESPONSE;
    default: return SZU_CAMPUS_NETWORK;
    }
}
struct SecurityOptions {
    long verifyPeer = 1, verifyHost = 2, followRedirects = 0, unrestrictedAuth = 0, noSignal = 1;
    long minimumTLS = CURL_SSLVERSION_TLSv1_2;
};
CURLcode performTransfer(CURL *handle, SZUCampusCancellation *token) {
    CURLM *multi = curl_multi_init();
    if (!multi) return CURLE_OUT_OF_MEMORY;
    if (curl_multi_add_handle(multi, handle) != CURLM_OK) {
        curl_multi_cleanup(multi);
        return CURLE_FAILED_INIT;
    }
    bool attached = false;
    {
        std::lock_guard<std::mutex> lock(token->multiMutex);
        if (!token->activeMulti) {
            token->activeMulti = multi;
            attached = true;
        }
    }
    CURLcode result = CURLE_FAILED_INIT;
    if (attached) {
        bool finished = false;
        while (!finished) {
            if (token->cancelled.load(std::memory_order_acquire)) {
                result = CURLE_ABORTED_BY_CALLBACK;
                break;
            }
            int running = 0;
            if (curl_multi_perform(multi, &running) != CURLM_OK) {
                result = CURLE_RECV_ERROR;
                break;
            }
            int remaining = 0;
            while (CURLMsg *message = curl_multi_info_read(multi, &remaining)) {
                if (message->msg == CURLMSG_DONE && message->easy_handle == handle) {
                    result = message->data.result;
                    finished = true;
                }
            }
            if (finished || !running) break;
            int descriptors = 0;
            // Cancellation wakes this poll directly. libcurl still enforces the original
            // total/connect timeouts, TLS checks, binding callbacks and response bounds.
            if (curl_multi_poll(multi, nullptr, 0, 1000, &descriptors) != CURLM_OK) {
                result = CURLE_RECV_ERROR;
                break;
            }
        }
        {
            std::lock_guard<std::mutex> lock(token->multiMutex);
            token->activeMulti = nullptr;
        }
    }
    curl_multi_remove_handle(multi, handle);
    curl_multi_cleanup(multi);
    return result;
}
CURLcode configureSecurity(CURL *handle) {
    const SecurityOptions options;
#define SZU_SET(option, value) do { const CURLcode code = curl_easy_setopt(handle, option, value); if (code != CURLE_OK) return code; } while (0)
    SZU_SET(CURLOPT_SSL_VERIFYPEER, options.verifyPeer);
    SZU_SET(CURLOPT_SSL_VERIFYHOST, options.verifyHost);
    SZU_SET(CURLOPT_SSLVERSION, options.minimumTLS);
    SZU_SET(CURLOPT_FOLLOWLOCATION, options.followRedirects);
    SZU_SET(CURLOPT_UNRESTRICTED_AUTH, options.unrestrictedAuth);
    SZU_SET(CURLOPT_NOSIGNAL, options.noSignal);
    SZU_SET(CURLOPT_PROXY, "");
    SZU_SET(CURLOPT_NOPROXY, "*");
    SZU_SET(CURLOPT_NETRC, CURL_NETRC_IGNORED);
    SZU_SET(CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    SZU_SET(CURLOPT_PROTOCOLS_STR, "http,https");
    SZU_SET(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    SZU_SET(CURLOPT_VERBOSE, 0L);
    SZU_SET(CURLOPT_DNS_CACHE_TIMEOUT, 0L);
    return CURLE_OK;
#undef SZU_SET
}
}
extern "C" SZUCampusCancellation *szu_campus_cancellation_create(void) {
    try { return new SZUCampusCancellation(); } catch (...) { return nullptr; }
}
extern "C" void szu_campus_cancellation_cancel(SZUCampusCancellation *token) {
    if (!token) return;
    token->cancelled.store(true, std::memory_order_release);
    // The same lock protects handle registration and cleanup against a concurrent wakeup.
    std::lock_guard<std::mutex> lock(token->multiMutex);
    if (token->activeMulti) (void)curl_multi_wakeup(token->activeMulti);
}
extern "C" int szu_campus_cancellation_is_cancelled(const SZUCampusCancellation *token) { return !token || token->cancelled.load(std::memory_order_acquire); }
extern "C" void szu_campus_cancellation_release(SZUCampusCancellation *token) { delete token; }
extern "C" int szu_campus_request_is_valid(const SZUCampusRequest *request) { Target target; try { return validate(request, target); } catch (...) { return 0; } }
extern "C" void szu_campus_response_release(SZUCampusResponse *response) {
    if (!response) return;
    free(response->body); free(response->headers); *response = {};
}
extern "C" SZUCampusResponse szu_campus_request(const SZUCampusRequest *request, SZUCampusCancellation *token) {
    SZUCampusResponse result{};
    result.error = SZU_CAMPUS_RESPONSE;
    if (!token) return result;
    if (szu_campus_cancellation_is_cancelled(token)) { result.error = SZU_CAMPUS_CANCELLED; return result; }
    static std::once_flag initialized;
    static CURLcode initialization = CURLE_FAILED_INIT;
    std::call_once(initialized, [] { initialization = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (initialization != CURLE_OK) { result.error = SZU_CAMPUS_NETWORK; return result; }
    CURL *handle = nullptr;
    curl_slist *resolution = nullptr, *headers = nullptr;
    try {
        Target target;
        if (!validate(request, target)) return result;
        Transfer transfer{token, request->interface_index, request->source_ipv4, request->interface_name, request->route_is_current, request->route_context, request->response_limit, {}, {}, 0, false, false};
        handle = curl_easy_init();
        if (!handle) { result.error = SZU_CAMPUS_NETWORK; return result; }
        CURLcode code = configureSecurity(handle);
#define SZU_SET(option, value) do { if (code == CURLE_OK) code = curl_easy_setopt(handle, option, value); } while (0)
        SZU_SET(CURLOPT_URL, request->url);
        SZU_SET(CURLOPT_HTTPGET, 1L);
        SZU_SET(CURLOPT_TIMEOUT_MS, static_cast<long>(ceil(request->timeout_seconds * 1000)));
        SZU_SET(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(ceil(request->timeout_seconds * 1000)));
        SZU_SET(CURLOPT_WRITEFUNCTION, writeBody); SZU_SET(CURLOPT_WRITEDATA, &transfer);
        SZU_SET(CURLOPT_HEADERFUNCTION, writeHeader); SZU_SET(CURLOPT_HEADERDATA, &transfer);
        SZU_SET(CURLOPT_OPENSOCKETFUNCTION, openSocket); SZU_SET(CURLOPT_OPENSOCKETDATA, &transfer);
        SZU_SET(CURLOPT_XFERINFOFUNCTION, progress); SZU_SET(CURLOPT_XFERINFODATA, &transfer);
        SZU_SET(CURLOPT_NOPROGRESS, 0L);
        SZU_SET(CURLOPT_PREREQFUNCTION, prereq); SZU_SET(CURLOPT_PREREQDATA, &transfer);
        SZU_SET(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(request->response_limit));
        if (target.host != "172.30.255.42") {
            const std::string mapping = target.host + ":" + std::to_string(target.port) + ":" + request->resolved_addresses;
            resolution = curl_slist_append(nullptr, mapping.c_str());
            if (!resolution) code = CURLE_OUT_OF_MEMORY;
            SZU_SET(CURLOPT_RESOLVE, resolution);
        }
        headers = curl_slist_append(nullptr, "Cache-Control: no-store");
        if (!headers) code = CURLE_OUT_OF_MEMORY;
        SZU_SET(CURLOPT_HTTPHEADER, headers);
        if (request->cookie && *request->cookie) SZU_SET(CURLOPT_COOKIE, request->cookie);
        if (code == CURLE_OK) code = performTransfer(handle, token);
        result.error = classify(code, transfer);
        if (code == CURLE_FILESIZE_EXCEEDED) result.error = SZU_CAMPUS_TOO_LARGE;
        if (result.error == SZU_CAMPUS_OK) {
            curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &result.status);
            if (result.status < 100 || result.status > 599 || transfer.headers.empty()) result.error = SZU_CAMPUS_RESPONSE;
            else {
                result.body = static_cast<unsigned char *>(malloc(transfer.body.size() ? transfer.body.size() : 1));
                result.headers = static_cast<char *>(malloc(transfer.headers.size() + 1));
                if (!result.body || !result.headers) result.error = SZU_CAMPUS_NETWORK;
                else {
                    if (!transfer.body.empty()) memcpy(result.body, transfer.body.data(), transfer.body.size());
                    memcpy(result.headers, transfer.headers.data(), transfer.headers.size());
                    result.headers[transfer.headers.size()] = '\0';
                    result.body_length = transfer.body.size(); result.headers_length = transfer.headers.size();
                }
            }
        }
#undef SZU_SET
    } catch (...) { result.error = SZU_CAMPUS_NETWORK; }
    if (handle) curl_easy_cleanup(handle);
    curl_slist_free_all(resolution); curl_slist_free_all(headers);
    if (szu_campus_cancellation_is_cancelled(token)) result.error = SZU_CAMPUS_CANCELLED;
    if (result.error != SZU_CAMPUS_OK) { free(result.body); free(result.headers); result.body = nullptr; result.headers = nullptr; result.body_length = 0; result.headers_length = 0; }
    return result;
}
#ifdef SZU_CAMPUS_CURL_TESTING
// A loopback-only transport harness. The production URL validator remains unchanged.
extern "C" int szu_campus_test_cancel_loopback(unsigned port, SZUCampusCancellation *token) {
    if (!token || port == 0 || port > 65535) return 0;
    CURL *handle = curl_easy_init();
    if (!handle) return 0;
    const auto index = if_nametoindex("lo0");
    Transfer transfer{token, index, "127.0.0.1", "lo0", [](void *) { return 1; }, nullptr, bodyLimit, {}, {}, 0, false, false};
    const auto url = std::string("http://127.0.0.1:") + std::to_string(port) + "/fixture";
    CURLcode code = configureSecurity(handle);
    if (code == CURLE_OK) code = curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    if (code == CURLE_OK) code = curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 4000L);
    if (code == CURLE_OK) code = curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeBody);
    if (code == CURLE_OK) code = curl_easy_setopt(handle, CURLOPT_WRITEDATA, &transfer);
    if (code == CURLE_OK) code = curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, writeHeader);
    if (code == CURLE_OK) code = curl_easy_setopt(handle, CURLOPT_HEADERDATA, &transfer);
    if (code == CURLE_OK) code = curl_easy_setopt(handle, CURLOPT_OPENSOCKETFUNCTION, openSocket);
    if (code == CURLE_OK) code = curl_easy_setopt(handle, CURLOPT_OPENSOCKETDATA, &transfer);
    if (code == CURLE_OK) code = curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, progress);
    if (code == CURLE_OK) code = curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &transfer);
    if (code == CURLE_OK) code = curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    if (code == CURLE_OK) code = performTransfer(handle, token);
    const int cancelled = classify(code, transfer) == SZU_CAMPUS_CANCELLED;
    curl_easy_cleanup(handle);
    return cancelled;
}
extern "C" int szu_campus_test_bounds(void) {
    SZUCampusCancellation token;
    Transfer transfer{&token, 1, "127.0.0.1", "lo0", [](void *) { return 1; }, nullptr, 3, {}, {}, 0, false, false};
    char abc[] = "abc", x[] = "x", interim[] = "HTTP/1.1 100 Continue\r\n", final[] = "HTTP/1.1 200 OK\r\n";
    if (writeBody(abc, 1, 3, &transfer) != 3 || writeBody(x, 1, 1, &transfer) != 0 || !transfer.tooLarge) return 0;
    Transfer header{&token, 1, "127.0.0.1", "lo0", [](void *) { return 1; }, nullptr, 3, {}, {}, 0, false, false};
    writeHeader(interim, 1, strlen(interim), &header); writeHeader(final, 1, strlen(final), &header);
    if (header.headers != final || header.headerBytes != strlen(interim) + strlen(final)) return 0;
    char cookie[] = "Set-Cookie: a=1\r\n";
    writeHeader(cookie, 1, strlen(cookie), &header); writeHeader(cookie, 1, strlen(cookie), &header);
    if (header.headers != std::string(final) + cookie + cookie) return 0;
    header.headerBytes = headerLimit;
    if (writeHeader(x, 1, 1, &header) != 0 || !header.tooLarge) return 0;
    token.cancelled.store(true);
    if (writeBody(abc, 1, 1, &header) != 0 || progress(&header, 0, 0, 0, 0) != 1) return 0;
    return 1;
}
extern "C" int szu_campus_test_bound_socket(uint32_t index) {
    SZUCampusCancellation token; Transfer transfer{&token, index, "127.0.0.1", "lo0", [](void *) { return 1; }, nullptr, 1, {}, {}, 0, false, false};
    curl_sockaddr address{}; address.family = AF_INET; address.socktype = SOCK_STREAM; address.protocol = IPPROTO_TCP;
    int fd = openSocket(&transfer, CURLSOCKTYPE_IPCXN, &address);
    if (fd == CURL_SOCKET_BAD) return 0;
    unsigned actual = 0; socklen_t length = sizeof(actual);
    int result = getsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &actual, &length) == 0 && actual == index;
    int noSigPipe = 0; length = sizeof(noSigPipe);
    result = result && getsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, &length) == 0 && noSigPipe == 1;
    sockaddr_in source{}; socklen_t sourceLength = sizeof(source);
    result = result && getsockname(fd, reinterpret_cast<sockaddr *>(&source), &sourceLength) == 0 &&
        source.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
    char local[] = "127.0.0.1", wrong[] = "127.0.0.2";
    result = result && prereq(&transfer, nullptr, local, 0, 0) == CURL_PREREQFUNC_OK &&
        prereq(&transfer, nullptr, wrong, 0, 0) == CURL_PREREQFUNC_ABORT;
    transfer.routeIsCurrent = [](void *) { return 0; };
    result = result && prereq(&transfer, nullptr, local, 0, 0) == CURL_PREREQFUNC_ABORT &&
        openSocket(&transfer, CURLSOCKTYPE_IPCXN, &address) == CURL_SOCKET_BAD;
    transfer.routeIsCurrent = [](void *) { return 1; };
    token.cancelled.store(true);
    result = result && prereq(&transfer, nullptr, local, 0, 0) == CURL_PREREQFUNC_ABORT;
    close(fd); return result;
}
extern "C" int szu_campus_test_security_options(void) {
    CURL *handle = curl_easy_init(); if (!handle) return 0;
    int result = configureSecurity(handle) == CURLE_OK;
    const SecurityOptions options;
    result = result && options.verifyPeer == 1 && options.verifyHost == 2 && options.followRedirects == 0 &&
        options.minimumTLS == CURL_SSLVERSION_TLSv1_2;
    curl_easy_cleanup(handle); return result;
}
extern "C" int szu_campus_test_broken_connection(uint32_t index) {
    SZUCampusCancellation token;
    Transfer transfer{&token, index, "127.0.0.1", "lo0", [](void *) { return 1; }, nullptr, 1, {}, {}, 0, false, false};
    curl_sockaddr address{}; address.family = AF_INET; address.socktype = SOCK_STREAM; address.protocol = IPPROTO_TCP;
    const int fd = openSocket(&transfer, CURLSOCKTYPE_IPCXN, &address);
    if (fd == CURL_SOCKET_BAD) return 0;
    const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int peer = -1;
    bool result = false;
    sockaddr_in endpoint{};
    endpoint.sin_len = sizeof(endpoint); endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(endpoint);
    if (listener >= 0 && bind(listener, reinterpret_cast<sockaddr *>(&endpoint), length) == 0 &&
        getsockname(listener, reinterpret_cast<sockaddr *>(&endpoint), &length) == 0 && listen(listener, 1) == 0 &&
        connect(fd, reinterpret_cast<sockaddr *>(&endpoint), length) == 0 &&
        (peer = accept(listener, nullptr, nullptr)) >= 0 && shutdown(fd, SHUT_WR) == 0) {
        // Deterministic broken connection: the production socket must report EPIPE
        // even when the process retains the system's default SIGPIPE disposition.
        const char byte = 'x';
        result = send(fd, &byte, 1, 0) == -1 && errno == EPIPE;
    }
    if (peer >= 0) close(peer);
    if (listener >= 0) close(listener);
    close(fd);
    return result;
}
#endif
#endif
