#pragma once
#ifdef SZU_NETWORK_TESTING
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <functional>
#include "szu/win/network.hpp"

namespace szu::win {
// Test targets inject SDK records into the production collection/projection;
// no OS capture, WLAN action, network traffic or identity candidate is created.
NetworkSnapshot network_link_projection_for_testing(const IP_ADAPTER_ADDRESSES* first,
    const std::function<NETIO_STATUS(MIB_IF_ROW2&)>& read_state, bool virtual_default = false);
void collect_physical_egress_for_testing(NetworkSnapshot&,
    const std::function<NETIO_STATUS(MIB_IPINTERFACE_ROW&)>& read_state);
}
#endif
