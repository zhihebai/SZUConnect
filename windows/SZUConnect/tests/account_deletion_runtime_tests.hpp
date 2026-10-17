#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace accountdeletionruntime {
class Fake final : public wifiruntime::Fake {
public:
    Fake() {
        files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"recover","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
    }
    szu::win::CredentialVault::ReadStatus read_credentials(szu::Credentials& value) override {
        if (damaged) return szu::win::CredentialVault::ReadStatus::failed;
        if (!saved) return szu::win::CredentialVault::ReadStatus::missing;
        value = *saved; return szu::win::CredentialVault::ReadStatus::found;
    }
    bool write_credentials(const szu::Credentials& value) override { saved = value; damaged = false; return true; }
    bool erase_credentials() override {
        ++erasures;
        if (fail_erase) return false;
        saved.reset(); damaged = false; return true;
    }
    szu::win::NetworkSnapshot capture_network() override {
        szu::win::NetworkSnapshot result;
        result.path.usable = true; result.path.link_active = true;
        result.path.identity = "fixture-account-deletion";
        result.access = "ethernet"; result.tunnel_known = true;
        return result;
    }
    szu::ConnectivityReport probe(szu::ProbeMode mode, const szu::CancelToken& token) override {
        ++probes;
        while (block_probe && !token.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (token.cancelled()) ++cancelled;
        auto report = szu::ConnectivityReport::simple(false, false); report.mode = mode;
        return report;
    }
    szu::CampusDiscovery discover(szu::CampusArea, const szu::CancelToken&) override {
        ++discoveries; return {szu::Discovery::confirmed, szu::Discovery::notChecked};
    }
    szu::LoginAttempt attempt(const szu::Credentials&, szu::CampusArea, const szu::CancelToken& token) override {
        ++attempts;
        while (block_attempt && !token.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (token.cancelled()) ++cancelled;
        szu::LoginAttempt result; result.outcome = szu::Outcome::success; return result;
    }
    std::optional<szu::Credentials> saved{szu::Credentials{"fixture-delete-account", "fixture-delete-password"}};
    bool fail_erase = false, damaged = false;
    std::atomic<bool> block_probe{false}, block_attempt{false};
    std::atomic<int> erasures{0}, probes{0}, discoveries{0}, attempts{0}, cancelled{0};
};
}

template<class Check> void run_account_deletion_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    {
        auto services = std::make_unique<accountdeletionruntime::Fake>(); auto* fake = services.get();
        fake->damaged = true; fake->fail_erase = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(!app.snapshot().has_saved && app.can_delete_account(),
              "unreadable app credentials keep the fixed-target delete action available");
        app.clear_notice();
        check(app.can_delete_account() && !app.delete_account() && app.can_delete_account() && fake->erasures == 1,
              "dismissed notices and failed removal do not hide unreadable credentials from retry");
        fake->fail_erase = false;
        check(app.delete_account() && !app.can_delete_account() && !fake->saved && !fake->damaged && fake->erasures == 2,
              "unreadable credentials can be removed without reading or enumerating their stored contents");
        app.shutdown();
    }
    {
        auto services = std::make_unique<accountdeletionruntime::Fake>(); auto* fake = services.get();
        fake->damaged = true;
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services)); app.start();
        check(!app.can_delete_account() && !app.delete_account() && fake->erasures == 0 && fake->damaged,
              "unreadable credentials do not bypass read-only deletion protection");
        app.shutdown();
    }
    {
        auto services = std::make_unique<accountdeletionruntime::Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const auto memory = fake->files[L"network-memory.json"];
        check(app.snapshot().has_saved && app.delete_account() && fake->erasures == 1 && !fake->saved &&
              !app.snapshot().has_saved && !app.snapshot().draft_complete && !app.snapshot().unsaved_changes &&
              app.draft().account.empty() && app.draft().password.empty(),
              "account deletion clears the fake vault, saved account and editor draft");
        check(fake->files[L"network-memory.json"] == memory && !app.remembered_networks().empty(),
              "account deletion preserves remembered networks and unrelated application settings");
        app.on_timer(kTimerAuto);
        wifiruntime::wait_for([&] { return fake->probes > 0 && !app.snapshot().busy; });
        check(app.snapshot().auto_login == AutoLoginState::needsAccount && fake->attempts == 0 &&
              fake->evidence().fake_wifi_calls == 0 && fake->evidence().vault_accesses == 0,
              "automatic monitoring after deletion cannot reuse the removed account or start Wi-Fi recovery");
        app.shutdown();
    }
    {
        auto services = std::make_unique<accountdeletionruntime::Fake>(); auto* fake = services.get();
        fake->fail_erase = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(!app.delete_account() && fake->erasures == 1 && fake->saved && app.snapshot().has_saved &&
              app.snapshot().paused && !app.snapshot().draft_complete && app.notice() == "account.deleteFailed",
              "vault deletion failure remains visible and retryable while automatic login is paused");
        app.on_timer(kTimerAuto);
        wifiruntime::wait_for([&] { return fake->probes > 0 && !app.snapshot().busy; });
        check(fake->attempts == 0 && fake->evidence().fake_wifi_calls == 0,
              "failed deletion cannot continue automatic login or Wi-Fi finding with the retained vault account");
        const auto saved_files = fake->files;
        app.shutdown();
        auto restarted_services = std::make_unique<accountdeletionruntime::Fake>(); auto* restarted = restarted_services.get();
        restarted->files = saved_files;
        AppController retry(nullptr, RuntimeMode::normal, std::move(restarted_services)); retry.start();
        check(retry.snapshot().paused && retry.snapshot().has_saved,
              "a failed account deletion keeps automatic login paused across restart");
        check(retry.delete_account() && restarted->erasures == 1 && !retry.snapshot().has_saved,
              "the retained account can be deleted successfully on retry");
        retry.shutdown();
    }
    {
        auto services = std::make_unique<accountdeletionruntime::Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services)); app.start();
        const auto saved_files = fake->files;
        check(!app.delete_account() && fake->erasures == 0 && fake->saved && app.snapshot().has_saved &&
              app.snapshot().draft_complete && saved_files == fake->files,
              "read-only deletion is rejected before any vault or local data change");
        app.shutdown();
    }
    {
        auto services = std::make_unique<accountdeletionruntime::Fake>(); auto* fake = services.get();
        fake->block_probe = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.connect();
        wifiruntime::wait_for([&] { return fake->probes == 1; });
        check(fake->probes == 1 && app.snapshot().busy && app.delete_account(),
              "account deletion can cancel a foreground connection before credentials are submitted");
        wifiruntime::wait_for([&] { return fake->cancelled == 1; });
        check(fake->cancelled == 1 && fake->attempts == 0 && !app.snapshot().has_saved && !app.snapshot().busy,
              "a late probe from the cancelled connection cannot revive the deleted account or submit credentials");
        app.shutdown();
    }
    {
        auto services = std::make_unique<accountdeletionruntime::Fake>(); auto* fake = services.get();
        fake->block_attempt = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start(); app.connect();
        wifiruntime::wait_for([&] { return fake->attempts == 1; });
        const bool submitted = fake->attempts == 1;
        const bool removed = app.delete_account();
        wifiruntime::wait_for([&] { return fake->cancelled == 1; });
        const auto state = app.snapshot();
        check(submitted && removed && fake->cancelled == 1 && !state.has_saved && state.cooldown_remaining &&
              std::any_of(state.holds.begin(), state.holds.end(), [](const Hold& hold) { return hold.reason == HoldReason::uncertain; }),
              "deleting after a fake submission preserves uncertain-result and cooldown protection despite a late success");
        check(fake->attempts == 1 && !state.online,
              "deletion never logs out the network, submits again or accepts the cancelled operation's late success");
        app.shutdown();
    }
}
