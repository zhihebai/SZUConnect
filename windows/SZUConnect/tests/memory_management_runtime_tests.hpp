#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <thread>
#include <string>
#include <utility>

namespace memorymergeruntime {
class Fake final : public wifiruntime::Fake {
public:
    Fake() {
        files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
        szu::NetworkMemory memory("00112233445566778899aabbccddeeff");
        const auto now = szu::app::AppController::wall_now();
        memory.remember({"wifi", {{"gwip", "192.0.2.1"}, {"net", "192.0.2.0/24"}}}, "teaching", true, false, now, "11111111");
        memory.remember({"wifi", {{"gw", "02:00:00:00:00:01"}, {"net", "192.0.2.0/24"}}}, "teaching", true, false, now, "22222222");
        files[L"network-memory.json"] = memory.encode();
        szu::NetworkLabels labels;
        labels.update_automatic("11111111", {"Fixture campus", "wifi", "Fixture campus", false});
        labels.update_automatic("22222222", {"Fixture campus", "wifi", "Fixture campus", false});
        labels.rename("22222222", "Fixture survivor");
        files[L"network-labels.json"] = labels.encode();
        szu::wifi::Preferences preferences;
        preferences.auto_join = true; preferences.excluded.insert("11111111");
        files[L"wifi-recovery.json"] = *preferences.encode();
    }
    szu::win::NetworkSnapshot capture_network() override {
        szu::win::NetworkSnapshot result;
        result.path.usable = result.path.link_active = true;
        result.path.identity = "fixture-memory-merge";
        result.path.descriptors = {{"wifi", {{"gw", "02:00:00:00:00:01"}, {"gwip", "192.0.2.1"}, {"net", "192.0.2.0/24"}}}};
        result.current_label = szu::NetworkLabel{"Fixture campus", "wifi", expose_ssid ? std::optional<std::string>("Fixture campus") : std::nullopt, false};
        result.access = "wifi";
        return result;
    }
    szu::CampusDiscovery discover(szu::CampusArea, const szu::CancelToken& token) override {
        const auto preferences = szu::wifi::Preferences::decode(files[L"wifi-recovery.json"]);
        const auto memory = szu::NetworkMemory::decode(files[L"network-memory.json"], szu::app::AppController::wall_now());
        opt_out_saved_before_discovery = preferences && preferences->excluded.count("11111111") &&
            preferences->excluded.count("22222222") && memory && memory->size() == 2;
        ++discoveries;
        while (block_discovery && !release_discovery && !token.cancelled())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++returned_discoveries;
        return {szu::Discovery::confirmed, szu::Discovery::notChecked};
    }
    bool write(const wchar_t* name, const std::string& data) override {
        if (fail_preferences && std::wstring(name) == L"wifi-recovery.json") return false;
        return wifiruntime::Fake::write(name, data);
    }
    bool fail_preferences = false, expose_ssid = true;
    std::atomic<int> discoveries{0}, returned_discoveries{0};
    std::atomic<bool> opt_out_saved_before_discovery{false}, block_discovery{false}, release_discovery{false};
};

class NameDisplayFake final : public wifiruntime::Fake {
public:
    NameDisplayFake() {
        files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"automatic","probeMode":"quick","appearanceMode":"system"})";
        szu::NetworkMemory memory("00112233445566778899aabbccddeeff");
        const auto now = szu::app::AppController::wall_now();
        memory.remember(route(1).descriptor, "teaching", true, false, now, "11111111");
        memory.remember(route(2).descriptor, "dormitory", true, true, now, "22222222");
        files[L"network-memory.json"] = memory.encode();
        szu::NetworkLabels labels;
        for (const auto* record_id : {"11111111", "22222222"})
            labels.update_automatic(record_id, {"Fixture campus", "wifi", "Fixture campus", false});
        files[L"network-labels.json"] = labels.encode();
        szu::wifi::Preferences preferences; preferences.auto_join = true;
        preferences.excluded.insert("22222222");
        files[L"wifi-recovery.json"] = *preferences.encode();
    }
    szu::physical::Route route(int index) const {
        const bool first = index == 1;
        szu::physical::Route value;
        value.interface_key = first ? "fixture-name-first" : "fixture-name-second";
        value.interface_index = first ? 21u : 22u; value.interface_luid = value.interface_index;
        value.source_ipv4 = first ? "192.0.2.7" : "198.51.100.7";
        value.addresses = {value.source_ipv4}; value.dns_servers = {first ? "192.0.2.53" : "198.51.100.53"};
        value.gateway_ipv4 = first ? "192.0.2.1" : "198.51.100.1";
        value.descriptor = {"wifi", {{"gw", first ? "02:00:00:00:00:01" : "02:00:00:00:00:02"},
            {"gwip", value.gateway_ipv4}, {"net", first ? "192.0.2.0/24" : "198.51.100.0/24"}}};
        value.identity = value.interface_key; value.display_name = "Fixture WLAN";
        return value;
    }
    szu::win::NetworkSnapshot capture_network() override {
        szu::win::NetworkSnapshot result;
        if (disconnected) return result;
        result.access = "wifi"; result.path.usable = result.path.link_active = true;
        result.path.requires_physical_binding = true;
        result.path.identity = "fixture-name-binding";
        const auto current = route(second ? 2 : 1);
        result.path.descriptors = {current.descriptor};
        if (!missing_binding) result.path.physical_candidates = {current};
        if (ambiguous_binding) {
            result.path.physical_candidates = {route(1), route(2)};
            result.path.descriptors = {route(1).descriptor, route(2).descriptor};
        }
        result.current_label = szu::NetworkLabel{"Fixture campus", "wifi", "Fixture campus", false};
        return result;
    }
    std::optional<szu::NetworkLabel> network_label(const szu::physical::Route&) override {
        return szu::NetworkLabel{"Fixture campus", "wifi", "Fixture campus", false};
    }
    bool set_expected_physical_route(const std::optional<szu::physical::Route>& attachment) override {
        return attachment && !missing_binding && !ambiguous_binding && *attachment == route(second ? 2 : 1);
    }
    bool second = false, missing_binding = false, ambiguous_binding = false, disconnected = false;
};
}

template<class Check> void run_memory_management_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    {
        auto services = std::make_unique<memorymergeruntime::Fake>(); auto* fake = services.get();
        fake->expose_ssid = false; fake->block_discovery = true;
        NetworkLabels labels;
        labels.update_automatic("11111111", {"Fixture source", "wifi", "Fixture preserved SSID", false});
        labels.rename("11111111", "Fixture source remark");
        labels.update_automatic("22222222", {"Fixture survivor", "wifi", std::nullopt, false});
        fake->files[L"network-labels.json"] = labels.encode();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(app.remember_network(), "sidecar serialization fixture starts duplicate verification");
        wifiruntime::wait_for([&] { return fake->discoveries == 1; });
        check(!app.rename_network("22222222", "Fixture concurrent rename"),
              "UI metadata writes cannot race a queued or active memory merge");
        fake->release_discovery = true;
        bool renamed = false;
        wifiruntime::wait_for([&] {
            if (!app.snapshot().busy) renamed = app.rename_network("22222222", "Fixture post-merge rename");
            return renamed;
        });
        const auto stored = NetworkLabels::decode(fake->files[L"network-labels.json"]);
        check(renamed && stored && stored->entries().at("22222222").ssid == "Fixture preserved SSID" &&
              stored->entries().at("22222222").display_name == "Fixture post-merge rename",
              "rename before result delivery reloads committed metadata and preserves the copied source SSID");
        app.refresh_environment(); app.on_changed();
        check(app.remembered_networks().size() == 1 && app.network_name("22222222") == "Fixture post-merge rename" &&
              app.wifi_name("22222222") == "Fixture preserved SSID" && !app.network_name_known("11111111"),
              "environment refresh and delayed worker feedback preserve the final remark and SSID after duplicate removal");
        app.shutdown();
    }
    {
        auto services = std::make_unique<memorymergeruntime::Fake>(); auto* fake = services.get();
        auto labels = *NetworkLabels::decode(fake->files[L"network-labels.json"]);
        labels.rename("11111111", "Fixture distinct source remark"); fake->files[L"network-labels.json"] = labels.encode();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto memory = fake->files[L"network-memory.json"], names = fake->files[L"network-labels.json"];
        check(!app.remember_network() && fake->discoveries == 0 && app.memory_verification_message() == "memory.verification.nameConflict" &&
              fake->files[L"network-memory.json"] == memory && fake->files[L"network-labels.json"] == names,
              "conflicting manual remarks stop duplicate verification before discovery or any memory and label write");
        app.shutdown();
    }
    const auto make = [] {
        auto fake = std::make_unique<wifiruntime::Fake>();
        fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
        NetworkMemory memory("00112233445566778899aabbccddeeff");
        const auto now = AppController::wall_now();
        memory.remember({"wifi", {{"gw", "02:00:00:00:00:01"}, {"gwip", "192.0.2.1"}, {"net", "192.0.2.0/24"}}}, "teaching", true, false, now, "11111111");
        memory.remember({"wifi", {{"gw", "02:00:00:00:00:02"}, {"gwip", "198.51.100.1"}, {"net", "198.51.100.0/24"}}}, "dormitory", false, true, now, "22222222");
        fake->files[L"network-memory.json"] = memory.encode();
        NetworkLabels labels;
        labels.update_automatic("11111111", {"Fixture campus", "wifi", "Fixture campus", false});
        labels.update_automatic("22222222", {"Fixture campus", "wifi", "Fixture campus", false});
        fake->files[L"network-labels.json"] = labels.encode();
        return fake;
    };
    {
        auto services = std::make_unique<memorymergeruntime::Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(app.remember_network(), "duplicate memory verification is admitted after its Wi-Fi opt-outs are saved");
        wifiruntime::wait_for([&] { return fake->discoveries == 1 && !app.snapshot().busy; });
        app.on_changed();
        check(fake->opt_out_saved_before_discovery && app.remembered_networks().size() == 1 &&
              app.remembered_networks().front().id == "22222222" && !app.wifi_selected("22222222") &&
              app.network_name("22222222") == "Fixture survivor",
              "merging duplicates preserves a disabled member's opt-out and the surviving record's remark");
        const auto stored = fake->files;
        app.shutdown();
        auto restarted_services = std::make_unique<memorymergeruntime::Fake>(); restarted_services->files = stored;
        AppController restarted(nullptr, RuntimeMode::normal, std::move(restarted_services)); restarted.start();
        check(restarted.remembered_networks().size() == 1 && !restarted.wifi_selected("22222222"),
              "the merged network stays excluded from automatic Wi-Fi finding after restart");
        restarted.shutdown();
    }
    {
        auto services = std::make_unique<memorymergeruntime::Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        fake->fail_preferences = true;
        const auto original_memory = fake->files[L"network-memory.json"];
        const auto original_preferences = fake->files[L"wifi-recovery.json"];
        check(!app.remember_network() && fake->discoveries == 0 && !app.snapshot().busy &&
              !app.wifi_selected("11111111") && !app.wifi_selected("22222222") && app.remembered_networks().size() == 2 &&
              fake->files[L"network-memory.json"] == original_memory && fake->files[L"wifi-recovery.json"] == original_preferences,
              "failed opt-out persistence disables both candidates in memory and refuses merging before discovery");
        app.shutdown();
    }
    {
        auto services = std::make_unique<memorymergeruntime::Fake>(); auto* fake = services.get();
        auto preferences = *wifi::Preferences::decode(fake->files[L"wifi-recovery.json"]);
        for (unsigned i = 0; i < 63; ++i) {
            char id[9]; std::snprintf(id, sizeof id, "%08x", 0x70000000u + i); preferences.excluded.insert(id);
        }
        fake->files[L"wifi-recovery.json"] = *preferences.encode();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(!app.remember_network() && fake->discoveries == 0 && !app.wifi_preferences().auto_join &&
              !wifi::Preferences::decode(fake->files[L"wifi-recovery.json"])->auto_join && app.remembered_networks().size() == 2,
              "full opt-out storage disables finding and leaves existing memories intact rather than discarding an exclusion");
        app.shutdown();
    }
    for (const bool fail_write : {false, true}) {
        auto services = std::make_unique<memorymergeruntime::Fake>(); auto* fake = services.get();
        auto preferences = *wifi::Preferences::decode(fake->files[L"wifi-recovery.json"]);
        preferences.excluded.clear(); fake->files[L"wifi-recovery.json"] = *preferences.encode();
        fake->block_discovery = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto original_memory = fake->files[L"network-memory.json"];
        check(app.remember_network(), "all-enabled duplicate verification starts without preflight exclusions");
        wifiruntime::wait_for([&] { return fake->discoveries == 1; });
        fake->fail_preferences = fail_write;
        check(app.snapshot().verifying_memory && app.set_wifi_selected("11111111", false) == !fail_write,
              "a user opt-out during discovery takes effect even if its file cannot be saved");
        wifiruntime::wait_for([&] { return fake->returned_discoveries == 1 && !app.snapshot().busy; });
        app.on_changed();
        check(app.memory_verification_message() == "memory.verification.cancelled" &&
              app.remembered_networks().size() == 2 && !app.wifi_selected("11111111") && !app.wifi_selected("22222222") &&
              fake->files[L"network-memory.json"] == original_memory && fake->evidence().fake_auth_submissions == 0,
              "a late successful proof cannot merge away an opt-out made during verification");
        app.shutdown();
    }
    {
        auto services = std::make_unique<memorymergeruntime::Fake>(); auto* fake = services.get();
        auto preferences = *wifi::Preferences::decode(fake->files[L"wifi-recovery.json"]);
        preferences.excluded.clear(); fake->files[L"wifi-recovery.json"] = *preferences.encode();
        fake->block_discovery = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(app.remember_network(), "global finding switch fixture starts verification");
        wifiruntime::wait_for([&] { return fake->discoveries == 1; });
        check(app.set_wifi_auto_join(false), "finding can be disabled while campus verification is pending");
        wifiruntime::wait_for([&] { return fake->returned_discoveries == 1 && !app.snapshot().busy; });
        app.on_changed();
        check(!app.wifi_preferences().auto_join && app.wifi_preferences().excluded.empty() && app.remembered_networks().size() == 2,
              "disabling all finding cancels verification without permanently excluding individual networks");
        app.shutdown();
    }
    {
        auto services = std::make_unique<memorymergeruntime::Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(app.remember_network(), "completed merge fixture starts verification");
        wifiruntime::wait_for([&] {
            app.on_changed();
            return fake->returned_discoveries == 1 && !app.snapshot().busy &&
                app.memory_verification_message() == "memory.verification.remembered";
        });
        const auto stored = fake->files[L"wifi-recovery.json"];
        check(!app.set_wifi_selected("11111111", false) && fake->files[L"wifi-recovery.json"] == stored,
              "a stale reference to a removed duplicate is rejected instead of reporting a saved opt-out");
        app.shutdown();
    }
    {
        auto services = std::make_unique<memorymergeruntime::Fake>(); auto* fake = services.get();
        auto preferences = *wifi::Preferences::decode(fake->files[L"wifi-recovery.json"]);
        preferences.excluded.clear(); fake->files[L"wifi-recovery.json"] = *preferences.encode();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(app.remember_network(), "unprocessed merge result fixture starts verification");
        wifiruntime::wait_for([&] { return fake->returned_discoveries == 1 && !app.snapshot().busy; });
        check(app.remembered_networks().size() == 1 && app.memory_verification_message() == "memory.verification.checking" &&
              app.set_wifi_selected("11111111", false) && !app.wifi_selected("22222222"),
              "closing an old visible duplicate before result delivery saves the opt-out for its actual survivor");
        const auto stored = *wifi::Preferences::decode(fake->files[L"wifi-recovery.json"]);
        check(stored.excluded.count("11111111") && stored.excluded.count("22222222"),
              "the commit-to-feedback window persists exclusions for every possible survivor");
        app.on_changed();
        check(!app.set_wifi_selected("11111111", false), "after feedback is resolved the removed id is no longer actionable");
        app.shutdown();
    }
    {
        auto services = std::make_unique<memorymergeruntime::Fake>(); auto* fake = services.get();
        auto memory = *NetworkMemory::decode(fake->files[L"network-memory.json"], AppController::wall_now());
        memory.remember({"wifi", {{"gw", "02:00:00:00:00:02"}, {"gwip", "198.51.100.1"}, {"net", "198.51.100.0/24"}}},
                        "teaching", true, false, AppController::wall_now(), "33333333");
        fake->files[L"network-memory.json"] = memory.encode();
        auto labels = *NetworkLabels::decode(fake->files[L"network-labels.json"]);
        labels.update_automatic("33333333", {"Fixture other", "wifi", "Fixture other", false});
        fake->files[L"network-labels.json"] = labels.encode();
        fake->block_discovery = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(app.remember_network(), "unrelated deletion fixture starts verification");
        wifiruntime::wait_for([&] { return fake->discoveries == 1; });
        app.forget_network("33333333");
        check(app.snapshot().verifying_memory && app.memory_verification_message() == "memory.verification.checking",
              "deleting an unrelated Wi-Fi preference does not cancel current campus verification");
        fake->release_discovery = true;
        wifiruntime::wait_for([&] {
            app.on_changed();
            return fake->returned_discoveries == 1 && !app.snapshot().busy &&
                app.memory_verification_message() == "memory.verification.remembered";
        });
        check(app.remembered_networks().size() == 1 && app.memory_verification_message() == "memory.verification.remembered",
              "current duplicate verification still completes after an unrelated network is forgotten");
        app.shutdown();
    }
    {
        auto services = make(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(app.remembered_networks().size() == 2 && app.network_display_name("11111111") == "Fixture campus" &&
              app.network_display_name("22222222") == "Fixture campus",
              "distinct same-name campus memories retain the original visible name without generated Wi-Fi suffixes");
        check(!app.can_edit_wifi_name("11111111"), "known automatic Wi-Fi name does not need a second name menu");
        const auto original_ssid = app.wifi_name("11111111");
        check(app.rename_network("11111111", "Fixture lecture halls") && app.wifi_name("11111111") == original_ssid &&
              app.wifi_preferences().manual_names.empty(), "renaming a display remark never changes automatic Wi-Fi target");
        check(app.set_wifi_name("11111111", "Fixture alternate") && app.can_edit_wifi_name("11111111") &&
              app.network_name("11111111") == "Fixture lecture halls", "explicit Wi-Fi override stays editable and does not rename its remark");
        app.forget_network("11111111");
        check(app.remembered_networks().size() == 1 && app.remembered_networks().front().id == "22222222" &&
              !app.network_name_known("11111111") && app.network_name_known("22222222") &&
              !app.wifi_preferences().manual_names.count("11111111") && app.wifi_preferences().excluded.count("11111111"),
              "forgetting selected network removes its label and Wi-Fi target but retains unrelated memory");
        const auto after = fake->files;
        app.forget_network("11111111");
        check(fake->files == after, "repeated stale forget action does not write or delete another network");
        app.shutdown();
        check(fake->evidence().fake_auth_submissions == 0 && fake->evidence().vault_accesses == 0,
              "memory management tests never use formal credentials or authenticate");
    }
    {
        auto services = make(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        fake->fail_write = true;
        const auto stored = fake->files[L"network-memory.json"];
        app.forget_network("11111111");
        check(app.remembered_networks().size() == 1 && !app.network_name_known("11111111") &&
              app.wifi_preferences().excluded.count("11111111") && app.notice() == "memory.writeFailed" &&
              fake->files[L"network-memory.json"] == stored, "failed deletion storage disables selected network immediately and reports failure");
        app.shutdown();
    }
    {
        auto services = make(); auto* fake = services.get();
        const auto original = fake->files;
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services)); app.start();
        const auto original_count = app.remembered_networks().size();
        app.forget_network("11111111"); app.forget_network(); app.forget_all_networks();
        check(!app.rename_network("11111111", "Readonly fixture") && !app.set_wifi_name("11111111", "Readonly fixture") &&
              app.remembered_networks().size() == original_count && fake->files == original,
              "observe-only rejects all remembered network mutations before persistence");
        app.shutdown();
    }
    {
        auto services = std::make_unique<memorymergeruntime::NameDisplayFake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto files = fake->files;
        const auto records = app.remembered_networks();
        const auto before = app.snapshot();
        check(app.current_network_record_id() == std::optional<std::string>("11111111") &&
              app.authentication_target_name() == "Fixture campus" &&
              app.network_display_name("11111111") == "Fixture campus" && app.network_display_name("22222222") == "Fixture campus",
              "a unique reliable physical binding labels only its actual memory as current without renaming either same-SSID record");
        check(fake->files == files && records.size() == 2 && app.snapshot().trusted == before.trusted &&
              app.snapshot().epoch == before.epoch && !app.wifi_selected("22222222") &&
              fake->evidence().fake_auth_submissions == 0,
              "reading current-memory and same-name display state preserves separate features, proofs, exclusions and authentication authority");
        const auto kept = records.front();
        app.forget_network("22222222");
        const auto remaining = app.remembered_networks();
        check(remaining.size() == 1 && remaining.front().id == "11111111" && remaining.front().features == kept.features &&
              remaining.front().verified == kept.verified && remaining.front().proof_teaching == kept.proof_teaching &&
              remaining.front().proof_dormitory == kept.proof_dormitory && app.network_display_name("11111111") == "Fixture campus" &&
              app.authentication_target_name() == "Fixture campus" && app.current_network_record_id() == std::optional<std::string>("11111111"),
              "forgetting the other same-name physical connection neither renumbers the survivor nor transfers or renews its proofs");
        app.shutdown();
    }
    for (const int state : {0, 1, 2}) {
        auto services = std::make_unique<memorymergeruntime::NameDisplayFake>(); auto* fake = services.get();
        fake->second = state == 0;
        fake->missing_binding = state == 1;
        fake->ambiguous_binding = state == 2;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto files = fake->files;
        check(state == 0 ? app.current_network_record_id() == std::optional<std::string>("22222222") : !app.current_network_record_id(),
              "current connection follows the selected physical memory and is absent for missing or ambiguous bindings");
        check(app.network_display_name("11111111") == "Fixture campus" && app.network_display_name("22222222") == "Fixture campus" &&
              fake->files == files && !app.wifi_selected("22222222") && fake->evidence().fake_auth_submissions == 0,
              "physical-selection presentation never rewrites same-name labels, enables an excluded target or submits credentials");
        app.shutdown();
    }
    {
        auto services = std::make_unique<memorymergeruntime::NameDisplayFake>(); auto* fake = services.get();
        auto memory = *json::parse(fake->files[L"network-memory.json"]);
        auto* records = memory.find("records");
        auto duplicate = records->items().front(); duplicate.set("id", json::Value::string("33333333")); records->push(duplicate);
        fake->files[L"network-memory.json"] = json::serialize(memory);
        NetworkLabels labels = *NetworkLabels::decode(fake->files[L"network-labels.json"]);
        labels.update_automatic("33333333", {"Fixture campus", "wifi", "Fixture campus", false});
        fake->files[L"network-labels.json"] = labels.encode();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto files = fake->files;
        check(app.snapshot().remembered && app.snapshot().authentication_route && !app.current_network_record_id() &&
              app.remembered_networks().size() == 3 && fake->files == files,
              "one physical binding matching multiple legacy memories does not arbitrarily mark one record as current or merge their proofs");
        app.shutdown();
    }
    {
        auto services = std::make_unique<memorymergeruntime::NameDisplayFake>(); auto* fake = services.get();
        const auto files = fake->files;
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services)); app.start();
        check(!app.current_network_record_id() && app.remembered_networks().empty() && !app.snapshot().remembered &&
              !app.snapshot().trusted && fake->files == files,
              "read-only presentation cannot mark a formal saved network as current or load its recovery authority");
        app.on_network_signal(); app.on_timer(kTimerNetwork); app.on_changed();
        check(!app.current_network_record_id() && app.remembered_networks().empty() && fake->files == files &&
              fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0 &&
              fake->evidence().fake_probe_calls == 0 && fake->evidence().vault_accesses == 0 &&
              fake->evidence().network_calls == 0,
              "read-only physical refresh leaves saved names, proofs, exclusions and credentials untouched without recovery work");
        app.shutdown();
    }
    for (const int interruption : {0, 1, 2}) {
        auto services = std::make_unique<memorymergeruntime::NameDisplayFake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto files = fake->files;
        check(app.current_network_record_id() == std::optional<std::string>("11111111"),
              "current-memory interruption fixture begins with one reliably selected saved attachment");
        if (interruption == 0) app.on_network_signal();
        else if (interruption == 1) app.on_suspend();
        else {
            fake->disconnected = true;
            app.on_network_signal(); app.on_timer(kTimerNetwork); app.on_changed();
        }
        check(!app.current_network_record_id() && app.remembered_networks().size() == 2 && fake->files == files &&
              !app.wifi_selected("22222222") && fake->evidence().fake_auth_submissions == 0 &&
              fake->evidence().fake_wifi_calls == 0 && fake->evidence().fake_probe_calls == 0,
              "pending physical refresh, sleep or disconnection removes the current marker without discarding memories or enabling excluded Wi-Fi");
        if (interruption == 1) { app.on_resume(); app.on_timer(kTimerWake); }
        else {
            fake->disconnected = false;
            app.on_timer(kTimerNetwork);
        }
        app.on_changed();
        check(app.current_network_record_id() == std::optional<std::string>("11111111") &&
              !app.snapshot().trusted && fake->files == files && !app.wifi_selected("22222222") &&
              fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0 &&
              fake->evidence().fake_probe_calls == 0,
              "reading a fresh matching attachment restores only its display marker without renewing proofs, session trust or recovery selection");
        app.shutdown();
    }
}
