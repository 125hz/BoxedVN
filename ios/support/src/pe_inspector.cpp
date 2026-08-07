/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 *
 *  A deliberately small, bounds-checked reader for the handful of header
 *  fields BoxedVN needs.  It never allocates based on file contents and never
 *  seeks past the buffer it was given, so a hostile or truncated file produces
 *  a diagnostic rather than a crash.
 *
 *  Field offsets follow the Microsoft PE/COFF specification, revision 11.
 */

#include "boxedvn/pe_inspector.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace boxedvn {
namespace {

constexpr uint16_t kSignatureMZ = 0x5A4D;  // 'MZ'
constexpr uint16_t kSignatureNE = 0x454E;  // 'NE'
constexpr uint16_t kSignatureLE = 0x454C;  // 'LE'
constexpr uint16_t kSignatureLX = 0x584C;  // 'LX'
constexpr uint32_t kSignaturePE = 0x00004550;  // 'PE\0\0'

constexpr uint16_t kMagicPE32 = 0x010B;
constexpr uint16_t kMagicPE32Plus = 0x020B;
constexpr uint16_t kMagicROM = 0x0107;

constexpr uint16_t kMachineUnknown = 0x0000;
constexpr uint16_t kMachineI386 = 0x014C;
constexpr uint16_t kMachineAMD64 = 0x8664;
constexpr uint16_t kMachineIA64 = 0x0200;
constexpr uint16_t kMachineARM = 0x01C0;
constexpr uint16_t kMachineARMNT = 0x01C4;
constexpr uint16_t kMachineARM64 = 0xAA64;
constexpr uint16_t kMachineTHUMB = 0x01C2;

// Offset of e_lfanew inside the DOS header.
constexpr size_t kOffsetLfanew = 0x3C;

// The COFF header is 20 bytes and starts 4 bytes after the PE signature.
constexpr size_t kCoffHeaderSize = 20;

// Data directory index of the CLR runtime header (COM+ descriptor).
constexpr size_t kDirectoryEntryComDescriptor = 14;

bool readU16(const uint8_t* data, size_t size, size_t offset, uint16_t* out) {
    if (offset + 2 > size) {
        return false;
    }
    *out = static_cast<uint16_t>(data[offset]) |
           static_cast<uint16_t>(data[offset + 1]) << 8;
    return true;
}

bool readU32(const uint8_t* data, size_t size, size_t offset, uint32_t* out) {
    if (offset + 4 > size) {
        return false;
    }
    *out = static_cast<uint32_t>(data[offset]) |
           static_cast<uint32_t>(data[offset + 1]) << 8 |
           static_cast<uint32_t>(data[offset + 2]) << 16 |
           static_cast<uint32_t>(data[offset + 3]) << 24;
    return true;
}

const char* machineName(uint16_t machine) {
    switch (machine) {
        case kMachineUnknown: return "IMAGE_FILE_MACHINE_UNKNOWN";
        case kMachineI386:    return "IMAGE_FILE_MACHINE_I386";
        case kMachineAMD64:   return "IMAGE_FILE_MACHINE_AMD64";
        case kMachineIA64:    return "IMAGE_FILE_MACHINE_IA64";
        case kMachineARM:     return "IMAGE_FILE_MACHINE_ARM";
        case kMachineARMNT:   return "IMAGE_FILE_MACHINE_ARMNT";
        case kMachineARM64:   return "IMAGE_FILE_MACHINE_ARM64";
        case kMachineTHUMB:   return "IMAGE_FILE_MACHINE_THUMB";
        default:              return "an unrecognised machine type";
    }
}

std::string hex16(uint16_t value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "0x%04X", value);
    return buffer;
}

// Fills in `runnable`, `backend` and the final sentence of `diagnostic`.
void resolveBackend(ExecutableInfo& info) {
    info.backend = selectBackend(info.architecture);
    info.runnable = info.backend != RuntimeBackendID::None;
    if (!info.runnable) {
        const std::string why = unsupportedArchitectureMessage(info.architecture);
        if (!why.empty()) {
            info.diagnostic += " " + why;
        }
    }
}

ExecutableInfo failure(ExecutableFormat format, std::string diagnostic) {
    ExecutableInfo info;
    info.format = format;
    info.architecture = GuestArchitecture::Unknown;
    info.runnable = false;
    info.backend = RuntimeBackendID::None;
    info.diagnostic = std::move(diagnostic);
    return info;
}

}  // namespace

ExecutableInfo inspectExecutable(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return failure(ExecutableFormat::NotAnExecutable,
                       "The file is empty.");
    }

    uint16_t mz = 0;
    if (!readU16(data, size, 0, &mz)) {
        return failure(ExecutableFormat::NotAnExecutable,
                       "The file is shorter than a DOS header (2 bytes read "
                       "from a " + std::to_string(size) + " byte file).");
    }
    if (mz != kSignatureMZ) {
        return failure(ExecutableFormat::NotAnExecutable,
                       "The file does not begin with the 'MZ' signature, so it "
                       "is not a Windows or DOS executable.");
    }

    uint32_t lfanew = 0;
    if (!readU32(data, size, kOffsetLfanew, &lfanew)) {
        return failure(ExecutableFormat::DosMz,
                       "The DOS header is truncated: e_lfanew at offset 0x3C "
                       "lies past the end of the file.");
    }

    // A plain DOS executable either has no extended header or points at zero.
    if (lfanew == 0) {
        ExecutableInfo info;
        info.format = ExecutableFormat::DosMz;
        info.architecture = GuestArchitecture::X86_16;
        info.diagnostic =
            "Plain DOS (MZ) executable with no extended header.";
        resolveBackend(info);
        return info;
    }

    // Guard against a nonsensical e_lfanew before using it as an offset.
    if (lfanew >= size) {
        return failure(ExecutableFormat::DosMz,
                       "e_lfanew points to offset " + std::to_string(lfanew) +
                       ", past the end of the " + std::to_string(size) +
                       " byte file. The header is malformed or the file is "
                       "truncated.");
    }

    uint16_t extendedSignature = 0;
    if (!readU16(data, size, lfanew, &extendedSignature)) {
        return failure(ExecutableFormat::DosMz,
                       "The extended header signature at offset " +
                       std::to_string(lfanew) + " is truncated.");
    }

    if (extendedSignature == kSignatureNE) {
        ExecutableInfo info;
        info.format = ExecutableFormat::NeWin16;
        info.architecture = GuestArchitecture::X86_16;
        info.diagnostic = "16-bit Windows (NE) executable.";
        resolveBackend(info);
        return info;
    }

    if (extendedSignature == kSignatureLE || extendedSignature == kSignatureLX) {
        ExecutableInfo info;
        info.format = ExecutableFormat::LeVxd;
        info.architecture = GuestArchitecture::Unknown;
        info.diagnostic =
            "Linear Executable (LE/LX). These are VxD drivers or OS/2 "
            "programs, not Windows applications.";
        resolveBackend(info);
        return info;
    }

    uint32_t pe = 0;
    if (!readU32(data, size, lfanew, &pe)) {
        return failure(ExecutableFormat::DosMz,
                       "The PE signature at offset " + std::to_string(lfanew) +
                       " is truncated.");
    }
    if (pe != kSignaturePE) {
        return failure(ExecutableFormat::Unknown,
                       "The extended header at offset " + std::to_string(lfanew) +
                       " has signature " + hex16(extendedSignature) +
                       ", which is neither 'PE', 'NE' nor 'LE'.");
    }

    const size_t coffOffset = static_cast<size_t>(lfanew) + 4;
    uint16_t machine = 0;
    if (!readU16(data, size, coffOffset, &machine)) {
        return failure(ExecutableFormat::Unknown,
                       "The COFF header is truncated: the file ends before the "
                       "machine field.");
    }

    uint16_t sizeOfOptionalHeader = 0;
    if (!readU16(data, size, coffOffset + 16, &sizeOfOptionalHeader)) {
        return failure(ExecutableFormat::Unknown,
                       "The COFF header is truncated: the file ends before "
                       "SizeOfOptionalHeader.");
    }

    if (sizeOfOptionalHeader == 0) {
        return failure(ExecutableFormat::Unknown,
                       "This is a PE object file, not an executable image "
                       "(SizeOfOptionalHeader is 0).");
    }

    const size_t optionalOffset = coffOffset + kCoffHeaderSize;
    uint16_t magic = 0;
    if (!readU16(data, size, optionalOffset, &magic)) {
        return failure(ExecutableFormat::Unknown,
                       "The optional header is truncated: the file ends before "
                       "its magic field.");
    }

    ExecutableInfo info;
    info.coffMachine = machine;
    info.optionalHeaderMagic = magic;

    switch (magic) {
        case kMagicPE32:
            info.format = ExecutableFormat::Pe32;
            break;
        case kMagicPE32Plus:
            info.format = ExecutableFormat::Pe32Plus;
            break;
        case kMagicROM:
            return failure(ExecutableFormat::Unknown,
                           "The optional header magic is 0x0107 (ROM image), "
                           "which is not a Windows application.");
        default:
            return failure(ExecutableFormat::Unknown,
                           "The optional header magic is " + hex16(magic) +
                           "; expected 0x010B (PE32) or 0x020B (PE32+).");
    }

    // The architecture comes from the COFF machine field.  The optional header
    // magic is cross-checked because the two must agree in a well-formed image.
    switch (machine) {
        case kMachineI386:
            info.architecture = GuestArchitecture::X86_32;
            break;
        case kMachineAMD64:
        case kMachineIA64:
            info.architecture = GuestArchitecture::X86_64;
            break;
        default:
            info.architecture = GuestArchitecture::Unknown;
            break;
    }

    const bool magicSaysPlus = (magic == kMagicPE32Plus);
    const bool machineSaysPlus = (info.architecture == GuestArchitecture::X86_64);

    if (info.architecture == GuestArchitecture::Unknown) {
        info.diagnostic = std::string("This is a ") + toString(info.format) +
                          " image whose COFF machine field is " + hex16(machine) +
                          " (" + machineName(machine) + "). BoxedVN only runs "
                          "x86 Windows programs.";
        resolveBackend(info);
        return info;
    }

    if (magicSaysPlus != machineSaysPlus) {
        return failure(ExecutableFormat::Unknown,
                       "The headers contradict each other: optional header "
                       "magic " + hex16(magic) + " does not match COFF machine " +
                       hex16(machine) + " (" + machineName(machine) +
                       "). The file is malformed.");
    }

    // Subsystem sits 68 bytes into the optional header in both layouts: PE32's
    // extra 4-byte BaseOfData field is exactly offset by PE32+'s 8-byte
    // ImageBase, so the two headers realign before Subsystem and diverge again
    // afterwards at SizeOfStackReserve.
    readU16(data, size, optionalOffset + 68, &info.subsystem);

    // NumberOfRvaAndSizes and the data directories, used only to report whether
    // the image is managed .NET.  A truncated tail is not an error here.
    const size_t numberOfRvaOffset = optionalOffset + (magic == kMagicPE32 ? 92 : 108);
    uint32_t numberOfRvaAndSizes = 0;
    if (readU32(data, size, numberOfRvaOffset, &numberOfRvaAndSizes) &&
        numberOfRvaAndSizes > kDirectoryEntryComDescriptor) {
        const size_t comDirOffset =
            numberOfRvaOffset + 4 + kDirectoryEntryComDescriptor * 8;
        uint32_t comRva = 0;
        uint32_t comSize = 0;
        if (readU32(data, size, comDirOffset, &comRva) &&
            readU32(data, size, comDirOffset + 4, &comSize)) {
            info.managedDotNet = (comRva != 0 && comSize != 0);
        }
    }

    if (info.architecture == GuestArchitecture::X86_32) {
        info.diagnostic = "32-bit x86 Windows executable (PE32, " +
                          std::string(machineName(machine)) + ").";
    } else {
        info.diagnostic = "64-bit Windows executable (PE32+, " +
                          std::string(machineName(machine)) + ").";
    }
    if (info.managedDotNet) {
        info.diagnostic += " The image declares a .NET runtime header.";
    }

    resolveBackend(info);
    return info;
}

ExecutableInfo inspectExecutableFile(const std::string& path,
                                     size_t maxHeaderBytes) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return failure(ExecutableFormat::Unknown,
                       "Could not open '" + path + "': " +
                       std::strerror(errno) + ".");
    }

    std::vector<uint8_t> buffer(maxHeaderBytes);
    const size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
    const bool readError = (read == 0) && (std::ferror(file) != 0);
    std::fclose(file);

    if (readError) {
        return failure(ExecutableFormat::Unknown,
                       "Could not read '" + path + "'.");
    }

    buffer.resize(read);
    return inspectExecutable(buffer.data(), buffer.size());
}

}  // namespace boxedvn
