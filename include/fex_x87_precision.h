// Optional FEX F64 stack <-> architectural x87 state conversion. GPLv2.
#pragma once
#include <cstdint>
#include <array>
extern "C" {
#include "../lib/softfloat/source/include/platform.h"
#include "../lib/softfloat/source/include/softfloat.h"
}
namespace boxedvn {
inline extFloat80_t fexX87ToExtended(uint64_t low, uint16_t high, bool reduced) {
    extFloat80_t value{};
    value.signif = low;
    value.signExp = high;
    if (reduced) {
        const auto savedFlags = softfloat_exceptionFlags;
        value = f64_to_extF80(float64_sf{low});
        softfloat_exceptionFlags = savedFlags;
    }
    return value;
}
inline std::array<uint64_t, 2> fexX87FromExtended(extFloat80_t value,
                                                bool reduced, uint16_t cw) {
    if (!reduced) return {value.signif, value.signExp};
    const auto savedRound = softfloat_roundingMode;
    const auto savedFlags = softfloat_exceptionFlags;
    const uint_fast8_t modes[] = {softfloat_round_near_even, softfloat_round_min,
                                 softfloat_round_max, softfloat_round_minMag};
    softfloat_roundingMode = modes[(cw >> 10) & 3];
    const uint64_t bits = extF80_to_f64(value).v;
    softfloat_roundingMode = savedRound;
    softfloat_exceptionFlags = savedFlags;
    return {bits, 0};
}
}
