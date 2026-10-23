#pragma once
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

void verify_same_name_memory_settings(HWND window, Canvas& canvas, float width, float height) {
    auto services = std::make_unique<memorymergeruntime::NameDisplayFake>(); auto* fake = services.get();
    AppController app(nullptr, RuntimeMode::normal, std::move(services)); app.start();
    const auto files = fake->files;
    const auto before = app.snapshot(); const auto scheduled = app.next_check();
    FixtureHost host; host.window = window; host.scale = canvas.dpi() / 96;
    UiState ui; ui.page = Page::settings;
    ViewContext context{app, ui, window, [] {}, [](const std::string&) {}, [] {}, [](const std::string&) {}};
    build_scene(host.scene, canvas, context, width, height);
    const auto count_prefix = [&](const std::string& prefix) {
        return std::count_if(host.scene.widgets.begin(), host.scene.widgets.end(), [&](const Widget& item) {
            return item.id.rfind(prefix, 0) == 0;
        });
    };
    const auto scope_fits = [&] {
        const auto* scope = host.scene.find("settings.memory.networkScopeDetail");
        if (!scope || scope->text != T("memory.networkScopeDetail") ||
            scope->text != L"校园不同区域的 Wi-Fi 可能同名。\n\n连接到不同网络时，需要分别核验并记住。" ||
            count_prefix("settings.memory.networkScopeDetail") != 1) return false;
        const auto& rect = scope->rect;
        if (rect.left < -0.1f || rect.right > width + 0.1f || rect.right <= rect.left ||
            canvas.measure(scope->text, scope->style, rect.right - rect.left).height > rect.bottom - rect.top + 0.2f) return false;
        std::size_t start = 0;
        int paragraphs = 0;
        for (;;) {
            const auto end = scope->text.find(L"\n\n", start);
            const auto paragraph = scope->text.substr(start, end == std::wstring::npos ? end : end - start);
            if (paragraph.empty() || canvas.inspect_text_layout_for_testing(paragraph, scope->style,
                    rect.right - rect.left).lines > 2) return false;
            ++paragraphs;
            if (end == std::wstring::npos) break;
            start = end + 2;
        }
        return paragraphs == 2;
    };
    check(count_prefix("memory.name.") == 2 && count_prefix("memory.sameNameDetail.") == 1 &&
        count_prefix("wifi.sameNameDetail.") == 1 && scope_fits(),
        "saved-network settings show one fixed two-paragraph scope explanation, separate records and one short same-name notice per group");
    Accessibility access(host); ComPtr<IRawElementProviderFragment> root;
    root.Attach(access.root_for_testing());
    const auto scope_node = find_node(root.Get(), L"settings.memory.networkScopeDetail");
    check(scope_node && property(scope_node.Get(), UIA_NamePropertyId) == T("memory.networkScopeDetail"),
        "native accessibility exposes the fixed network-scope explanation exactly as displayed");
    std::vector<std::string> visible_fields{"settings.memory.networkScopeDetail"};
    for (const auto* raw_id : {"11111111", "22222222"}) {
        const std::string id = raw_id;
        const bool current = id == "11111111";
        const auto* name = host.scene.find("memory.name." + id);
        const auto* detail = host.scene.find("memory.detail." + id);
        const auto* manage = host.scene.find("memory.manage." + id);
        const auto* target = host.scene.find("wifi.network." + id);
        check(name && name->text == L"Fixture campus" && name->role == Role::text && detail &&
            (detail->text.find(T("memory.currentConnection")) != std::wstring::npos) == current &&
            detail->text.find(T(current ? "memory.entry.teaching" : "memory.entry.dormitory")) != std::wstring::npos &&
            detail->text.find(L'\n') != std::wstring::npos && manage && manage->enabled && manage->action &&
            manage->text == T("memory.manage") && manage->name == T("memory.manage") + L"：Fixture campus",
            "each original network name keeps its own entry details and ID-specific management control, with only its actual attachment marked current");
        check(target && target->text == L"Fixture campus" && target->checked == current &&
            target->detail == (current ? T("memory.currentConnection") : L"") && target->enabled && target->action,
            "same-name finding targets retain individual checked states and show current connection only beside the physical match");
        const auto name_node = find_node(root.Get(), win::widen("memory.name." + id));
        const auto detail_node = find_node(root.Get(), win::widen("memory.detail." + id));
        const auto manage_node = find_node(root.Get(), win::widen("memory.manage." + id));
        const auto target_node = find_node(root.Get(), win::widen("wifi.network." + id));
        ComPtr<IRawElementProviderSimple> manage_simple;
        ComPtr<IUnknown> invocation;
        if (manage_node) manage_node.As(&manage_simple);
        if (manage_simple) manage_simple->GetPatternProvider(UIA_InvokePatternId, invocation.GetAddressOf());
        ComPtr<IToggleProvider> toggle;
        if (target_node) target_node.As(&toggle);
        ToggleState selected = ToggleState_Indeterminate;
        const bool toggle_ready = toggle && SUCCEEDED(toggle->get_ToggleState(&selected));
        check(name && detail && manage && target && name_node && detail_node && manage_node && target_node && invocation &&
            property(name_node.Get(), UIA_NamePropertyId) == name->text &&
            property(detail_node.Get(), UIA_NamePropertyId) == detail->text &&
            property(manage_node.Get(), UIA_NamePropertyId) == manage->name &&
            property(target_node.Get(), UIA_NamePropertyId) == target->text &&
            property(target_node.Get(), UIA_HelpTextPropertyId) == target->detail && toggle_ready &&
            selected == (current ? ToggleState_On : ToggleState_Off),
            "native UIA exposes original names, displayed details, separate management IDs and unchanged finding selections without invoking a menu or toggle");
        for (const auto* prefix : {"memory.name.", "memory.detail.", "memory.manage.", "wifi.network."})
            visible_fields.push_back(std::string(prefix) + id);
        if (name && detail && manage) {
            const auto disjoint = [](const D2D1_RECT_F& left, const D2D1_RECT_F& right) {
                return left.right <= right.left + 0.1f || right.right <= left.left + 0.1f ||
                    left.bottom <= right.top + 0.1f || right.bottom <= left.top + 0.1f;
            };
            check(name->rect.bottom <= detail->rect.top + 0.1f && disjoint(name->rect, manage->rect) &&
                disjoint(detail->rect, manage->rect),
                "saved-network name, metadata and management target never overlap when the settings row wraps at narrow widths");
        }
    }
    for (const auto& item : host.scene.widgets)
        if (item.id.rfind("memory.sameNameDetail.", 0) == 0 || item.id.rfind("wifi.sameNameDetail.", 0) == 0) {
            visible_fields.push_back(item.id);
            const auto node = find_node(root.Get(), win::widen(item.id));
            check(item.text == T("memory.sameNameDetail") && node && property(node.Get(), UIA_NamePropertyId) == item.text,
                "the single same-name explanation in each section has the same rendered and accessible wording");
        }
    bool geometry = true, complete_text = true, reachable = true;
    for (const auto& id : visible_fields) {
        const auto* item = host.scene.find(id);
        if (!item) { geometry = complete_text = reachable = false; continue; }
        const auto& rect = item->rect;
        geometry = geometry && std::isfinite(rect.left) && std::isfinite(rect.right) && std::isfinite(rect.top) &&
            std::isfinite(rect.bottom) && rect.left >= -0.1f && rect.right <= width + 0.1f &&
            rect.top >= 0 && rect.bottom <= host.scene.layers[0].content_height + 0.1f &&
            rect.right > rect.left && rect.bottom > rect.top;
        float text_width = rect.right - rect.left;
        float text_height = canvas.measure(item->text, item->style, text_width).height;
        if (item->kind == Kind::button) {
            text_width -= 30;
            text_height = canvas.measure(item->text, TextStyle::callout, text_width).height + 12;
        } else if (item->kind == Kind::toggle) {
            text_width -= 64;
            text_height = canvas.measure(item->text, item->style, text_width).height + 16;
            if (!item->detail.empty()) text_height += canvas.measure(item->detail, TextStyle::caption, text_width).height + 2;
        }
        complete_text = complete_text && text_width > 0 && text_height <= rect.bottom - rect.top + 0.2f;
        if (item->focusable) geometry = geometry && rect.right - rect.left >= tokens::kSizeMinTarget - 0.1f &&
            rect.bottom - rect.top >= tokens::kSizeMinTarget - 0.1f;
        auto& layer = host.scene.layers[0];
        layer.scroll = std::max(0.0f, rect.top - layer.viewport.top - 8);
        host.scene.clamp_scroll(0);
        const auto visible = host.scene.visible_rect(*item);
        reachable = reachable && visible.top >= layer.viewport.top - 0.1f &&
            visible.bottom <= layer.viewport.bottom + 0.1f;
    }
    check(geometry && complete_text && reachable,
        "all saved-name and finding fields fit their actual DirectWrite measure and remain fully reachable at the existing narrow DPI and text sizes");
    for (const auto* anchor : {"memory.name.11111111", "wifi.network.11111111"}) {
        const auto* first = host.scene.find(anchor);
        if (first) host.scene.layers[0].scroll = std::max(0.0f, first->rect.top - host.scene.layers[0].viewport.top - 8);
        host.scene.clamp_scroll(0);
        const bool painting = canvas.begin();
        check(painting, "begin native saved-network or finding-target settings paint");
        if (painting) {
            paint(canvas, make_palette(false, false), host.scene, {}, width, height, 0);
            check(canvas.end(), "saved-network and finding-target rows paint through native Direct2D and DirectWrite at the measured narrow layout");
        }
    }
    access.disconnect();
    const auto after = app.snapshot();
    check(fake->files == files && app.next_check() == scheduled && after.epoch == before.epoch &&
        after.trusted == before.trusted && after.remembered == before.remembered && !app.wifi_selected("22222222") &&
        app.current_network_record_id() == std::optional<std::string>("11111111") &&
        fake->evidence().fake_auth_submissions == 0 && fake->evidence().fake_probe_calls == 0 &&
        fake->evidence().fake_wifi_calls == 0 && fake->evidence().network_calls == 0 &&
        fake->evidence().vault_accesses == 0 && fake->evidence().production_storage_accesses == 0,
        "painting, scrolling and native accessibility retain both same-name memories, proof bytes, trust and excluded Wi-Fi without recovery work");
    app.shutdown();

    auto empty_services = std::make_unique<memorymergeruntime::NameDisplayFake>(); auto* empty = empty_services.get();
    empty->files.erase(L"network-memory.json"); empty->files.erase(L"network-labels.json");
    AppController empty_app(nullptr, RuntimeMode::normal, std::move(empty_services)); empty_app.start();
    const auto empty_files = empty->files;
    const auto empty_before = empty_app.snapshot(); const auto empty_due = empty_app.next_check();
    UiState empty_ui; empty_ui.page = Page::settings;
    ViewContext empty_context{empty_app, empty_ui, window, [] {}, [](const std::string&) {}, [] {}, [](const std::string&) {}};
    build_scene(host.scene, canvas, empty_context, width, height);
    check(scope_fits() && count_prefix("memory.name.") == 0 && count_prefix("memory.sameNameDetail.") == 0 &&
        std::count_if(host.scene.widgets.begin(), host.scene.widgets.end(), [](const Widget& item) {
            return item.kind == Kind::text && item.text == T("memory.none");
        }) == 1,
        "first-use settings keep the same concise scope explanation visible before any campus network has been remembered");
    Accessibility empty_access(host); ComPtr<IRawElementProviderFragment> empty_root;
    empty_root.Attach(empty_access.root_for_testing());
    const auto empty_scope = find_node(empty_root.Get(), L"settings.memory.networkScopeDetail");
    check(empty_scope && property(empty_scope.Get(), UIA_NamePropertyId) == T("memory.networkScopeDetail"),
        "first-use native accessibility includes the scope explanation without requiring duplicate saved names");
    empty_access.disconnect();
    const auto empty_after = empty_app.snapshot();
    check(empty->files == empty_files && empty_app.remembered_networks().empty() && !empty_app.current_network_record_id() &&
        empty_app.next_check() == empty_due && empty_after.epoch == empty_before.epoch &&
        empty_after.trusted == empty_before.trusted && empty_after.memory_count == 0 &&
        empty->evidence().fake_auth_submissions == 0 && empty->evidence().fake_probe_calls == 0 &&
        empty->evidence().fake_wifi_calls == 0 && empty->evidence().network_calls == 0 &&
        empty->evidence().vault_accesses == 0 && empty->evidence().production_storage_accesses == 0,
        "showing first-use network scope neither saves a memory nor grants trust, changes selections, schedules work or uses formal services");
    empty_app.shutdown();
}
