#include "info_popover.hpp"
#include "shared_copy.hpp"
#include "shared_tokens.hpp"
#include "szu/win/text.hpp"
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace szu::app {
namespace {
constexpr wchar_t kClass[] = L"SZUConnect.InfoPopover.v1";
constexpr UINT kClose = WM_APP + 71;
PopoverRect rect(const RECT& r) { return {float(r.left), float(r.top), float(r.right), float(r.bottom)}; }
}
InfoPopover::InfoPopover(HWND owner) : owner_(owner) {}
InfoPopover::~InfoPopover() {
    if (access_) access_->disconnect();
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
}
bool InfoPopover::create() {
    WNDCLASSEXW wc{}; wc.cbSize = sizeof wc;
    wc.lpfnWndProc = window_proc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 1, 1, owner_, nullptr, wc.hInstance, this);
    if (!hwnd_ || !canvas_.init() || !canvas_.attach(hwnd_, dpi_)) return false;
    access_ = std::make_unique<Accessibility>(*this);
    const DWORD round = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33, &round, sizeof round);
    const MARGINS shadow{1, 1, 1, 1}; DwmExtendFrameIntoClientArea(hwnd_, &shadow);
    return true;
}
bool InfoPopover::show(std::wstring title, std::wstring body, RECT anchor, RECT context,
                       const Palette& palette, float dpi, float text_scale, bool keyboard) {
    dpi_ = dpi;
    if (!hwnd_ && !create()) return false;
    if (title != title_ || body != body_) scene_.layers[0].scroll = 0;
    title_ = std::move(title); body_ = std::move(body); palette_ = palette;
    palette_.background = palette_.surface;
    canvas_.set_dpi(dpi_); canvas_.set_text_scale(text_scale);
    BOOL dark = palette_.dark; DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof dark);
    const COLORREF border = to_colorref(palette_.border); DwmSetWindowAttribute(hwnd_, 34, &border, sizeof border);
    SetWindowTextW(hwnd_, title_.c_str());
    MONITORINFO mi{}; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    const float scale = host_scale(), padding = tokens::kSpaceL;
    const float width = std::min(tokens::kSizeInfoPopoverMaxWidth,
        tokens::kSizeInfoPopoverWidth * std::max(1.0f, text_scale));
    const float header = std::max(tokens::kSizeMinTarget,
        canvas_.measure(title_, TextStyle::headline, width - 2 * padding - tokens::kSizeMinTarget).height);
    const float narrow_header = std::max(tokens::kSizeMinTarget,
        canvas_.measure(title_, TextStyle::headline, tokens::kSizeInfoPopoverMinWidth - 2 * padding - tokens::kSizeMinTarget).height);
    const float min_height = narrow_header + 2 * padding + tokens::kSpaceS + canvas_.line_height(TextStyle::callout);
    const float body_height = canvas_.measure(body_, TextStyle::callout, width - 2 * padding - tokens::kSpaceS).height;
    auto work = rect(mi.rcWork);
    const float margin = tokens::kSpaceS * scale;
    work.left += margin; work.top += margin; work.right -= margin; work.bottom -= margin;
    auto placed = place_info_popover(work, rect(anchor), rect(context), width * scale,
        (2 * padding + header + tokens::kSpaceS + body_height) * scale,
        tokens::kSizeInfoPopoverMinWidth * scale,
        std::max(tokens::kSizeInfoPopoverMinHeight, min_height) * scale,
        tokens::kSpaceS * scale);
    if (!placed.avoids_context) {
        // If a whole card leaves no readable space, still keep its heading and
        // nearby explanation visible. Other controls remain usable underneath.
        RECT local = context;
        local.top = std::max(local.top, anchor.top - static_cast<LONG>(padding * scale));
        local.bottom = std::min(local.bottom, anchor.bottom + static_cast<LONG>(2 * tokens::kSizeMinTarget * scale));
        placed = place_info_popover(work, rect(anchor), rect(local), width * scale,
            (2 * padding + header + tokens::kSpaceS + body_height) * scale,
            tokens::kSizeInfoPopoverMinWidth * scale,
            min_height * scale,
            tokens::kSpaceS * scale);
    }
    const auto r = placed.rect;
    const int x = static_cast<int>(std::ceil(r.left)), y = static_cast<int>(std::ceil(r.top));
    const int w = static_cast<int>(std::floor(r.right)) - x, h = static_cast<int>(std::floor(r.bottom)) - y;
    const float minimum_width = 2 * padding + tokens::kSizeMinTarget + canvas_.line_height(TextStyle::callout);
    const float actual_header = std::max(tokens::kSizeMinTarget,
        canvas_.measure(title_, TextStyle::headline, std::max(1.0f, w / scale - 2 * padding - tokens::kSizeMinTarget)).height);
    const float minimum_height = 2 * padding + actual_header + tokens::kSpaceS + canvas_.line_height(TextStyle::callout);
    if (w / scale < minimum_width || h / scale < minimum_height) { hide(); return false; }
    if (!SetWindowPos(hwnd_, HWND_TOP, x, y, w, h, SWP_NOACTIVATE)) { hide(); return false; }
    canvas_.refresh_text_rendering();
    layout(w / scale, h / scale);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE); InvalidateRect(hwnd_, nullptr, FALSE); UpdateWindow(hwnd_);
    if (keyboard) focus_close();
    return true;
}
void InfoPopover::layout(float width, float height) {
    const float old_scroll = scene_.layers[0].scroll;
    width_ = width; height_ = height; scene_ = Scene{};
    const float pad = tokens::kSpaceL, target = tokens::kSizeMinTarget;
    const float title_width = std::max(1.0f, width - 2 * pad - target);
    const float title_height = canvas_.measure(title_, TextStyle::headline, title_width).height;
    const float head = std::max(target, title_height);
    Builder header(scene_, canvas_, 0, pad, title_width, pad + (head - title_height) / 2, false);
    header.text(title_, TextStyle::headline, "textPrimary", Role::heading, "info.title");
    Builder close(scene_, canvas_, 0, width - pad - target, target, pad, false);
    close.icon_button("info.close", win::widen(copy::text("action.close")), "x", [this] { hide(true); });
    const float body_top = pad + head + tokens::kSpaceS;
    scene_.layers[0].viewport = {pad, body_top, width - pad, std::max(body_top, height - pad)};
    Builder content(scene_, canvas_, 0, pad, std::max(1.0f, width - 2 * pad - tokens::kSpaceS), body_top);
    content.text(body_, TextStyle::callout, "textSecondary", Role::text, "info.body");
    scene_.layers[0].content_height = content.y() - body_top;
    scene_.layers[0].scroll = old_scroll; scene_.clamp_scroll(0);
    interaction_.focused = "info.close";
    if (access_) access_->structure_changed();
}
void InfoPopover::hide(bool restore_focus) {
    if (!visible()) return;
    const HWND foreground = GetForegroundWindow();
    ShowWindow(hwnd_, SW_HIDE); interaction_ = Interaction{};
    scene_ = Scene{};
    if (access_) access_->structure_changed();
    if (restore_focus && (foreground == owner_ || foreground == hwnd_) && IsWindowVisible(owner_) && !IsIconic(owner_)) {
        SetForegroundWindow(owner_); SetFocus(owner_);
    }
}
void InfoPopover::focus_close() {
    if (!visible()) return;
    SetForegroundWindow(hwnd_); SetFocus(hwnd_);
    interaction_.focused = "info.close"; InvalidateRect(hwnd_, nullptr, FALSE);
    access_->focus_changed("info.close");
}
std::string InfoPopover::host_focus() const {
    GUITHREADINFO focus{}; focus.cbSize = sizeof focus;
    return visible() && GetGUIThreadInfo(GetWindowThreadProcessId(hwnd_, nullptr), &focus) && focus.hwndFocus == hwnd_
        ? interaction_.focused : std::string{};
}
void InfoPopover::host_set_focus(const std::string& id) { if (id == "info.close") focus_close(); }
void InfoPopover::host_activate(const std::string& id) { if (id == "info.close") PostMessageW(hwnd_, kClose, 0, 0); }
bool InfoPopover::scroll(float dy) {
    if (!visible()) return false;
    scene_.layers[0].scroll += dy; scene_.clamp_scroll(0); InvalidateRect(hwnd_, nullptr, FALSE); return true;
}
LRESULT CALLBACK InfoPopover::window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<InfoPopover*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) { self = static_cast<InfoPopover*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd; SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}
LRESULT InfoPopover::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hwnd_, &ps);
        if (canvas_.begin()) {
            Interaction shown = interaction_;
            if (GetFocus() != hwnd_) shown.focused.clear();
            paint(canvas_, palette_, scene_, shown, width_, height_, 0);
            canvas_.stroke_round({0.5f, 0.5f, width_ - 0.5f, height_ - 0.5f}, tokens::kRadiusMedium, palette_.border, tokens::kSizeBorderWidth);
            if (!canvas_.end()) InvalidateRect(hwnd_, nullptr, FALSE);
        }
        EndPaint(hwnd_, &ps); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: canvas_.resize(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && reinterpret_cast<HWND>(lp) != owner_) hide();
        return 0;
    case WM_MOUSEMOVE: {
        if (!tracking_) { TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0}; TrackMouseEvent(&t); tracking_ = true; }
        const auto* w = scene_.hit(GET_X_LPARAM(lp) / host_scale(), GET_Y_LPARAM(lp) / host_scale());
        interaction_.hovered = w ? w->id : ""; InvalidateRect(hwnd_, nullptr, FALSE); return 0;
    }
    case WM_MOUSELEAVE: tracking_ = false; interaction_.hovered.clear(); InvalidateRect(hwnd_, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        const auto* w = scene_.hit(GET_X_LPARAM(lp) / host_scale(), GET_Y_LPARAM(lp) / host_scale());
        interaction_.pressed = w ? w->id : ""; return 0;
    }
    case WM_LBUTTONUP: {
        const auto* w = scene_.hit(GET_X_LPARAM(lp) / host_scale(), GET_Y_LPARAM(lp) / host_scale());
        if (w && w->id == interaction_.pressed) hide(true);
        interaction_.pressed.clear(); return 0;
    }
    case WM_MOUSEWHEEL: scroll(-GET_WHEEL_DELTA_WPARAM(wp) / float(WHEEL_DELTA) * 44); return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE || wp == VK_RETURN || wp == VK_SPACE) { hide(true); return 0; }
        if (wp == VK_TAB) { focus_close(); return 0; }
        if (wp == VK_UP || wp == VK_DOWN) { scroll(wp == VK_UP ? -44.0f : 44.0f); return 0; }
        if (wp == VK_PRIOR || wp == VK_NEXT) { scroll((wp == VK_PRIOR ? -1 : 1) * (scene_.layers[0].viewport.bottom - scene_.layers[0].viewport.top) * 0.85f); return 0; }
        if (wp == VK_HOME || wp == VK_END) { scroll(wp == VK_HOME ? -1e6f : 1e6f); return 0; }
        break;
    case WM_GETOBJECT: if (access_) return access_->on_get_object(wp, lp); break;
    case kClose: case WM_CLOSE: hide(true); return 0;
    case WM_DESTROY: if (access_) access_->disconnect(); return 0;
    case WM_NCDESTROY: SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0); hwnd_ = nullptr; return 0;
    default: break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
} // namespace szu::app
