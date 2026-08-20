#include "boxedvn_test.h"
#include "boxedvn/elf_inspector.h"

#include <cstdint>
#include <vector>

using namespace boxedvn;

namespace {

void write16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void write32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

void write64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    write32(bytes, offset, static_cast<uint32_t>(value));
    write32(bytes, offset + 4, static_cast<uint32_t>(value >> 32));
}

std::vector<uint8_t> elf64Image() {
    std::vector<uint8_t> bytes(0x240, 0);
    bytes[0] = 0x7f; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
    bytes[4] = 2; bytes[5] = 1; bytes[6] = 1;
    write16(bytes, 16, 3);
    write16(bytes, 18, 62);
    write32(bytes, 20, 1);
    write64(bytes, 24, 0x140001000ULL);
    write64(bytes, 32, 64);
    write16(bytes, 52, 64);
    write16(bytes, 54, 56);
    write16(bytes, 56, 2);

    write32(bytes, 64, 1);
    write32(bytes, 68, 5);
    write64(bytes, 72, 0x200);
    write64(bytes, 80, 0x140001000ULL);
    write64(bytes, 96, 0x20);
    write64(bytes, 104, 0x1000);
    write64(bytes, 112, 0x1000);

    write32(bytes, 120, 3);
    write32(bytes, 124, 4);
    write64(bytes, 128, 0x180);
    write64(bytes, 152, 28);
    write64(bytes, 160, 28);
    write64(bytes, 168, 1);
    const char interpreter[] = "/lib64/ld-linux-x86-64.so.2";
    for (size_t index = 0; index < sizeof(interpreter); ++index) {
        bytes[0x180 + index] = static_cast<uint8_t>(interpreter[index]);
    }
    return bytes;
}

} // namespace

BOXEDVN_TEST(elf_inspector_accepts_x86_64_with_64_bit_addresses) {
    const std::vector<uint8_t> bytes = elf64Image();
    const ELFImageInfo image = inspectELF(bytes.data(), bytes.size());
    CHECK_EQ(image.valid, true);
    CHECK_EQ(image.architecture == ELFGuestArchitecture::X86_64, true);
    CHECK_EQ(image.positionIndependent, true);
    CHECK_EQ(image.entry, uint64_t(0x140001000ULL));
    CHECK_EQ(image.loadSegments.size(), size_t(1));
    CHECK_EQ(image.loadSegments[0].virtualAddress, uint64_t(0x140001000ULL));
    CHECK_EQ(image.loadSegments[0].executable, true);
    CHECK_EQ(image.interpreter, std::string("/lib64/ld-linux-x86-64.so.2"));
}

BOXEDVN_TEST(elf_inspector_rejects_mismatched_32_bit_class_and_x64_machine) {
    std::vector<uint8_t> bytes = elf64Image();
    bytes[4] = 1;
    const ELFImageInfo image = inspectELF(bytes.data(), bytes.size());
    CHECK_EQ(image.valid, false);
    CHECK_EQ(image.architecture == ELFGuestArchitecture::Unsupported, true);
}

BOXEDVN_TEST(elf_inspector_rejects_program_table_outside_file) {
    std::vector<uint8_t> bytes = elf64Image();
    write64(bytes, 32, 0xfffffffffffffff0ULL);
    const ELFImageInfo image = inspectELF(bytes.data(), bytes.size());
    CHECK_EQ(image.valid, false);
    CHECK_CONTAINS(image.error, "program table");
}

BOXEDVN_TEST(elf_inspector_rejects_segment_file_overrun) {
    std::vector<uint8_t> bytes = elf64Image();
    write64(bytes, 96, 0x1000);
    write64(bytes, 104, 0x1000);
    const ELFImageInfo image = inspectELF(bytes.data(), bytes.size());
    CHECK_EQ(image.valid, false);
    CHECK_CONTAINS(image.error, "outside the file");
}
