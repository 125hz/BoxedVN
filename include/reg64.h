/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef __REG64_H__
#define __REG64_H__

// cpu.h (32-bit guest path) defines preprocessor macros named u8/h8/u16/
// h16 that expand to byte[0]/byte[1]/word[0]/word[1]. If a TU includes
// cpu.h before reg64.h those macros corrupt the fields below. Defensively
// undo them here; the 32-bit Reg union doesn't need them in this file's
// scope.
#undef u8
#undef h8
#undef u16
#undef h16

// 64-bit guest register slot. Used by CPU64 (the x86-64 interpreter) for
// the 16 general-purpose registers RAX..R15.
//
// The 32-bit guest's register slot (struct Reg in cpu.h) is kept
// independent at sizeof==4 so that the long-tuned 32-bit JIT keeps its
// signed 8-bit displacement encoding of CPU-struct offsets — see the
// static_asserts in source/emulation/cpu/jit/jitCodeGen.cpp.
//
// On a little-endian host (every host Boxedwine targets), u64/u32/u16/u8
// all alias the low bytes of the same storage, matching x86's
// RAX/EAX/AX/AL aliasing. In x86-64, writes to the 32-bit name (.u32)
// must zero-extend the upper 32 bits; callers that simulate a 32-bit
// destination on a 64-bit register MUST write through Reg64::setU32(),
// not bare .u32 = value, because the bare write leaves the upper 32 bits
// untouched.
struct Reg64 {
    union {
        U64 u64;
        U32 u32;
        union {
            U16 u16;
            struct {
                U8 u8;
                U8 h8;
            };
        };
    };

    inline void setU64(U64 v) { u64 = v; }
    inline void setU32(U32 v) { u64 = v; }      // zero-extends, per x86-64
    inline void setU16(U16 v) { u16 = v; }      // preserves upper 48 bits
    inline void setU8(U8 v)   { u8 = v; }       // preserves upper 56 bits
    inline void setH8(U8 v)   { h8 = v; }       // AH/CH/DH/BH style — only valid for RAX..RBX without REX
};

#endif
