#include "szu/win/interface_state.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {
bool same_state(const MIB_IF_ROW2& a, const MIB_IF_ROW2& b) {
    return a.InterfaceLuid.Value == b.InterfaceLuid.Value && a.InterfaceIndex == b.InterfaceIndex &&
        a.OperStatus == b.OperStatus && a.MediaConnectState == b.MediaConnectState && a.AccessType == b.AccessType &&
        a.MediaType == b.MediaType &&
        a.InterfaceAndOperStatusFlags.HardwareInterface == b.InterfaceAndOperStatusFlags.HardwareInterface &&
        a.InterfaceAndOperStatusFlags.FilterInterface == b.InterfaceAndOperStatusFlags.FilterInterface &&
        a.InterfaceAndOperStatusFlags.EndPointInterface == b.InterfaceAndOperStatusFlags.EndPointInterface &&
        a.InterfaceAndOperStatusFlags.ConnectorPresent == b.InterfaceAndOperStatusFlags.ConnectorPresent &&
        a.InterfaceAndOperStatusFlags.NotMediaConnected == b.InterfaceAndOperStatusFlags.NotMediaConnected;
}
void summarize(const char* label, std::vector<double> values) {
    std::sort(values.begin(), values.end());
    std::printf("%s: count=%zu median=%.3f ms max=%.3f ms\n", label, values.size(), values[values.size() / 2], values.back());
}
}
int main(int argc, char** argv) {
    const bool benchmark = argc == 2 && std::string(argv[1]) == "--benchmark";
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2Ex(MibIfTableNormalWithoutStatistics, &table) != NO_ERROR || !table) return 1;
    std::vector<NET_LUID> interfaces;
    for (ULONG i = 0; i < table->NumEntries; ++i) interfaces.push_back(table->Table[i].InterfaceLuid);
    FreeMibTable(table);
    int stable = 0, failures = 0;
    for (const auto luid : interfaces) {
        MIB_IF_ROW2 before{}, state{}, after{};
        before.InterfaceLuid = state.InterfaceLuid = after.InterfaceLuid = luid;
        if (GetIfEntry2(&before) != NO_ERROR || szu::win::read_interface_state(state) != NO_ERROR ||
            GetIfEntry2(&after) != NO_ERROR || !same_state(before, after)) continue;
        ++stable;
        if (!same_state(state, after)) ++failures;
    }
    MIB_IF_ROW2 invalid{};
    invalid.InterfaceLuid.Value = ~0ull;
    if (szu::win::read_interface_state(invalid) == NO_ERROR) ++failures;
    if (benchmark && !interfaces.empty()) {
        std::vector<double> counters, state;
        for (int i = 0; i < 40; ++i) {
            MIB_IF_ROW2 row{}; row.InterfaceLuid = interfaces[static_cast<std::size_t>(i) % interfaces.size()];
            auto started = std::chrono::steady_clock::now();
            const auto old_status = GetIfEntry2(&row);
            const auto middle = std::chrono::steady_clock::now();
            const auto new_status = szu::win::read_interface_state(row);
            const auto finished = std::chrono::steady_clock::now();
            if (old_status == NO_ERROR && new_status == NO_ERROR) {
                counters.push_back(std::chrono::duration<double, std::milli>(middle - started).count());
                state.push_back(std::chrono::duration<double, std::milli>(finished - middle).count());
            }
        }
        if (!counters.empty()) { summarize("interface with counters", counters); summarize("interface state only", state); }
    }
    std::printf("Native interface state: %d stable comparisons, %d failures; no network traffic\n", stable, failures);
    return failures || !stable ? 1 : 0;
}
