#pragma once
#include "szu/win/text.hpp"

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "accessibility.hpp"
#include "canvas.hpp"
#include "controller.hpp"
#include "scene.hpp"
#include "theme.hpp"
#include "tray.hpp"
#include "views.hpp"
#include "offline_smoke.hpp"
#include "info_popover.hpp"

namespace szu::app {

inline constexpr wchar_t kWindowClass[] = L"SZUConnect.MainWindow.v1";

class MainWindow final : public AccessHost {
public:
    MainWindow(HINSTANCE instance, bool read_only, bool background, bool offline_smoke = false, int timeout_ms = 10000);
    ~MainWindow() override;
    bool create();
    void show();
    int run();
    const SmokeResult& smoke_result() const { return smoke_result_; }
    const RuntimeEvidence* runtime_evidence() const { return app_ ? &app_->runtime_evidence() : nullptr; }

    HWND host_window() const override { return hwnd_; }
    const Scene& host_scene() const override { return scene_; }
    float host_scale() const override { return dpi_ / 96.0f; }
    std::string host_focus() const override { return interaction_.focused; }
    bool host_interactive() const override { return hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_) && !scene_dirty_; }
    void host_set_focus(const std::string& id) override { if (host_interactive()) set_focus(id, true); }
    void host_activate(const std::string& id) override;

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK edit_proc(HWND edit, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR ref);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    bool on_created();
    bool size_for_client(float width, float height);
    void smoke_step();
    void prepare_smoke_scene();
    bool validate_smoke_scene();
    bool validate_smoke_dpi_matrix();
    void capture_smoke_frame(const std::string& page);
    void rebuild(bool prepare_hidden = false);
    void update_visual_timers();
    void request_rebuild();
    void layout_edits();
    HWND ensure_edit(const std::string& id, bool secret);
    void load_edit_text(const std::string& id, HWND edit);
    void on_edit_changed(HWND edit);
    std::string edit_id(HWND edit) const;
    void apply_theme();
    void activate(const std::string& id);
    void set_focus(const std::string& id, bool keyboard);
    void move_focus(int delta);
    void scroll_into_view(const Widget& w);
    void scroll_by(float dy);
    bool on_key(WPARAM key);
    void open_overlay(const std::string& name);
    void close_overlay();
    void open_info(const std::string& topic);
    void refresh_info(bool keyboard = false, bool opening = false);
    void dismiss_info();
    void update_tray();
    void show_and_focus();
    void present_window(bool focus);
    void hide_to_tray();
    bool paint_frame(bool smoke_progress = true);
    bool verify_smoke_window_presentation();
    bool verify_smoke_caption_feedback();
    float width_dip() const;
    float height_dip() const;
    LRESULT caption_hit(float x, float y) const;
    void cancel_caption_press(bool restore_focus = false);
    void repaint_caption();
    void update_frame_margins();

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    bool read_only_;
    bool background_;
    bool offline_smoke_;
    int smoke_timeout_ms_;
    SmokeResult smoke_result_;
    unsigned smoke_capture_stage_ = 0;
    bool smoke_capture_ready_ = false;
    unsigned smoke_scene_builds_ = 0;
    bool smoke_dark_ = false;
    float smoke_text_scale_ = 1.0f;
    int exit_code_ = 0;
    float dpi_ = 96.0f;
    Canvas canvas_;
    Scene scene_;
    Interaction interaction_;
    UiState ui_;
    Palette palette_ = make_palette(false, false);
    std::unique_ptr<AppController> app_;
    std::unique_ptr<ViewContext> ctx_;
    std::unique_ptr<Accessibility> access_;
    std::unique_ptr<InfoPopover> info_;
    std::string info_topic_, info_anchor_, activation_id_;
    std::map<std::string, HWND> edits_;
    std::set<std::string> visible_edits_;
    HFONT edit_font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    bool in_action_ = false;
    bool rebuild_requested_ = false;
    bool scene_dirty_ = true;
    bool syncing_edits_ = false;
    bool keyboard_cues_ = false;
    bool reduce_motion_ = false;
    bool taskbar_dark_ = true;
    bool quitting_ = false;
    Appearance applied_appearance_ = Appearance::system;
    bool theme_applied_ = false;
    bool tracking_mouse_ = false;
    WPARAM pressed_caption_ = 0;
    HWND caption_focus_ = nullptr;
    int frame_height_px_ = 0;
    Tray tray_;
    HPOWERNOTIFY power_ = nullptr;
    UINT taskbar_created_ = 0;
    std::wstring last_headline_;
    float spin_ = 0;
    std::deque<std::string> pending_activations_;
};

}  // namespace szu::app
