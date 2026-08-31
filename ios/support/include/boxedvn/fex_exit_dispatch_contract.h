/*
 * BoxedVN - FEX exit-dispatch ABI contract.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BOXEDVN_FEX_EXIT_DISPATCH_CONTRACT_H
#define BOXEDVN_FEX_EXIT_DISPATCH_CONTRACT_H

#include <cstddef>
#include <cstdint>

namespace boxedvn {

// FEX emits this record immediately after an indirect call to the function in
// CpuStateFrame::Pointers.ExitFunctionLink. The definition is private to
// FEXCore, so the integration keeps the exact public ABI shape here.
struct FexExitFunctionLinkData final {
    std::uint64_t hostCode;
    std::uint64_t guestRIP;
    std::int64_t callerOffset;
};

static_assert(sizeof(FexExitFunctionLinkData) == 24);
static_assert(offsetof(FexExitFunctionLinkData, hostCode) == 0);
static_assert(offsetof(FexExitFunctionLinkData, guestRIP) == 8);
static_assert(offsetof(FexExitFunctionLinkData, callerOffset) == 16);

struct FexExitDispatchTransition final {
    std::uint64_t guestRIP;
    std::uint64_t hostTarget;
};

// Direct block linking is optional. An integration may instead restore the
// target guest RIP and send execution through the ordinary dispatcher cache.
constexpr FexExitDispatchTransition dispatchWithoutBlockLinking(
    const FexExitFunctionLinkData& record,
    std::uint64_t dispatcherLoopTop) noexcept {
    return {record.guestRIP, dispatcherLoopTop};
}

// ---------------------------------------------------------------------------
// The L1 lookup an indirect exit performs before it branches.
//
// FEX caches {HostCode, GuestCode} pairs in a direct-mapped table that is
// zero-initialised, and marks an entry dead by zeroing its GuestCode. Two
// consequences follow, and the emitted lookup in DEF_OP(ExitFunction) honoured
// neither: an EMPTY slot reads {0, 0}, so comparing the guest key alone makes
// slot zero a hit for a dynamic guest target of zero -- and the emitted
// `ret TMP2` then branches to host address zero.
//
// A device run does exactly that in the `leave; mov eax, edx; ret` epilogue of
// glibc's __libc_sigaction at guest 0x7a4004d4b9: host PC 0, fault address 0,
// and no way left to recover the guest target or the block that produced it.
// The dispatcher's own copy of this lookup has always tested the host pointer
// as well; this is that rule, stated where it can be exercised exhaustively.
// ---------------------------------------------------------------------------

struct FexLookupCacheEntry final {
    std::uint64_t hostCode;
    std::uint64_t guestCode;
};

enum class FexIndirectExitPath {
    // The cache named a real compiled block for this exact target.
    CacheHit,
    // Anything else. Execution re-enters the dispatcher with the target in
    // State.rip, which compiles the block -- or raises a guest fault when the
    // guest has no executable mapping there.
    DispatcherFallback,
};

struct FexIndirectExit final {
    FexIndirectExitPath path;
    std::uint64_t hostTarget;
    std::uint64_t guestRIP;
};

// A cache hit requires BOTH halves: the key must match AND the host pointer
// must be a real one. A null host pointer is never a compiled block.
constexpr FexIndirectExit resolveIndirectExit(
    const FexLookupCacheEntry& entry, std::uint64_t guestTarget,
    std::uint64_t dispatcherLoopTop) noexcept {
    if (entry.guestCode == guestTarget && entry.hostCode != 0) {
        return {FexIndirectExitPath::CacheHit, entry.hostCode, guestTarget};
    }
    return {FexIndirectExitPath::DispatcherFallback, dispatcherLoopTop,
            guestTarget};
}

// An empty slot, and what invalidation leaves behind. Both halves are cleared:
// every reader now requires a non-null host pointer, so a null is a miss and a
// miss is always correct, while leaving a dead code pointer in place would let
// a zero key match it.
constexpr FexLookupCacheEntry emptyLookupCacheEntry() noexcept {
    return {0, 0};
}

constexpr FexLookupCacheEntry invalidateLookupCacheEntry(
    const FexLookupCacheEntry& entry, std::uint64_t address) noexcept {
    return entry.guestCode == address ? emptyLookupCacheEntry() : entry;
}

// ---------------------------------------------------------------------------
// The frame arithmetic a `leave; ret` epilogue performs, and where the bytes
// it touches actually live once BoxedWine has relocated the guest stack.
// ---------------------------------------------------------------------------

// LEAVE sets RSP to RBP and pops the saved frame pointer, so the return
// address the following RET consumes is the qword above the frame pointer.
constexpr std::uint64_t fexReturnSlotForFrame(std::uint64_t rbp) noexcept {
    return rbp + 8;
}

// The saved frame pointer LEAVE restores sits at the frame pointer itself.
constexpr std::uint64_t fexSavedFramePointerSlot(std::uint64_t rbp) noexcept {
    return rbp;
}

// After LEAVE and RET, the stack pointer is above both qwords.
constexpr std::uint64_t fexStackPointerAfterLeaveRet(std::uint64_t rbp) noexcept {
    return rbp + 16;
}

// A RET has already consumed the qword below the stack pointer it leaves
// behind, which is the slot to re-read when asking what it actually popped.
constexpr std::uint64_t fexPoppedSlotForStackPointer(std::uint64_t rsp) noexcept {
    return rsp - 8;
}

}  // namespace boxedvn

#endif  // BOXEDVN_FEX_EXIT_DISPATCH_CONTRACT_H
