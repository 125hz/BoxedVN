/*
 * BoxedVN - host-independent Linux ELF image inspection.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#include "boxedvn/elf_inspector.h"

#include <limits>

namespace boxedvn {

namespace {

constexpr uint8_t kELFClass32 = 1;
constexpr uint8_t kELFClass64 = 2;
constexpr uint8_t kELFLittleEndian = 1;
constexpr uint16_t kELFMachine386 = 3;
constexpr uint16_t kELFMachineX86_64 = 62;
constexpr uint16_t kELFTypeExecutable = 2;
constexpr uint16_t kELFTypeShared = 3;
constexpr uint32_t kProgramLoad = 1;
constexpr uint32_t kProgramInterpreter = 3;
constexpr uint32_t kFlagExecute = 1;
constexpr uint32_t kFlagWrite = 2;
constexpr uint32_t kFlagRead = 4;

bool rangeFits(uint64_t offset, uint64_t length, size_t size) {
    return offset <= size && length <= static_cast<uint64_t>(size) - offset;
}

uint16_t read16(const uint8_t* value) {
    return static_cast<uint16_t>(value[0]) |
           static_cast<uint16_t>(value[1]) << 8;
}

uint32_t read32(const uint8_t* value) {
    return static_cast<uint32_t>(value[0]) |
           static_cast<uint32_t>(value[1]) << 8 |
           static_cast<uint32_t>(value[2]) << 16 |
           static_cast<uint32_t>(value[3]) << 24;
}

uint64_t read64(const uint8_t* value) {
    return static_cast<uint64_t>(read32(value)) |
           static_cast<uint64_t>(read32(value + 4)) << 32;
}

ELFImageInfo fail(const char* error) {
    ELFImageInfo result;
    result.error = error;
    return result;
}

} // namespace

ELFImageInfo inspectELF(const uint8_t* bytes, size_t size) {
    if (bytes == nullptr || size < 16) {
        return fail("ELF identification is truncated");
    }
    if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' ||
        bytes[3] != 'F') {
        return fail("ELF magic is missing");
    }
    const uint8_t elfClass = bytes[4];
    if (elfClass != kELFClass32 && elfClass != kELFClass64) {
        return fail("ELF class is not 32-bit or 64-bit");
    }
    if (bytes[5] != kELFLittleEndian) {
        return fail("big-endian ELF images are not supported");
    }
    const size_t headerSize = elfClass == kELFClass64 ? 64 : 52;
    const size_t programHeaderSize = elfClass == kELFClass64 ? 56 : 32;
    if (size < headerSize) {
        return fail("ELF header is truncated");
    }

    const uint16_t type = read16(bytes + 16);
    const uint16_t machine = read16(bytes + 18);
    if (type != kELFTypeExecutable && type != kELFTypeShared) {
        return fail("ELF image is not executable or position independent");
    }

    ELFImageInfo result;
    result.positionIndependent = type == kELFTypeShared;
    if (elfClass == kELFClass32 && machine == kELFMachine386) {
        result.architecture = ELFGuestArchitecture::X86;
    } else if (elfClass == kELFClass64 && machine == kELFMachineX86_64) {
        result.architecture = ELFGuestArchitecture::X86_64;
    } else {
        result.architecture = ELFGuestArchitecture::Unsupported;
        result.error = "ELF class and machine are not a supported x86 pairing";
        return result;
    }

    const uint64_t programOffset = elfClass == kELFClass64
        ? read64(bytes + 32) : read32(bytes + 28);
    const uint16_t declaredHeaderSize = read16(bytes + (elfClass == kELFClass64 ? 52 : 40));
    const uint16_t declaredProgramSize = read16(bytes + (elfClass == kELFClass64 ? 54 : 42));
    const uint16_t programCount = read16(bytes + (elfClass == kELFClass64 ? 56 : 44));
    result.entry = elfClass == kELFClass64 ? read64(bytes + 24) : read32(bytes + 24);
    if (declaredHeaderSize < headerSize || declaredProgramSize < programHeaderSize) {
        return fail("ELF header sizes are smaller than the selected class");
    }
    if (programCount != 0 &&
        programOffset > std::numeric_limits<uint64_t>::max() -
                            static_cast<uint64_t>(programCount) * declaredProgramSize) {
        return fail("ELF program table overflows the address width");
    }
    const uint64_t tableBytes = static_cast<uint64_t>(programCount) * declaredProgramSize;
    if (!rangeFits(programOffset, tableBytes, size)) {
        return fail("ELF program table lies outside the file");
    }

    for (uint16_t index = 0; index < programCount; ++index) {
        const uint8_t* program = bytes + programOffset +
                                 static_cast<uint64_t>(index) * declaredProgramSize;
        const uint32_t programType = read32(program);
        const uint32_t flags = elfClass == kELFClass64 ? read32(program + 4)
                                                       : read32(program + 24);
        const uint64_t fileOffset = elfClass == kELFClass64 ? read64(program + 8)
                                                            : read32(program + 4);
        const uint64_t virtualAddress = elfClass == kELFClass64 ? read64(program + 16)
                                                                : read32(program + 8);
        const uint64_t fileSize = elfClass == kELFClass64 ? read64(program + 32)
                                                          : read32(program + 16);
        const uint64_t memorySize = elfClass == kELFClass64 ? read64(program + 40)
                                                            : read32(program + 20);
        const uint64_t alignment = elfClass == kELFClass64 ? read64(program + 48)
                                                           : read32(program + 28);
        if (fileSize > memorySize) {
            return fail("ELF segment file size exceeds its memory size");
        }
        if (!rangeFits(fileOffset, fileSize, size)) {
            return fail("ELF segment data lies outside the file");
        }
        if (virtualAddress > std::numeric_limits<uint64_t>::max() - memorySize) {
            return fail("ELF segment virtual range overflows");
        }
        if (programType == kProgramLoad) {
            result.loadSegments.push_back({
                fileOffset, virtualAddress, fileSize, memorySize, alignment,
                (flags & kFlagRead) != 0,
                (flags & kFlagWrite) != 0,
                (flags & kFlagExecute) != 0,
            });
        } else if (programType == kProgramInterpreter) {
            if (fileSize == 0 || fileSize > 4096) {
                return fail("ELF interpreter path has an invalid size");
            }
            const char* path = reinterpret_cast<const char*>(bytes + fileOffset);
            size_t length = 0;
            while (length < fileSize && path[length] != '\0') {
                ++length;
            }
            if (length == fileSize) {
                return fail("ELF interpreter path is not terminated");
            }
            result.interpreter.assign(path, length);
        }
    }
    if (result.loadSegments.empty()) {
        return fail("ELF image contains no loadable segments");
    }
    result.valid = true;
    result.error.clear();
    return result;
}

} // namespace boxedvn
