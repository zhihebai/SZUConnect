#pragma once
#include <mutex>
#include "szu/physical.hpp"

namespace szu::physical {

// Addresses are connection candidates only. The caller still verifies the bound
// attachment, TLS peer and campus page on every request; no service proof lives here.
class AddressResolver {
public:
    struct Hooks {
        std::function<double()> now;
        std::function<bool()> cancelled;
        std::function<bool(const Route&)> current;
        std::function<std::vector<std::string>(std::string_view, const Route&, int)> associated;
        std::function<ResolvedAddresses(std::string_view, const Route&, int)> encrypted;
        std::function<ResolvedAddresses(std::string_view, const Route&, int)> associated_with_ttl;
    };
    std::vector<std::string> resolve(std::string_view host, const Route& route, int timeout_ms, const Hooks& hooks);
private:
    struct Entry {
        std::uint64_t id = 0;
        std::string host;
        Route route;
        std::vector<std::string> addresses;
        double fresh_until = 0, retain_until = 0;
    };
    struct Revision { std::string host; Route route; std::uint64_t query = 0; };
    std::mutex mutex_;
    std::uint64_t next_entry_id_ = 1, next_query_id_ = 1, evicted_query_floor_ = 0;
    std::vector<Entry> entries_;
    std::vector<Revision> revisions_;
    std::uint64_t begin_query();
    std::optional<Entry> cached(std::string_view host, const Route& route, double now);
    std::optional<std::vector<std::string>> retained(const Entry& entry, double now, bool stale);
    void remember(std::string_view host, const Route& route, const std::vector<std::string>& addresses,
                  double now, double ttl, std::uint64_t query);
};
}
