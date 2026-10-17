// Actual COM providers and DirectWrite painting with fake records only.
// No controller, runtime, storage, credentials, network or WLAN services are linked.
#include "accessibility.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <wrl/client.h>

using namespace szu::app;
using Microsoft::WRL::ComPtr;
namespace {
int checks = 0, failures = 0;
void check(bool passed, const char* message) {
    ++checks;
    if (!passed) { ++failures; std::printf("FAIL: %s\n", message); }
}
class FixtureHost final : public AccessHost {
public:
    HWND window = nullptr;
    Scene scene;
    std::string focused;
    HWND host_window() const override { return window; }
    const Scene& host_scene() const override { return scene; }
    float host_scale() const override { return 1; }
    std::string host_focus() const override { return focused; }
    void host_set_focus(const std::string& id) override {
        const Widget* widget = scene.find(id);
        if (widget && widget->layer == scene.top_layer() && widget->enabled && widget->focusable) focused = id;
    }
    void host_activate(const std::string& id) override {
        const Widget* widget = scene.find(id);
        if (!widget || widget->layer != scene.top_layer() || !widget->enabled || !widget->action) return;
        const auto action = widget->action;
        action();
    }
    void key_down(int key) {
        if (key == VK_RETURN || key == VK_SPACE) host_activate(focused);
    }
};
bool property_is(IRawElementProviderFragment* node, PROPERTYID property, const std::wstring& expected) {
    ComPtr<IRawElementProviderSimple> simple;
    if (!node || FAILED(node->QueryInterface(IID_PPV_ARGS(simple.GetAddressOf())))) return false;
    VARIANT value{};
    const HRESULT result = simple->GetPropertyValue(property, &value);
    const bool equal = SUCCEEDED(result) && value.vt == VT_BSTR && value.bstrVal &&
        std::wstring(value.bstrVal, SysStringLen(value.bstrVal)) == expected;
    VariantClear(&value);
    return equal;
}
void nested_modal_actions(HWND window) {
    Scene ordering;
    ordering.layers.resize(3);
    Widget upper; upper.id = "sheet.close"; upper.layer = 2;
    Widget lower = upper; lower.layer = 1;
    ordering.widgets = {upper, lower};
    check(ordering.find("sheet.close") == &ordering.widgets[0],
        "same-id lookup prefers the highest modal layer even if a lower layer was appended later");
    ordering.widgets.push_back(upper);
    check(ordering.find("sheet.close") == &ordering.widgets.back(),
        "same-layer lookup prefers the last painted same-id widget");
    check(!ordering.find("") && !ordering.find("missing"), "empty and missing modal ids do not resolve");

    // Exercise mouse, both keyboard activation keys and actual COM invocation.
    for (int route : {0, 1, 2, 3}) {
        FixtureHost host; host.window = window;
        host.scene.layers.resize(3);
        host.scene.layers[1].modal = host.scene.layers[2].modal = true;
        int parent_closed = 0, child_closed = 0;
        auto close_layer = [&](int layer) {
            if (layer == 2) ++child_closed; else ++parent_closed;
            std::erase_if(host.scene.widgets, [=](const Widget& widget) { return widget.layer == layer; });
            host.scene.layers.pop_back();
            host.focused.clear();
        };
        for (int layer : {1, 2}) {
            Widget close;
            close.id = "sheet.close"; close.kind = Kind::button; close.role = Role::button;
            close.name = layer == 2 ? L"Close account deletion" : L"Close about";
            close.layer = layer; close.focusable = true; close.scrolls = false;
            close.rect = D2D1::RectF(340, 12, 384, 56);
            close.action = [&, layer] { close_layer(layer); };
            host.scene.widgets.push_back(close);
        }
        Accessibility access(host);
        ComPtr<IRawElementProviderFragment> root;
        root.Attach(access.root_for_testing());
        ComPtr<IRawElementProviderFragment> child;
        const HRESULT child_result = root->Navigate(NavigateDirection_FirstChild, child.GetAddressOf());
        ComPtr<IInvokeProvider> child_invoke;
        if (child) child.As(&child_invoke);
        check(SUCCEEDED(child_result) && child_invoke &&
            property_is(child.Get(), UIA_NamePropertyId, L"Close account deletion"),
            "UIA exposes the child close action rather than the obscured parent action");

        host.scene.widgets[1].enabled = false;
        const Widget* disabled = host.scene.find("sheet.close");
        host.host_set_focus("sheet.close");
        host.host_activate("sheet.close");
        check(disabled && disabled->layer == 2 && !disabled->enabled && !host.scene.hit(362, 34) &&
            host.scene.focus_order().empty() && host.host_focus().empty() && parent_closed == 0 && child_closed == 0,
            "disabled child close never falls through to an enabled parent by lookup, mouse, focus or activation");
        check(child_invoke && child_invoke->Invoke() == static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED) &&
            parent_closed == 0 && child_closed == 0,
            "UIA reports disabled child close without invoking the parent");

        host.scene.widgets[1].enabled = true;
        const Widget* hit = host.scene.hit(362, 34);
        const auto focus = host.scene.focus_order();
        check(hit && hit->layer == 2 && host.scene.find("sheet.close") == hit &&
            focus.size() == 1 && focus.front() == hit,
            "nested close lookup, mouse hit and keyboard order identify the same top-layer widget");
        if (route == 0) {
            if (hit) { const std::string id = hit->id; host.host_activate(id); }
        } else if (route == 1 || route == 2) {
            if (!focus.empty()) host.host_set_focus(focus.front()->id);
            check(host.host_focus() == "sheet.close", "keyboard can focus the nested close control");
            host.key_down(route == 1 ? VK_RETURN : VK_SPACE);
        } else {
            check(child && SUCCEEDED(child->SetFocus()) && host.host_focus() == "sheet.close",
                "native UIA focus routes to the nested close control");
            check(child_invoke && SUCCEEDED(child_invoke->Invoke()), "native UIA invokes the nested close control");
        }
        check(child_closed == 1 && parent_closed == 0 && host.scene.top_layer() == 1,
            "nested close removes only the child and leaves the parent open");
        const Widget* parent = host.scene.find("sheet.close");
        check(parent && parent->layer == 1 && host.scene.hit(362, 34) == parent,
            "parent close is available again after the child is removed");
        ComPtr<IRawElementProviderFragment> parent_node;
        const HRESULT parent_result = root->Navigate(NavigateDirection_FirstChild, parent_node.GetAddressOf());
        ComPtr<IInvokeProvider> parent_invoke;
        if (parent_node) parent_node.As(&parent_invoke);
        check(SUCCEEDED(parent_result) && parent_invoke &&
            property_is(parent_node.Get(), UIA_NamePropertyId, L"Close about") && SUCCEEDED(parent_invoke->Invoke()) &&
            parent_closed == 1 && child_closed == 1 && host.scene.top_layer() == 0,
            "fresh native UIA invocation can close the parent after closing its child");
        access.disconnect();
    }
}
void provider_history(HWND window) {
    constexpr std::size_t count = 1100;
    FixtureHost host; host.window = window;
    for (std::size_t i = 0; i < count; ++i) {
        Widget row; row.kind = Kind::text; row.role = Role::text;
        row.text = row.name = L"Offline history row " + std::to_wstring(i);
        row.rect = D2D1::RectF(12, static_cast<float>(i * 26), 300, static_cast<float>(i * 26 + 24));
        if (i % 10 == 0) row.id = "fixture.row." + std::to_string(i);
        host.scene.widgets.push_back(std::move(row));
        if (i % 17 == 0) { Widget decoration; decoration.kind = Kind::panel; host.scene.widgets.push_back(std::move(decoration)); }
    }
    Accessibility access(host);
    ComPtr<IRawElementProviderFragment> root;
    root.Attach(access.root_for_testing());
    ComPtr<IRawElementProviderFragment> retained;
    reset_accessible_list_builds_for_testing();
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(3);
    bool properties = true, boundaries = true;
    std::size_t visited = 0;
    for (bool forward : {true, false}) {
        ComPtr<IRawElementProviderFragment> current;
        properties &= SUCCEEDED(root->Navigate(forward ? NavigateDirection_FirstChild : NavigateDirection_LastChild, current.GetAddressOf()));
        std::size_t step = 0;
        while (current && step < count && std::chrono::steady_clock::now() < deadline) {
            const auto index = forward ? step : count - step - 1;
            const auto id = index % 10 == 0 ? L"fixture.row." + std::to_wstring(index) : L"auto." + std::to_wstring(index);
            properties &= property_is(current.Get(), UIA_NamePropertyId, L"Offline history row " + std::to_wstring(index)) &&
                property_is(current.Get(), UIA_AutomationIdPropertyId, id);
            if (forward && index == 0) retained = current;
            ComPtr<IRawElementProviderFragment> next;
            properties &= SUCCEEDED(current->Navigate(forward ? NavigateDirection_NextSibling : NavigateDirection_PreviousSibling, next.GetAddressOf()));
            current = std::move(next); ++step; ++visited;
        }
        boundaries &= step == count && !current;
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    const auto enumerations = accessible_list_builds_for_testing();
    std::printf("fake record UIA: %zu visits, %zu accessible-list builds, %lld ms\n", visited, enumerations, static_cast<long long>(elapsed));
    check(properties && boundaries && visited == count * 2, "all mixed explicit/idless text nodes retain order, properties and empty ends in both directions");
    check(elapsed < 3000 && enumerations <= count * 10, "large native record tree avoids nested full-list enumeration within three seconds");
    check(host.scene.widgets.size() > count, "UIA traversal never removes records or decoration widgets");
    std::erase_if(host.scene.widgets, [](const Widget& w) { return w.id == "fixture.row.0"; });
    ComPtr<IRawElementProviderSimple> retained_simple;
    if (retained) retained.As(&retained_simple);
    VARIANT value{};
    check(retained_simple && retained_simple->GetPropertyValue(UIA_NamePropertyId, &value) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE),
        "a removed explicit record key returns element-not-available");
    VariantClear(&value);
    ComPtr<IRawElementProviderFragment> missing_next;
    check(retained && retained->Navigate(NavigateDirection_NextSibling, missing_next.GetAddressOf()) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) && !missing_next,
        "a missing record cannot navigate as another record");
    access.disconnect();
    ComPtr<IRawElementProviderSimple> root_simple; root.As(&root_simple);
    check(root_simple && root_simple->GetPropertyValue(UIA_NamePropertyId, &value) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE),
        "retained native root loses host access after disconnect");
    VariantClear(&value);
}
int ink_pixels(HWND window, float dpi_scale, D2D1_RECT_F area, COLORREF background) {
    HDC dc = GetDC(window);
    if (!dc) return 0;
    const int left = static_cast<int>(area.left * dpi_scale), top = static_cast<int>(area.top * dpi_scale);
    const int width = static_cast<int>(area.right * dpi_scale) - left, height = static_cast<int>(area.bottom * dpi_scale) - top;
    if (width <= 0 || height <= 0) { ReleaseDC(window, dc); return 0; }
    HDC memory = CreateCompatibleDC(dc);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP image = memory ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!image || !bits) { if (image) DeleteObject(image); if (memory) DeleteDC(memory); ReleaseDC(window, dc); return 0; }
    const auto previous = SelectObject(memory, image);
    const bool copied = BitBlt(memory, 0, 0, width, height, dc, left, top, SRCCOPY) != FALSE;
    int ink = 0;
    if (copied) {
        const auto* pixels = static_cast<const unsigned char*>(bits);
        for (int i = 0; i < width * height; ++i) {
            const auto offset = static_cast<std::size_t>(i) * 4;
            if (std::abs(int(pixels[offset + 2]) - int(GetRValue(background))) > 20 ||
                std::abs(int(pixels[offset + 1]) - int(GetGValue(background))) > 20 || std::abs(int(pixels[offset]) - int(GetBValue(background))) > 20) ++ink;
        }
    }
    SelectObject(memory, previous); DeleteObject(image); DeleteDC(memory); ReleaseDC(window, dc);
    return ink;
}
std::vector<COLORREF> capture_pixels(HWND window, float dpi_scale, D2D1_RECT_F area) {
    std::vector<COLORREF> result;
    HDC dc = GetDC(window);
    if (!dc) return result;
    const int left = static_cast<int>(area.left * dpi_scale), top = static_cast<int>(area.top * dpi_scale);
    const int width = static_cast<int>(area.right * dpi_scale) - left, height = static_cast<int>(area.bottom * dpi_scale) - top;
    if (width <= 0 || height <= 0) { ReleaseDC(window, dc); return result; }
    HDC memory = CreateCompatibleDC(dc);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP image = memory ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!image || !bits) { if (image) DeleteObject(image); if (memory) DeleteDC(memory); ReleaseDC(window, dc); return result; }
    const auto previous = SelectObject(memory, image);
    if (BitBlt(memory, 0, 0, width, height, dc, left, top, SRCCOPY) && GdiFlush()) {
        const auto* pixels = static_cast<const unsigned char*>(bits);
        result.reserve(static_cast<std::size_t>(width) * height);
        for (int i = 0; i < width * height; ++i) {
            const auto offset = static_cast<std::size_t>(i) * 4;
            result.push_back(RGB(pixels[offset + 2], pixels[offset + 1], pixels[offset]));
        }
    }
    SelectObject(memory, previous); DeleteObject(image); DeleteDC(memory); ReleaseDC(window, dc);
    return result;
}
int changed_pixels(const std::vector<COLORREF>& before, const std::vector<COLORREF>& after) {
    if (before.empty() || before.size() != after.size()) return -1;
    int changed = 0;
    for (std::size_t i = 0; i < before.size(); ++i) if (before[i] != after[i]) ++changed;
    return changed;
}
void paint_nested_modal_feedback(HWND window) {
    Canvas canvas;
    const float dpi = static_cast<float>(GetDpiForWindow(window)), scale = dpi / 96;
    const bool initialized = canvas.init() && canvas.attach(window, dpi);
    check(initialized, "nested modal feedback creates a native drawing surface");
    if (!initialized) return;
    RECT client{}; GetClientRect(window, &client);
    const float width = client.right / scale, height = client.bottom / scale;
    const float half = std::min({22.0f, width / 8, height / 4});
    const auto parent_rect = D2D1::RectF(width / 4 - half, height / 2 - half, width / 4 + half, height / 2 + half);
    const auto child_rect = D2D1::RectF(width * 3 / 4 - half, height / 2 - half, width * 3 / 4 + half, height / 2 + half);
    struct Pixels { std::vector<COLORREF> parent, child; };
    for (bool dark : {false, true}) {
        Scene scene; scene.layers.resize(3);
        scene.layers[1].modal = scene.layers[2].modal = true;
        for (int layer : {1, 2}) {
            Widget surface; surface.layer = layer; surface.scrolls = false; surface.fill = "surface";
            surface.rect = layer == 1 ? D2D1::RectF(0, 0, width, height) : inflate(child_rect, 8);
            scene.widgets.push_back(surface);
            Widget close; close.layer = layer; close.scrolls = false;
            close.id = "sheet.close"; close.kind = Kind::button; close.button = ButtonStyle::plain;
            close.icon = "x"; close.radius = 8; close.focusable = true;
            close.rect = layer == 1 ? parent_rect : child_rect;
            scene.widgets.push_back(close);
        }
        const auto palette = make_palette(dark, false);
        auto render = [&](const Interaction& state) {
            Pixels pixels;
            const bool began = canvas.begin();
            check(began, "begin native modal feedback repaint");
            if (!began) return pixels;
            paint(canvas, palette, scene, state, width, height, 0);
            check(canvas.end(), "complete native modal feedback repaint");
            DwmFlush();
            pixels.parent = capture_pixels(window, scale, inflate(parent_rect, 5));
            pixels.child = capture_pixels(window, scale, inflate(child_rect, 5));
            check(!pixels.parent.empty() && !pixels.child.empty(), "capture both modal controls including their focus rings");
            return pixels;
        };
        const auto baseline = render({});
        const std::vector<Interaction> interactions{
            {"sheet.close", "", ""}, {"sheet.close", "sheet.close", ""}, {"", "", "sheet.close"}
        };
        int state_index = 0;
        for (const auto& state : interactions) {
            const auto active = render(state);
            const int parent_changes = changed_pixels(baseline.parent, active.parent);
            const int child_changes = changed_pixels(baseline.child, active.child);
            std::printf("native modal feedback: %s state %d, parent %d changed pixels, child %d\n",
                dark ? "dark" : "light", state_index++, parent_changes, child_changes);
            check(parent_changes == 0, "child hover, pressed and focus leave the obscured same-id parent pixels unchanged");
            check(child_changes > 10, "top-layer close visibly responds to hover, pressed and focus");
        }
        scene.widgets.back().enabled = false;
        const auto disabled_baseline = render({});
        for (const auto& state : interactions) {
            const auto active = render(state);
            check(changed_pixels(disabled_baseline.child, active.child) == 0 &&
                changed_pixels(disabled_baseline.parent, active.parent) == 0,
                "disabled child close ignores hover, pressed and focus without highlighting the parent");
        }
        std::erase_if(scene.widgets, [](const Widget& widget) { return widget.layer == 2; });
        scene.layers.pop_back();
        const auto parent_baseline = render({});
        for (const auto& state : interactions) {
            const auto active = render(state);
            check(changed_pixels(parent_baseline.parent, active.parent) > 10,
                "parent close regains hover, pressed and focus feedback after the child is removed");
        }
    }
}
void paint_history(HWND window) {
    Canvas canvas;
    const float dpi = static_cast<float>(GetDpiForWindow(window)), scale = dpi / 96;
    check(canvas.init() && canvas.attach(window, dpi), "fake history creates actual DirectWrite and Direct2D surfaces");
    RECT client{}; GetClientRect(window, &client);
    const float width = client.right / scale, height = client.bottom / scale;
    Scene scene; scene.chrome_height = 52;
    scene.layers[0].viewport = D2D1::RectF(0, 52, width, height);
    scene.layers[0].content_height = 30000;
    Widget background; background.fill = "surface"; background.rect = D2D1::RectF(0, 0, width, 30000); scene.widgets.push_back(background);
    Widget heading; heading.kind = Kind::text; heading.text = L"Offline records"; heading.rect = D2D1::RectF(8, 8, width - 56, 36);
    heading.scrolls = false; scene.widgets.push_back(heading);
    Widget close; close.kind = Kind::button; close.button = ButtonStyle::primary; close.icon = "x"; close.scrolls = false;
    close.rect = D2D1::RectF(width - 48, 4, width - 4, 48); scene.widgets.push_back(close);
    Widget margin; margin.kind = Kind::text; margin.id = "fixture.margin"; margin.text = L"near edge";
    margin.rect = D2D1::RectF(18, 25, width - 22, 49); scene.widgets.push_back(margin);
    for (int i = 0; i < 1100; ++i) {
        Widget row; row.kind = Kind::text; row.text = L"Fixture row " + std::to_wstring(i);
        row.rect = D2D1::RectF(18, 60.0f + i * 26.0f, width - 22, 84.0f + i * 26.0f); scene.widgets.push_back(std::move(row));
    }
    const auto count = scene.widgets.size();
    const auto palette = make_palette(false, false);
    // A text row three DIPs outside the viewport still needs its focus/glyph margin.
    Scene edge = scene;
    std::erase_if(edge.widgets, [](const Widget& w) { return w.kind == Kind::text && w.text.starts_with(L"Fixture row "); });
    canvas.reset_drawn_text_layouts_for_testing();
    check(canvas.begin(), "begin native conservative-edge repaint");
    paint(canvas, palette, edge, Interaction{"", "", "fixture.margin"}, width, height, 0);
    check(canvas.end() && canvas.drawn_text_layouts_for_testing() == 2,
        "viewport culling preserves fixed heading and a focused text row three DIPs beyond its edge");
    for (float scroll : {0.0f, 13000.0f, 0.0f}) {
        scene.layers[0].scroll = scroll;
        canvas.reset_drawn_text_layouts_for_testing();
        check(canvas.begin(), "begin native fake history repaint");
        paint(canvas, palette, scene, Interaction{"", "", "fixture.margin"}, width, height, 0);
        check(canvas.end(), "complete native fake history repaint");
        DwmFlush();
        const auto layouts = canvas.drawn_text_layouts_for_testing();
        std::printf("fake record paint: scroll %.0f, %zu text layouts\n", scroll, layouts);
        check(layouts >= 3 && layouts <= 24, "history repaint lays out visible rows and conservative edge margin rather than 1100 offscreen rows");
        check(scene.widgets.size() == count, "viewport paint never removes history or accessible widgets");
        const auto bg = to_colorref(palette.surface);
        check(ink_pixels(window, scale, D2D1::RectF(18, 60, std::min(width - 22, 170.0f), 84), bg) > 5,
            "a scrolled-in record is actually painted and becomes readable after returning");
        check(ink_pixels(window, scale, close.rect, to_colorref(palette.background)) > 5, "fixed close action remains painted during history scroll");
        HDC dc = GetDC(window);
        const auto pixel = dc ? GetPixel(dc, static_cast<int>(2 * scale), static_cast<int>((height - 4) * scale)) : CLR_INVALID;
        if (dc) ReleaseDC(window, dc);
        check(pixel == bg, "giant intersecting background is retained despite extending beyond viewport");
    }
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Isolated fake records view", WS_POPUP,
        24, 24, 440, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "create fake history window without application services");
    if (window) {
        ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
        nested_modal_actions(window); provider_history(window); paint_nested_modal_feedback(window); paint_history(window); DestroyWindow(window);
    }
    CoUninitialize();
    std::printf("native records view: %d checks, %d failures; no data/network services\n", checks, failures);
    return failures ? 1 : 0;
}
