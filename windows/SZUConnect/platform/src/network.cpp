#include "szu/win/network.hpp"
#include "szu/win/interface_state.hpp"
#ifdef SZU_NETWORK_TESTING
#include "szu/win/network_link_testing.hpp"
#endif

#include "szu/win/text.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <map>
#include <limits>
#include <thread>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")

namespace szu::win {
namespace {

struct Adapter {
    std::string key;         // LUID as text; joins routes to interfaces
    std::string name;        // friendly name (UI only)
    std::string kind;        // physical | tunnel | loopback | other
    bool wifi = false;
    bool up = false;
    bool link_active = false;
    bool has_gateway = false;
    std::uint64_t luid = 0;
    std::string gateway_bytes;
    std::wstring dns_suffix;
    std::string gateway_ip, network4, dhcp_ip, dns_list;  // RAM-only inputs of the network-memory descriptor
};

bool contains_ci(const std::wstring& haystack, const wchar_t* needle) {
    std::wstring h = haystack, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    std::transform(n.begin(), n.end(), n.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return h.find(n) != std::wstring::npos;
}

std::string classify_kind(const IP_ADAPTER_ADDRESSES* a) {
    const std::wstring description = a->Description ? a->Description : L"";
    if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) return "loopback";
    // IPv6 transition adapters are not VPNs; counting them would report false tunnels.
    if (a->TunnelType == TUNNEL_TYPE_6TO4 || a->TunnelType == TUNNEL_TYPE_ISATAP || a->TunnelType == TUNNEL_TYPE_TEREDO ||
        a->TunnelType == TUNNEL_TYPE_IPHTTPS)
        return "other";
    for (const wchar_t* marker : {L"wintun", L"wireguard", L"tap-windows", L"openvpn", L"clash", L"mihomo", L"sing-box", L"meta tunnel",
                                  L"tun ", L"vpn"})
        if (contains_ci(description, marker)) return "tunnel";
    if (a->IfType == IF_TYPE_TUNNEL || a->IfType == IF_TYPE_PPP || a->IfType == IF_TYPE_PROP_VIRTUAL) return "tunnel";
    if (a->IfType == IF_TYPE_IEEE80211) return "physical";
    if (a->IfType == IF_TYPE_ETHERNET_CSMACD) {
        for (const wchar_t* marker : {L"hyper-v", L"vmware", L"virtualbox", L"vethernet", L"loopback"})
            if (contains_ci(description, marker)) return "other";
        return "physical";
    }
    return "other";
}

bool known_virtual_attachment(const IP_ADAPTER_ADDRESSES* a) {
    if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL ||
        a->IfType == IF_TYPE_PROP_VIRTUAL || a->TunnelType != TUNNEL_TYPE_NONE) return true;
    const std::wstring description = a->Description ? a->Description : L"";
    for (const wchar_t* marker : {L"wintun", L"wireguard", L"tap-windows", L"openvpn", L"clash", L"mihomo", L"sing-box",
                                  L"meta tunnel", L"tun ", L"vpn", L"hyper-v", L"vmware", L"virtualbox", L"vethernet",
                                  L"loopback", L"wi-fi direct virtual", L"hosted network virtual"})
        if (contains_ci(description, marker)) return true;
    return false;
}

bool usable_connected_source(const IP_ADAPTER_ADDRESSES* a) {
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
        if (!u->Address.lpSockaddr || u->DadState != IpDadStatePreferred || !u->ValidLifetime) continue;
        if (u->Address.lpSockaddr->sa_family == AF_INET) {
            char text[INET_ADDRSTRLEN] = {};
            const auto* ip = &reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr)->sin_addr;
            if (InetNtopA(AF_INET, ip, text, sizeof text) && physical::usable_ipv4(text)) return true;
        } else if (u->Address.lpSockaddr->sa_family == AF_INET6) {
            const auto* ip = &reinterpret_cast<const sockaddr_in6*>(u->Address.lpSockaddr)->sin6_addr;
            if (!IN6_IS_ADDR_UNSPECIFIED(ip) && !IN6_IS_ADDR_LOOPBACK(ip) && !IN6_IS_ADDR_LINKLOCAL(ip) &&
                !IN6_IS_ADDR_MULTICAST(ip) && !IN6_IS_ADDR_V4MAPPED(ip)) return true;
        }
    }
    return false;
}

using InterfaceStateReader = std::function<NETIO_STATUS(MIB_IF_ROW2&)>;
void collect_physical_egress(NetworkSnapshot& snapshot,
    const std::function<NETIO_STATUS(MIB_IPINTERFACE_ROW&)>& read_state) {
    snapshot.physical_egress.clear();
    for (const auto& route : snapshot.path.physical_candidates) {
        const auto read = [&]() -> std::optional<std::pair<bool, bool>> {
            MIB_IPINTERFACE_ROW row{}; InitializeIpInterfaceEntry(&row);
            row.Family = AF_INET; row.InterfaceLuid.Value = route.interface_luid; row.InterfaceIndex = route.interface_index;
            if (read_state(row) != NO_ERROR || row.Family != AF_INET || row.InterfaceLuid.Value != route.interface_luid ||
                row.InterfaceIndex != route.interface_index) return std::nullopt;
            return std::pair{row.ForwardingEnabled != 0, row.WeakHostSend != 0};
        };
        const auto first = read(), second = read();
        std::string value = "unknown";
        if (first && first == second) value = first->first
            ? (first->second ? "forwardingAndWeakHostSend" : "forwarding") : (first->second ? "weakHostSend" : "normal");
        snapshot.physical_egress.push_back({route, std::move(value)});
    }
}

bool active_attachment(const IP_ADAPTER_ADDRESSES* a, const InterfaceStateReader& read_state, bool preserve_physical_unknown = false) {
    if (a->OperStatus != IfOperStatusUp) return false;
    physical::OtherLinkEvidence evidence;
    evidence.up = true;
    evidence.known_virtual = known_virtual_attachment(a);
    evidence.usable_source = usable_connected_source(a);
    MIB_IF_ROW2 row{}; row.InterfaceLuid = a->Luid; row.InterfaceIndex = a->IfIndex;
    if (read_state(row) == NO_ERROR && row.InterfaceLuid.Value == a->Luid.Value && row.InterfaceIndex == a->IfIndex) {
        if (row.OperStatus != IfOperStatusUp) return false;
        evidence.details_readable = true;
        evidence.hardware_interface = row.InterfaceAndOperStatusFlags.HardwareInterface != 0;
        evidence.connector_present = row.InterfaceAndOperStatusFlags.ConnectorPresent != 0;
        evidence.media_connected = row.MediaConnectState == MediaConnectStateConnected;
        evidence.media_disconnected = row.MediaConnectState == MediaConnectStateDisconnected ||
            row.InterfaceAndOperStatusFlags.NotMediaConnected != 0;
        evidence.known_virtual = evidence.known_virtual || row.InterfaceAndOperStatusFlags.FilterInterface ||
            row.InterfaceAndOperStatusFlags.EndPointInterface || row.AccessType == NET_IF_ACCESS_LOOPBACK ||
            row.MediaType == NdisMediumTunnel || row.MediaType == NdisMediumLoopback;
        // A physical mobile-broadband modem may expose a legacy PPP interface.
        // Hardware evidence disambiguates it from software PPP/VPN interfaces.
        if (a->IfType == IF_TYPE_PPP && !evidence.hardware_interface) evidence.known_virtual = true;
    } else if (a->IfType == IF_TYPE_PPP) {
        // Existing software PPP/VPN interfaces must not suppress Wi-Fi forever.
        evidence.known_virtual = true;
    }
    if (physical::other_physical_link_active(evidence)) return true;
    // An UP Ethernet/WLAN may already be acquiring its first address. Missing
    // or inconclusive details cannot permit takeover; only confirmed virtual
    // or disconnected evidence can release this conservative physical gate.
    return preserve_physical_unknown && !evidence.known_virtual &&
        !(evidence.details_readable && evidence.media_disconnected);
}

std::string address_text(const SOCKADDR_INET& address) {
    char buffer[INET6_ADDRSTRLEN] = {};
    if (address.si_family == AF_INET) InetNtopA(AF_INET, &address.Ipv4.sin_addr, buffer, sizeof buffer);
    else if (address.si_family == AF_INET6) InetNtopA(AF_INET6, &address.Ipv6.sin6_addr, buffer, sizeof buffer);
    return buffer;
}

bool unspecified(const SOCKADDR_INET& address) {
    if (address.si_family == AF_INET) return address.Ipv4.sin_addr.S_un.S_addr == 0;
    if (address.si_family == AF_INET6) return IN6_IS_ADDR_UNSPECIFIED(&address.Ipv6.sin6_addr) != 0;
    return true;
}

std::string sockaddr_text(const SOCKADDR* sa) {
    char buffer[INET6_ADDRSTRLEN] = {};
    if (!sa) return {};
    if (sa->sa_family == AF_INET) InetNtopA(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, buffer, sizeof buffer);
    else if (sa->sa_family == AF_INET6) InetNtopA(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, buffer, sizeof buffer);
    return buffer;
}

// "a.b.c.d/len" of the first non link-local IPv4 address; never the client address itself.
std::string network_text(const IP_ADAPTER_UNICAST_ADDRESS* first) {
    for (const auto* u = first; u; u = u->Next) {
        if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
        const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
        unsigned char o[4];
        std::memcpy(o, &sin->sin_addr.S_un.S_addr, 4);
        const unsigned prefix = u->OnLinkPrefixLength;
        if (prefix == 0 || prefix > 32 || (o[0] == 169 && o[1] == 254)) continue;
        const std::uint32_t ip = (std::uint32_t(o[0]) << 24) | (std::uint32_t(o[1]) << 16) | (std::uint32_t(o[2]) << 8) | o[3];
        const std::uint32_t mask = prefix == 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1u);
        const std::uint32_t net = ip & mask;
        char text[32];
        std::snprintf(text, sizeof text, "%u.%u.%u.%u/%u", net >> 24, (net >> 16) & 255u, (net >> 8) & 255u, net & 255u, prefix);
        return text;
    }
    return {};
}

std::string mac_text(const UCHAR* bytes, ULONG length) {
    char text[32];
    bool any = false;
    std::string out;
    for (ULONG i = 0; i < length; ++i) {
        std::snprintf(text, sizeof text, i ? ":%02x" : "%02x", bytes[i]);
        out += text;
        any |= bytes[i] != 0;
    }
    return any ? out : std::string();
}

std::string lower_ascii(std::string text) {
    for (char& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

std::uint64_t fnv1a(const std::string& data) {
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::vector<Adapter> collect_adapters(const IP_ADAPTER_ADDRESSES* first, NetworkSnapshot& snapshot,
                                    const InterfaceStateReader& read_state) {
    std::vector<Adapter> adapters;
    for (auto* a = first; a; a = a->Next) {
        Adapter adapter;
        adapter.luid = a->Luid.Value;
        adapter.key = std::to_string(a->Luid.Value);
        adapter.name = narrow(a->FriendlyName ? a->FriendlyName : L"");
        adapter.kind = classify_kind(a);
        // IfType/name is only a presentation classification. The takeover gate
        // must also reject a confirmed software interface or disconnected media.
        if (adapter.kind == "physical") adapter.link_active = active_attachment(a, read_state, true);
        else if (adapter.kind == "other" || a->IfType == IF_TYPE_PPP)
            snapshot.other_physical_link_active = snapshot.other_physical_link_active || active_attachment(a, read_state);
        adapter.wifi = a->IfType == IF_TYPE_IEEE80211;
        adapter.up = a->OperStatus == IfOperStatusUp;
        adapter.has_gateway = a->FirstGatewayAddress != nullptr;
        if (a->FirstGatewayAddress && a->FirstGatewayAddress->Address.lpSockaddr)
            adapter.gateway_bytes.assign(reinterpret_cast<const char*>(a->FirstGatewayAddress->Address.lpSockaddr),
                                         static_cast<std::size_t>(a->FirstGatewayAddress->Address.iSockaddrLength));
        adapter.dns_suffix = a->DnsSuffix ? a->DnsSuffix : L"";
        if (a->FirstGatewayAddress && a->FirstGatewayAddress->Address.lpSockaddr)
            adapter.gateway_ip = sockaddr_text(a->FirstGatewayAddress->Address.lpSockaddr);
        adapter.network4 = network_text(a->FirstUnicastAddress);
        if (a->Dhcpv4Server.lpSockaddr) adapter.dhcp_ip = sockaddr_text(a->Dhcpv4Server.lpSockaddr);
        std::vector<std::string> servers;
        for (auto* d = a->FirstDnsServerAddress; d; d = d->Next)
            if (d->Address.lpSockaddr) servers.push_back(sockaddr_text(d->Address.lpSockaddr));
        std::sort(servers.begin(), servers.end());
        for (const auto& server : servers) adapter.dns_list += (adapter.dns_list.empty() ? "" : ",") + server;
        adapters.push_back(std::move(adapter));
    }
    return adapters;
}

const Adapter* project_adapter_links(NetworkSnapshot& snapshot, const std::vector<Adapter>& adapters,
                                    bool default_on_physical, bool default_on_tunnel) {
    bool wifi = false, ethernet = false, tunnel_gateway = false, physical_up = false;
    const Adapter* primary = nullptr;
    for (const Adapter& a : adapters) {
        if (!a.up || a.kind == "loopback") continue;
        if (a.kind == "physical" || a.kind == "tunnel") snapshot.adapter_names.push_back(a.name);
        if (a.kind == "physical" && a.link_active) {
            physical_up = true;
            if (a.has_gateway) {
                (a.wifi ? wifi : ethernet) = true;
                if (!primary) primary = &a;
            }
        }
        if (a.kind == "tunnel" && a.has_gateway) tunnel_gateway = true;
    }
    if (wifi && ethernet) snapshot.access = "multiple";
    else if (wifi) snapshot.access = "wifi";
    else if (ethernet) snapshot.access = "ethernet";
    else if (physical_up && (tunnel_gateway || default_on_tunnel)) snapshot.access = "tunnelHidden";
    else snapshot.access = "none";
    snapshot.path.usable = primary != nullptr || tunnel_gateway || (default_on_physical && physical_up) || default_on_tunnel;
    snapshot.path.link_active = physical_up;
    snapshot.path.tunnel = snapshot.tunnel.tunnel;
    snapshot.path.constrained = false;
    return primary;
}

// Each read builds physical attachments independently of the TUN/global DNS.
// The second read must agree only on the selected interface's authority.
std::vector<physical::Attachment> read_physical_evidence(bool& readable, physical::CandidateDiagnostics& diagnostics) {
    readable = false;
    ULONG size = 32 * 1024;
    std::vector<unsigned char> buffer(size);
    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW && size <= 4 * 1024 * 1024) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) return {};
    PMIB_IPFORWARD_TABLE2 table = nullptr;
    if (GetIpForwardTable2(AF_INET, &table) != NO_ERROR || !table) return {};
    readable = true;
    std::map<std::uint64_t, std::vector<std::string>> defaults;
    std::map<std::uint64_t, std::uint64_t> metrics;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const auto& row = table->Table[i];
        if (row.DestinationPrefix.Prefix.si_family != AF_INET || row.DestinationPrefix.PrefixLength != 0 ||
            !unspecified(row.DestinationPrefix.Prefix) ||
            row.Loopback || !row.ValidLifetime || unspecified(row.NextHop)) continue;
        const auto gateway = address_text(row.NextHop);
        if (!physical::usable_ipv4(gateway)) continue;
        defaults[row.InterfaceLuid.Value].push_back(gateway);
        MIB_IPINTERFACE_ROW iface{};
        InitializeIpInterfaceEntry(&iface);
        iface.Family = AF_INET; iface.InterfaceLuid = row.InterfaceLuid;
        if (GetIpInterfaceEntry(&iface) == NO_ERROR) {
            const std::uint64_t metric = std::uint64_t(row.Metric) + iface.Metric;
            const auto old = metrics.find(row.InterfaceLuid.Value);
            if (old == metrics.end() || metric < old->second) metrics[row.InterfaceLuid.Value] = metric;
        }
    }
    FreeMibTable(table);
    std::map<std::pair<std::uint64_t, std::string>, std::string> neighbours;
    PMIB_IPNET_TABLE2 neighbour_table = nullptr;
    if (GetIpNetTable2(AF_INET, &neighbour_table) == NO_ERROR && neighbour_table) {
        for (ULONG i = 0; i < neighbour_table->NumEntries; ++i) {
            const auto& row = neighbour_table->Table[i];
            if (row.PhysicalAddressLength != 6 || row.State < NlnsProbe) continue;
            const auto mac = mac_text(row.PhysicalAddress, row.PhysicalAddressLength);
            if (!mac.empty()) neighbours[{row.InterfaceLuid.Value, address_text(row.Address)}] = mac;
        }
        FreeMibTable(neighbour_table);
    }
    std::vector<physical::Attachment> out;
    std::uint64_t primary = 0, best_metric = std::numeric_limits<std::uint64_t>::max();
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
        if (classify_kind(a) != "physical") continue;
        diagnostics.interface_count = std::min<std::size_t>(4096, diagnostics.interface_count + 1);
        if (a->OperStatus != IfOperStatusUp) { diagnostics.reject(physical::CandidateRejection::inactiveLink); continue; }
        if (!a->IfIndex || !a->Luid.Value) { diagnostics.reject(physical::CandidateRejection::missingInterfaceIndex); continue; }
        MIB_IF_ROW2 hardware{}; hardware.InterfaceLuid = a->Luid; hardware.InterfaceIndex = a->IfIndex;
        physical::HardwareEvidence authority;
        authority.expected_luid = a->Luid.Value; authority.expected_index = a->IfIndex;
        authority.readable = read_interface_state(hardware) == NO_ERROR;
        authority.actual_luid = hardware.InterfaceLuid.Value; authority.actual_index = hardware.InterfaceIndex;
        authority.up = hardware.OperStatus == IfOperStatusUp;
        authority.hardware_interface = hardware.InterfaceAndOperStatusFlags.HardwareInterface != 0;
        authority.filter_interface = hardware.InterfaceAndOperStatusFlags.FilterInterface != 0;
        authority.endpoint_interface = hardware.InterfaceAndOperStatusFlags.EndPointInterface != 0;
        // Description/IfType can exclude known virtuals, but only positive OS
        // hardware evidence can admit a physical campus-authentication candidate.
        if (!physical::physical_hardware_confirmed(authority)) { diagnostics.reject(physical::CandidateRejection::unsupportedInterface); continue; }
        physical::Attachment attachment;
        attachment.physical = true; attachment.up = true;
        auto& r = attachment.route;
        r.interface_key = std::to_string(a->Luid.Value); r.interface_luid = a->Luid.Value; r.interface_index = a->IfIndex;
        r.display_name = narrow(a->FriendlyName ? a->FriendlyName : L"");
        r.descriptor.kind = a->IfType == IF_TYPE_IEEE80211 ? "wifi" : "ethernet";
        std::vector<std::string> gateways;
        for (auto* g = a->FirstGatewayAddress; g; g = g->Next) {
            if (!g->Address.lpSockaddr || g->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto ip = sockaddr_text(g->Address.lpSockaddr);
            if (physical::usable_ipv4(ip)) gateways.push_back(ip);
        }
        std::sort(gateways.begin(), gateways.end()); gateways.erase(std::unique(gateways.begin(), gateways.end()), gateways.end());
        if (gateways.size() != 1) {
            diagnostics.reject(gateways.empty() ? physical::CandidateRejection::invalidGateway : physical::CandidateRejection::conflictingDefaultRoutes);
            continue;
        }
        r.gateway_ipv4 = gateways.front();
        attachment.bound_default_gateways = defaults[a->Luid.Value];
        std::map<std::string, std::string> source_networks;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET || u->DadState != IpDadStatePreferred ||
                !u->ValidLifetime || u->OnLinkPrefixLength == 0 || u->OnLinkPrefixLength > 32) continue;
            const auto ip = sockaddr_text(u->Address.lpSockaddr);
            if (!physical::usable_ipv4(ip)) continue;
            MIB_UNICASTIPADDRESS_ROW current{};
            InitializeUnicastIpAddressEntry(&current);
            current.InterfaceLuid = a->Luid;
            current.Address.Ipv4 = *reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            if (GetUnicastIpAddressEntry(&current) != NO_ERROR || current.SkipAsSource || current.DadState != IpDadStatePreferred || !current.ValidLifetime ||
                current.OnLinkPrefixLength != u->OnLinkPrefixLength || current.InterfaceLuid.Value != a->Luid.Value ||
                current.InterfaceIndex != a->IfIndex) continue;
            r.addresses.push_back(ip);
            auto one = *u; one.Next = nullptr;
            source_networks[ip] = network_text(&one);
        }
        // Adapter-address enumeration order is not a source preference. Ask the
        // read-only routing API on this exact LUID, targeting its own gateway.
        SOCKADDR_INET target{}, source{}; target.si_family = AF_INET;
        if (InetPtonA(AF_INET, r.gateway_ipv4.c_str(), &target.Ipv4.sin_addr) != 1) {
            diagnostics.reject(physical::CandidateRejection::invalidGateway); continue;
        }
        MIB_IPFORWARD_ROW2 selected{};
        if (GetBestRoute2(&a->Luid, a->IfIndex, nullptr, &target, 0, &selected, &source) != NO_ERROR ||
            selected.InterfaceLuid.Value != a->Luid.Value || selected.InterfaceIndex != a->IfIndex || source.si_family != AF_INET) {
            diagnostics.reject(physical::CandidateRejection::missingSourceAddress); continue;
        }
        r.source_ipv4 = address_text(source);
        const auto source_net = source_networks.find(r.source_ipv4);
        if (source_net == source_networks.end()) { diagnostics.reject(physical::CandidateRejection::invalidSourceAddress); continue; }
        r.descriptor.raw["net"] = source_net->second;
        std::sort(r.addresses.begin(), r.addresses.end());
        r.addresses.erase(std::unique(r.addresses.begin(), r.addresses.end()), r.addresses.end());
        r.addresses.erase(std::find(r.addresses.begin(), r.addresses.end(), r.source_ipv4));
        r.addresses.insert(r.addresses.begin(), r.source_ipv4);
        for (auto* d = a->FirstDnsServerAddress; d; d = d->Next) {
            if (!d->Address.lpSockaddr || d->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto ip = sockaddr_text(d->Address.lpSockaddr);
            if (!physical::usable_ipv4(ip)) {
                attachment.collection_failure = physical::CandidateRejection::invalidDNS;
                r.dns_servers.clear(); break;
            }
            r.dns_servers.push_back(ip);
        }
        std::sort(r.dns_servers.begin(), r.dns_servers.end());
        r.dns_servers.erase(std::unique(r.dns_servers.begin(), r.dns_servers.end()), r.dns_servers.end());
        for (const auto& dns : r.dns_servers) r.descriptor.raw["dns"] += (r.descriptor.raw["dns"].empty() ? "" : ",") + dns;
        r.descriptor.raw["gwip"] = r.gateway_ipv4;
        if (const auto n = neighbours.find({r.interface_luid, r.gateway_ipv4}); n != neighbours.end()) r.descriptor.raw["gw"] = n->second;
        if (a->Dhcpv4Server.lpSockaddr && a->Dhcpv4Server.lpSockaddr->sa_family == AF_INET) {
            const auto dhcp = sockaddr_text(a->Dhcpv4Server.lpSockaddr);
            if (physical::usable_ipv4(dhcp)) r.descriptor.raw["dhcp"] = dhcp;
            // Static configurations legitimately have no DHCP server. A present
            // malformed server on a DHCP-enabled NIC cannot be silently omitted.
            else if (a->Flags & IP_ADAPTER_DHCP_ENABLED)
                attachment.collection_failure = physical::CandidateRejection::invalidDHCPServer;
        }
        const auto suffix = lower_ascii(narrow(a->DnsSuffix ? a->DnsSuffix : L""));
        if (!suffix.empty()) r.descriptor.raw["sfx"] = suffix;
        if (const auto m = metrics.find(r.interface_luid); m != metrics.end() &&
            (m->second < best_metric || (m->second == best_metric && r.interface_luid < primary))) {
            best_metric = m->second; primary = r.interface_luid;
        }
        out.push_back(std::move(attachment));
    }
    for (auto& a : out) a.route.is_system_primary = a.route.interface_luid == primary;
    return out;
}

}  // namespace

std::vector<physical::Route> capture_physical_candidates() {
    return capture_physical_candidate_report().routes;
}

physical::CandidateReport capture_physical_candidate_report() {
    bool first_readable = false, second_readable = false;
    physical::CandidateDiagnostics first_diagnostics, second_diagnostics;
    const auto first = read_physical_evidence(first_readable, first_diagnostics);
    const auto second = read_physical_evidence(second_readable, second_diagnostics);
    if (!first_readable || !second_readable) {
        physical::CandidateReport report;
        report.diagnostics.reject(physical::CandidateRejection::systemSnapshotUnavailable);
        return report;
    }
    auto report = physical::stable_candidate_report(first, second);
    report.diagnostics.interface_count = second_diagnostics.interface_count;
    // Candidate decoding and collection reject disjoint records. Take the larger
    // collection count across snapshots without counting the same failure twice.
    for (const auto& [reason, count] : first_diagnostics.rejections)
        second_diagnostics.rejections[reason] = std::max(second_diagnostics.rejections[reason], count);
    for (const auto& [reason, count] : second_diagnostics.rejections) report.diagnostics.reject(reason, count);
    return report;
}

bool physical_route_current(const physical::Route& expected) {
    const auto current = capture_physical_candidates();
    return std::any_of(current.begin(), current.end(), [&](const auto& r) { return r == expected; });
}

NetworkSnapshot capture_network(bool read_wifi_name) {
    NetworkSnapshot snapshot;
    snapshot.path.requires_physical_binding = true;
    auto physical_report = capture_physical_candidate_report();
    snapshot.path.physical_candidates = std::move(physical_report.routes);
    snapshot.physical_diagnostics = std::move(physical_report.diagnostics);
    collect_physical_egress(snapshot, [](MIB_IPINTERFACE_ROW& row) { return GetIpInterfaceEntry(&row); });
    ULONG size = 32 * 1024;
    std::vector<unsigned char> buffer(size);
    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) {
        snapshot.access = "unknown";
        return snapshot;
    }
    const auto adapters = collect_adapters(reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), snapshot, read_interface_state);

    // Gateway hardware addresses come from the neighbour cache (read-only; nothing is sent to resolve them).
    std::map<std::pair<std::uint64_t, std::string>, std::string> neighbours;
    PMIB_IPNET_TABLE2 neighbour_table = nullptr;
    if (GetIpNetTable2(AF_UNSPEC, &neighbour_table) == NO_ERROR && neighbour_table) {
        for (ULONG i = 0; i < neighbour_table->NumEntries; ++i) {
            const MIB_IPNET_ROW2& row = neighbour_table->Table[i];
            if (row.PhysicalAddressLength != 6 || row.State < NlnsProbe) continue;
            const std::string mac = mac_text(row.PhysicalAddress, row.PhysicalAddressLength);
            if (!mac.empty()) neighbours[{row.InterfaceLuid.Value, address_text(row.Address)}] = mac;
        }
        FreeMibTable(neighbour_table);
    }

    std::vector<routes::Interface> interfaces;
    for (const Adapter& a : adapters) interfaces.push_back({a.key, a.up, a.up, a.kind});
    std::vector<routes::Route> table;
    bool default_on_physical = false, default_on_tunnel = false;
    PMIB_IPFORWARD_TABLE2 routes_raw = nullptr;
    if (GetIpForwardTable2(AF_UNSPEC, &routes_raw) == NO_ERROR && routes_raw) {
        snapshot.tunnel_known = true;
        for (ULONG i = 0; i < routes_raw->NumEntries; ++i) {
            const MIB_IPFORWARD_ROW2& row = routes_raw->Table[i];
            routes::Route route;
            route.family = row.DestinationPrefix.Prefix.si_family == AF_INET6 ? 6 : 4;
            route.destination = address_text(row.DestinationPrefix.Prefix);
            route.prefix = row.DestinationPrefix.PrefixLength;
            route.interface_name = std::to_string(row.InterfaceLuid.Value);
            const auto adapter = std::find_if(adapters.begin(), adapters.end(), [&](const Adapter& a) { return a.luid == row.InterfaceLuid.Value; });
            if (adapter != adapters.end() && adapter->up) route.flags.push_back("up");
            if (!unspecified(row.NextHop)) route.flags.push_back("gateway");
            if (row.Protocol == MIB_IPPROTO_LOCAL || row.Loopback) route.flags.push_back("local");
            table.push_back(route);
            if (route.prefix == 0 && adapter != adapters.end() && adapter->up) {
                if (adapter->kind == "tunnel") default_on_tunnel = true;
                if (adapter->kind == "physical") default_on_physical = true;
            }
        }
        FreeMibTable(routes_raw);
    }
    snapshot.tunnel = routes::classify(table, interfaces, "windows");

    const Adapter* primary = project_adapter_links(snapshot, adapters, default_on_physical, default_on_tunnel);
    if (primary) {
        // Opaque fingerprint: adapter LUID + gateway + DNS suffix. Never shown or exported.
        const std::string material = primary->key + "|" + primary->gateway_bytes + "|" + narrow(primary->dns_suffix);
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(fnv1a(material)));
        snapshot.path.identity = hex;
    }
    std::string summary;
    for (const auto& name : snapshot.adapter_names) {
        if (!summary.empty()) summary += "、";
        summary += name;
    }
    snapshot.path.interfaces = summary;
    for (const Adapter& a : adapters) {
        if (!a.up || !a.link_active || a.kind != "physical" || !a.has_gateway) continue;
        NetworkDescriptor descriptor;
        descriptor.kind = a.wifi ? "wifi" : "ethernet";
        if (!a.gateway_ip.empty()) descriptor.raw["gwip"] = a.gateway_ip;
        if (const auto it = neighbours.find({a.luid, a.gateway_ip}); it != neighbours.end()) descriptor.raw["gw"] = it->second;
        if (!a.network4.empty()) descriptor.raw["net"] = a.network4;
        if (!a.dhcp_ip.empty()) descriptor.raw["dhcp"] = a.dhcp_ip;
        if (!a.dns_list.empty()) descriptor.raw["dns"] = a.dns_list;
        const std::string suffix = lower_ascii(narrow(a.dns_suffix));
        if (!suffix.empty()) descriptor.raw["sfx"] = suffix;
        snapshot.path.descriptors.push_back(std::move(descriptor));
    }
    if (primary && snapshot.path.descriptors.size() == 1 && snapshot.path.descriptors.front().reliable()) {
        NetworkLabel label;
        label.kind = primary->wifi ? "wifi" : "ethernet";
        if (primary->wifi && read_wifi_name) label.ssid = connected_ssid(primary->luid);
        label.display_name = primary->wifi ? label.ssid.value_or(NetworkLabels::fallback("wifi")) : NetworkLabels::clean_name(primary->name);
        if (label.display_name.empty()) label.display_name = NetworkLabels::fallback(label.kind);
        snapshot.current_label = std::move(label);
    }
    return snapshot;
}

#ifdef SZU_NETWORK_TESTING
void collect_physical_egress_for_testing(NetworkSnapshot& snapshot,
    const std::function<NETIO_STATUS(MIB_IPINTERFACE_ROW&)>& read_state) {
    collect_physical_egress(snapshot, read_state);
}

NetworkSnapshot network_link_projection_for_testing(const IP_ADAPTER_ADDRESSES* first,
    const std::function<NETIO_STATUS(MIB_IF_ROW2&)>& read_state, bool virtual_default) {
    NetworkSnapshot snapshot;
    snapshot.path.requires_physical_binding = true;
    if (!read_state) { snapshot.access = "unknown"; return snapshot; }
    const auto adapters = collect_adapters(first, snapshot, read_state);
    snapshot.tunnel_known = true;
    snapshot.tunnel.tunnel = virtual_default;
    project_adapter_links(snapshot, adapters, false, virtual_default);
    return snapshot;
}
#endif

std::string physical_egress_status(const NetworkSnapshot& snapshot, const std::optional<physical::Route>& selected) {
    const auto route = selected ? selected : snapshot.path.physical_candidates.size() == 1
        ? std::optional<physical::Route>(snapshot.path.physical_candidates.front()) : std::nullopt;
    if (!route) return "unknown";
    const auto found = std::find_if(snapshot.physical_egress.begin(), snapshot.physical_egress.end(),
        [&](const auto& item) { return item.route == *route; });
    if (found == snapshot.physical_egress.end()) return "unknown";
    for (const auto* value : {"normal", "forwarding", "weakHostSend", "forwardingAndWeakHostSend"})
        if (found->binding == value) return value;
    return "unknown";
}

namespace {
void WINAPI on_interface_change(PVOID context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
    static_cast<NetworkWatcher*>(context)->notify();
}
void WINAPI on_route_change(PVOID context, PMIB_IPFORWARD_ROW2, MIB_NOTIFICATION_TYPE) {
    static_cast<NetworkWatcher*>(context)->notify();
}
}  // namespace

NetworkWatcher::NetworkWatcher(std::function<void()> changed) : changed_(std::move(changed)) {
    HANDLE handle = nullptr;
    if (NotifyIpInterfaceChange(AF_UNSPEC, &on_interface_change, this, FALSE, &handle) == NO_ERROR) interface_handle_ = handle;
    handle = nullptr;
    if (NotifyRouteChange2(AF_UNSPEC, &on_route_change, this, FALSE, &handle) == NO_ERROR) route_handle_ = handle;
}

NetworkWatcher::~NetworkWatcher() {
    if (interface_handle_) CancelMibChangeNotify2(static_cast<HANDLE>(interface_handle_));
    if (route_handle_) CancelMibChangeNotify2(static_cast<HANDLE>(route_handle_));
}

void NetworkWatcher::notify() {
    if (changed_) changed_();
}

proxy::WindowsRaw read_system_proxy() {
    proxy::WindowsRaw raw;
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&config)) {
        raw.readable = false;
        return raw;
    }
    raw.auto_detect = config.fAutoDetect != FALSE;
    if (config.lpszAutoConfigUrl) {
        raw.auto_config_url = narrow(config.lpszAutoConfigUrl);
        GlobalFree(config.lpszAutoConfigUrl);
    }
    if (config.lpszProxy) {
        raw.proxy = narrow(config.lpszProxy);
        GlobalFree(config.lpszProxy);
    }
    if (config.lpszProxyBypass) {
        raw.bypass = narrow(config.lpszProxyBypass);
        GlobalFree(config.lpszProxyBypass);
    }
    return raw;
}

struct RegistryWatcher::Impl {
    HKEY key = nullptr;
    HANDLE changed = nullptr;
    HANDLE stop = nullptr;
    std::thread thread;
    std::function<void()> callback;
};

RegistryWatcher::RegistryWatcher(const wchar_t* subkey, std::function<void()> changed) : impl_(new Impl) {
    impl_->callback = std::move(changed);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_NOTIFY, &impl_->key) != ERROR_SUCCESS) return;
    impl_->changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    impl_->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->changed || !impl_->stop) return;
    Impl* impl = impl_;
    impl_->thread = std::thread([impl] {
        while (true) {
            if (RegNotifyChangeKeyValue(impl->key, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME, impl->changed, TRUE) != ERROR_SUCCESS)
                return;
            HANDLE handles[2] = {impl->stop, impl->changed};
            const DWORD which = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (which != WAIT_OBJECT_0 + 1) return;
            if (impl->callback) impl->callback();
        }
    });
}

RegistryWatcher::~RegistryWatcher() {
    if (impl_->stop) SetEvent(impl_->stop);
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->changed) CloseHandle(impl_->changed);
    if (impl_->stop) CloseHandle(impl_->stop);
    if (impl_->key) RegCloseKey(impl_->key);
    delete impl_;
}

}  // namespace szu::win
