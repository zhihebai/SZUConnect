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
    HWND host_window() const override { return window; }
    const Scene& host_scene() const override { return scene; }
    float host_scale() const override { return 1; }
    std::string host_focus() const override { return {}; }
    void host_set_focus(const std::string&) override {}
    void host_activate(const std::string&) override {}
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
        provider_history(window); paint_history(window); DestroyWindow(window);
    }
    CoUninitialize();
    std::printf("native records view: %d checks, %d failures; no data/network services\n", checks, failures);
    return failures ? 1 : 0;
}
