/* Exact-bit x87 diagnostics, independent of graphics and text shaping. */
#pragma once
#include <cstdint>
#include <cstring>

struct X87ProbeResult {
    std::uint64_t integerLoad;
    std::uint64_t minimum;
    std::uint64_t scaledSubnormal;
    std::uint32_t restored;
    bool passed() const {
        return integerLoad == 0x4026000000000000ULL && minimum == 12 &&
               scaledSubnormal == 0x00d0000000000000ULL && restored == 11;
    }
};

inline X87ProbeResult runX87Probe() {
    alignas(16) unsigned char saved[512] = {};
    alignas(16) unsigned char live[512] = {};
    const std::uint16_t cw = 0x027f;
    const std::uint32_t eleven = 11, oneFloat = 0x3f800000, scale = 0x5f800000;
    const std::uint64_t subnormal = 1;
    X87ProbeResult out{};
    // No API calls while live x87 values are being measured. Preserve the
    // caller's whole FPU/SSE context and report raw bits without printf FP.
    __asm__ volatile(
        "fxsave %[saved]\n\t"
        "fninit\n\t"
        "fldcw %[cw]\n\t"
        "fildl %[eleven]\n\t"
        "fstpl %[integerLoad]\n\t"
        "fildl %[eleven]\n\t"
        "fdivs %[one]\n\t"
        "frndint\n\t"
        "fadds %[one]\n\t"
        "fistpll %[minimum]\n\t"
        "fldl %[subnormal]\n\t"
        "fmuls %[scale]\n\t"
        "fstpl %[scaled]\n\t"
        "fildl %[eleven]\n\t"
        "fxsave %[live]\n\t"
        "fninit\n\t"
        "fxrstor %[live]\n\t"
        "fistpl %[restored]\n\t"
        "fxrstor %[saved]\n\t"
        : [saved] "+m"(saved), [live] "+m"(live),
          [integerLoad] "=m"(out.integerLoad), [minimum] "=m"(out.minimum),
          [scaled] "=m"(out.scaledSubnormal), [restored] "=m"(out.restored)
        : [cw] "m"(cw), [eleven] "m"(eleven), [one] "m"(oneFloat),
          [subnormal] "m"(subnormal), [scale] "m"(scale)
        : "memory", "st");
    return out;
}
