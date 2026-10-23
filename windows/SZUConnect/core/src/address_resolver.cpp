#include "szu/address_resolver.hpp"
#include <algorithm>
#include <cmath>

namespace szu::physical {
namespace {
bool https_target(std::string_view host) { return host == "net.szu.edu.cn" || host == "www.apple.com"; }
bool valid_answers(const std::vector<std::string>& values) {
    return !values.empty() && values.size() <= 16 && std::all_of(values.begin(), values.end(), usable_ipv4);
}
void current(const Route& route, const AddressResolver::Hooks& hooks) {
    if (hooks.cancelled()) throw Error(Failure::cancelled);
    if (!hooks.current(route)) throw Error(Failure::unavailable);
    if (hooks.cancelled()) throw Error(Failure::cancelled);
}
}

std::optional<AddressResolver::Entry> AddressResolver::cached(std::string_view host, const Route& route, double now) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [now](const Entry& value) {
        return now >= value.retain_until;
    }), entries_.end());
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& value) {
        return value.host == host && value.route == route;
    });
    return found == entries_.end() ? std::nullopt : std::optional<Entry>(*found);
}

std::optional<std::vector<std::string>> AddressResolver::retained(const Entry& entry, double now, bool stale) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [now](const Entry& value) {
        return now >= value.retain_until;
    }), entries_.end());
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& value) {
        return value.id == entry.id && value.host == entry.host && value.route == entry.route;
    });
    // A newer answer, including TTL zero, may have replaced or removed the snapshot.
    if (found == entries_.end() || now >= (stale ? found->retain_until : found->fresh_until)) return std::nullopt;
    return found->addresses;
}

void AddressResolver::remember(std::string_view host, const Route& route, const std::vector<std::string>& addresses,
                               double now, double ttl) {
    ttl = std::isfinite(ttl) ? std::clamp(ttl, 0.0, 300.0) : 0;
    // A DNS TTL governs ordinary reuse. HTTPS can additionally try a prior address
    // for at most a day after both resolvers fail, with fresh hostname/chain checks.
    const double retention = https_target(host) ? 86400.0 : ttl;
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& value) {
        return now >= value.retain_until || (value.host == host && value.route == route);
    }), entries_.end());
    if (ttl <= 0 || retention <= 0) return;
    if (entries_.size() >= 24) entries_.erase(entries_.begin());
    entries_.push_back({next_entry_id_++, std::string(host), route, addresses, now + ttl, now + retention});
}

std::vector<std::string> AddressResolver::resolve(std::string_view host, const Route& route, int timeout_ms, const Hooks& hooks) {
    if (!allowed_host(host) || timeout_ms <= 0 || timeout_ms > 30000 || !hooks.now || !hooks.cancelled || !hooks.current ||
        !hooks.associated || !hooks.encrypted || route.identity.empty() || !route.interface_index || !route.interface_luid ||
        !usable_ipv4(route.source_ipv4) || route.addresses.empty() || route.addresses.front() != route.source_ipv4 ||
        route.addresses.size() > 16 || route.dns_servers.empty() || route.dns_servers.size() > 8 ||
        !std::all_of(route.addresses.begin(), route.addresses.end(), usable_ipv4) ||
        !std::all_of(route.dns_servers.begin(), route.dns_servers.end(), usable_ipv4)) throw Error(Failure::unavailable);
    const double began = hooks.now();
    if (!std::isfinite(began)) throw Error(Failure::unavailable);
    const double deadline = began + timeout_ms / 1000.0;
    const auto remaining = [&] {
        const double value = hooks.now();
        if (!std::isfinite(value) || value < began) throw Error(Failure::unavailable);
        return static_cast<int>(std::clamp(std::floor((deadline - value) * 1000.0), 0.0, 30000.0));
    };
    const auto accept = [&](std::vector<std::string> values) {
        if (remaining() <= 0) throw Error(Failure::timeout);
        current(route, hooks);
        return values;
    };
    current(route, hooks);
    Failure failure = Failure::unavailable;
    try {
        const int budget = std::min(1000, remaining());
        if (budget <= 0) throw Error(Failure::timeout);
        auto addresses = hooks.associated(host, route, budget);
        current(route, hooks);
        if (remaining() <= 0) throw Error(Failure::timeout);
        if (!valid_answers(addresses)) throw Error(Failure::invalidResponse);
        return accept(std::move(addresses));
    } catch (const Error& error) {
        if (error.reason == Failure::cancelled || error.reason == Failure::security) throw;
        failure = error.reason;
    }
    current(route, hooks);
    const auto previous = cached(host, route, hooks.now());
    if (remaining() <= 0) throw Error(Failure::timeout);
    if (previous)
        if (auto addresses = retained(*previous, hooks.now(), false)) return accept(std::move(*addresses));
    try {
        const int reserved = previous && https_target(host) ? 100 : 0;
        const int budget = remaining() - reserved;
        if (budget <= 0) throw Error(Failure::timeout);
        auto answer = hooks.encrypted(host, route, budget);
        current(route, hooks);
        if (remaining() <= 0) throw Error(Failure::timeout);
        if (!valid_answers(answer.addresses)) throw Error(Failure::invalidResponse);
        const double received = hooks.now();
        current(route, hooks);
        if (remaining() <= 0) throw Error(Failure::timeout);
        remember(host, route, answer.addresses, received, answer.ttl_seconds);
        return accept(std::move(answer.addresses));
    } catch (const Error& error) {
        if (error.reason == Failure::cancelled || error.reason == Failure::security) throw;
        failure = error.reason;
    }
    current(route, hooks);
    if (remaining() <= 0) throw Error(Failure::timeout);
    // A candidate is allowed only for the two fixed HTTPS hosts. HTTP probes
    // never reuse an expired address, and no cache read extends its lifetime.
    if (previous && https_target(host))
        if (auto addresses = retained(*previous, hooks.now(), true)) return accept(std::move(*addresses));
    throw Error(failure);
}
}
