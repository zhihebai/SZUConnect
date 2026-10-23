#pragma once
#include "campus_verification_runtime_tests.hpp"
#include "shared_copy.hpp"

namespace diagnosticcontextruntime {
class Fake final : public campusverificationruntime::Fake {
public:
    Fake() : campusverificationruntime::Fake("manual") {}
    szu::win::NetworkSnapshot capture_network() override {
        ++captures;
        auto result = campusverificationruntime::Fake::capture_network();
        result.access = changed ? "multiple" : "ethernet";
        result.tunnel_known = true;
        result.tunnel.tunnel = result.path.tunnel = !changed;
        result.adapter_names = {"Fixture private adapter"};
        result.path.interfaces = "fixture-private-interface-guid";
        return result;
    }
    szu::proxy::WindowsRaw read_proxy() override {
        ++proxy_reads;
        szu::proxy::WindowsRaw result;
        result.readable = true;
        if (!changed) result.proxy = "http=127.0.0.1:19876";
        return result;
    }
    int captures = 0, proxy_reads = 0;
};
}

template<class Check> void run_diagnostic_context_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    for (const auto mode : {RuntimeMode::normal, RuntimeMode::observeOnly}) {
        auto services = std::make_unique<diagnosticcontextruntime::Fake>(); auto* fake = services.get();
        json::Value journal = json::Value::object();
        journal.set("schema", json::Value::number(1)); journal.set("generation", json::Value::number(1));
        journal.set("notBefore", json::Value::number(AppController::wall_now() + 60));
        auto holds = json::Value::array(), hold = json::Value::object();
        hold.set("reason", json::Value::number(static_cast<int>(HoldReason::userPaused)));
        hold.set("origin", json::Value::number(static_cast<int>(HoldOrigin::user)));
        holds.push(std::move(hold)); journal.set("holds", std::move(holds));
        fake->files[L"recovery.json"] = json::serialize(journal);
        AppController app(nullptr, mode, std::move(services)); app.start();
        const auto files = fake->files; const auto before = app.snapshot(); const auto due = app.next_check();
        const int captures = fake->captures, proxy_reads = fake->proxy_reads;
        const auto output = app.diagnostic_export_text(), copied = app.diagnostic_summary_text();
        const auto report = json::parse(output);
        const auto* environment = report ? report->find("environment") : nullptr;
        const auto* recovery = report ? report->find("recovery") : nullptr;
        check(report && report->find("app")->string_at("build") == kBuild &&
            copied.find(std::string(" (") + kBuild + ")") == std::string::npos && copied.find("Build ") == std::string::npos,
            "JSON retains internal build metadata while public copied summary shows only the product version");
        check(environment && environment->string_at("connection") == "ethernet" && environment->string_at("systemProxy") == "on" &&
            environment->string_at("tun") == "detected" && environment->bool_at("refreshing") == false &&
            copied.find(copy::format("records.diagnostics.environment", {{"access", std::string(copy::text("env.access.ethernet"))},
                {"proxy", std::string(copy::text("env.proxy.on"))}, {"tunnel", std::string(copy::text("env.tunnel.detected"))}})) != std::string::npos,
            "collected physical connection, proxy and TUN context reach Controller JSON and copied text as fixed enums");
        check(recovery && recovery->bool_at("readOnly") == (mode == RuntimeMode::observeOnly) &&
            recovery->bool_at("paused") == before.paused && recovery->bool_at("networkAvailable") == before.network_available &&
            recovery->bool_at("networkRemembered") == before.remembered && recovery->bool_at("sessionTrusted") == before.trusted &&
            recovery->find("holdReasons")->size() == 1 && recovery->find("holdReasons")->items()[0].as_string() == "userPaused" &&
            recovery->number_at("cooldownRemainingSeconds").value_or(0) > 0 &&
            recovery->number_at("cooldownRemainingSeconds").value_or(999) <= 60 &&
            copied.find(std::string(copy::text("records.diagnostics.protection.userPaused"))) != std::string::npos,
            "current recovery protections, readonly mode and cooldown are copied from the same engine snapshot");
        bool private_absent = true;
        for (const auto* marker : {"Fixture private adapter", "Fixture Ethernet", "Fixture laboratory cable", "fixture-private-interface-guid",
                "192.0.2.", "127.0.0.1", "19876", "fixture-account", "fixture-password", "source_ipv4", "interface_luid",
                "gateway_ipv4", "dns_servers", "descriptor", "https://", "C:\\Users\\"})
            private_absent = private_absent && output.find(marker) == std::string::npos && copied.find(marker) == std::string::npos;
        check(private_absent && fake->captures == captures && fake->proxy_reads == proxy_reads && fake->files == files &&
            app.snapshot().epoch == before.epoch && app.snapshot().holds.size() == before.holds.size() &&
            app.snapshot().trusted == before.trusted && app.next_check() == due &&
            fake->probes == 0 && fake->system_probes == 0 && fake->discoveries == 0 && fake->evidence().fake_auth_submissions == 0,
            "diagnostic snapshot adds no reads, network requests, credentials, raw network fields, grants or scheduling changes");
        app.on_network_signal();
        const auto pending = json::parse(app.diagnostic_export_text());
        check(pending && pending->find("environment")->bool_at("refreshing") == true &&
            pending->find("environment")->string_at("connection") == "unknown" &&
            pending->find("environment")->string_at("systemProxy") == "unknown" && pending->find("environment")->string_at("tun") == "unknown" &&
            pending->find("recovery")->bool_at("networkAvailable") == false &&
            pending->find("recovery")->bool_at("networkRemembered") == false && pending->find("recovery")->bool_at("sessionTrusted") == false &&
            pending->find("campusVerification")->string_at("state") == "notChecked" && !pending->find("physicalNetwork") &&
            app.diagnostic_summary_text().find("physical interfaces=") == std::string::npos &&
            app.diagnostic_summary_text().find(copy::text("env.identification.ready")) == std::string::npos,
            "pending network change cannot label old environment, network eligibility or campus verification as current");
        fake->changed = true; app.refresh_environment();
        const auto replaced = json::parse(app.diagnostic_export_text());
        check(replaced && replaced->find("environment")->bool_at("refreshing") == false &&
            replaced->find("environment")->string_at("connection") == "multiple" &&
            replaced->find("environment")->string_at("systemProxy") == "off" && replaced->find("environment")->string_at("tun") == "notDetected" &&
            replaced->find("recovery")->bool_at("networkRemembered") == false,
            "replacement snapshot exports the new anonymous connection without borrowing the previous network memory");
        app.on_suspend();
        const auto sleeping = json::parse(app.diagnostic_export_text());
        check(sleeping && sleeping->find("environment")->bool_at("refreshing") == true &&
            sleeping->find("environment")->string_at("connection") == "unknown" && sleeping->find("recovery")->bool_at("networkAvailable") == false &&
            !sleeping->find("physicalNetwork") && app.diagnostic_summary_text().find("physical interfaces=") == std::string::npos &&
            app.diagnostic_summary_text().find(copy::text("env.identification.ready")) == std::string::npos,
            "sleep invalidates current environmental claims while preserving actual protection diagnostics");
        const int captures_before_wake = fake->captures;
        app.on_resume();
        const auto resuming = json::parse(app.diagnostic_export_text());
        check(resuming && resuming->find("environment")->bool_at("refreshing") == true &&
            resuming->find("environment")->string_at("connection") == "unknown" && !resuming->find("physicalNetwork") &&
            resuming->find("campusVerification")->string_at("state") == "notChecked" &&
            fake->captures == captures_before_wake && app.diagnostic_summary_text().find(copy::text("env.identification.ready")) == std::string::npos,
            "resume before the wake capture cannot revive a pre-sleep environment or physical identification");
        fake->changed = false;
        app.on_timer(kTimerWake);
        const auto awake = json::parse(app.diagnostic_export_text());
        check(awake && awake->find("environment")->bool_at("refreshing") == false &&
            awake->find("environment")->string_at("connection") == "ethernet" &&
            awake->find("environment")->string_at("systemProxy") == "on" && awake->find("physicalNetwork") &&
            fake->captures == captures_before_wake + 1 && fake->probes == 0 && fake->discoveries == 0,
            "the actual wake capture restores current diagnostics without changing authentication or starting new requests");
        app.shutdown();
        const auto stopped = json::parse(app.diagnostic_export_text());
        check(stopped && stopped->find("environment")->bool_at("refreshing") == true && !stopped->find("physicalNetwork") &&
            app.diagnostic_summary_text().find("physical interfaces=") == std::string::npos &&
            app.diagnostic_summary_text().find(copy::text("env.identification.ready")) == std::string::npos,
            "shutdown excludes the previous physical identification and ready summary");
    }
    for (const bool replace : {false, true}) {
        auto services = std::make_unique<diagnosticcontextruntime::Fake>(); auto* fake = services.get();
        fake->block = true; fake->confirmed = false;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        check(app.verify_campus_service(), "diagnostic late-result fixture starts only an independent campus check");
        wifiruntime::wait_for([&] { return fake->discoveries == 1; });
        if (replace) { fake->changed = true; app.refresh_environment(); }
        else app.cancel();
        fake->released = true;
        wifiruntime::wait_for([&] { app.on_changed(); return fake->returned == 1 && !app.snapshot().busy; });
        const auto report = json::parse(app.diagnostic_export_text());
        check(report && report->find("campusVerification")->string_at("state") == "notChecked" &&
            report->find("campusVerification")->string_at("issue") == "none" &&
            report->find("environment")->string_at("connection") == (replace ? "multiple" : "ethernet") &&
            report->find("environment")->string_at("tun") == (replace ? "notDetected" : "detected") &&
            fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0,
            "late campus results after cancellation or replacement cannot alter the exported current environment or create authentication");
        app.shutdown();
    }
}
