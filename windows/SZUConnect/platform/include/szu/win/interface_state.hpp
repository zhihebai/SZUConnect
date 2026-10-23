#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

namespace szu::win {
// Identity, hardware flags and link state are needed; traffic counters are not.
inline NETIO_STATUS read_interface_state(MIB_IF_ROW2& row) {
    return GetIfEntry2Ex(MibIfEntryNormalWithoutStatistics, &row);
}
}  // namespace szu::win
