/*
 * BoxedWine - canonical low guest addresses served through a high host alias.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * Windows x86-64 binaries and Wine's loader genuinely require canonical LOW
 * guest addresses: the initial TEB block is reserved below 2 GiB,
 * KUSER_SHARED_DATA lives at 0x7ffe0000, and ordinary PE images prefer
 * 0x140000000. None of those can be host-mapped at their own address on iOS,
 * where everything below 4 GiB is under __PAGEZERO and the band above it is
 * already taken. Refusing them made Wine's TEB reservation fail, so it left
 * its block pointer null and dereferenced a small absolute offset from it.
 *
 * The guest keeps its canonical addresses; only the host address the bytes
 * live at is translated. The alias base is chosen so translation is a single
 * OR:
 *
 *   host = guest | kGuestLowAliasBase
 *
 * which is exact for both halves at once. Low guest addresses share no bit
 * with the base, so OR is addition. Every permitted HIGH guest address already
 * contains the base's bits, so OR is the identity there and high mappings keep
 * being dereferenced at their own address -- the translator emits one ARM64
 * `orr` with an encodable logical immediate for every guest memory access,
 * with no branch, no comparison and no extra register.
 *
 * This header is dependency-free so the arithmetic can be tested on any host.
 */

#ifndef BOXEDWINE_GUEST_LOW_ALIAS_H
#define BOXEDWINE_GUEST_LOW_ALIAS_H

#include <cstdint>

namespace boxedvn {

// Canonical guest addresses below this are served through the alias. Eight
// GiB covers the below-2-GiB TEB and KUSER regions and the standard
// 0x140000000 / 0x170000000 x86-64 PE and DLL bases.
inline constexpr std::uint64_t kGuestLowLimit = 0x200000000ULL;

// Host base for the alias. A single contiguous run of ones so ARM64 can OR it
// as a logical immediate, aligned to the low limit so OR equals addition, and
// inside the proven native host window.
inline constexpr std::uint64_t kGuestLowAliasBase = 0x7800000000ULL;

inline constexpr std::uint64_t kGuestLowAliasEnd =
    kGuestLowAliasBase + kGuestLowLimit;

// High identity lanes begin immediately above the alias window and stay within
// one aligned block, so every address in them carries the base's bits.
inline constexpr std::uint64_t kGuestHighBase = kGuestLowAliasEnd;
inline constexpr std::uint64_t kGuestHighEnd = kGuestHighBase + kGuestLowLimit;

static_assert((kGuestLowLimit & (kGuestLowLimit - 1)) == 0,
              "low guest limit must be a power of two");
static_assert((kGuestLowAliasBase & (kGuestLowLimit - 1)) == 0,
              "alias base must be aligned to the low limit so OR adds");
static_assert((kGuestHighBase & kGuestLowAliasBase) == kGuestLowAliasBase,
              "high lane base must already contain the alias base bits");
static_assert(((kGuestHighEnd - 1) & kGuestLowAliasBase) == kGuestLowAliasBase,
              "high lane end must already contain the alias base bits");
static_assert((kGuestHighBase ^ (kGuestHighEnd - 1)) < kGuestLowLimit,
              "high lane must not cross an alias-base bit boundary");

// Translate a canonical guest address to the host address its bytes live at.
// Identity for every permitted high address by construction.
inline constexpr std::uint64_t guestToHostAddress(
    std::uint64_t guestAddress) noexcept {
    return guestAddress | kGuestLowAliasBase;
}

// Recover the canonical guest address from a host address. Addresses outside
// the alias window are returned unchanged, so this is safe to apply to a fault
// address of unknown provenance.
inline constexpr std::uint64_t hostToGuestAddress(
    std::uint64_t hostAddress) noexcept {
    return (hostAddress >= kGuestLowAliasBase &&
            hostAddress < kGuestLowAliasEnd)
               ? hostAddress - kGuestLowAliasBase
               : hostAddress;
}

// True when this canonical guest address is served through the alias rather
// than dereferenced at its own address.
inline constexpr bool isLowAliasedGuestAddress(
    std::uint64_t guestAddress) noexcept {
    return guestAddress < kGuestLowLimit;
}

// True when [address, address+length) is a guest range this address space can
// host: either entirely canonical-low, or entirely inside the high lane.
inline constexpr bool guestRangeHostable(std::uint64_t address,
                                         std::uint64_t length) noexcept {
    if (length == 0 || address > UINT64_MAX - length) {
        return false;
    }
    const std::uint64_t end = address + length;
    if (end <= kGuestLowLimit) {
        return true;
    }
    return address >= kGuestHighBase && end <= kGuestHighEnd;
}

} // namespace boxedvn

#endif // BOXEDWINE_GUEST_LOW_ALIAS_H
