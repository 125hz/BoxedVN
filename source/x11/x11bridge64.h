/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifndef __X11_BRIDGE64_H__
#define __X11_BRIDGE64_H__

#ifdef BOXEDWINE_GUEST_X64

class CPU64;

// The x86-64 guest X11 bridge. Entered from the 64-bit syscall dispatcher
// for BOXEDWINE_X64_HOSTCALL_X11_BRIDGE with RDI=op, RSI=guest argument
// array, RDX=argument count. Returns the value to place in RAX.
//
// Every operation runs against the same XServer, XWindow and DisplayData
// objects the 32-bit int 0x9b path uses; only the guest-facing marshalling
// differs. See include/boxedwine_x64_x11_bridge.h for the ABI.
U64 x11Bridge64(CPU64* cpu, U64 op, U64 argsAddress, U64 count);

#endif // BOXEDWINE_GUEST_X64
#endif
