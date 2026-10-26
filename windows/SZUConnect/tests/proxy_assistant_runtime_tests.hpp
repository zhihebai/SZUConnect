#pragma once

#include "../generated/shared_copy.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace proxyassistantruntime {
class Fake final : public wifiruntime::Fake {
public:
    Fake() {
        physical_path = true;
        files[L"recovery.json"] = "{\"schema\":1,\"generation\":1,\"notBefore\":" +
            std::to_string(static_cast<long long>(szu::app::AppController::wall_now() + 90)) +
            ",\"holds\":[{\"reason\":2,\"origin\":2}]}";
    }
    szu::app::RuntimeMode mode() const override { return szu::app::RuntimeMode::normal; }
    szu::win::CredentialVault::ReadStatus read_credentials(szu::Credentials& out) override {
        out = credentials; return szu::win::CredentialVault::ReadStatus::found;
    }
    bool write_credentials(const szu::Credentials& value) override {
        ++credential_writes; credentials = value; return true;
    }
    bool write(const wchar_t* name, const std::string& value) override {
        ++application_writes; return wifiruntime::Fake::write(name, value);
    }
    szu::win::NetworkSnapshot capture_network() override {
        auto result = wifiruntime::Fake::capture_network();
        szu::NetworkDescriptor descriptor;
        descriptor.kind = "wifi";
        descriptor.raw = {{"gwip", "192.0.2.1"}, {"net", "192.0.2.0/24"}};
        result.path.descriptors = {descriptor};
        return result;
    }
    szu::ConnectivityReport probe(szu::ProbeMode mode, const szu::CancelToken&) override {
        ++probes;
        auto report = szu::ConnectivityReport::simple(true, true); report.mode = mode; return report;
    }
    szu::CampusDiscovery discover(szu::CampusArea, const szu::CancelToken&) override {
        ++discoveries; return {szu::Discovery::confirmed, szu::Discovery::confirmed};
    }
    szu::LoginAttempt attempt(const szu::Credentials&, szu::CampusArea, const szu::CancelToken&) override {
        ++submits; return {};
    }
    szu::compat::Input compatibility(bool proxy, const std::string& tunnel, const szu::CancelToken&) override {
        ++ordinary_checks;
        szu::compat::Input result;
        result.proxy = proxy ? "on" : "off"; result.tunnel = tunnel;
        result.campus_direct = "reachable"; return result;
    }
    szu::compat::Input proxy_config_check(bool proxy, const std::string& tunnel, const szu::CancelToken& token) override {
        actual_proxy = proxy; ++assistant_checks;
        while (block_check && !token.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (token.cancelled()) ++cancelled_checks;
        szu::compat::Input result;
        result.proxy = proxy ? "on" : "off"; result.tunnel = tunnel;
        result.campus_direct = result.campus_system = "reachable";  // Deliberately late success after cancellation.
        ++returned_checks;
        return result;
    }
    szu::proxyassist::Preview prepare_proxy_config(HWND, szu::proxyassist::Format format) override {
        ++prepares; last_format = format; auto result = next_preview;
        result.format = format; prepared = result.ready && result.error.empty(); return result;
    }
    szu::proxyassist::Result apply_proxy_config() override {
        ++applies;
        if (next_apply.success && next_apply.changed) { ++external_writes; backup = true; }
        return next_apply;
    }
    szu::proxyassist::Result restore_proxy_config() override {
        ++restores;
        if (next_restore.success && next_restore.changed) { ++external_writes; backup = false; }
        return next_restore;
    }
    szu::proxyassist::Result discard_proxy_config_backup() override {
        ++discards;
        if (next_discard.success) backup = false;
        return next_discard;
    }
    void clear_proxy_config_preview() override { ++clears; prepared = false; }
    bool proxy_config_has_backup() override { ++backup_reads; return backup; }

    szu::Credentials credentials{"fixture-proxy-account", "fixture-proxy-password"};
    szu::proxyassist::Preview next_preview{
        szu::proxyassist::Format::clashVergeScript, "PRIVATE_SENTINEL.js", {"rules", "dns"}, "", true, true};
    szu::proxyassist::Result next_apply{"", true, true}, next_restore{"", true, true}, next_discard{"", true, true};
    szu::proxyassist::Format last_format = szu::proxyassist::Format::clashVergeScript;
    int prepares = 0, applies = 0, restores = 0, discards = 0, clears = 0, backup_reads = 0;
    int external_writes = 0, application_writes = 0, credential_writes = 0;
    int probes = 0, discoveries = 0, submits = 0;
    std::atomic<int> ordinary_checks{0}, assistant_checks{0}, cancelled_checks{0}, returned_checks{0};
    std::atomic<bool> block_check{false}, actual_proxy{true};
    bool prepared = false, backup = false;
};

inline bool same_holds(const std::vector<szu::Hold>& a, const std::vector<szu::Hold>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].reason != b[i].reason || a[i].area != b[i].area || a[i].generation != b[i].generation ||
            a[i].epoch != b[i].epoch || a[i].origin != b[i].origin || a[i].rejection != b[i].rejection) return false;
    return true;
}

inline bool same_memories(const std::vector<szu::MemoryRecord>& a, const std::vector<szu::MemoryRecord>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].created != b[i].created || a[i].verified != b[i].verified ||
            a[i].area != b[i].area || a[i].proof_teaching != b[i].proof_teaching ||
            a[i].proof_dormitory != b[i].proof_dormitory || a[i].features != b[i].features) return false;
    return true;
}

inline std::string stable_export(const szu::app::AppController& app) {
    auto value = szu::json::parse(app.diagnostic_export_text());
    if (!value) return "invalid-export";
    value->set("exportedAt", szu::json::Value::string("ignored-clock"));
    if (auto* recovery = value->find("recovery")) {
        const auto seconds = recovery->number_at("cooldownRemainingSeconds");
        if (seconds && *seconds > 0 && *seconds <= 86400)
            recovery->set("cooldownRemainingSeconds", szu::json::Value::number(1));
    }
    return szu::json::serialize(*value);
}
inline std::string stable_summary(const szu::app::AppController& app) {
    auto text = app.diagnostic_summary_text();
    const std::string marker = "<remaining-seconds>";
    const auto pattern = szu::copy::format("records.diagnostics.cooldown", {{"seconds", marker}});
    const auto split = pattern.find(marker);
    if (split == std::string::npos) return text;
    const auto prefix = pattern.substr(0, split), suffix = pattern.substr(split + marker.size());
    const auto begin = text.find("\n" + prefix);
    if (begin == std::string::npos) return text;
    const auto number = begin + 1 + prefix.size();
    const auto newline = text.find('\n', number);
    const auto end = newline == std::string::npos ? text.size() : newline;
    if (end < number + suffix.size() || text.substr(end - suffix.size(), suffix.size()) != suffix) return text;
    const auto digits = text.substr(number, end - number - suffix.size());
    if (!digits.empty() && digits.size() <= 5 &&
        std::all_of(digits.begin(), digits.end(), [](char value) { return value >= '0' && value <= '9'; }))
        text.replace(number, digits.size(), marker);
    return text;
}
}  // namespace proxyassistantruntime

template<class Check> void run_proxy_assistant_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    using proxyassistantruntime::Fake;
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services));
        app.open_proxy_assistant(); app.prepare_proxy_config(nullptr);
        check(fake->prepares == 1 && fake->applies == 0 && fake->restores == 0 && fake->external_writes == 0 &&
              app.proxy_config_preview().ready && app.proxy_config_preview().changed && fake->prepared,
              "selecting and previewing a fake configuration never writes or restores a file");
        app.apply_proxy_config();
        check(fake->applies == 1 && fake->external_writes == 1 && app.proxy_config_has_backup() &&
              app.proxy_config_feedback() == "proxyAssistant.saved" && !app.proxy_config_preview().ready && !fake->prepared,
              "only explicit save applies a preview and then clears its ownership");
        app.apply_proxy_config();
        check(fake->applies == 1, "cleared preview cannot be saved twice");
        app.restore_proxy_config();
        check(fake->restores == 1 && fake->external_writes == 2 && !app.proxy_config_has_backup() &&
              app.proxy_config_feedback() == "proxyAssistant.restored",
              "explicit restoration updates the fixed feedback and backup state");
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services));
        app.prepare_proxy_config(nullptr);
        app.set_proxy_config_format(proxyassist::Format::mihomoYaml);
        check(!fake->prepared && !app.proxy_config_preview().ready && app.proxy_config_feedback().empty(),
              "switching format discards both controller and runtime previews");
        app.apply_proxy_config();
        check(fake->applies == 0, "format switch prevents applying a stale file");
        app.prepare_proxy_config(nullptr);
        check(fake->last_format == proxyassist::Format::mihomoYaml, "selected format reaches the actual runtime API");
        app.close_proxy_assistant(); app.apply_proxy_config();
        check(!fake->prepared && !app.proxy_config_preview().ready && fake->applies == 0,
              "closing the sheet invalidates the prepared file before another apply");
        fake->next_preview.changed = false;
        app.prepare_proxy_config(nullptr); app.apply_proxy_config();
        check(fake->applies == 0 && app.proxy_config_feedback() == "proxyAssistant.unchanged",
              "unchanged preview does not write or manufacture a new backup");
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services));
        fake->next_preview.error = "readFailed";
        app.prepare_proxy_config(nullptr); app.apply_proxy_config();
        check(fake->applies == 0 && !app.proxy_config_preview().ready &&
              app.proxy_config_feedback() == "proxyAssistant.error.readFailed",
              "prepare error rejects save even if a runtime mistakenly returns ready and changed");
        fake->next_preview.error = "PRIVATE_SENTINEL: unexpected parser path";
        app.prepare_proxy_config(nullptr); app.apply_proxy_config();
        check(fake->applies == 0 && app.proxy_config_feedback() == "proxyAssistant.error.unavailable" &&
              app.proxy_config_preview().filename.empty(),
              "unknown prepare errors become fixed copy without exposing their raw value or filename");
        fake->backup = true; fake->next_restore = {"backupCorrupt", false, false};
        app.open_proxy_assistant(); app.restore_proxy_config();
        check(fake->restores == 1 && fake->external_writes == 0 && app.proxy_config_has_backup() &&
              app.proxy_config_feedback() == "proxyAssistant.error.backupCorrupt",
              "unreadable backup retains the backup and reports its fixed recovery token");
        fake->next_preview.error.clear(); app.prepare_proxy_config(nullptr);
        fake->next_apply = {"PRIVATE_SENTINEL: write path", false, false};
        app.apply_proxy_config();
        check(fake->applies == 1 && fake->external_writes == 0 && !app.proxy_config_preview().ready &&
              app.proxy_config_feedback() == "proxyAssistant.error.unavailable",
              "failed save clears the preview and never renders raw file error text");
        const auto exported = app.diagnostic_export_text() + app.diagnostic_summary_text();
        check(exported.find("PRIVATE_SENTINEL") == std::string::npos && exported.find("proxyAssistant") == std::string::npos,
              "filenames and file operation errors never enter copied or JSON diagnostics");
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        fake->backup = true;
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services));
        app.open_proxy_assistant(); app.prepare_proxy_config(nullptr); app.apply_proxy_config(); app.restore_proxy_config();
        app.discard_proxy_config_backup();
        app.set_proxy_config_format(proxyassist::Format::singBoxJson); app.prepare_proxy_config(nullptr);
        app.close_proxy_assistant();
        check(fake->prepares == 0 && fake->applies == 0 && fake->restores == 0 && fake->discards == 0 && fake->backup_reads == 0 &&
              fake->external_writes == 0 && fake->backup && !app.proxy_config_has_backup(),
              "observe-only blocks selection, backup reading, saving and restoration before any fake I/O API");
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        fake->backup = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services));
        app.open_proxy_assistant(); app.prepare_proxy_config(nullptr);
        app.close_proxy_assistant(); app.open_proxy_assistant();
        check(fake->discards == 0 && fake->backup && app.proxy_config_has_backup(),
              "opening, previewing and cancelling never discard an existing backup without the explicit action");
        fake->next_discard = {"backupFailed", false, false};
        app.discard_proxy_config_backup();
        check(fake->discards == 1 && fake->backup && app.proxy_config_has_backup() && fake->external_writes == 0 &&
              app.proxy_config_feedback() == "proxyAssistant.error.backupFailed",
              "failed backup removal leaves the original backup and external configuration intact");
        app.restore_proxy_config();
        check(fake->restores == 1 && fake->external_writes == 1 && !app.proxy_config_has_backup() &&
              app.proxy_config_feedback() == "proxyAssistant.restored",
              "restoration remains available after an unsuccessful backup removal");
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        fake->backup = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services));
        app.open_proxy_assistant(); app.prepare_proxy_config(nullptr);
        fake->block_check = true;
        app.run_compat(true);
        wifiruntime::wait_for([&] { return fake->assistant_checks == 1; });
        app.discard_proxy_config_backup();
        wifiruntime::wait_for([&] { return fake->cancelled_checks == 1 && fake->returned_checks == 1; });
        check(fake->discards == 1 && fake->external_writes == 0 && !fake->backup && !app.proxy_config_has_backup() &&
              !app.proxy_config_preview().ready && !fake->prepared && !app.compat().done && !app.compat().running &&
              app.proxy_config_feedback() == "proxyAssistant.backupDiscarded",
              "confirmed backup removal preserves external content and invalidates preview and late diagnostics");
        app.discard_proxy_config_backup();
        check(fake->discards == 1, "backup removal is not repeated after its ownership has been cleared");
        app.prepare_proxy_config(nullptr);
        check(app.proxy_config_preview().ready && fake->prepares == 2,
              "a new file can be previewed after intentionally releasing the one-backup slot");
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services));
        app.run_compat();
        wifiruntime::wait_for([&] { return app.compat().done; });
        check(fake->ordinary_checks == 1 && fake->assistant_checks == 0 && app.compat().done &&
              !app.compat().current_path_included,
              "ordinary diagnostics retain their separate runtime API and do not claim a system-path comparison");
        app.run_compat(true);
        wifiruntime::wait_for([&] { return app.compat().done; });
        check(fake->assistant_checks == 1 && fake->ordinary_checks == 1 && !fake->actual_proxy &&
              app.compat().done && app.compat().current_path_included &&
              app.compat().input.proxy == "off" && app.compat().input.campus_system == "reachable",
              "assistant comparison uses its own runtime API even when the actual system proxy is off");

        for (int action = 0; action < 5; ++action) {
            app.prepare_proxy_config(nullptr);
            app.run_compat(true);
            wifiruntime::wait_for([&] { return app.compat().done; });
            if (action == 0) app.open_proxy_assistant();
            if (action == 1) app.set_proxy_config_format(proxyassist::Format::singBoxJson);
            if (action == 2) app.apply_proxy_config();
            if (action == 3) app.restore_proxy_config();
            if (action == 4) app.close_proxy_assistant();
            check(!app.compat().done && !app.compat().running && !app.compat().current_path_included,
                  "opening, changing format, saving, restoring or closing clears the previous comparison");
        }

        fake->block_check = true;
        for (int action = 0; action < 6; ++action) {
            app.prepare_proxy_config(nullptr);
            const int calls = fake->assistant_checks;
            const int cancellations = fake->cancelled_checks;
            app.run_compat(true);
            wifiruntime::wait_for([&] { return fake->assistant_checks > calls; });
            if (action == 0) app.open_proxy_assistant();
            if (action == 1) app.set_proxy_config_format(proxyassist::Format::mihomoYaml);
            if (action == 2) app.apply_proxy_config();
            if (action == 3) app.restore_proxy_config();
            if (action == 4) app.close_proxy_assistant();
            if (action == 5) app.on_network_signal();
            wifiruntime::wait_for([&] { return fake->cancelled_checks > cancellations && fake->returned_checks > calls; });
            check(fake->cancelled_checks > cancellations && !app.compat().done && !app.compat().running &&
                  !app.compat().current_path_included,
                  "assistant cancellation rejects deliberately late success after configuration or network change");
        }
        check(fake->ordinary_checks == 1 && fake->probes == 0 && fake->discoveries == 0 && fake->submits == 0 &&
              fake->evidence().network_calls == 0,
              "comparison dispatch and cancellation perform no real I/O or campus authentication");
        app.shutdown();
    }
    {
        auto services = std::make_unique<Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const bool trusted = app.trust(); app.pause();
        const auto before = app.snapshot(); const auto before_time = std::chrono::steady_clock::now();
        const auto draft = app.draft();
        const auto credentials = fake->credentials;
        const auto memories = app.remembered_networks();
        const auto files = fake->files;
        const auto due = app.next_check();
        const auto last = app.last_checked();
        const auto summary = proxyassistantruntime::stable_summary(app);
        const auto exported = proxyassistantruntime::stable_export(app);
        const auto writes = fake->application_writes;
        const auto credential_writes = fake->credential_writes;
        check(before.has_saved && !memories.empty() && trusted && before.trusted && !before.holds.empty() &&
              before.cooldown_remaining.value_or(0) > 0 && due.has_value(),
              "isolation fixture begins with saved credentials, trusted memory, active protection and a scheduled check");
        // Ensure countdown formatting changes while the protected deadline does not.
        std::this_thread::sleep_for(std::chrono::milliseconds(1050));
        app.open_proxy_assistant(); app.prepare_proxy_config(nullptr);
        app.run_compat(true);
        wifiruntime::wait_for([&] { return app.compat().done; });
        check(fake->assistant_checks == 1 && !fake->actual_proxy && app.compat().done,
              "credential-free campus comparison runs under existing holds without pretending a proxy is enabled");
        app.apply_proxy_config();
        const int external_writes = fake->external_writes;
        app.discard_proxy_config_backup();
        check(fake->discards == 1 && fake->external_writes == external_writes && !app.proxy_config_has_backup(),
              "removing an owned backup under active campus protections does not rewrite the proxy configuration");
        app.prepare_proxy_config(nullptr); app.apply_proxy_config();
        app.restore_proxy_config(); app.close_proxy_assistant();
        const auto after = app.snapshot();
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - before_time).count();
        check(after.has_saved == before.has_saved && after.saved_account_hint == before.saved_account_hint &&
              app.draft() == draft && fake->credentials == credentials && fake->credential_writes == credential_writes,
              "proxy file operations leave saved account and credential editor unchanged");
        check(after.trusted == before.trusted && after.paused == before.paused &&
              proxyassistantruntime::same_holds(before.holds, after.holds) &&
              after.generation == before.generation && after.epoch == before.epoch &&
              after.cooldown_remaining && before.cooldown_remaining &&
              *after.cooldown_remaining <= *before.cooldown_remaining + 0.01 &&
              *after.cooldown_remaining >= *before.cooldown_remaining - elapsed - 0.1,
              "proxy setup preserves network trust, holds, generation and the original cooldown deadline");
        check(proxyassistantruntime::same_memories(memories, app.remembered_networks()) && fake->files == files &&
              fake->application_writes == writes && app.next_check() == due && app.last_checked() == last,
              "proxy setup does not change network memory, application files or the existing monitoring schedule");
        check(fake->probes == 0 && fake->discoveries == 0 && fake->submits == 0 &&
              fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 &&
              proxyassistantruntime::stable_summary(app) == summary && proxyassistantruntime::stable_export(app) == exported,
              "save and restore neither request network/authentication nor alter diagnostic contents");
        app.shutdown();
    }
}
