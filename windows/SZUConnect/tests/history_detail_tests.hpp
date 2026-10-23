#pragma once
#include "szu/presentation.hpp"
#include <cmath>
#include <limits>
#include <optional>
#include <string>

inline void history_detail_tests(szutest::Harness& t) {
    using namespace szu;
    t.begin("history-detail");
    const double now = 1790000000;
    records::SampleContext context;
    context.started_at = now - 4; context.operation = "manual"; context.monitoring_mode = "recover"; context.area = "automatic";
    context.environment = {"wifi", "on", "detected", false};
    auto report = ConnectivityReport::simple(false, false); report.mode = ProbeMode::complete;
    report.apple = {EndpointState::timeout, 1.234, std::nullopt};
    report.microsoft = {EndpointState::tls, 0.875, std::nullopt};
    report.complete_result = CompleteCheckResult::unavailable;
    const auto sample = records::sample_from(report, now, 3, 0, context);
    t.check(sample.apple_ms == 1234 && sample.microsoft_ms == 875 && sample.latency_ms == -1 &&
        sample.diagnosis == "security" && sample.complete_result == "unavailable" && sample.context &&
        sample.context->area == "automatic", "failed endpoint durations and original context survive without becoming successful latency");
    records::HealthLog health; health.append(sample);
    const auto encoded = json::serialize(health.to_json());
    const auto restored = records::HealthLog::from_json(encoded);
    t.check(restored && json::serialize(restored->to_json()) == encoded,
        "health reload preserves failure timing, diagnosis, completion time and bounded anonymous operation context");
    const auto legacy = records::HealthLog::from_json(R"({"schemaVersion":1,"samples":[{"date":"2026-09-21T14:13:20Z","online":false,"mode":"quick","apple":"dns","microsoft":"notCompleted"}]})");
    t.check(legacy && legacy->items().size() == 1 && !legacy->items()[0].context && !legacy->items()[0].apple_ms &&
        legacy->items()[0].diagnosis.empty(), "legacy health data remains readable without invented timings or environment");
    auto unsafe = sample;
    unsafe.mode = unsafe.apple = unsafe.microsoft = unsafe.diagnosis = unsafe.complete_result = "https://private.invalid/secret";
    unsafe.latency_ms = std::numeric_limits<double>::infinity();
    unsafe.apple_ms = std::numeric_limits<double>::quiet_NaN(); unsafe.microsoft_ms = 120001;
    unsafe.site_total = 13; unsafe.site_passed = 14;
    unsafe.context->environment = {"192.0.2.7", "private-account", "Private SSID", false};
    records::HealthLog safe; safe.append(unsafe);
    const auto sanitized = json::serialize(safe.to_json());
    t.check(sanitized.find("private") == std::string::npos && sanitized.find("192.0.2.7") == std::string::npos &&
        sanitized.find("Private SSID") == std::string::npos && sanitized.find("Milliseconds") == std::string::npos &&
        sanitized.find("websiteTotal") == std::string::npos && safe.items()[0].mode == "quick" &&
        safe.items()[0].apple == "notCompleted" && safe.items()[0].context->environment.connection == "unknown",
        "unknown strings, non-finite timings and out-of-range counts never enter saved or exported history");
    const auto unsafe_file = records::HealthLog::from_json(R"({"samples":[{"date":"2026-09-21T14:13:20Z","online":false,"mode":"private","apple":"https://private.invalid","microsoft":"dns","appleMilliseconds":123,"microsoftMilliseconds":120001,"latencyMilliseconds":1e300,"diagnosis":"private","completeResult":"private","context":{"startedAt":"2026-09-21T14:13:19Z","phase":"connectivityCheck","operation":"manual","monitoringMode":"manual","area":"automatic","environment":{"connection":"192.0.2.7","systemProxy":"private","tun":"private","refreshing":false}}}]})");
    const auto safe_file = unsafe_file ? json::serialize(unsafe_file->to_json()) : std::string();
    t.check(unsafe_file && unsafe_file->items().size() == 1 && !unsafe_file->items()[0].apple_ms &&
        !unsafe_file->items()[0].microsoft_ms && safe_file.find("private") == std::string::npos &&
        safe_file.find("192.0.2.7") == std::string::npos && safe_file.find("Milliseconds") == std::string::npos,
        "reading untrusted historical JSON sanitizes raw enums, addresses, durations and non-waited endpoint timing before re-export");
    auto invalid_context = sample;
    invalid_context.context->phase = "authentication-password-raw"; safe.append(invalid_context);
    t.check(!safe.items().back().context, "unsupported phases discard their entire context instead of inventing authentication steps");
    invalid_context.context = context; invalid_context.context->started_at = std::numeric_limits<double>::quiet_NaN();
    safe.append(invalid_context);
    t.check(!safe.items().back().context, "non-finite context dates are omitted before ISO conversion");
    const auto size = safe.items().size(); invalid_context.time = std::numeric_limits<double>::infinity(); safe.append(invalid_context);
    t.check(safe.items().size() == size, "non-finite completion dates do not create corrupt samples");
    auto boundary = sample; boundary.apple_ms = 0; boundary.microsoft_ms = 120000;
    records::HealthLog limits;
    for (int n = 0; n < 400; ++n) { boundary.time = now + n; limits.append(boundary); }
    const auto bounded = records::HealthLog::from_json(json::serialize(limits.to_json()));
    t.check(bounded && bounded->items().size() == 360 && bounded->items().back().apple_ms == 0 &&
        bounded->items().back().microsoft_ms == 120000, "extended history keeps the 360-sample limit and inclusive duration bounds on reload");
    boundary.time = now - records::HealthLog::kWindowSeconds; limits.append(boundary);
    t.check(limits.items().size() == 360, "an old extended sample cannot reintroduce data outside the 24-hour window");
    auto invalid_report = report; invalid_report.apple.elapsed = std::numeric_limits<double>::infinity();
    invalid_report.microsoft.elapsed = -1;
    const auto invalid_durations = records::sample_from(invalid_report, now);
    t.check(!invalid_durations.apple_ms && !invalid_durations.microsoft_ms, "invalid transport durations remain absent rather than becoming zero or overflowing");
    records::HealthLog recent;
    for (int n = 0; n < 5; ++n) { auto failure = sample; failure.time = now + n; recent.append(failure); }
    auto quick = ConnectivityReport::simple(true, false); quick.microsoft.state = EndpointState::notCompleted;
    recent.append(records::sample_from(quick, now + 5));
    auto cancelled = report; cancelled.complete_result = CompleteCheckResult::cancelled;
    recent.append(records::sample_from(cancelled, now + 6));
    records::ExportContext export_context{"1.0.0", "34", "windows", "manual", "dormitory", "quick", now + 7};
    const auto summary = records::diagnostics_summary_text(export_context, recent);
    const auto exported = records::diagnostics_export(export_context, recent, records::EventLog{});
    t.check(summary.find(records::iso8601(now + 4)) != std::string::npos &&
        summary.find(records::iso8601(now + 3)) != std::string::npos && summary.find(records::iso8601(now + 2)) != std::string::npos &&
        summary.find(records::iso8601(now + 1)) == std::string::npos && summary.find(records::iso8601(now + 5)) == std::string::npos &&
        summary.find(records::iso8601(now + 6)) == std::string::npos && summary.find("1234 ms") != std::string::npos,
        "copied summary keeps only three newest failed checks and excludes quick non-waited endpoints and cancelled rounds");
    t.check(exported.find("samples")->size() == recent.items().size() &&
        json::serialize(*exported.find("samples")) == json::serialize(*recent.to_json().find("samples")),
        "JSON exports the same detailed sample records without truncating them to the copied failure summary");
    auto partial = ConnectivityReport::simple(true, false); partial.mode = ProbeMode::complete;
    partial.apple.elapsed = 0.123; partial.microsoft.state = EndpointState::tls; partial.microsoft.elapsed = 0.456;
    records::HealthLog partial_history; partial_history.append(records::sample_from(partial, now));
    const auto partial_summary = records::diagnostics_summary_text(export_context, partial_history);
    t.check(partial_summary.find("123 ms") != std::string::npos && partial_summary.find("456 ms") != std::string::npos &&
        partial_summary.find("{ms}") == std::string::npos && partial_summary.find(std::string(copy::text("records.sampleResult.partial"))) != std::string::npos,
        "partial reports describe their actual successful and failed timings without unresolved placeholders or an all-clear summary");
    export_context.now = now + records::HealthLog::kWindowSeconds + 1;
    const auto expired = records::diagnostics_export(export_context, partial_history, records::EventLog{});
    t.check(expired.find("samples")->size() == 0 && partial_history.items().size() == 1,
        "export observes the 24-hour boundary even after an idle period without mutating the retained in-memory history");

    records::FailureContext failure;
    failure.started_at = now - 5; failure.collected_at = now; failure.operation = "manual";
    failure.monitoring_mode = "recover"; failure.area = "automatic"; failure.status = "security";
    failure.environment = {"wifi", "on", "detected", false}; failure.campus_state = "unconfirmed";
    failure.campus_issue = CampusVerificationIssue::tls; failure.recovery.state = "paused"; failure.recovery.paused = true;
    failure.recovery.cooldown_remaining = 90;
    failure.recovery.hold_reasons = {HoldReason::userPaused, HoldReason::security, HoldReason::uncertain, HoldReason::credentials};
    physical::CandidateDiagnostics physical;
    physical.interface_count = physical.candidate_count = 9000;
    for (int n = 0; n <= static_cast<int>(physical::CandidateRejection::noIPv4Service); ++n)
        physical.reject(static_cast<physical::CandidateRejection>(n), 9000);
    physical.rejections[static_cast<physical::CandidateRejection>(999)] = 9000;
    failure.physical_network = physical; failure.physical_collected_at = now - 6;
    records::EventLog failures;
    t.check(failures.append(now, "fixture diagnostic", true, failure), "operation result creates one explicitly failed contextual event");
    auto repeated = failure; repeated.collected_at = now + 1; repeated.started_at = now - 1; repeated.recovery.cooldown_remaining = 89;
    t.check(!failures.append(now + 1, "fixture diagnostic", true, repeated), "stable repeated failures retain the existing 300-second deduplication without countdown spam");
    repeated.campus_issue = CampusVerificationIssue::dns;
    t.check(failures.append(now + 2, "fixture diagnostic", true, repeated), "a changed fixed failure reason is retained even inside the duplicate-message window");
    const auto failure_json = json::serialize(failures.to_json());
    const auto failures_back = records::EventLog::from_json(failure_json);
    t.check(failures_back && failures_back->items().size() == 2 && failures_back->items()[0].failure &&
        failures_back->items()[0].context && failures_back->items()[0].context->recovery.paused &&
        failures_back->items()[0].context->recovery.hold_reasons.size() == 4 &&
        failures_back->items()[0].context->physical_network->interface_count == 4096 &&
        failures_back->items()[0].context->physical_network->rejections.size() == static_cast<std::size_t>(physical::CandidateRejection::noIPv4Service) + 1 &&
        json::serialize(failures_back->to_json()) == failure_json,
        "failure contexts survive reload with original timestamps, protections and bounded known physical rejection reasons");
    auto injected = json::parse(failure_json);
    injected->find("events")->items()[0].find("context")->set("status", json::Value::string("https://private.invalid/path"));
    const auto untrusted_event = records::EventLog::from_json(json::serialize(*injected));
    t.check(untrusted_event && !untrusted_event->items()[0].context &&
        json::serialize(untrusted_event->to_json()).find("private.invalid") == std::string::npos,
        "unrecognized raw operation status discards the context without leaking arbitrary strings on re-export");
    records::EventLog full_events;
    for (int n = 0; n < 301; ++n) full_events.append(now + n, "fixture diagnostic " + std::to_string(n), true, failure);
    const auto full_events_json = json::serialize(full_events.to_json());
    const auto full_events_back = records::EventLog::from_json(full_events_json);
    t.check(full_events.items().size() == 300 && full_events_json.size() < 4 * 1024 * 1024 &&
        full_events_back && full_events_back->items().size() == 300,
        "300 maximum-field diagnostic events fit both the 4 MiB JSON parser and 8 MiB native storage read limits");
}
