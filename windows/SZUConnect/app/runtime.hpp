// Explicit runtime boundary. The offline factory never constructs native persistence or network services.
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include "szu/internet.hpp"
#include "szu/wifi_recovery.hpp"
#include "szu/win/network.hpp"
#include "szu/win/system.hpp"

namespace szu::app {
enum class RuntimeMode { normal, observeOnly, offlineSmoke };
struct RuntimeEvidence {
    std::atomic<int> formal_services_created{0}, production_storage_accesses{0}, vault_accesses{0};
    std::atomic<int> network_calls{0}, watchers_started{0}, autostart_writes{0};
    std::atomic<int> fake_probe_calls{0}, fake_auth_submissions{0};
    std::atomic<int> wifi_calls{0}, fake_wifi_calls{0};
};
class RuntimeServices : public ConnectionService {
public:
    virtual RuntimeMode mode() const = 0;
    virtual RuntimeEvidence& evidence() = 0;
    virtual WebsiteCheckResult check_website(const std::string&, bool, const CancelToken&) = 0;
    // Presentation-only public connectivity. Never enters the authentication engine.
    // Offline/injected runtimes opt in explicitly; default starts no native services.
    virtual std::optional<ConnectivityReport> system_connectivity(const CancelToken&) { return std::nullopt; }
    virtual compat::Input compatibility(bool, const std::string&, const CancelToken&) = 0;
    virtual win::Storage::ReadStatus read(const wchar_t*, std::string&) = 0;
    virtual bool write(const wchar_t*, const std::string&) = 0;
    virtual bool write_preserving(const wchar_t*, const std::string&, bool) = 0;
    virtual bool quarantine(const wchar_t*) = 0;
    virtual bool storage_available() const = 0;
    virtual win::CredentialVault::ReadStatus read_credentials(Credentials&) = 0;
    virtual bool write_credentials(const Credentials&) = 0;
    virtual bool launch_enabled() = 0;
    virtual bool launch_approved() = 0;
    virtual bool set_launch(bool) = 0;
    virtual win::NetworkSnapshot capture_network() = 0;
    virtual std::optional<NetworkLabel> network_label(const physical::Route& route) {
        return NetworkLabel{route.display_name, route.descriptor.kind, std::nullopt, false};
    }
    virtual proxy::WindowsRaw read_proxy() = 0;
    virtual std::string random_hex(std::size_t) = 0;
    virtual void start_watchers(std::function<void()>) = 0;
    virtual void stop_watchers() = 0;
    virtual wifi::Result recover_wifi(const std::vector<wifi::Network>&, bool, const CancelToken&) { return wifi::Result::unavailable; }
    virtual bool fake_auth_passed() const { return false; }
};
std::unique_ptr<RuntimeServices> make_runtime(RuntimeMode mode);
}  // namespace szu::app
