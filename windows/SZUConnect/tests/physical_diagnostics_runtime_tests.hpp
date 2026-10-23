#pragma once
#include <functional>
#include "campus_verification_runtime_tests.hpp"

namespace physicaldiagnosticsruntime {
class Fake final : public campusverificationruntime::Fake {
public:
    Fake() : campusverificationruntime::Fake("manual") {}
    szu::physical::CandidateReport collected() const {
        szu::physical::Attachment input;
        input.route = route(); input.physical = input.up = true;
        input.route.interface_key = "fixture-private-interface-guid";
        input.route.interface_luid = 123456789012345;
        input.route.display_name = "Fixture private adapter";
        input.route.descriptor.raw["dns"] = "192.0.2.53";
        if (missing_dns) input.route.dns_servers.clear();
        input.bound_default_gateways = {input.route.gateway_ipv4};
        return szu::physical::stable_candidate_report({input}, {input});
    }
    szu::win::NetworkSnapshot capture_network() override {
        ++captures;
        auto result = campusverificationruntime::Fake::capture_network();
        auto report = collected();
        result.path.physical_candidates = std::move(report.routes);
        result.path.descriptors.clear();
        for (const auto& candidate : result.path.physical_candidates) result.path.descriptors.push_back(candidate.descriptor);
        result.physical_diagnostics = std::move(report.diagnostics);
        result.adapter_names = {"Fixture private adapter"};
        result.current_label = szu::NetworkLabel{"Fixture private SSID", "wifi", "Fixture private SSID", true};
        return result;
    }
    szu::physical::CandidateDiagnostics capture_physical_diagnostics() override {
        ++diagnostic_captures;
        auto result = supplied ? *supplied : collected().diagnostics;
        if (during_capture) during_capture();
        return result;
    }
    bool missing_dns = true;
    std::optional<szu::physical::CandidateDiagnostics> supplied;
    std::function<void()> during_capture;
    int captures = 0, diagnostic_captures = 0;
};
}

template<class Check> void run_physical_diagnostics_runtime_tests(Check& check) {
    using namespace szu;
    using namespace szu::app;
    for (const auto mode : {RuntimeMode::normal, RuntimeMode::observeOnly}) {
        auto services = std::make_unique<physicaldiagnosticsruntime::Fake>(); auto* fake = services.get();
        AppController app(nullptr, mode, std::move(services)); app.start();
        const auto files = fake->files; const auto before = app.snapshot(); const auto due = app.next_check();
        const int credential_reads = fake->credential_reads;
        const auto initial = json::parse(app.diagnostic_export_text());
        const auto* physical = initial ? initial->find("physicalNetwork") : nullptr;
        check(physical && physical->number_at("physicalInterfaceCount") == 1 && physical->number_at("candidateCount") == 0 &&
            physical->find("rejections")->number_at("missingDNS") == 1 &&
            app.physical_identification_needed() && app.physical_identification_detail() == "env.identification.reason.missingDNS" &&
            app.diagnostic_summary_text().find("physical interfaces=1 candidates=0 reasons=missingDNS=1") != std::string::npos,
            "collected double-snapshot rejection reaches controller, visible explanation, copied text and exported JSON");
        physical::CandidateDiagnostics oversized;
        oversized.interface_count = 999999; oversized.candidate_count = 0;
        oversized.rejections[physical::CandidateRejection::inactiveLink] = 1;
        oversized.rejections[physical::CandidateRejection::missingDNS] = 999999;
        oversized.rejections[physical::CandidateRejection::invalidDNS] = 0;
        oversized.rejections[static_cast<physical::CandidateRejection>(999)] = 999999;
        fake->supplied = oversized;
        app.refresh_physical_identification();
        const auto bounded = json::parse(app.diagnostic_export_text());
        const auto* bounded_physical = bounded ? bounded->find("physicalNetwork") : nullptr;
        const auto copied = app.diagnostic_summary_text();
        check(bounded_physical && bounded_physical->number_at("physicalInterfaceCount") == 4096 &&
            bounded_physical->number_at("candidateCount") == 0 && bounded_physical->find("rejections")->size() == 2 &&
            bounded_physical->find("rejections")->number_at("missingDNS") == 4096 &&
            copied.find("physical interfaces=4096 candidates=0 reasons=inactiveLink=1,missingDNS=4096") != std::string::npos &&
            app.physical_identification_detail() == "env.identification.reason.missingDNS",
            "a diagnostic refresh clamps counts, drops unknown or zero reasons and updates every presentation from one safe snapshot");
        const auto exported = app.diagnostic_export_text();
        bool private_values_absent = true;
        for (const auto* marker : {"Fixture private adapter", "Fixture private SSID", "Fixture laboratory cable",
                "fixture-private-interface-guid", "192.0.2.", "123456789012345", "fixture-account", "fixture-password",
                "interface_key", "interface_luid", "source_ipv4", "descriptor", "dns_servers"})
            private_values_absent = private_values_absent && copied.find(marker) == std::string::npos && exported.find(marker) == std::string::npos;
        check(private_values_absent, "physical diagnostics export no addresses, SSID, adapter label, GUID, LUID, raw records or credentials");
        const auto after = app.snapshot();
        check(after.epoch == before.epoch && after.generation == before.generation && after.trusted == before.trusted &&
            after.remembered == before.remembered && after.memory_count == before.memory_count &&
            after.holds.size() == before.holds.size() && after.paused == before.paused &&
            app.next_check() == due && fake->files == files && fake->captures == 1 &&
            fake->credential_reads == credential_reads && fake->probes == 0 && fake->system_probes == 0 && fake->discoveries == 0 &&
            fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0,
            "collecting, presenting and exporting anonymous diagnostics grants no authority, writes nothing and starts no probe or recovery");
        app.shutdown();
        check(fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 && fake->evidence().production_storage_accesses == 0,
            "end-to-end physical diagnostic regression remains isolated in ordinary and observe-only modes");
    }
    for (const int interruption : {0, 1, 2, 3}) {
        auto services = std::make_unique<physicaldiagnosticsruntime::Fake>(); auto* fake = services.get();
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        physical::CandidateDiagnostics late; late.interface_count = 77; late.candidate_count = 77;
        fake->supplied = late;
        fake->during_capture = [&] {
            if (interruption == 0) app.cancel();
            else if (interruption == 1) app.on_network_signal();
            else if (interruption == 2) { fake->changed = true; app.refresh_environment(); }
            else app.shutdown();
        };
        app.refresh_physical_identification();
        const auto exported = json::parse(app.diagnostic_export_text());
        const auto* physical = exported ? exported->find("physicalNetwork") : nullptr;
        const bool current_capture = interruption == 0 || interruption == 2;
        check(exported && (current_capture ? physical && physical->number_at("physicalInterfaceCount") == 1 &&
                physical->number_at("candidateCount") == 0 : !physical) &&
            app.identification_feedback() != "env.identification.refreshed" &&
            app.physical_identification_detail() == "env.identification.reason.missingDNS",
            "a late diagnostic capture cannot replace current evidence; a pending network or shutdown omits the old physical export");
        check(fake->evidence().fake_auth_submissions == 0 && fake->evidence().network_calls == 0 &&
            fake->evidence().fake_wifi_calls == 0, "discarding a late diagnostic read does not perform a network recovery action");
        app.shutdown();
    }
}
