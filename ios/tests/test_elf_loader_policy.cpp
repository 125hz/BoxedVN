#include "boxedvn_test.h"
#include "elfloaderpolicy64.h"

BOXEDVN_TEST(elf64_relro_is_deferred_to_program_interpreter) {
    CHECK_EQ(kernelOwnsElf64Relro(true), false);
}

BOXEDVN_TEST(elf64_relro_is_finalized_without_program_interpreter) {
    CHECK_EQ(kernelOwnsElf64Relro(false), true);
}
