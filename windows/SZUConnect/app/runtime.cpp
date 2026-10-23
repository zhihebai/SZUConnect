#include "runtime.hpp"
#include <map>
#include <mutex>
#include "szu/win/service.hpp"
#include "szu/win/system_probe.hpp"
#include "szu/win/crash_diagnostics.hpp"
#include "szu/win/wifi_recovery.hpp"
#include "szu/win/proxy_assistant.hpp"
#include "szu/win/proxy_yaml.hpp"
#include "szu/portal_wire.hpp"
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <thread>

namespace szu::app {
namespace {
class NativeRuntime final : public RuntimeServices {
public:
    explicit NativeRuntime(RuntimeMode mode) : mode_(mode), storage_(mode == RuntimeMode::observeOnly ?
        win::Storage::Access::readOnly : win::Storage::Access::writable) { evidence_.formal_services_created = 2; }
    ~NativeRuntime() override { stop_watchers(); }
    RuntimeMode mode() const override { return mode_; }
    RuntimeEvidence& evidence() override { return evidence_; }
    crash::Report crash_diagnostics(const CancelToken& token) override {
        return mode_ == RuntimeMode::normal ? win::collect_crash_diagnostics(token) : crash::Report{};
    }
    bool set_expected_physical_route(const std::optional<physical::Route>& route) override {
        return service_.set_expected_physical_route(route);
    }
    wifi::Result recover_wifi(const std::vector<wifi::Network>& networks, bool power, const CancelToken& token) override {
        if (mode_ != RuntimeMode::normal || token.cancelled()) return wifi::Result::cancelled;
        ++evidence_.wifi_calls; return win::recover_wifi(networks, power, token);
    }
    ConnectivityReport probe(ProbeMode mode, const CancelToken& token) override {
        ++evidence_.network_calls; return service_.probe(mode, token);
    }
    CampusDiscovery discover(CampusArea area, const CancelToken& token) override {
        if (mode_ == RuntimeMode::observeOnly) return {};
        ++evidence_.network_calls; return service_.discover(area, token);
    }
    LoginAttempt attempt(const Credentials& c, CampusArea area, const CancelToken& token) override {
        return attempt_with_requirements(c, area, token, false);
    }
    LoginAttempt attempt_with_requirements(const Credentials& c, CampusArea area, const CancelToken& token,
                                          bool require_client_config) override {
        if (mode_ == RuntimeMode::observeOnly) {
            LoginAttempt stopped; stopped.outcome = Outcome::cancelled; stopped.credential_submission_started = false; return stopped;
        }
        ++evidence_.network_calls; return service_.attempt_with_requirements(c, area, token, require_client_config);
    }
    WebsiteCheckResult check_website(const std::string& url, bool system, const CancelToken& token) override {
        ++evidence_.network_calls; return service_.check_website(url, system, token);
    }
    std::optional<ConnectivityReport> system_connectivity(ProbeMode mode, const CancelToken& token) override {
        return win::probe_system_connectivity(mode, token, [this](const win::HttpRequest& request, const CancelToken& child) {
            ++evidence_.network_calls; return win::http_get(request, child);
        });
    }
    compat::Input compatibility(bool proxy, const std::string& tunnel, const CancelToken& token) override {
        ++evidence_.network_calls; return service_.compatibility(proxy, tunnel, token);
    }
    compat::Input proxy_config_check(bool proxy, const std::string& tunnel, const CancelToken& token) override {
        ++evidence_.network_calls; return service_.compatibility(proxy, tunnel, token, true);
    }
    win::Storage::ReadStatus read(const wchar_t* name, std::string& out) override {
        ++evidence_.production_storage_accesses; return storage_.read(name, out);
    }
    bool write(const wchar_t* name, const std::string& data) override {
        if (mode_ == RuntimeMode::observeOnly) return false;
        ++evidence_.production_storage_accesses; return storage_.write(name, data);
    }
    bool write_preserving(const wchar_t* name, const std::string& data, bool preserve) override {
        if (mode_ == RuntimeMode::observeOnly) return false;
        ++evidence_.production_storage_accesses; return storage_.write_preserving_existing(name, data, preserve);
    }
    bool quarantine(const wchar_t* name) override {
        if (mode_ == RuntimeMode::observeOnly) return false;
        ++evidence_.production_storage_accesses; return storage_.quarantine(name);
    }
    bool storage_available() const override { return storage_.available(); }
    win::CredentialVault::ReadStatus read_credentials(Credentials& out) override {
        if (mode_ != RuntimeMode::normal) return win::CredentialVault::ReadStatus::missing;
        ++evidence_.vault_accesses; return win::CredentialVault::read(out);
    }
    bool write_credentials(const Credentials& c) override {
        if (mode_ == RuntimeMode::observeOnly) return false;
        ++evidence_.vault_accesses; return win::CredentialVault::write(c);
    }
    bool erase_credentials() override {
        if (mode_ != RuntimeMode::normal) return false;
        ++evidence_.vault_accesses; return win::CredentialVault::remove();
    }
    bool launch_enabled() override { return win::Autostart::enabled(); }
    bool launch_approved() override { return win::Autostart::approved(); }
    bool set_launch(bool enable) override {
        if (mode_ == RuntimeMode::observeOnly) return false;
        ++evidence_.autostart_writes; return win::Autostart::set(enable);
    }
    win::NetworkSnapshot capture_network() override { return win::capture_network(mode_ == RuntimeMode::normal); }
    physical::CandidateDiagnostics capture_physical_diagnostics() override {
        return win::capture_physical_candidate_report().diagnostics;
    }
    std::optional<NetworkLabel> network_label(const physical::Route& route) override {
        NetworkLabel label{route.display_name, route.descriptor.kind, std::nullopt, false};
        if (mode_ == RuntimeMode::normal && route.descriptor.kind == "wifi") {
            if (!win::physical_route_current(route)) return std::nullopt;
            label.ssid = win::connected_ssid(route.interface_luid);
            if (!win::physical_route_current(route)) return std::nullopt;
            if (label.ssid) label.display_name = *label.ssid;
        }
        return label;
    }
    proxy::WindowsRaw read_proxy() override { return win::read_system_proxy(); }
    proxyassist::Preview prepare_proxy_config(HWND owner, proxyassist::Format format) override {
        proxyassist::Preview denied; denied.format = format; denied.error = "unavailable";
        if (format != proxyassist::Format::clashVergeScript && format != proxyassist::Format::mihomoYaml && format != proxyassist::Format::singBoxJson) return denied;
        if (mode_ != RuntimeMode::normal || !storage_.available()) return denied;
        ensure_proxy_transaction(); proxy_config_->clear_preview();
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return denied;
        dialog->SetOptions(FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT);
        const COMDLG_FILTERSPEC filters[]{{L"Clash Verge Rev script", L"*.js"}, {L"Mihomo YAML", L"*.yaml;*.yml"}, {L"sing-box JSON", L"*.json"}};
        dialog->SetFileTypes(1, &filters[static_cast<int>(format)]);
        if (format == proxyassist::Format::clashVergeScript) {
            PWSTR roaming = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DONT_VERIFY, nullptr, &roaming))) {
                const std::wstring folder = std::wstring(roaming) + L"\\io.github.clash-verge-rev.clash-verge-rev\\profiles";
                CoTaskMemFree(roaming);
                // Public client layout; suggest its folder only after the user opens the picker.
                // No profile, subscription, controller secret or running config is read here.
                const DWORD attributes = folder.size() > 2 && folder[1] == L':' ? GetFileAttributesW(folder.c_str()) : INVALID_FILE_ATTRIBUTES;
                if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    Microsoft::WRL::ComPtr<IShellItem> start;
                    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&start)))) dialog->SetFolder(start.Get());
                    dialog->SetFileName(L"Script.js");
                }
            }
        }
        dialog->SetTitle(L"选择需要添加校园直连规则的配置文件");
        const HRESULT shown = dialog->Show(owner);
        if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) { denied.error.clear(); return denied; }
        if (FAILED(shown)) return denied;
        Microsoft::WRL::ComPtr<IShellItem> item; PWSTR path = nullptr;
        if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return denied;
        const std::wstring selected(path); CoTaskMemFree(path);
        ++evidence_.production_storage_accesses;
        const auto transform = [format](std::string_view text) {
            switch (format) {
                case proxyassist::Format::clashVergeScript: return proxyassist::patch_clash_verge_script(text);
                case proxyassist::Format::mihomoYaml: return win::patch_mihomo_yaml(text);
                case proxyassist::Format::singBoxJson: return proxyassist::patch_sing_box(text);
            }
            proxyassist::Patch failure; failure.error = "invalidConfig"; return failure;
        };
        return proxy_config_->prepare(selected, format, transform);
    }
    proxyassist::Result apply_proxy_config() override {
        if (mode_ != RuntimeMode::normal || !proxy_config_) return {"unavailable"};
        ++evidence_.production_storage_accesses; return proxy_config_->apply();
    }
    proxyassist::Result restore_proxy_config() override {
        if (mode_ != RuntimeMode::normal || !storage_.available()) return {"unavailable"};
        ensure_proxy_transaction(); ++evidence_.production_storage_accesses; return proxy_config_->restore();
    }
    void clear_proxy_config_preview() override { if (proxy_config_) proxy_config_->clear_preview(); }
    proxyassist::Result discard_proxy_config_backup() override {
        if (mode_ != RuntimeMode::normal || !storage_.available()) return {"unavailable"};
        ensure_proxy_transaction(); ++evidence_.production_storage_accesses; return proxy_config_->discard_backup();
    }
    bool proxy_config_has_backup() override {
        if (mode_ != RuntimeMode::normal || !storage_.available()) return false;
        ensure_proxy_transaction(); ++evidence_.production_storage_accesses; return proxy_config_->has_backup();
    }
    std::string random_hex(std::size_t bytes) override { return win::random_hex(bytes); }
    void start_watchers(std::function<void()> changed) override {
        stop_watchers();
        net_ = std::make_unique<win::NetworkWatcher>(changed); ++evidence_.watchers_started;
        proxy_ = std::make_unique<win::RegistryWatcher>(L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", changed);
        ++evidence_.watchers_started;
    }
    void stop_watchers() override { net_.reset(); proxy_.reset(); }
private:
    void ensure_proxy_transaction() {
        if (!proxy_config_) proxy_config_ = std::make_unique<win::ProxyConfigTransaction>(storage_.folder());
    }
    std::unique_ptr<win::ProxyConfigTransaction> proxy_config_;
    RuntimeMode mode_;
    RuntimeEvidence evidence_;
    win::WinHttpService service_;
    win::Storage storage_;
    std::unique_ptr<win::NetworkWatcher> net_;
    std::unique_ptr<win::RegistryWatcher> proxy_;
};

class OfflineRuntime final : public RuntimeServices {
public:
    OfflineRuntime() {
        files_[L"settings.json"] = R"({"formatVersion":1,"monitoringMode":"manual","area":"teaching","probeMode":"complete","appearanceMode":"system","tutorial":{"status":"completed"}})";
    }
    RuntimeMode mode() const override { return RuntimeMode::offlineSmoke; }
    RuntimeEvidence& evidence() override { return evidence_; }
    wifi::Result recover_wifi(const std::vector<wifi::Network>&, bool, const CancelToken& token) override {
        ++evidence_.fake_wifi_calls; return token.cancelled() ? wifi::Result::cancelled : wifi::Result::notFound;
    }
    ConnectivityReport probe(ProbeMode mode, const CancelToken& token) override {
        ++evidence_.fake_probe_calls;
        auto r = ConnectivityReport::simple(connected_, connected_); r.mode = mode;
        if (token.cancelled()) r.apple.state = r.microsoft.state = EndpointState::cancelled;
        return r;
    }
    CampusDiscovery discover(CampusArea, const CancelToken& token) override {
        return token.cancelled() ? CampusDiscovery{} : CampusDiscovery{Discovery::confirmed, Discovery::confirmed};
    }
    LoginAttempt attempt(const Credentials& c, CampusArea, const CancelToken& token) override {
        LoginAttempt r;
        if (token.cancelled()) { r.outcome = Outcome::cancelled; return r; }
        if (c != Credentials{"offline-fixture", "offline-password"}) return r;
        ++evidence_.fake_auth_submissions; connected_ = true; r.outcome = Outcome::success; return r;
    }
    WebsiteCheckResult check_website(const std::string&, bool, const CancelToken& token) override {
        return token.cancelled() ? WebsiteCheckResult{"cancelled", 0, 0} : WebsiteCheckResult{"accessible", 200, 0};
    }
    compat::Input compatibility(bool, const std::string& tunnel, const CancelToken&) override {
        compat::Input r; r.proxy = "off"; r.tunnel = tunnel; return r;
    }
    win::Storage::ReadStatus read(const wchar_t* name, std::string& out) override {
        std::lock_guard<std::mutex> lock(files_mutex_);
        const auto i = files_.find(name);
        if (i == files_.end()) return win::Storage::ReadStatus::missing;
        out = i->second; return win::Storage::ReadStatus::ok;
    }
    bool write(const wchar_t* name, const std::string& data) override { std::lock_guard<std::mutex> lock(files_mutex_); files_[name] = data; return true; }
    bool write_preserving(const wchar_t* name, const std::string& data, bool) override { return write(name, data); }
    bool quarantine(const wchar_t*) override { return false; }
    bool storage_available() const override { return true; }
    win::CredentialVault::ReadStatus read_credentials(Credentials& out) override {
        if (!credentials_) return win::CredentialVault::ReadStatus::missing;
        out = *credentials_; return win::CredentialVault::ReadStatus::found;
    }
    bool write_credentials(const Credentials& value) override { credentials_ = value; return true; }
    bool erase_credentials() override { credentials_.reset(); return true; }
    bool launch_enabled() override { return launch_; }
    bool launch_approved() override { return true; }
    bool set_launch(bool enable) override { launch_ = enable; return true; }
    win::NetworkSnapshot capture_network() override {
        win::NetworkSnapshot n; n.path.usable = true; n.path.identity = "offline-fixture-path";
        n.path.interfaces = "offline fixture"; n.access = "ethernet"; n.tunnel_known = true; return n;
    }
    proxy::WindowsRaw read_proxy() override { proxy::WindowsRaw p; p.readable = true; return p; }
    std::string random_hex(std::size_t bytes) override { return std::string(bytes * 2, 'a'); }
    void start_watchers(std::function<void()>) override {}
    void stop_watchers() override {}
    bool fake_auth_passed() const override { return connected_ && evidence_.fake_auth_submissions == 1; }
private:
    RuntimeEvidence evidence_;
    std::atomic<bool> connected_{false};
    std::map<std::wstring, std::string> files_; // controller/worker serialize writes through existing engine gates
    std::mutex files_mutex_;
    bool launch_ = false;
    std::optional<Credentials> credentials_{Credentials{"offline-fixture", "offline-password"}};
};
}  // namespace
std::unique_ptr<RuntimeServices> make_runtime(RuntimeMode mode) {
    if (mode == RuntimeMode::offlineSmoke) return std::make_unique<OfflineRuntime>();
    return std::make_unique<NativeRuntime>(mode);
}
}  // namespace szu::app
