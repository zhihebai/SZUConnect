#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include "wifi_runtime_tests.hpp"

namespace priorityruntime {
enum class Phase { checking, website, discovering, loggingIn, verifying };
class Fake final : public wifiruntime::Fake {
public:
    explicit Fake(Phase phase) : phase_(phase) {
        if (phase == Phase::website) {
            files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"recover","area":"teaching","probeMode":"complete","appearanceMode":"system"})";
            szu::WebsitePreferences preferences{{{"fixture","https://fixture.invalid/",true}},true};
            files[L"websites.json"] = *preferences.encode();
        }
    }
    szu::win::NetworkSnapshot capture_network() override {
        szu::win::NetworkSnapshot snapshot; snapshot.access="wifi";
        snapshot.path.usable=true; snapshot.path.link_active=true; snapshot.path.identity="fake-priority-physical";
        snapshot.path.descriptors={szu::NetworkDescriptor{"wifi",{{"gwip","192.0.2.1"},{"net","192.0.2.0/24"}}}};
        snapshot.current_label=szu::NetworkLabel{"Fake remembered campus","wifi","SZU-fixture",false};
        return snapshot;
    }
    szu::ConnectivityReport probe(szu::ProbeMode mode,const szu::CancelToken& token) override {
        const auto count=++probe_calls; ++evidence().fake_probe_calls;
        if ((phase_==Phase::checking&&count==1)||(phase_==Phase::verifying&&submitted&&count==3)) gate(token);
        auto report=szu::ConnectivityReport::simple(submitted,submitted); report.mode=mode;
        if(token.cancelled())report.apple.state=report.microsoft.state=szu::EndpointState::cancelled;
        return report;
    }
    szu::WebsiteCheckResult check_website(const std::string&,bool,const szu::CancelToken& token) override {
        const auto count=++website_calls;
        if(phase_==Phase::website&&count==1)gate(token);
        return token.cancelled()?szu::WebsiteCheckResult{"cancelled",0,0}:szu::WebsiteCheckResult{"accessible",200,0};
    }
    szu::CampusDiscovery discover(szu::CampusArea,const szu::CancelToken& token) override {
        ++discovery_calls; if(phase_==Phase::discovering)gate(token);
        return token.cancelled()?szu::CampusDiscovery{}:szu::CampusDiscovery{szu::Discovery::confirmed,szu::Discovery::confirmed};
    }
    szu::LoginAttempt attempt(const szu::Credentials& credentials,szu::CampusArea,const szu::CancelToken& token) override {
        ++attempt_calls; if(phase_==Phase::loggingIn)gate(token);
        szu::LoginAttempt result;
        if(token.cancelled()){result.outcome=szu::Outcome::cancelled;return result;}
        if(credentials!=szu::Credentials{"offline-fixture","offline-password"})return result;
        ++evidence().fake_auth_submissions; submitted=true; result.outcome=szu::Outcome::success; return result;
    }
    std::atomic<int> probe_calls{0},website_calls{0},discovery_calls{0},attempt_calls{0},gate_entries{0};
    std::atomic<bool> release{false},cancelled{false},gate_ended{false},submitted{false};
private:
    void gate(const szu::CancelToken& token) {
        ++gate_entries;
        while(!release&&!token.cancelled())std::this_thread::sleep_for(std::chrono::milliseconds(1));
        cancelled=token.cancelled(); gate_ended=true;
    }
    Phase phase_;
};
}

template<class Check> void run_manual_priority_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    using priorityruntime::Phase;
    for(const auto phase:{Phase::checking,Phase::website}) {
        auto services=std::make_unique<priorityruntime::Fake>(phase); auto* fake=services.get();
        AppController app(nullptr,RuntimeMode::normal,std::move(services)); app.start(); app.on_timer(kTimerAuto);
        wifiruntime::wait_for([&]{return fake->gate_entries>0;});
        check(fake->gate_entries==1&&app.snapshot().busy_kind==OperationKind::automatic&&app.snapshot().status==EngineStatus::checking,
              "controller fixture enters automatic credential-free base/selected-website probe");
        app.check();
        wifiruntime::wait_for([&]{return fake->cancelled&&fake->probe_calls>=2;});
        check(fake->cancelled&&fake->probe_calls==2&&fake->discovery_calls==0&&fake->attempt_calls==0,
              "foreground check cancels suspended automatic probe token before queue wait and immediately runs its own read-only check");
        fake->release=true; app.shutdown();
        check(fake->evidence().network_calls==0&&fake->evidence().wifi_calls==0&&fake->evidence().fake_auth_submissions==0,
              "manual preemption test touches no real network/WLAN and sends no credentials");
    }
    for(const auto phase:{Phase::discovering,Phase::loggingIn,Phase::verifying}) {
        auto services=std::make_unique<priorityruntime::Fake>(phase); auto* fake=services.get();
        AppController app(nullptr,RuntimeMode::normal,std::move(services)); app.start();
        wifiruntime::wait_for([&]{if(fake->gate_entries==0)app.on_timer(kTimerAuto);return fake->gate_entries>0;});
        const auto expected=phase==Phase::discovering?EngineStatus::discovering:phase==Phase::loggingIn?EngineStatus::loggingIn:EngineStatus::verifying;
        check(fake->gate_entries==1&&app.snapshot().status==expected&&app.snapshot().busy_kind==OperationKind::automatic,
              "controller fixture reaches protected discovery/submission/verification phase");
        app.check(); app.connect(); app.check();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(!fake->cancelled&&!fake->gate_ended&&app.snapshot().status==expected,
              "manual clicks cannot cancel protected authentication operation or its token");
        fake->release=true;
        wifiruntime::wait_for([&]{return fake->probe_calls>=3&&!app.snapshot().busy;});
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        check(fake->probe_calls==3&&fake->attempt_calls==1&&fake->evidence().fake_auth_submissions==1,
              "protected-phase clicks are ignored rather than queued as later checks or repeated logins");
        app.shutdown();
    }
    {
        auto services=std::make_unique<wifiruntime::Fake>(); auto* fake=services.get(); fake->block=true; fake->virtual_path=true;
        AppController app(nullptr,RuntimeMode::normal,std::move(services)); app.start(); app.set_wifi_auto_join(true);
        app.on_timer(kTimerWiFi); wifiruntime::wait_for([&]{return fake->evidence().fake_wifi_calls==1;});
        app.check(); app.connect(); app.check();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(!fake->wifi_cancelled&&fake->evidence().fake_probe_calls==0,
              "foreground click does not preempt an active Wi-Fi association job");
        fake->release_wifi=true;
        wifiruntime::wait_for([&]{return fake->evidence().fake_probe_calls>=1;});
        std::this_thread::sleep_for(std::chrono::milliseconds(25)); app.shutdown();
        check(!fake->wifi_cancelled&&fake->evidence().fake_probe_calls==1,
              "multiple clicks behind Wi-Fi work produce at most one pending foreground check");
    }
}
