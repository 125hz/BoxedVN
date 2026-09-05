// Native x86 reference for the device's exact-bit x87 diagnostic.
#include "../../tools/guest-probes/x87_probe.h"
#include <cstdio>

int main() {
    const auto result = runX87Probe();
    std::printf("x87 reference: integer=%016llx minimum=%llu scaled=%016llx restored=%u pass=%d\n",
                (unsigned long long)result.integerLoad,
                (unsigned long long)result.minimum,
                (unsigned long long)result.scaledSubnormal,
                unsigned(result.restored), result.passed());
    return result.passed() ? 0 : 1;
}
