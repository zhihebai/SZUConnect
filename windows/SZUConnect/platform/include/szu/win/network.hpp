// Network path snapshot (IP Helper) and change notifications. Read-only: never changes
// routes, DNS, proxy or VPN settings.
#pragma once
#include <functional>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "szu/engine.hpp"
#include "szu/environment.hpp"
#include "szu/netlabels.hpp"
#include "szu/win/physical.hpp"

namespace szu::win {

struct PhysicalEgress {
    physical::Route route;
    std::string binding = "unknown";
};

struct NetworkSnapshot {
    PathSnapshot path;                         // fed to ConnectionEngine::receive_path
    physical::CandidateDiagnostics physical_diagnostics; // anonymous counts; no address or interface name
    std::vector<PhysicalEgress> physical_egress; // transient interface flags, excluded from memory identity
    std::string access;                        // env.access.* key suffix: wifi | ethernet | multiple | tunnelHidden | none | unknown
    std::vector<std::string> adapter_names;    // friendly adapter names of live interfaces (UI only; never exported)
    routes::Evidence tunnel;                   // route-level VPN/TUN evidence (context, not proof)
    bool tunnel_known = false;
    // WWAN/other connected hardware, or an UP usable attachment whose physical
    // nature cannot be ruled out. Wi-Fi must not take it over. This field never
    // grants campus identity, a physical authentication candidate or memory.
    bool other_physical_link_active = false;
    std::optional<NetworkLabel> current_label;  // only the unique physical descriptor; local UI metadata
};

NetworkSnapshot capture_network(bool read_wifi_name = true);
std::string physical_egress_status(const NetworkSnapshot&, const std::optional<physical::Route>& selected);
std::optional<std::string> connected_ssid(std::uint64_t interface_luid);

// Calls `changed` (on a system thread) after interface or route changes. Debounce in the caller.
class NetworkWatcher {
public:
    explicit NetworkWatcher(std::function<void()> changed);
    ~NetworkWatcher();
    NetworkWatcher(const NetworkWatcher&) = delete;
    NetworkWatcher& operator=(const NetworkWatcher&) = delete;
    void notify();

private:
    std::function<void()> changed_;
    void* interface_handle_ = nullptr;
    void* route_handle_ = nullptr;
};

// System proxy of the current user (WinHttpGetIEProxyConfigForCurrentUser) + change watcher.
proxy::WindowsRaw read_system_proxy();

class RegistryWatcher {
public:
    RegistryWatcher(const wchar_t* subkey, std::function<void()> changed);
    ~RegistryWatcher();
    RegistryWatcher(const RegistryWatcher&) = delete;
    RegistryWatcher& operator=(const RegistryWatcher&) = delete;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace szu::win
