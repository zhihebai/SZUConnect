#pragma once
#include "szu/address_resolver.hpp"
#include <atomic>
#include <chrono>
#include <limits>
#include <semaphore>
#include <thread>

namespace addresstest {
struct Fixture {
    szu::physical::AddressResolver resolver;
    szu::physical::Route route;
    double time = 100, ttl = 120;
    bool live = true, stopped = false, associated_ok = false, encrypted_ok = true;
    int associated_calls = 0, encrypted_calls = 0, associated_budget = 0, encrypted_budget = 0;
    std::vector<std::string> addresses{"192.0.2.45"};
    szu::physical::Failure encrypted_error = szu::physical::Failure::timeout;
    std::function<void()> during_encrypted;
    Fixture() {
        route.interface_key = "fixture-resolver-interface"; route.interface_index = 7; route.interface_luid = 7;
        route.identity = "fixture-resolver-attachment"; route.source_ipv4 = "192.0.2.7";
        route.addresses = {route.source_ipv4}; route.dns_servers = {"192.0.2.53"}; route.gateway_ipv4 = "192.0.2.1";
    }
    szu::physical::AddressResolver::Hooks hooks() {
        return {[&] { return time; }, [&] { return stopped; }, [&](const auto&) { return live; },
            [&](std::string_view, const auto&, int budget) {
                ++associated_calls; associated_budget = budget; time += budget / 1000.0;
                if (!associated_ok) throw szu::physical::Error(szu::physical::Failure::timeout);
                return addresses;
            },
            [&](std::string_view, const auto&, int budget) {
                ++encrypted_calls; encrypted_budget = budget; time += std::min(0.25, budget / 2000.0);
                if (during_encrypted) during_encrypted();
                if (!encrypted_ok) throw szu::physical::Error(encrypted_error);
                return szu::physical::ResolvedAddresses{addresses, ttl};
            }};
    }
    std::vector<std::string> get(std::string_view host = "net.szu.edu.cn") { return resolver.resolve(host, route, 6000, hooks()); }
    bool fails(std::string_view host = "net.szu.edu.cn") {
        try { (void)get(host); return false; } catch (const szu::physical::Error&) { return true; }
    }
};
}

inline void address_resolver_tests(Harness& t) {
    using namespace szu::physical;
    t.begin("bounded-encrypted-address-resolution");
    {
        addresstest::Fixture f;
        t.check(f.get() == f.addresses && f.associated_calls == 1 && f.encrypted_calls == 1 &&
            f.associated_budget == 1000 && f.encrypted_budget <= 5000 && f.time < 106,
            "associated DNS gets first bounded opportunity before encrypted resolution within one deadline");
        f.encrypted_ok = false;
        t.check(f.get() == f.addresses && f.associated_calls == 2 && f.encrypted_calls == 1,
            "a fresh same-attachment encrypted answer can avoid a duplicate HTTPS resolver request");
        f.time += 121;
        t.check(f.get() == f.addresses && f.encrypted_calls == 2,
            "after DNS TTL expires an unavailable resolver may leave a bounded HTTPS connection candidate");
        f.time = 86501.3;
        t.check(f.fails(), "reading old HTTPS candidates does not extend their one-day maximum retention");
    }
    {
        addresstest::Fixture f; f.associated_ok = true;
        t.check(f.get() == f.addresses && f.encrypted_calls == 0,
            "a valid associated DNS answer remains first choice and contacts no public resolver");
        f.associated_ok = false; f.encrypted_ok = false;
        t.check(f.fails(), "the legacy associated-DNS interface does not invent cache TTL metadata");
    }
    for (const auto host : {"net.szu.edu.cn", "www.apple.com", "www.msftconnecttest.com"}) {
        addresstest::Fixture f;
        t.check(f.get(host) == f.addresses, "only fixed campus and built-in probe hosts admit encrypted resolution");
        f.time += 121; f.encrypted_ok = false;
        t.check((host == std::string_view("www.msftconnecttest.com")) ? f.fails(host) : !f.fails(host),
            "expired candidates are restricted to HTTPS hosts and never feed the HTTP connectivity probe");
    }
    for (const auto host : {"example.com", "NET.SZU.EDU.CN", "net.szu.edu.cn.evil.invalid", "172.30.255.42", "dns.alidns.com"}) {
        addresstest::Fixture f;
        t.check(f.fails(host) && f.associated_calls == 0 && f.encrypted_calls == 0,
            "custom, malformed and bootstrap hosts cannot expand the fixed lookup allowlist");
    }
    for (int mutation = 0; mutation < 7; ++mutation) {
        addresstest::Fixture f; (void)f.get(); f.encrypted_ok = false;
        switch (mutation) {
            case 0: f.route.identity += "-new"; break;
            case 1: f.route.interface_key += "-new"; break;
            case 2: ++f.route.interface_index; break;
            case 3: ++f.route.interface_luid; break;
            case 4: f.route.source_ipv4 = "192.0.2.8"; f.route.addresses = {f.route.source_ipv4}; break;
            case 5: f.route.dns_servers = {"192.0.2.54"}; break;
            case 6: f.route.gateway_ipv4 = "192.0.2.2"; break;
        }
        t.check(f.fails(), "address candidates do not cross physical interface, source, DNS, gateway or attachment changes");
    }
    for (const auto failure : {Failure::cancelled, Failure::security}) {
        addresstest::Fixture f; (void)f.get(); f.time += 121;
        f.encrypted_ok = false; f.encrypted_error = failure;
        t.check(f.fails(), "cancellation or TLS security failure cannot fall back to an old HTTPS candidate");
    }
    for (const auto ttl : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
        addresstest::Fixture f; f.ttl = ttl; (void)f.get(); f.encrypted_ok = false;
        t.check(f.fails(), "zero, negative or non-finite DNS TTL never creates a retained address entry");
        addresstest::Fixture prior; (void)prior.get(); prior.time += 121; prior.ttl = ttl;
        (void)prior.get(); prior.encrypted_ok = false;
        t.check(prior.fails(), "a new uncached answer also revokes a previously retained address for that attachment");
    }
    for (const auto invalid : {"198.18.0.1", "127.0.0.1", "0.0.0.0", "224.0.0.1"}) {
        addresstest::Fixture f; f.addresses = {invalid};
        t.check(f.fails(), "encrypted responses still reject FakeIP, loopback and unusable address candidates");
    }
    for (const bool cancel : {false, true}) {
        addresstest::Fixture f;
        f.during_encrypted = [&] { if (cancel) f.stopped = true; else f.live = false; };
        t.check(f.fails(), "a late encrypted answer cannot survive cancellation or a changed physical lease");
        f.during_encrypted = {}; f.stopped = false; f.live = true; f.encrypted_ok = false;
        t.check(f.fails(), "discarded late answers never populate the address cache");
    }
    {
        addresstest::Fixture f; (void)f.get(); f.time += 121;
        f.during_encrypted = [&] { f.time += 10; }; f.encrypted_ok = false;
        t.check(f.fails(), "a backend overrun cannot publish an old candidate after the common DNS deadline");
    }
    {
        addresstest::Fixture f; f.ttl = 20000;
        (void)f.get("www.msftconnecttest.com"); f.time += 301; f.encrypted_ok = false;
        t.check(f.fails("www.msftconnecttest.com") && f.encrypted_calls == 2,
            "the final resolver still caps advertised DNS lifetimes at five minutes and HTTP has no stale fallback");
    }
    {
        addresstest::Fixture f;
        f.during_encrypted = [&] { f.time += std::max(0.0, f.encrypted_budget / 1000.0 - 0.26); };
        const double cold_began = f.time;
        const auto cold = f.resolver.resolve("net.szu.edu.cn", f.route, 4200, f.hooks());
        const double cold_elapsed = f.time - cold_began;
        t.check(cold == f.addresses && cold_elapsed > 4 && cold_elapsed < 4.2 && f.associated_calls == 1 && f.encrypted_calls == 1,
            "an empty resolver cache accepts a slow valid encrypted answer inside the original bounded DNS deadline");
        f.during_encrypted = {}; f.encrypted_ok = false;
        const double warm_began = f.time;
        const auto warm = f.resolver.resolve("net.szu.edu.cn", f.route, 4200, f.hooks());
        t.check(warm == f.addresses && f.time - warm_began < cold_elapsed && f.encrypted_calls == 1,
            "a fresh warm address can shorten DNS work without changing the request budget or bypassing associated DNS");
    }
    {
        addresstest::Fixture f; f.during_encrypted = [&] { f.time += 10; };
        t.check(f.fails(), "an over-deadline cold answer is rejected without any earlier cached candidate");
        f.during_encrypted = {}; f.encrypted_ok = false;
        t.check(f.fails(), "an over-deadline first answer never seeds a cache that masks a subsequent resolver failure");
    }
    for (const bool same_key : {false, true}) for (const double replacement_ttl : {0.0, 120.0}) {
        AddressResolver resolver;
        const auto route = addresstest::Fixture{}.route;
        const std::string host = "net.szu.edu.cn";
        const std::vector<std::string> addresses{"192.0.2.80"};
        std::atomic<double> clock{100};
        const auto hooks = [&](std::function<ResolvedAddresses(std::string_view, const Route&, int)> encrypted) {
            AddressResolver::Hooks result;
            result.now = [&] { return clock.load(); }; result.cancelled = [] { return false; };
            result.current = [](const Route&) { return true; };
            result.associated = [](std::string_view, const Route&, int) -> std::vector<std::string> { throw Error(Failure::unavailable); };
            result.encrypted = std::move(encrypted); return result;
        };
        (void)resolver.resolve(host, route, 6000, hooks([&](auto, const auto&, int) { return ResolvedAddresses{addresses, 1}; }));
        clock = 102;
        std::binary_semaphore entered{0}, released{0};
        bool barrier_released = false;
        std::optional<Failure> old_failure;
        std::vector<std::string> old_result;
        std::thread old([&] {
            try {
                old_result = resolver.resolve(host, route, 6000, hooks([&](auto, const auto&, int) -> ResolvedAddresses {
                    entered.release(); barrier_released = released.try_acquire_for(std::chrono::seconds(5));
                    throw Error(Failure::unavailable);
                }));
            } catch (const Error& error) { old_failure = error.reason; }
        });
        const bool waiting = entered.try_acquire_for(std::chrono::seconds(5));
        bool new_answer = false;
        try {
            new_answer = resolver.resolve(same_key ? host : "www.apple.com", route, 6000,
                hooks([&](auto, const auto&, int) { return ResolvedAddresses{addresses, replacement_ttl}; })) == addresses;
        } catch (const Error&) {}
        released.release(); old.join();
        t.check(waiting && barrier_released && new_answer, "parallel cache regression reaches both real resolver requests through the barrier");
        t.check(same_key ? old_failure == Failure::unavailable && old_result.empty() : !old_failure && old_result == addresses,
            same_key ? "a same-key revocation or replacement cannot resurrect the old Entry even when the address bytes are identical"
                : "a different hostname on the same physical route leaves its still-retained Entry usable");
    }
}
