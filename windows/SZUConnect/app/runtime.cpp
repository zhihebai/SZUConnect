#include "runtime.hpp"
#include <map>
#include <mutex>
#include "szu/win/service.hpp"
#include "szu/win/wifi_recovery.hpp"
#include "szu/portal_wire.hpp"
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
        if (mode_ == RuntimeMode::observeOnly) return {};
        ++evidence_.network_calls; return service_.attempt(c, area, token);
    }
    WebsiteCheckResult check_website(const std::string& url, bool system, const CancelToken& token) override {
        ++evidence_.network_calls; return service_.check_website(url, system, token);
    }
    std::optional<ConnectivityReport> system_connectivity(const CancelToken& token) override {
        ConnectivityReport report; report.mode = ProbeMode::complete;
        const auto probe = [&](const char* url, EndpointResult& result, bool apple) {
            ++evidence_.network_calls;
            win::HttpRequest request; request.url = url; request.proxy = win::ProxyMode::system;
            request.timeout_ms = 6000; request.max_bytes = wire::kMaxProbeBytes;
            const auto response = win::http_get(request, token);
            result.elapsed = response.elapsed;
            switch (response.error) {
                case win::HttpError::none:
                    result.state = (apple ? wire::is_connectivity_success(response.body, response.status) :
                        wire::is_microsoft_success(response.body, response.status)) ? EndpointState::success :
                        response.status >= 300 && response.status < 400 ? EndpointState::redirect : EndpointState::unexpectedResponse;
                    break;
                case win::HttpError::timeout: result.state = EndpointState::timeout; break;
                case win::HttpError::dns: result.state = EndpointState::dns; break;
                case win::HttpError::tls: result.state = EndpointState::tls; break;
                case win::HttpError::cancelled: result.state = EndpointState::cancelled; break;
                case win::HttpError::redirect: result.state = EndpointState::redirect; break;
                default: result.state = EndpointState::network; break;
            }
        };
        std::thread apple([&] { probe("https://www.apple.com/library/test/success.html", report.apple, true); });
        std::thread microsoft([&] { probe("http://www.msftconnecttest.com/connecttest.txt", report.microsoft, false); });
        apple.join(); microsoft.join();
        if (token.cancelled()) return std::nullopt;
        return report;
    }
    compat::Input compatibility(bool proxy, const std::string& tunnel, const CancelToken& token) override {
        ++evidence_.network_calls; return service_.compatibility(proxy, tunnel, token);
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
        ++evidence_.vault_accesses; return win::CredentialVault::read(out);
    }
    bool write_credentials(const Credentials& c) override {
        if (mode_ == RuntimeMode::observeOnly) return false;
        ++evidence_.vault_accesses; return win::CredentialVault::write(c);
    }
    bool launch_enabled() override { return win::Autostart::enabled(); }
    bool launch_approved() override { return win::Autostart::approved(); }
    bool set_launch(bool enable) override {
        if (mode_ == RuntimeMode::observeOnly) return false;
        ++evidence_.autostart_writes; return win::Autostart::set(enable);
    }
    win::NetworkSnapshot capture_network() override { return win::capture_network(mode_ == RuntimeMode::normal); }
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
    std::string random_hex(std::size_t bytes) override { return win::random_hex(bytes); }
    void start_watchers(std::function<void()> changed) override {
        stop_watchers();
        net_ = std::make_unique<win::NetworkWatcher>(changed); ++evidence_.watchers_started;
        proxy_ = std::make_unique<win::RegistryWatcher>(L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", changed);
        ++evidence_.watchers_started;
    }
    void stop_watchers() override { net_.reset(); proxy_.reset(); }
private:
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
        out = {"offline-fixture", "offline-password"}; return win::CredentialVault::ReadStatus::found;
    }
    bool write_credentials(const Credentials&) override { return true; }
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
};
}  // namespace
std::unique_ptr<RuntimeServices> make_runtime(RuntimeMode mode) {
    if (mode == RuntimeMode::offlineSmoke) return std::make_unique<OfflineRuntime>();
    return std::make_unique<NativeRuntime>(mode);
}
}  // namespace szu::app
