/*
 * BoxedWine - CMPXCHG16B semantics and encoding rules.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * Wine's ntdll reaches `lock cmpxchg16b [r8]` (f0 49 0f c7 08) in a hot path
 * and the device log stopped there with "unimpl opcode". The instruction is a
 * 128-bit compare-and-swap: memory is compared against RDX:RAX, and on
 * equality RCX:RBX is stored and ZF set, while on inequality memory is left
 * alone and what was actually there is loaded into RDX:RAX with ZF clear.
 * The caller in ntdll follows it with `sete r10b`, so ZF is the architectural
 * result that matters; the SDM leaves the other status flags undefined.
 *
 * The decision and the arithmetic live here, apart from the interpreter, so
 * they can be executed and tested without a CPU, an address space, or a guest.
 * cpu64.cpp supplies the memory access and the lock; everything it decides is
 * decided by these two functions.
 */

#ifndef __CMPXCHG16B_H__
#define __CMPXCHG16B_H__

#include <cstdint>

#if defined(__cplusplus)

namespace boxedvn {

struct Cmpxchg16bResult {
    // True when the 128-bit memory value equalled RDX:RAX.
    bool succeeded = false;
    // The two qwords to store, valid only when `succeeded`. Memory is left
    // completely untouched otherwise -- a failed compare-and-swap must not
    // write, or a competing thread's value would be lost.
    std::uint64_t writeLow = 0;
    std::uint64_t writeHigh = 0;
    // RAX and RDX afterwards. On failure they carry what memory actually held,
    // which is how a compare-and-swap loop makes progress.
    std::uint64_t raxAfter = 0;
    std::uint64_t rdxAfter = 0;
    // ZF afterwards: set on success, clear on failure.
    bool zeroFlag = false;
};

inline Cmpxchg16bResult evaluateCmpxchg16b(std::uint64_t memoryLow,
                                           std::uint64_t memoryHigh,
                                           std::uint64_t rax,
                                           std::uint64_t rdx,
                                           std::uint64_t rbx,
                                           std::uint64_t rcx) noexcept {
    Cmpxchg16bResult result;
    if (memoryLow == rax && memoryHigh == rdx) {
        result.succeeded = true;
        result.writeLow = rbx;
        result.writeHigh = rcx;
        result.raxAfter = rax;
        result.rdxAfter = rdx;
        result.zeroFlag = true;
        return result;
    }
    result.succeeded = false;
    result.raxAfter = memoryLow;
    result.rdxAfter = memoryHigh;
    result.zeroFlag = false;
    return result;
}

// True when a 0F C7 encoding is CMPXCHG16B.
//
// The reg field is an opcode extension, so REX.R is not part of it and the
// caller must mask before asking. REX.W selects the 128-bit form: without it
// the same encoding is CMPXCHG8B, which nothing has needed. /1 has no register
// form at all -- mod==11 is an invalid encoding and must not be executed as
// something else.
inline bool isCmpxchg16bEncoding(std::uint8_t regField, bool rexW,
                                 bool registerForm) noexcept {
    return (regField & 7) == 1 && rexW && !registerForm;
}

} // namespace boxedvn

#endif // __cplusplus

#endif
