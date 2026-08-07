/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 */

#include "boxedvn/architecture.h"

#include <algorithm>

namespace boxedvn {

const char* toString(GuestArchitecture arch) {
    switch (arch) {
        case GuestArchitecture::Unknown: return "unknown";
        case GuestArchitecture::X86_16:  return "x86_16";
        case GuestArchitecture::X86_32:  return "x86_32";
        case GuestArchitecture::X86_64:  return "x86_64";
    }
    return "unknown";
}

const char* toString(ExecutableFormat format) {
    switch (format) {
        case ExecutableFormat::Unknown:         return "unknown";
        case ExecutableFormat::NotAnExecutable: return "not-an-executable";
        case ExecutableFormat::DosMz:           return "dos-mz";
        case ExecutableFormat::NeWin16:         return "ne-win16";
        case ExecutableFormat::LeVxd:           return "le-vxd";
        case ExecutableFormat::Pe32:            return "pe32";
        case ExecutableFormat::Pe32Plus:        return "pe32+";
    }
    return "unknown";
}

const char* toString(RuntimeBackendID backend) {
    switch (backend) {
        case RuntimeBackendID::None:         return "none";
        case RuntimeBackendID::BoxedwineX86: return "boxedwine-x86";
        case RuntimeBackendID::FutureX64:    return "future-x64";
    }
    return "none";
}

bool RuntimeBackendCapabilities::canExecute(GuestArchitecture arch) const {
    if (!implemented) {
        return false;
    }
    return std::find(executableArchitectures.begin(),
                     executableArchitectures.end(),
                     arch) != executableArchitectures.end();
}

RuntimeBackendCapabilities boxedwineX86Capabilities() {
    RuntimeBackendCapabilities caps;
    caps.id = RuntimeBackendID::BoxedwineX86;
    caps.implemented = true;

    // Boxedwine runs a 32-bit Wine under an emulated Linux kernel.  Win16
    // programs are handled by that Wine, not by a separate backend.
    caps.executableArchitectures = {
        GuestArchitecture::X86_16,
        GuestArchitecture::X86_32,
    };

#if defined(BOXEDWINE_JIT)
    caps.requiresJIT = true;
#else
    caps.requiresJIT = false;
#endif

    // Boxedwine always contains its "normal" (interpreting) CPU core, and falls
    // back to it when a block cannot be compiled.  On iOS without external JIT
    // activation the process cannot map executable pages at all, so the JIT
    // build cannot silently degrade to it -- see docs/KNOWN_LIMITATIONS_IOS.md.
    caps.hasInterpreterFallback = true;

    caps.rendererIDs = {"sdl-framebuffer"};
    return caps;
}

RuntimeBackendCapabilities futureX64Capabilities() {
    RuntimeBackendCapabilities caps;
    caps.id = RuntimeBackendID::FutureX64;
    caps.implemented = false;
    caps.executableArchitectures = {};
    caps.requiresJIT = true;
    caps.hasInterpreterFallback = false;
    caps.rendererIDs = {};
    return caps;
}

std::vector<RuntimeBackendCapabilities> allBackends() {
    return {boxedwineX86Capabilities(), futureX64Capabilities()};
}

RuntimeBackendID selectBackend(GuestArchitecture arch) {
    for (const RuntimeBackendCapabilities& caps : allBackends()) {
        if (caps.canExecute(arch)) {
            return caps.id;
        }
    }
    return RuntimeBackendID::None;
}

std::string unsupportedArchitectureMessage(GuestArchitecture arch) {
    if (selectBackend(arch) != RuntimeBackendID::None) {
        return std::string();
    }
    switch (arch) {
        case GuestArchitecture::X86_64:
            return "x64 is not supported by the current runtime. BoxedVN runs "
                   "16-bit and 32-bit Windows programs through Boxedwine's "
                   "32-bit Wine; a 64-bit backend has not been implemented.";
        case GuestArchitecture::Unknown:
            return "The architecture of this file could not be determined, so "
                   "it cannot be run.";
        default:
            return std::string("No implemented runtime backend can execute ") +
                   toString(arch) + " executables.";
    }
}

}  // namespace boxedvn
