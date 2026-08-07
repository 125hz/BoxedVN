/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  GPLv2; see license.txt.
 *
 *  These tests build PE images byte by byte rather than checking in binaries,
 *  so the exact header field under test is visible in the test itself.
 */

#include <cstring>
#include <vector>

#include "boxedvn/pe_inspector.h"
#include "boxedvn_test.h"

using namespace boxedvn;

namespace {

void putU16(std::vector<uint8_t>& image, size_t offset, uint16_t value) {
    if (image.size() < offset + 2) {
        image.resize(offset + 2, 0);
    }
    image[offset] = static_cast<uint8_t>(value & 0xFF);
    image[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void putU32(std::vector<uint8_t>& image, size_t offset, uint32_t value) {
    if (image.size() < offset + 4) {
        image.resize(offset + 4, 0);
    }
    for (int i = 0; i < 4; ++i) {
        image[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

// Builds a minimal but structurally valid PE image.
std::vector<uint8_t> makePE(uint16_t machine, uint16_t optionalMagic,
                            uint16_t sizeOfOptionalHeader = 224,
                            uint16_t subsystem = 2) {
    constexpr uint32_t kPEOffset = 0x80;
    std::vector<uint8_t> image(kPEOffset + 24 + 240, 0);

    putU16(image, 0, 0x5A4D);            // 'MZ'
    putU32(image, 0x3C, kPEOffset);      // e_lfanew
    putU32(image, kPEOffset, 0x00004550);  // 'PE\0\0'

    // COFF header
    putU16(image, kPEOffset + 4, machine);
    putU16(image, kPEOffset + 6, 1);     // NumberOfSections
    putU16(image, kPEOffset + 20, sizeOfOptionalHeader);
    putU16(image, kPEOffset + 22, 0x0102);  // Characteristics

    // Optional header
    const size_t optional = kPEOffset + 24;
    putU16(image, optional, optionalMagic);
    putU16(image, optional + 68, subsystem);
    putU32(image, optional + (optionalMagic == 0x010B ? 92 : 108), 16);

    return image;
}

}  // namespace

BOXEDVN_TEST(pe32_i386_is_recognised_and_runnable) {
    const std::vector<uint8_t> image = makePE(0x014C, 0x010B);
    const ExecutableInfo info = inspectExecutable(image.data(), image.size());

    CHECK_EQ(std::string(toString(info.format)), std::string("pe32"));
    CHECK_EQ(std::string(toString(info.architecture)), std::string("x86_32"));
    CHECK_EQ(info.runnable, true);
    CHECK_EQ(std::string(toString(info.backend)), std::string("boxedwine-x86"));
    CHECK_EQ(info.subsystem, static_cast<uint16_t>(2));
}

BOXEDVN_TEST(pe32plus_amd64_is_rejected_with_x64_message) {
    const std::vector<uint8_t> image = makePE(0x8664, 0x020B);
    const ExecutableInfo info = inspectExecutable(image.data(), image.size());

    CHECK_EQ(std::string(toString(info.format)), std::string("pe32+"));
    CHECK_EQ(std::string(toString(info.architecture)), std::string("x86_64"));
    CHECK_EQ(info.runnable, false);
    CHECK_EQ(std::string(toString(info.backend)), std::string("none"));
    CHECK_CONTAINS(info.diagnostic, "x64 is not supported by the current runtime");
}

BOXEDVN_TEST(pe32plus_arm64_is_rejected_as_non_x86) {
    const std::vector<uint8_t> image = makePE(0xAA64, 0x020B);
    const ExecutableInfo info = inspectExecutable(image.data(), image.size());

    CHECK_EQ(std::string(toString(info.architecture)), std::string("unknown"));
    CHECK_EQ(info.runnable, false);
    CHECK_CONTAINS(info.diagnostic, "ARM64");
}

BOXEDVN_TEST(contradictory_magic_and_machine_are_rejected) {
    // PE32+ magic with an i386 machine: malformed, and must not be treated as
    // a runnable 32-bit image.
    const std::vector<uint8_t> image = makePE(0x014C, 0x020B);
    const ExecutableInfo info = inspectExecutable(image.data(), image.size());

    CHECK_EQ(info.runnable, false);
    CHECK_CONTAINS(info.diagnostic, "contradict");
}

BOXEDVN_TEST(empty_file_fails_safely) {
    const ExecutableInfo info = inspectExecutable(nullptr, 0);
    CHECK_EQ(std::string(toString(info.format)), std::string("not-an-executable"));
    CHECK_EQ(info.runnable, false);
    CHECK_CONTAINS(info.diagnostic, "empty");
}

BOXEDVN_TEST(non_executable_data_fails_safely) {
    const char* text = "This is a readme, not a program.";
    const ExecutableInfo info = inspectExecutable(
        reinterpret_cast<const uint8_t*>(text), std::strlen(text));
    CHECK_EQ(std::string(toString(info.format)), std::string("not-an-executable"));
    CHECK_CONTAINS(info.diagnostic, "MZ");
}

BOXEDVN_TEST(lfanew_past_end_of_file_is_rejected) {
    std::vector<uint8_t> image(0x40, 0);
    putU16(image, 0, 0x5A4D);
    putU32(image, 0x3C, 0x7FFFFFFF);
    const ExecutableInfo info = inspectExecutable(image.data(), image.size());

    CHECK_EQ(info.runnable, false);
    CHECK_CONTAINS(info.diagnostic, "past the end");
}

BOXEDVN_TEST(truncated_after_pe_signature_is_rejected) {
    std::vector<uint8_t> image = makePE(0x014C, 0x010B);
    image.resize(0x80 + 6);  // cut inside the COFF header
    const ExecutableInfo info = inspectExecutable(image.data(), image.size());

    CHECK_EQ(info.runnable, false);
    CHECK_CONTAINS(info.diagnostic, "truncated");
}

BOXEDVN_TEST(dos_mz_without_extended_header_is_win16_runnable) {
    std::vector<uint8_t> image(0x40, 0);
    putU16(image, 0, 0x5A4D);
    putU32(image, 0x3C, 0);
    const ExecutableInfo info = inspectExecutable(image.data(), image.size());

    CHECK_EQ(std::string(toString(info.format)), std::string("dos-mz"));
    CHECK_EQ(std::string(toString(info.architecture)), std::string("x86_16"));
    CHECK_EQ(info.runnable, true);
}

BOXEDVN_TEST(ne_win16_is_recognised_and_runnable) {
    std::vector<uint8_t> image(0x100, 0);
    putU16(image, 0, 0x5A4D);
    putU32(image, 0x3C, 0x40);
    putU16(image, 0x40, 0x454E);  // 'NE'
    const ExecutableInfo info = inspectExecutable(image.data(), image.size());

    CHECK_EQ(std::string(toString(info.format)), std::string("ne-win16"));
    CHECK_EQ(info.runnable, true);
}

BOXEDVN_TEST(le_vxd_is_recognised_and_not_runnable) {
    std::vector<uint8_t> image(0x100, 0);
    putU16(image, 0, 0x5A4D);
    putU32(image, 0x3C, 0x40);
    putU16(image, 0x40, 0x454C);  // 'LE'
    const ExecutableInfo info = inspectExecutable(image.data(), image.size());

    CHECK_EQ(std::string(toString(info.format)), std::string("le-vxd"));
    CHECK_EQ(info.runnable, false);
    CHECK_CONTAINS(info.diagnostic, "VxD");
}

BOXEDVN_TEST(coff_object_file_is_rejected) {
    std::vector<uint8_t> image = makePE(0x014C, 0x010B, /*sizeOfOptionalHeader=*/0);
    const ExecutableInfo info = inspectExecutable(image.data(), image.size());

    CHECK_EQ(info.runnable, false);
    CHECK_CONTAINS(info.diagnostic, "object file");
}

BOXEDVN_TEST(unknown_optional_header_magic_is_rejected) {
    const std::vector<uint8_t> image = makePE(0x014C, 0x1234);
    const ExecutableInfo info = inspectExecutable(image.data(), image.size());

    CHECK_EQ(info.runnable, false);
    CHECK_CONTAINS(info.diagnostic, "0x1234");
}

BOXEDVN_TEST(dotnet_directory_is_reported_but_still_runnable) {
    std::vector<uint8_t> image = makePE(0x014C, 0x010B);
    // NumberOfRvaAndSizes is at optional + 92 for PE32; the COM descriptor is
    // directory index 14, so its RVA/size pair starts 4 + 14*8 bytes later.
    const size_t numberOfRva = 0x80 + 24 + 92;
    putU32(image, numberOfRva + 4 + 14 * 8, 0x2008);      // RVA
    putU32(image, numberOfRva + 4 + 14 * 8 + 4, 0x48);    // size
    const ExecutableInfo info = inspectExecutable(image.data(), image.size());

    CHECK_EQ(info.managedDotNet, true);
    CHECK_EQ(info.runnable, true);
    CHECK_CONTAINS(info.diagnostic, ".NET");
}

BOXEDVN_TEST(missing_file_reports_the_path_and_errno) {
    const ExecutableInfo info =
        inspectExecutableFile("/definitely/not/a/real/path/game.exe");
    CHECK_EQ(info.runnable, false);
    CHECK_CONTAINS(info.diagnostic, "/definitely/not/a/real/path/game.exe");
}
