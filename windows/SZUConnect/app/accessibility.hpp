// UI Automation for the custom-drawn widgets. The window's root fragment exposes the widgets of the
// top (modal) layer as a flat list; native EDIT controls are exposed by Windows itself.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <UIAutomation.h>

#include <string>
#include <memory>

#include "scene.hpp"

namespace szu::app {

class AccessHost {
public:
    virtual ~AccessHost() = default;
    virtual HWND host_window() const = 0;
    virtual std::wstring host_automation_id() const { return L"SZUConnect.Main"; }
    virtual const Scene& host_scene() const = 0;
    virtual float host_scale() const = 0;  // DIPs -> physical pixels
    virtual std::string host_focus() const = 0;
    virtual void host_set_focus(const std::string& id) = 0;
    virtual void host_activate(const std::string& id) = 0;  // must defer; UIA may be iterating
};

struct AccessHostLifetime;

class Accessibility {
public:
    explicit Accessibility(AccessHost& host);
    ~Accessibility();
    LRESULT on_get_object(WPARAM wparam, LPARAM lparam);
    void focus_changed(const std::string& id);
    void structure_changed();
    void live_region(const std::string& id);
    void disconnect();
    bool verify_retained_provider_disconnect(); // offline smoke: a retained COM node must lose host access
#ifdef SZU_VIEW_TESTING
    IRawElementProviderFragment* root_for_testing();
#endif

private:
    std::shared_ptr<AccessHostLifetime> state_;
    IRawElementProviderSimple* root_ = nullptr;
};
#ifdef SZU_VIEW_TESTING
std::size_t accessible_list_builds_for_testing();
void reset_accessible_list_builds_for_testing();
#endif

}  // namespace szu::app
