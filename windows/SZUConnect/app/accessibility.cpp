#include "accessibility.hpp"

#include <charconv>
#include <cstdlib>
#include <map>
#include <vector>

#include "szu/win/text.hpp"

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "oleaut32.lib")

namespace szu::app {
struct AccessHostLifetime { AccessHost* host; };
namespace {
#ifdef SZU_VIEW_TESTING
std::size_t accessible_list_builds = 0;
#endif

std::vector<const Widget*> accessible(const Scene& scene) {
#ifdef SZU_VIEW_TESTING
    ++accessible_list_builds;
#endif
    std::vector<const Widget*> out;
    const int top = scene.top_layer();
    for (const Widget& w : scene.widgets) {
        if (w.layer != top || w.role == Role::none || w.role == Role::field) continue;
        if (w.name.empty() && w.text.empty()) continue;
        out.push_back(&w);
    }
    return out;
}

// Widgets without an id still need a stable key inside one scene: role + index among accessible widgets.
std::string key_of(const Scene& scene, const Widget* target) {
    if (!target->id.empty()) return target->id;
    int index = 0;
    for (const Widget* w : accessible(scene)) {
        if (w == target) return "auto." + std::to_string(index);
        ++index;
    }
    return {};
}

const Widget* resolve(const Scene& scene, const std::string& key) {
    if (key.rfind("auto.", 0) == 0) {
        const int wanted = std::atoi(key.c_str() + 5);
        int index = 0;
        for (const Widget* w : accessible(scene)) {
            if (index == wanted) return w->id.empty() ? w : nullptr;
            ++index;
        }
        return nullptr;
    }
    for (const Widget* w : accessible(scene))
        if (w->id == key) return w;
    return nullptr;
}

int runtime_for(const std::string& key) {
    static std::map<std::string, int> ids;
    auto found = ids.find(key);
    if (found != ids.end()) return found->second;
    const int id = static_cast<int>(ids.size()) + 1;
    ids.emplace(key, id);
    return id;
}

void set_bstr(VARIANT* v, const std::wstring& text) {
    v->vt = VT_BSTR;
    v->bstrVal = SysAllocString(text.c_str());
}
void set_bool(VARIANT* v, bool flag) {
    v->vt = VT_BOOL;
    v->boolVal = flag ? VARIANT_TRUE : VARIANT_FALSE;
}
void set_int(VARIANT* v, int value) {
    v->vt = VT_I4;
    v->lVal = value;
}

class RootProvider;

class NodeProvider final : public IRawElementProviderSimple,
                           public IRawElementProviderFragment,
                           public IInvokeProvider,
                           public IToggleProvider,
                           public ISelectionItemProvider {
public:
    NodeProvider(std::shared_ptr<AccessHostLifetime> state, IRawElementProviderFragmentRoot* root, std::string key) : state_(std::move(state)), root_(root), key_(std::move(key)) {
        root_->AddRef();
    }
    ~NodeProvider() { root_->Release(); }

    // IUnknown
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) *out = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IRawElementProviderFragment)) *out = static_cast<IRawElementProviderFragment*>(this);
        else if (riid == __uuidof(IInvokeProvider)) *out = static_cast<IInvokeProvider*>(this);
        else if (riid == __uuidof(IToggleProvider)) *out = static_cast<IToggleProvider*>(this);
        else if (riid == __uuidof(ISelectionItemProvider)) *out = static_cast<ISelectionItemProvider*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    // IRawElementProviderSimple
    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* options) override {
        *options = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        return S_OK;
    }
    IFACEMETHODIMP GetPatternProvider(PATTERNID pattern, IUnknown** out) override {
        *out = nullptr;
        const Widget* w = widget();
        if (!w) return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
        if (pattern == UIA_InvokePatternId && (w->role == Role::button || w->role == Role::link) && w->action)
            *out = static_cast<IInvokeProvider*>(this);
        else if (pattern == UIA_TogglePatternId && w->role == Role::toggle)
            *out = static_cast<IToggleProvider*>(this);
        else if (pattern == UIA_SelectionItemPatternId && (w->role == Role::radio || w->role == Role::tab))
            *out = static_cast<ISelectionItemProvider*>(this);
        if (*out) AddRef();
        return S_OK;
    }
    IFACEMETHODIMP GetPropertyValue(PROPERTYID property, VARIANT* v) override {
        v->vt = VT_EMPTY;
        const Widget* w = widget();
        if (!w) return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
        switch (property) {
            case UIA_NamePropertyId: set_bstr(v, w->name.empty() ? w->text : w->name); break;
            case UIA_AutomationIdPropertyId: set_bstr(v, win::widen(key_)); break;
            case UIA_HelpTextPropertyId:
                if (!w->detail.empty()) set_bstr(v, w->detail);
                break;
            case UIA_ControlTypePropertyId: {
                int type = UIA_TextControlTypeId;
                switch (w->role) {
                    case Role::button: type = UIA_ButtonControlTypeId; break;
                    case Role::link: type = UIA_HyperlinkControlTypeId; break;
                    case Role::toggle: type = UIA_CheckBoxControlTypeId; break;
                    case Role::radio: type = UIA_RadioButtonControlTypeId; break;
                    case Role::tab: type = UIA_TabItemControlTypeId; break;
                    default: break;
                }
                set_int(v, type);
                break;
            }
            case UIA_IsKeyboardFocusablePropertyId: set_bool(v, w->focusable && w->enabled && state_->host->host_interactive()); break;
            case UIA_HasKeyboardFocusPropertyId: set_bool(v, state_->host->host_interactive() && !w->id.empty() && state_->host->host_focus() == w->id); break;
            case UIA_IsEnabledPropertyId: set_bool(v, w->enabled && state_->host->host_interactive()); break;
            case UIA_IsControlElementPropertyId:
            case UIA_IsContentElementPropertyId: set_bool(v, true); break;
            case UIA_LiveSettingPropertyId:
                if (w->live) set_int(v, Polite);
                break;
            case UIA_FrameworkIdPropertyId: set_bstr(v, L"SZUConnect"); break;
            default: break;
        }
        return S_OK;
    }
    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** out) override {
        *out = nullptr;
        return S_OK;
    }

    // IRawElementProviderFragment
    IFACEMETHODIMP Navigate(NavigateDirection direction, IRawElementProviderFragment** out) override;
    IFACEMETHODIMP GetRuntimeId(SAFEARRAY** out) override {
        int ids[2] = {UiaAppendRuntimeId, runtime_for(key_)};
        SAFEARRAY* array = SafeArrayCreateVector(VT_I4, 0, 2);
        if (!array) return E_OUTOFMEMORY;
        for (LONG i = 0; i < 2; ++i) SafeArrayPutElement(array, &i, &ids[i]);
        *out = array;
        return S_OK;
    }
    IFACEMETHODIMP get_BoundingRectangle(UiaRect* out) override {
        *out = UiaRect{};
        const Widget* w = widget();
        if (!w) return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
        const D2D1_RECT_F r = state_->host->host_scene().visible_rect(*w);
        const float s = state_->host->host_scale();
        POINT origin{static_cast<LONG>(r.left * s), static_cast<LONG>(r.top * s)};
        ClientToScreen(state_->host->host_window(), &origin);
        out->left = origin.x;
        out->top = origin.y;
        out->width = (r.right - r.left) * s;
        out->height = (r.bottom - r.top) * s;
        return S_OK;
    }
    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
        *out = nullptr;
        return S_OK;
    }
    IFACEMETHODIMP SetFocus() override {
        const Widget* w = widget();
        if (!w) return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
        if (!state_->host->host_interactive()) return static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED);
        if (w->focusable && !w->id.empty()) state_->host->host_set_focus(w->id);
        return S_OK;
    }
    IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** out) override {
        *out = root_;
        root_->AddRef();
        return S_OK;
    }

    // Patterns
    IFACEMETHODIMP Invoke() override { return activate(); }
    IFACEMETHODIMP Toggle() override { return activate(); }
    IFACEMETHODIMP get_ToggleState(ToggleState* state) override {
        const Widget* w = widget();
        if (!w) return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
        *state = w->checked ? ToggleState_On : ToggleState_Off;
        return S_OK;
    }
    IFACEMETHODIMP Select() override { return activate(); }
    IFACEMETHODIMP AddToSelection() override { return activate(); }
    IFACEMETHODIMP RemoveFromSelection() override { return static_cast<HRESULT>(UIA_E_INVALIDOPERATION); }
    IFACEMETHODIMP get_IsSelected(BOOL* selected) override {
        const Widget* w = widget();
        if (!w) return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
        *selected = w->checked ? TRUE : FALSE;
        return S_OK;
    }
    IFACEMETHODIMP get_SelectionContainer(IRawElementProviderSimple** out) override {
        *out = nullptr;
        return S_OK;
    }

private:
    const Widget* widget() const { return state_->host ? resolve(state_->host->host_scene(), key_) : nullptr; }
    HRESULT activate() {
        const Widget* w = widget();
        if (!w) return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
        if (!state_->host->host_interactive() || !w->enabled || !w->action || w->id.empty())
            return static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED);
        state_->host->host_activate(w->id);
        return S_OK;
    }
    LONG refs_ = 1;
    std::shared_ptr<AccessHostLifetime> state_;
    IRawElementProviderFragmentRoot* root_;
    std::string key_;
};

class RootProvider final : public IRawElementProviderSimple, public IRawElementProviderFragment, public IRawElementProviderFragmentRoot {
public:
    explicit RootProvider(std::shared_ptr<AccessHostLifetime> state) : state_(std::move(state)) {}

    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) *out = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IRawElementProviderFragment)) *out = static_cast<IRawElementProviderFragment*>(this);
        else if (riid == __uuidof(IRawElementProviderFragmentRoot)) *out = static_cast<IRawElementProviderFragmentRoot*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* options) override {
        *options = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        return S_OK;
    }
    IFACEMETHODIMP GetPatternProvider(PATTERNID, IUnknown** out) override {
        *out = nullptr;
        return S_OK;
    }
    IFACEMETHODIMP GetPropertyValue(PROPERTYID property, VARIANT* v) override {
        v->vt = VT_EMPTY;
        if (!state_->host) return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
        if (property == UIA_NamePropertyId) {
            wchar_t title[256]{};
            GetWindowTextW(state_->host->host_window(), title, 256);
            set_bstr(v, title);
        } else if (property == UIA_AutomationIdPropertyId) set_bstr(v, state_->host->host_automation_id());
        return S_OK;
    }
    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** out) override {
        if (!state_->host) { *out = nullptr; return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE); }
        return UiaHostProviderFromHwnd(state_->host->host_window(), out);
    }

    IFACEMETHODIMP Navigate(NavigateDirection direction, IRawElementProviderFragment** out) override {
        *out = nullptr;
        if (direction != NavigateDirection_FirstChild && direction != NavigateDirection_LastChild) return S_OK;
        if (!state_->host) return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
        const auto items = accessible(state_->host->host_scene());
        if (items.empty()) return S_OK;
        const Widget* target = direction == NavigateDirection_FirstChild ? items.front() : items.back();
        *out = new NodeProvider(state_, this, key_of(state_->host->host_scene(), target));
        return S_OK;
    }
    IFACEMETHODIMP GetRuntimeId(SAFEARRAY** out) override {
        *out = nullptr;  // the HWND host supplies the root's runtime id
        return S_OK;
    }
    IFACEMETHODIMP get_BoundingRectangle(UiaRect* out) override {
        *out = UiaRect{};  // provided by the HWND host
        return S_OK;
    }
    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
        *out = nullptr;
        return S_OK;
    }
    IFACEMETHODIMP SetFocus() override {
        if (!state_->host) return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
        if (!state_->host->host_interactive()) return static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED);
        ::SetFocus(state_->host->host_window());
        return S_OK;
    }
    IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** out) override {
        *out = this;
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** out) override {
        *out = nullptr;
        if (!state_->host) return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
        POINT pt{static_cast<LONG>(x), static_cast<LONG>(y)};
        ScreenToClient(state_->host->host_window(), &pt);
        const float s = state_->host->host_scale();
        const float dx = static_cast<float>(pt.x) / s, dy = static_cast<float>(pt.y) / s;
        const Scene& scene = state_->host->host_scene();
        const auto items = accessible(scene);
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            const D2D1_RECT_F r = scene.visible_rect(**it);
            if (dx >= r.left && dx < r.right && dy >= r.top && dy < r.bottom) {
                *out = new NodeProvider(state_, this, key_of(scene, *it));
                return S_OK;
            }
        }
        *out = this;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP GetFocus(IRawElementProviderFragment** out) override {
        *out = nullptr;
        if (!state_->host) return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
        const std::string id = state_->host->host_focus();
        if (!id.empty() && resolve(state_->host->host_scene(), id)) *out = new NodeProvider(state_, this, id);
        return S_OK;
    }

private:
    LONG refs_ = 1;
    std::shared_ptr<AccessHostLifetime> state_;
};

IFACEMETHODIMP NodeProvider::Navigate(NavigateDirection direction, IRawElementProviderFragment** out) {
    *out = nullptr;
    if (direction == NavigateDirection_Parent) {
        return root_->QueryInterface(__uuidof(IRawElementProviderFragment), reinterpret_cast<void**>(out));
    }
    if (direction != NavigateDirection_NextSibling && direction != NavigateDirection_PreviousSibling) return S_OK;
    if (!state_->host) return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
    const Scene& scene = state_->host->host_scene();
    const auto items = accessible(scene);
    std::size_t anonymous_index = items.size();
    if (key_.rfind("auto.", 0) == 0) {
        const char* first = key_.data() + 5;
        const char* last = key_.data() + key_.size();
        const auto parsed = std::from_chars(first, last, anonymous_index);
        if (parsed.ec != std::errc{} || parsed.ptr != last ||
            (last - first > 1 && *first == '0')) anonymous_index = items.size();
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i]->id.empty() ? i != anonymous_index : items[i]->id != key_) continue;
        const bool next = direction == NavigateDirection_NextSibling;
        if ((next && i + 1 < items.size()) || (!next && i > 0)) {
            const auto adjacent = next ? i + 1 : i - 1;
            const auto adjacent_key = items[adjacent]->id.empty() ? "auto." + std::to_string(adjacent) : items[adjacent]->id;
            *out = new NodeProvider(state_, root_, adjacent_key);
        }
        return S_OK;
    }
    return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
}

}  // namespace

Accessibility::Accessibility(AccessHost& host) : state_(std::make_shared<AccessHostLifetime>(AccessHostLifetime{&host})) {}

Accessibility::~Accessibility() { disconnect(); }
#ifdef SZU_VIEW_TESTING
IRawElementProviderFragment* Accessibility::root_for_testing() { return new RootProvider(state_); }
std::size_t accessible_list_builds_for_testing() { return accessible_list_builds; }
void reset_accessible_list_builds_for_testing() { accessible_list_builds = 0; }
#endif

bool Accessibility::verify_retained_provider_disconnect() {
    auto state = std::make_shared<AccessHostLifetime>(AccessHostLifetime{state_->host});
    auto* root = new RootProvider(state);
    auto* node = new NodeProvider(state, root, "status.headline");
    state->host = nullptr;
    VARIANT value{};
    const bool ok = root->GetPropertyValue(UIA_NamePropertyId, &value) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) &&
        node->GetPropertyValue(UIA_NamePropertyId, &value) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) &&
        node->Invoke() == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
    node->Release(); root->Release();
    return ok;
}

IRawElementProviderFragment* Accessibility::retained_node_for_smoke(const std::string& id) {
    if (!state_->host || !resolve(state_->host->host_scene(), id)) return nullptr;
    auto* root = new RootProvider(state_);
    auto* node = new NodeProvider(state_, root, id);
    root->Release();
    return node;
}

void Accessibility::disconnect() {
    state_->host = nullptr; // retained root/node COM references can no longer reach a destroyed window
    if (root_) {
        UiaDisconnectProvider(root_);
        root_->Release();
        root_ = nullptr;
    }
}

LRESULT Accessibility::on_get_object(WPARAM wparam, LPARAM lparam) {
    if (!state_->host) return 0;
    if (static_cast<long>(lparam) != static_cast<long>(UiaRootObjectId)) return 0;
    if (!root_) root_ = new RootProvider(state_);
    return UiaReturnRawElementProvider(state_->host->host_window(), wparam, lparam, root_);
}

void Accessibility::focus_changed(const std::string& id) {
    if (!root_ || id.empty() || !UiaClientsAreListening()) return;
    IRawElementProviderFragmentRoot* fragment_root = nullptr;
    if (FAILED(root_->QueryInterface(__uuidof(IRawElementProviderFragmentRoot), reinterpret_cast<void**>(&fragment_root)))) return;
    auto* node = new NodeProvider(state_, fragment_root, id);
    UiaRaiseAutomationEvent(node, UIA_AutomationFocusChangedEventId);
    node->Release();
    fragment_root->Release();
}

void Accessibility::structure_changed() {
    if (!root_ || !UiaClientsAreListening()) return;
    UiaRaiseStructureChangedEvent(root_, StructureChangeType_ChildrenInvalidated, nullptr, 0);
}

void Accessibility::live_region(const std::string& id) {
    if (!root_ || id.empty() || !UiaClientsAreListening()) return;
    IRawElementProviderFragmentRoot* fragment_root = nullptr;
    if (FAILED(root_->QueryInterface(__uuidof(IRawElementProviderFragmentRoot), reinterpret_cast<void**>(&fragment_root)))) return;
    auto* node = new NodeProvider(state_, fragment_root, id);
    UiaRaiseAutomationEvent(node, UIA_LiveRegionChangedEventId);
    node->Release();
    fragment_root->Release();
}

}  // namespace szu::app
