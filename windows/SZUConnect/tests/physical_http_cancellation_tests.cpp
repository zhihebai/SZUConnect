// Local HTTP fixtures exercise the native transfer loop without campus requests,
// credentials, proxy changes, DNS changes or routing changes.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "szu/win/http.hpp"
#include "szu/win/physical.hpp"
#include "szu/win/service.hpp"

namespace szu::win {
HttpResponse physical_http_loopback_test(std::uint32_t index, std::uint64_t luid, unsigned short port,
                                        int timeout_ms, const CancelToken& token);
bool physical_http_current_test(const physical::Route& route);
bool physical_http_native_scope_test();
}

namespace {
using Clock = std::chrono::steady_clock;
using namespace szu;
using namespace szu::win;
enum class Reply { silent, headers, complete };
struct Server {
    SOCKET listener = INVALID_SOCKET;
    unsigned short port = 0;
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::condition_variable changed;
    bool received = false, released = false;
    std::thread worker;

    explicit Server(Reply reply, std::string body = "done") {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) return;
        sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR || listen(listener, 1) == SOCKET_ERROR) return;
        int size = sizeof(local);
        if (getsockname(listener, reinterpret_cast<sockaddr*>(&local), &size) == SOCKET_ERROR) return;
        port = ntohs(local.sin_port);
        worker = std::thread([this, reply, body = std::move(body)] {
            SOCKET client = INVALID_SOCKET;
            while (!stopping) {
                fd_set read; FD_ZERO(&read); FD_SET(listener, &read);
                timeval wait{0, 50000};
                if (select(0, &read, nullptr, nullptr, &wait) > 0) { client = accept(listener, nullptr, nullptr); break; }
            }
            if (client == INVALID_SOCKET) return;
            DWORD timeout = 1000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            std::string request;
            char buffer[1024];
            while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
                const int read = recv(client, buffer, sizeof(buffer), 0);
                if (read <= 0) break;
                request.append(buffer, static_cast<std::size_t>(read));
            }
            if (request.find("\r\n\r\n") != std::string::npos) {
                if (reply != Reply::silent) {
                    std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                    if (reply == Reply::complete) response += body;
                    std::size_t sent = 0;
                    while (sent < response.size()) {
                        const int count = send(client, response.data() + sent, static_cast<int>(response.size() - sent), 0);
                        if (count <= 0) break;
                        sent += static_cast<std::size_t>(count);
                    }
                }
                std::unique_lock<std::mutex> lock(mutex);
                received = true; changed.notify_all();
                if (reply != Reply::complete) changed.wait_for(lock, std::chrono::seconds(3), [&] { return released; });
            }
            shutdown(client, SD_BOTH); closesocket(client);
        });
    }
    bool wait_received() {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(2), [&] { return received; });
    }
    ~Server() {
        stopping = true;
        { std::lock_guard<std::mutex> lock(mutex); released = true; }
        changed.notify_all();
        if (worker.joinable()) worker.join();
        if (listener != INVALID_SOCKET) closesocket(listener);
    }
};
struct Checks {
    int count = 0, failures = 0;
    void check(bool result, const char* label) {
        ++count;
        if (!result) { ++failures; std::printf("FAIL: %s\n", label); }
    }
};
void summarize(const char* label, std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    std::printf("%s: count=%zu median=%.3f ms max=%.3f ms\n", label, samples.size(), samples[samples.size() / 2], samples.back());
}
}

int main(int argc, char** argv) {
    const bool benchmark = argc == 2 && std::string(argv[1]) == "--benchmark";
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    Checks checks;
    std::uint32_t index = 0; std::uint64_t luid = 0;
    PMIB_UNICASTIPADDRESS_TABLE table = nullptr;
    if (GetUnicastIpAddressTable(AF_INET, &table) == NO_ERROR && table) {
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const auto& row = table->Table[i];
            if (row.Address.Ipv4.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) { index = row.InterfaceIndex; luid = row.InterfaceLuid.Value; break; }
        }
        FreeMibTable(table);
    }
    checks.check(index && luid, "loopback interface exists");
    if (!index || !luid) { WSACleanup(); return 1; }
    checks.check(physical_http_native_scope_test(), "lease validation rejects ownership loss before or during its complete route read");
    for (const auto reply : {Reply::silent, Reply::headers}) {
        std::vector<double> samples;
        for (int i = 0; i < 5; ++i) {
            Server server(reply); CancelToken token; HttpResponse result;
            checks.check(server.port != 0, "fixture binds only loopback");
            std::thread worker([&] { result = physical_http_loopback_test(index, luid, server.port, 5000, token); });
            checks.check(server.wait_received(), "fixture received the request before cancellation");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto start = Clock::now(); token.cancel(); worker.join();
            const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            samples.push_back(elapsed);
            checks.check(result.error == HttpError::cancelled && result.request_started, "cancelled sent request retains submission evidence");
            if (!benchmark) checks.check(elapsed < 500, "cancellation wakes a silent transfer within 500 ms");
        }
        summarize(reply == Reply::silent ? "cancel before headers" : "cancel during body", samples);
    }
    {
        Server slow(Reply::silent), fast(Reply::complete, "Microsoft Connect Test");
        struct Session final : FixedHttpSession {
            std::uint32_t index; std::uint64_t luid; Server& slow; Server& fast;
            Session(std::uint32_t i, std::uint64_t l, Server& s, Server& f) : index(i), luid(l), slow(s), fast(f) {}
            HttpResponse get(const HttpRequest& request, const CancelToken& token) override {
                const bool apple = request.url.find("www.apple.com") != std::string::npos;
                if (!apple) { slow.wait_received(); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
                return physical_http_loopback_test(index, luid, apple ? slow.port : fast.port, request.timeout_ms, token);
            }
        };
        WinHttpService service({[](const physical::Route&) { return true; },
            [&](const physical::Route&, bool, std::function<bool()>) -> std::unique_ptr<FixedHttpSession> {
                return std::make_unique<Session>(index, luid, slow, fast);
            }});
        physical::Route route; route.identity = "loopback-fixture";
        service.set_expected_physical_route(route);
        CancelToken token;
        const auto start = Clock::now();
        const auto result = service.probe(ProbeMode::quick, token);
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        checks.check(result.microsoft.succeeded() && result.apple.state == EndpointState::notCompleted && !token.cancelled(),
            "quick success cancels only the silent peer and preserves its not-completed result");
        if (!benchmark) checks.check(elapsed < 600, "quick success joins the cancelled native peer promptly");
        std::printf("quick success with silent peer: %.3f ms\n", elapsed);
    }
    {
        Server server(Reply::silent); CancelToken token;
        const auto start = Clock::now();
        const auto result = physical_http_loopback_test(index, luid, server.port, 150, token);
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        checks.check(result.error == HttpError::timeout && result.request_started && elapsed >= 100 && elapsed < 1000,
            "uncancelled silent response retains request deadline and sent evidence");
    }
    {
        CancelToken token; token.cancel();
        const auto result = physical_http_loopback_test(index, luid, 1, 5000, token);
        checks.check(result.error == HttpError::cancelled && !result.request_started, "pre-cancelled transfer opens no connection");
    }
    for (int i = 0; i < 20; ++i) {
        Server server(Reply::complete); CancelToken token;
        const auto result = physical_http_loopback_test(index, luid, server.port, 1000, token);
        checks.check(result.error == HttpError::none && result.body == "done" && result.request_started, "complete response remains intact");
        token.cancel(); // A completed transfer must release its wakeup registration.
    }
    if (benchmark) {
        const auto candidates = capture_physical_candidates();
        if (!candidates.empty()) {
            std::vector<double> previous, current;
            for (int i = 0; i < 20; ++i) {
                auto start = Clock::now();
                const bool valid = physical_route_current(candidates.front()) && physical_route_current(candidates.front());
                previous.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
                start = Clock::now(); const bool valid_twice = physical_http_current_test(candidates.front());
                current.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
                checks.check(valid && valid_twice, "read-only route measurement retains current lease");
            }
            summarize("previous transfer check (four snapshots)", previous);
            summarize("transfer scope check (two snapshots with ownership guards)", current);
        } else std::printf("Route timing skipped: no physical candidate.\n");
    }
    std::printf("Native transfer cancellation: %d checks, %d failures\n", checks.count, checks.failures);
    WSACleanup();
    return checks.failures ? 1 : 0;
}
