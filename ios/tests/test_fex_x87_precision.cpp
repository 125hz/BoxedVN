#include "fex_x87_precision.h"
#include <cstdio>
int main() {
    unsigned failures = 0;
    auto check = [&](bool ok) { if (!ok) ++failures; };
    for (uint64_t bits : {0ULL, 0x8000000000000000ULL, 0x3ff0000000000000ULL,
                         0xc004000000000000ULL, 1ULL, 0x0010000000000000ULL,
                         0x7fefffffffffffffULL, 0x7ff0000000000000ULL,
                         0xfff0000000000000ULL, 0x7ff8000000000000ULL}) {
        const auto extended = boxedvn::fexX87ToExtended(bits, 0, true);
        check(boxedvn::fexX87FromExtended(extended, true, 0x37f)[0] == bits);
        const auto full = boxedvn::fexX87FromExtended(extended, false, 0x37f);
        check(full[0] == extended.signif && full[1] == extended.signExp);
    }
    extFloat80_t halfway{};
    halfway.signExp = 0x3fff; halfway.signif = 0x8000000000000400ULL;
    softfloat_roundingMode = softfloat_round_minMag;
    softfloat_exceptionFlags = softfloat_flag_invalid;
    check(boxedvn::fexX87FromExtended(halfway, true, 0x37f)[0] == 0x3ff0000000000000ULL);
    check(boxedvn::fexX87FromExtended(halfway, true, 0xb7f)[0] == 0x3ff0000000000001ULL);
    check(softfloat_roundingMode == softfloat_round_minMag);
    check(softfloat_exceptionFlags == softfloat_flag_invalid);
    auto exact = boxedvn::fexX87FromExtended(halfway, false, 0x37f);
    check(exact[0] == halfway.signif && exact[1] == halfway.signExp);
    std::printf("x87 precision conversion failures: %u\n", failures);
    return failures ? 1 : 0;
}
