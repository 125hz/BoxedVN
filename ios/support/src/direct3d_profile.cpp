/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 *
 *  Field offsets follow the Microsoft PE/COFF specification, revision 11.
 *  Every read is bounds-checked against the real file length and every loop
 *  is bounded, because these files come from whatever the user imported.
 */

#include "boxedvn/direct3d_profile.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace boxedvn {
namespace {

constexpr std::uint16_t kSignatureMZ = 0x5A4D;
constexpr std::uint32_t kSignaturePE = 0x00004550;
constexpr std::uint16_t kMagicPE32 = 0x010B;
constexpr std::uint16_t kMagicPE32Plus = 0x020B;

constexpr std::size_t kOffsetLfanew = 0x3C;
constexpr std::size_t kCoffHeaderSize = 20;
constexpr std::size_t kSectionHeaderSize = 40;

constexpr std::size_t kDirectoryImport = 1;
constexpr std::size_t kDirectoryDelayImport = 13;

// An import descriptor is 20 bytes and a delay-import descriptor 32. Both
// arrays end at an all-zero entry; these caps stop a malformed file that
// never terminates one.
constexpr std::size_t kImportDescriptorSize = 20;
constexpr std::size_t kDelayDescriptorSize = 32;
constexpr std::size_t kMaxDescriptors = 512;

// No real DLL name approaches this. It bounds the string read, not the name.
constexpr std::size_t kMaxModuleNameLength = 96;

// Sections beyond this mean a file that is not a normal Windows image.
constexpr std::uint16_t kMaxSections = 96;

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

struct Section {
    std::uint32_t virtualAddress = 0;
    std::uint32_t virtualSize = 0;
    std::uint32_t rawOffset = 0;
    std::uint32_t rawSize = 0;
};

// A file read through a handle rather than a buffer: an import table can sit
// tens of megabytes into a large DLL, and reading that far just to find a
// name would defeat the point of scanning a whole game directory.
class PeFile {
public:
    explicit PeFile(const std::string& path) {
        file_ = std::fopen(path.c_str(), "rb");
        if (file_ == nullptr) {
            return;
        }
        if (std::fseek(file_, 0, SEEK_END) == 0) {
            const long end = std::ftell(file_);
            if (end > 0) {
                size_ = static_cast<std::uint64_t>(end);
            }
        }
    }

    ~PeFile() {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }

    PeFile(const PeFile&) = delete;
    PeFile& operator=(const PeFile&) = delete;

    bool ok() const { return file_ != nullptr && size_ > 0; }

    bool read(std::uint64_t offset, void* out, std::size_t length) {
        if (!ok() || length == 0 || offset > size_ ||
            size_ - offset < length) {
            return false;
        }
        if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) {
            return false;
        }
        return std::fread(out, 1, length, file_) == length;
    }

    bool readU16(std::uint64_t offset, std::uint16_t* out) {
        std::uint8_t bytes[2];
        if (!read(offset, bytes, sizeof(bytes))) {
            return false;
        }
        *out = static_cast<std::uint16_t>(bytes[0]) |
               static_cast<std::uint16_t>(bytes[1]) << 8;
        return true;
    }

    bool readU32(std::uint64_t offset, std::uint32_t* out) {
        std::uint8_t bytes[4];
        if (!read(offset, bytes, sizeof(bytes))) {
            return false;
        }
        *out = static_cast<std::uint32_t>(bytes[0]) |
               static_cast<std::uint32_t>(bytes[1]) << 8 |
               static_cast<std::uint32_t>(bytes[2]) << 16 |
               static_cast<std::uint32_t>(bytes[3]) << 24;
        return true;
    }

    // Reads a NUL-terminated ASCII name, stopping at the bound or the end of
    // the file rather than assuming either.
    std::string readName(std::uint64_t offset) {
        std::string name;
        for (std::size_t i = 0; i < kMaxModuleNameLength; ++i) {
            std::uint8_t byte = 0;
            if (!read(offset + i, &byte, 1) || byte == 0) {
                break;
            }
            if (byte < 0x20 || byte > 0x7E) {
                return std::string();
            }
            name.push_back(static_cast<char>(byte));
        }
        return name;
    }

private:
    std::FILE* file_ = nullptr;
    std::uint64_t size_ = 0;
};

bool rvaToOffset(const std::vector<Section>& sections, std::uint32_t rva,
                 std::uint64_t* offset) {
    for (const Section& section : sections) {
        // VirtualSize can legitimately be 0 in an old linker's output, in
        // which case SizeOfRawData describes the section's extent.
        const std::uint32_t extent =
            section.virtualSize != 0 ? section.virtualSize : section.rawSize;
        if (extent == 0 || rva < section.virtualAddress) {
            continue;
        }
        const std::uint32_t delta = rva - section.virtualAddress;
        if (delta >= extent || delta >= section.rawSize) {
            continue;
        }
        *offset = static_cast<std::uint64_t>(section.rawOffset) + delta;
        return true;
    }
    return false;
}

void collectFrom(PeFile& file, const std::vector<Section>& sections,
                 std::uint32_t directoryRva, std::size_t descriptorSize,
                 std::size_t nameFieldOffset, std::uint64_t imageBase,
                 std::vector<std::string>& out) {
    if (directoryRva == 0) {
        return;
    }
    std::uint64_t descriptor = 0;
    if (!rvaToOffset(sections, directoryRva, &descriptor)) {
        return;
    }

    for (std::size_t i = 0; i < kMaxDescriptors; ++i) {
        const std::uint64_t base = descriptor + i * descriptorSize;
        std::uint32_t nameRva = 0;
        if (!file.readU32(base + nameFieldOffset, &nameRva)) {
            return;
        }

        // The terminator is an all-zero descriptor; a zero name field is the
        // cheapest reliable part of it to test.
        if (nameRva == 0) {
            return;
        }

        // Delay-import descriptors written before the v2 format store virtual
        // addresses rather than RVAs. Recognise that instead of discarding
        // the whole table.
        std::uint64_t nameOffset = 0;
        if (!rvaToOffset(sections, nameRva, &nameOffset)) {
            if (imageBase == 0 || nameRva < imageBase ||
                nameRva - imageBase > UINT32_MAX ||
                !rvaToOffset(sections,
                             static_cast<std::uint32_t>(nameRva - imageBase),
                             &nameOffset)) {
                return;
            }
        }

        const std::string name = file.readName(nameOffset);
        if (!name.empty()) {
            out.push_back(toLower(name));
        }
    }
}

}  // namespace

bool isModernDirect3DModule(const std::string& moduleName) {
    const std::string name = toLower(moduleName);
    if (name == "dxgi.dll") {
        return true;
    }
    // d3d10.dll, d3d10_1.dll, d3d10core.dll, d3d11.dll, d3d11_*.dll, d3d12*.
    // Deliberately not d3d9/d3d8/ddraw: wined3d handles those on Metal, and
    // routing them through DXVK is what broke a Direct3D 9 title in build 65.
    static const char* const prefixes[] = {"d3d10", "d3d11", "d3d12"};
    for (const char* prefix : prefixes) {
        const std::string candidate(prefix);
        if (name.size() >= candidate.size() &&
            name.compare(0, candidate.size(), candidate) == 0) {
            return true;
        }
    }
    return false;
}

bool isModernDirect3DFileName(const std::string& fileName) {
    const std::string name = toLower(fileName);
    // ANGLE reaches Direct3D 11 through LoadLibrary, so no import table names
    // it. Its own presence is the evidence: a game shipping libGLESv2.dll is
    // a Chromium, Electron or NW.js title whose GPU process is a D3D11
    // client, and on wined3d that process falls back to software rasterising
    // the whole page.
    return name == "libglesv2.dll" || name == "libegl.dll" ||
           isModernDirect3DModule(name);
}

std::vector<std::string> readPeImportedModules(const std::string& path) {
    std::vector<std::string> modules;
    PeFile file(path);
    if (!file.ok()) {
        return modules;
    }

    std::uint16_t mz = 0;
    if (!file.readU16(0, &mz) || mz != kSignatureMZ) {
        return modules;
    }

    std::uint32_t lfanew = 0;
    if (!file.readU32(kOffsetLfanew, &lfanew) || lfanew == 0) {
        return modules;
    }

    std::uint32_t signature = 0;
    if (!file.readU32(lfanew, &signature) || signature != kSignaturePE) {
        return modules;
    }

    const std::uint64_t coff = static_cast<std::uint64_t>(lfanew) + 4;
    std::uint16_t sectionCount = 0;
    std::uint16_t optionalSize = 0;
    if (!file.readU16(coff + 2, &sectionCount) ||
        !file.readU16(coff + 16, &optionalSize) || sectionCount == 0 ||
        sectionCount > kMaxSections || optionalSize == 0) {
        return modules;
    }

    const std::uint64_t optional = coff + kCoffHeaderSize;
    std::uint16_t magic = 0;
    if (!file.readU16(optional, &magic)) {
        return modules;
    }

    std::uint64_t imageBase = 0;
    std::uint64_t directoryCountOffset = 0;
    if (magic == kMagicPE32) {
        std::uint32_t base32 = 0;
        if (!file.readU32(optional + 28, &base32)) {
            return modules;
        }
        imageBase = base32;
        directoryCountOffset = optional + 92;
    } else if (magic == kMagicPE32Plus) {
        std::uint32_t low = 0;
        std::uint32_t high = 0;
        if (!file.readU32(optional + 24, &low) ||
            !file.readU32(optional + 28, &high)) {
            return modules;
        }
        imageBase = (static_cast<std::uint64_t>(high) << 32) | low;
        directoryCountOffset = optional + 108;
    } else {
        return modules;
    }

    std::uint32_t directoryCount = 0;
    if (!file.readU32(directoryCountOffset, &directoryCount)) {
        return modules;
    }
    const std::uint64_t directories = directoryCountOffset + 4;

    std::vector<Section> sections;
    sections.reserve(sectionCount);
    const std::uint64_t sectionTable = optional + optionalSize;
    for (std::uint16_t i = 0; i < sectionCount; ++i) {
        const std::uint64_t entry = sectionTable + i * kSectionHeaderSize;
        Section section;
        if (!file.readU32(entry + 8, &section.virtualSize) ||
            !file.readU32(entry + 12, &section.virtualAddress) ||
            !file.readU32(entry + 16, &section.rawSize) ||
            !file.readU32(entry + 20, &section.rawOffset)) {
            return modules;
        }
        sections.push_back(section);
    }

    if (directoryCount > kDirectoryImport) {
        std::uint32_t rva = 0;
        if (file.readU32(directories + kDirectoryImport * 8, &rva)) {
            collectFrom(file, sections, rva, kImportDescriptorSize, 12,
                        imageBase, modules);
        }
    }
    if (directoryCount > kDirectoryDelayImport) {
        std::uint32_t rva = 0;
        if (file.readU32(directories + kDirectoryDelayImport * 8, &rva)) {
            collectFrom(file, sections, rva, kDelayDescriptorSize, 4,
                        imageBase, modules);
        }
    }
    return modules;
}

Direct3DUsage detectDirect3DUsage(const std::string& gameDirectory,
                                  std::size_t maxFiles,
                                  std::size_t maxDepth) {
    Direct3DUsage usage;
    if (gameDirectory.empty()) {
        return usage;
    }

    std::error_code ec;
    const fs::path root(gameDirectory);
    if (!fs::is_directory(root, ec)) {
        return usage;
    }

    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return usage;
    }

    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (static_cast<std::size_t>(it.depth()) >= maxDepth) {
            it.disable_recursion_pending();
        }
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }

        const fs::path& file = it->path();
        const std::string extension = toLower(file.extension().string());
        if (extension != ".exe" && extension != ".dll") {
            continue;
        }

        const std::string fileName = file.filename().string();
        if (isModernDirect3DFileName(fileName)) {
            usage.needsModernDirect3D = true;
            usage.evidence = fileName + " ships with the game";
            usage.filesInspected++;
            return usage;
        }

        if (usage.filesInspected >= maxFiles) {
            usage.reachedLimit = true;
            break;
        }
        usage.filesInspected++;

        for (const std::string& module : readPeImportedModules(
                 file.string())) {
            if (isModernDirect3DModule(module)) {
                usage.needsModernDirect3D = true;
                usage.evidence = fileName + " imports " + module;
                return usage;
            }
        }
    }
    return usage;
}

}  // namespace boxedvn
