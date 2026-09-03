/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"

#ifdef BOXEDWINE_GUEST_X64

#include "legacysyscall64.h"
#include "cpu64.h"
#include "kmemory64.h"
#include "kprocess.h"
#include "kthread.h"

#include <atomic>

// `struct user_desc` (asm/ldt.h) as the i386 ABI lays it out. Sixteen bytes,
// four dwords, and the flag word is a bitfield the kernel's own macros read by
// position rather than by name.
#define K_USER_DESC_SIZE            16
#define K_USER_DESC_OFF_ENTRY        0
#define K_USER_DESC_OFF_BASE         4
#define K_USER_DESC_OFF_LIMIT        8
#define K_USER_DESC_OFF_FLAGS       12

#define K_USER_DESC_SEG_32BIT       0x01
#define K_USER_DESC_CONTENTS        0x06
#define K_USER_DESC_READ_EXEC_ONLY  0x08
#define K_USER_DESC_LIMIT_IN_PAGES  0x10
#define K_USER_DESC_SEG_NOT_PRESENT 0x20
#define K_USER_DESC_USEABLE         0x40

// The descriptor's limit field is twenty bits wide.
#define K_USER_DESC_LIMIT_MASK      0xfffff

// An "empty" descriptor: the kernel's LDT_empty() reads a slot with
// read_exec_only and seg_not_present set (and nothing else) as a request to
// clear it, which is how a thread releases a TLS slot.
#define K_USER_DESC_EMPTY_FLAGS \
    (K_USER_DESC_READ_EXEC_ONLY | K_USER_DESC_SEG_NOT_PRESENT)

// modify_ldt(2) function codes.
#define K_MODIFY_LDT_READ            0
#define K_MODIFY_LDT_WRITE           1
#define K_MODIFY_LDT_WRITE_NEWMODE   0x11

// Each entry of the real LDT is eight bytes on the wire, whatever the
// sixteen-byte user_desc that describes it looks like.
#define K_LDT_ENTRY_BYTES            8

namespace boxedvn {

namespace {

U32 legacyProcessId(CPU64* cpu) {
    return cpu && cpu->thread && cpu->thread->process
        ? (U32)cpu->thread->process->id : 0;
}

U32 legacyThreadId(CPU64* cpu) {
    return cpu && cpu->thread ? (U32)cpu->thread->id : 0;
}

// The compat entry zero-extends the low half of each register into the i386
// argument, which is why a 64-bit caller of this gate has to keep whatever it
// passes below 4 GiB -- and why truncating here is the ABI rather than a
// shortcut. Wine's WoW64 setup allocates its `user_desc` in the low half of
// the address space for exactly this reason.
U32 legacyArgument(const Reg64& reg) {
    return (U32)(reg.u64 & 0xffffffffu);
}

// Everything this gate reads or writes is a 32-bit pointer into guest memory.
// A page with no mapping is EFAULT here, the way it is in the kernel; the
// sparse reader would otherwise hand back zeros and a bogus descriptor would
// be installed silently.
bool legacyAddressReadable(CPU64* cpu, U32 address, U32 length) {
    if (!cpu || !cpu->memory || length == 0) {
        return false;
    }
    const U64 first = (U64)address >> K64_PAGE_SHIFT;
    const U64 last = ((U64)address + length - 1) >> K64_PAGE_SHIFT;
    for (U64 page = first; page <= last; page++) {
        if (!cpu->memory->isPageMapped(page)) {
            return false;
        }
    }
    return true;
}

// One line per i386 syscall number this gate does not serve. A guest that
// retries an unserved number does so in a loop; the number is what identifies
// it, so the number is what the report is keyed on. Numbers at or above the
// bitmap's reach share a small budget rather than going unreported.
void reportUnservedLegacySyscall(CPU64* cpu, U32 number) {
    static constexpr U32 kTrackedNumbers = 512;
    static std::atomic<U32> reported[kTrackedNumbers / 32];
    static std::atomic<U32> highNumberReports {0};
    bool report = false;
    if (number < kTrackedNumbers) {
        const U32 bit = 1u << (number & 31);
        const U32 previous =
            reported[number >> 5].fetch_or(bit, std::memory_order_relaxed);
        report = (previous & bit) == 0;
    } else {
        report = highNumberReports.fetch_add(1, std::memory_order_relaxed) < 8;
    }
    if (!report) {
        return;
    }
    klog_fmt("BOXEDWINE_X64_LEGACY_SYSCALL pid=%u tid=%u nr=%u status=enosys "
             "rip=0x%llx",
             legacyProcessId(cpu), legacyThreadId(cpu), number,
             (unsigned long long)(cpu ? cpu->rip : 0));
}

// The first sightings of a served call, in full. Bounded for the same reason
// every other diagnostic here is: a thread that reinstalls its TLS in a loop
// must not be able to fill a device's disk.
bool shouldReportServedLegacySyscall() {
    static std::atomic<U32> reports {0};
    return reports.fetch_add(1, std::memory_order_relaxed) < 32;
}

LegacyThreadAreaDescriptor* descriptorSlot(CPU64* cpu, U32 entry) {
    if (!cpu || entry >= K_GUEST_SEGMENT_TABLE_ENTRIES) {
        return nullptr;
    }
    return &cpu->threadAreaDescriptors[entry];
}

// Fill one slot from a `user_desc` the guest supplied. The bit meanings are
// the kernel's fill_ldt(): the flag word decides granularity, default operand
// size and presence, and the descriptor is always a ring-3 data segment.
void installDescriptor(LegacyThreadAreaDescriptor& slot, U32 entry, U32 base,
                       U32 limit, U32 flags) {
    slot.entry = entry;
    slot.selector = (U16)((entry << 3) | 3);
    slot.base = base;
    slot.limit = limit & K_USER_DESC_LIMIT_MASK;
    slot.flags = flags;
    slot.granularity = (flags & K_USER_DESC_LIMIT_IN_PAGES) != 0;
    slot.defaultBig = (flags & K_USER_DESC_SEG_32BIT) != 0;
    slot.present = (flags & K_USER_DESC_SEG_NOT_PRESENT) == 0;
    slot.installed = true;
}

void clearDescriptor(LegacyThreadAreaDescriptor& slot, U32 entry) {
    slot = LegacyThreadAreaDescriptor {};
    slot.entry = entry;
    slot.selector = (U16)((entry << 3) | 3);
    slot.flags = K_USER_DESC_EMPTY_FLAGS;
}

bool descriptorIsEmptyRequest(U32 base, U32 limit, U32 flags) {
    // LDT_empty(): nothing set but the two bits that say "not a segment".
    return base == 0 && limit == 0 &&
           (flags & ~(U32)K_USER_DESC_EMPTY_FLAGS) == 0 &&
           (flags & K_USER_DESC_READ_EXEC_ONLY) != 0 &&
           (flags & K_USER_DESC_SEG_NOT_PRESENT) != 0;
}

// set_thread_area(2). The entry number selects one of the three GDT slots
// Linux reserves for thread-local storage; -1 asks the kernel to choose a free
// one and report it back through the same structure.
S32 serveSetThreadArea(CPU64* cpu, U32 descriptorAddress,
                       LegacySyscallResult& out) {
    if (!legacyAddressReadable(cpu, descriptorAddress, K_USER_DESC_SIZE)) {
        if (shouldReportServedLegacySyscall()) {
            klog_fmt("BOXEDWINE_X64_LEGACY_SYSCALL pid=%u tid=%u "
                     "nr=set_thread_area status=efault u_info=0x%x",
                     legacyProcessId(cpu), legacyThreadId(cpu),
                     descriptorAddress);
        }
        return -K_EFAULT;
    }
    KMemory64* memory = cpu->memory;
    S32 entry = (S32)memory->readd((U64)descriptorAddress + K_USER_DESC_OFF_ENTRY);
    const U32 base = memory->readd((U64)descriptorAddress + K_USER_DESC_OFF_BASE);
    const U32 limit = memory->readd((U64)descriptorAddress + K_USER_DESC_OFF_LIMIT);
    const U32 flags = memory->readd((U64)descriptorAddress + K_USER_DESC_OFF_FLAGS);
    const bool empty = descriptorIsEmptyRequest(base, limit, flags);
    S32 result = 0;

    if (entry == -1) {
        if (empty) {
            // Nothing to free and no slot to name: the kernel rejects this.
            result = -K_EINVAL;
        } else {
            entry = -1;
            for (U32 candidate = K_GUEST_TLS_ENTRY_MIN;
                 candidate <= K_GUEST_TLS_ENTRY_MAX; candidate++) {
                LegacyThreadAreaDescriptor* slot = descriptorSlot(cpu, candidate);
                if (slot && !slot->installed) {
                    entry = (S32)candidate;
                    break;
                }
            }
            if (entry == -1) {
                // Out of thread-local slots, which is ESRCH in the kernel and
                // not EINVAL: the request was well formed.
                result = -K_ESRCH;
            }
        }
    }

    if (result == 0 &&
        (entry < (S32)K_GUEST_TLS_ENTRY_MIN || entry > (S32)K_GUEST_TLS_ENTRY_MAX ||
         (U32)entry >= K_GUEST_SEGMENT_TABLE_ENTRIES)) {
        result = -K_EINVAL;
    }

    if (result == 0) {
        LegacyThreadAreaDescriptor* slot = descriptorSlot(cpu, (U32)entry);
        if (!slot) {
            result = -K_EINVAL;
        } else {
            if (empty) {
                clearDescriptor(*slot, (U32)entry);
            } else {
                installDescriptor(*slot, (U32)entry, base, limit, flags);
                out.descriptorInstalled = true;
                out.descriptor = *slot;
            }
            // The chosen entry goes back to the guest; Wine reads it to build
            // the selector it then loads into FS.
            memory->writed((U64)descriptorAddress + K_USER_DESC_OFF_ENTRY,
                           (U32)entry);
        }
    }

    if (shouldReportServedLegacySyscall()) {
        klog_fmt("BOXEDWINE_X64_LEGACY_SYSCALL pid=%u tid=%u nr=set_thread_area "
                 "entry=%d base=0x%x limit=0x%x flags=0x%x empty=%d result=%d "
                 "rip=0x%llx",
                 legacyProcessId(cpu), legacyThreadId(cpu), entry, base, limit,
                 flags, empty ? 1 : 0, result, (unsigned long long)cpu->rip);
    }
    return result;
}

// get_thread_area(2). Reads back whatever set_thread_area installed. A slot
// that was never written reads as the empty descriptor rather than as an
// error, which is what the kernel returns for an unused TLS entry.
S32 serveGetThreadArea(CPU64* cpu, U32 descriptorAddress) {
    if (!legacyAddressReadable(cpu, descriptorAddress, K_USER_DESC_SIZE)) {
        return -K_EFAULT;
    }
    KMemory64* memory = cpu->memory;
    const S32 entry =
        (S32)memory->readd((U64)descriptorAddress + K_USER_DESC_OFF_ENTRY);
    if (entry < (S32)K_GUEST_TLS_ENTRY_MIN || entry > (S32)K_GUEST_TLS_ENTRY_MAX ||
        (U32)entry >= K_GUEST_SEGMENT_TABLE_ENTRIES) {
        return -K_EINVAL;
    }
    const LegacyThreadAreaDescriptor* slot = descriptorSlot(cpu, (U32)entry);
    const U32 base = slot && slot->installed ? slot->base : 0;
    const U32 limit = slot && slot->installed ? slot->limit : 0;
    const U32 flags = slot && slot->installed ? slot->flags
                                              : (U32)K_USER_DESC_EMPTY_FLAGS;
    memory->writed((U64)descriptorAddress + K_USER_DESC_OFF_BASE, base);
    memory->writed((U64)descriptorAddress + K_USER_DESC_OFF_LIMIT, limit);
    memory->writed((U64)descriptorAddress + K_USER_DESC_OFF_FLAGS, flags);
    if (shouldReportServedLegacySyscall()) {
        klog_fmt("BOXEDWINE_X64_LEGACY_SYSCALL pid=%u tid=%u nr=get_thread_area "
                 "entry=%d base=0x%x limit=0x%x flags=0x%x result=0 rip=0x%llx",
                 legacyProcessId(cpu), legacyThreadId(cpu), entry, base, limit,
                 flags, (unsigned long long)cpu->rip);
    }
    return 0;
}

// The eight bytes the processor actually reads for a descriptor, built from a
// user_desc the way the kernel's LDT_entry_a/LDT_entry_b macros do. modify_ldt
// func 0 hands these back, so a guest that reads its own LDT sees the segment
// it installed rather than zeros.
void encodeDescriptorEntry(const LegacyThreadAreaDescriptor& slot, U32* low,
                           U32* high) {
    const U32 base = slot.base;
    const U32 limit = slot.limit;
    const U32 flags = slot.flags;
    *low = ((base & 0x0000ffffu) << 16) | (limit & 0x0000ffffu);
    *high = (base & 0xff000000u) |
            ((base & 0x00ff0000u) >> 16) |
            (limit & 0x000f0000u) |
            ((((flags & K_USER_DESC_READ_EXEC_ONLY) ? 1u : 0u) ^ 1u) << 9) |
            (((flags & K_USER_DESC_CONTENTS) >> 1) << 10) |
            ((((flags & K_USER_DESC_SEG_NOT_PRESENT) ? 1u : 0u) ^ 1u) << 15) |
            (((flags & K_USER_DESC_USEABLE) ? 1u : 0u) << 20) |
            (((flags & K_USER_DESC_SEG_32BIT) ? 1u : 0u) << 22) |
            (((flags & K_USER_DESC_LIMIT_IN_PAGES) ? 1u : 0u) << 23) |
            0x7000u; // S = 1, DPL = 3
}

// modify_ldt(2). The LDT and the GDT are one table here, as they are in the
// translator's own thread state (FEX mirrors the LDT onto the GDT array and
// indexes both by selector >> 3), so an LDT index names the same slot a GDT
// index of the same value does.
S32 serveModifyLdt(CPU64* cpu, U32 func, U32 pointer, U32 byteCount,
                   LegacySyscallResult& out) {
    if (func == K_MODIFY_LDT_READ) {
        if (byteCount == 0) {
            return 0; // nothing asked for, nothing written
        }
        if (!legacyAddressReadable(cpu, pointer, byteCount)) {
            return -K_EFAULT;
        }
        U32 written = 0;
        for (U32 entry = 0; entry < K_GUEST_SEGMENT_TABLE_ENTRIES; entry++) {
            if (written + K_LDT_ENTRY_BYTES > byteCount) {
                break;
            }
            const LegacyThreadAreaDescriptor* slot = descriptorSlot(cpu, entry);
            U32 low = 0;
            U32 high = 0;
            if (slot && slot->installed) {
                encodeDescriptorEntry(*slot, &low, &high);
            }
            cpu->memory->writed((U64)pointer + written, low);
            cpu->memory->writed((U64)pointer + written + 4, high);
            written += K_LDT_ENTRY_BYTES;
        }
        return (S32)written;
    }
    if (func != K_MODIFY_LDT_WRITE && func != K_MODIFY_LDT_WRITE_NEWMODE) {
        // read_default_ldt (func 2) and anything else: named in the log rather
        // than silently refused, the same way an unserved number is.
        if (shouldReportServedLegacySyscall()) {
            klog_fmt("BOXEDWINE_X64_LEGACY_SYSCALL pid=%u tid=%u nr=modify_ldt "
                     "func=%u status=enosys rip=0x%llx",
                     legacyProcessId(cpu), legacyThreadId(cpu), func,
                     (unsigned long long)cpu->rip);
        }
        return -K_ENOSYS;
    }
    if (byteCount != K_USER_DESC_SIZE) {
        return -K_EINVAL;
    }
    if (!legacyAddressReadable(cpu, pointer, K_USER_DESC_SIZE)) {
        return -K_EFAULT;
    }
    KMemory64* memory = cpu->memory;
    const S32 entry = (S32)memory->readd((U64)pointer + K_USER_DESC_OFF_ENTRY);
    const U32 base = memory->readd((U64)pointer + K_USER_DESC_OFF_BASE);
    const U32 limit = memory->readd((U64)pointer + K_USER_DESC_OFF_LIMIT);
    const U32 flags = memory->readd((U64)pointer + K_USER_DESC_OFF_FLAGS);
    if (entry < 0 || (U32)entry >= K_GUEST_SEGMENT_TABLE_ENTRIES) {
        return -K_EINVAL;
    }
    LegacyThreadAreaDescriptor* slot = descriptorSlot(cpu, (U32)entry);
    if (!slot) {
        return -K_EINVAL;
    }
    if (descriptorIsEmptyRequest(base, limit, flags)) {
        clearDescriptor(*slot, (U32)entry);
    } else {
        installDescriptor(*slot, (U32)entry, base, limit, flags);
        out.descriptorInstalled = true;
        out.descriptor = *slot;
    }
    if (shouldReportServedLegacySyscall()) {
        klog_fmt("BOXEDWINE_X64_LEGACY_SYSCALL pid=%u tid=%u nr=modify_ldt "
                 "func=%u entry=%d base=0x%x limit=0x%x flags=0x%x result=0 "
                 "rip=0x%llx",
                 legacyProcessId(cpu), legacyThreadId(cpu), func, entry, base,
                 limit, flags, (unsigned long long)cpu->rip);
    }
    return 0;
}

} // namespace

LegacySyscallResult kernelLegacySyscall64(CPU64* cpu) {
    LegacySyscallResult out;
    if (!cpu || !cpu->memory) {
        out.result = -K_ENOSYS;
        return out;
    }
    out.number = legacyArgument(cpu->reg[X64_RAX]);
    const U32 a1 = legacyArgument(cpu->reg[X64_RBX]);
    const U32 a2 = legacyArgument(cpu->reg[X64_RCX]);
    const U32 a3 = legacyArgument(cpu->reg[X64_RDX]);

    switch (out.number) {
        case K_LEGACY_I386_SYS_set_thread_area:
            out.result = serveSetThreadArea(cpu, a1, out);
            break;
        case K_LEGACY_I386_SYS_get_thread_area:
            out.result = serveGetThreadArea(cpu, a1);
            break;
        case K_LEGACY_I386_SYS_modify_ldt:
            out.result = serveModifyLdt(cpu, a1, a2, a3, out);
            break;
        default:
            reportUnservedLegacySyscall(cpu, out.number);
            out.result = -K_ENOSYS;
            break;
    }

    // The compat entry stores the i386 answer in rax sign-extended, so a
    // negative errno is a negative 64-bit value to the caller's `cmp`.
    cpu->reg[X64_RAX].u64 = (U64)(S64)out.result;
    return out;
}

U64 kernelLegacySegmentBase64(CPU64* cpu, U16 selector) {
    if (!cpu || (selector & ~(U16)3) == 0) {
        return 0; // the null selector, whatever its requested privilege level
    }
    const boxedvn::GuestSegmentSelector decoded =
        boxedvn::decodeGuestSegmentSelector(selector);
    if (decoded.index >= K_GUEST_SEGMENT_TABLE_ENTRIES) {
        return 0;
    }
    const LegacyThreadAreaDescriptor& slot =
        cpu->threadAreaDescriptors[decoded.index];
    if (!slot.installed) {
        // Every descriptor this port publishes that was not installed through
        // the gate is flat, so its base is zero.
        return 0;
    }
    return (U64)slot.base;
}

} // namespace boxedvn

#endif // BOXEDWINE_GUEST_X64
