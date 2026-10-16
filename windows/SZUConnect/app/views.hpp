// Page, sheet and tutorial builders. They only read the controller and UiState and describe widgets;
// actions call back into the controller or change UiState and ask the window to rebuild.
#pragma once
#include <functional>
#include <array>
#include <string>
#include <vector>

#include "controller.hpp"
#include "scene.hpp"
#include "szu/presentation.hpp"

namespace szu::app {

enum class Page { connection, records, settings };
inline constexpr std::array<const char*, 11> kGuideSteps{"welcome", "account", "detection", "login", "memory", "recovery", "websites", "network", "startup", "wifi", "finish"};

struct WebsiteDetails {
    WebsiteCheck check;
    bool follow_system_proxy = true;
};

struct UiState {
    Page page = Page::connection;
    float caption_width = 138;
    float caption_height = 56;
    float toolbar_height = 56;
    WPARAM hovered_caption = 0;
    WPARAM pressed_caption = 0;
    bool maximized = false;
    std::vector<std::string> overlays;  // modal sheets above the real page and optional guide
    int tutorial_step = 1;              // one-based index into kGuideSteps
    bool tutorial_visible = false;
    double tutorial_entered_at = 0;
    bool show_password = false;
    bool reload_account_inputs = false;
    bool failures_only = false;
    bool advanced_network = false;
    bool technical_network = false;
    bool setup_expanded = true;
    std::vector<std::string> expanded_help_faq;  // session-only disclosure state; answers start collapsed
    bool showing_samples = false;
    std::wstring search;                // mirrors the records search EDIT
    std::string network_name_id;
    std::wstring network_name_draft;
    std::string network_name_error;
    std::wstring internet_url;          // add-URL draft only; selected sites live in controller config
    std::optional<WebsiteDetails> website_details;  // completed result captured when its chip is opened
    std::string internet_error;
    bool clear_internet_input = false;
    std::string home_focus;             // route old entry points to one inline home section
    std::string account_feedback;       // copy key
    std::string records_feedback;       // copy key
    std::string copied;                 // widget id currently showing "已复制"
    std::string selected_sample_key;   // session-only sample fingerprint + duplicate ordinal; empty = none
};

struct ViewContext {
    AppController& app;
    UiState& ui;
    HWND hwnd;
    std::function<void()> refresh;                   // rebuild + repaint after UiState changes
    std::function<void(const std::string&)> open;    // push an overlay
    std::function<void()> close;                     // pop the top overlay
    std::function<void(const std::string&)> copied;  // show "已复制" on a widget for a moment
};

struct Link {
    const char* key;
    const char* url;
};
extern const Link kOfficialLinks[11];
extern const Link kCreditLinks[1];

void build_scene(Scene& scene, Canvas& canvas, ViewContext& ctx, float width, float height);

// helpers shared by the view files
std::wstring T(std::string_view key);
std::wstring F(std::string_view key, const std::vector<std::pair<std::string_view, std::string>>& values);
std::string U(std::string_view key);  // UTF-8 copy text
std::string clock_text(double unix_seconds, bool with_date = false);
std::string stamp_text(double unix_seconds);   // "MM-DD HH:MM:SS"
std::string clock_seconds(double unix_seconds); // "HH:MM:SS"
std::wstring info_label(const std::string& topic);  // accessible name of a section ⓘ button
void perform_action(ViewContext& ctx, const std::string& action);
void build_page(Builder& b, ViewContext& ctx, const EngineSnapshot& snap, const status::Presentation& p);
void build_inline_account(Builder& b, ViewContext& ctx, const EngineSnapshot& snap);
void build_inline_guard(Builder& b, ViewContext& ctx, const EngineSnapshot& snap);
void build_network_details(Builder& b, ViewContext& ctx, const EngineSnapshot& snap);
void build_overlay(Builder& b, ViewContext& ctx, const std::string& name, const EngineSnapshot& snap, const status::Presentation& p);
void build_tutorial_coach(Builder& b, ViewContext& ctx, const EngineSnapshot& snap, const status::Presentation& p);
void focus_tutorial(ViewContext& ctx);

}  // namespace szu::app
