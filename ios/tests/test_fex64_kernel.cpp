#include "boxedvn_test.h"
#include "boxedvn/fex64_kernel.h"

#include <array>
#include <cstdint>
#include <string>

using namespace boxedvn;

BOXEDVN_TEST(fex64_kernel_routes_write_through_checked_guest_memory) {
    std::array<char, 6> bytes {'h', 'e', 'l', 'l', 'o', '\n'};
    GuestAddressSpace64 memory;
    CHECK_EQ(memory.add({0x2000, bytes.size(),
                         reinterpret_cast<uintptr_t>(bytes.data()),
                         GuestMemoryRead}), true);
    std::string output;
    FEX64Kernel kernel(memory, [&output](int descriptor, std::string_view value) {
        CHECK_EQ(descriptor, 1);
        output.append(value);
    });
    Linux64Syscall call {1, {1, 0x2000, bytes.size(), 0, 0, 0}};
    const Linux64SyscallResult result = kernel.dispatch(call);
    CHECK_EQ(result.value, static_cast<int64_t>(bytes.size()));
    CHECK_EQ(output, std::string("hello\n"));
}

BOXEDVN_TEST(fex64_kernel_rejects_unmapped_guest_pointer) {
    GuestAddressSpace64 memory;
    FEX64Kernel kernel(memory);
    const Linux64SyscallResult result =
        kernel.dispatch({1, {1, 0x4000, 8, 0, 0, 0}});
    CHECK_EQ(result.value < 0, true);
}

BOXEDVN_TEST(fex64_kernel_owns_exit_semantics) {
    GuestAddressSpace64 memory;
    FEX64Kernel kernel(memory);
    const Linux64SyscallResult result = kernel.dispatch({231, {298, 0, 0, 0, 0, 0}});
    CHECK_EQ(result.action == Linux64SyscallAction::ExitProcess, true);
    CHECK_EQ(kernel.exited(), true);
    CHECK_EQ(kernel.exitCode(), 42);
}

BOXEDVN_TEST(fex64_kernel_reports_unimplemented_syscalls) {
    GuestAddressSpace64 memory;
    FEX64Kernel kernel(memory);
    CHECK_EQ(kernel.dispatch({9999, {}}).value < 0, true);
}
