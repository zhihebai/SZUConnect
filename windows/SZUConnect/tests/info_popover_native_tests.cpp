// Native owned-window verification. No AppController, runtime, storage, credentials,
// network or WLAN services are linked or constructed. No bitmap files are created.
#include "info_popover.hpp"
#include "shared_copy.hpp"
#include "shared_tokens.hpp"
#include "szu/win/text.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <dwmapi.h>
#include <initializer_list>
#include <string>
#include <thread>
#include <wrl/client.h>

using namespace szu;
using namespace szu::app;
using Microsoft::WRL::ComPtr;

namespace {
int checks = 0, failures = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { ++failures; std::printf("FAIL: %s\n", message); }
}
void pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
bool overlaps(RECT a, RECT b) {
    return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
}
bool same(D2D1_RECT_F a, D2D1_RECT_F b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
void inspect(InfoPopover& popover, HWND owner, RECT anchor, RECT context, RECT work,
    const std::wstring& title, const std::wstring& body, const Palette& palette, float text_scale) {
    check(popover.visible(), "popover is visible");
    check(IsWindowEnabled(owner) != FALSE, "owner stays enabled and nonmodal");
    const HWND popup = popover.host_window();
    check(GetWindow(popup, GW_OWNER) == owner, "popover is owned by its test window");
    const auto style = GetWindowLongPtrW(popup, GWL_STYLE), extended = GetWindowLongPtrW(popup, GWL_EXSTYLE);
    check((style & WS_POPUP) && (extended & WS_EX_TOOLWINDOW) && !(extended & WS_EX_TOPMOST), "popover has tool-window rather than global-topmost scope");
    RECT bounds{}, client{}; GetWindowRect(popup, &bounds); GetClientRect(popup, &client);
    check(bounds.left >= work.left && bounds.top >= work.top && bounds.right <= work.right && bounds.bottom <= work.bottom,
        "native popup rectangle stays inside monitor work area");
    check(!overlaps(bounds, anchor) && !overlaps(bounds, context), "native popup avoids the anchor and explained row");
    const auto& scene = popover.host_scene();
    check(scene.layers.size() == 1 && !scene.layers[0].modal, "popover scene has no modal backdrop");
    const auto* title_widget = scene.find("info.title");
    const auto* body_widget = scene.find("info.body");
    const auto* close = scene.find("info.close");
    check(title_widget && title_widget->text == title && title_widget->role == Role::heading &&
        title_widget->style == TextStyle::headline && !title_widget->scrolls, "shared heading stays fixed");
    check(body_widget && body_widget->text == body && body_widget->role == Role::text && body_widget->scrolls &&
        body_widget->style == TextStyle::callout, "complete shared explanation uses scrolling callout text");
    check(close && close->focusable && close->action && !close->scrolls && close->role == Role::button &&
        close->rect.right - close->rect.left >= tokens::kSizeMinTarget && close->rect.bottom - close->rect.top >= tokens::kSizeMinTarget,
        "fixed close button retains its 44-DIP target");
    const float scale = popover.host_scale();
    if (close) check(close->rect.left >= 0 && close->rect.top >= 0 && close->rect.right <= client.right / scale &&
        close->rect.bottom <= client.bottom / scale, "close target is fully inside the native client");
    check(scene.focus_order().size() == 1 && scene.focus_order()[0]->id == "info.close", "keyboard order exposes one close action");
    check(scene.layers[0].viewport.bottom > scene.layers[0].viewport.top, "scrolling body has readable positive height");
    check(scene.layers[0].viewport.bottom - scene.layers[0].viewport.top >= tokens::kFontCalloutSize * tokens::kFontLineHeight * text_scale,
        "actual wrapped heading leaves at least one complete line of body text");
    // Inspect a real painted background pixel without retaining or exporting an image.
    UpdateWindow(popup); DwmFlush();
    HDC dc = GetDC(popup);
    const COLORREF pixel = dc ? GetPixel(dc, client.right / 2, std::max(1, static_cast<int>(4 * scale))) : CLR_INVALID;
    if (dc) ReleaseDC(popup, dc);
    const COLORREF expected = to_colorref(palette.surface);
    check(pixel != CLR_INVALID && std::abs(int(GetRValue(pixel)) - int(GetRValue(expected))) <= 2 &&
        std::abs(int(GetGValue(pixel)) - int(GetGValue(expected))) <= 2 &&
        std::abs(int(GetBValue(pixel)) - int(GetBValue(expected))) <= 2, "native surface paint follows the passed light/dark palette");
}

ComPtr<IUIAutomationElement> find_id(IUIAutomation* automation, IUIAutomationElement* root, const wchar_t* id) {
    VARIANT value{}; value.vt = VT_BSTR; value.bstrVal = SysAllocString(id);
    ComPtr<IUIAutomationCondition> condition;
    automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, value, condition.GetAddressOf());
    VariantClear(&value);
    ComPtr<IUIAutomationElement> result;
    if (condition) root->FindFirst(TreeScope_Descendants, condition.Get(), result.GetAddressOf());
    return result;
}
bool name_is(IUIAutomationElement* element, const std::wstring& expected) {
    if (!element) return false;
    BSTR name = nullptr;
    const HRESULT result = element->get_CurrentName(&name);
    const bool equal = SUCCEEDED(result) && name && std::wstring(name, SysStringLen(name)) == expected;
    SysFreeString(name);
    return equal;
}
}  // namespace

int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    check(SUCCEEDED(com), "native UI COM initialization");
    if (FAILED(com)) return 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    constexpr wchar_t owner_class[] = L"SZUConnect.InfoPopover.IsolatedTest.v1";
    WNDCLASSEXW window_class{}; window_class.cbSize = sizeof window_class;
    window_class.lpfnWndProc = DefWindowProcW; window_class.hInstance = GetModuleHandleW(nullptr); window_class.lpszClassName = owner_class;
    check(RegisterClassExW(&window_class) != 0, "isolated owner class registration");
    MONITORINFO monitor{}; monitor.cbSize = sizeof monitor;
    check(GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor) != FALSE, "read current monitor work area");
    const RECT work = monitor.rcWork;
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, owner_class, L"Isolated information popover test", WS_OVERLAPPEDWINDOW,
        work.left + 24, work.top + 24, 320, 240, nullptr, nullptr, window_class.hInstance, nullptr);
    check(owner != nullptr, "isolated native owner creation");
    if (!owner) { CoUninitialize(); return 1; }
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    const float dpi = static_cast<float>(GetDpiForWindow(owner)), scale = dpi / 96;
    {
        InfoPopover popover(owner);
        const char* topics[]{"status", "probe", "auto", "memory", "account", "websites", "records", "network", "area", "addressResolution"};
        int index = 0;
        for (bool dark : {false, true}) for (float text_scale : {1.0f, 1.5f}) for (const char* topic : topics) {
            const std::wstring title = win::widen(copy::text(std::string("info.") + topic + ".title"));
            const std::wstring body = win::widen(copy::text(std::string("info.") + topic + ".body"));
            const int size = static_cast<int>(44 * scale), margin = static_cast<int>(16 * scale);
            const int x = work.left + margin + (index % 3) * (work.right - work.left - 2 * margin - size) / 2;
            const int y = work.top + margin + ((index / 3) % 3) * (work.bottom - work.top - 2 * margin - size) / 2;
            const RECT anchor{x, y, x + size, y + size};
            const RECT context{x - static_cast<int>(140 * scale), y - static_cast<int>(10 * scale), anchor.right, y + static_cast<int>(54 * scale)};
            const auto palette = make_palette(dark, false);
            check(popover.show(title, body, anchor, context, palette, dpi, text_scale, false), "show shared topic on real Windows");
            if (popover.visible()) {
                check(popover.host_focus().empty(), "mouse opening does not claim keyboard focus in an inactive popup");
                inspect(popover, owner, anchor, context, work, title, body, palette, text_scale);
                if (index % 3 == 0) SendMessageW(popover.host_window(), WM_KEYDOWN, VK_ESCAPE, 0);
                else if (index % 3 == 1) { popover.host_activate("info.close"); pump(); }
                else popover.hide();
                check(!popover.visible() && IsWindowEnabled(owner), "Escape, accessible close or hide dismisses without disabling owner");
                check(!popover.host_scene().find("info.body"), "hidden popover clears its accessible explanation content");
            }
            ++index;
        }
        const int center_x = (work.left + work.right) / 2, center_y = (work.top + work.bottom) / 2;
        const RECT anchor{center_x, center_y, center_x + static_cast<int>(44 * scale), center_y + static_cast<int>(44 * scale)};
        const std::wstring title = win::widen(copy::text("info.network.title"));
        check(popover.show(title, win::widen(copy::text("info.network.body")), anchor, anchor, make_palette(false, false), dpi, 1.5f, true),
            "keyboard opening activates the explanation close target");
        check(popover.host_focus() == "info.close", "active popup reports its actual keyboard focus");
        popover.hide(true);
        std::wstring long_body;
        for (int repeat = 0; repeat < 20; ++repeat) long_body += win::widen(copy::text("info.network.body")) + L"\n\n";
        check(popover.show(title, long_body, anchor, anchor, make_palette(false, false), dpi, 1.5f, false), "show a deliberately long isolated explanation");
        if (popover.visible()) {
            const auto* close = popover.host_scene().find("info.close");
            const auto* heading = popover.host_scene().find("info.title");
            const auto close_before = close ? popover.host_scene().visible_rect(*close) : D2D1_RECT_F{};
            const auto title_before = heading ? popover.host_scene().visible_rect(*heading) : D2D1_RECT_F{};
            const auto& layer = popover.host_scene().layers[0];
            check(layer.content_height > layer.viewport.bottom - layer.viewport.top, "long body requires scrolling");
            check(popover.scroll(1.0e6f) && popover.host_scene().layers[0].scroll > 0, "long body scroll reaches lower paragraphs");
            check(close && heading && same(close_before, popover.host_scene().visible_rect(*close)) &&
                same(title_before, popover.host_scene().visible_rect(*heading)), "scrolling never moves the heading or close target");
            SendMessageW(popover.host_window(), WM_KEYDOWN, VK_HOME, 0);
            check(popover.host_scene().layers[0].scroll == 0, "Home returns the explanation to its first paragraph");
            SendMessageW(popover.host_window(), WM_KEYDOWN, VK_END, 0);
            check(popover.host_scene().layers[0].scroll > 0, "End reaches the last paragraph");
            check(popover.show(title, long_body, anchor, anchor, make_palette(true, false), dpi, 1.0f, false) &&
                popover.host_scene().layers[0].scroll > 0, "same explanation preserves scrolling during theme/text reflow");
            check(popover.show(title + L" (fixture)", long_body + L"\nfixture", anchor, anchor, make_palette(false, false), dpi, 1.0f, false) &&
                popover.host_scene().layers[0].scroll == 0, "different explanation starts at its first paragraph");
            SendMessageW(popover.host_window(), WM_ACTIVATE, WA_INACTIVE, 0);
            check(!popover.visible(), "deactivation outside the owner dismisses the explanation");
        }
        const std::wstring body = win::widen(copy::text("info.network.body"));
        check(popover.show(title, body, anchor, anchor, make_palette(false, false), dpi, 1.0f, false), "show explanation for a native UIA client");
        if (popover.visible()) {
            struct Result { bool available = false, root_name = false, root_id = false, title = false, body = false, close = false, invoked = false; } result;
            std::atomic<bool> done = false;
            const HWND popup = popover.host_window();
            std::thread reader([&] {
                const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                if (SUCCEEDED(initialized)) {
                    {
                        ComPtr<IUIAutomation> automation;
                        ComPtr<IUIAutomationElement> root;
                        if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(automation.GetAddressOf()))) && SUCCEEDED(automation->ElementFromHandle(popup, root.GetAddressOf()))) {
                            result.available = true;
                            result.root_name = name_is(root.Get(), title);
                            BSTR automation_id = nullptr;
                            result.root_id = SUCCEEDED(root->get_CurrentAutomationId(&automation_id)) && automation_id &&
                                std::wstring(automation_id, SysStringLen(automation_id)) == L"SZUConnect.InfoPopover";
                            SysFreeString(automation_id);
                            const auto title_element = find_id(automation.Get(), root.Get(), L"info.title");
                            const auto body_element = find_id(automation.Get(), root.Get(), L"info.body");
                            const auto close_element = find_id(automation.Get(), root.Get(), L"info.close");
                            result.title = name_is(title_element.Get(), title); result.body = name_is(body_element.Get(), body);
                            CONTROLTYPEID type = 0;
                            result.close = close_element && SUCCEEDED(close_element->get_CurrentControlType(&type)) && type == UIA_ButtonControlTypeId;
                            ComPtr<IUnknown> pattern;
                            ComPtr<IUIAutomationInvokePattern> invoke;
                            result.invoked = close_element && SUCCEEDED(close_element->GetCurrentPattern(UIA_InvokePatternId, pattern.GetAddressOf())) &&
                                pattern && SUCCEEDED(pattern.As(&invoke)) && SUCCEEDED(invoke->Invoke());
                        }
                    }
                    CoUninitialize();
                }
                done = true;
            });
            // UIA runs on a separate COM thread; keep its native provider's window responsive.
            // CTest bounds the whole executable if a platform COM call fails to return.
            while (!done) { pump(); MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT); }
            reader.join(); pump();
            check(result.root_name && result.root_id, "native UIA root identifies the current explanation rather than the main window");
            check(result.available && result.title && result.body && result.close, "native UIA exposes complete title, body and close button");
            check(result.invoked && !popover.visible(), "native UIA Invoke closes the owned explanation window");
        }
        popover.hide(); popover.hide();
        check(IsWindowEnabled(owner), "idempotent hide preserves owner state");
    }
    DestroyWindow(owner); UnregisterClassW(owner_class, window_class.hInstance);
    CoUninitialize();
    std::printf("native info popover: %d checks, %d failures; actual DPI %.0f; no data/network services\n", checks, failures, dpi);
    return failures ? 1 : 0;
}
