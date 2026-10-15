#include "../Sources/CampusPhysicalCurl.h"
#include <net/if.h>
#include <signal.h>
#include <cstdio>

int main() {
    struct sigaction disposition{};
    disposition.sa_handler = SIG_DFL;
    sigemptyset(&disposition.sa_mask);
    if (sigaction(SIGPIPE, &disposition, nullptr) != 0) return 1;
    const auto loopback = if_nametoindex("lo0");
    if (!loopback || !szu_campus_test_broken_connection(loopback)) return 2;
    struct sigaction actual{};
    if (sigaction(SIGPIPE, nullptr, &actual) != 0 || actual.sa_handler != SIG_DFL) return 3;
    std::puts("PASS: broken campus socket returns EPIPE without terminating the process or changing its signal handler");
}
