/*
 * BoxedVN - x86-64 Linux syscall seam used by the optional FEX CPU backend.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#include "boxedvn/fex64_kernel.h"

#include <cerrno>
#include <limits>
#include <utility>

namespace boxedvn {

namespace {
constexpr uint64_t kSysWrite = 1;
constexpr uint64_t kSysExit = 60;
constexpr uint64_t kSysExitGroup = 231;
constexpr uint64_t kMaximumSingleWrite = 1024u * 1024u;
}

FEX64Kernel::FEX64Kernel(GuestAddressSpace64& memory, Output output)
    : memory_(memory), output_(std::move(output)) {}

Linux64SyscallResult FEX64Kernel::dispatch(const Linux64Syscall& syscall) {
    switch (syscall.number) {
        case kSysWrite: {
            const int descriptor = static_cast<int>(syscall.arguments[0]);
            const uint64_t address = syscall.arguments[1];
            const uint64_t length = syscall.arguments[2];
            if (descriptor != 1 && descriptor != 2) {
                return {-EBADF, Linux64SyscallAction::Continue};
            }
            if (length > kMaximumSingleWrite ||
                length > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                return {-EINVAL, Linux64SyscallAction::Continue};
            }
            const void* bytes = memory_.translate(address, length, GuestMemoryRead);
            if (bytes == nullptr && length != 0) {
                return {-EFAULT, Linux64SyscallAction::Continue};
            }
            if (output_ && length != 0) {
                output_(descriptor, std::string_view(
                    static_cast<const char*>(bytes), static_cast<size_t>(length)));
            }
            return {static_cast<int64_t>(length), Linux64SyscallAction::Continue};
        }
        case kSysExit:
            exited_ = true;
            exitCode_ = static_cast<int>(syscall.arguments[0] & 0xffu);
            return {0, Linux64SyscallAction::ExitThread};
        case kSysExitGroup:
            exited_ = true;
            exitCode_ = static_cast<int>(syscall.arguments[0] & 0xffu);
            return {0, Linux64SyscallAction::ExitProcess};
        default:
            return {-ENOSYS, Linux64SyscallAction::Continue};
    }
}

} // namespace boxedvn
