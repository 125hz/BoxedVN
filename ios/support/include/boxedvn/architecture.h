/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
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

#ifndef BOXEDVN_ARCHITECTURE_H
#define BOXEDVN_ARCHITECTURE_H

#include <string>
#include <vector>

namespace boxedvn {

// The architecture of the *guest* Windows executable.
//
// This is deliberately independent of the host architecture.  Boxedwine's
// "ARM64 build" and "64-bit build" describe the machine BoxedVN runs on; the
// Windows programs it executes are 16-bit or 32-bit x86.
enum class GuestArchitecture {
    Unknown = 0,
    X86_16,   // real-mode / Win16 segmented x86
    X86_32,   // IA-32
    X86_64,   // AMD64
};

const char* toString(GuestArchitecture arch);

// The container format of a candidate executable.  Used to give the user an
// accurate reason when something cannot run, rather than "failed to launch".
enum class ExecutableFormat {
    Unknown = 0,
    NotAnExecutable,  // no MZ signature at all
    DosMz,            // plain DOS executable, no extended header
    NeWin16,          // 16-bit Windows (New Executable)
    LeVxd,            // LE/LX - VxD drivers, OS/2
    Pe32,             // PE with optional header magic 0x10B
    Pe32Plus,         // PE with optional header magic 0x20B
};

const char* toString(ExecutableFormat format);

// Identifies which emulation backend would be responsible for an executable.
//
// Both IDs describe execution owned by BoxedWine.  The FEX identifier is an
// optional CPU/backend path for a 64-bit BoxedWine process; it must never mean
// the separate native-Wine application from the historical reference branch.
enum class RuntimeBackendID {
    None = 0,
    BoxedwineX86,  // Boxedwine: emulated Linux kernel + 32-bit Wine + x86 CPU
    BoxedwineFEX64, // Boxedwine kernel + x86-64 Linux ABI + optional FEX CPU
};

const char* toString(RuntimeBackendID backend);

// Describes what a backend can actually do.  Populated from real build
// configuration, never hard-coded optimistically.
struct RuntimeBackendCapabilities {
    RuntimeBackendID id = RuntimeBackendID::None;

    // False for any backend that has not been written yet.
    bool implemented = false;

    // Guest architectures the backend can execute.
    std::vector<GuestArchitecture> executableArchitectures;

    // True when the backend cannot run at a usable speed without host JIT.
    bool requiresJIT = false;

    // True when an interpreter fallback exists and is deliberately supported.
    bool hasInterpreterFallback = false;

    // Bring-up is intentionally represented component-by-component.  An x64
    // executable is selectable only once every required layer is linked; a
    // translator-only build must not advertise that it can run Wine64.
    bool translatorAvailable = false;
    bool guestKernelAvailable = false;
    bool wineAvailable = false;
    bool dxmtAvailable = false;

    // Renderer identifiers the backend can present through, most preferred
    // first.  See renderer.h.
    std::vector<std::string> rendererIDs;

    bool canExecute(GuestArchitecture arch) const;
};

// Capabilities of the Boxedwine x86 backend as compiled into *this* binary.
// Reflects the compile-time configuration, so a build without the ARM64 JIT
// reports requiresJIT/hasInterpreterFallback honestly.
RuntimeBackendCapabilities boxedwineX86Capabilities();

// Optional FEX backend behind BoxedWine's 64-bit guest ABI.  During bring-up
// this reports the individual compiled components while implemented remains
// false until the full runtime is present.
RuntimeBackendCapabilities boxedwineFEX64Capabilities();

std::vector<RuntimeBackendCapabilities> allBackends();

// Selects the backend that should run an executable of the given architecture.
// Returns RuntimeBackendID::None when no *implemented* backend can run it.
RuntimeBackendID selectBackend(GuestArchitecture arch);

// A user-facing explanation of why an architecture cannot be run.  Empty when
// it can be run.
std::string unsupportedArchitectureMessage(GuestArchitecture arch);

}  // namespace boxedvn

#endif  // BOXEDVN_ARCHITECTURE_H
