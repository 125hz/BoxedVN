/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef __ELF_LOADER_POLICY64_H__
#define __ELF_LOADER_POLICY64_H__

// A PT_INTERP dynamic linker must finish rebasing and relocating the main
// image before its PT_GNU_RELRO pages become read-only. BoxedWine may finalize
// RELRO only for images whose relocation lifecycle it owns itself.
constexpr bool kernelOwnsElf64Relro(bool hasProgramInterpreter) {
    return !hasProgramInterpreter;
}

#endif
