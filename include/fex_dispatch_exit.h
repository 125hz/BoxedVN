// BoxedVN's lock-free transition to the existing FEX dispatcher. GPLv2.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace boxedvn {
inline std::array<uint32_t,4> fexDispatchExit(size_t ripOffset, size_t dispatcherOffset) {
    if ((ripOffset | dispatcherOffset) & 7 || ripOffset > 32760 || dispatcherOffset > 32760) return {};
    // Native ARM64 FEX ABI: x28=frame, x0/x1=temporaries, LR=exit record.
    // Static guest GPRs, SIMD, x87 state and NZCV remain in their registers.
    return {0xf94007c0u, // ldr x0,[x30,#8] (record.GuestRIP)
            0xf9000380u | uint32_t(ripOffset/8)<<10,
            0xf9400381u | uint32_t(dispatcherOffset/8)<<10,
            0xd61f0020u}; // br x1
}
}
