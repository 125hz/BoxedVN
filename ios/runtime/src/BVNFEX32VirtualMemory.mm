/*
 * BoxedVN - Darwin virtual-memory provider for the FEX32 contract.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#include "BVNFEX32VirtualMemory.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <limits>

namespace {

constexpr std::uint64_t kMinimumPageSize = 0x1000;

std::uint64_t darwinPageSize() noexcept {
    static const std::uint64_t size = []() noexcept {
        const long value = ::sysconf(_SC_PAGESIZE);
        return value > 0 ? static_cast<std::uint64_t>(value)
                         : kMinimumPageSize;
    }();
    return size;
}

bool isPowerOfTwo(std::uint64_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

int hostProtection(std::uint8_t access) noexcept {
    int result = PROT_NONE;
    if ((access & boxedvn::Fex32WindowRead) != 0 ||
        (access & boxedvn::Fex32WindowExecute) != 0) {
        result |= PROT_READ;
    }
    if ((access & boxedvn::Fex32WindowWrite) != 0) {
        result |= PROT_WRITE;
    }
    return result;
}

bool validAccess(std::uint8_t access) noexcept {
    constexpr std::uint8_t known =
        boxedvn::Fex32WindowRead | boxedvn::Fex32WindowWrite |
        boxedvn::Fex32WindowExecute;
    return (access & ~known) == 0;
}

bool alignedRange(boxedvn::HostAddress64 address, std::uint64_t size,
                  std::uint64_t pageSize) noexcept {
    return size != 0 && pageSize != 0 &&
           (address.value % pageSize) == 0 && (size % pageSize) == 0;
}

bool discardPages(void* address, std::uint64_t size) noexcept {
#if defined(MADV_DONTNEED)
    return ::madvise(address, static_cast<size_t>(size), MADV_DONTNEED) == 0;
#elif defined(MADV_FREE)
    return ::madvise(address, static_cast<size_t>(size), MADV_FREE) == 0;
#else
    (void)address;
    (void)size;
    return false;
#endif
}

}  // namespace

BVNFEX32DarwinVirtualMemory::~BVNFEX32DarwinVirtualMemory() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reservedSize_ != 0) {
        ::mach_vm_deallocate(mach_task_self(), reservedBase_.value,
                             reservedSize_);
        reservedBase_ = {};
        reservedSize_ = 0;
    }
}

std::uint64_t BVNFEX32DarwinVirtualMemory::hostPageSize() const noexcept {
    return darwinPageSize();
}

std::optional<boxedvn::HostAddress64>
BVNFEX32DarwinVirtualMemory::reserve(std::uint64_t size,
                                     std::uint64_t alignment) noexcept {
    const std::uint64_t pageSize = hostPageSize();
    if (!alignedRange(boxedvn::HostAddress64(0), size, pageSize) ||
        !isPowerOfTwo(alignment) || alignment < pageSize ||
        (alignment % pageSize) != 0 ||
        size > std::numeric_limits<std::uint64_t>::max() - alignment) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (reservedSize_ != 0) {
        return std::nullopt;
    }

    // Both operands are host-page aligned, so the full over-reservation is
    // page aligned too. Requesting size+alignment (rather than size+alignment
    // minus one) keeps Mach's rounded allocation size equal to our arithmetic.
    const std::uint64_t requestedSpan = size + alignment;
    mach_vm_address_t raw = 0;
    const kern_return_t allocation = mach_vm_allocate(
        mach_task_self(), &raw, static_cast<mach_vm_size_t>(requestedSpan),
        VM_FLAGS_ANYWHERE);
    if (allocation != KERN_SUCCESS) {
        return std::nullopt;
    }

    const std::uint64_t rawValue = static_cast<std::uint64_t>(raw);
    const auto layout = bvn_fex32::computeReservationLayout(
        rawValue, size, alignment);
    if (!layout.has_value()) {
        ::mach_vm_deallocate(mach_task_self(), raw, requestedSpan);
        return std::nullopt;
    }

    const std::uint64_t alignedValue = layout->aligned;
    if (layout->prefix != 0 && mach_vm_deallocate(
            mach_task_self(), raw,
            static_cast<mach_vm_size_t>(layout->prefix)) !=
            KERN_SUCCESS) {
        ::mach_vm_deallocate(mach_task_self(), raw, requestedSpan);
        return std::nullopt;
    }
    if (layout->suffix != 0 && mach_vm_deallocate(
            mach_task_self(),
            static_cast<mach_vm_address_t>(alignedValue + size),
            static_cast<mach_vm_size_t>(layout->suffix)) != KERN_SUCCESS) {
        ::mach_vm_deallocate(
            mach_task_self(), static_cast<mach_vm_address_t>(alignedValue),
            static_cast<mach_vm_size_t>(requestedSpan - layout->prefix));
        return std::nullopt;
    }

    // mach_vm_allocate starts as zero-filled read/write memory. Keep the
    // reservation inaccessible until the window commits host-page groups.
    if (mach_vm_protect(mach_task_self(),
                        static_cast<mach_vm_address_t>(alignedValue),
                        static_cast<mach_vm_size_t>(size), FALSE,
                        VM_PROT_NONE) !=
        KERN_SUCCESS) {
        ::mach_vm_deallocate(mach_task_self(), alignedValue, size);
        return std::nullopt;
    }

    reservedBase_.value = alignedValue;
    reservedSize_ = size;
    return reservedBase_;
}

bool BVNFEX32DarwinVirtualMemory::validRangeLocked(
    boxedvn::HostAddress64 address, std::uint64_t size) const noexcept {
    const std::uint64_t pageSize = hostPageSize();
    if (reservedSize_ == 0 || !alignedRange(address, size, pageSize) ||
        address.value < reservedBase_.value) {
        return false;
    }
    const std::uint64_t offset = address.value - reservedBase_.value;
    return offset <= reservedSize_ && size <= reservedSize_ - offset;
}

bool BVNFEX32DarwinVirtualMemory::commit(
    boxedvn::HostAddress64 address, std::uint64_t size,
    std::uint8_t access) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!validRangeLocked(address, size)) {
        return false;
    }
    if (!validAccess(access) || access == boxedvn::Fex32WindowNone) {
        return false;
    }
    const mach_vm_address_t hostAddress = address.value;
    const mach_vm_size_t hostSize = size;
    // A commit establishes fresh anonymous storage. This explicit zeroing is
    // required even on systems where MADV_FREE is only an eviction hint.
    if (mach_vm_protect(mach_task_self(), hostAddress, hostSize, FALSE,
                        VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS) {
        return false;
    }
    std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(address.value)),
                0, static_cast<size_t>(size));
    if (mach_vm_protect(
            mach_task_self(), hostAddress, hostSize, FALSE,
            static_cast<vm_prot_t>(hostProtection(access))) ==
        KERN_SUCCESS) {
        return true;
    }
    mach_vm_protect(mach_task_self(), hostAddress, hostSize, FALSE,
                    VM_PROT_NONE);
    return false;
}

bool BVNFEX32DarwinVirtualMemory::protect(
    boxedvn::HostAddress64 address, std::uint64_t size,
    std::uint8_t access) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!validRangeLocked(address, size)) {
        return false;
    }
    if (!validAccess(access)) {
        return false;
    }
    return mach_vm_protect(
               mach_task_self(), address.value, size, FALSE,
               static_cast<vm_prot_t>(hostProtection(access))) ==
           KERN_SUCCESS;
}

bool BVNFEX32DarwinVirtualMemory::decommit(
    boxedvn::HostAddress64 address, std::uint64_t size) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!validRangeLocked(address, size)) {
        return false;
    }
    // Removing access is the authoritative decommit transition. Discard is a
    // best-effort physical-memory hint: commit always zeroes the complete host
    // page group, so MADV support cannot affect guest-visible bytes.
    if (mach_vm_protect(mach_task_self(), address.value, size, FALSE,
                        VM_PROT_NONE) != KERN_SUCCESS) {
        return false;
    }
    (void)discardPages(reinterpret_cast<void*>(
                           static_cast<uintptr_t>(address.value)), size);
    return true;
}

void BVNFEX32DarwinVirtualMemory::release(
    boxedvn::HostAddress64 address, std::uint64_t size) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reservedSize_ == 0 || address.value != reservedBase_.value ||
        size != reservedSize_) {
        return;
    }
    if (::mach_vm_deallocate(mach_task_self(), address.value, size) ==
        KERN_SUCCESS) {
        reservedBase_ = {};
        reservedSize_ = 0;
    }
}

std::unique_ptr<BVNFEX32DarwinVirtualMemory>
BVNFEX32CreateDarwinVirtualMemory() {
    return std::make_unique<BVNFEX32DarwinVirtualMemory>();
}

std::unique_ptr<BVNFEX32DarwinGuestWindow>
BVNFEX32DarwinGuestWindow::create() {
    auto result = std::unique_ptr<BVNFEX32DarwinGuestWindow>(
        new BVNFEX32DarwinGuestWindow());
    result->memory_ = BVNFEX32CreateDarwinVirtualMemory();
    result->window_ = boxedvn::Fex32GuestWindow::create(*result->memory_);
    if (!result->window_) {
        return nullptr;
    }
    return result;
}
