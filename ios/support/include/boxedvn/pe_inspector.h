/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 */

#ifndef BOXEDVN_PE_INSPECTOR_H
#define BOXEDVN_PE_INSPECTOR_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "boxedvn/architecture.h"

namespace boxedvn {

// Result of inspecting a candidate Windows executable.
//
// Every failure path sets `diagnostic` to a specific, quotable reason.  There
// is no generic "unsupported file" outcome.
struct ExecutableInfo {
    ExecutableFormat format = ExecutableFormat::Unknown;
    GuestArchitecture architecture = GuestArchitecture::Unknown;

    // Raw COFF machine value (PE only), e.g. 0x014c for I386.  Zero otherwise.
    uint16_t coffMachine = 0;

    // Raw optional-header magic (PE only): 0x10B = PE32, 0x20B = PE32+.
    uint16_t optionalHeaderMagic = 0;

    // Windows subsystem field (PE only): 2 = GUI, 3 = console.  Zero otherwise.
    uint16_t subsystem = 0;

    // True when the PE declares a COM+/.NET descriptor directory.  Purely
    // informational: Boxedwine can run some .NET, but not reliably.
    bool managedDotNet = false;

    // True when an implemented backend can execute this file.
    bool runnable = false;

    // Backend that would run it, or RuntimeBackendID::None.
    RuntimeBackendID backend = RuntimeBackendID::None;

    // Human-readable explanation.  Always populated.
    std::string diagnostic;
};

// Inspects an in-memory image.  `size` may be smaller than the real file; the
// parser only needs the headers and is bounds-checked at every step, so a
// truncated read yields a precise "truncated" diagnostic rather than a crash.
ExecutableInfo inspectExecutable(const uint8_t* data, size_t size);

// Reads at most `maxHeaderBytes` from `path` and inspects it.  A read failure
// produces ExecutableFormat::Unknown with the errno reason in `diagnostic`.
ExecutableInfo inspectExecutableFile(const std::string& path,
                                     size_t maxHeaderBytes = 64 * 1024);

}  // namespace boxedvn

#endif  // BOXEDVN_PE_INSPECTOR_H
