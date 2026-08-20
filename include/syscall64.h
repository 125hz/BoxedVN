/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef __SYSCALL64_H__
#define __SYSCALL64_H__

#ifdef BOXEDWINE_GUEST_X64

class CPU64;

// x86-64 syscall dispatch. Called from CPU64::step() when it encounters a
// SYSCALL instruction (0F 05). The syscall number is in RAX; arguments
// are in RDI, RSI, RDX, R10, R8, R9; the result goes back in RAX.
//
// This is NOT a thin wrapper around ksyscall(): the x86-64 syscall table
// is renumbered relative to i386, and several syscalls (mmap, brk, every
// pointer-bearing struct) need 64-bit handling that the existing i386
// handlers can't provide. Phase 3 v1 implements only the handful needed
// to make ld-linux-x86-64.so.2 reach the program entry point; everything
// else returns -ENOSYS with a klog.
void ksyscall64(CPU64* cpu);

#endif // BOXEDWINE_GUEST_X64
#endif
