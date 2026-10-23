#pragma once
#include "campus_verification_runtime_tests.hpp"
#include "shared_copy.hpp"

namespace campusdiagnosticsruntime {
class Fake final : public campusverificationruntime::Fake {
public:
    szu::CampusDiscovery discover(szu::CampusArea, const szu::CancelToken&) override {
        ++discoveries;
        while (block && !released) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++returned;
        return confirmed ? szu::CampusDiscovery{szu::Discovery::confirmed, szu::Discovery::confirmed}
            : szu::CampusDiscovery{szu::Discovery::unconfirmed, szu::Discovery::notChecked, false, issue};
    }
    szu::CampusVerificationIssue issue = szu::CampusVerificationIssue::dnsTimeout;
};
}

template<class Check> void run_campus_diagnostics_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    const std::pair<CampusVerificationIssue, const char*> issues[] = {
        {CampusVerificationIssue::dnsTimeout, "dnsTimeout"}, {CampusVerificationIssue::dns, "dns"},
        {CampusVerificationIssue::tls, "tls"}, {CampusVerificationIssue::connection, "connection"},
        {CampusVerificationIssue::dnsTls, "dnsTls"},
        {CampusVerificationIssue::http, "http"}, {CampusVerificationIssue::page, "page"},
        {CampusVerificationIssue::binding, "binding"}
    };
    for (const auto& [issue, token] : issues) {
        auto runtime = std::make_unique<campusdiagnosticsruntime::Fake>(); auto* fake = runtime.get();
        fake->confirmed = false; fake->issue = issue;
        AppController app(nullptr, RuntimeMode::normal, std::move(runtime)); app.start();
        const auto memory = fake->files[L"network-memory.json"], labels = fake->files[L"network-labels.json"];
        check(app.verify_campus_service(), "campus diagnostic fixture starts credential-free verification");
        wifiruntime::wait_for([&] { app.on_changed(); return fake->returned == 1 && !app.snapshot().busy; });
        app.on_changed();
        const auto text = app.diagnostic_export_text(), copied = app.diagnostic_summary_text();
        const auto parsed = json::parse(text); const auto* campus = parsed ? parsed->find("campusVerification") : nullptr;
        check(campus && campus->string_at("state") == "unconfirmed" && campus->string_at("issue") == token &&
            copied.find(std::string("campus verification=unconfirmed issue=") + token) != std::string::npos,
            "actual Controller verification exposes the same fixed failure reason in JSON and copied summary");
        const auto hint = std::string("campus.verify.") + token + "Hint";
        check(status::campus_verification_hint_key(app.snapshot()) == hint && copy::contains(hint),
            "each exported Controller failure also selects its implemented user-facing explanation");
        bool private_absent = true;
        for (const auto* marker : {"192.0.2.", "Fixture Ethernet", "Fixture laboratory cable",
                "fixture-campus-verification-ethernet", "fixture-account", "fixture-password", "source_ipv4", "interface_luid"})
            private_absent = private_absent && text.find(marker) == std::string::npos && copied.find(marker) == std::string::npos;
        check(private_absent && fake->files[L"network-memory.json"] == memory && fake->files[L"network-labels.json"] == labels &&
            fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0,
            "campus failure diagnostics contain no private network fields or credentials and preserve data and recovery isolation");
        fake->confirmed = true;
        check(app.verify_campus_service(), "failed verification remains available for a new credential-free attempt");
        wifiruntime::wait_for([&] { app.on_changed(); return fake->returned == 2 && !app.snapshot().busy; });
        const auto recovered = json::parse(app.diagnostic_export_text());
        check(recovered && recovered->find("campusVerification") &&
            recovered->find("campusVerification")->string_at("state") == "confirmed" &&
            recovered->find("campusVerification")->string_at("issue") == "none",
            "successful current verification removes the old exported failure reason");
        app.shutdown();
    }
    for (const bool replace : {false, true}) for (const auto issue : {CampusVerificationIssue::dnsTimeout, CampusVerificationIssue::dnsTls}) {
        auto runtime = std::make_unique<campusdiagnosticsruntime::Fake>(); auto* fake = runtime.get();
        fake->confirmed = false; fake->block = true; fake->issue = issue;
        AppController app(nullptr, RuntimeMode::normal, std::move(runtime)); app.start();
        check(app.verify_campus_service(), "late diagnostic fixture starts");
        wifiruntime::wait_for([&] { return fake->discoveries == 1; });
        const auto pending = json::parse(app.diagnostic_export_text());
        check(pending && pending->find("campusVerification")->string_at("state") == "checking" &&
            pending->find("campusVerification")->string_at("issue") == "none",
            "in-flight verification does not export a completed failure");
        if (replace) { fake->changed = true; app.refresh_environment(); }
        else app.cancel();
        fake->released = true;
        wifiruntime::wait_for([&] { app.on_changed(); return fake->returned == 1 && !app.snapshot().busy; });
        const auto after = json::parse(app.diagnostic_export_text());
        check(after && after->find("campusVerification")->string_at("issue") == "none" &&
            app.diagnostic_summary_text().find("issue=dnsTimeout") == std::string::npos &&
            app.diagnostic_summary_text().find("issue=dnsTls") == std::string::npos &&
            fake->evidence().fake_auth_submissions == 0,
            "late failures after cancel or replacement cannot enter copied or exported diagnostics");
        app.shutdown();
    }
    records::ExportContext context;
    context.campus_state = "unconfirmed";
    context.campus_issue = static_cast<CampusVerificationIssue>(999);
    const auto unknown = records::diagnostics_export(context, {}, {});
    check(unknown.find("campusVerification") && unknown.find("campusVerification")->string_at("issue") == "none",
        "unknown issue enum values are never serialized as arbitrary diagnostic content");
    context.campus_state = "fixture-private-state";
    check(!records::diagnostics_export(context, {}, {}).find("campusVerification") &&
        records::diagnostics_summary_text(context, {}).find("fixture-private-state") == std::string::npos,
        "unknown campus state text is excluded from both export formats");
}
