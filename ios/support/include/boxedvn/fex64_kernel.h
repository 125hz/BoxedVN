/*
 * BoxedVN - x86-64 Linux syscall seam used by the optional FEX CPU backend.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BOXEDVN_FEX64_KERNEL_H
#define BOXEDVN_FEX64_KERNEL_H

#include "boxedvn/guest_address_space.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>

namespace boxedvn {

struct Linux64Syscall {
    uint64_t number = 0;
    std::array<uint64_t, 6> arguments {};
};

enum class Linux64SyscallAction {
    Continue,
    ExitThread,
    ExitProcess,
};

struct Linux64SyscallResult {
    int64_t value = 0;
    Linux64SyscallAction action = Linux64SyscallAction::Continue;
};

// First, deliberately small portion of the BoxedWine-owned x86-64 syscall
// dispatcher.  It is a separate ABI adapter because Linux i386 and x86-64 use
// different numbers, registers, pointer widths, structures, and signal frames.
// FEX calls this object; it does not call Darwin syscalls directly.
class FEX64Kernel {
public:
    using Output = std::function<void(int descriptor, std::string_view bytes)>;

    explicit FEX64Kernel(GuestAddressSpace64& memory, Output output = {});

    Linux64SyscallResult dispatch(const Linux64Syscall& syscall);
    bool exited() const { return exited_; }
    int exitCode() const { return exitCode_; }

private:
    GuestAddressSpace64& memory_;
    Output output_;
    bool exited_ = false;
    int exitCode_ = 0;
};

} // namespace boxedvn

#endif
