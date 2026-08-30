/*
 * BoxedWine - recognising Wine's PE-side NT syscall stubs.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * Wine's PE ntdll does not call the Unix side directly. winebuild emits one
 * 32-byte thunk per NT service, and each thunk chooses between a raw x86-64
 * SYSCALL and an indirect call to __wine_syscall_dispatcher, based on the
 * SystemCall flag in KUSER_SHARED_DATA. The packaged ntdll.dll (SHA-256
 * 1a811cce0973b86d307ca19eae613345e55a1b0e285b381a159fbb4f899d7c2c) uses:
 *
 *     4c 8b d1                    mov  r10, rcx
 *     b8 <ordinal>                mov  eax, <NT ordinal>
 *     f6 04 25 08 03 fe 7f 01     test byte ptr [0x7ffe0308], 1
 *     75 03                       jne  .bridge
 *     0f 05                       syscall           <-- the raw path
 *     c3                          ret
 *   .bridge:
 *     eb 01                       jmp  .indirect
 *     c3                          ret               ; padding
 *   .indirect:
 *     ff 14 25 00 10 fe 7f        call [0x7ffe1000] ; __wine_syscall_dispatcher
 *     c3                          ret
 *
 * The jne lands on the `eb 01` at syscall+3, which hops the padding `ret` at
 * syscall+5 and reaches the dispatcher call at syscall+6. Resuming at the jne
 * target -- syscall+3 -- is therefore correct and is what this reports: it is
 * the address Wine's own branch would have gone to, so the redirect follows
 * Wine's control flow rather than second-guessing it.
 *
 * An earlier revision of this matcher assumed the dispatcher call sat directly
 * at syscall+3 with no bridge, a 29-byte thunk. No Wine emits that here, so
 * nothing matched, ordinals 154 and 227 reached BoxedWine's Linux syscall
 * table, and the device log shows the consequence: -ENOSYS returned to a
 * caller that retries forever. That layout is still accepted, because it is
 * cheap to keep and is validated just as completely -- the byte at +3 selects
 * which tail is required, and neither tail is optional.
 *
 * On a host where Wine has wired the raw SYSCALL to its own dispatcher it sets
 * the SystemCall flag. BoxedWine has not, so the flag reads 0, every thunk
 * falls into the raw SYSCALL, and RAX holds a *Windows NT* ordinal rather than
 * a Linux syscall number. Dispatching that through the Linux table is
 * meaningless: NT ordinal 227 is not clock_settime and 154 is not modify_ldt.
 *
 * The repair is to flip Wine's own decision rather than to answer the wrong
 * question: set the SystemCall flag so every later thunk takes its native
 * indirect path, and steer the in-flight thunk onto that path too. That is
 * only safe if the instruction really is one of these thunks, so this header
 * validates the whole shape -- every opcode byte of the layout, both
 * KUSER_SHARED_DATA addresses decoded from their own displacement fields, both
 * branch targets computed from their own displacements, the immediate against
 * RAX, and the published dispatcher pointer.
 *
 * Deliberately no address range is involved. An older implementation gated on
 * ntdll's 0x170000000 image base; this Wine maps the stubs at module base
 * 0x6fffffc50000 and the check silently stopped matching. A genuine Linux
 * SYSCALL from glibc or the Unix ntdll cannot match this pattern no matter
 * what address it sits at, which is a far stronger guarantee than any range.
 */

#ifndef __WINE_NT_SYSCALL_STUB_H__
#define __WINE_NT_SYSCALL_STUB_H__

#include <cstdint>

// KUSER_SHARED_DATA is fixed at 0x7ffe0000 on Windows and Wine keeps it there.
// SystemCall is the byte at +0x308; Wine publishes __wine_syscall_dispatcher
// at +0x1000.
#define K_WINE_KUSER_SYSTEM_CALL_FLAG 0x7ffe0308ULL
#define K_WINE_KUSER_SYSCALL_DISPATCHER 0x7ffe1000ULL

#if defined(__cplusplus)

namespace boxedvn {

// Reads `length` bytes of canonical guest memory at `address`. Returns false
// when any byte of the range is not readable, which must not be treated as a
// match: an unreadable stub is not a stub.
using GuestMemoryReader = bool (*)(void* context, uint64_t address,
                                   uint8_t* out, unsigned length);

struct WineNtSyscallStub {
    // The NT service ordinal the thunk loads into EAX. Wine's dispatcher reads
    // it from RAX, so it has to survive the redirect.
    uint32_t ntOrdinal = 0;
    // Address of the raw SYSCALL instruction, which is where execution
    // actually is. Reported in the marker so a log names the stub rather than
    // the byte after it.
    uint64_t syscallAddress = 0;
    // Where to resume: the address Wine's own jne would have branched to,
    // always syscallAddress + 3.
    uint64_t indirectPath = 0;
    // Where the `call [0x7ffe1000]` actually is -- syscallAddress + 6 in the
    // packaged bridged layout, syscallAddress + 3 in the direct one. Not used
    // to resume; recorded so a test can prove the bridge resolves to it.
    uint64_t dispatcherCall = 0;
    // The dispatcher pointer Wine published, for the marker.
    uint64_t dispatcher = 0;
};

// The thunk's bytes relative to the SYSCALL instruction. The packaged layout
// runs to +13; the direct layout ends at +10 and is read separately.
inline constexpr int kWineNtStubFirstOffset = -18;
inline constexpr int kWineNtStubLastOffset = 13;
inline constexpr unsigned kWineNtStubLength = 32;

// The head, up to and including the byte that selects which tail follows.
inline constexpr unsigned kWineNtStubHeadLength = 22;

// True when the SYSCALL at `syscallAddress` is the raw path of one of Wine's
// NT thunks and can be safely redirected onto its own indirect path.
//
// `rax` is the guest's RAX at the SYSCALL. Requiring the thunk's own immediate
// to equal it is what separates a thunk being executed from a thunk merely
// being present: a jump into the middle of one, or a Linux syscall that
// happens to sit after matching bytes, will not agree.
inline bool matchWineNtSyscallStub(GuestMemoryReader reader, void* context,
                                   uint64_t syscallAddress, uint64_t rax,
                                   WineNtSyscallStub& out) {
    if (reader == nullptr) {
        return false;
    }
    // An NT ordinal is a 32-bit value loaded by `mov eax`, which zeroes the
    // upper half. A Linux syscall number in RAX with high bits set cannot be
    // one of these.
    if ((rax >> 32) != 0) {
        return false;
    }
    // The thunk starts 18 bytes back. Refuse to wrap around the address space
    // rather than reading somewhere else entirely.
    if (syscallAddress < 18) {
        return false;
    }

    // Read the head first: everything through the byte at +3 that decides
    // which tail this thunk has. The very first comparison below rejects an
    // ordinary glibc syscall, so the tail read is only ever reached by
    // something already shaped like a thunk.
    uint8_t head[kWineNtStubHeadLength] = {};
    const uint64_t start = syscallAddress + (uint64_t)kWineNtStubFirstOffset;
    if (!reader(context, start, head, kWineNtStubHeadLength)) {
        return false;
    }

    auto at = [&head](int offset) -> uint8_t {
        return head[offset - kWineNtStubFirstOffset];
    };
    auto dword = [&at](int offset) -> uint32_t {
        return (uint32_t)at(offset) | ((uint32_t)at(offset + 1) << 8) |
               ((uint32_t)at(offset + 2) << 16) |
               ((uint32_t)at(offset + 3) << 24);
    };

    // mov r10, rcx -- the thunk saves the NT call's first argument before
    // SYSCALL would clobber RCX.
    if (at(-18) != 0x4C || at(-17) != 0x8B || at(-16) != 0xD1) {
        return false;
    }
    // mov eax, imm32
    if (at(-15) != 0xB8) {
        return false;
    }
    const uint32_t ordinal = dword(-14);
    if (ordinal != (uint32_t)rax) {
        return false;
    }
    // test byte ptr [disp32], 1 -- opcode F6 /0 with a RIP-independent
    // absolute address (modrm 04, sib 25).
    if (at(-10) != 0xF6 || at(-9) != 0x04 || at(-8) != 0x25) {
        return false;
    }
    if ((uint64_t)dword(-7) != K_WINE_KUSER_SYSTEM_CALL_FLAG) {
        return false;
    }
    if (at(-3) != 0x01) {
        return false;
    }
    // jne rel8 -- the branch taken when the flag is set.
    if (at(-2) != 0x75) {
        return false;
    }
    const int8_t branchDisplacement = (int8_t)at(-1);
    // The jne ends at the SYSCALL, so its target is relative to that.
    const uint64_t branchTarget =
        syscallAddress + (uint64_t)(int64_t)branchDisplacement;
    if (branchTarget != syscallAddress + 3) {
        return false;
    }
    // syscall (0F 05)
    if (at(0) != 0x0F || at(1) != 0x05) {
        return false;
    }
    // ret, closing the raw path.
    if (at(2) != 0xC3) {
        return false;
    }

    const uint8_t tailSelector = at(3);
    uint64_t dispatcherCall = 0;
    if (tailSelector == 0xEB) {
        // The packaged layout: a two-byte bridge over a padding ret, then the
        // dispatcher call. Read and validate all ten remaining bytes.
        uint8_t tail[10] = {};
        if (!reader(context, syscallAddress + 4, tail, sizeof(tail))) {
            return false;
        }
        auto tailAt = [&tail](int offset) -> uint8_t {
            return tail[offset - 4];
        };
        auto tailDword = [&tailAt](int offset) -> uint32_t {
            return (uint32_t)tailAt(offset) |
                   ((uint32_t)tailAt(offset + 1) << 8) |
                   ((uint32_t)tailAt(offset + 2) << 16) |
                   ((uint32_t)tailAt(offset + 3) << 24);
        };
        // jmp rel8, ending at +5, so its target is relative to that.
        const int8_t bridgeDisplacement = (int8_t)tailAt(4);
        const uint64_t bridgeTarget =
            syscallAddress + 5 + (uint64_t)(int64_t)bridgeDisplacement;
        if (bridgeTarget != syscallAddress + 6) {
            return false;
        }
        // The padding ret the bridge hops over.
        if (tailAt(5) != 0xC3) {
            return false;
        }
        // call [disp32]
        if (tailAt(6) != 0xFF || tailAt(7) != 0x14 || tailAt(8) != 0x25) {
            return false;
        }
        if ((uint64_t)tailDword(9) != K_WINE_KUSER_SYSCALL_DISPATCHER) {
            return false;
        }
        // ret, closing the indirect path.
        if (tailAt(13) != 0xC3) {
            return false;
        }
        dispatcherCall = bridgeTarget;
    } else if (tailSelector == 0xFF) {
        // The direct layout: the dispatcher call sits on the jne target with
        // no bridge. Validated to exactly the same depth.
        uint8_t tail[7] = {};
        if (!reader(context, syscallAddress + 4, tail, sizeof(tail))) {
            return false;
        }
        auto tailAt = [&tail](int offset) -> uint8_t {
            return tail[offset - 4];
        };
        auto tailDword = [&tailAt](int offset) -> uint32_t {
            return (uint32_t)tailAt(offset) |
                   ((uint32_t)tailAt(offset + 1) << 8) |
                   ((uint32_t)tailAt(offset + 2) << 16) |
                   ((uint32_t)tailAt(offset + 3) << 24);
        };
        if (tailAt(4) != 0x14 || tailAt(5) != 0x25) {
            return false;
        }
        if ((uint64_t)tailDword(6) != K_WINE_KUSER_SYSCALL_DISPATCHER) {
            return false;
        }
        if (tailAt(10) != 0xC3) {
            return false;
        }
        dispatcherCall = syscallAddress + 3;
    } else {
        return false;
    }

    // Wine has to have published a dispatcher before any of this means
    // anything. A zero here is a thunk that is present but not yet wired.
    uint8_t pointer[8] = {};
    if (!reader(context, K_WINE_KUSER_SYSCALL_DISPATCHER, pointer, 8)) {
        return false;
    }
    uint64_t dispatcher = 0;
    for (int index = 7; index >= 0; --index) {
        dispatcher = (dispatcher << 8) | (uint64_t)pointer[index];
    }
    if (dispatcher == 0 || dispatcher == ~(uint64_t)0) {
        return false;
    }

    out.ntOrdinal = ordinal;
    out.syscallAddress = syscallAddress;
    out.indirectPath = branchTarget;
    out.dispatcherCall = dispatcherCall;
    out.dispatcher = dispatcher;
    return true;
}

} // namespace boxedvn

#endif // __cplusplus

#endif
