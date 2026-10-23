#include "szu/win/physical.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "szu/engine.hpp"
#include "szu/address_resolver.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "bcrypt.lib")

namespace szu::win {
namespace {
using Clock = std::chrono::steady_clock;
double now_seconds() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
struct Winsock {
    bool ready = false;
    Winsock() { WSADATA data{}; ready = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
    ~Winsock() { if (ready) WSACleanup(); }
};
struct Sockets {
    std::vector<SOCKET> values;
    ~Sockets() { for (const auto s : values) closesocket(s); }
};
void cancelled(const CancelToken& token) {
    if (token.cancelled()) throw physical::Error(physical::Failure::cancelled);
}
void current(const physical::Route& route, const CancelToken& token) {
    cancelled(token);
    const bool matches = physical_route_current(route);
    cancelled(token);
    if (!matches) throw physical::Error(physical::Failure::unavailable);
}
bool bound_socket(SOCKET socket, const physical::Route& route, const std::string& server) {
    const DWORD index = htonl(route.interface_index);
    if (setsockopt(socket, IPPROTO_IP, IP_UNICAST_IF, reinterpret_cast<const char*>(&index), sizeof(index)) == SOCKET_ERROR) return false;
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR) return false;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    if (InetPtonA(AF_INET, route.source_ipv4.c_str(), &local.sin_addr) != 1 ||
        bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof local) == SOCKET_ERROR) return false;
    sockaddr_in remote{};
    remote.sin_family = AF_INET; remote.sin_port = htons(53);
    if (InetPtonA(AF_INET, server.c_str(), &remote.sin_addr) != 1 ||
        connect(socket, reinterpret_cast<const sockaddr*>(&remote), sizeof remote) == SOCKET_ERROR) return false;
    sockaddr_in actual{}; int length = sizeof actual;
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&actual), &length) == SOCKET_ERROR || actual.sin_family != AF_INET ||
        actual.sin_addr.S_un.S_addr != local.sin_addr.S_un.S_addr) return false;
    DWORD actual_index = 0; length = sizeof actual_index;
    if (getsockopt(socket, IPPROTO_IP, IP_UNICAST_IF, reinterpret_cast<char*>(&actual_index), &length) == SOCKET_ERROR ||
        actual_index != route.interface_index) return false;  // getsockopt returns host byte order.
    sockaddr_in peer{}; length = sizeof peer;
    return getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &length) == 0 && peer.sin_family == AF_INET &&
        peer.sin_port == remote.sin_port && peer.sin_addr.S_un.S_addr == remote.sin_addr.S_un.S_addr;
}
}  // namespace

static std::vector<std::string> resolve_physical_udp_a(const std::string& host, const physical::Route& route,
                                                     int timeout_ms, const CancelToken& token) {
    cancelled(token);
    if (!physical::allowed_host(host) || timeout_ms <= 0 || timeout_ms > 30000 || !route.interface_index ||
        route.interface_index > 0x00ffffffu || !route.interface_luid || !physical::usable_ipv4(route.source_ipv4) ||
        route.addresses.empty() || route.addresses.size() > 16 || route.addresses.front() != route.source_ipv4 ||
        route.dns_servers.empty() || route.dns_servers.size() > 8 ||
        !std::all_of(route.addresses.begin(), route.addresses.end(), physical::usable_ipv4) ||
        !std::all_of(route.dns_servers.begin(), route.dns_servers.end(), physical::usable_ipv4))
        throw physical::Error(physical::Failure::unavailable);
    const auto deadline = now_seconds() + timeout_ms / 1000.0;
    current(route, token);
    NET_LUID luid{}; luid.Value = route.interface_luid;
    NET_IFINDEX actual_index = 0;
    if (ConvertInterfaceLuidToIndex(&luid, &actual_index) != NO_ERROR || actual_index != route.interface_index)
        throw physical::Error(physical::Failure::unavailable);
    Winsock winsock;
    if (!winsock.ready) throw physical::Error(physical::Failure::unavailable);
    std::uint16_t transaction = 0;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&transaction), sizeof transaction, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw physical::Error(physical::Failure::unavailable);
    Sockets sockets;
    for (const auto& server : route.dns_servers) {
        cancelled(token);
        if (now_seconds() >= deadline) throw physical::Error(physical::Failure::timeout);
        const auto s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) continue;
        if (!bound_socket(s, route, server)) { closesocket(s); continue; }
        sockets.values.push_back(s);
    }
    cancelled(token);
    if (sockets.values.empty()) throw physical::Error(physical::Failure::unavailable);
    physical::ExchangeHooks hooks;
    hooks.now = now_seconds;
    hooks.cancelled = [&] { return token.cancelled(); };
    hooks.send = [&](std::size_t i, const physical::Packet& packet) {
        current(route, token);
        if (now_seconds() >= deadline) throw physical::Error(physical::Failure::timeout);
        return send(sockets.values[i], reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0) ==
            static_cast<int>(packet.size());
    };
    hooks.receive = [&](const std::vector<std::size_t>& active, double wait) {
        std::vector<WSAPOLLFD> poll;
        for (const auto i : active) poll.push_back({sockets.values[i], POLLRDNORM, 0});
        const auto milliseconds = static_cast<INT>(std::clamp(std::floor(wait * 1000.0), 1.0, 50.0));
        const auto result = WSAPoll(poll.data(), static_cast<ULONG>(poll.size()), milliseconds);
        cancelled(token);
        if (result == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEINTR) return std::vector<physical::ExchangeEvent>{};
            throw physical::Error(physical::Failure::unavailable);
        }
        std::vector<physical::ExchangeEvent> events;
        for (std::size_t i = 0; i < poll.size(); ++i) {
            if (poll[i].revents & POLLRDNORM) {
                physical::Packet packet(65536);
                const auto got = recv(poll[i].fd, reinterpret_cast<char*>(packet.data()), static_cast<int>(packet.size()), 0);
                if (got >= 0) {
                    packet.resize(static_cast<std::size_t>(got));
                    events.push_back({physical::ExchangeEvent::Kind::packet, active[i], std::move(packet)});
                } else {
                    const auto error = WSAGetLastError();
                    if (error != WSAEWOULDBLOCK && error != WSAEINTR) events.push_back({physical::ExchangeEvent::Kind::failed, active[i], {}});
                }
            }
            if (poll[i].revents & (POLLERR | POLLHUP | POLLNVAL)) events.push_back({physical::ExchangeEvent::Kind::failed, active[i], {}});
        }
        return events;
    };
    auto result = physical::dns_exchange(host, transaction, sockets.values.size(), deadline, hooks);
    current(route, token);
    if (now_seconds() >= deadline) throw physical::Error(physical::Failure::timeout);
    return result;
}

std::vector<std::string> resolve_physical_a(const std::string& host, const physical::Route& route,
                                          int timeout_ms, const CancelToken& token) {
    static physical::AddressResolver resolver;
    physical::AddressResolver::Hooks hooks;
    hooks.now = now_seconds;
    hooks.cancelled = [&] { return token.cancelled(); };
    hooks.current = [&](const physical::Route& candidate) { return physical_route_current(candidate); };
    hooks.associated = [&](std::string_view name, const physical::Route& candidate, int budget) {
        return resolve_physical_udp_a(std::string(name), candidate, budget, token);
    };
    hooks.encrypted = [&](std::string_view name, const physical::Route& candidate, int budget) {
        return resolve_physical_https_a(std::string(name), candidate, budget, token);
    };
    return resolver.resolve(host, route, timeout_ms, hooks);
}
}  // namespace szu::win
