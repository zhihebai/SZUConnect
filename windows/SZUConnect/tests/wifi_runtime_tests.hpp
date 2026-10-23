#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "szu/json.hpp"

namespace wifiruntime {
// Delegation uses the existing offline service for protocol/vault fixtures; no native runtime,
// storage, WLAN handle or watcher is constructed even when testing a normal-mode controller.
class Fake : public szu::app::RuntimeServices {
public:
    Fake() : base_(szu::app::make_runtime(szu::app::RuntimeMode::offlineSmoke)) {
        files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"recover","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
        szu::NetworkMemory memory("00112233445566778899aabbccddeeff");
        szu::NetworkDescriptor descriptor; descriptor.kind="wifi";
        descriptor.raw={{"gwip","192.0.2.1"},{"net","192.0.2.0/24"}};
        memory.remember(descriptor,"teaching",true,false,szu::app::AppController::wall_now(),id);
        files[L"network-memory.json"] = memory.encode();
        szu::NetworkLabels labels; labels.update_automatic(id,{"Fake remembered campus","wifi","SZU-fixture",false});
        files[L"network-labels.json"] = labels.encode();
    }
    szu::app::RuntimeMode mode() const override { return szu::app::RuntimeMode::offlineSmoke; }
    szu::app::RuntimeEvidence& evidence() override { return base_->evidence(); }
    szu::ConnectivityReport probe(szu::ProbeMode p,const szu::CancelToken& t) override { return base_->probe(p,t); }
    szu::CampusDiscovery discover(szu::CampusArea a,const szu::CancelToken& t) override { return base_->discover(a,t); }
    szu::LoginAttempt attempt(const szu::Credentials& c,szu::CampusArea a,const szu::CancelToken& t) override { return base_->attempt(c,a,t); }
    szu::WebsiteCheckResult check_website(const std::string& u,bool f,const szu::CancelToken& t) override { return base_->check_website(u,f,t); }
    szu::compat::Input compatibility(bool p,const std::string& n,const szu::CancelToken& t) override { return base_->compatibility(p,n,t); }
    szu::win::Storage::ReadStatus read(const wchar_t* name,std::string& out) override {
        const auto found=files.find(name);
        if(found==files.end())return szu::win::Storage::ReadStatus::missing;
        out=found->second; return szu::win::Storage::ReadStatus::ok;
    }
    bool write(const wchar_t* name,const std::string& data) override {
        if (fail_write) return false;
        files[name]=data; return true;
    }
    bool write_preserving(const wchar_t* name,const std::string& data,bool) override { return write(name,data); }
    bool quarantine(const wchar_t*) override { ++quarantines; return false; }
    bool storage_available() const override { return true; }
    szu::win::CredentialVault::ReadStatus read_credentials(szu::Credentials& c) override { return base_->read_credentials(c); }
    bool write_credentials(const szu::Credentials& c) override { return base_->write_credentials(c); }
    bool launch_enabled() override { return false; }
    bool launch_approved() override { return true; }
    bool set_launch(bool) override { return false; }
    szu::win::NetworkSnapshot capture_network() override {
        szu::win::NetworkSnapshot snapshot; snapshot.access=physical_path ? "wifi" : "none";
        snapshot.path.link_active=physical_path;
        snapshot.path.usable=virtual_path || physical_path; snapshot.path.tunnel=virtual_path;
        snapshot.path.identity=physical_path ? "fake-connected" : "fake-disconnected"; return snapshot;
    }
    szu::proxy::WindowsRaw read_proxy() override { return base_->read_proxy(); }
    std::string random_hex(std::size_t n) override { return base_->random_hex(n); }
    void start_watchers(std::function<void()>) override {}
    void stop_watchers() override {}
    szu::wifi::Result recover_wifi(const std::vector<szu::wifi::Network>& networks,bool power,const szu::CancelToken& token) override {
        ++base_->evidence().fake_wifi_calls;
        selected = networks; permitted_power = power;
        if (block) while (!release_wifi && !token.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        wifi_cancelled = token.cancelled();
        return token.cancelled() ? szu::wifi::Result::cancelled : szu::wifi::Result::notFound;
    }
    const std::string id="01234567";
    std::map<std::wstring,std::string> files;
    bool fail_write=false, permitted_power=false, virtual_path=false, physical_path=false;
    std::atomic<bool> block{false};
    std::atomic<bool> release_wifi{false}, wifi_cancelled{false};
    int quarantines=0;
    std::vector<szu::wifi::Network> selected;
private:
    std::unique_ptr<szu::app::RuntimeServices> base_;
};
inline void wait_for(const std::function<bool()>& condition) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!condition()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
}
template<class Check> void run_wifi_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    for (const bool physical : {false, true}) {
        auto services = std::make_unique<wifiruntime::Fake>(); auto* fake = services.get();
        fake->physical_path = physical; fake->virtual_path = true;
        wifi::Preferences preferences; preferences.auto_join = true; preferences.power_on = true;
        preferences.excluded.insert(fake->id);
        fake->files[L"wifi-recovery.json"] = *preferences.encode();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto files = fake->files;
        const auto before = app.snapshot(); const auto due = app.next_check();
        check(app.wifi_selection_required() && app.wifi_candidates().size() == 1 && !app.wifi_selected(fake->id) &&
            (!physical || app.wifi_status() == "connected"),
            "zero selected Wi-Fi is a separate incomplete-setting state even while a physical connection hides it in radio status");
        check(app.wifi_selection_required() && fake->files == files && app.next_check() == due &&
            app.snapshot().epoch == before.epoch && app.snapshot().trusted == before.trusted &&
            fake->evidence().fake_wifi_calls == 0 && fake->evidence().fake_auth_submissions == 0,
            "reading missing-target state preserves exclusions, schedule, trust and radio or authentication activity");
        check(app.set_wifi_selected(fake->id, true) && !app.wifi_selection_required() && app.wifi_selected(fake->id),
            "only an explicit target selection completes the missing-target setting without requiring another enable action");
        check(app.set_wifi_selected(fake->id, false) && app.wifi_selection_required() &&
            app.set_wifi_auto_join(false) && !app.wifi_selection_required(),
            "an explicit opt-out is retained and turning finding off removes its incomplete-setting warning");
        app.shutdown();
    }
    for (const bool excluded : {false, true}) {
        auto services = std::make_unique<wifiruntime::Fake>(); auto* fake = services.get();
        wifi::Preferences preferences; preferences.auto_join = true;
        if (excluded) preferences.excluded.insert(fake->id);
        fake->files[L"wifi-recovery.json"] = *preferences.encode();
        const auto files = fake->files;
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services)); app.start();
        check(app.wifi_preferences().auto_join && !app.wifi_selection_required() && !app.set_wifi_selected(fake->id, excluded) &&
            !app.wifi_selection_required() && fake->files == files,
            "read-only isolation never interprets hidden memory as an incomplete target setting or edits the user's selected or excluded choices");
        app.on_timer(kTimerWiFi); app.on_changed();
        check(!app.wifi_selection_required() && fake->files == files && fake->evidence().fake_wifi_calls == 0 &&
            fake->evidence().fake_auth_submissions == 0 && fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0,
            "read-only target presentation never searches, authenticates, reads credentials or changes formal settings");
        app.shutdown();
    }
#ifdef _WIN32
    {
        const HWND window = CreateWindowExW(0, L"STATIC", L"Offline Wi-Fi notification fixture", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(window != nullptr, "isolated notification window is available");
        if (window) {
            auto services = std::make_unique<wifiruntime::Fake>(); auto* fake = services.get();
            AppController app(window, RuntimeMode::normal, std::move(services)); app.start();
            const auto drain_changes = [&] {
                int count = 0; MSG message{};
                while (PeekMessageW(&message, window, kMsgChanged, kMsgChanged, PM_REMOVE)) {
                    ++count; app.on_changed();
                }
                return count;
            };
            drain_changes();
            int changes = 0;
            for (int i = 0; i < 10; ++i) {
                app.on_timer(kTimerWiFi);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                changes += drain_changes();
            }
            std::printf("Disabled Wi-Fi: %d worker-change notifications for 10 timer ticks\n", changes);
            check(changes == 0 && fake->evidence().fake_wifi_calls == 0,
                "disabled finding neither queues empty recovery work nor requests a display refresh");
            app.set_wifi_auto_join(true); drain_changes();
            fake->block = true;
            app.on_timer(kTimerWiFi);
            wifiruntime::wait_for([&] { return fake->evidence().fake_wifi_calls > 0; });
            check(fake->evidence().fake_wifi_calls == 1 && app.wifi_status() == "searching" && drain_changes() > 0,
                "enabling finding still admits due work and publishes the searching state");
            fake->release_wifi = true;
            wifiruntime::wait_for([&] { return app.wifi_status() != "searching"; });
            std::this_thread::sleep_for(std::chrono::milliseconds(20)); drain_changes();
            changes = 0;
            for (int i = 0; i < 10; ++i) {
                app.on_timer(kTimerWiFi);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                changes += drain_changes();
            }
            check(changes == 0 && fake->evidence().fake_wifi_calls == 1 && app.wifi_status() == "waiting",
                "retry backoff retains the result without empty work or redundant notifications");
            fake->physical_path = true; app.refresh_environment(); drain_changes();
            changes = 0;
            for (int i = 0; i < 10; ++i) {
                app.on_timer(kTimerWiFi);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                changes += drain_changes();
            }
            check(changes == 0 && fake->evidence().fake_wifi_calls == 1 && app.wifi_status() == "connected",
                "an existing physical connection suppresses empty work without changing the connection");
            app.shutdown(); DestroyWindow(window);
        }
    }
#endif
    {
        auto services=std::make_unique<wifiruntime::Fake>(); auto* fake=services.get();
        AppController app(nullptr,RuntimeMode::normal,std::move(services)); app.start();
        check(!app.wifi_preferences().auto_join&&!app.wifi_preferences().power_on&&app.wifi_candidates().size()==1,
              "Wi-Fi defaults off while remembered labels populate candidates without granting trust");
        app.on_timer(kTimerWiFi); std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(fake->evidence().fake_wifi_calls==0,"disabled Wi-Fi finding never reaches injected radio");
        check(app.set_wifi_auto_join(true)&&!app.wifi_preferences().power_on,"association enable does not grant radio power-on permission");
        app.on_timer(kTimerWiFi);
        wifiruntime::wait_for([&]{return fake->evidence().fake_wifi_calls>0&&!app.snapshot().busy;});
        app.shutdown();
        check(fake->evidence().fake_wifi_calls==1&&fake->selected.size()==1&&!fake->permitted_power,
              "normal-mode controller uses only remembered selected candidate through fake runtime");
        check(fake->evidence().wifi_calls==0&&fake->evidence().production_storage_accesses==0&&fake->evidence().vault_accesses==0&&
              fake->evidence().network_calls==0&&fake->evidence().watchers_started==0,"injected Wi-Fi test touches no formal permissions/storage/network");
        check(fake->files.count(L"wifi-recovery.json")==1&&fake->files.at(L"settings.json").find("wifi")==std::string::npos,
              "Wi-Fi sidecar preserves the existing settings schema");
    }
    {
        auto services=std::make_unique<wifiruntime::Fake>(); auto* fake=services.get();
        AppController app(nullptr,RuntimeMode::normal,std::move(services)); app.start();
        check(app.set_wifi_auto_join(true)&&app.set_wifi_name(fake->id,"Manual-fixture"),"manual candidate accepts only a remembered Wi-Fi record");
        check(!app.set_wifi_name("89abcdef","Other")&&!app.set_wifi_name(fake->id,"bad\n"),"manual candidate rejects unknown record and unsafe SSID");
        const auto original=fake->files[L"wifi-recovery.json"];
        fake->fail_write=true;
        check(!app.set_wifi_selected(fake->id,false)&&!app.wifi_selected(fake->id)&&fake->files[L"wifi-recovery.json"]==original,
              "failed opt-out write still suppresses candidate in current process and preserves file");
        app.on_timer(kTimerWiFi); std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(fake->evidence().fake_wifi_calls==0,"revoked target cannot associate after disk failure");
        app.forget_all_networks();
        check(app.wifi_candidates().empty()&&app.wifi_preferences().manual_names.empty()&&!app.wifi_preferences().auto_join,
              "forget-all removes candidate/manual name and disables finding even when memory/sidecar writes fail");
        app.shutdown();
    }
    {
        auto services=std::make_unique<wifiruntime::Fake>(); auto* fake=services.get();
        fake->files[L"wifi-recovery.json"]="damaged-original";
        AppController app(nullptr,RuntimeMode::normal,std::move(services)); app.start();
        check(app.wifi_unreadable()&&!app.set_wifi_auto_join(true)&&fake->files[L"wifi-recovery.json"]=="damaged-original"&&fake->quarantines==0,
              "damaged Wi-Fi sidecar is retained unchanged and automatic finding remains disabled");
        app.on_timer(kTimerWiFi); app.shutdown();
        check(fake->evidence().fake_wifi_calls==0,"damaged sidecar never requests WLAN");
    }
    {
        auto services=std::make_unique<wifiruntime::Fake>(); auto* fake=services.get();
        AppController app(nullptr,RuntimeMode::normal,std::move(services)); app.start();
        app.set_wifi_auto_join(true);
        fake->files[L"wifi-recovery.json"]="changed-after-start";
        check(!app.set_wifi_power_on(true)&&app.wifi_unreadable()&&fake->files[L"wifi-recovery.json"]=="changed-after-start",
              "sidecar damage after startup is not overwritten by a later user toggle");
        app.on_timer(kTimerWiFi); app.shutdown();
        check(fake->evidence().fake_wifi_calls==0,"post-start unreadable settings suspend finding");
    }
    {
        auto services=std::make_unique<wifiruntime::Fake>(); auto* fake=services.get();
        wifi::Preferences preferences; preferences.auto_join=true; preferences.power_on=true;
        fake->files[L"wifi-recovery.json"]=*preferences.encode();
        const auto original=fake->files;
        AppController app(nullptr,RuntimeMode::observeOnly,std::move(services)); app.start();
        check(!app.set_wifi_auto_join(false)&&!app.set_wifi_power_on(false)&&!app.set_wifi_name(fake->id,"Other"),
              "observe-only blocks Wi-Fi edits before persistence/native API");
        app.on_timer(kTimerWiFi); app.shutdown();
        check(fake->evidence().fake_wifi_calls==0&&fake->files==original,"observe-only never searches, powers radios or writes the sidecar");
    }
    {
        auto services=std::make_unique<wifiruntime::Fake>(); auto* fake=services.get(); fake->block=true;
        AppController app(nullptr,RuntimeMode::normal,std::move(services)); app.start(); app.set_wifi_auto_join(true);
        app.on_timer(kTimerWiFi); wifiruntime::wait_for([&]{return fake->evidence().fake_wifi_calls>0;});
        app.forget_all_networks(); app.shutdown();
        check(fake->evidence().fake_wifi_calls==1&&app.wifi_candidates().empty()&&app.wifi_status()=="disabled",
              "forget during a blocking fake scan cancels authorization and discards late result without taking over another network");
    }
}
