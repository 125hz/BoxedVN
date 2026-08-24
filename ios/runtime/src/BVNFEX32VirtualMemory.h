/*
 * BoxedVN - Darwin virtual-memory provider for the FEX32 contract.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * This provider is deliberately diagnostic-only. It is not selected by the
 * Boxedwine CPU factory and does not make the current 32-bit path FEX-capable.
 */

#ifndef BVN_FEX32_VIRTUAL_MEMORY_H
#define BVN_FEX32_VIRTUAL_MEMORY_H

#include "boxedvn/fex32_guest_window.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>

namespace bvn_fex32 {

struct ReservationLayout final {
    std::uint64_t raw = 0;
    std::uint64_t span = 0;
    std::uint64_t aligned = 0;
    std::uint64_t prefix = 0;
    std::uint64_t suffix = 0;
};

// Pure arithmetic for the Darwin over-reserve/trim operation. Keeping this
// separate from Mach calls lets the Windows host tests cover alignment and
// overflow without claiming to exercise iOS VM.
constexpr std::optional<ReservationLayout> computeReservationLayout(
    std::uint64_t raw, std::uint64_t size,
    std::uint64_t alignment) noexcept {
    if (size == 0 || alignment == 0 ||
        (alignment & (alignment - 1)) != 0 ||
        size > std::numeric_limits<std::uint64_t>::max() - alignment ||
        raw > std::numeric_limits<std::uint64_t>::max() - (alignment - 1)) {
        return std::nullopt;
    }

    const std::uint64_t span = size + alignment;
    const std::uint64_t aligned =
        (raw + alignment - 1) & ~(alignment - 1);
    if (aligned < raw || aligned > std::numeric_limits<std::uint64_t>::max() -
                              size) {
        return std::nullopt;
    }
    const std::uint64_t rawEnd = raw + span;
    const std::uint64_t alignedEnd = aligned + size;
    if (rawEnd < raw || alignedEnd < aligned || alignedEnd > rawEnd) {
        return std::nullopt;
    }
    return ReservationLayout{raw, span, aligned, aligned - raw,
                             rawEnd - alignedEnd};
}

}  // namespace bvn_fex32

/// Mach-backed implementation of the injected FEX32 address-window seam.
///
/// The object reserves one high, 4 GiB-aligned span and changes protection at
/// the host page granularity. Guest execute is intentionally not translated
/// into host PROT_EXEC: FEX's translated code belongs in the separate iOS JIT
/// arena, and allowing guest pages to become executable would violate the
/// runtime's W^X boundary.
class BVNFEX32DarwinVirtualMemory final
    : public boxedvn::Fex32VirtualMemory {
public:
    BVNFEX32DarwinVirtualMemory() = default;
    ~BVNFEX32DarwinVirtualMemory() override;

    BVNFEX32DarwinVirtualMemory(const BVNFEX32DarwinVirtualMemory&) = delete;
    BVNFEX32DarwinVirtualMemory& operator=(
        const BVNFEX32DarwinVirtualMemory&) = delete;

    std::uint64_t hostPageSize() const noexcept override;
    std::optional<boxedvn::HostAddress64> reserve(
        std::uint64_t size, std::uint64_t alignment) noexcept override;
    bool commit(boxedvn::HostAddress64 address, std::uint64_t size,
                std::uint8_t access) noexcept override;
    bool protect(boxedvn::HostAddress64 address, std::uint64_t size,
                 std::uint8_t access) noexcept override;
    bool decommit(boxedvn::HostAddress64 address,
                  std::uint64_t size) noexcept override;
    void release(boxedvn::HostAddress64 address,
                 std::uint64_t size) noexcept override;

private:
    bool validRangeLocked(boxedvn::HostAddress64 address,
                          std::uint64_t size) const noexcept;

    mutable std::mutex mutex_;
    boxedvn::HostAddress64 reservedBase_{};
    std::uint64_t reservedSize_ = 0;
};

/// Creates the provider without selecting or running it.
std::unique_ptr<BVNFEX32DarwinVirtualMemory>
BVNFEX32CreateDarwinVirtualMemory();

/// Owns the provider and the Fex32GuestWindow in the required order. This is
/// the smallest safe probe object for future runtime experiments: callers can
/// inspect the real reservation and page-state contract without exposing a
/// dangling provider reference.
class BVNFEX32DarwinGuestWindow final {
public:
    static std::unique_ptr<BVNFEX32DarwinGuestWindow> create();

    ~BVNFEX32DarwinGuestWindow() = default;

    BVNFEX32DarwinGuestWindow(const BVNFEX32DarwinGuestWindow&) = delete;
    BVNFEX32DarwinGuestWindow& operator=(
        const BVNFEX32DarwinGuestWindow&) = delete;

    boxedvn::Fex32GuestWindow* window() noexcept { return window_.get(); }
    const boxedvn::Fex32GuestWindow* window() const noexcept {
        return window_.get();
    }

private:
    BVNFEX32DarwinGuestWindow() = default;

    // Declaration order is intentional: window_ is destroyed before
    // memory_, satisfying Fex32GuestWindow's injected-provider lifetime rule.
    std::unique_ptr<BVNFEX32DarwinVirtualMemory> memory_;
    std::unique_ptr<boxedvn::Fex32GuestWindow> window_;
};

#endif  // BVN_FEX32_VIRTUAL_MEMORY_H
