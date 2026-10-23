#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class HeroWebsiteChipFake final : public campusverificationruntime::Fake {
public:
    explicit HeroWebsiteChipFake(std::size_t count) {
        files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"automatic","probeMode":"complete","appearanceMode":"system"})";
        WebsitePreferences preferences;
        for (std::size_t index = 0; index < count; ++index) {
            const auto host = index + 1 == count ? std::string(54, 'a') + ".long-name.example" :
                index == 0 ? std::string("a.example") : "site" + std::to_string(index) + ".example";
            preferences.targets.push_back({"hero-site-" + std::to_string(index), "https://" + host + "/", true});
        }
        files[L"websites.json"] = *preferences.encode();
    }
    ConnectivityReport probe(ProbeMode mode, const CancelToken& token) override {
        ++probes;
        const bool blocked = block_probe;
        if (blocked) {
            ++block_entered;
            while (!release_probe && !token.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++block_exits;
        }
        auto report = ConnectivityReport::simple(true, true); report.mode = mode;
        report.apple.elapsed = 0.142; report.microsoft.elapsed = 1.263;
        if (token.cancelled()) {
            report.apple.state = report.microsoft.state = EndpointState::cancelled;
            report.complete_result = CompleteCheckResult::cancelled;
        }
        return report;
    }
    WebsiteCheckResult check_website(const std::string& url, bool, const CancelToken&) override {
        ++site_checks;
        return url.find("site1.") == std::string::npos ? WebsiteCheckResult{"accessible", 200, 0.246} :
            WebsiteCheckResult{"timeout", 0, 8.0};
    }
    win::NetworkSnapshot capture_network() override {
        if (!unbound) return campusverificationruntime::Fake::capture_network();
        win::NetworkSnapshot result; result.access = "ethernet"; result.tunnel_known = true;
        result.path.usable = result.path.link_active = true; result.path.identity = "fixture-hero-unbound";
        return result;
    }
    bool set_expected_physical_route(const std::optional<physical::Route>& expected) override {
        return unbound ? !expected : campusverificationruntime::Fake::set_expected_physical_route(expected);
    }
    std::atomic<int> site_checks{0};
    std::atomic<bool> block_probe{false}, release_probe{false};
    std::atomic<int> block_entered{0}, block_exits{0};
    bool unbound = false;
};

struct HeroChipProfile { float dpi, width, text_scale; };
struct HeroChipPaintFacts {
    const Widget* widget = nullptr;
    D2D1_RECT_F visual{};
    float label_baseline = 0, detail_baseline = 0;
};

struct HeroChipSlot {
    std::string key;
    D2D1_RECT_F rect{}, visual{};
};

std::vector<HeroChipSlot> hero_chip_slots(Canvas& canvas, const Scene& scene) {
    std::vector<HeroChipSlot> result;
    for (const auto& item : scene.widgets) if (item.layer == 0 && item.kind == Kind::chip && item.compact)
        result.push_back({item.id.empty() ? "base:" + win::narrow(item.text) : item.id,
            item.rect, result_chip_layout(canvas, item, item.rect).visual});
    return result;
}

bool same_hero_chip_slots(const std::vector<HeroChipSlot>& before, const std::vector<HeroChipSlot>& after) {
    if (before.empty() || before.size() != after.size()) return false;
    // Status headings may have different line counts; compare the result group
    // relative to its first row, rather than imposing a fixed heading height.
    for (std::size_t index = 0; index < before.size(); ++index) {
        const auto& x = before[index]; const auto& y = after[index];
        if (x.key != y.key || std::abs(x.rect.left - y.rect.left) > 0.5f || std::abs(x.rect.right - y.rect.right) > 0.5f ||
            std::abs((x.rect.top - before.front().rect.top) - (y.rect.top - after.front().rect.top)) > 0.5f ||
            std::abs((x.rect.bottom - before.front().rect.top) - (y.rect.bottom - after.front().rect.top)) > 0.5f ||
            std::abs((x.visual.bottom - x.visual.top) - (y.visual.bottom - y.visual.top)) > 0.5f) return false;
    }
    return true;
}

bool hero_pending_website_nodes(FixtureHost& host, const std::vector<WebsiteTarget>& targets, const std::wstring& detail) {
    Accessibility access(host); ComPtr<IRawElementProviderFragment> root;
    root.Attach(access.root_for_testing()); bool valid = true;
    for (const auto& target : targets) {
        const auto id = "website.result." + target.id; const auto* item = host.scene.find(id);
        if (!target.enabled) { valid = valid && !item; continue; }
        const auto node = find_node(root.Get(), win::widen(id));
        ComPtr<IRawElementProviderSimple> simple; ComPtr<IUnknown> invoke;
        if (node) node.As(&simple);
        valid = valid && item && item->detail == detail && !item->action && !item->focusable && item->role == Role::text &&
            simple && SUCCEEDED(simple->GetPatternProvider(UIA_InvokePatternId, invoke.GetAddressOf())) && !invoke;
    }
    access.disconnect(); return valid;
}

void verify_real_hero_website_check_lifecycle(HWND window, Canvas& canvas, RuntimeMode mode) {
    auto services = std::make_unique<HeroWebsiteChipFake>(3); auto* fake = services.get();
    auto preferences = *WebsitePreferences::decode(fake->files[L"websites.json"]);
    preferences.targets.back().enabled = false;
    fake->files[L"websites.json"] = *preferences.encode();
    fake->block_probe = true;
    AppController app(nullptr, mode, std::move(services)); app.start();
    constexpr float width = 500, height = 1200;
    SetWindowPos(window, nullptr, 0, 0, 1000, 2400, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
    canvas.set_dpi(192); canvas.resize(1000, 2400); canvas.set_text_scale(1.5f);
    FixtureHost host; host.window = window; host.scale = 2;
    UiState ui;
    ViewContext context{app, ui, window, [] {}, [&](const std::string& name) { ui.overlays.push_back(name); },
        [&] { if (!ui.overlays.empty()) ui.overlays.pop_back(); }, [](const std::string&) {}};
    const auto build = [&] { build_scene(host.scene, canvas, context, width, height); };
    build(); const auto initial = hero_chip_slots(canvas, host.scene);
    check(initial.size() == 4 && hero_pending_website_nodes(host, preferences.targets, T("compat.result.notRun")),
        "a complete-check selection keeps enabled website placeholders visible before its first report without enabling old details");
    app.check();
    wifiruntime::wait_for([&] { app.on_changed(); return fake->block_entered == 1 && app.snapshot().busy && !app.internet_report(); });
    build(); const auto running = hero_chip_slots(canvas, host.scene);
    check(fake->block_entered == 1 && app.snapshot().busy && !app.internet_report() && running.size() == 4 &&
        hero_pending_website_nodes(host, preferences.targets, T("dashboard.checking")) && same_hero_chip_slots(initial, running),
        "the real blocked request clears InternetCheckService.latest but retains the same enabled website slots as noninvokable checking placeholders");
    fake->release_probe = true;
    wifiruntime::wait_for([&] { app.on_changed(); return !app.snapshot().busy && app.internet_report() && app.last_checked() && app.system_checked_at(); });
    build(); const auto completed = hero_chip_slots(canvas, host.scene);
    bool finished = app.internet_report() && app.internet_report()->websites.size() == 2 && same_hero_chip_slots(running, completed);
    for (const auto& target : preferences.targets) {
        const auto* item = host.scene.find("website.result." + target.id);
        finished = finished && (target.enabled ? item && item->action && item->focusable && item->role == Role::button : !item);
    }
    check(finished, "one real complete round replaces only placeholder contents and actions while retaining website identities, widths and row slots");
    const auto completed_time = app.last_checked();
    app.on_network_signal(); app.on_timer(kTimerNetwork); app.on_changed();
    wifiruntime::wait_for([&] { app.on_changed(); app.set_probe(ProbeMode::quick); return app.settings().probe == ProbeMode::quick; });
    build();
    check(app.internet_report() && app.internet_report()->base.mode == ProbeMode::complete && app.internet_report()->websites_deferred &&
        app.last_checked() == completed_time && same_hero_chip_slots(completed, hero_chip_slots(canvas, host.scene)) &&
        hero_pending_website_nodes(host, preferences.targets, T("compat.result.notRun")),
        "deferred old complete results retain non-success website placeholders and their original scope even after the next selection becomes quick");
    wifiruntime::wait_for([&] { app.on_changed(); app.set_probe(ProbeMode::complete); return app.settings().probe == ProbeMode::complete; });
    fake->release_probe = false;
    app.check();
    wifiruntime::wait_for([&] { app.on_changed(); return fake->block_entered == 2 && app.snapshot().busy && !app.internet_report(); });
    build();
    check(hero_pending_website_nodes(host, preferences.targets, T("dashboard.checking")),
        "starting a later check immediately withdraws the previous successful website detail actions");
    app.cancel(); fake->release_probe = true;
    wifiruntime::wait_for([&] { app.on_changed(); return fake->block_exits == 2 && !app.snapshot().busy; });
    build();
    check(!app.internet_report() && hero_pending_website_nodes(host, preferences.targets, T("compat.result.notRun")),
        "cancelling the real round cannot republish earlier successful website results or their detail actions");
    fake->block_probe = false;
    wifiruntime::wait_for([&] { app.on_changed(); app.set_probe(ProbeMode::quick); return app.settings().probe == ProbeMode::quick; });
    const int website_calls = fake->site_checks;
    fake->release_probe = false; fake->block_probe = true;
    app.check();
    wifiruntime::wait_for([&] { app.on_changed(); return fake->block_entered == 3 && app.snapshot().busy && !app.internet_report(); });
    build();
    bool quick_has_no_sites = fake->block_entered == 3 && app.snapshot().busy && hero_chip_slots(canvas, host.scene).size() == 2;
    for (const auto& target : preferences.targets) quick_has_no_sites = quick_has_no_sites && !host.scene.find("website.result." + target.id);
    check(quick_has_no_sites, "a real quick round displays only the two basic probes despite configured enabled websites");
    fake->release_probe = true;
    wifiruntime::wait_for([&] { app.on_changed(); return !app.snapshot().busy && app.internet_report() && app.internet_report()->base.mode == ProbeMode::quick; });
    build();
    check(app.internet_report() && app.internet_report()->websites.empty() && hero_chip_slots(canvas, host.scene).size() == 2 &&
        fake->site_checks == website_calls && fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0 &&
        fake->evidence().network_calls == 0 && fake->evidence().production_storage_accesses == 0 && fake->evidence().vault_accesses == 0,
        "completed quick checks keep websites absent and issue no website requests, real authentication or production I/O");
    app.shutdown();
}

void verify_hero_actual_scope_overrides(HWND window, Canvas& canvas) {
    constexpr float width = 500, height = 1200;
    SetWindowPos(window, nullptr, 0, 0, 1000, 2400, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
    canvas.set_dpi(192); canvas.resize(1000, 2400); canvas.set_text_scale(1.5f);
    for (const std::string scenario : {"uncertainUnbound", "uncertainBound", "priority"}) {
        const bool priority = scenario == "priority", deferred = scenario == "uncertainBound";
        auto services = std::make_unique<HeroWebsiteChipFake>(2); auto* fake = services.get();
        fake->unbound = scenario == "uncertainUnbound";
        fake->files[L"settings.json"] = std::string(R"({"formatVersion":1,"monitoringMode":")") + (priority ? "recover" : "manual") +
            R"(","area":"automatic","probeMode":")" + (priority ? "complete" : "quick") + R"(","appearanceMode":"system"})";
        if (!priority) fake->files[L"recovery.json"] = "{\"schema\":1,\"generation\":1,\"notBefore\":" +
            std::to_string(static_cast<long long>(AppController::wall_now() + 90)) + ",\"holds\":[{\"reason\":2,\"origin\":2}]}";
        fake->block_probe = true;
        AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
        const ProbeMode preference = priority ? ProbeMode::complete : ProbeMode::quick;
        const ProbeMode actual = priority ? ProbeMode::quick : ProbeMode::complete;
        if (priority) app.on_timer(kTimerAuto); else app.check();
        wifiruntime::wait_for([&] { app.on_changed(); return fake->block_entered == 1 && app.snapshot().busy && app.internet_activity(); });
        const auto activity = app.internet_activity();
        FixtureHost host; host.window = window; host.scale = 2; UiState ui;
        ViewContext context{app, ui, window, [] {}, [&](const std::string& name) { ui.overlays.push_back(name); },
            [&] { if (!ui.overlays.empty()) ui.overlays.pop_back(); }, [](const std::string&) {}};
        const auto build = [&] { build_scene(host.scene, canvas, context, width, height); };
        build(); const auto running = hero_chip_slots(canvas, host.scene);
        check(app.settings().probe == preference && activity && activity->mode == actual &&
            activity->websites.size() == (priority ? 0u : 2u) && activity->websites_deferred == deferred &&
            running.size() == (priority ? 2u : 4u),
            "real uncertain verification and prioritized recovery display their owned scope rather than the opposite saved quick/complete preference");
        if (!priority) check(hero_pending_website_nodes(host, app.website_targets(), T(deferred ? "compat.result.notRun" : "dashboard.checking")),
            "a bound uncertain check labels intentionally deferred websites unmeasured while an unbound full round labels its actual websites checking");
        fake->release_probe = true;
        wifiruntime::wait_for([&] {
            app.on_changed();
            return !app.snapshot().busy && app.internet_report() && app.internet_report()->base.mode == actual &&
                !app.internet_activity() && app.last_checked();
        });
        build();
        const auto report = app.internet_report(); const auto checked = app.last_checked();
        check(report && checked && report->base.mode == actual && report->websites_deferred == deferred &&
            report->websites.size() == (priority || deferred ? 0u : 2u) && app.settings().probe == preference &&
            same_hero_chip_slots(running, hero_chip_slots(canvas, host.scene)),
            "override completion retains its actual mode and website slots without changing the saved scope or fabricating deferred successes");
        if (deferred) check(hero_pending_website_nodes(host, app.website_targets(), T("compat.result.notRun")),
            "a completed bound verification retains its deferred websites as noninvokable unmeasured placeholders");
        if (priority) {
            for (const auto selected : {ProbeMode::quick, ProbeMode::complete}) {
                wifiruntime::wait_for([&] { app.on_changed(); app.set_probe(selected); return app.settings().probe == selected; });
                build();
                check(app.settings().probe == selected && app.internet_report() && app.internet_report()->base.mode == ProbeMode::quick && app.last_checked() == checked &&
                    hero_chip_slots(canvas, host.scene).size() == 2 && !host.scene.find("website.result.hero-site-0"),
                    "changing the next preference cannot retroactively add website slots to an already completed prioritized quick round");
            }
        }
        check(fake->site_checks == (priority || deferred ? 0 : 2) && fake->evidence().fake_auth_submissions == 0 &&
            fake->evidence().network_calls == 0 && fake->evidence().fake_wifi_calls == 0 && fake->evidence().production_storage_accesses == 0,
            "effective-scope UI checks use only the engine-selected fake requests without credential submissions or production I/O");
        app.shutdown();
    }
}

// Ask DirectWrite for the baseline of the same bounded, vertically centered
// single-line paragraph used by Canvas::text_single_line.
std::optional<float> hero_chip_baseline(IDWriteFactory* factory, Canvas& canvas, const std::wstring& text,
                                        TextStyle style, const D2D1_RECT_F& rect) {
    const float width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (!factory || text.empty() || width <= 0 || height <= 0) return std::nullopt;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), canvas.text_format_for_testing(style),
        width, height, layout.GetAddressOf()))) return std::nullopt;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_TEXT_METRICS metrics{}; DWRITE_LINE_METRICS line{}; UINT32 lines = 0;
    if (FAILED(layout->GetMetrics(&metrics)) || FAILED(layout->GetLineMetrics(&line, 1, &lines)) || lines != 1 ||
        metrics.height > height + 0.2f) return std::nullopt;
    return rect.top + metrics.top + line.baseline;
}

void verify_hero_website_chip_matrix(HWND window, Canvas& canvas, RuntimeMode mode, bool exhaustive) {
    constexpr float height = 1200;
    std::vector<HeroChipProfile> profiles;
    if (exhaustive) {
        for (const float dpi : {120.0f, 144.0f, 168.0f, 192.0f})
            for (const float width : {480.0f, 500.0f, 700.0f, 900.0f})
                for (const float scale : {1.0f, 1.5f}) profiles.push_back({dpi, width, scale});
    } else profiles = {{120, 480, 1}, {192, 500, 1.5f}};
    ComPtr<IDWriteFactory> text_factory;
    const bool factory_ready = SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(text_factory.GetAddressOf())));
    check(factory_ready, "hero chip regression obtains native DirectWrite baseline metrics");
    bool saw_mixed_row = false, saw_wrapped_group = false;
    const std::vector<std::size_t> counts = exhaustive ? std::vector<std::size_t>{0, 1, 2, 3, WebsiteTarget::maximum_count} : std::vector<std::size_t>{2};
    for (const std::size_t count : counts) {
        auto services = std::make_unique<HeroWebsiteChipFake>(count); auto* fake = services.get();
        AppController app(nullptr, mode, std::move(services)); app.start(); app.check();
        wifiruntime::wait_for([&] {
            app.on_changed();
            const auto report = app.internet_report();
            return !app.snapshot().busy && report && report->websites.size() == count && app.last_checked() && app.system_checked_at();
        });
        app.on_changed();
        const auto report = app.internet_report();
        check(report && report->base.mode == ProbeMode::complete && report->websites.size() == count,
            "hero layout receives an actual completed controller report for zero, one, two, three or twelve selected websites");
        if (!report || report->websites.size() != count) { app.shutdown(); continue; }
        const auto before = app.snapshot(); const auto files = fake->files; const auto due = app.next_check();
        const auto checked = app.last_checked(), system_checked = app.system_checked_at();
        const int probe_calls = fake->probes, system_calls = fake->system_probes, site_calls = fake->site_checks;
        FixtureHost host; host.window = window;
        UiState ui;
        ViewContext context{app, ui, window, [] {}, [&](const std::string& name) { ui.overlays.push_back(name); },
            [&] { if (!ui.overlays.empty()) ui.overlays.pop_back(); }, [](const std::string&) {}};
        auto case_profiles = profiles;
        if (exhaustive && count == 2)
            for (float width = 480; width <= 900; width += 4) case_profiles.push_back({144, width, 1});
        for (const auto profile : case_profiles) {
            const auto pixel_width = static_cast<UINT>(std::ceil(profile.width * profile.dpi / 96));
            const auto pixel_height = static_cast<UINT>(std::ceil(height * profile.dpi / 96));
            SetWindowPos(window, nullptr, 0, 0, static_cast<int>(pixel_width), static_cast<int>(pixel_height),
                SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
            canvas.set_dpi(profile.dpi); canvas.resize(pixel_width, pixel_height); canvas.set_text_scale(profile.text_scale);
            host.scale = profile.dpi / 96; host.scene = Scene{};
            const auto build = [&] { build_scene(host.scene, canvas, context, profile.width, height); };
            build();
            const auto* primary = host.scene.find("hero.primary");
            const Widget* card = nullptr;
            if (primary) for (const auto& widget : host.scene.widgets) {
                if (widget.kind != Kind::panel || widget.layer != 0 || widget.fill != "surface" || widget.border.empty()) continue;
                if (widget.rect.left <= primary->rect.left && widget.rect.right >= primary->rect.right &&
                    widget.rect.top <= primary->rect.top && widget.rect.bottom >= primary->rect.bottom &&
                    (!card || widget.rect.bottom - widget.rect.top < card->rect.bottom - card->rect.top)) card = &widget;
            }
            std::vector<std::vector<HeroChipPaintFacts>> rows;
            bool geometry = card != nullptr, measured = factory_ready;
            std::size_t chips = 0, last_chip_index = 0, picker_index = host.scene.widgets.size();
            float last_right = 0, chip_bottom = 0;
            for (std::size_t index = 0; index < host.scene.widgets.size(); ++index) {
                const auto& chip = host.scene.widgets[index];
                if (chip.id == "probe.0") picker_index = index;
                if (!card || chip.layer != 0 || chip.kind != Kind::chip || !chip.compact || chip.rect.top < card->rect.top ||
                    chip.rect.bottom > card->rect.bottom) continue;
                if (rows.empty() || chip.rect.left < last_right - 0.2f) rows.emplace_back();
                const auto paint = result_chip_layout(canvas, chip, chip.rect);
                const auto label = hero_chip_baseline(text_factory.Get(), canvas, chip.text, TextStyle::caption_strong, paint.label);
                const auto detail = hero_chip_baseline(text_factory.Get(), canvas, chip.detail, TextStyle::caption, paint.detail);
                measured = measured && label.has_value() && detail.has_value();
                geometry = geometry && chip.rect.left >= card->rect.left && chip.rect.right <= card->rect.right &&
                    paint.visual.top >= chip.rect.top - 0.2f && paint.visual.bottom <= chip.rect.bottom + 0.2f &&
                    paint.label.right <= paint.detail.left + 0.2f && paint.detail.right <= chip.rect.right + 0.2f;
                rows.back().push_back({&chip, paint.visual, label.value_or(0), detail.value_or(0)});
                last_right = chip.rect.right; chip_bottom = std::max(chip_bottom, chip.rect.bottom);
                last_chip_index = index; ++chips;
            }
            bool aligned = true, uniform_gaps = true, nonoverlapping = true;
            float previous_bottom = 0, previous_hit_bottom = 0;
            std::optional<float> reference_gap;
            for (const auto& row : rows) {
                const auto& first = row.front();
                const float center = (first.visual.top + first.visual.bottom) / 2;
                float top = first.visual.top, bottom = first.visual.bottom, hit_bottom = 0;
                bool has_action = false, has_static = false;
                for (const auto& item : row) {
                    aligned = aligned && std::abs((item.visual.top + item.visual.bottom) / 2 - center) < 0.5f &&
                        std::abs(item.label_baseline - first.label_baseline) < 0.5f &&
                        std::abs(item.detail_baseline - first.detail_baseline) < 0.5f;
                    if (previous_bottom) nonoverlapping = nonoverlapping && item.widget->rect.top >= previous_hit_bottom - 0.2f;
                    top = std::min(top, item.visual.top); bottom = std::max(bottom, item.visual.bottom);
                    hit_bottom = std::max(hit_bottom, item.widget->rect.bottom);
                    has_action = has_action || !!item.widget->action; has_static = has_static || !item.widget->action;
                }
                saw_mixed_row = saw_mixed_row || (has_action && has_static);
                if (previous_bottom) {
                    const float gap = top - previous_bottom;
                    uniform_gaps = uniform_gaps && gap >= -0.2f && (!reference_gap || std::abs(gap - *reference_gap) < 0.5f);
                    reference_gap = reference_gap.value_or(gap);
                }
                previous_bottom = bottom; previous_hit_bottom = hit_bottom;
            }
            saw_wrapped_group = saw_wrapped_group || (count > 0 && rows.size() > 1);
            bool balanced_tail = true;
            if (rows.size() > 1 && rows.back().size() == 1 && rows[rows.size() - 2].size() > 2) {
                const auto& previous = rows[rows.size() - 2];
                const auto& moved = *previous.back().widget; const auto& tail = *rows.back().front().widget;
                const float horizontal_gap = previous[1].widget->rect.left - previous[0].widget->rect.right;
                const float inset = rows.front().front().widget->rect.left - card->rect.left;
                const float available = card->rect.right - card->rect.left - inset * 2;
                balanced_tail = moved.rect.right - moved.rect.left + tail.rect.right - tail.rect.left + horizontal_gap > available + 0.5f;
            }
            float meta_top = std::numeric_limits<float>::max(), meta_bottom = 0;
            for (std::size_t index = last_chip_index + 1; index < picker_index; ++index) {
                const auto& item = host.scene.widgets[index];
                if (item.kind == Kind::text && !item.text.empty()) {
                    meta_top = std::min(meta_top, item.rect.top); meta_bottom = std::max(meta_bottom, item.rect.bottom);
                }
            }
            const auto* quick = host.scene.find("probe.0"); const auto* complete = host.scene.find("probe.1");
            const auto* info = host.scene.find("probe.row.info");
            const bool below = card && quick && complete && info && meta_bottom > meta_top && meta_top >= chip_bottom - 0.2f &&
                quick->rect.top >= meta_bottom - 0.2f && complete->rect.top >= meta_bottom - 0.2f && info->rect.top >= meta_bottom - 0.2f &&
                card->rect.bottom >= std::max({quick->rect.bottom, complete->rect.bottom, info->rect.bottom}) &&
                quick->rect.right <= complete->rect.left + 0.2f && complete->rect.right <= info->rect.left + 0.2f;
            if (!(geometry && measured && aligned && uniform_gaps && nonoverlapping && below && chips == count + 2))
                std::printf("hero chips count=%zu dpi=%.0f width=%.0f text=%.0f%% geometry=%d metrics=%d aligned=%d gaps=%d overlap=%d below=%d chips=%zu\n",
                    count, profile.dpi, profile.width, profile.text_scale * 100, geometry, measured, aligned, uniform_gaps, nonoverlapping, below, chips);
            check(chips == count + 2 && geometry && measured && aligned,
                "actual hero chips share a visual midline and native text baselines in every mixed clickable/static row");
            check(uniform_gaps && nonoverlapping && below && balanced_tail,
                "wrapped hero rows have equal visual gaps and disjoint targets, followed by intact metadata, selector and card boundaries");
            for (const auto& target : app.website_targets()) {
                const auto* chip = host.scene.find("website.result." + target.id);
                if (!chip) { check(false, "every selected website has a real hero result action"); continue; }
                auto& layer = host.scene.layers[0];
                layer.scroll = std::max(0.0f, chip->rect.top - layer.viewport.top - 8); host.scene.clamp_scroll(0);
                const auto visible = host.scene.visible_rect(*chip);
                bool clickable = chip->role == Role::button && chip->enabled && chip->focusable && chip->action && !chip->name.empty() &&
                    chip->rect.right - chip->rect.left >= tokens::kSizeMinTarget - 0.2f &&
                    chip->rect.bottom - chip->rect.top >= tokens::kSizeMinTarget - 0.2f;
                for (const float x : {visible.left + 2, (visible.left + visible.right) / 2, visible.right - 2})
                    clickable = clickable && host.scene.hit(x, (visible.top + visible.bottom) / 2) == chip;
                check(clickable, "the complete website result remains a distinct pointer and keyboard target after wrapping and scrolling");
            }
            host.scene.layers[0].scroll = 0; host.scene.clamp_scroll(0);
            for (const bool dark : {false, true}) {
                const bool began = canvas.begin();
                if (began) paint(canvas, make_palette(dark, false), host.scene, {}, profile.width, height, 0);
                check(began && canvas.end(), "native light and dark hero rendering completes with the actual mixed result geometry");
            }
            if (profile.dpi == 192 && profile.text_scale == 1.5f && profile.width == (exhaustive ? 480 : 500)) {
                FixtureHost busy_host; busy_host.window = window; busy_host.scale = host.scale;
                busy_host.scene.layers[0].viewport = D2D1::RectF(0, 0, profile.width, height);
                Builder busy_page(busy_host.scene, canvas, 0, tokens::kSpaceXxl, profile.width - tokens::kSpaceXxl * 2, 24);
                auto busy = app.snapshot(); busy.busy = true; busy.verifying_memory = busy.verifying_campus = false;
                build_page(busy_page, context, busy, status::present(status::input_from(busy)));
                busy_host.scene.layers[0].content_height = busy_page.y();
                std::vector<const Widget*> completed_chips, busy_chips;
                for (const auto& widget : host.scene.widgets)
                    if (widget.layer == 0 && widget.kind == Kind::chip && widget.compact) completed_chips.push_back(&widget);
                for (const auto& widget : busy_host.scene.widgets)
                    if (widget.layer == 0 && widget.kind == Kind::chip && widget.compact) busy_chips.push_back(&widget);
                bool stable = completed_chips.size() == count + 2 && busy_chips.size() == completed_chips.size();
                Accessibility busy_access(busy_host); ComPtr<IRawElementProviderFragment> busy_root;
                busy_root.Attach(busy_access.root_for_testing());
                for (std::size_t index = 0; index < busy_chips.size(); ++index) {
                    const auto& item = *busy_chips[index];
                    const auto visible = result_chip_layout(canvas, item, item.rect).visual;
                    if (index < completed_chips.size()) {
                        const auto& completed = *completed_chips[index];
                        const auto previous_visual = result_chip_layout(canvas, completed, completed.rect).visual;
                        stable = stable && std::abs((item.rect.bottom - item.rect.top) - (completed.rect.bottom - completed.rect.top)) < 0.5f &&
                            std::abs((visible.bottom - visible.top) - (previous_visual.bottom - previous_visual.top)) < 0.5f;
                    }
                    const auto expected_detail = T(item.id.starts_with("website.result.") ? "compat.result.notRun" : "dashboard.checking");
                    stable = stable && item.detail == expected_detail && !item.action && !item.focusable && item.role == Role::text &&
                        !busy_host.scene.hit((item.rect.left + item.rect.right) / 2, (item.rect.top + item.rect.bottom) / 2);
                    ComPtr<IRawElementProviderFragment> node;
                    if (busy_root) busy_root->Navigate(NavigateDirection_FirstChild, node.GetAddressOf());
                    bool found = false;
                    for (int visited = 0; node && visited < 256; ++visited) {
                        if (property(node.Get(), UIA_NamePropertyId) == item.name) {
                            ComPtr<IRawElementProviderSimple> simple; ComPtr<IUnknown> invoke;
                            node.As(&simple);
                            found = simple && SUCCEEDED(simple->GetPatternProvider(UIA_InvokePatternId, invoke.GetAddressOf())) && !invoke;
                            break;
                        }
                        ComPtr<IRawElementProviderFragment> next;
                        node->Navigate(NavigateDirection_NextSibling, next.GetAddressOf()); node = std::move(next);
                    }
                    stable = stable && found;
                }
                busy_access.disconnect();
                check(stable && (count == 0 || busy_host.scene.find("website.result.hero-site-0")),
                    "busy and completed results keep equal slot heights while old website details lose pointer, focus and native UIA invocation until completion");
                for (const auto& target : app.website_targets()) {
                    const std::string id = "website.result." + target.id;
                    Accessibility access(host); ComPtr<IRawElementProviderFragment> root;
                    root.Attach(access.root_for_testing()); const auto node = find_node(root.Get(), win::widen(id));
                    ComPtr<IRawElementProviderSimple> simple; ComPtr<IUnknown> pattern; ComPtr<IInvokeProvider> invoke;
                    if (node) node.As(&simple);
                    if (simple) simple->GetPatternProvider(UIA_InvokePatternId, pattern.GetAddressOf());
                    if (pattern) pattern.As(&invoke);
                    const bool focused = node && SUCCEEDED(node->SetFocus()) && host.focused == id;
                    const bool invoked = invoke && SUCCEEDED(invoke->Invoke()); access.disconnect();
                    check(focused && invoked && ui.overlays == std::vector<std::string>{"websiteResult"} &&
                        ui.website_details && ui.website_details->check.target.id == target.id && ui.website_details->check.target.url == target.url,
                        "native UIA focuses and invokes each captured website result, including the clipped long host, without changing its identity");
                    build();
                    check(host.scene.find("sheet.close") && invoke_r11_action(host, L"sheet.close"),
                        "the actual result details expose a reachable native close action");
                    build();
                }
            }
        }
        const auto after = app.snapshot();
        check(fake->files == files && fake->probes == probe_calls && fake->system_probes == system_calls && fake->site_checks == site_calls &&
            app.last_checked() == checked && app.system_checked_at() == system_checked && app.next_check() == due &&
            after.epoch == before.epoch && after.generation == before.generation && after.trusted == before.trusted &&
            after.remembered == before.remembered && after.holds.size() == before.holds.size() && after.paused == before.paused &&
            fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0 && fake->evidence().network_calls == 0 &&
            fake->evidence().vault_accesses == 0 && fake->evidence().production_storage_accesses == 0,
            "hero resize, rendering, pointer lookup and UIA details preserve captured results and all recovery/data state using isolated services only");
        app.shutdown();
    }
    check(saw_wrapped_group && (!exhaustive || saw_mixed_row),
        "the native matrix includes actual wrapping and, at wider sizes, mixed website/base rows rather than only single-item specimens");
    if (exhaustive) {
        verify_real_hero_website_check_lifecycle(window, canvas, mode);
        if (mode == RuntimeMode::normal) verify_hero_actual_scope_overrides(window, canvas);
    }
}

}  // namespace
