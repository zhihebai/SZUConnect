#include "main_window.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <oleacc.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <wingdi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../resources/resource.h"
#include "shared_copy.hpp"
#include "shared_icons.hpp"
#include "shared_tokens.hpp"
#include "szu/win/system.hpp"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "gdi32.lib")

namespace szu::app {

namespace {
std::wstring window_text(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) GetWindowTextW(edit, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}
}  // namespace

MainWindow::MainWindow(HINSTANCE instance, bool read_only, bool background, bool offline_smoke, int timeout_ms)
    : instance_(instance), read_only_(read_only), background_(background), offline_smoke_(offline_smoke), smoke_timeout_ms_(timeout_ms) {}

MainWindow::~MainWindow() {
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    if (edit_font_) DeleteObject(edit_font_);
    if (edit_brush_) DeleteObject(edit_brush_);
}

bool MainWindow::create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::window_proc;
    wc.hInstance = instance_;
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = offline_smoke_ ? L"SZUConnect.OfflineSmoke.MainWindow.v1" : read_only_ ? L"SZUConnect.Observe.MainWindow.v1" : kWindowClass;
    if (!RegisterClassExW(&wc)) return false;
    const int dpi = static_cast<int>(GetDpiForSystem());
    std::wstring title = win::widen(copy::text("app.name"));
    if (read_only_) title += L" · " + win::widen(copy::text("settings.readOnlyMode"));
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                            MulDiv(static_cast<int>(tokens::kSizeWindowsDefaultClientWidth), dpi, 96), MulDiv(static_cast<int>(tokens::kSizeWindowsDefaultClientHeight), dpi, 96), nullptr,
                            nullptr, instance_, this);
    return hwnd_ != nullptr;
}

void MainWindow::show() {
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    rebuild();
    UpdateWindow(hwnd_);
}

int MainWindow::run() {
    MSG message{};
    BOOL got;
    while ((got = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (got == -1) { exit_code_ = 1; smoke_result_.failure = "messageLoopFailed"; if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_); return 1; }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK MainWindow::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    MainWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handle(message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool MainWindow::size_for_client(float width, float height) {
    RECT bounds{0, 0, MulDiv(static_cast<int>(width), static_cast<int>(dpi_), 96),
        MulDiv(static_cast<int>(height), static_cast<int>(dpi_), 96)};
    if (!AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, FALSE, 0, static_cast<UINT>(dpi_))) return false;
    bounds.top = 0; // Client toolbar replaces the caption; native side/bottom borders stay.
    MONITORINFO monitor{}; monitor.cbSize = sizeof monitor;
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    const RECT& work = monitor.rcWork;
    const int margin = MulDiv(8, static_cast<int>(dpi_), 96);
    const int available_w = std::max(1, static_cast<int>(work.right - work.left) - 2 * margin);
    const int available_h = std::max(1, static_cast<int>(work.bottom - work.top) - 2 * margin);
    const int outer_w = std::min(static_cast<int>(bounds.right - bounds.left), available_w);
    const int outer_h = std::min(static_cast<int>(bounds.bottom - bounds.top), available_h);
    const int x = static_cast<int>(work.left) + (static_cast<int>(work.right - work.left) - outer_w) / 2;
    const int y = static_cast<int>(work.top) + (static_cast<int>(work.bottom - work.top) - outer_h) / 2;
    return SetWindowPos(hwnd_, nullptr, x, y, outer_w, outer_h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED) != FALSE;
}

bool MainWindow::on_created() {
    smoke_result_.window_created = true;
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));
    if (!size_for_client(tokens::kSizeWindowsDefaultClientWidth, tokens::kSizeWindowsDefaultClientHeight)) {
        smoke_result_.failure = "initialWindowSizingFailed"; exit_code_ = 1; return false;
    }
    if (!canvas_.init() || !canvas_.attach(hwnd_, dpi_)) { smoke_result_.failure = "canvasInitializationFailed"; exit_code_ = 1; return false; }
    smoke_result_.canvas_initialized = true;
    app_ = std::make_unique<AppController>(hwnd_, offline_smoke_ ? RuntimeMode::offlineSmoke : read_only_ ? RuntimeMode::observeOnly : RuntimeMode::normal);
    ctx_.reset(new ViewContext{*app_, ui_, hwnd_, [this] { request_rebuild(); }, [this](const std::string& name) { open_overlay(name); },
                               [this] { close_overlay(); },
                               [this](const std::string& id) {
                                   ui_.copied = id;
                                   SetTimer(hwnd_, kTimerFeedback, 2000, nullptr);
                               }});
    access_ = std::make_unique<Accessibility>(*this);
    app_->start();
    apply_theme();
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (!offline_smoke_) {
        tray_.add(hwnd_, instance_);
        power_ = RegisterSuspendResumeNotification(hwnd_, DEVICE_NOTIFY_WINDOW_HANDLE);
    }
    if (app_->settings().tutorial == "none" && !background_ && !read_only_ && !offline_smoke_) {
        ui_.tutorial_visible = true; ui_.tutorial_entered_at = AppController::wall_now();
    }
    rebuild();
    update_tray();
    if (offline_smoke_) {
        prepare_smoke_scene();
        if (!smoke_result_.failure.empty()) { exit_code_ = 1; return false; }
        if (!SetTimer(hwnd_, 20, static_cast<UINT>(smoke_timeout_ms_), nullptr)) { smoke_result_.failure = "deadlineTimerFailed"; exit_code_ = 1; return false; }
        app_->connect();
    }
    return true;
}

void MainWindow::smoke_step() {
    if (!offline_smoke_ || !app_) return;
    if (smoke_result_.failure.empty() && (!app_->fake_auth_passed() || app_->snapshot().busy)) return;
    if (!smoke_result_.failure.empty() || smoke_capture_stage_ == kSmokeFrameLabels.size()) {
        if (smoke_result_.failure.empty() && (!smoke_result_.visual_unavailable.empty() ||
            smoke_result_.visual_frames.size() != kSmokeFrameLabels.size())) smoke_result_.failure = "visualCaptureIncomplete";
        if (smoke_result_.failure.empty() && !access_->verify_retained_provider_disconnect()) smoke_result_.failure = "uiaDisconnectFailed";
        exit_code_ = smoke_result_.failure.empty() ? 0 : 1; quitting_ = true; DestroyWindow(hwnd_); return;
    }
    if (!smoke_capture_ready_) return;
    smoke_capture_ready_ = false;
    capture_smoke_frame(kSmokeFrameLabels[smoke_capture_stage_]);
    if (!smoke_result_.failure.empty()) { PostMessageW(hwnd_, WM_APP + 10, 0, 0); return; }
    ++smoke_capture_stage_;
    if (smoke_capture_stage_ < kSmokeFrameLabels.size()) prepare_smoke_scene();
    else PostMessageW(hwnd_, WM_APP + 10, 0, 0);
    if (!smoke_result_.failure.empty()) PostMessageW(hwnd_, WM_APP + 10, 0, 0);
}

void MainWindow::prepare_smoke_scene() {
    if (!offline_smoke_ || !app_ || !app_->offline_smoke()) return;
    // Only the isolated test window changes. Default is a 500-DIP client;
    // actual client dimensions are recorded. Text150 is text size, never a DPI claim.
    constexpr Page pages[]{Page::connection, Page::records, Page::settings, Page::settings,
        Page::connection, Page::records, Page::settings, Page::connection,
        Page::connection, Page::settings, Page::connection, Page::connection,
        Page::settings, Page::settings, Page::settings,
        Page::settings, Page::settings, Page::settings, Page::settings,
        Page::connection, Page::connection, Page::records, Page::connection, Page::connection};
    static_assert(sizeof pages / sizeof pages[0] == kSmokeFrameLabels.size());
    if (smoke_capture_stage_ >= kSmokeFrameLabels.size()) return;
    ui_.page = pages[smoke_capture_stage_];
    ui_.show_password = false;
    ui_.overlays = smoke_capture_stage_ >= 15 && smoke_capture_stage_ <= 18 ? std::vector<std::string>{"help"} :
        smoke_capture_stage_ >= 12 && smoke_capture_stage_ <= 14 ? std::vector<std::string>{"about"} : std::vector<std::string>{};
    ui_.expanded_help_faq = smoke_capture_stage_ == 16 ? std::vector<std::string>{"wifi"} : std::vector<std::string>{};
    ui_.advanced_network = smoke_capture_stage_ == 19 || smoke_capture_stage_ == 20;
    ui_.technical_network = ui_.advanced_network;
    ui_.showing_samples = smoke_capture_stage_ == 21;
    ui_.selected_sample_key.clear();
    ui_.failures_only = false;
    ui_.search.clear();
    ui_.home_focus = smoke_capture_stage_ == 7 ? "home.account" : "";
    for (auto& layer : scene_.layers) layer.scroll = 0;
    smoke_dark_ = smoke_capture_stage_ == 8 || smoke_capture_stage_ == 9;
    smoke_text_scale_ = smoke_capture_stage_ == 10 || smoke_capture_stage_ == 14 || smoke_capture_stage_ == 18 || smoke_capture_stage_ == 20 || smoke_capture_stage_ == 23 ? 1.5f : 1.0f;
    apply_theme();
    const auto is_wide = [](std::size_t stage) { return stage >= 4 && stage <= 6; };
    // At 200% DPI, the full-height BMP set exceeds the retained-image budget.
    // Shorten only this isolated test viewport; keep the real DPI and glyph pixels.
    std::size_t planned_pixel_width = 0;
    for (std::size_t stage = 0; stage < kSmokeFrameLabels.size(); ++stage) {
        const int width = static_cast<int>(is_wide(stage) ? tokens::kSizeWindowDefaultWidth : tokens::kSizeWindowsDefaultClientWidth);
        planned_pixel_width += static_cast<std::size_t>(MulDiv(width, static_cast<int>(dpi_), 96));
    }
    const auto height_pixels = (kSmokeBitmapBudgetBytes - kSmokeBitmapHeaderBytes * kSmokeFrameLabels.size()) / (4 * planned_pixel_width);
    const float capture_height = std::min(tokens::kSizeWindowsDefaultClientHeight,
        std::floor(static_cast<float>(height_pixels) * 96.0f / dpi_));
    const bool wide = is_wide(smoke_capture_stage_);
    // One accessible-text scene uses the narrow client to exercise the text-only
    // tier even when the operating system's caption metrics are slightly smaller.
    const float capture_width = smoke_capture_stage_ == 10 ? 480.0f :
        wide ? tokens::kSizeWindowDefaultWidth : tokens::kSizeWindowsDefaultClientWidth;
    if (!size_for_client(capture_width, capture_height)) {
        smoke_result_.failure = "captureResizeUnavailable"; return;
    }
    rebuild();
    if (smoke_capture_stage_ == 19) {
        // Exercise the actual disclosure actions with isolated services: closing the
        // outer card must forget the technical expansion, and reopening starts compact.
        const auto* outer = scene_.find("network.disclosure");
        if (!outer || !outer->action) { smoke_result_.failure = "networkDisclosureActionMissing"; return; }
        const auto collapse = outer->action; collapse();
        if (ui_.advanced_network || ui_.technical_network || scene_.find("network.technical") || scene_.find("network.technical.summary")) {
            smoke_result_.failure = "networkDisclosureDidNotCollapse"; return;
        }
        const auto* closed = scene_.find("network.disclosure");
        if (!closed || !closed->action) { smoke_result_.failure = "networkDisclosureActionMissing"; return; }
        const auto reopen = closed->action; reopen();
        const auto* technical = scene_.find("network.technical");
        if (!ui_.advanced_network || ui_.technical_network || !technical || technical->checked || !technical->action ||
            scene_.find("network.technical.summary")) {
            smoke_result_.failure = "networkTechnicalDidNotStartCollapsed"; return;
        }
        const auto expand = technical->action; expand();
        if (!ui_.technical_network || !scene_.find("network.technical.summary")) {
            smoke_result_.failure = "networkTechnicalDidNotExpand"; return;
        }
    }
    if (smoke_capture_stage_ == 21) {
        std::vector<std::string> ribbon_ids;
        for (const auto& widget : scene_.widgets)
            if (widget.focusable && widget.id.starts_with("records.cell.")) ribbon_ids.push_back(widget.id);
        if (ribbon_ids.size() < 2) { smoke_result_.failure = "recordsSelectionFixtureUnavailable"; return; }
        const auto selection_is = [this](const std::string& key) {
            std::vector<std::string> ids;
            int selected = 0, details = 0;
            for (const auto& widget : scene_.widgets) {
                if (!widget.focusable) continue;
                if (widget.id.empty() || std::find(ids.begin(), ids.end(), widget.id) != ids.end()) return false;
                ids.push_back(widget.id);
                if (widget.id.starts_with("records.cell.") && widget.checked) {
                    ++selected;
                    if (widget.id != key) return false;
                }
                details += widget.id == "records.detail.close";
            }
            return ui_.selected_sample_key == key && selected == (key.empty() ? 0 : 1) && details == (key.empty() ? 0 : 1);
        };
        const auto click_sample = [this](const std::string& id) {
            const auto* sample = scene_.find(id);
            if (!sample || !sample->action) return false;
            const auto click = sample->action; click();
            return true;
        };
        // Different samples can share timestamps; each target must select itself
        // exclusively, and clicking it again must remove the single details panel.
        if (!click_sample(ribbon_ids[0]) || !selection_is(ribbon_ids[0]) ||
            !click_sample(ribbon_ids[1]) || !selection_is(ribbon_ids[1]) ||
            !click_sample(ribbon_ids[1]) || !selection_is({}) ||
            !click_sample(ribbon_ids[1]) || !selection_is(ribbon_ids[1])) {
            smoke_result_.failure = "recordsSelectionIdentityInvalid"; return;
        }
    }
    if (smoke_capture_stage_ == 3 || smoke_capture_stage_ == 13 || smoke_capture_stage_ == 17) scroll_by(1.0e6f);
    if (smoke_capture_stage_ == 16) if (const auto* faq = scene_.find("help.faq.wifi")) scroll_into_view(*faq);
    if (ui_.advanced_network) if (const auto* network = scene_.find("network.technical")) scroll_into_view(*network);
    if (smoke_capture_stage_ == 21) if (const auto* close = scene_.find("records.detail.close")) scroll_into_view(*close);
    if (smoke_capture_stage_ == 0) {
        const auto before = smoke_scene_builds_;
        for (int i = 0; i < 10; ++i) handle(WM_TIMER, kTimerWiFi, 0);
        const auto builds = smoke_scene_builds_ - before;
        std::printf("Idle Wi-Fi: %u scene builds for 10 timer ticks\n", builds);
        if (builds != 0) { smoke_result_.failure = "idleWifiTimerRebuiltScene." + std::to_string(builds); return; }
        if (!IsWindowVisible(hwnd_)) {
            const auto angle = spin_;
            handle(WM_TIMER, kTimerSpin, 0);
            handle(WM_TIMER, kTimerUiClock, 0);
            if (spin_ != angle || smoke_scene_builds_ != before) {
                smoke_result_.failure = "hiddenVisualTimerUpdatedScene"; return;
            }
        }
    }
    if (smoke_capture_stage_ == 0) {
        // Test real ribbon widgets without changing the displayed scene or
        // retaining another bitmap. A constrained monitor may be narrower than
        // the normal client minimum; all 24 samples must remain selectable.
        for (const float width : {72.0f, 200.0f, 320.0f, 392.0f}) {
            constexpr float left = 17, top = 11;
            constexpr std::size_t count = 24, selected = 7;
            Scene sample;
            sample.layers[0].viewport = D2D1::RectF(left, top, left + width, top + 28);
            std::vector<bool> activated(count, false);
            std::vector<RibbonCell> cells;
            for (std::size_t i = 0; i < count; ++i)
                cells.push_back({"offline.ribbon." + std::to_string(i), i % 2 ? "warning" : "success",
                    L"Offline sample " + std::to_wstring(i), i == selected, [&activated, i] { activated[i] = true; }});
            Builder builder(sample, canvas_, 0, left, width, top);
            builder.ribbon(std::move(cells));
            const auto fail = [this, width](const std::string& reason) {
                smoke_result_.failure = "ribbonLayoutInvalid." + std::to_string(static_cast<int>(width)) + "." + reason;
            };
            const auto order = sample.focus_order();
            if (sample.widgets.size() != count || order.size() != count || builder.y() != top + 28) { fail("count"); return; }
            for (std::size_t i = 0; i < count; ++i) {
                const auto& widget = sample.widgets[i];
                const auto& r = widget.rect;
                if (!std::isfinite(r.left) || !std::isfinite(r.right) || r.left < left || r.right > left + width ||
                    r.right <= r.left || r.top != top || r.bottom != top + 28 ||
                    (i == 0 && r.left != left) || (i + 1 == count && r.right != left + width) ||
                    (i > 0 && r.left <= sample.widgets[i - 1].rect.right)) { fail("geometry." + std::to_string(i)); return; }
                if (order[i] != &widget || widget.kind != Kind::panel || widget.role != Role::button ||
                    widget.id != "offline.ribbon." + std::to_string(i) || widget.name != L"Offline sample " + std::to_wstring(i) ||
                    widget.fill != (i % 2 ? "warning" : "success") || widget.checked != (i == selected) ||
                    widget.border != (i == selected ? "selectionFill" : "") || !widget.action ||
                    sample.hit((r.left + r.right) / 2, (r.top + r.bottom) / 2) != &widget) { fail("identity." + std::to_string(i)); return; }
                // The ordinary 392-DIP row retains its original cell width and gap.
                if (width == 392 && (std::abs(r.right - r.left - (width - 3 * (count - 1)) / count) > 0.01f ||
                    (i > 0 && std::abs(r.left - sample.widgets[i - 1].rect.right - 3) > 0.01f))) { fail("normalSpacing"); return; }
                widget.action();
                if (!activated[i]) { fail("action." + std::to_string(i)); return; }
            }
        }
    }
    if (smoke_capture_stage_ == 0) {
        // Exercise the real view callbacks against the isolated runtime, without
        // opening external links, reading credentials or changing formal data.
        UiState state = ui_; state.tutorial_visible = false; state.overlays.clear();
        Scene sample;
        ViewContext context{*app_, state, hwnd_, [] {},
            [&state](const std::string& name) { state.overlays = {name}; },
            [&state] { state.overlays.clear(); state.expanded_help_faq.clear(); }, [](const std::string&) {}};
        const auto rebuild = [&] { build_scene(sample, canvas_, context, 500, 700); };
        const auto invoke = [&](const std::string& id) {
            rebuild();
            const auto* widget = sample.find(id);
            if (!widget || !widget->enabled || !widget->focusable || !widget->action) {
                smoke_result_.failure = "interactionMissing." + id; return false;
            }
            const auto action = widget->action; action();
            ++smoke_result_.interaction_checks;
            return true;
        };
        const auto require = [this](bool condition, const char* reason) {
            if (!condition) smoke_result_.failure = std::string("interactionInvalid.") + reason;
            return condition;
        };
        for (const auto& [id, page] : std::vector<std::pair<std::string, Page>>{
            {"nav.records", Page::records}, {"nav.settings", Page::settings}, {"nav.connection", Page::connection}}) {
            if (!invoke(id) || !require(state.page == page, "navigation")) return;
        }
        state.overlays = {"help"};
        for (const char* key : {"wifi", "cooldown", "noInternet", "rejected", "vpn", "nonCampus", "dorm", "official"}) {
            const auto id = std::string("help.faq.") + key;
            rebuild();
            if (!require(!sample.find(id + ".answer"), "faqInitiallyClosed") || !invoke(id)) return;
            rebuild();
            if (!require(sample.find(id + ".answer") && state.expanded_help_faq.size() == 1, "faqExpanded") || !invoke(id)) return;
            rebuild();
            if (!require(!sample.find(id + ".answer") && state.expanded_help_faq.empty(), "faqCollapsed")) return;
        }
        if (!invoke("sheet.close") || !require(state.overlays.empty(), "helpClose")) return;
        state.overlays = {"about"};
        if (!invoke("sheet.close") || !require(state.overlays.empty(), "aboutClose")) return;
        state.page = Page::records;
        if (!invoke("records.failures") || !require(state.failures_only, "filterOn") ||
            !invoke("records.failures") || !require(!state.failures_only, "filterOff")) return;
        state.page = Page::connection; state.advanced_network = false; state.technical_network = false;
        if (!invoke("network.disclosure") || !require(state.advanced_network, "networkExpand") ||
            !invoke("network.technical") || !require(state.technical_network, "technicalExpand") ||
            !invoke("network.technical") || !require(!state.technical_network, "technicalCollapse") ||
            !invoke("network.disclosure") || !require(!state.advanced_network, "networkCollapse")) return;
        // Trust-related hero/banner actions navigate to the single management
        // section. Navigation itself must never reserve verification or trust.
        const auto before = app_->snapshot();
        for (const char* action : {"trust", "remember"}) {
            state.home_focus.clear(); perform_action(context, action);
            ++smoke_result_.interaction_checks;
            const auto after = app_->snapshot();
            if (!require(state.home_focus == "guard.memory" && after.trusted == before.trusted &&
                after.remembered == before.remembered && after.busy == before.busy, "trustNavigationOnly")) return;
        }
    }
    if (smoke_capture_stage_ == 0 || smoke_capture_stage_ == 10) {
        // Build auxiliary dialogs with fake display data only. They do not create
        // native fields, mutate the actual scene, or add retained screenshots.
        const float test_width = 480, test_height = smoke_capture_stage_ == 10 ? 220.0f : 540.0f;
        for (const std::string name : {"websiteResult", "wifiName", "memoryRename", "memoryForget", "accountDelete"}) {
            UiState state = ui_; state.page = Page::settings; state.tutorial_visible = false;
            state.overlays = {name}; state.network_name_id = "offline-preview";
            state.network_name_draft = L"Offline preview"; state.network_name_error.clear();
            WebsiteDetails details;
            details.check.target = {"offline-preview", "https://example.invalid/offline-check", true};
            details.check.result.state = "httpStatus"; details.check.result.status = 503;
            state.website_details = details;
            Scene sample;
            ViewContext context{*app_, state, hwnd_, [] {}, [](const std::string&) {},
                [&state] { state.overlays.clear(); }, [](const std::string&) {}};
            build_scene(sample, canvas_, context, test_width, test_height);
            const auto fail = [this, &name](const std::string& reason) { smoke_result_.failure = "auxiliaryDialogLayoutInvalid." + name + "." + reason; };
            const auto* close = sample.find("sheet.close");
            const auto* title = sample.find("sheet.title");
            const Widget* frame = nullptr;
            for (const auto& widget : sample.widgets) if (widget.layer == 1 && !widget.scrolls &&
                widget.kind == Kind::panel && widget.border == "borderStrong") frame = &widget;
            const float expected_width = name == "websiteResult" ? tokens::kSizeWebsiteDetailsWidth : tokens::kSizeNetworkNameDialogWidth;
            if (sample.top_layer() != 1 || !frame || !close || !title || !sample.layers[1].modal ||
                std::abs(frame->rect.right - frame->rect.left - expected_width) > 0.5f ||
                frame->rect.left < 0 || frame->rect.right > test_width || frame->rect.top < sample.chrome_height || frame->rect.bottom > test_height ||
                close->scrolls || close->role != Role::button || !close->focusable || !close->action ||
                std::abs(close->rect.right - close->rect.left - tokens::kSizeMinTarget) > 0.5f ||
                std::abs(close->rect.bottom - close->rect.top - tokens::kSizeMinTarget) > 0.5f ||
                title->scrolls || title->role != Role::heading || title->text.empty() ||
                title->rect.top < frame->rect.top || title->rect.bottom > sample.layers[1].viewport.top ||
                close->rect.top < frame->rect.top || close->rect.right > frame->rect.right ||
                close->rect.left <= title->rect.right || close->rect.bottom > sample.layers[1].viewport.top ||
                sample.layers[1].viewport.bottom <= sample.layers[1].viewport.top) { fail("header"); return; }
            std::vector<std::string> ids;
            for (const auto& widget : sample.widgets) {
                if (widget.layer != 1) continue;
                const auto& r = widget.rect;
                // Caption controls are intentionally drawn above modal layers;
                // they belong to the shared toolbar, not the auxiliary card.
                const bool caption_id = widget.id == "frame.minimize" || widget.id == "frame.maximize" ||
                    widget.id == "frame.close" || widget.id == "frame.edge";
                const bool caption_glyph = widget.kind == Kind::icon &&
                    (widget.icon == "minus" || widget.icon == "window-maximize" || widget.icon == "window-restore" || widget.icon == "x") &&
                    r.left >= test_width - state.caption_width;
                if (!widget.scrolls && r.top >= 0 && r.bottom <= sample.chrome_height && (caption_id || caption_glyph)) continue;
                if (!std::isfinite(r.left) || !std::isfinite(r.right) || !std::isfinite(r.top) || !std::isfinite(r.bottom) ||
                    r.left < frame->rect.left || r.right > frame->rect.right || r.right < r.left || r.bottom < r.top ||
                    (widget.scrolls && r.top < sample.layers[1].viewport.top)) { fail("geometry." + widget.id); return; }
                if (!widget.focusable) continue;
                if (widget.id.empty() || std::find(ids.begin(), ids.end(), widget.id) != ids.end() ||
                    r.right - r.left + 0.01f < tokens::kSizeMinTarget || r.bottom - r.top + 0.01f < tokens::kSizeMinTarget) {
                    fail("target." + widget.id + "." + std::to_string(r.right - r.left) + "x" + std::to_string(r.bottom - r.top)); return;
                }
                ids.push_back(widget.id);
            }
            sample.layers[1].scroll = 1.0e6f; sample.clamp_scroll(1);
            if ((smoke_capture_stage_ == 10 && sample.layers[1].scroll <= 0) ||
                sample.visible_rect(*close).top != close->rect.top || sample.visible_rect(*close).bottom != close->rect.bottom) { fail("scroll"); return; }
            const auto close_action = close->action;
            if (name == "wifiName" || name == "memoryRename") {
                const auto* field = sample.find("memory.name");
                const auto* cancel = sample.find("memory.nameCancel");
                const auto* save = sample.find("memory.nameSave");
                if (!field || field->kind != Kind::field || field->role != Role::field || !field->focusable || field->name.empty() ||
                    !cancel || cancel->role != Role::button || !cancel->action || !save || save->role != Role::button || !save->action ||
                    cancel->rect.left >= save->rect.left) { fail("fieldActions"); return; }
                const auto cancel_action = cancel->action; cancel_action();
                if (!state.overlays.empty()) { fail("cancelAction"); return; }
                state.overlays = {name};
            }
            if (name == "memoryForget" || name == "accountDelete") {
                const auto prefix = name == "memoryForget" ? "memory.forget" : "account.delete";
                const auto* cancel = sample.find(std::string(prefix) + "Cancel");
                const auto* confirm = sample.find(std::string(prefix) + "Confirm");
                const auto before = app_->snapshot();
                if (!cancel || !confirm || !cancel->action || !confirm->action || !cancel->focusable || !confirm->focusable ||
                    cancel->rect.left >= confirm->rect.left) { fail("confirmationActions"); return; }
                const auto cancel_action = cancel->action; cancel_action();
                if (!state.overlays.empty() || before.has_saved != app_->snapshot().has_saved ||
                    before.memory_count != app_->snapshot().memory_count) { fail("cancelMutation"); return; }
                state.overlays = {name};
            }
            close_action();
            if (!state.overlays.empty()) { fail("closeAction"); return; }
        }
    }
    // A resize may synchronously paint; require a fresh paint of the final scrolled scene.
    smoke_capture_ready_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::validate_smoke_scene() {
    if (!offline_smoke_) return false;
    const float width = width_dip();
    const float caption_left = width - ui_.caption_width;
    const char* caption_ids[]{"frame.minimize", "frame.maximize", "frame.close"};
    const LRESULT caption_hits[]{HTMINBUTTON, HTMAXBUTTON, HTCLOSE};
    for (int i = 0; i < 3; ++i) {
        const auto* target = scene_.find(caption_ids[i]);
        if (!target || target->focusable || target->action || target->rect.top != 0 ||
            target->rect.right - target->rect.left < tokens::kSizeMinTarget ||
            std::abs(target->rect.bottom - (ui_.caption_height - 1)) > 0.1f ||
            caption_hit((target->rect.left + target->rect.right) / 2, ui_.caption_height / 2) != caption_hits[i]) {
            smoke_result_.failure = "captionLayoutInvalid"; return false;
        }
    }
    for (const auto& widget : scene_.widgets) if (widget.layer == 0 && !widget.scrolls && widget.focusable && widget.rect.top < ui_.caption_height) {
        if (widget.rect.right > caption_left - 16 + 0.5f ||
            std::abs((widget.rect.top + widget.rect.bottom) / 2 - ui_.caption_height / 2) > 0.5f) {
            smoke_result_.failure = "toolbarCaptionOverlapOrMisalignment"; return false;
        }
    }
    const auto* brand = scene_.find("nav.brand");
    const auto* brand_icon = scene_.find("nav.brandIcon");
    const char* nav_ids[]{"nav.connection", "nav.records", "nav.settings"};
    const char* nav_icons[]{"signal", "records", "settings"};
    const auto* first_nav = scene_.find(nav_ids[0]);
    if (!first_nav || (brand && brand_icon) || (!brand && !brand_icon)) {
        smoke_result_.failure = "navigationBrandingMissingOrDuplicated"; return false;
    }
    const bool icons = !first_nav->icon.empty();
    const int tier = brand ? 2 : icons ? 1 : 0;
    if ((smoke_capture_stage_ == 0 && tier != 1) ||
        (smoke_capture_stage_ >= 4 && smoke_capture_stage_ <= 6 && tier != 2) ||
        (smoke_capture_stage_ == 10 && tier != 0)) {
        smoke_result_.failure = "navigationTierScenarioInvalid"; return false;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        const auto* nav = scene_.find(nav_ids[i]);
        if (!nav || nav->kind != Kind::nav || nav->role != Role::tab || !nav->focusable || !nav->action || nav->scrolls ||
            nav->text != T(nav_ids[i]) || nav->name != nav->text || nav->enabled != ui_.overlays.empty() ||
            nav->icon != (icons ? nav_icons[i] : "") || nav->rect.right - nav->rect.left < tokens::kSizeMinTarget ||
            nav->rect.bottom - nav->rect.top < tokens::kSizeMinTarget) {
            smoke_result_.failure = "navigationTabInvalid"; return false;
        }
        const float label_width = nav->rect.right - nav->rect.left - tokens::kSizeDesktopNavigationHorizontalPadding * 2 -
            (icons ? tokens::kSizeDesktopNavigationIconSize + tokens::kSizeDesktopNavigationIconGap : 0);
        const auto text = canvas_.measure(nav->text, nav->checked ? TextStyle::body_strong : TextStyle::body, label_width);
        if (label_width <= 0 || !std::isfinite(text.width) || !std::isfinite(text.height) || text.width > label_width + 0.5f ||
            text.height + 8 > nav->rect.bottom - nav->rect.top + 0.5f) {
            smoke_result_.failure = "navigationTextDoesNotFit"; return false;
        }
    }
    const Widget* mark = brand ? brand : brand_icon;
    if (mark->scrolls || mark->layer != 0 || mark->focusable || mark->action || mark->rect.left < 0 ||
        (first_nav->rect.top < ui_.caption_height && mark->rect.right + 8 > first_nav->rect.left) ||
        mark->rect.top < 0 || mark->rect.bottom > ui_.caption_height ||
        (brand && (!icons || brand->kind != Kind::text || brand->style != TextStyle::headline || brand->text != T("app.name"))) ||
        (brand_icon && (brand_icon->kind != Kind::icon || brand_icon->icon != "appBrand"))) {
        smoke_result_.failure = "navigationBrandingLayoutInvalid"; return false;
    }
    if (brand) for (const auto& widget : scene_.widgets) {
        if (widget.layer == 0 && !widget.scrolls && widget.kind == Kind::icon && widget.icon == "appBrand") {
            smoke_result_.failure = "navigationBrandRetainedHiddenIcon"; return false;
        }
    }
    ++smoke_result_.navigation_tiers_checked[tier];
    // Section titles retain 44-DIP information targets while their visual line
    // stays compact. The target is beside the title and shares its vertical centre.
    if (ui_.page == Page::connection && smoke_capture_stage_ != 11 && smoke_capture_stage_ < 22) {
        const auto state = app_->snapshot();
        int memory_actions = 0;
        for (const auto& widget : scene_.widgets) {
            if (widget.layer == 0 && widget.focusable &&
                (widget.text == T("memory.remember") || widget.text == T("memory.rememberTeaching"))) {
                ++memory_actions;
                if (widget.id != "guard.remember") {
                    smoke_result_.failure = "memoryVerificationHasDuplicateEntry"; return false;
                }
            }
        }
        if (scene_.find("setup.remember") || !scene_.find("guard.memory.state") ||
            memory_actions != (state.remembered || state.verifying_memory ? 0 : 1)) {
            smoke_result_.failure = "memoryVerificationEntryMissingOrDuplicated"; return false;
        }
        for (const char* id : {"home.guard", "home.account", "home.websites"}) {
            const auto* title = scene_.find(id);
            const auto* info = scene_.find(std::string(id) + ".info");
            if (!title || !info || title->role != Role::heading || title->style != TextStyle::headline ||
                !info->focusable || !info->action || info->icon != "info" ||
                std::abs(info->rect.right - info->rect.left - tokens::kSizeMinTarget) > 0.5f ||
                std::abs(info->rect.bottom - info->rect.top - tokens::kSizeMinTarget) > 0.5f ||
                std::abs((title->rect.top + title->rect.bottom) / 2 - (info->rect.top + info->rect.bottom) / 2) > 0.5f ||
                info->rect.left < title->rect.right - tokens::kSpaceXxs - 0.5f ||
                info->rect.left > title->rect.right + tokens::kSpaceXs + 0.5f) {
                smoke_result_.failure = "sectionTitleInformationSpacingInvalid"; return false;
            }
            ++smoke_result_.section_title_spacing_checked;
        }
    }
    for (const Widget& widget : scene_.widgets) {
        if (!widget.icon.empty() && widget.icon != "appBrand" && icons::path(widget.icon).empty()) {
            smoke_result_.failure = "missingIcon." + widget.id + "." + widget.icon;
            return false;
        }
        const auto& r = widget.rect;
        const bool finite = std::isfinite(r.left) && std::isfinite(r.top) && std::isfinite(r.right) && std::isfinite(r.bottom);
        const bool layer = widget.layer >= 0 && static_cast<std::size_t>(widget.layer) < scene_.layers.size();
        const bool positive = r.right >= r.left && r.bottom >= r.top &&
            (!widget.focusable || (r.right > r.left && r.bottom > r.top));
        // Vertical overflow is intentional scrolling. Interactive horizontal overflow is not.
        const bool column = !widget.focusable || (r.left >= -0.5f && r.right <= width + 0.5f);
        if (!finite || !layer || !positive || !column) {
            const std::string label = smoke_capture_stage_ < kSmokeFrameLabels.size() ? kSmokeFrameLabels[smoke_capture_stage_] : "unknown";
            smoke_result_.failure = "sceneGeometryInvalid." + label + "." + widget.id;
            smoke_result_.visual_unavailable = "sceneGeometryInvalid";
            return false;
        }
    }
    for (std::size_t i = 0; i < scene_.widgets.size(); ++i) {
        const Widget& a = scene_.widgets[i];
        if (!a.focusable) continue;
        for (std::size_t j = i + 1; j < scene_.widgets.size(); ++j) {
            const Widget& b = scene_.widgets[j];
            // Panels, text, icons and fixed-vs-scrolled controls can intentionally nest.
            // Distinct interactive peers in the same coordinate space cannot share targets.
            if (!b.focusable || a.layer != b.layer || a.scrolls != b.scrolls) continue;
            const float overlap_x = std::min(a.rect.right, b.rect.right) - std::max(a.rect.left, b.rect.left);
            const float overlap_y = std::min(a.rect.bottom, b.rect.bottom) - std::max(a.rect.top, b.rect.top);
            if (overlap_x > 0.5f && overlap_y > 0.5f) {
                const std::string label = kSmokeFrameLabels[smoke_capture_stage_];
                smoke_result_.failure = "sceneTargetsOverlap." + label + "." + a.id + "." + b.id;
                smoke_result_.visual_unavailable = "sceneTargetsOverlap";
                return false;
            }
        }
    }
    std::map<std::string, int> selections;
    for (const auto& widget : scene_.widgets) {
        if (widget.kind == Kind::radio && !widget.group.empty())
            selections[std::to_string(widget.layer) + "." + widget.group] += widget.checked ? 1 : 0;
        if (widget.kind == Kind::nav) selections["navigation"] += widget.checked ? 1 : 0;
    }
    for (const auto& [group, count] : selections) if (count != 1) {
        smoke_result_.failure = "singleChoiceSelectionInvalid." + group; return false;
    }
    const bool about_stage = smoke_capture_stage_ >= 12 && smoke_capture_stage_ <= 14;
    const bool help_stage = smoke_capture_stage_ >= 15 && smoke_capture_stage_ <= 18;
    const auto contains = [](const D2D1_RECT_F& outer, const D2D1_RECT_F& inner) {
        return inner.left >= outer.left - 0.5f && inner.right <= outer.right + 0.5f &&
            inner.top >= outer.top - 0.5f && inner.bottom <= outer.bottom + 0.5f;
    };
    const auto valid_link = [this, &contains](const std::string& id, std::string_view url, const std::wstring& label) {
        const Widget* link = scene_.find(id);
        const std::wstring canonical = win::widen(url);
        const auto expected_name = label == canonical ? canonical : label + L" · " + canonical;
        if (!link || link->layer != scene_.top_layer() || link->kind != Kind::panel ||
            link->button != ButtonStyle::link || link->role != Role::link || !link->focusable || !link->enabled || !link->action ||
            link->text != label || link->detail != canonical || link->name != expected_name ||
            link->rect.right - link->rect.left < tokens::kSizeMinTarget || link->rect.bottom - link->rect.top < tokens::kSizeMinTarget) return false;
        for (const Widget& text : scene_.widgets) {
            if (text.layer != link->layer || text.kind != Kind::text || text.text != label || !contains(link->rect, text.rect)) continue;
            const float available = text.rect.right - text.rect.left;
            const auto measured = canvas_.measure(text.text, text.style, available);
            return !text.focusable && !text.action && text.role == Role::none && text.style == TextStyle::callout_strong &&
                std::isfinite(measured.width) && std::isfinite(measured.height) && measured.width <= available + 0.5f &&
                measured.height > 0 && measured.height <= text.rect.bottom - text.rect.top + 0.5f;
        }
        return false;
    };
    if (about_stage || help_stage) {
        const auto* close = scene_.find("sheet.close");
        const auto& layer = scene_.layers.back();
        if (scene_.top_layer() != 1 || !layer.modal || !close || close->scrolls || !close->focusable || !close->action ||
            close->rect.right - close->rect.left < tokens::kSizeMinTarget || close->rect.bottom - close->rect.top < tokens::kSizeMinTarget ||
            close->rect.top < scene_.chrome_height || close->rect.bottom > layer.viewport.top ||
            close->rect.right > width || close->rect.bottom > height_dip() ||
            scene_.visible_rect(*close).top != close->rect.top || scene_.visible_rect(*close).bottom != close->rect.bottom ||
            layer.viewport.bottom <= layer.viewport.top ||
            ((smoke_capture_stage_ == 13 || smoke_capture_stage_ == 17) && layer.scroll <= 0)) {
            smoke_result_.failure = "fixedSheetHeaderInvalid"; return false;
        }
        ++smoke_result_.fixed_sheet_headers_checked;
    }
    if (about_stage) {
        const bool about_modal = ui_.overlays.size() == 1 && ui_.overlays.front() == "about" && scene_.top_layer() == 1;
        if (!about_modal || !valid_link("about.project", links::kProjectUrl, win::widen(links::kProjectUrl))) {
            smoke_result_.failure = "aboutCanonicalLinkInvalid.project"; return false;
        }
        ++smoke_result_.about_links_checked;
        for (const Link& link : kCreditLinks) {
            if (!valid_link(std::string("about.") + link.key, link.url, T(std::string("about.") + link.key + ".name"))) {
                smoke_result_.failure = "aboutCanonicalLinkInvalid." + std::string(link.key); return false;
            }
            ++smoke_result_.about_links_checked;
        }
    }
    if (help_stage) {
        if (ui_.overlays != std::vector<std::string>{"help"} || !scene_.find("help.quickStart")) {
            smoke_result_.failure = "helpStructureInvalid"; return false;
        }
        for (const auto* key : {"wifi", "cooldown", "noInternet", "rejected", "vpn", "nonCampus", "dorm", "official"}) {
            const auto id = std::string("help.faq.") + key;
            const auto* question = scene_.find(id);
            const bool expanded = smoke_capture_stage_ == 16 && std::string_view(key) == "wifi";
            const auto expected_name = T(id + ".q") + L" · " + T(expanded ? "disclosure.expanded" : "disclosure.collapsed");
            if (!question || question->role != Role::button || !question->focusable || !question->action || question->checked != expanded ||
                question->name != expected_name || question->rect.right - question->rect.left < tokens::kSizeMinTarget ||
                question->rect.bottom - question->rect.top < tokens::kSizeMinTarget ||
                static_cast<bool>(scene_.find(id + ".answer")) != expanded) {
                smoke_result_.failure = "helpFAQInvalid." + std::string(key) +
                    (question ? ".size=" + std::to_string(question->rect.right - question->rect.left) + "x" +
                        std::to_string(question->rect.bottom - question->rect.top) + ".checked=" + std::to_string(question->checked) +
                        ".name=" + std::to_string(question->name == expected_name) + ".answer=" +
                        std::to_string(static_cast<bool>(scene_.find(id + ".answer"))) : ".missing"); return false;
            }
        }
        for (const auto& link : kOfficialLinks) {
            if (!valid_link(std::string("help.link.") + link.key, link.url, T(std::string("link.") + link.key))) {
                smoke_result_.failure = "helpCanonicalLinkInvalid." + std::string(link.key); return false;
            }
        }
        ++smoke_result_.help_layouts_checked;
    }
    if (ui_.page == Page::records) {
        std::vector<std::string> interactive_ids;
        int selected_samples = 0, details_panels = 0;
        for (const auto& widget : scene_.widgets) {
            if (!widget.focusable) continue;
            if (widget.id.empty() || std::find(interactive_ids.begin(), interactive_ids.end(), widget.id) != interactive_ids.end()) {
                smoke_result_.failure = "recordsInteractiveIDNotUnique"; return false;
            }
            interactive_ids.push_back(widget.id);
            if (widget.id.starts_with("records.cell.") && widget.checked) {
                ++selected_samples;
                if (widget.id != ui_.selected_sample_key) { smoke_result_.failure = "recordsSelectedTargetMismatch"; return false; }
            }
            details_panels += widget.id == "records.detail.close";
        }
        const int expected_selection = ui_.selected_sample_key.empty() ? 0 : 1;
        if (selected_samples != expected_selection || details_panels != expected_selection) {
            smoke_result_.failure = "recordsSelectedDetailsNotExclusive"; return false;
        }
        const auto* filter = scene_.find("records.failures");
        if (!filter || filter->kind != Kind::toggle || !filter->compact || filter->role != Role::toggle || !filter->focusable || !filter->action ||
            filter->rect.bottom - filter->rect.top < tokens::kSizeMinTarget) {
            smoke_result_.failure = "recordsFilterLayoutInvalid"; return false;
        }
        const auto outer_card = [this, &contains](const Widget& text) -> const Widget* {
            const Widget* found = nullptr;
            for (const auto& card : scene_.widgets) {
                if (card.kind != Kind::panel || card.layer != text.layer || card.fill != "surface" || card.border != "border" || !contains(card.rect, text.rect)) continue;
                if (!found || card.rect.bottom - card.rect.top < found->rect.bottom - found->rect.top) found = &card;
            }
            return found;
        };
        bool checked_row = false;
        for (const auto& text : scene_.widgets) {
            if (text.layer || text.kind != Kind::text || text.style != TextStyle::caption) continue;
            bool is_stamp = false;
            if (ui_.showing_samples) for (const auto& sample : app_->health().items()) is_stamp |= text.text == win::widen(stamp_text(sample.time));
            else for (const auto& event : app_->events().items()) is_stamp |= text.text == win::widen(clock_text(event.time, true));
            if (!is_stamp) continue;
            const auto* card = outer_card(text);
            const float expected_inset = tokens::kSpaceS + (ui_.showing_samples ? tokens::kSpaceS : tokens::kSpaceM);
            if (!card || std::abs(text.rect.left - card->rect.left - expected_inset) > 0.5f) {
                smoke_result_.failure = "recordsRowPaddingInvalid"; return false;
            }
            checked_row = true;
        }
        if (!checked_row) { smoke_result_.failure = "recordsFixtureRowsUnavailable"; return false; }
        if (smoke_capture_stage_ == 21) {
            const auto* close = scene_.find("records.detail.close");
            if (ui_.selected_sample_key.empty() || !close || !close->focusable || !close->action ||
                close->rect.right - close->rect.left < tokens::kSizeMinTarget || close->rect.bottom - close->rect.top < tokens::kSizeMinTarget) {
                smoke_result_.failure = "recordsDetailLayoutInvalid"; return false;
            }
            for (const char* key : {"time", "mode", "result", "apple", "microsoft"}) {
                const auto label = T(std::string("records.detail.") + key);
                const auto found = std::find_if(scene_.widgets.begin(), scene_.widgets.end(), [&label](const Widget& widget) {
                    return widget.layer == 0 && widget.kind == Kind::text && widget.style == TextStyle::caption && widget.text == label;
                });
                if (found == scene_.widgets.end() || std::abs(found->rect.right - found->rect.left - 72) > 0.5f) {
                    smoke_result_.failure = "recordsDetailColumnsInvalid"; return false;
                }
            }
        }
        ++smoke_result_.records_layouts_checked;
    }
    if (ui_.page == Page::connection && smoke_capture_stage_ != 11 && smoke_capture_stage_ < 22 && ui_.overlays.empty()) {
        const auto* disclosure = scene_.find("network.disclosure");
        const auto* summary = scene_.find("network.summary");
        const auto* technical = scene_.find("network.technical");
        if (!disclosure || !disclosure->focusable || !disclosure->action || disclosure->checked != ui_.advanced_network ||
            disclosure->rect.bottom - disclosure->rect.top < tokens::kSizeMinTarget || !summary || summary->text.empty() ||
            static_cast<bool>(technical) != ui_.advanced_network ||
            static_cast<bool>(scene_.find("network.environment.access")) != ui_.advanced_network ||
            static_cast<bool>(scene_.find("network.environment.proxy")) != ui_.advanced_network ||
            static_cast<bool>(scene_.find("network.environment.tunnel")) != ui_.advanced_network ||
            static_cast<bool>(scene_.find("network.technical.summary")) != (ui_.advanced_network && ui_.technical_network)) {
            smoke_result_.failure = "networkDisclosureHierarchyInvalid"; return false;
        }
        if (technical && (!technical->focusable || !technical->action || technical->checked != ui_.technical_network ||
            technical->rect.bottom - technical->rect.top < tokens::kSizeMinTarget || technical->rect.top <= disclosure->rect.bottom)) {
            smoke_result_.failure = "networkTechnicalDisclosureInvalid"; return false;
        }
        const auto* local_check = scene_.find("network.check");
        if (static_cast<bool>(local_check) != (ui_.advanced_network && ui_.technical_network) ||
            (local_check && (!local_check->focusable || !local_check->action || local_check->icon.empty() ||
                local_check->rect.bottom - local_check->rect.top < tokens::kSizeMinTarget))) {
            smoke_result_.failure = "networkLocalCheckInvalid"; return false;
        }
        ++smoke_result_.network_layouts_checked;
    }
    if (smoke_capture_stage_ == 22 || smoke_capture_stage_ == 23) {
        for (const char* id : {"offline.result.long", "offline.result.endpoint"}) {
            const auto* chip = scene_.find(id);
            if (!chip || chip->kind != Kind::chip || !chip->compact || chip->name.find(L".example") == std::wstring::npos ||
                (chip->action && chip->rect.bottom - chip->rect.top + 0.01f < tokens::kSizeMinTarget)) {
                smoke_result_.failure = "resultChipFixtureInvalid." + std::string(id) +
                    (chip ? ".height=" + std::to_string(chip->rect.bottom - chip->rect.top) : ".missing"); return false;
            }
            const auto layout = result_chip_layout(canvas_, *chip, chip->rect);
            const float detail_width = canvas_.measure(chip->detail, TextStyle::caption, 10000).width;
            if (!contains(chip->rect, layout.visual) || !contains(layout.visual, layout.label) || !contains(layout.visual, layout.detail) ||
                layout.label.right <= layout.label.left || layout.detail.left < layout.label.right ||
                layout.detail.right - layout.detail.left + 0.5f < detail_width ||
                layout.visual.bottom - layout.visual.top > std::max(tokens::kSizeChipHeight, canvas_.line_height(TextStyle::caption) + 8) + 0.5f ||
                (std::string_view(id) == "offline.result.long" &&
                    canvas_.measure(chip->text, TextStyle::caption_strong, 10000).width <= layout.label.right - layout.label.left)) {
                smoke_result_.failure = "resultChipStatusOrBoundsInvalid"; return false;
            }
            if (chip->action && scene_.hit((chip->rect.left + chip->rect.right) / 2, (chip->rect.top + chip->rect.bottom) / 2) != chip) {
                smoke_result_.failure = "resultChipHitTargetInvalid"; return false;
            }
            ++smoke_result_.result_chip_layouts_checked;
        }
        // Footer wrapping is tested with fake labels independently of runtime data.
        Scene footer;
        Builder builder(footer, canvas_, 0, 24, 300, 12);
        builder.selector_row("offline.footer", L"离线布局样本 · .example · 完整检测 12:34:56 · 下次检测 12:35", {L"快速检测", L"完整检测"},
            0, [](int) {}, L"离线说明", [] {}, true, 190);
        const auto* leading = footer.widgets.empty() ? nullptr : &footer.widgets.front();
        const Widget* picker = nullptr;
        for (const auto& widget : footer.widgets) if (widget.kind == Kind::radio && (!picker || widget.rect.top < picker->rect.top)) picker = &widget;
        if (!leading || leading->kind != Kind::text || !picker || std::abs(picker->rect.top - leading->rect.bottom - tokens::kSpaceS) > 0.5f) {
            smoke_result_.failure = "stackedFooterSpacingInvalid"; return false;
        }
        ++smoke_result_.stacked_footer_layouts_checked;
    }
    ++smoke_result_.checked_layouts;
    return true;
}

void MainWindow::capture_smoke_frame(const std::string& page) {
    if (!offline_smoke_ || !app_ || !app_->offline_smoke()) return;
    const int previous_about_links = smoke_result_.about_links_checked;
    if (!validate_smoke_scene()) return;
    if (smoke_capture_stage_ == 0 && (width_dip() > tokens::kSizeWindowsDefaultClientWidth + 0.5f ||
        height_dip() > tokens::kSizeWindowsDefaultClientHeight + 0.5f)) {
        smoke_result_.failure = "initialWindowNotCompact"; return;
    }
    auto unavailable = [this](const char* reason) {
        if (smoke_result_.visual_unavailable.empty()) smoke_result_.visual_unavailable = reason;
    };
    RECT client{};
    if (!GetClientRect(hwnd_, &client)) { unavailable("clientRectUnavailable"); return; }
    const LONG width = client.right - client.left, height = client.bottom - client.top;
    if (width < 64 || height < 64 || width > 2048 || height > 2048) { unavailable("captureDimensionsUnavailable"); return; }
    const auto pixels_size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    std::size_t total = 0;
    for (const auto& frame : smoke_result_.visual_frames) total += frame.bmp.size();
    if (smoke_result_.visual_frames.size() >= kSmokeFrameLabels.size() || total + pixels_size + kSmokeBitmapHeaderBytes > kSmokeBitmapBudgetBytes) {
        unavailable("captureBudgetExceeded"); return;
    }
    HDC source = GetDC(hwnd_);
    if (!source) { unavailable("captureDCUnavailable"); return; }
    HDC destination = CreateCompatibleDC(source);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* raw_pixels = nullptr;
    HBITMAP bitmap = destination ? CreateDIBSection(source, &info, DIB_RGB_COLORS, &raw_pixels, nullptr, 0) : nullptr;
    if (!destination || !bitmap || !raw_pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (destination) DeleteDC(destination);
        ReleaseDC(hwnd_, source); unavailable("captureBitmapUnavailable"); return;
    }
    HGDIOBJ previous = SelectObject(destination, bitmap);
    if (!previous || previous == reinterpret_cast<HGDIOBJ>(static_cast<LONG_PTR>(-1))) {
        DeleteObject(bitmap); DeleteDC(destination); ReleaseDC(hwnd_, source); unavailable("captureSelectUnavailable"); return;
    }
    std::memset(raw_pixels, 0, pixels_size);
    // Runs only after EndPaint on a separate posted message. Never sends WM_PAINT/WM_PRINT recursively.
    DwmFlush();
    const bool copied = BitBlt(destination, 0, 0, width, height, source, 0, 0, SRCCOPY) != FALSE;
    GdiFlush();
    const auto* pixels = static_cast<const std::uint8_t*>(raw_pixels);
    std::size_t varied = 0, visible = 0;
    if (copied) for (std::size_t offset = 0; offset < pixels_size; offset += 4) {
        visible += pixels[offset] > 8 || pixels[offset + 1] > 8 || pixels[offset + 2] > 8;
        varied += pixels[offset] != pixels[0] || pixels[offset + 1] != pixels[1] || pixels[offset + 2] != pixels[2];
    }
    // Verify the painted selection fill independently of checked state and layout.
    int selection_samples = 0;
    bool selections_painted = true;
    // Help and About have no choices. Their background navigation is covered by the modal;
    // report zero samples for those hidden pixels.
    const bool about_label = page == "about" || page == "about-detail" || page == "about-text150";
    const bool help_label = page == "help" || page == "help-expanded" || page == "help-detail" || page == "help-text150";
    const bool selection_not_applicable = (about_label || help_label) && scene_.top_layer() == 1 &&
        ui_.overlays.size() == 1 && (ui_.overlays.front() == "about" || ui_.overlays.front() == "help") &&
        std::none_of(scene_.widgets.begin(), scene_.widgets.end(), [this](const Widget& widget) {
            return widget.layer == scene_.top_layer() && (widget.kind == Kind::nav || widget.kind == Kind::radio);
        });
    if (copied) for (const auto& widget : scene_.widgets) {
        if (!widget.checked || (widget.kind != Kind::nav && !(widget.kind == Kind::radio && widget.compact))) continue;
        if (widget.layer != scene_.top_layer()) continue;
        const auto rect = scene_.visible_rect(widget);
        const float sample_x = (rect.left + rect.right) / 2;
        const float visual_height = widget.kind == Kind::nav ? tokens::kSizeDesktopNavigationVisualHeight : tokens::kSizeDesktopPickerVisualHeight;
        const float sample_y = rect.top + (rect.bottom - rect.top - visual_height) / 2 + 3.0f;
        const auto& viewport = scene_.layers[static_cast<std::size_t>(widget.layer)].viewport;
        if (widget.scrolls && (sample_y < viewport.top || sample_y >= viewport.bottom)) continue;
        const int px = static_cast<int>(sample_x * dpi_ / 96.0f), py = static_cast<int>(sample_y * dpi_ / 96.0f);
        if (px < 0 || py < 0 || px >= width || py >= height) continue;
        const std::size_t offset = (static_cast<std::size_t>(py) * static_cast<std::size_t>(width) + static_cast<std::size_t>(px)) * 4;
        const COLORREF expected = to_colorref(widget.kind == Kind::nav ? palette_.surface : palette_.selection_fill);
        if (std::abs(static_cast<int>(pixels[offset]) - static_cast<int>(GetBValue(expected))) > 4 ||
            std::abs(static_cast<int>(pixels[offset + 1]) - static_cast<int>(GetGValue(expected))) > 4 ||
            std::abs(static_cast<int>(pixels[offset + 2]) - static_cast<int>(GetRValue(expected))) > 4) {
            selections_painted = false;
            smoke_result_.failure = "selectionHighlightMissing." + page + "." + widget.id;
            unavailable("selectionHighlightMissing"); break;
        }
        ++selection_samples;
    }
    if (selection_samples == 0 && !selection_not_applicable && smoke_result_.failure.empty()) {
        selections_painted = false; smoke_result_.failure = "selectionHighlightNotSampled." + page;
    }
    // Compare real pixels across the shared bottom edge, including all three caption zones.
    int edge_samples = 0;
    bool edge_painted = copied;
    const int edge_y = static_cast<int>((ui_.toolbar_height - 1) * dpi_ / 96.0f);
    const int ref_x = static_cast<int>(6 * dpi_ / 96.0f);
    if (edge_y < 4 || edge_y >= height || ref_x >= width) edge_painted = false;
    if (edge_painted) {
        const auto ref = (static_cast<std::size_t>(edge_y) * width + ref_x) * 4;
        const auto above = (static_cast<std::size_t>(edge_y - 4) * width + ref_x) * 4;
        bool distinct = false;
        for (int channel = 0; channel < 3; ++channel) distinct |= std::abs(static_cast<int>(pixels[ref + channel]) - pixels[above + channel]) > 4;
        edge_painted = distinct;
        for (int i = 0; i < 9 && edge_painted; ++i) {
            const float caption_left = width_dip() - ui_.caption_width;
            const float sample_x = i < 6 ? caption_left * (i + 1.0f) / 7.0f : caption_left + ui_.caption_width * (i - 6 + 0.5f) / 3.0f;
            const int x = static_cast<int>(sample_x * dpi_ / 96.0f);
            const auto sample = (static_cast<std::size_t>(edge_y) * width + x) * 4;
            for (int channel = 0; channel < 3; ++channel)
                if (std::abs(static_cast<int>(pixels[ref + channel]) - pixels[sample + channel]) > 2) edge_painted = false;
            if (edge_painted) ++edge_samples;
        }
    }
    if (!edge_painted) { smoke_result_.failure = "toolbarBottomEdgeMissing." + page; unavailable("toolbarBottomEdgeMissing"); }
    // Check the branding that was actually painted: compact tiers show the app
    // image, while the wider tier replaces it with the four-character name.
    const auto* branding = scene_.find("nav.brand");
    const bool text_brand = branding != nullptr;
    if (!branding) branding = scene_.find("nav.brandIcon");
    int branding_samples = 0;
    if (copied && branding) {
        const float scale = dpi_ / 96.0f;
        const int left = std::clamp(static_cast<int>(std::floor(branding->rect.left * scale)), 0, static_cast<int>(width));
        const int right = std::clamp(static_cast<int>(std::ceil(branding->rect.right * scale)), 0, static_cast<int>(width));
        const int top = std::clamp(static_cast<int>(std::floor(branding->rect.top * scale)), 0, static_cast<int>(height));
        const int bottom = std::clamp(static_cast<int>(std::ceil(branding->rect.bottom * scale)), 0, static_cast<int>(height));
        const COLORREF background = to_colorref(palette_.surface);
        for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
            if (std::abs(static_cast<int>(pixels[offset]) - static_cast<int>(GetBValue(background))) > 24 ||
                std::abs(static_cast<int>(pixels[offset + 1]) - static_cast<int>(GetGValue(background))) > 24 ||
                std::abs(static_cast<int>(pixels[offset + 2]) - static_cast<int>(GetRValue(background))) > 24) ++branding_samples;
        }
    }
    const bool branding_painted = branding_samples >= 12 && (text_brand ? !canvas_.app_icon_painted() : canvas_.app_icon_painted());
    if (!branding_painted) { smoke_result_.failure = "navigationBrandingNotPainted." + page; unavailable("navigationBrandingNotPainted"); }
    const auto count = pixels_size / 4;
    if (copied && selections_painted && edge_painted && branding_painted && visible > count / 20 && varied > count / 200) {
        SmokeFrame frame; frame.page = page; frame.width = static_cast<unsigned>(width); frame.height = static_cast<unsigned>(height);
        frame.client_width_dip = width_dip(); frame.client_height_dip = height_dip();
        frame.text_scale = smoke_text_scale_; frame.dark = smoke_dark_;
        frame.text_rendering = canvas_.text_rendering_info();
        frame.app_icon_painted = canvas_.app_icon_painted();
        const auto* navigation = scene_.find("nav.connection");
        frame.navigation_tier = text_brand ? "brand" : navigation && !navigation->icon.empty() ? "icons" : "text";
        frame.branding_pixel_samples = branding_samples;
        frame.selection_samples = selection_samples;
        frame.selection_check_applicable = !selection_not_applicable;
        frame.about_links_checked = smoke_result_.about_links_checked - previous_about_links;
        frame.header_edge_samples = edge_samples;
        frame.bmp.reserve(pixels_size + 54);
        auto put16 = [&frame](std::uint16_t value) { frame.bmp.push_back(static_cast<std::uint8_t>(value)); frame.bmp.push_back(static_cast<std::uint8_t>(value >> 8)); };
        auto put32 = [&frame](std::uint32_t value) { for (unsigned shift = 0; shift < 32; shift += 8) frame.bmp.push_back(static_cast<std::uint8_t>(value >> shift)); };
        put16(0x4D42); put32(static_cast<std::uint32_t>(pixels_size + 54)); put16(0); put16(0); put32(54);
        put32(40); put32(frame.width); put32(static_cast<std::uint32_t>(-static_cast<std::int32_t>(height)));
        put16(1); put16(32); put32(0); put32(static_cast<std::uint32_t>(pixels_size)); put32(0); put32(0); put32(0); put32(0);
        frame.bmp.insert(frame.bmp.end(), pixels, pixels + pixels_size);
        smoke_result_.visual_frames.push_back(std::move(frame));
    } else unavailable(copied ? "captureBlankOrUniform" : "captureCopyUnavailable");
    if (previous) SelectObject(destination, previous);
    DeleteObject(bitmap); DeleteDC(destination); ReleaseDC(hwnd_, source);
}

float MainWindow::width_dip() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    return static_cast<float>(rc.right - rc.left) * 96.0f / dpi_;
}

float MainWindow::height_dip() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    return static_cast<float>(rc.bottom - rc.top) * 96.0f / dpi_;
}

void MainWindow::request_rebuild() {
    if (in_action_) rebuild_requested_ = true;
    else rebuild();
}

void MainWindow::rebuild() {
    if (!app_ || !ctx_) return;
    if (offline_smoke_) { smoke_capture_ready_ = false; ++smoke_scene_builds_; } // A changed scene must paint before its pixels are sampled.
    if (!theme_applied_ || app_->settings().appearance != applied_appearance_) apply_theme();
    update_visual_timers();
    const float w = width_dip(), h = height_dip();
    RECT caption{};
    const float scale = dpi_ / 96.0f;
    ui_.caption_width = SUCCEEDED(DwmGetWindowAttribute(hwnd_, DWMWA_CAPTION_BUTTON_BOUNDS, &caption, sizeof caption)) && caption.right > caption.left
        ? std::clamp((caption.right - caption.left) / scale, 3 * tokens::kSizeMinTarget, 192.0f)
        : 3 * tokens::kSizeWindowsCaptionButtonWidth;
    ui_.maximized = IsZoomed(hwnd_) != FALSE;
    if (w <= 1 || h <= 1) return;
    build_scene(scene_, canvas_, *ctx_, w, h);
    update_frame_margins();
    if (offline_smoke_ && smoke_capture_stage_ == 11) {
        // An internal, fake-data-only glyph specimen. Keep the real header so
        // brand painting and selected navigation are verified in the same frame.
        std::erase_if(scene_.widgets, [](const Widget& widget) { return widget.scrolls; });
        Builder sample(scene_, canvas_, 0, 24, w - 48, 84, false);
        sample.heading(L"文字可读性样本");
        sample.text(L"字体：" + canvas_.text_rendering_info().font_family, TextStyle::caption);
        sample.gap(16);
        const std::wstring glyphs = L"十 土 士 干 千 王 三 田 目 日 口 中";
        sample.text(L"12 DIP · 小字", TextStyle::caption);
        sample.text(glyphs, TextStyle::caption, "textPrimary");
        sample.gap(16);
        sample.text(L"14 DIP · 正文", TextStyle::caption);
        sample.text(glyphs, TextStyle::body, "textPrimary");
        sample.text(L"深大随连  快速检测  校园网络  Wi-Fi 0123456789", TextStyle::body, "textPrimary");
        sample.gap(16);
        sample.text(L"14 DIP · 强调", TextStyle::caption);
        sample.text(glyphs, TextStyle::body_strong, "textPrimary");
        sample.gap(16);
        sample.text(L"18 DIP · 标题", TextStyle::caption);
        sample.text(glyphs, TextStyle::title, "textPrimary");
    }
    if (offline_smoke_ && (smoke_capture_stage_ == 22 || smoke_capture_stage_ == 23)) {
        // Long hostnames are display fixtures only. No URL is handed to a service.
        std::erase_if(scene_.widgets, [](const Widget& widget) { return widget.scrolls; });
        Builder sample(scene_, canvas_, 0, 24, w - 48, ui_.toolbar_height + 12, false);
        sample.card_begin("surface", "border", tokens::kSpaceM, tokens::kRadiusLarge);
        sample.text(L"离线布局样本 · .example 测试数据", TextStyle::caption, "textSecondary");
        sample.gap(tokens::kSpaceS);
        const std::wstring host = L"a-very-long-offline-layout-sample-with-a-long-name-and-no-network-requests.example";
        std::vector<ChipSpec> chips{
            {host + L" 访问受限", "warning", "alertTriangle", host + L"：访问受限", "offline.result.long", [] {}, host},
            {L"sample.example 检测中…", "busy", "loader", L"sample.example：检测中…", "offline.result.endpoint", {}, L"sample.example"}};
        sample.chips(std::move(chips));
        sample.card_end();
    }
    const Widget* focused = scene_.find(interaction_.focused);
    if (!focused || focused->layer != scene_.top_layer()) {
        interaction_.focused.clear();
        if (keyboard_cues_) {
            const auto order = scene_.focus_order();
            if (!order.empty()) interaction_.focused = order.front()->id;
        }
    }
    layout_edits();
    if (const Widget* headline = scene_.find("status.headline"); headline && headline->text != last_headline_) {
        last_headline_ = headline->text;
        access_->live_region("status.headline");
    }
    access_->structure_changed();
    refresh_info();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::update_visual_timers() {
    if (!app_) return;
    const bool visible = IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
    const auto snap = app_->snapshot();
    if (visible && (snap.busy || app_->system_connectivity_running()) && !reduce_motion_)
        SetTimer(hwnd_, kTimerSpin, 50, nullptr);
    else { KillTimer(hwnd_, kTimerSpin); spin_ = 0; }
    // Cooldown text must advance even when Wi-Fi recovery has no work to do.
    // The final tick also rebuilds once after expiration to refresh enabled actions.
    if (visible && !snap.online && snap.cooldown_remaining)
        SetTimer(hwnd_, kTimerUiClock, 1000, nullptr);
    else KillTimer(hwnd_, kTimerUiClock);
}

// ---------------- native text fields ----------------
HWND MainWindow::ensure_edit(const std::string& id, bool secret) {
    const auto found = edits_.find(id);
    if (found != edits_.end()) return found->second;
    DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL;
    if (secret) style |= ES_PASSWORD;
    HWND edit = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, hwnd_, nullptr, instance_, nullptr);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), TRUE);
    const WPARAM limit = id == "account.password" ? 1024 : id == "account.account" ? 256 : id == "memory.name" ? 160 : 2048;
    SendMessageW(edit, EM_SETLIMITTEXT, limit, 0);
    if (id == "internet.url" || id == "records.search") {
        const std::wstring cue = win::widen(copy::text(id == "internet.url" ? "internet.addPlaceholder" : "records.search"));
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
    } else if (id == "account.account" || id == "account.password") {
        const auto cue = win::widen(copy::text(id == "account.account" ? "dashboard.accountPlaceholder" : "account.passwordLabel"));
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
    } else if (id == "memory.name") {
        const auto cue = win::widen(copy::text(!ui_.overlays.empty() && ui_.overlays.back() == "wifiName" ?
            "wifiRecovery.windows.manualSSID.placeholder" : "memory.namePlaceholder"));
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
    }
    // Accessible name for the native control (dynamic annotation).
    if (const Widget* field = scene_.find(id)) {
        Microsoft::WRL::ComPtr<IAccPropServices> props;
        if (SUCCEEDED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(props.GetAddressOf()))))
            props->SetHwndPropStr(edit, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, PROPID_ACC_NAME, field->name.c_str());
    }
    SetWindowSubclass(edit, &MainWindow::edit_proc, 1, reinterpret_cast<DWORD_PTR>(this));
    edits_[id] = edit;
    return edit;
}

std::string MainWindow::edit_id(HWND edit) const {
    for (const auto& [id, handle] : edits_)
        if (handle == edit) return id;
    return {};
}

void MainWindow::load_edit_text(const std::string& id, HWND edit) {
    std::wstring text;
    if (id == "account.account") {
        text = win::widen(app_->draft().account);
    } else if (id == "account.password") {
        std::string password = app_->draft().password;
        text = win::widen(password);
        win::wipe(password);
    } else if (id == "internet.url") {
        text = ui_.internet_url;
    } else if (id == "memory.name") {
        text = ui_.network_name_draft;
    } else if (id == "records.search") {
        text = ui_.search;
    }
    SetWindowTextW(edit, text.c_str());
    win::wipe(text);
}

void MainWindow::layout_edits() {
    syncing_edits_ = true;
    const float s = dpi_ / 96.0f;
    const int top = scene_.top_layer();
    const bool reload_account = ui_.reload_account_inputs;
    std::set<std::string> present;
    for (const Widget& w : scene_.widgets) {
        if (w.kind != Kind::field || w.layer != top) continue;
        present.insert(w.id);
        HWND edit = ensure_edit(w.id, w.secret);
        if (!visible_edits_.count(w.id) || (reload_account && (w.id == "account.account" || w.id == "account.password"))) load_edit_text(w.id, edit);
        if (w.id == "internet.url" && ui_.clear_internet_input) {
            SetWindowTextW(edit, L""); ui_.clear_internet_input = false;
        }
        const WPARAM mask = w.secret ? 0x25CF : 0;
        if (static_cast<WPARAM>(SendMessageW(edit, EM_GETPASSWORDCHAR, 0, 0)) != mask) {
            SendMessageW(edit, EM_SETPASSWORDCHAR, mask, 0);
            InvalidateRect(edit, nullptr, TRUE);
        }
        const D2D1_RECT_F r = scene_.visible_rect(w);
        const Layer& layer = scene_.layers[static_cast<std::size_t>(top)];
        const bool visible = r.top >= layer.viewport.top && r.bottom <= layer.viewport.bottom;
        const float line = canvas_.line_height(TextStyle::body);
        EnableWindow(edit, !app_->read_only() || w.id == "records.search");
        SetWindowPos(edit, HWND_TOP, static_cast<int>((r.left + 12) * s), static_cast<int>((r.top + (r.bottom - r.top - line) / 2) * s),
                     static_cast<int>((r.right - r.left - 24) * s), static_cast<int>(line * s),
                     SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }
    for (const auto& [id, edit] : edits_) {
        if (present.count(id)) continue;
        if (GetFocus() == edit) SetFocus(hwnd_);
        ShowWindow(edit, SW_HIDE);
        if (id == "account.password") SetWindowTextW(edit, L"");  // the password never lingers in a hidden control
    }
    visible_edits_ = present;
    ui_.reload_account_inputs = false;
    syncing_edits_ = false;
}

void MainWindow::on_edit_changed(HWND edit) {
    if (syncing_edits_) return;
    const std::string id = edit_id(edit);
    if (id == "account.account" || id == "account.password") {
        const auto account_edit = edits_.find("account.account");
        const auto password_edit = edits_.find("account.password");
        const std::wstring account = account_edit != edits_.end() ? window_text(account_edit->second) : L"";
        std::wstring password = password_edit != edits_.end() ? window_text(password_edit->second) : L"";
        std::string password_utf8 = win::narrow(password);
        app_->set_draft(win::narrow(account), password_utf8);
        win::wipe(password);
        win::wipe(password_utf8);
        ui_.account_feedback.clear();
    } else if (id == "internet.url") {
        ui_.internet_url = window_text(edit);
        ui_.internet_error.clear();
    } else if (id == "memory.name") {
        ui_.network_name_draft = window_text(edit);
        ui_.network_name_error.clear();
    } else if (id == "records.search") {
        ui_.search = window_text(edit);
    }
    request_rebuild();
}

LRESULT CALLBACK MainWindow::edit_proc(HWND edit, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<MainWindow*>(ref);
    if (self->pressed_caption_ && message == WM_KEYDOWN && wparam == VK_ESCAPE) {
        self->cancel_caption_press(true);
        return 0;
    }
    switch (message) {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            self->dismiss_info();
            break;
        case WM_KEYDOWN:
            if (wparam == VK_RETURN) {
                const std::string id = self->edit_id(edit);
                if (id == "memory.name") self->activate("memory.nameSave");
                else if (id == "internet.url") self->activate("internet.add");
                else if (id == "account.account" || id == "account.password") self->activate("account.save");
                return 0;
            }
            if (wparam == VK_TAB || wparam == VK_ESCAPE || wparam == VK_F5 ||
                (GetKeyState(VK_CONTROL) < 0 && (wparam == 'Q' || wparam == 'R' || (wparam >= '1' && wparam <= '3')))) {
                self->keyboard_cues_ = true;
                self->on_key(wparam);
                return 0;
            }
            break;
        case WM_CHAR:
            if (wparam == L'\t' || wparam == L'\r' || wparam == 27) return 0;  // suppress the default beep
            if (GetKeyState(VK_CONTROL) < 0 && (wparam == 17 || wparam == 18 || (wparam >= '1' && wparam <= '3'))) return 0;
            break;
        case WM_SETFOCUS:
            self->interaction_.focused = self->edit_id(edit);
            InvalidateRect(self->hwnd_, nullptr, FALSE);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(edit, &MainWindow::edit_proc, 1);
            break;
        default: break;
    }
    return DefSubclassProc(edit, message, wparam, lparam);
}

// ---------------- theme ----------------
void MainWindow::apply_theme() {
    // Fake appearance applies solely to the isolated visual fixtures; no system setting is written.
    win::SystemAppearance sys = offline_smoke_ ? win::SystemAppearance{} : win::read_appearance();
    if (offline_smoke_) { sys.apps_dark = smoke_dark_; sys.text_scale = smoke_text_scale_; }
    const Appearance preference = app_ ? app_->settings().appearance : Appearance::system;
    const bool dark = offline_smoke_ ? smoke_dark_ : preference == Appearance::system ? sys.apps_dark : preference == Appearance::dark;
    palette_ = make_palette(dark, sys.high_contrast);
    applied_appearance_ = preference;
    theme_applied_ = true;
    reduce_motion_ = sys.reduce_motion;
    taskbar_dark_ = sys.taskbar_dark;
    canvas_.set_text_scale(sys.text_scale);
    canvas_.refresh_text_rendering();
    BOOL immersive = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &immersive, sizeof immersive);
    if (edit_brush_) DeleteObject(edit_brush_);
    edit_brush_ = CreateSolidBrush(to_colorref(palette_.surface));
    HFONT previous = edit_font_;
    const int height = -static_cast<int>(tokens::kFontBodySize * sys.text_scale * dpi_ / 96.0f + 0.5f);
    const auto edit_family = canvas_.text_rendering_info().font_family;
    edit_font_ = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, edit_family.c_str());
    for (const auto& [id, edit] : edits_) {
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font_), TRUE);
        InvalidateRect(edit, nullptr, TRUE);
    }
    if (previous) DeleteObject(previous);
}

// ---------------- interaction ----------------
void MainWindow::host_activate(const std::string& id) {
    pending_activations_.push_back(id);
    PostMessageW(hwnd_, kMsgActivateWidget, 0, 0);
}

void MainWindow::activate(const std::string& id) {
    if (offline_smoke_) return; // smoke navigation is driven internally; external links/dialogs stay unreachable
    const Widget* w = scene_.find(id);
    if (!w || !w->enabled || w->layer != scene_.top_layer()) return;
    if (w->kind == Kind::field) {
        set_focus(id, true);
        return;
    }
    if (!w->action) return;
    if (w->icon == "info") scroll_into_view(*w);
    const std::function<void()> action = w->action;  // the scene may be rebuilt while it runs
    activation_id_ = id;
    if (info_ && info_->visible() && id != info_anchor_) dismiss_info();
    in_action_ = true;
    action();
    in_action_ = false;
    activation_id_.clear();
    if (rebuild_requested_) {
        rebuild_requested_ = false;
        rebuild();
    }
    update_tray();
}

void MainWindow::set_focus(const std::string& id, bool keyboard) {
    if (info_ && info_->visible() && id != info_anchor_) dismiss_info();
    const Widget* w = scene_.find(id);
    if (!w) return;
    interaction_.focused = id;
    if (keyboard) keyboard_cues_ = true;
    if (w->kind == Kind::field) {
        if (const auto edit = edits_.find(id); edit != edits_.end()) SetFocus(edit->second);
    } else if (GetFocus() != hwnd_) {
        SetFocus(hwnd_);
    }
    scroll_into_view(*w);
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (w->kind != Kind::field) access_->focus_changed(id);
}

void MainWindow::move_focus(int delta) {
    std::vector<const Widget*> order;
    for (const Widget* w : scene_.focus_order()) {
        // One tab stop per radio group: the selected option (arrow keys move inside the group).
        if (w->role == Role::radio && !w->checked) {
            bool group_has_checked = false;
            for (const Widget* other : scene_.focus_order())
                if (other->group == w->group && other->checked) group_has_checked = true;
            if (group_has_checked) continue;
        }
        order.push_back(w);
    }
    if (order.empty()) return;
    int index = -1;
    for (std::size_t i = 0; i < order.size(); ++i)
        if (order[i]->id == interaction_.focused) index = static_cast<int>(i);
    const int count = static_cast<int>(order.size());
    const int next = index < 0 ? (delta > 0 ? 0 : count - 1) : (index + delta + count) % count;
    set_focus(order[static_cast<std::size_t>(next)]->id, true);
}

void MainWindow::scroll_into_view(const Widget& w) {
    if (!w.scrolls) return;
    Layer& l = scene_.layers[static_cast<std::size_t>(w.layer)];
    const float top = w.rect.top - l.scroll, bottom = w.rect.bottom - l.scroll;
    const float margin = 16;
    if (top < l.viewport.top + margin) l.scroll -= (l.viewport.top + margin) - top;
    else if (bottom > l.viewport.bottom - margin) l.scroll += bottom - (l.viewport.bottom - margin);
    scene_.clamp_scroll(w.layer);
    layout_edits();
}

void MainWindow::scroll_by(float dy) {
    dismiss_info();
    const int top = scene_.top_layer();
    scene_.layers[static_cast<std::size_t>(top)].scroll += dy;
    scene_.clamp_scroll(top);
    layout_edits();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::on_key(WPARAM key) {
    if (pressed_caption_) {
        if (key == VK_ESCAPE) cancel_caption_press(true);
        return true;
    }
    if (info_ && info_->visible()) {
        if (key == VK_ESCAPE) { info_->hide(true); return true; }
        if (key == VK_TAB) { info_->focus_close(); return true; }
    }
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    const Layer& layer = scene_.layers[static_cast<std::size_t>(scene_.top_layer())];
    const float page = (layer.viewport.bottom - layer.viewport.top) * 0.85f;
    switch (key) {
        case VK_TAB:
            move_focus(shift ? -1 : 1);
            return true;
        case VK_RETURN:
        case VK_SPACE:
            if (interaction_.focused.empty()) return false;
            keyboard_cues_ = true;
            activate(interaction_.focused);
            return true;
        case VK_ESCAPE:
            if (ui_.overlays.empty() && !ui_.tutorial_visible) return false;
            close_overlay();
            return true;
        case VK_LEFT:
        case VK_UP:
        case VK_RIGHT:
        case VK_DOWN: {
            const int delta = key == VK_LEFT || key == VK_UP ? -1 : 1;
            const Widget* w = scene_.find(interaction_.focused);
            if (w && w->enabled && w->step && !w->group.empty()) {
                const std::string group = w->group;
                const auto dot = w->id.rfind('.');
                const int index = std::atoi(w->id.c_str() + dot + 1);
                int count = 0;
                for (const Widget& other : scene_.widgets)
                    if (other.group == group && other.layer == w->layer) ++count;
                const std::function<void(int)> step = w->step;
                in_action_ = true;
                step(delta);
                in_action_ = false;
                rebuild_requested_ = false;
                rebuild();
                if (count > 0) set_focus(group + "." + std::to_string((index + delta + count) % count), true);
                return true;
            }
            if (key == VK_UP || key == VK_DOWN) {
                scroll_by(delta * 48.0f);
                return true;
            }
            return false;
        }
        case VK_PRIOR: scroll_by(-page); return true;
        case VK_NEXT: scroll_by(page); return true;
        case VK_HOME:
            if (!ctrl) return false;
            scroll_by(-1.0e6f);
            return true;
        case VK_END:
            if (!ctrl) return false;
            scroll_by(1.0e6f);
            return true;
        case VK_F5:
            app_->check();
            return true;
        case 'R':
            if (!ctrl) return false;
            app_->check();
            return true;
        case 'Q':
            if (!ctrl || offline_smoke_) return false;
            PostMessageW(hwnd_, WM_COMMAND, kCmdQuit, 0);
            return true;
        case '1':
        case '2':
        case '3':
            if (!ctrl || !ui_.overlays.empty()) return false;
            dismiss_info();
            ui_.show_password = false;
            ui_.page = key == '1' ? Page::connection : key == '2' ? Page::records : Page::settings;
            if (ui_.page != Page::connection) app_->cancel_compat();
            scene_.layers[0].scroll = 0;
            rebuild();
            return true;
        default: return false;
    }
}

void MainWindow::dismiss_info() {
    if (info_) info_->hide();
    info_anchor_.clear(); info_topic_.clear();
}

void MainWindow::open_info(const std::string& topic) {
    const Widget* anchor = scene_.find(activation_id_.empty() ? interaction_.focused : activation_id_);
    if (!anchor || anchor->icon != "info" || anchor->layer != scene_.top_layer()) return;
    if (info_ && info_->visible() && info_anchor_ == anchor->id && info_topic_ == topic) { dismiss_info(); return; }
    if (!info_) info_ = std::make_unique<InfoPopover>(hwnd_);
    info_anchor_ = anchor->id; info_topic_ = topic;
    interaction_.focused = anchor->id;
    refresh_info(keyboard_cues_, true);
}

void MainWindow::refresh_info(bool keyboard, bool opening) {
    if (!info_ || info_anchor_.empty() || info_topic_.empty()) return;
    if (!opening && !info_->visible()) return;
    const Widget* anchor = scene_.find(info_anchor_);
    if (!anchor || anchor->layer != scene_.top_layer() || !anchor->enabled || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) {
        dismiss_info(); return;
    }
    const auto r = scene_.visible_rect(*anchor);
    const auto viewport = scene_.layers[static_cast<std::size_t>(anchor->layer)].viewport;
    if (anchor->scrolls && (r.top < viewport.top || r.bottom > viewport.bottom)) { dismiss_info(); return; }
    D2D1_RECT_F context{r.left, r.top, r.right, r.bottom};
    float area = 1e30f;
    for (const auto& w : scene_.widgets) {
        if (w.layer != anchor->layer || w.kind != Kind::panel || w.fill != "surface") continue;
        const auto card = scene_.visible_rect(w);
        const float candidate_area = (card.right - card.left) * (card.bottom - card.top);
        if (card.left <= r.left && card.right >= r.right && card.top <= r.top && card.bottom >= r.bottom && candidate_area < area) {
            context = card; area = candidate_area;
        }
    }
    if (area == 1e30f) {
        // Some settings headings precede their card. Protect the complete
        // heading and its following text, not just the invisible click target.
        context.left = tokens::kSpaceXxl;
        context.right = width_dip() - tokens::kSpaceXxl;
        context.top = std::max(viewport.top, r.top - tokens::kSpaceL);
        context.bottom = std::min(viewport.bottom, r.bottom + 2 * tokens::kSizeMinTarget);
    }
    // A long expanded network panel can fill the display. Protect its explained
    // header and nearby text rather than pretending the whole screen is unavailable.
    if (context.bottom - context.top > height_dip() * 0.65f) {
        context.top = std::max(viewport.top, r.top - tokens::kSpaceL);
        context.bottom = std::min(viewport.bottom, r.bottom + 2 * tokens::kSizeMinTarget);
    }
    auto screen_rect = [this](D2D1_RECT_F d) {
        POINT origin{}; ClientToScreen(hwnd_, &origin);
        const float s = dpi_ / 96.0f;
        return RECT{origin.x + static_cast<LONG>(std::floor(d.left * s)), origin.y + static_cast<LONG>(std::floor(d.top * s)),
            origin.x + static_cast<LONG>(std::ceil(d.right * s)), origin.y + static_cast<LONG>(std::ceil(d.bottom * s))};
    };
    const auto title = win::widen(copy::text("info." + info_topic_ + ".title"));
    const auto body = win::widen(copy::text("info." + info_topic_ + ".body"));
    if (!info_->show(title, body, screen_rect(r), screen_rect(context), palette_, dpi_, canvas_.text_rendering_info().text_scale, keyboard)) dismiss_info();
}

void MainWindow::open_overlay(const std::string& name) {
    if (name.rfind("info:", 0) == 0) { open_info(name.substr(5)); return; }
    dismiss_info();
    if (name == "tutorial") {
        ui_.overlays.clear(); ui_.tutorial_visible = true; ui_.tutorial_step = 1;
        ui_.tutorial_entered_at = AppController::wall_now();
        focus_tutorial(*ctx_); return;
    }
    if (name == "account" || name == "guard" || name == "website" || name == "network") {
        ui_.overlays.clear();
        ui_.page = Page::connection;
        ui_.home_focus = name == "website" ? "home.websites" : "home." + name;
        if (name == "network") { ui_.advanced_network = true; ui_.technical_network = true; }
        ui_.show_password = false;
        interaction_.focused.clear();
        request_rebuild();
        return;
    }

    if (!ui_.overlays.empty() && ui_.overlays.back() == name) return;
    if (std::find(ui_.overlays.begin(), ui_.overlays.end(), name) != ui_.overlays.end()) return;
    ui_.overlays.push_back(name);
    if (name == "help") ui_.expanded_help_faq.clear();
    if (name == "account") {
        ui_.account_feedback.clear();
        ui_.show_password = false;
    }
    interaction_.focused.clear();
    request_rebuild();
}

void MainWindow::close_overlay() {
    if (ui_.overlays.empty()) {
        if (ui_.tutorial_visible) {
            ui_.tutorial_visible = false;
            if (app_->settings().tutorial == "none") app_->set_tutorial("skipped");
            request_rebuild();
        }
        return;
    }
    const std::string top = ui_.overlays.back();
    if (top == "account") ui_.show_password = false;
    if (top == "network") app_->cancel_compat();
    ui_.overlays.pop_back();
    interaction_.focused.clear();
    request_rebuild();
}

void MainWindow::update_tray() {
    if (!app_) return;
    const EngineSnapshot snap = app_->snapshot();
    std::string state = Tray::state_for(snap);
    const std::string key = state == "paused" ? "attention" : state;
    std::string status_text(copy::text("tray.state." + key));
    if (!snap.busy && !(snap.paused && snap.mode == MonitoringMode::recover) && snap.network_available && !snap.sleeping) {
        if (const auto report = app_->internet_report()) {
            const auto verdict = report->verdict();
            status_text = std::string(copy::text("internet." + std::string(to_string(verdict)) + ".title"));
            state = verdict == InternetVerdict::allPassed || verdict == InternetVerdict::basicPassed ? "online" : "attention";
        } else if (snap.online) {
            status_text = std::string(copy::text("internet.pending.title")); state = "idle";
        }
    }
    const auto system = app_->system_connectivity();
    if (status::system_path_can_lead(snap) && system && system->online() && !snap.online) {
        status_text = std::string(copy::text("env.systemOnline.title")); state = "online";
    }
    if (read_only_) status_text = std::string(copy::text("settings.readOnlyMode")) + " · " + status_text;
    tray_.update(state, taskbar_dark_, win::widen(copy::format("tray.tooltip", {{"status", status_text}})));
}

void MainWindow::show_and_focus() {
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    rebuild();
    SetForegroundWindow(hwnd_);
}

// ---------------- message handling ----------------
LRESULT MainWindow::caption_hit(float x, float y) const {
    const float left = width_dip() - ui_.caption_width;
    if (x < left || x >= width_dip() || y < 0 || y >= ui_.caption_height - 1) return HTCLIENT;
    const LRESULT hits[]{HTMINBUTTON, HTMAXBUTTON, HTCLOSE};
    return hits[std::clamp(static_cast<int>((x - left) / (ui_.caption_width / 3)), 0, 2)];
}

void MainWindow::cancel_caption_press(bool restore_focus) {
    const HWND previous_focus = caption_focus_;
    caption_focus_ = nullptr;
    pressed_caption_ = 0; ui_.pressed_caption = 0; ui_.hovered_caption = 0;
    if (GetCapture() == hwnd_) ReleaseCapture();
    // A release/Escape restores the original input and selection. Never steal focus
    // back after deactivation, another window taking capture, or the input disappearing.
    if (restore_focus && previous_focus && GetFocus() == hwnd_ && GetForegroundWindow() == hwnd_ && IsWindowVisible(hwnd_) &&
        IsChild(hwnd_, previous_focus) && IsWindowVisible(previous_focus) && IsWindowEnabled(previous_focus)) SetFocus(previous_focus);
    if (app_) request_rebuild();
}

void MainWindow::update_frame_margins() {
    const int height = static_cast<int>(std::ceil(ui_.toolbar_height * dpi_ / 96.0f));
    if (height == frame_height_px_) return;
    const MARGINS margins{0, 0, height, 0};
    DwmExtendFrameIntoClientArea(hwnd_, &margins);
    frame_height_px_ = height;
}

LRESULT MainWindow::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    // Give DWM the first opportunity to process standard caption hit testing and hover.
    LRESULT dwm_hit = 0;
    const bool dwm_handled = (message == WM_NCHITTEST || message == WM_NCMOUSEMOVE || message == WM_NCMOUSELEAVE) &&
        DwmDefWindowProc(hwnd_, message, wparam, lparam, &dwm_hit);
    if (message == WM_NCHITTEST) {
        const auto native = DefWindowProcW(hwnd_, message, wparam, lparam);
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(hwnd_, &point);
        if (!IsZoomed(hwnd_)) {
            if (native == HTLEFT || native == HTRIGHT || native == HTBOTTOM || native == HTBOTTOMLEFT ||
                native == HTBOTTOMRIGHT || native == HTTOPLEFT || native == HTTOPRIGHT) return native;
            if (point.y < MulDiv(4, static_cast<int>(dpi_), 96)) return HTTOP;
        }
        const float x = point.x * 96.0f / dpi_, y = point.y * 96.0f / dpi_;
        const auto caption = caption_hit(x, y);
        if (caption != HTCLIENT) return dwm_handled && dwm_hit == caption ? dwm_hit : caption;
        if (app_) {
            // Disabled modal navigation is still a control, never a drag surface.
            for (const auto& widget : scene_.widgets) if (!widget.scrolls && widget.layer == 0 && widget.focusable &&
                x >= widget.rect.left && x < widget.rect.right && y >= widget.rect.top && y < widget.rect.bottom) return HTCLIENT;
            if (scene_.hit(x, y)) return HTCLIENT;
            if (y >= 0 && y < ui_.toolbar_height) return HTCAPTION;
        }
        return dwm_handled && dwm_hit != HTMINBUTTON && dwm_hit != HTMAXBUTTON && dwm_hit != HTCLOSE ? dwm_hit : HTCLIENT;
    }
    if (message == WM_NCLBUTTONDOWN && (wparam == HTMINBUTTON || wparam == HTMAXBUTTON || wparam == HTCLOSE)) {
        const HWND focused = GetFocus();
        caption_focus_ = !edit_id(focused).empty() && IsWindowVisible(focused) && IsWindowEnabled(focused) ? focused : nullptr;
        if (GetFocus() != hwnd_) SetFocus(hwnd_); // Track Escape/characters on the host, then restore the input on release.
        interaction_.pressed.clear();
        keyboard_cues_ = false; pressed_caption_ = wparam; ui_.pressed_caption = wparam; ui_.hovered_caption = wparam;
        SetCapture(hwnd_); if (app_) request_rebuild(); return 0;
    }
    if ((message == WM_LBUTTONUP || message == WM_NCLBUTTONUP) && pressed_caption_) {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (message == WM_NCLBUTTONUP) ScreenToClient(hwnd_, &point);
        const auto pressed = pressed_caption_;
        const bool release_inside = caption_hit(point.x * 96.0f / dpi_, point.y * 96.0f / dpi_) == static_cast<LRESULT>(pressed);
        cancel_caption_press(true);
        if (release_inside) PostMessageW(hwnd_, WM_SYSCOMMAND,
            pressed == HTMINBUTTON ? SC_MINIMIZE : pressed == HTCLOSE ? SC_CLOSE : IsZoomed(hwnd_) ? SC_RESTORE : SC_MAXIMIZE, 0);
        return 0;
    }
    if (message == WM_MOUSEMOVE && pressed_caption_) {
        const auto hit = caption_hit(GET_X_LPARAM(lparam) * 96.0f / dpi_, GET_Y_LPARAM(lparam) * 96.0f / dpi_);
        const WPARAM hovered = hit == HTCLIENT ? 0 : hit;
        if (ui_.hovered_caption != hovered) { ui_.hovered_caption = hovered; if (app_) request_rebuild(); }
        return 0;
    }
    if (pressed_caption_ && (message == WM_CANCELMODE || message == WM_CAPTURECHANGED || message == WM_KILLFOCUS ||
        (message == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE) || (message == WM_KEYDOWN && wparam == VK_ESCAPE))) {
        cancel_caption_press(message == WM_KEYDOWN);
        if (message == WM_KEYDOWN) return 0;
    }
    if (message == WM_NCMOUSEMOVE || message == WM_NCMOUSELEAVE) {
        if (message == WM_NCMOUSEMOVE) { TRACKMOUSEEVENT track{sizeof track, TME_LEAVE | TME_NONCLIENT, hwnd_, 0}; TrackMouseEvent(&track); }
        const WPARAM hovered = message == WM_NCMOUSEMOVE && (wparam == HTMINBUTTON || wparam == HTMAXBUTTON || wparam == HTCLOSE) ? wparam : 0;
        if (ui_.hovered_caption != hovered) { ui_.hovered_caption = hovered; if (app_) request_rebuild(); }
    }
    switch (message) {
        case WM_NCCALCSIZE:
            if (wparam) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
                const LONG top = params->rgrc[0].top;
                DefWindowProcW(hwnd_, message, wparam, lparam);
                params->rgrc[0].top = top + (IsZoomed(hwnd_) ? GetSystemMetricsForDpi(SM_CYFRAME, static_cast<UINT>(dpi_)) +
                    GetSystemMetricsForDpi(SM_CXPADDEDBORDER, static_cast<UINT>(dpi_)) : 0);
                return 0;
            }
            break;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE && info_ && reinterpret_cast<HWND>(lparam) != info_->host_window()) dismiss_info();
            frame_height_px_ = 0; update_frame_margins(); break;
        case WM_CREATE:
            return on_created() ? 0 : -1;
        case WM_MOVE:
            if (app_) refresh_info();
            break;
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_NCRBUTTONDOWN:
            dismiss_info();
            break;
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) dismiss_info();
            canvas_.resize(LOWORD(lparam), HIWORD(lparam));
            if (app_) rebuild();
            return 0;
        case WM_SHOWWINDOW:
            if (!wparam) { KillTimer(hwnd_, kTimerSpin); KillTimer(hwnd_, kTimerUiClock); spin_ = 0; }
            break;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = MulDiv(480, static_cast<int>(dpi_), 96);
            // Offline capture uses a shorter viewport to stay within its 48 MiB budget
            // at real DPI. The normal window minimum remains 540 DIP.
            info->ptMinTrackSize.y = MulDiv(offline_smoke_ ? 200 : 540, static_cast<int>(dpi_), 96);
            MONITORINFO monitor{}; monitor.cbSize = sizeof monitor;
            if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
                const int margin = 2 * MulDiv(8, static_cast<int>(dpi_), 96);
                info->ptMinTrackSize.x = std::min(info->ptMinTrackSize.x, std::max<LONG>(1, monitor.rcWork.right - monitor.rcWork.left - margin));
                info->ptMinTrackSize.y = std::min(info->ptMinTrackSize.y, std::max<LONG>(1, monitor.rcWork.bottom - monitor.rcWork.top - margin));
            }
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = static_cast<float>(HIWORD(wparam));
            canvas_.set_dpi(dpi_);
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            apply_theme();
            rebuild();
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            bool painted = false;
            if (app_ && canvas_.begin()) {
                Interaction shown = interaction_;
                if (!keyboard_cues_) {
                    const Widget* f = scene_.find(shown.focused);
                    if (!f || f->kind != Kind::field) shown.focused.clear();
                }
                paint(canvas_, palette_, scene_, shown, width_dip(), height_dip(), spin_);
                if (!canvas_.end()) InvalidateRect(hwnd_, nullptr, FALSE);
                else {
                    painted = true;
                    if (offline_smoke_) {
                        ++smoke_result_.render_frames;
                        smoke_result_.scene_widgets = std::max(smoke_result_.scene_widgets, static_cast<int>(scene_.widgets.size()));
                        const std::string page = ui_.page == Page::connection ? "connection" : ui_.page == Page::records ? "records" : "settings";
                        auto& pages = smoke_result_.native_painted_pages;
                        if (std::find(pages.begin(), pages.end(), page) == pages.end()) pages.push_back(page);
                        if (app_->fake_auth_passed() && !app_->snapshot().busy) smoke_capture_ready_ = true;
                        PostMessageW(hwnd_, WM_APP + 10, 0, 0);
                    }
                }
            }
            EndPaint(hwnd_, &ps);
            if (painted) for (const auto& [id, edit] : edits_) {
                if (!visible_edits_.count(id) || !IsWindowVisible(edit)) continue;
                // Native EDIT areas are excluded from the parent's D2D paint. A newly shown
                // child can otherwise retain pixels from the previous page until its own
                // low-priority WM_PAINT runs. Repaint only those children after parent EndPaint;
                // UpdateWindow(edit) neither reenters parent WM_PAINT nor changes keyboard focus.
                InvalidateRect(edit, nullptr, TRUE);
                UpdateWindow(edit);
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            if (ui_.hovered_caption) { ui_.hovered_caption = 0; if (app_) request_rebuild(); }
            if (!tracking_mouse_) {
                TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&track);
                tracking_mouse_ = true;
            }
            const float s = dpi_ / 96.0f;
            const Widget* w = scene_.hit(static_cast<float>(GET_X_LPARAM(lparam)) / s, static_cast<float>(GET_Y_LPARAM(lparam)) / s);
            const std::string id = w ? w->id : std::string();
            if (id != interaction_.hovered) {
                interaction_.hovered = id;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_mouse_ = false;
            if (!interaction_.hovered.empty()) {
                interaction_.hovered.clear();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && !interaction_.hovered.empty()) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN: {
            const float s = dpi_ / 96.0f;
            keyboard_cues_ = false;
            if (GetFocus() != hwnd_) SetFocus(hwnd_);
            const Widget* w = scene_.hit(static_cast<float>(GET_X_LPARAM(lparam)) / s, static_cast<float>(GET_Y_LPARAM(lparam)) / s);
            if (info_ && info_->visible() && (!w || w->id != info_anchor_)) dismiss_info();
            if (w) {
                interaction_.pressed = w->id;
                interaction_.focused = w->id;
                SetCapture(hwnd_);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            const float s = dpi_ / 96.0f;
            const Widget* w = scene_.hit(static_cast<float>(GET_X_LPARAM(lparam)) / s, static_cast<float>(GET_Y_LPARAM(lparam)) / s);
            const std::string pressed = interaction_.pressed;
            interaction_.pressed.clear();
            if (w && !pressed.empty() && w->id == pressed) activate(pressed);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL:
            scroll_by(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<float>(WHEEL_DELTA) * 56.0f);
            return 0;
        case WM_KEYDOWN:
            if (on_key(wparam)) return 0;
            break;
        case WM_COMMAND:
            if (offline_smoke_) return 0;
            if (lparam && HIWORD(wparam) == EN_CHANGE) {
                on_edit_changed(reinterpret_cast<HWND>(lparam));
                return 0;
            }
            switch (LOWORD(wparam)) {
                case kCmdOpen: show_and_focus(); return 0;
                case kCmdCheck: app_->check(); return 0;
                case kCmdLogin: app_->connect(); return 0;
                case kCmdPause: app_->pause(); rebuild(); update_tray(); return 0;
                case kCmdResume: app_->resume(); rebuild(); update_tray(); return 0;
                case kCmdQuit:
                    quitting_ = true;
                    DestroyWindow(hwnd_);
                    return 0;
                default: break;
            }
            break;
        case WM_CTLCOLORSTATIC:
            if (edit_id(reinterpret_cast<HWND>(lparam)).empty()) break;
            [[fallthrough]];
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, to_colorref(IsWindowEnabled(reinterpret_cast<HWND>(lparam)) ? palette_.text_primary : palette_.text_tertiary));
            SetBkColor(dc, to_colorref(palette_.surface));
            return reinterpret_cast<LRESULT>(edit_brush_);
        }
        case WM_TIMER:
            if (offline_smoke_ && wparam == 20) {
                smoke_result_.failure = "timeout"; smoke_step(); return 0;
            }
            if (wparam == kTimerSpin) {
                if (!IsWindowVisible(hwnd_) || IsIconic(hwnd_)) { update_visual_timers(); return 0; }
                spin_ = std::fmod(spin_ + 18.0f, 360.0f);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == kTimerFeedback) {
                KillTimer(hwnd_, kTimerFeedback);
                ui_.copied.clear();
                rebuild();
                return 0;
            }
            if (wparam == kTimerUiClock) {
                if (!IsWindowVisible(hwnd_) || IsIconic(hwnd_)) { update_visual_timers(); return 0; }
                rebuild();
                update_tray();
                return 0;
            }
            if (app_) app_->on_timer(wparam);
            if (wparam == kTimerWiFi) return 0; // Actual recovery/status changes post kMsgChanged.
            rebuild();
            update_tray();
            return 0;
        case WM_POWERBROADCAST:
            if (app_) {
                if (wparam == PBT_APMSUSPEND) { dismiss_info(); app_->on_suspend(); }
                else if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) app_->on_resume();
                rebuild();
            }
            return TRUE;
        case WM_SETTINGCHANGE:
        case WM_DISPLAYCHANGE:
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            if (app_) {
                apply_theme();
                rebuild();
                update_tray();
            }
            break;
        case WM_GETOBJECT:
            if (access_) {
                const LRESULT result = access_->on_get_object(wparam, lparam);
                if (result) return result;
            }
            break;
        case WM_CLOSE:
            dismiss_info();
            if (offline_smoke_) { smoke_result_.failure = "closedEarly"; smoke_step(); return 0; }
            if (!quitting_) {
                ShowWindow(hwnd_, SW_HIDE);  // keeps guarding from the tray; "退出" in the tray menu quits
                return 0;
            }
            break;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wparam && app_) {
                quitting_ = true;
                app_->shutdown();
            }
            return 0;
        case WM_DESTROY:
            dismiss_info();
            info_.reset();
            tray_.remove();
            if (power_) {
                UnregisterSuspendResumeNotification(power_);
                power_ = nullptr;
            }
            if (access_) access_->disconnect();
            if (app_) { app_->shutdown(); smoke_result_.fake_auth_passed = app_->fake_auth_passed(); }
            smoke_result_.clean_shutdown = true;
            KillTimer(hwnd_, 20);
            PostQuitMessage(exit_code_);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            break;
        default:
            if (offline_smoke_ && message == WM_APP + 10) { smoke_step(); return 0; }
            if (message == kMsgChanged && app_) {
                app_->on_changed();
                rebuild();
                update_tray();
                return 0;
            }
            if (message == kMsgNetwork && app_) {
                app_->on_network_signal();
                return 0;
            }
            if (message == kMsgActivateWidget) {
                while (!pending_activations_.empty()) {
                    const std::string id = pending_activations_.front();
                    pending_activations_.pop_front();
                    activate(id);
                }
                return 0;
            }
            if (message == kMsgTray && app_) {
                const UINT event = LOWORD(lparam);
                if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONUP) show_and_focus();
                else if (event == WM_CONTEXTMENU) tray_.show_menu(hwnd_, app_->snapshot(), GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam));
                return 0;
            }
            if (message == win::SingleInstance::activation_message()) {
                show_and_focus();
                return 0;
            }
            if (taskbar_created_ != 0 && message == taskbar_created_) {
                tray_.add(hwnd_, instance_);
                update_tray();
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

}  // namespace szu::app
