// Minimal test harness + scripted fake service shared by the core test executables.
#pragma once
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "szu/engine.hpp"
#include "szu/json.hpp"

namespace szutest {

struct Harness {
    int checks = 0;
    int failures = 0;
    std::string section;
    void begin(const std::string& name) { section = name; }
    void check(bool ok, const std::string& what) {
        ++checks;
        if (!ok) {
            ++failures;
            std::printf("  FAIL [%s] %s\n", section.c_str(), what.c_str());
        }
    }
    template <class A, class B>
    void equal(const A& a, const B& b, const std::string& what) {
        check(a == b, what);
    }
};

inline std::string root_dir = "../../..";

inline szu::json::Value load(const std::string& relative) {
    std::ifstream in(root_dir + "/" + relative, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    auto value = szu::json::parse(buffer.str());
    if (!value) {
        std::printf("cannot load fixture %s\n", relative.c_str());
        std::exit(2);
    }
    return *value;
}

inline std::string str(const szu::json::Value& v, std::string_view key, std::string fallback = {}) {
    return v.string_at(key).value_or(fallback);
}

// Mirrors tools/reference/recovery_reference.py Fake.
class ScriptedService final : public szu::ConnectionService {
public:
    std::deque<std::string> probes;
    std::string default_probe = "offline";
    std::vector<std::map<std::string, std::string>> discovery{{{"teaching", "unconfirmed"}, {"dormitory", "notChecked"}}};
    std::map<std::string, std::string> attempts;
    std::vector<std::pair<std::string, std::string>> submissions;
    std::vector<std::pair<std::string, std::string>> attempt_log;
    int discoveries = 0;
    int probe_calls = 0;
    std::function<void()> during_probe;
    std::function<void()> during_discovery;
    std::function<void()> during_attempt;
    std::vector<szu::CampusArea> discovery_areas;

    szu::ConnectivityReport probe(szu::ProbeMode mode, const szu::CancelToken& token) override {
        ++probe_calls;
        if (during_probe) during_probe();
        std::string p = default_probe;
        if (!probes.empty()) {
            p = probes.front();
            probes.pop_front();
        }
        auto report = szu::ConnectivityReport::simple(p == "online" || p == "partial", p == "online");
        if (p == "partial") report.microsoft.state = szu::EndpointState::redirect;
        if (token.cancelled()) {
            report.apple.state = szu::EndpointState::cancelled;
            report.microsoft.state = szu::EndpointState::cancelled;
        }
        report.mode = mode;
        return report;
    }

    szu::CampusDiscovery discover(szu::CampusArea area, const szu::CancelToken&) override {
        ++discoveries;
        discovery_areas.push_back(area);
        if (during_discovery) during_discovery();  // deliberately allows a late successful response after cancellation
        std::map<std::string, std::string> value = discovery.front();
        if (discovery.size() > 1) discovery.erase(discovery.begin());
        szu::CampusDiscovery d;
        d.teaching = *szu::discovery_from(value["teaching"]);
        d.dormitory = area == szu::CampusArea::teaching ? szu::Discovery::notChecked : *szu::discovery_from(value["dormitory"]);
        return d;
    }

    szu::LoginAttempt attempt(const szu::Credentials& credentials, szu::CampusArea area, const szu::CancelToken&) override {
        if (during_attempt) during_attempt();  // allow late results and elapsed-time fixtures
        const std::string name(szu::to_string(area));
        const std::string token = attempts.count(name) ? attempts[name] : "unavailable";
        attempt_log.emplace_back(name, token);
        if (token != "unavailable" && token != "challengeRefused" && token != "security") submissions.emplace_back(name, credentials.account);
        szu::LoginAttempt a;
        if (token.rfind("rejected:", 0) == 0) {
            a.outcome = szu::Outcome::rejected;
            const std::string kind = token.substr(9);
            a.rejection = kind == "password" ? szu::RejectionKind::password
                        : kind == "account" ? szu::RejectionKind::account
                        : kind == "areaMismatch" ? szu::RejectionKind::areaMismatch
                                                 : szu::RejectionKind::unspecified;
            a.error = szu::PortalError::rejected;
            return a;
        }
        for (szu::Outcome o : {szu::Outcome::success, szu::Outcome::unavailable, szu::Outcome::challengeRefused, szu::Outcome::security,
                               szu::Outcome::uncertain, szu::Outcome::invalidCredentials, szu::Outcome::cancelled})
            if (szu::to_string(o) == token) a.outcome = o;
        a.credential_submission_started = a.outcome != szu::Outcome::unavailable && a.outcome != szu::Outcome::challengeRefused && a.outcome != szu::Outcome::security;
        return a;
    }
};

}  // namespace szutest
