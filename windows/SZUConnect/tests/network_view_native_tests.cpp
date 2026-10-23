// The real view, DirectWrite and UIA consume only injected double-snapshot evidence.
#include "accessibility.hpp"
#include "views.hpp"
#include "physical_diagnostics_runtime_tests.hpp"
#include "r11_presentation_runtime_tests.hpp"
#include "memory_management_runtime_tests.hpp"
#include "crash_export_runtime_tests.hpp"
#include "shared_tokens.hpp"
#include "shared_copy.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <wrl/client.h>

using namespace szu;
using namespace szu::app;
using Microsoft::WRL::ComPtr;
namespace {
int checks = 0, failures = 0;
void check(bool passed, const char* message) {
    ++checks;
    if (!passed) { ++failures; std::printf("FAIL: %s\n", message); }
}

#include "header_brand_native_checks.hpp"

class FixtureHost final : public AccessHost {
public:
    HWND window = nullptr;
    Scene scene;
    float scale = 1;
    std::string focused;
    HWND host_window() const override { return window; }
    const Scene& host_scene() const override { return scene; }
    float host_scale() const override { return scale; }
    std::string host_focus() const override { return focused; }
    void host_set_focus(const std::string& id) override {
        const auto* widget = scene.find(id);
        if (widget && widget->enabled && widget->focusable) focused = id;
    }
    void host_activate(const std::string& id) override {
        const auto* widget = scene.find(id);
        if (widget && widget->enabled && widget->action) { const auto action = widget->action; action(); }
    }
};

std::wstring property(IRawElementProviderFragment* node, PROPERTYID id) {
    ComPtr<IRawElementProviderSimple> simple;
    if (!node || FAILED(node->QueryInterface(IID_PPV_ARGS(simple.GetAddressOf())))) return {};
    VARIANT value{};
    std::wstring result;
    if (SUCCEEDED(simple->GetPropertyValue(id, &value)) && value.vt == VT_BSTR && value.bstrVal)
        result.assign(value.bstrVal, SysStringLen(value.bstrVal));
    VariantClear(&value);
    return result;
}

ComPtr<IRawElementProviderFragment> find_node(IRawElementProviderFragment* root, const std::wstring& id) {
    ComPtr<IRawElementProviderFragment> node;
    if (!root || FAILED(root->Navigate(NavigateDirection_FirstChild, node.GetAddressOf()))) return {};
    for (int count = 0; node && count < 256; ++count) {
        if (property(node.Get(), UIA_AutomationIdPropertyId) == id) return node;
        ComPtr<IRawElementProviderFragment> next;
        if (FAILED(node->Navigate(NavigateDirection_NextSibling, next.GetAddressOf()))) return {};
        node = std::move(next);
    }
    return {};
}

void build_guard(FixtureHost& host, Canvas& canvas, ViewContext& context, float width, float height) {
    host.scene = Scene{};
    host.scene.layers[0].viewport = D2D1::RectF(0, 0, width, height);
    Builder builder(host.scene, canvas, 0, tokens::kSpaceL, width - 2 * tokens::kSpaceL, tokens::kSpaceL);
    builder.card_begin("surface", "border", tokens::kSpaceL, tokens::kRadiusLarge);
    build_inline_guard(builder, context, context.app.snapshot());
    builder.card_end();
    host.scene.layers[0].content_height = builder.y() + tokens::kSpaceL;
}

void verify_bounds(Canvas& canvas, const Scene& scene, float width, float height) {
    bool bounds = true, measured = true, minimum_targets = true;
    for (const auto& widget : scene.widgets) {
        const auto& rect = widget.rect;
        const bool inside = std::isfinite(rect.left) && std::isfinite(rect.right) && std::isfinite(rect.top) && std::isfinite(rect.bottom) &&
            rect.left >= -0.1f && rect.right <= width + 0.1f && rect.top >= 0 && rect.bottom <= height + 0.1f &&
            rect.right > rect.left && (widget.kind == Kind::divider ?
                rect.bottom >= rect.top && rect.bottom - rect.top <= tokens::kSizeBorderWidth + 0.1f : rect.bottom > rect.top);
        if (!inside) std::printf("  geometry id=%s kind=%d rect=%.2f,%.2f,%.2f,%.2f viewport=%.2f,%.2f text=%.0f%%\n",
            widget.id.c_str(), static_cast<int>(widget.kind), rect.left, rect.top, rect.right, rect.bottom, width, height,
            canvas.text_rendering_info().text_scale * 100);
        bounds = bounds && inside;
        if (widget.kind == Kind::text && !widget.text.empty()) {
            const auto measured_text = canvas.measure(widget.text, widget.style, rect.right - rect.left);
            measured = measured && measured_text.height <= rect.bottom - rect.top + 0.2f;
        }
        if (widget.action && widget.focusable)
            minimum_targets = minimum_targets && rect.right - rect.left >= tokens::kSizeMinTarget - 0.1f &&
                rect.bottom - rect.top >= tokens::kSizeMinTarget - 0.1f;
    }
    check(bounds, "native diagnostic view keeps every widget inside the narrow viewport");
    check(measured, "DirectWrite measures every diagnostic text block without vertical clipping");
    check(minimum_targets, "diagnostic view retains minimum keyboard and pointer target sizes with large text");
}

void exercise_narrow_layout(Canvas& canvas, float width) {
    ComPtr<IDWriteFactory> factory;
    const bool factory_ready = SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(factory.GetAddressOf())));
    check(factory_ready, "create native DirectWrite reference for narrow-line regression");
    if (!factory_ready) return;
    const auto style = TextStyle::callout;
    auto* format = canvas.text_format_for_testing(style);
    const float line_width = canvas.measure(std::wstring(20, L'网'), style, 10000).width + 0.5f;
    for (const std::size_t length : {22u, 42u}) {
        const std::wstring sample(length, L'网');
        const auto original = text_layout_detail::create(factory.Get(), format, sample, line_width);
        const auto before = text_layout_detail::facts(original.Get());
        const auto after = canvas.inspect_text_layout_for_testing(sample, style, line_width);
        const auto measured = canvas.measure(sample, style, line_width);
        check(before.lines == (length == 22 ? 2u : 3u) && after.lines == before.lines &&
            std::abs(after.width - before.width) < 0.01f && std::abs(after.layout_width - line_width) < 0.01f &&
            std::abs(measured.height - after.height) < 0.01f && std::abs(measured.width - after.width) < 0.01f,
            "short Chinese paragraphs retain the actual line measure and native line breaks without implicit balancing");
    }
    const std::wstring explicit_lines = std::wstring(20, L'网') + L"\n网络";
    const auto manual = canvas.inspect_text_layout_for_testing(explicit_lines, style, line_width);
    const auto address = canvas.inspect_text_layout_for_testing(L"https://example.invalid/校园网络检测与排查", style, 110);
    const auto code = canvas.inspect_text_layout_for_testing(std::wstring(22, L'网'), TextStyle::mono, line_width);
    check(manual.lines == 2 && std::abs(manual.layout_width - line_width) < 0.01f &&
        std::abs(address.layout_width - 110) < 0.01f && std::abs(code.layout_width - line_width) < 0.01f,
        "manual line breaks, addresses and monospaced diagnostics retain their original line measure");

    Scene scene;
    constexpr float left = 24;
    Builder builder(scene, canvas, 0, left, width - left * 2, 24);
    const std::wstring heading = L"正在检查校园网登录服务";
    builder.status_header("loader", "busy", heading, L"正在确认当前网络对应的校园服务。", L"状态说明", [] {});
    const auto* title = scene.find("status.headline");
    const auto* info = scene.find("status.meta.info");
    const auto title_facts = title ? canvas.inspect_text_layout_for_testing(title->text, title->style,
        title->rect.right - title->rect.left) : TextLayoutFacts{};
    check(title && title->text == heading && info && info->rect.right <= width - left + 0.1f &&
        info->rect.left + 14 >= title->rect.left + title_facts.width + 8 &&
        std::abs(info->rect.top + 22 - (title->rect.top + std::min(title_facts.height, canvas.line_height(TextStyle::hero)) / 2)) < 0.1f,
        "status information target stays beside the first title line and outside its ink instead of becoming an orphan");

    Scene online_scene;
    Builder online(online_scene, canvas, 0, left, width - left * 2, 24);
    online.card_begin("surface", "border", tokens::kSpaceXl, tokens::kRadiusLarge);
    const auto online_detail = T("env.systemOnline.detail");
    online.status_header("checkmark-circle", "success", T("env.systemOnline.title"), online_detail,
        L"状态说明", [] {});
    online.card_end();
    const auto description = std::find_if(online_scene.widgets.begin(), online_scene.widgets.end(),
        [&online_detail](const Widget& widget) { return widget.kind == Kind::text && widget.text == online_detail; });
    const bool found_description = description != online_scene.widgets.end();
    const auto description_facts = found_description
        ? canvas.inspect_text_layout_for_testing(description->text, description->style,
            description->rect.right - description->rect.left) : TextLayoutFacts{};
    check(found_description && description_facts.lines == 1 &&
        description_facts.width <= description->rect.right - description->rect.left + 0.1f &&
        description_facts.height <= description->rect.bottom - description->rect.top + 0.1f,
        "system-online hero description stays on one complete line at the actual narrow-card width and text scale");
    builder.gap(20);
    const std::wstring metadata = L"校园直连 · Wi-Fi · 网络隧道 · 完整检测 12:34:56 · 下次检测 12:36";
    builder.selector_row("layout.probe", metadata, {L"快速检测", L"完整检测"}, 0, [](int) {}, L"检测说明", [] {});
    int atoms = 0;
    bool intact_atoms = true, meaningful_separators = true;
    for (const auto& widget : scene.widgets) {
        if (widget.kind != Kind::text || widget.style != TextStyle::caption) continue;
        if (widget.text == L" · ") { meaningful_separators = meaningful_separators && widget.rect.left > left; continue; }
        if (widget.text == L"校园直连" || widget.text == L"Wi-Fi" || widget.text == L"网络隧道" ||
            widget.text == L"完整检测 12:34:56" || widget.text == L"下次检测 12:36") {
            ++atoms;
            intact_atoms = intact_atoms && canvas.inspect_text_layout_for_testing(widget.text, widget.style,
                widget.rect.right - widget.rect.left).lines == 1;
        }
    }
    const auto* picker_info = scene.find("layout.probe.row.info");
    const auto* last_option = scene.find("layout.probe.1");
    const bool picker_grouped = picker_info && last_option && last_option->rect.right <= picker_info->rect.left &&
        picker_info->rect.top >= last_option->rect.top - 0.1f && picker_info->rect.bottom <= last_option->rect.bottom + 0.1f &&
        std::abs(picker_info->rect.top + picker_info->rect.bottom - last_option->rect.top - last_option->rect.bottom) < 0.1f;
    if (!(atoms == 5 && intact_atoms && meaningful_separators && picker_grouped))
        std::printf("  metadata atoms=%d intact=%d separators=%d info_top=%.2f picker_top=%.2f picker_right=%.2f info_left=%.2f width=%.2f text=%.0f%%\n",
            atoms, intact_atoms, meaningful_separators, picker_info ? picker_info->rect.top : -1,
            last_option ? last_option->rect.top : -1, last_option ? last_option->rect.right : -1,
            picker_info ? picker_info->rect.left : -1, width, canvas.text_rendering_info().text_scale * 100);
    check(atoms == 5 && intact_atoms && meaningful_separators && picker_grouped,
        "path, mode/time and next-check metadata wrap as complete semantic units while the picker and info remain together");

    Scene actions;
    Builder measuring(actions, canvas, 0, left, width - left * 2, 24);
    const float a_width = std::ceil(measuring.measure_button(L"暂停自动重连", "pause"));
    const float b_width = std::ceil(measuring.measure_button(L"核验校园服务", "shield-check"));
    Builder controls(actions, canvas, 0, left, a_width + b_width + tokens::kSpaceS + 0.5f, 24);
    controls.buttons({{"layout.pause", L"暂停自动重连", ButtonStyle::secondary, [] {}, "pause"},
        {"layout.verify", L"核验校园服务", ButtonStyle::secondary, [] {}, "shield-check"},
        {"layout.forget", L"忘记此网络", ButtonStyle::plain, [] {}, ""}});
    const auto* pause = actions.find("layout.pause");
    const auto* verify = actions.find("layout.verify");
    const auto* forget = actions.find("layout.forget");
    const auto order = actions.focus_order();
    check(pause && verify && forget && verify->rect.top > pause->rect.top && verify->rect.top == forget->rect.top &&
        verify->rect.right < forget->rect.left && order.size() == 3 && order[0] == pause && order[1] == verify && order[2] == forget,
        "an otherwise orphaned final secondary action shares a deliberate row with its neighbour without changing reading order");
    check(canvas.begin(), "begin native narrow layout paint");
    paint(canvas, make_palette(false, false), scene, {}, width, 1200, 0);
    check(canvas.end(), "the exact measured narrow layout paints through native DirectWrite");
}

void verify_layout_cache(Canvas& canvas) {
    ComPtr<IDWriteFactory> factory;
    const bool ready = SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(factory.GetAddressOf())));
    check(ready, "create bounded text-layout cache reference");
    if (!ready) return;
    TextLayoutCache cache;
    auto* format = canvas.text_format_for_testing(TextStyle::callout);
    const auto first = cache.get(factory.Get(), format, L"校园网络检测与排查", 120);
    const auto second = cache.get(factory.Get(), format, L"校园网络检测与排查", 120);
    check(first.layout && first.layout.Get() == second.layout.Get() && cache.size() == 1,
        "measurement and paint reuse the same native line layout for an unchanged text/width/format");
    for (int index = 0; index < 80; ++index)
        cache.get(factory.Get(), format, L"校园网络 " + std::to_wstring(index), 200);
    check(cache.size() == 64, "varying network status text keeps the native layout cache bounded");
    cache.clear();
    check(cache.size() == 0, "font or text-scale changes can discard every cached native line layout");
}

struct LineSnapshot {
    std::vector<DWRITE_LINE_METRICS> lines;
    DWRITE_TEXT_METRICS text{};
    float available_width = 0;
    bool valid = false;
};

LineSnapshot line_snapshot(IDWriteTextLayout* layout) {
    LineSnapshot result;
    if (!layout || FAILED(layout->GetMetrics(&result.text)) || !result.text.lineCount) return result;
    result.available_width = layout->GetMaxWidth();
    result.lines.resize(result.text.lineCount);
    UINT32 count = 0;
    result.valid = SUCCEEDED(layout->GetLineMetrics(result.lines.data(), static_cast<UINT32>(result.lines.size()), &count)) &&
        count == result.lines.size();
    return result;
}

bool same_lines(const LineSnapshot& a, const LineSnapshot& b) {
    if (!a.valid || !b.valid || a.lines.size() != b.lines.size() ||
        std::abs(a.available_width - b.available_width) > 0.01f ||
        std::abs(a.text.widthIncludingTrailingWhitespace - b.text.widthIncludingTrailingWhitespace) > 0.01f ||
        std::abs(a.text.height - b.text.height) > 0.01f) return false;
    for (std::size_t index = 0; index < a.lines.size(); ++index) {
        const auto& x = a.lines[index]; const auto& y = b.lines[index];
        if (x.length != y.length || x.trailingWhitespaceLength != y.trailingWhitespaceLength || x.newlineLength != y.newlineLength ||
            x.isTrimmed != y.isTrimmed || std::abs(x.height - y.height) > 0.01f || std::abs(x.baseline - y.baseline) > 0.01f)
            return false;
    }
    return true;
}

void verify_continuous_paragraph_widths(Canvas& canvas) {
    ComPtr<IDWriteFactory> factory;
    const bool ready = SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(factory.GetAddressOf())));
    check(ready, "create native DirectWrite reference for continuous paragraph resizing");
    if (!ready) return;
    struct Sample { std::wstring text; TextStyle style; bool monotonic; };
    const std::vector<Sample> samples{
        {T("about.creditsBody"), TextStyle::callout, false},
        {T("about.atmluck.detail"), TextStyle::caption, false},
        {T("wifiRecovery.autoJoinDetail"), TextStyle::callout, false},
        {T("wifiRecovery.windows.openOnly"), TextStyle::callout, false},
        {L"校园认证实现参考并改编自 AutoSZUWeb。欢迎在 GitHub 提交问题、建议或改进。", TextStyle::callout, false},
        {std::wstring(60, L'网'), TextStyle::callout, true}
    };
    constexpr int first_width = 300, last_width = 960, width_step = 2;
    for (const float scale : {1.0f, 1.5f}) {
        canvas.set_text_scale(scale);
        for (const auto& sample : samples) {
            auto* format = canvas.text_format_for_testing(sample.style);
            TextLayoutCache cache;
            std::vector<LineSnapshot> forward;
            bool native_match = true, actual_width = true, stable_growing = true, facts_match = true;
            for (int width = first_width; width <= last_width; width += width_step) {
                const auto cached = cache.get(factory.Get(), format, sample.text, static_cast<float>(width));
                const auto current = line_snapshot(cached.layout.Get());
                const auto native = text_layout_detail::create(factory.Get(), format, sample.text, static_cast<float>(width));
                native_match = same_lines(current, line_snapshot(native.Get())) && native_match;
                actual_width = current.valid && std::abs(current.available_width - width) < 0.01f && actual_width;
                facts_match = current.valid && cached.facts.lines == current.lines.size() &&
                    std::abs(cached.facts.layout_width - width) < 0.01f &&
                    std::abs(cached.facts.width - current.text.widthIncludingTrailingWhitespace) < 0.01f &&
                    std::abs(cached.facts.height - current.text.height) < 0.01f && facts_match;
                if (sample.monotonic && !forward.empty()) {
                    const auto& previous = forward.back();
                    stable_growing = current.valid && previous.valid && current.lines.size() <= previous.lines.size() &&
                        current.text.height <= previous.text.height + 0.01f &&
                        current.lines.front().length >= previous.lines.front().length && stable_growing;
                }
                forward.push_back(current);
            }
            check(native_match, "settings/about paragraphs match native line metrics at every width from 300 to 960 DIP");
            check(actual_width && facts_match, "paragraph measurement retains the entire available width and matching native height");
            if (sample.monotonic)
                check(stable_growing, "widening a Chinese paragraph never moves first-line characters backwards or adds a line");

            bool reverse_match = true;
            for (int width = last_width; width >= first_width; width -= width_step) {
                const auto cached = cache.get(factory.Get(), format, sample.text, static_cast<float>(width));
                const auto index = static_cast<std::size_t>((width - first_width) / width_step);
                reverse_match = same_lines(line_snapshot(cached.layout.Get()), forward[index]) && reverse_match;
            }
            check(reverse_match, "widening and narrowing revisit identical paragraph line breaks regardless of resize direction");
            const auto retained = cache.get(factory.Get(), format, sample.text, static_cast<float>(first_width));
            for (int index = 0; index < 80; ++index)
                cache.get(factory.Get(), format, L"缓存排版样本 " + std::to_wstring(index), 400);
            const auto recreated = cache.get(factory.Get(), format, sample.text, static_cast<float>(first_width));
            check(cache.size() == 64 && recreated.layout.Get() != retained.layout.Get() &&
                same_lines(line_snapshot(recreated.layout.Get()), forward.front()),
                "cache eviction recreates the same paragraph layout and retains the bounded 64-entry cache");
            const auto measured = canvas.measure(sample.text, sample.style, first_width);
            check(std::abs(measured.width - recreated.facts.width) < 0.01f &&
                std::abs(measured.height - recreated.facts.height) < 0.01f,
                "the canvas measurement path agrees with the native layout used for painting at normal and large text sizes");
        }
    }
    canvas.set_text_scale(1.0f);
}

void verify_fixed_probe_row(Canvas& canvas) {
    constexpr float left = 24, top = 24;
    const std::wstring metadata = L"校园直连 · Wi-Fi · 网络隧道 · 完整检测 12:34:56 · 下次检测 12:36";
    for (const float width : {480.0f, 500.0f, 560.0f, 640.0f, 760.0f, 960.0f}) {
        for (const auto& summary : {std::wstring(L"校园直连 · Wi-Fi"), metadata}) {
            Scene scene;
            Builder builder(scene, canvas, 0, left, width - left * 2, top);
            builder.selector_row("fixed.probe", summary, {L"快速检测", L"完整检测"}, 0, [](int) {}, L"检测说明", [] {});
            const auto* quick = scene.find("fixed.probe.0");
            const auto* complete = scene.find("fixed.probe.1");
            const auto* info = scene.find("fixed.probe.row.info");
            float metadata_bottom = top;
            for (const auto& widget : scene.widgets)
                if (widget.kind == Kind::text) metadata_bottom = std::max(metadata_bottom, widget.rect.bottom);
            check(quick && complete && info && std::abs(quick->rect.left - left - 4) < 0.1f &&
                std::abs(quick->rect.top - metadata_bottom - tokens::kSpaceS) < 0.1f &&
                std::abs(quick->rect.top - complete->rect.top) < 0.1f &&
                std::abs(quick->rect.bottom - complete->rect.bottom) < 0.1f &&
                complete->rect.right <= info->rect.left && info->rect.right <= width - left + 0.1f,
                "probe picker stays on a separate left-aligned row from compact to wide windows, regardless of metadata length");
        }
    }
}

void verify_comparison_help(FixtureHost& host, Canvas& canvas, ViewContext& context, float width, float height) {
    std::string opened;
    ViewContext help_context{context.app, context.ui, context.hwnd, context.refresh,
        [&](const std::string& name) { opened = name; }, context.close, context.copied};
    host.scene = Scene{};
    host.scene.layers[0].viewport = D2D1::RectF(0, 0, width, height);
    Builder builder(host.scene, canvas, 0, tokens::kSpaceL, width - 2 * tokens::kSpaceL, tokens::kSpaceL);
    build_network_comparison(builder, help_context, context.app.snapshot());
    host.scene.layers[0].content_height = builder.y() + tokens::kSpaceL;
    const auto* heading = host.scene.find("network.comparison.heading");
    const auto* info = host.scene.find("network.comparison.info");
    check(heading && info && info->enabled && info->focusable && info->action &&
        heading->rect.right <= info->rect.left + 0.1f && info->rect.right <= width - tokens::kSpaceL + 0.1f &&
        std::abs((heading->rect.top + heading->rect.bottom) - (info->rect.top + info->rect.bottom)) < 0.1f,
        "comparison explanation remains beside its heading without reducing either result column at narrow widths and large text sizes");
    verify_bounds(canvas, host.scene, width, height);
    Accessibility access(host);
    ComPtr<IRawElementProviderFragment> root;
    root.Attach(access.root_for_testing());
    const auto node = find_node(root.Get(), L"network.comparison.info");
    ComPtr<IInvokeProvider> invoke;
    if (node) node.As(&invoke);
    check(node && property(node.Get(), UIA_NamePropertyId) == info_label("networkComparison") &&
        invoke && SUCCEEDED(invoke->Invoke()) && opened == "info:networkComparison",
        "native comparison help invokes the real information-popover route with an accessible description");
    const auto explanation = T("info.networkComparison.body");
    check(!T("info.networkComparison.title").empty() && explanation.find(T("env.systemProbe")) != std::wstring::npos &&
        explanation.find(T("env.physicalProbe")) != std::wstring::npos && explanation.find(L"\n\n") != std::wstring::npos &&
        explanation.find(L"校园登录服务") != std::wstring::npos,
        "the invoked comparison popover resolves both labelled connection paths and separate campus-service checking in short paragraphs");
    access.disconnect();
}

void verify_full_history_ribbon(HWND window, Canvas& canvas) {
    for (const bool identical : {false, true}) {
        auto services = std::make_unique<wifiruntime::Fake>();
        auto* fake = services.get();
        fake->files[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"quick","appearanceMode":"system"})";
        auto stored = json::Value::object(), samples = json::Value::array();
        stored.set("schemaVersion", json::Value::number(1));
        const auto date = records::iso8601(AppController::wall_now() - 60);
        for (std::size_t index = 0; index < records::HealthLog::kLimit; ++index) {
            auto sample = json::Value::object();
            sample.set("date", json::Value::string(date));
            sample.set("online", json::Value::boolean(true));
            sample.set("mode", json::Value::string("complete"));
            sample.set("apple", json::Value::string("success"));
            sample.set("microsoft", json::Value::string("success"));
            sample.set("latencyMilliseconds", json::Value::number(identical || index % 3 == 0
                ? 42.1234567890123 : 70.2345678901234 + static_cast<double>(index) / 10000));
            sample.set("websiteTotal", json::Value::number(3));
            sample.set("websitePassed", json::Value::number(2));
            samples.push(std::move(sample));
        }
        stored.set("samples", std::move(samples));
        const auto original_health = json::serialize(stored);
        fake->files[L"health.json"] = original_health;
        AppController app(nullptr, RuntimeMode::observeOnly, std::move(services)); app.start();
        const auto original_files = fake->files;
        check(app.health().items().size() == 360, "real records view loads a full isolated history with repeated timestamps and exact duplicate samples");

        // Preserve the previous key calculation as an independent compatibility reference.
        std::vector<std::string> fingerprints, expected;
        std::size_t legacy_comparisons = 0;
        for (const auto& sample : app.health().items()) {
            auto fields = json::Value::array();
            fields.push(json::Value::number(sample.time));
            fields.push(json::Value::boolean(sample.online));
            fields.push(json::Value::string(sample.mode));
            fields.push(json::Value::string(sample.apple));
            fields.push(json::Value::string(sample.microsoft));
            fields.push(json::Value::number(sample.latency_ms));
            fields.push(json::Value::number(sample.site_total));
            fields.push(json::Value::number(sample.site_passed));
            const auto fingerprint = json::serialize(fields);
            legacy_comparisons += fingerprints.size();
            const auto ordinal = std::count(fingerprints.begin(), fingerprints.end(), fingerprint);
            fingerprints.push_back(fingerprint);
            expected.push_back("records.cell." + fingerprint + "." + std::to_string(ordinal));
        }
        expected.erase(expected.begin(), expected.end() - 24);
        check(legacy_comparisons == 64620 && expected.size() == 24 &&
            expected.front().ends_with(identical ? ".336" : ".112"),
            "legacy reference counts duplicates before the visible ribbon and retains their full-history ordinals");

        Scene scene; UiState ui; ui.page = Page::records;
        ViewContext context{app, ui, window, [] {}, [](const std::string&) {}, [] {}, [](const std::string&) {}};
        const auto build = [&](float width = 500) { build_scene(scene, canvas, context, width, 1200); };
        const auto keys = [&] {
            std::vector<std::string> result;
            for (const auto& widget : scene.widgets)
                if (widget.id.starts_with("records.cell.")) result.push_back(widget.id);
            return result;
        };
        const auto selected = [&](const std::string& key) {
            int count = 0;
            for (const auto& widget : scene.widgets) {
                if (!widget.id.starts_with("records.cell.") || !widget.checked) continue;
                if (widget.id != key) return false;
                ++count;
            }
            return ui.selected_sample_key == key && count == (key.empty() ? 0 : 1) &&
                (scene.find("records.detail.close") != nullptr) == !key.empty();
        };
        const auto click = [&](const std::string& key) {
            const auto* widget = scene.find(key);
            if (!widget || !widget->action) return false;
            const auto action = widget->action; action(); build(); return true;
        };
        build();
        check(keys() == expected, "linear duplicate counting produces the same 24 ordered ribbon keys and double precision as the previous algorithm");
        check(click(expected.front()) && selected(expected.front()), "the first visible repeated sample selects its own full-history identity");
        build(560);
        check(keys() == expected && selected(expected.front()), "resizing and rebuilding retain the selected sample and every ribbon identity");
        app.set_probe(ProbeMode::complete); app.on_changed(); build();
        check(keys() == expected && selected(expected.front()), "an unrelated controller update cannot change historical ribbon keys or selection");
        check(click(expected.back()) && selected(expected.back()), "another identical or same-time sample replaces the previous selection exclusively");
        check(click(expected.back()) && selected({}), "clicking the selected repeated sample clears its single detail panel");
        ui.selected_sample_key = expected.front() + ".missing"; build();
        check(keys() == expected && selected({}), "a stale selection is cleared without assigning it to a different repeated sample");
        app.shutdown();
        check(fake->files == original_files && fake->files[L"health.json"] == original_health &&
            fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 &&
            fake->evidence().production_storage_accesses == 0 && fake->evidence().fake_probe_calls == 0 &&
            fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0,
            "full-history ribbon compatibility uses injected data only and preserves the original health-log bytes and all files");
    }
}

void verify_automatic_entry_feedback(HWND window, Canvas& canvas) {
    auto services = std::make_unique<r11presentationruntime::Fake>("manual", CampusArea::automatic, true);
    auto* fake = services.get();
    AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
    const auto files = fake->files;
    check(app.verify_campus_service(), "automatic-entry feedback fixture starts an isolated service-only verification");
    wifiruntime::wait_for([&] { app.on_changed(); return !app.snapshot().busy &&
        app.memory_verification_message() == "campus.verify.confirmed"; });
    const auto state = app.snapshot();
    FixtureHost host; host.window = window;
    UiState ui;
    ViewContext context{app, ui, window, [] {}, [](const std::string&) {}, [] {}, [](const std::string&) {}};
    build_guard(host, canvas, context, 500, 1600);
    const auto* feedback = host.scene.find("guard.memory.feedback");
    check(state.area == CampusArea::automatic && state.needs_selection && state.required_campus_confirmed && state.last_discovery &&
        state.last_discovery->teaching == Discovery::confirmed && state.last_discovery->dormitory == Discovery::confirmed &&
        feedback && feedback->text == T("campus.verify.confirmed") && !host.scene.find("campus.entries.selection") && !host.scene.find("guard.area"),
        "freshly confirmed dual services retain successful verification feedback without prompting an automatic-mode user to choose a protocol");
    check(fake->files == files && fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0 &&
        app.settings().area == CampusArea::automatic,
        "simplified service feedback changes no saved entry preference, network memory or authentication behavior");
    app.shutdown();
}

}  // namespace
#include "r11_presentation_native_checks.hpp"
#include "automatic_campus_presentation_native_checks.hpp"
#include "hero_website_chip_native_checks.hpp"
namespace {

#include "proxy_assistant_view_checks.hpp"
#include "wifi_zero_selection_view_checks.hpp"
#include "memory_name_native_checks.hpp"
#include "forget_all_confirmation_native_checks.hpp"
#include "home_disclosure_native_checks.hpp"
#include "license_group_native_checks.hpp"
#include "record_feedback_native_checks.hpp"
#include "history_detail_native_checks.hpp"

void exercise_home(HWND window, Canvas& canvas, float dpi, float width, float text_scale, RuntimeMode mode) {
    constexpr float height = 1200;
    const auto pixel_width = static_cast<UINT>(std::ceil(width * dpi / 96));
    const auto pixel_height = static_cast<UINT>(std::ceil(height * dpi / 96));
    SetWindowPos(window, nullptr, 0, 0, static_cast<int>(pixel_width), static_cast<int>(pixel_height), SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
    canvas.set_dpi(dpi); canvas.resize(pixel_width, pixel_height); canvas.set_text_scale(text_scale);
    verify_home_disclosures(window, canvas, width, height, mode);
}

void exercise(HWND window, Canvas& canvas, float dpi, float width, float text_scale, RuntimeMode mode) {
    constexpr float height = 1200;
    const auto pixel_width = static_cast<UINT>(std::ceil(width * dpi / 96));
    const auto pixel_height = static_cast<UINT>(std::ceil(height * dpi / 96));
    SetWindowPos(window, nullptr, 0, 0, static_cast<int>(pixel_width), static_cast<int>(pixel_height), SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
    canvas.set_dpi(dpi); canvas.resize(pixel_width, pixel_height); canvas.set_text_scale(text_scale);
    if ((dpi == 120 && width == 480 && text_scale == 1) || (dpi == 192 && width == 500 && text_scale == 1.5f))
        verify_help_quick_start_disclosure(window, canvas, width, height, mode);
    verify_connection_details_and_website_scope(window, canvas, width, height, mode);
    verify_proxy_assistant_view(window, canvas, width, height, mode);
    verify_wifi_zero_selection_view(window, canvas, width, height, mode);
    verify_forget_all_confirmation(window, canvas, width, height, mode);
    verify_license_group(window, canvas, width, height, mode);
    verify_record_feedback_explanations(window, canvas, width, height, mode);
    if (mode == RuntimeMode::normal && dpi == 192 && width == 480 && text_scale == 1.5f)
        verify_record_export_interaction(window, canvas, width, height);
    verify_historical_probe_timing(window, canvas, width, height, mode);
    if (mode == RuntimeMode::normal) {
        verify_same_name_memory_settings(window, canvas, width, height);
        exercise_narrow_layout(canvas, width);
        verify_r11_basic_probe_detail(canvas, width);
        verify_automatic_campus_presentation(window, canvas, width);
        if (width == 500) verify_fixed_probe_row(canvas);
    }
    auto services = std::make_unique<physicaldiagnosticsruntime::Fake>(); auto* fake = services.get();
    AppController app(nullptr, mode, std::move(services)); app.start();
    const auto before = app.snapshot(); const auto due = app.next_check(); const auto files = fake->files;
    const int credentials = fake->credential_reads, captures = fake->captures;
    FixtureHost host; host.window = window; host.scale = dpi / 96;
    UiState ui;
    int refreshes = 0, overlays = 0, copied = 0;
    ViewContext context{app, ui, window, [&] { ++refreshes; }, [&](const std::string&) { ++overlays; },
        [&] { ++overlays; }, [&](const std::string&) { ++copied; }};
    if (width == 500) verify_header_brand(canvas, context);
    build_guard(host, canvas, context, width, height);
    const auto* reason = host.scene.find("network.identification.reason");
    const auto* refresh = host.scene.find("network.identification.refresh");
    const auto* copy = host.scene.find("network.identification.copy");
    check(reason && reason->text == T("env.identification.reason.missingDNS") && reason->role == Role::status &&
        refresh && refresh->enabled && refresh->action && copy && copy->enabled && copy->action &&
        copy->text == T("env.identification.copy"), "collected physical rejection produces real visible reason and enabled refresh/copy controls");
    check(!host.scene.find("guard.remember") && !host.scene.find("guard.once") && !host.scene.find("campus.verify"),
        "an unidentified physical connection exposes no remember, trust or campus-verification action");
    const auto summary = app.diagnostic_summary_text(); const auto report = json::parse(app.diagnostic_export_text());
    check(report && report->find("physicalNetwork") && report->find("physicalNetwork")->number_at("physicalInterfaceCount") == 1 &&
        report->find("physicalNetwork")->number_at("candidateCount") == 0 &&
        report->find("physicalNetwork")->find("rejections")->number_at("missingDNS") == 1 &&
        summary.find(U("env.identification.reason.missingDNS")) != std::string::npos &&
        summary.find("physical interfaces=1 candidates=0 reasons=missingDNS=1") != std::string::npos,
        "actual rendered reason agrees with the clipboard source and exported JSON from the same captured snapshot");
    verify_bounds(canvas, host.scene, width, height);
    canvas.reset_drawn_text_layouts_for_testing();
    check(canvas.begin(), "begin native diagnostic view paint on a hidden HWND");
    paint(canvas, make_palette(false, false), host.scene, {}, width, height, 0);
    check(canvas.end() && canvas.drawn_text_layouts_for_testing() >= 4, "Direct2D and DirectWrite paint the real built diagnostic widgets");

    Accessibility access(host);
    ComPtr<IRawElementProviderFragment> root;
    root.Attach(access.root_for_testing());
    const auto reason_node = find_node(root.Get(), L"network.identification.reason");
    const auto refresh_node = find_node(root.Get(), L"network.identification.refresh");
    const auto copy_node = find_node(root.Get(), L"network.identification.copy");
    check(reason_node && property(reason_node.Get(), UIA_NamePropertyId) == T("env.identification.reason.missingDNS") &&
        refresh_node && property(refresh_node.Get(), UIA_NamePropertyId) == T("env.identification.refresh") &&
        copy_node && property(copy_node.Get(), UIA_NamePropertyId) == T("env.identification.copy"),
        "native UIA exposes the same diagnostic reason and action names as the painted view");
    physical::CandidateDiagnostics next; next.interface_count = 3; next.reject(physical::CandidateRejection::missingDefaultRoute, 2);
    fake->supplied = next;
    ComPtr<IInvokeProvider> invoke;
    if (refresh_node) refresh_node.As(&invoke);
    check(invoke && SUCCEEDED(invoke->Invoke()) && fake->diagnostic_captures == 1 && refreshes == 1,
        "native UIA invokes the actual diagnostic refresh action exactly once");
    access.disconnect();
    build_guard(host, canvas, context, width, height);
    reason = host.scene.find("network.identification.reason");
    const auto* feedback = host.scene.find("network.identification.feedback");
    const auto updated = json::parse(app.diagnostic_export_text());
    check(reason && reason->text == T("env.identification.reason.missingDefaultRoute") &&
        feedback && feedback->text == T("env.identification.refreshed") && updated && updated->find("physicalNetwork") &&
        updated->find("physicalNetwork")->number_at("physicalInterfaceCount") == 3 &&
        updated->find("physicalNetwork")->find("rejections")->number_at("missingDefaultRoute") == 2 &&
        app.diagnostic_summary_text().find("physical interfaces=3 candidates=0 reasons=missingDefaultRoute=2") != std::string::npos,
        "refresh rebuilds visible reason and feedback while copied text and JSON adopt the same new anonymous counts");
    verify_bounds(canvas, host.scene, width, height);
    canvas.reset_drawn_text_layouts_for_testing();
    check(canvas.begin(), "begin native refreshed diagnostic paint");
    paint(canvas, make_palette(false, false), host.scene, {}, width, height, 0);
    check(canvas.end() && canvas.drawn_text_layouts_for_testing() >= 4, "refreshed diagnostic information is painted using the actual view builder");
    const auto after = app.snapshot();
    check(fake->captures == captures && fake->credential_reads == credentials && fake->files == files && app.next_check() == due &&
        after.epoch == before.epoch && after.generation == before.generation && after.trusted == before.trusted &&
        after.remembered == before.remembered && after.memory_count == before.memory_count && after.paused == before.paused &&
        after.holds.size() == before.holds.size() && fake->probes == 0 && fake->system_probes == 0 && fake->discoveries == 0 &&
        fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0 && overlays == 0 && copied == 0,
        "paint, UIA and diagnostic refresh never login, probe, grant trust, change memory/holds/schedule, open overlays or access the clipboard");
    verify_comparison_help(host, canvas, context, width, height);
    check(fake->files == files && app.next_check() == due && fake->probes == 0 && fake->system_probes == 0 && fake->discoveries == 0 &&
        fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_wifi_calls == 0,
        "opening comparison explanations never starts a check, changes settings or grants authentication authority");
    host.scene = Scene{};
    host.scene.layers[0].viewport = D2D1::RectF(0, 0, width, height);
    Builder details(host.scene, canvas, 0, tokens::kSpaceL, width - 2 * tokens::kSpaceL, tokens::kSpaceL);
    build_network_details(details, context, app.snapshot());
    host.scene.layers[0].content_height = details.y() + tokens::kSpaceL;
    const auto* campus = host.scene.find("network.environment.campusService");
    check(campus && campus->text == T("env.connectionDetails.notChecked") &&
        !host.scene.find("network.environment.interfaces") && !host.scene.find("network.environment.authenticationTarget") &&
        !host.scene.find("network.environment.systemPrimary") && !host.scene.find("network.technical"),
        "unidentified connections still show campus-service status while unavailable connection names produce no empty rows or advanced disclosure");
    verify_bounds(canvas, host.scene, width, height);
    app.shutdown();
    check(fake->evidence().network_calls == 0 && fake->evidence().vault_accesses == 0 && fake->evidence().production_storage_accesses == 0,
        "native diagnostic UI regression uses no production network, credential or storage services");
}
}

int main(int argc, char** argv) {
    if (argc > 2) return 2;
    const std::string scope = argc == 1 ? "all" : argv[1];
    const bool home_only = scope == "--home-view-only" || scope == "--normal-home-view-only" || scope == "--observe-home-view-only";
    if (argc == 2 && scope != "--paragraph-layout-only" && scope != "--view-only" &&
        scope != "--normal-view-only" && scope != "--observe-view-only" && scope != "--hero-website-chips-only" && !home_only) return 2;
    const auto started = std::chrono::steady_clock::now();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Isolated physical diagnostics view", WS_POPUP,
        0, 0, 1000, 1200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "create hidden native diagnostic view window");
    if (window) {
        Canvas canvas;
        const bool ready = canvas.init() && canvas.attach(window, 96);
        check(ready, "initialize native Direct2D and DirectWrite diagnostic canvas");
        if (ready && (scope == "all" || scope == "--paragraph-layout-only")) {
            verify_layout_cache(canvas);
            verify_continuous_paragraph_widths(canvas);
        }
        if (ready && scope != "--paragraph-layout-only" && scope != "--observe-view-only" && scope != "--hero-website-chips-only" && !home_only) {
            verify_full_history_ribbon(window, canvas);
            verify_automatic_entry_feedback(window, canvas);
            verify_r11_memory_actions(window, canvas);
            verify_proxy_assistant_recommendation_view(window, canvas);
        }
        if (ready && scope != "--paragraph-layout-only") for (const auto mode : {RuntimeMode::normal, RuntimeMode::observeOnly}) {
            if (((scope == "--normal-view-only" || scope == "--normal-home-view-only") && mode != RuntimeMode::normal) ||
                ((scope == "--observe-view-only" || scope == "--observe-home-view-only") && mode != RuntimeMode::observeOnly)) continue;
            if (scope == "all" || scope == "--hero-website-chips-only")
                verify_hero_website_chip_matrix(window, canvas, mode, true);
            if (scope == "--hero-website-chips-only") continue;
            for (const float dpi : {120.0f, 144.0f, 168.0f, 192.0f})
                for (const float width : {480.0f, 500.0f})
                    for (const float text_scale : {1.0f, 1.5f}) {
                        if (!home_only) exercise(window, canvas, dpi, width, text_scale, mode);
                        if (home_only || scope == "all" || scope == "--view-only")
                            exercise_home(window, canvas, dpi, width, text_scale, mode);
                    }
        }
        DestroyWindow(window);
    }
    CoUninitialize();
    std::printf("native physical diagnostics view: %d checks, %d failures; fake double-snapshot inputs only\n", checks, failures);
    std::printf("scope=%s elapsed=%.3f seconds\n", scope.c_str(),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    return failures ? 1 : 0;
}
