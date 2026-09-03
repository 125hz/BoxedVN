/*
 * BoxedWine - the legacy i386 `int 0x80` gate a 64-bit guest still uses.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * A 64-bit process is not supposed to need the i386 syscall gate, and almost
 * none of one ever does. Wine's 64-bit unix ntdll is the exception: the 32-bit
 * FS and GS selectors a WoW64 thread runs on can only be allocated through
 * set_thread_area, which has no x86-64 syscall number at all, so
 * dlls/ntdll/unix/signal_x86_64.c reaches it with `int $0x80` and the i386
 * number in eax. That code path runs in EVERY WoW64 process, which is every
 * process that starts a 32-bit program.
 *
 * Both 64-bit backends meet the instruction, and they must answer it
 * identically: the launched process runs on the translator, which raises a
 * general protection fault the FEX adapter serves, and every process forked
 * from it runs on the CPU64 interpreter, which decodes the opcode itself. The
 * decision -- which numbers are served, which descriptor slots are legal, what
 * the descriptor bits mean, and what eax carries back -- lives here so the two
 * cannot drift apart.
 *
 * The descriptor bookkeeping is the part that has to be shared rather than
 * duplicated. A descriptor installed here is what a later `mov fs, r/m16`
 * resolves its base from; if one backend stored the descriptor somewhere the
 * other could not read, a thread that installed its TLS on one and then loaded
 * FS on the other would address its TEB through a zero base.
 */

#ifndef __LEGACYSYSCALL64_H__
#define __LEGACYSYSCALL64_H__

#ifdef BOXEDWINE_GUEST_X64

#include "guest_segment_table.h"

class CPU64;

// The i386 syscall numbers this gate serves. These are the i386 table's
// numbers (arch/x86/entry/syscalls/syscall_32.tbl), NOT the x86-64 ones a
// SYSCALL instruction carries; 243 is set_thread_area on i386 and
// io_setup on x86-64, which is exactly the confusion the gate exists to avoid.
#define K_LEGACY_I386_SYS_modify_ldt       123
#define K_LEGACY_I386_SYS_set_thread_area  243
#define K_LEGACY_I386_SYS_get_thread_area  244

// GDT_ENTRY_TLS_MIN..GDT_ENTRY_TLS_MAX: the three descriptor slots Linux hands
// to user space, and therefore the only entry numbers set_thread_area accepts.
#define K_GUEST_TLS_ENTRY_MIN 12
#define K_GUEST_TLS_ENTRY_MAX 14

// `int 0x80` is two bytes (CD 80). The gate resumes after them.
#define K_LEGACY_SYSCALL_INSTRUCTION_LENGTH 2

#if defined(__cplusplus)

namespace boxedvn {

// One installed descriptor, in the terms the x86 descriptor uses rather than
// the terms `struct user_desc` uses, so a backend that has to publish it into
// a translator's own descriptor table does not repeat the decode.
struct LegacyThreadAreaDescriptor {
    U32  entry = 0;        // index into the descriptor table
    U16  selector = 0;     // (entry << 3) | 3 -- ring 3, GDT
    U32  base = 0;
    U32  limit = 0;        // 20 bits, as the descriptor field is
    U32  flags = 0;        // the user_desc flag word, kept verbatim
    bool granularity = false;  // G  <- limit_in_pages
    bool defaultBig = false;   // D  <- seg_32bit
    bool present = false;      // P  <- !seg_not_present
    bool installed = false;    // false: this slot was never written
};

// What the gate did, for a caller that has more to do than store eax.
struct LegacySyscallResult {
    U32  number = 0;       // the i386 syscall number that was in eax
    S32  result = 0;       // the i386 ABI answer: 0 or a negative errno
    U32  instructionLength = K_LEGACY_SYSCALL_INSTRUCTION_LENGTH;
    bool descriptorInstalled = false;
    LegacyThreadAreaDescriptor descriptor;
};

// Serve one `int 0x80`. The caller has already established that the two bytes
// at rip are CD 80; this reads the number from eax and the arguments from
// ebx/ecx/edx/esi/edi/ebp, performs the call, and writes the i386 result back
// into eax the way the kernel's compat entry does (sign-extended into rax).
//
// It does NOT advance rip: the interpreter counts instruction bytes for its own
// dispatch and the adapter resumes the translator, so each advances by
// `instructionLength` itself.
//
// A number this gate does not serve returns -ENOSYS in eax and is reported once
// per number, so a guest looping on one cannot fill a device's disk.
LegacySyscallResult kernelLegacySyscall64(CPU64* cpu);

// The base a segment selector names, resolved through the descriptors above.
// This is what a `mov fs, r/m16` / `pop fs` has to install as the segment base:
// a Linux process that reloads FS with an LDT/GDT selector gets the base out of
// the descriptor the selector indexes, and Wine's WoW64 layer relies on exactly
// that between set_thread_area and the far jump into 32-bit code.
//
// A null selector, or one naming a slot no descriptor was installed in, is a
// zero base -- the same answer the flat long-mode table gives.
U64 kernelLegacySegmentBase64(CPU64* cpu, U16 selector);

} // namespace boxedvn

#endif // __cplusplus

#endif // BOXEDWINE_GUEST_X64
#endif
