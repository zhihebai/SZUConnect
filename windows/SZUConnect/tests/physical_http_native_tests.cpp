// Real Windows SDK/libcurl/Schannel gate and source-binding isolation tests.
// Reads the loopback index/LUID and binds one unconnected loopback TCP socket.
// Never contacts campus, modifies routes/proxy/DNS or reads credentials/files.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <cstdint>
#include <cstdio>

namespace szu::win {
bool physical_http_native_socket_test(std::uint32_t index, std::uint64_t luid);
bool physical_http_native_policy_test();
}
int main() {
    int failures = 0;
    const bool policy = szu::win::physical_http_native_policy_test();
    std::printf("Native fixed target/TLS/cookie/bounds policy: %s\n", policy ? "PASS" : "FAIL");
    if (!policy) ++failures;
    PMIB_UNICASTIPADDRESS_TABLE table = nullptr;
    bool socket = false;
    if (GetUnicastIpAddressTable(AF_INET, &table) == NO_ERROR && table) {
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const auto& row = table->Table[i];
            if (row.Address.si_family == AF_INET && row.Address.Ipv4.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
                socket = szu::win::physical_http_native_socket_test(row.InterfaceIndex, row.InterfaceLuid.Value); break;
            }
        }
        FreeMibTable(table);
    }
    std::printf("Native exact interface/source/pre-send/cancellation socket: %s\n", socket ? "PASS" : "FAIL");
    if (!socket) ++failures;
    return failures ? 1 : 0;
}
