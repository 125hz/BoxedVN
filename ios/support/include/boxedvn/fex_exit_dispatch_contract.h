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

// ---------------------------------------------------------------------------
// What one guest CALL's return-address push records, and what makes a return
// slot trustworthy.
//
// Device evidence, revision 59fe87a2: the guest stopped with RIP zero out of
// the epilogue of __libc_sigaction, and a checked re-read of its stack showed
// the frame chain intact -- every saved frame pointer correct -- while every
// return-address slot above it held zero. LEAVE and RET both did exactly the
// right arithmetic. So either the CALL never stored the return address, or
// something zeroed the slot before the RET, and nothing observable at RET time
// separates those two.
//
// The push therefore reads its own store back, immediately, through the same
// translated host address it just wrote. That single fact is the discriminator.
// ---------------------------------------------------------------------------

struct FexCallReturnPush final {
    // The return address the CALL meant to push.
    std::uint64_t intendedReturn;
    // Canonical guest address of the slot, which is the stack pointer after
    // the push.
    std::uint64_t slotGuest;
    // The host address that canonical slot resolves to.
    std::uint64_t slotHost;
    // What an immediate load from slotHost returned.
    std::uint64_t readback;
};

enum class FexCallPushVerdict {
    // The slot holds what the CALL stored. A later zero is somebody else's
    // write, not a lost push.
    Stored,
    // The push did not land: the store never reached the slot it named.
    NotStored,
    // The slot holds something else entirely.
    Mismatched,
};

constexpr FexCallPushVerdict classifyCallReturnPush(
    const FexCallReturnPush& push) noexcept {
    if (push.readback == push.intendedReturn) {
        return FexCallPushVerdict::Stored;
    }
    return push.readback == 0 ? FexCallPushVerdict::NotStored
                              : FexCallPushVerdict::Mismatched;
}

// A CALL's return address is never zero: it is the address of the instruction
// after the call. A recorded push claiming otherwise is itself the anomaly.
constexpr bool fexCallReturnPushIsWellFormed(
    const FexCallReturnPush& push) noexcept {
    return push.intendedReturn != 0 && push.slotGuest != 0 &&
           push.slotHost != 0;
}

// The Push lowering copies the value into a temporary when it would otherwise
// be destroyed by the address or result write. These are the shapes that can
// occur, and the bits the witness packs them into.
enum class FexPushOverlap : std::uint64_t {
    // Value, address and result are all distinct.
    None = 0,
    // The value register is also the address register.
    ValueIsAddress = 1ULL << 8,
    // The value register is also the result register.
    ValueIsResult = 1ULL << 9,
    // The lowering copied the value to a temporary before storing it.
    ValueCopied = 1ULL << 10,
};

constexpr std::uint64_t fexPushInfo(std::uint64_t valueSizeBytes,
                                    bool valueIsAddress, bool valueIsResult,
                                    bool valueCopied) noexcept {
    std::uint64_t info = valueSizeBytes & 0xFF;
    if (valueIsAddress) {
        info |= static_cast<std::uint64_t>(FexPushOverlap::ValueIsAddress);
    }
    if (valueIsResult) {
        info |= static_cast<std::uint64_t>(FexPushOverlap::ValueIsResult);
    }
    if (valueCopied) {
        info |= static_cast<std::uint64_t>(FexPushOverlap::ValueCopied);
    }
    return info;
}

constexpr std::uint64_t fexPushInfoValueSize(std::uint64_t info) noexcept {
    return info & 0xFF;
}

constexpr bool fexPushInfoHas(std::uint64_t info,
                              FexPushOverlap overlap) noexcept {
    return (info & static_cast<std::uint64_t>(overlap)) != 0;
}

// ---------------------------------------------------------------------------
// What a guest target the host can never execute must become.
// ---------------------------------------------------------------------------

enum class FexInvalidTargetDisposition {
    // Compile it. Only ever correct for an address the guest can execute.
    Compile,
    // Raise a synchronous guest fault at that address, which is what a real
    // kernel does when a program jumps into an unmapped page.
    GuestFault,
};

// A guest RIP inside the null page is not code and never will be. It must not
// be compiled, and refusing it must not hand the dispatcher a null host
// pointer to branch to -- that is a host crash at PC 0 with the guest RIP
// already discarded, which is how two device runs ended.
constexpr FexInvalidTargetDisposition dispositionForGuestTarget(
    std::uint64_t guestRIP) noexcept {
    return guestRIP < 0x1000 ? FexInvalidTargetDisposition::GuestFault
                             : FexInvalidTargetDisposition::Compile;
}

// A compiled-block lookup that produced no host code. Branching to it is never
// correct, whatever produced it.
constexpr bool fexHostTargetIsExecutable(std::uint64_t hostTarget) noexcept {
    return hostTarget != 0;
}

// FEX does not make SYSCALL a block-ending instruction for this non-Windows
// host configuration. If the BoxedWine syscall layer restores or otherwise
// replaces RIP (rt_sigreturn, synchronous self-signal delivery, exec, ...),
// returning normally would keep executing the already translated bytes after
// SYSCALL and silently discard that replacement. Such a syscall must leave the
// current FEX block and re-enter the dispatcher with the resulting RIP.
constexpr bool fexSyscallMustLeaveCurrentBlock(
    std::uint64_t postSyscallRIP,
    std::uint64_t resultingRIP) noexcept {
    return resultingRIP != postSyscallRIP;
}

}  // namespace boxedvn

#endif  // BOXEDVN_FEX_EXIT_DISPATCH_CONTRACT_H
