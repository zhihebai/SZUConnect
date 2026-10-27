#pragma once
#include "szu/presentation.hpp"

template<class Harness> void run_history_statistics_tests(Harness& t) {
    using namespace szu;
    constexpr double now = 1800000000;
    records::HealthLog health;
    const double times[] = {now, now + 2, now + 1, now + 3};
    const double latencies[] = {30, 10, 40, -1};
    for (int i = 0; i < 4; ++i) {
        records::Sample sample;
        sample.time = times[i]; sample.online = i == 1 || i == 2; sample.latency_ms = latencies[i];
        health.append(sample);
    }
    const auto original = json::serialize(health.to_json());
    const auto all = health.summary(now + 3);
    t.check(all.samples == 4 && all.online == 2 && all.median_latency_ms == 30,
        "history median includes valid durations independently of the online count");
    const auto even = health.summary(now + records::HealthLog::kWindowSeconds + 1);
    t.check(even.samples == 3 && even.online == 2 && even.median_latency_ms == 25,
        "history statistics exclude expired samples before computing an even median");
    const auto range = health.range(now + records::HealthLog::kWindowSeconds + 1, 2);
    t.check(range.count == 3 && range.first == now + 2 && range.last == now + 3 &&
        range.ribbon_count == 2 && range.ribbon_first == now + 1 && range.ribbon_last == now + 3,
        "out-of-order timestamps retain insertion order in the history ribbon");
    const auto zero = health.range(now + 3, 0);
    t.check(zero.count == 4 && zero.first == now && zero.last == now + 3 &&
        zero.ribbon_count == 0 && !zero.ribbon_first && !zero.ribbon_last,
        "a zero-width history ribbon retains coverage without reading past its last sample");
    const auto oversized = health.range(now + 3, 1000);
    t.check(oversized.ribbon_count == 4 && oversized.ribbon_first == now && oversized.ribbon_last == now + 3,
        "an oversized ribbon remains bounded by the retained history");
    t.check(health.summary(now + records::HealthLog::kWindowSeconds).samples == 4 &&
        health.range(now + records::HealthLog::kWindowSeconds).count == 4,
        "the 24-hour cutoff includes its exact boundary");
    const auto empty = health.range(now + records::HealthLog::kWindowSeconds + 4);
    t.check(empty.count == 0 && !empty.first && !empty.last && !empty.ribbon_first && !empty.ribbon_last &&
        !health.summary(now + records::HealthLog::kWindowSeconds + 4).median_latency_ms,
        "expired history has neither a range nor an invented latency");
    t.check(health.summary(now - 1).samples == 4 && health.range(now - 1).count == 4 &&
        json::serialize(health.to_json()) == original,
        "moving the query clock backwards does not mutate or reorder saved observations");

    records::HealthLog full;
    for (std::size_t i = 0; i < records::HealthLog::kLimit; ++i) {
        records::Sample sample;
        const auto value = (i * 137) % records::HealthLog::kLimit;
        sample.time = now + static_cast<double>(i);
        sample.online = value % 5 != 0; sample.latency_ms = static_cast<double>(value + 1);
        full.append(sample);
    }
    const auto stored = json::serialize(full.to_json());
    const auto summary = full.summary(now + 360);
    const auto last = full.range(now + 360);
    t.check(summary.samples == 360 && summary.online == 288 && summary.median_latency_ms == 180.5 &&
        last.count == 360 && last.ribbon_count == 24 && last.ribbon_first == now + 336 && last.ribbon_last == now + 359,
        "a full unsorted latency window preserves exact median, success count and ribbon coverage");
    const auto restored = records::HealthLog::from_json(stored);
    t.check(restored && restored->summary(now + 360).median_latency_ms == 180.5 &&
        json::serialize(full.to_json()) == stored,
        "statistics remain compatible with existing serialized records and never sort the source history");
}

