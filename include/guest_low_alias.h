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

// ---------------------------------------------------------------------------
// Wine's top-down arena.
//
// Wine reserves exactly this range PROT_NONE with MAP_FIXED_NOREPLACE and then
// commits accessible subranges inside it. The device log shows
// 0x7fffffdb0000+0x240000, 0x7ffffe000000+0x100000 and 0x7fffffc50000+0x39b000,
// all inside it; every one was refused as outside the identity lane, and Wine
// answered with an address-space search that reached 8,388,609 mmaps at about
// 98% of a core.
//
// The single OR cannot serve this range. Those guest addresses already carry
// the alias base's bits, so the OR is the identity there and the host would
// have to map 0x7ffffe000000 itself, which iOS will not do. It needs a second
// lane -- and the second lane has to be reachable without a comparison,
// because FEX keeps guest arithmetic flags in the host NZCV register
// (DEF_OP(LoadNZCV)/StoreNZCV) and a `tst`/`csel` inside memory-operand
// generation would corrupt them.
//
// The lane is therefore chosen so the whole translation is two flag-free
// instructions on one scratch register:
//
//     host = (guest | kGuestLowAliasBase) & ~kGuestTopClearMask
//
// which works because every arena address has bits 39..46 all set and every
// other hostable guest address has them all clear. Clearing that field is the
// relocation, and it is a no-op everywhere else.
inline constexpr std::uint64_t kGuestTopBase = 0x7FFFFE000000ULL;
inline constexpr std::uint64_t kGuestTopEnd = 0x7FFFFFFF0000ULL;
inline constexpr std::uint64_t kGuestTopLength = kGuestTopEnd - kGuestTopBase;

// Bits 39..46: set in every arena address, clear in every other hostable one.
inline constexpr std::uint64_t kGuestTopClearMask = 0x7F8000000000ULL;

// The host block the arena is served from. Derived, not chosen: it is what
// clearing that field leaves behind.
inline constexpr std::uint64_t kGuestTopHostBase =
    kGuestTopBase & ~kGuestTopClearMask;
inline constexpr std::uint64_t kGuestTopHostEnd =
    kGuestTopHostBase + kGuestTopLength;

static_assert(kGuestTopBase < kGuestTopEnd, "top arena must be non-empty");
static_assert((kGuestTopBase & kGuestTopClearMask) == kGuestTopClearMask,
              "every bit of the clear mask must be set at the arena base");
static_assert(((kGuestTopEnd - 1) & kGuestTopClearMask) == kGuestTopClearMask,
              "every bit of the clear mask must be set at the arena end");
static_assert((kGuestHighEnd & kGuestTopClearMask) == 0,
              "the identity lane must not touch the clear mask");
static_assert(((kGuestLowLimit - 1) & kGuestTopClearMask) == 0,
              "canonical low addresses must not touch the clear mask");
static_assert(((kGuestLowAliasEnd - 1) & kGuestTopClearMask) == 0,
              "the low alias window must not touch the clear mask");
static_assert(kGuestTopHostBase >= kGuestHighEnd,
              "the top host alias must sit above the identity lane");
static_assert(kGuestTopHostBase % 0x4000ULL == 0,
              "the top host alias must be host-page aligned");
static_assert((kGuestTopHostBase | kGuestLowAliasBase) == kGuestTopHostBase,
              "the top host alias must already carry the alias base bits");
static_assert(((kGuestTopHostEnd - 1) | kGuestLowAliasBase) ==
                  kGuestTopHostEnd - 1,
              "the whole top host alias must carry the alias base bits");
static_assert((kGuestTopHostEnd - 1) < kGuestTopBase,
              "the host alias must not collide with the canonical arena");

// Translate a canonical guest address to the host address its bytes live at.
// Identity for every permitted high address by construction.
inline constexpr bool isTopArenaGuestAddress(
    std::uint64_t guestAddress) noexcept {
    return guestAddress >= kGuestTopBase && guestAddress < kGuestTopEnd;
}

// The translation the ARM64 translator emits, spelled as the two operations it
// actually emits. Written this way on purpose: the emulator and the translator
// must not merely agree in intent, they must compute the same bits.
inline constexpr std::uint64_t guestToHostAddress(
    std::uint64_t guestAddress) noexcept {
    return (guestAddress | kGuestLowAliasBase) & ~kGuestTopClearMask;
}

// True when this host address is the alias of a canonical top-arena address.
inline constexpr bool isTopArenaHostAddress(
    std::uint64_t hostAddress) noexcept {
    return hostAddress >= kGuestTopHostBase && hostAddress < kGuestTopHostEnd;
}

// Recover the canonical guest address from a host address. Addresses outside
// the alias window are returned unchanged, so this is safe to apply to a fault
// address of unknown provenance.
inline constexpr std::uint64_t hostToGuestAddress(
    std::uint64_t hostAddress) noexcept {
    // The two alias windows are disjoint, and neither can be mistaken for a
    // valid identity-lane guest address: the low alias is below the identity
    // lane's end and the top alias is above it. An address in the identity
    // lane itself belongs to neither and is returned unchanged, which is what
    // makes this safe on a fault address of unknown provenance.
    if (hostAddress >= kGuestLowAliasBase && hostAddress < kGuestLowAliasEnd) {
        return hostAddress - kGuestLowAliasBase;
    }
    if (isTopArenaHostAddress(hostAddress)) {
        return hostAddress | kGuestTopClearMask;
    }
    return hostAddress;
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
    if (address >= kGuestHighBase && end <= kGuestHighEnd) {
        return true;
    }
    // Wholly inside Wine's top arena. A range that straddles the arena
    // boundary is refused rather than split: one mapping has to land in one
    // host lane.
    return address >= kGuestTopBase && end <= kGuestTopEnd;
}

} // namespace boxedvn

#endif // BOXEDWINE_GUEST_LOW_ALIAS_H
