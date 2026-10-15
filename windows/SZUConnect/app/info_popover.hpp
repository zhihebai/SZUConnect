// Small owned explanation window. It never disables or dims its owner.
#pragma once
#include "accessibility.hpp"
#include "info_popover_placement.hpp"

namespace szu::app {
class InfoPopover final : public AccessHost {
public:
    explicit InfoPopover(HWND owner);
    ~InfoPopover() override;
    bool show(std::wstring title, std::wstring body, RECT anchor, RECT context,
              const Palette& palette, float dpi, float text_scale, bool keyboard);
    void hide(bool restore_focus = false);
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    HWND host_window() const override { return hwnd_; }
    std::wstring host_automation_id() const override { return L"SZUConnect.InfoPopover"; }
    const Scene& host_scene() const override { return scene_; }
    float host_scale() const override { return dpi_ / 96.0f; }
    std::string host_focus() const override;
    void host_set_focus(const std::string& id) override;
    void host_activate(const std::string& id) override;
    void focus_close();
    bool scroll(float dy);
private:
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    bool create();
    void layout(float width, float height);
    HWND owner_, hwnd_ = nullptr;
    Canvas canvas_;
    Scene scene_;
    Interaction interaction_;
    Palette palette_ = make_palette(false, false);
    std::unique_ptr<Accessibility> access_;
    std::wstring title_, body_;
    float dpi_ = 96, width_ = 0, height_ = 0;
    bool tracking_ = false;
};
} // namespace szu::app
