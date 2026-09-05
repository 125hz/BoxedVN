/* BoxedVN - FEX AArch64 prediction-stack guard recovery. GPLv2. */
#pragma once

#include <cstdint>
#include <limits>

namespace boxedvn {

// Matches the guarded mmap in BVNFEXBackend.mm, not FEX's 4 KB guest page.
constexpr std::uint64_t fexCallRetHostGuardBytes = 0x4000;
constexpr unsigned fexCallRetHostRegister = 25;

// This stack is a host return-address predictor, separate from guest RSP.
// Only admit FEX's STP pre-decrement / LDP post-increment through x25 and
// the current thread's guard. Unrelated guest memory faults must reach Wine.
inline std::uint64_t recoverFexCallRetGuard(
    std::uint64_t base, std::uint64_t size, std::uint64_t sp,
    std::uint64_t fault, std::uint32_t instruction) {
    constexpr auto guard = fexCallRetHostGuardBytes;
    if (base < guard || base > std::numeric_limits<std::uint64_t>::max() - guard ||
        size < 64 || (base & 15) || (size & 15) || (sp & 15) ||
        size > std::numeric_limits<std::uint64_t>::max() - base - guard) return 0;
    const auto end = base + size;
    if (!((fault >= base - guard && fault < base) ||
          (fault >= end && fault < end + guard))) return 0;

    // Ignore only Rt/Rt2. Retain the register, width, immediate and mode.
    const auto opcode = instruction & 0xffff83e0U;
    std::uint64_t access;
    if (opcode == 0xa9bf0320U && sp >= 16) { // stp Xn, Xm, [x25, #-16]!
        access = sp - 16;
    } else if (opcode == 0xa8c10320U) { // ldp Xn, Xm, [x25], #16
        access = sp;
    } else {
        return 0;
    }
    if (fault < access || fault - access >= 16) return 0;
    return base + size / 4;
}

} // namespace boxedvn
