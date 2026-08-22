/*
 * BoxedVN - checked address contract for a 32-bit guest.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BOXEDVN_GUEST_ADDRESS_CONTRACT32_H
#define BOXEDVN_GUEST_ADDRESS_CONTRACT32_H

#include <cstdint>
#include <optional>

namespace boxedvn {

// These wrappers deliberately do not expose a void* conversion. A guest
// address is an integer in the guest ABI; a host address is an integer until
// a platform-owned mapping layer explicitly turns it into a pointer.
struct GuestAddress32 final {
    std::uint32_t value = 0;

    explicit constexpr GuestAddress32(std::uint32_t value = 0) noexcept
        : value(value) {}
};

struct HostAddress64 final {
    std::uint64_t value = 0;

    explicit constexpr HostAddress64(std::uint64_t value = 0) noexcept
        : value(value) {}
};

static_assert(sizeof(GuestAddress32) == sizeof(std::uint32_t));
static_assert(sizeof(HostAddress64) == sizeof(std::uint64_t));

class GuestAddressContract32 final {
public:
    enum class Mode : std::uint8_t {
        Paged,
        BiasedDirect,
    };

    static constexpr std::uint32_t kGuestPointerBits = 32;
    static constexpr std::uint64_t kGuestAddressLimit =
        std::uint64_t{1} << kGuestPointerBits;

    // Creates a contract only when the host window is representable and no
    // larger than the complete 32-bit guest address space. The window is
    // [hostBias, hostBias + hostWindow); it may be empty for Paged mode.
    static std::optional<GuestAddressContract32> create(
        Mode mode, std::uint64_t hostBias, std::uint64_t hostWindow) noexcept;

    Mode mode() const noexcept { return mode_; }
    std::uint64_t hostBias() const noexcept { return hostBias_; }
    std::uint64_t hostWindow() const noexcept { return hostWindow_; }

    // Ranges are half-open and must contain at least one byte. The check is
    // independent of the mapping mode, so syscall validation can use it even
    // when page lookup remains owned by a separate MMU.
    static bool validGuestRange(GuestAddress32 base,
                                std::uint64_t length) noexcept;

    // These conversions are available only for a direct biased mapping. A
    // Paged contract returns no address because page lookup is not implicit.
    // Both methods validate the entire requested range, not just its first
    // byte.
    std::optional<HostAddress64> guestToHost(
        GuestAddress32 guestAddress, std::uint64_t length = 1) const noexcept;
    std::optional<GuestAddress32> hostToGuest(
        HostAddress64 hostAddress, std::uint64_t length = 1) const noexcept;

private:
    constexpr GuestAddressContract32(Mode mode, std::uint64_t hostBias,
                                     std::uint64_t hostWindow) noexcept
        : mode_(mode), hostBias_(hostBias), hostWindow_(hostWindow) {}

    static bool checkedAdd(std::uint64_t left, std::uint64_t right,
                           std::uint64_t& result) noexcept;

    Mode mode_;
    std::uint64_t hostBias_;
    std::uint64_t hostWindow_;
};

}  // namespace boxedvn

#endif  // BOXEDVN_GUEST_ADDRESS_CONTRACT32_H
