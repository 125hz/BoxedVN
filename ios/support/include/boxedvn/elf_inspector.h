/*
 * BoxedVN - host-independent Linux ELF image inspection.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BOXEDVN_ELF_INSPECTOR_H
#define BOXEDVN_ELF_INSPECTOR_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace boxedvn {

enum class ELFGuestArchitecture {
    Unknown,
    X86,
    X86_64,
    Unsupported,
};

struct ELFLoadSegment {
    uint64_t fileOffset = 0;
    uint64_t virtualAddress = 0;
    uint64_t fileSize = 0;
    uint64_t memorySize = 0;
    uint64_t alignment = 0;
    bool readable = false;
    bool writable = false;
    bool executable = false;
};

struct ELFImageInfo {
    bool valid = false;
    bool positionIndependent = false;
    ELFGuestArchitecture architecture = ELFGuestArchitecture::Unknown;
    uint64_t entry = 0;
    std::string interpreter;
    std::vector<ELFLoadSegment> loadSegments;
    std::string error;
};

// Parses both Linux ELF32/i386 and ELF64/x86-64 without using host ELF
// headers. It performs bounds and integer-overflow checks before exposing any
// loader metadata. Execution remains gated separately by backend readiness.
ELFImageInfo inspectELF(const uint8_t* bytes, size_t size);

} // namespace boxedvn

#endif
