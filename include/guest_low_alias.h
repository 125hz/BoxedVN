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

// The ONE block the whole host layout lives in, and the real ceiling on it.
// The alias base is a contiguous run of ones, so the interval that begins at
// it and spans its LOWEST set bit is exactly the set of addresses that already
// carry every one of its bits: adding anything smaller than that bit cannot
// clear one, and cannot reach a bit above the run either. Every host address
// this address space ever dereferences -- the alias window, the identity lane
// and the relocated top arena -- has to lie inside it, because outside it the
// OR is not the identity. Address space the host might grant elsewhere is
// unreachable whatever the host says. For the base below that block is
// [0x7800000000, 0x8000000000): 32 GiB, and that is the whole budget.
inline constexpr std::uint64_t kGuestAliasBlockSpan =
    kGuestLowAliasBase & (~kGuestLowAliasBase + 1);
inline constexpr std::uint64_t kGuestAliasBlockEnd =
    kGuestLowAliasBase + kGuestAliasBlockSpan;

// The high identity lane begins immediately above the alias window and runs to
// the base of the relocated top arena, which is the next thing in the block.
// It used to stop one kGuestLowLimit above its base -- not because anything
// required that, but because the invariant below was stated as "the lane must
// not cross an alias-base bit boundary", which is sufficient for a lane of
// exactly that size and refuses every larger one that is equally exact. The
// sixteen GiB between the old end and the arena were simply never offered.
// Spelled as a literal because the arena's host base is derived further down;
// the two are asserted equal there, so neither can move without the other.
inline constexpr std::uint64_t kGuestHighBase = kGuestLowAliasEnd;
inline constexpr std::uint64_t kGuestHighEnd = 0x7F80000000ULL;

static_assert((kGuestLowLimit & (kGuestLowLimit - 1)) == 0,
              "low guest limit must be a power of two");
static_assert((kGuestLowAliasBase & (kGuestLowLimit - 1)) == 0,
              "alias base must be aligned to the low limit so OR adds");
static_assert(kGuestLowAliasBase != 0,
              "the alias base must have a lowest set bit to span a block");
static_assert((kGuestLowAliasBase & (kGuestAliasBlockSpan - 1)) == 0,
              "the block span must be the alias base's lowest set bit, so no "
              "base bit lies below it");
// The identity lane's correctness proof, in one statement. For any A in
// [kGuestLowAliasBase, kGuestAliasBlockEnd), A = kGuestLowAliasBase + d with
// d < the base's lowest set bit. Every base bit is at or above that bit, so
// the base's bits below it are all zero across the whole of d and
// base + d == base | d. Hence A & base == base -- the OR is the identity for
// every address in the lane, not merely at its two ends -- and A stays below
// kGuestAliasBlockEnd, so it carries no bit above the base's run either and
// the top-arena mask is clear throughout. Containment is the invariant;
// equal lane sizes never were.
static_assert(kGuestHighBase >= kGuestLowAliasBase &&
              kGuestHighBase < kGuestHighEnd &&
              kGuestHighEnd <= kGuestAliasBlockEnd,
              "the identity lane must lie inside the alias base's own block");
static_assert((kGuestHighBase & kGuestLowAliasBase) == kGuestLowAliasBase,
              "high lane base must already contain the alias base bits");
static_assert(((kGuestHighEnd - 1) & kGuestLowAliasBase) == kGuestLowAliasBase,
              "high lane end must already contain the alias base bits");

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
//
// How far DOWN the arena may reach is the other half of one decision. Its host
// image is kGuestTopBase - kGuestTopClearMask, so the base and the image move
// together, and the image has to land in the alias block, above the low alias
// window and above the identity lane. The last of those is the binding one:
// the arena's base and kGuestHighEnd are the SAME NUMBER, related by the mask
// (kGuestTopBase == (kGuestTopClearMask | kGuestTopHostBase)), so widening the
// arena narrows the identity lane byte for byte. What the two share is fixed:
// the block, less the low alias window, less the 64 KiB that the arena's fixed
// top end leaves unused above its image -- 24 GiB less 64 KiB, and no more,
// whatever the host would be willing to give.
//
// The arena was 32 MiB of that, which is why the device log shows Wine's
// try_map_free_area walking down from its own user-space limit and running out
// 320 KiB below the old base. It is 2 GiB now and the identity lane has the
// other 22. Neither figure is one Wine asked for; they are a split, and the
// only way to know whether it is the right one is the witness in
// kmemory64.cpp, which now says which lane a refusal fell outside of.
inline constexpr std::uint64_t kGuestTopBase = 0x7FFF80000000ULL;
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
static_assert(((kGuestAliasBlockEnd - 1) & kGuestTopClearMask) == 0,
              "no address in the alias block may touch the clear mask");
static_assert(kGuestTopHostBase >= kGuestHighEnd,
              "the top host alias must sit above the identity lane");
// The arena's translation is exact and injective for the same reason the
// identity lane's is. For g in [kGuestTopBase, kGuestTopEnd) every mask bit
// is set, so g & ~mask == g - kGuestTopClearMask, which sweeps
// [kGuestTopHostBase, kGuestTopHostEnd) one address for one. Requiring that
// image to lie inside the alias block makes
// (g | base) & ~mask == (g & ~mask) | base == g & ~mask: the OR contributes
// nothing, the subtraction is the whole translation, and no two arena
// addresses can collide.
static_assert(kGuestTopHostBase >= kGuestLowAliasBase &&
              kGuestTopHostEnd <= kGuestAliasBlockEnd,
              "the relocated arena's host image must lie inside the alias "
              "block, or clearing the mask would not be exact");
static_assert(kGuestTopHostBase == kGuestHighEnd,
              "the identity lane must end exactly where the arena's host "
              "block begins, so the block carries no unreachable gap");
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

// ---------------------------------------------------------------------------
// What Wine actually reserves here, and where that comes from.
//
// No preloader runs in this configuration. The packaged wine64 layer carries
// no wine64-preloader -- the runtime validator never checked for one, and our
// own /usr/lib/x86_64-linux-gnu/wine/wine-preloader is a guest link created
// only when that path is ABSENT, pointing at a name the archive does not
// contain. Every device log shows the consequence: an execve of
// wine-preloader with argc N, then immediately an execve of the loader itself
// with argc N-1 and no BOXEDWINE_X64_EXEC line in between, which is Wine's
// loader/main.c preloader_exec() falling back to execv(argv[1], argv + 1)
// after the first exec fails.
//
// So wine_main_preload_info stays null and ntdll's mmap_init takes its
// no-preloader branch (dlls/ntdll/unix/virtual.c, Wine 9):
//
//     if (preload_info) return;
//     /* if we don't have a preloader, try to reserve the space now */
//     reserve_area( (void *)0x000000010000, (void *)0x000068000000 );
//     reserve_area( (void *)0x00007f000000, (void *)0x00007fff0000 );
//     reserve_area( (void *)0x7ffffe000000, (void *)0x7fffffff0000 );
//
// Those three ranges are the whole arena. Nothing else contributes to it:
// WINEPRELOADRESERVE is parsed by the preloader that never runs, and ntdll
// uses it only to EXCLUDE a range from its searches, in
// find_reserved_free_area_outside_preloader -- it never adds a reserved area.
// An environment variable therefore cannot hand Wine an arena inside our
// identity lane, which is why the launch does not try.
//
// reserve_area halves recursively rather than failing, so a range this
// address space does not hand over WHOLE is not refused; it is silently
// shrunk, and the guest quietly runs with less arena than it asked for. That
// is the failure mode these assertions exist to prevent, and the reason the
// mmap path names each of these ranges in the log when the guest asks for it.
struct GuestWineArenaRange {
    std::uint64_t base;
    std::uint64_t end;
    const char* name;
};

inline constexpr GuestWineArenaRange kWineArenaRanges[3] = {
    {0x000000010000ULL, 0x000068000000ULL, "dos-low"},
    {0x00007f000000ULL, 0x00007fff0000ULL, "teb"},
    {0x7ffffe000000ULL, 0x7fffffff0000ULL, "top-down"},
};

// Named on its own because the loader has to keep out of it: it is the only
// one of the three that a guest layout of ours has ever collided with.
inline constexpr std::uint64_t kWineArenaTopDownBase = 0x7ffffe000000ULL;
inline constexpr std::uint64_t kWineArenaTopDownEnd = 0x7fffffff0000ULL;

static_assert(guestRangeHostable(kWineArenaRanges[0].base,
                                 kWineArenaRanges[0].end -
                                     kWineArenaRanges[0].base),
              "ntdll reserves the DOS/low area without a preloader; this "
              "address space has to be able to host all of it");
static_assert(guestRangeHostable(kWineArenaRanges[1].base,
                                 kWineArenaRanges[1].end -
                                     kWineArenaRanges[1].base),
              "ntdll reserves the sub-2-GiB TEB area without a preloader");
static_assert(guestRangeHostable(kWineArenaRanges[2].base,
                                 kWineArenaRanges[2].end -
                                     kWineArenaRanges[2].base),
              "ntdll reserves the top-down arena without a preloader; it must "
              "lie inside the relocated top lane");
static_assert(kWineArenaTopDownBase == kWineArenaRanges[2].base &&
                  kWineArenaTopDownEnd == kWineArenaRanges[2].end,
              "the named top-down bounds must be the reserved range itself");
static_assert(kWineArenaTopDownBase >= kGuestTopBase &&
                  kWineArenaTopDownEnd <= kGuestTopEnd,
              "the top-down arena must fit inside the lane that serves it");

// True when [address, address+length) lies inside one of the ranges ntdll
// reserves for itself. Wine halves each range on refusal, so subranges are
// asked for too and must be recognised.
inline constexpr int guestWineArenaRangeIndex(std::uint64_t address,
                                              std::uint64_t length) noexcept {
    return (length == 0 || address > UINT64_MAX - length)
               ? -1
               : (address >= kWineArenaRanges[0].base &&
                          address + length <= kWineArenaRanges[0].end
                      ? 0
                      : (address >= kWineArenaRanges[1].base &&
                                 address + length <= kWineArenaRanges[1].end
                             ? 1
                             : (address >= kWineArenaRanges[2].base &&
                                        address + length <=
                                            kWineArenaRanges[2].end
                                    ? 2
                                    : -1)));
}

// ---------------------------------------------------------------------------
// The ceiling that actually binds, which is not the size of the arena.
//
// The arena above is 1.7 GiB and the low lane hosts eight, so the obvious
// reading of
//
//     err:virtual:allocate_virtual_memory out of memory for allocation,
//                                          base (nil) size 48d30000
//
// is that Wine ran out of arena and a larger one would fix it. It would not,
// and the reason belongs here because the change that follows from it is not a
// change to any of the three ranges.
//
// That allocation came from a 32-bit image running under WoW64, and every
// allocation the 32-bit half makes arrives at ntdll carrying an explicit
// ceiling. dlls/wow64/syscall.c reads the process's own HighestUserAddress
// once and keeps it:
//
//     highest_user_address = (ULONG_PTR)info.HighestUserAddress;
//     default_zero_bits    = (ULONG_PTR)info.HighestUserAddress | 0x7fffffff;
//
// wow64_NtAllocateVirtualMemory passes get_zero_bits(zero_bits), which
// substitutes default_zero_bits whenever the caller asked for none, and ntdll
// turns that straight back into limit_high through get_zero_bits_limit().
// What HighestUserAddress is comes from virtual_set_large_address_space():
//
//     if (is_wow64())
//         user_space_wow_limit = ((main_image_info.ImageCharacteristics &
//                                  IMAGE_FILE_LARGE_ADDRESS_AWARE)
//                                 ? limit_4g : limit_2g) - 1;
//
// So the IMAGE decides: 2 GiB for an ordinary 32-bit PE, 4 GiB for one linked
// large-address-aware. That is Windows' rule faithfully implemented, not a
// Wine limitation, and no reservation anyone could hand Wine reaches past it.
//
// Under the 2 GiB ceiling the entire space a 32-bit process can allocate from
// is [0x10000, 0x80000000), and Wine partitions it into two halves that cannot
// be combined. map_view tries map_reserved_area first, which can only place a
// view INSIDE a range mmap_add_reserved_area knows about; failing that it
// tries map_free_area, which probes with MAP_FIXED_NOREPLACE and can therefore
// only place a view OUTSIDE those ranges, because a reserved range is a real
// PROT_NONE mapping and answers EEXIST. So the largest single allocation an
// ordinary 32-bit process can obtain is the larger of
//
//     the longest free run inside [0x10000, 0x68000000)     -- 1.62 GiB
//     the longest free run inside [0x68000000, 0x7f000000)  --  368 MiB
//
// and 0x48d30000 is 1.14 GiB. Merging those two -- reserving up to 0x7f000000
// rather than 0x68000000 -- would raise the first to 1.98 GiB, and that is the
// most an arena change can ever be worth to a 32-bit process. It also cannot
// be done from this side: mmap_init's constants are reachable only through a
// preloader, and Wine's own preloader reserves the same addresses
// (loader/preloader.c, { 0x10000, 0x100000 }, { 0x110000, 0x67ef0000 },
// { 0x7f000000, 0xff0000 }, { 0x7ffffe000000, 0x1ff0000 }), so it would take a
// patched ntdll or a patched preloader to move them at all.
//
// Raising the image's own ceiling to 4 GiB is worth more and costs a linker
// flag. [0x80000000, 0x100000000) is two contiguous gigabytes that no
// reservation covers, that nothing in this address space places anything in,
// and that the low lane already hosts: Wine's map_free_area walks straight
// into it as soon as the image stops declaring that it cannot be there.
//
// A 64-bit image is not in this band at all. Its allocations carry no ceiling,
// map_view falls through to an unhinted anon_mmap_alloc, and that draws from
// the identity lane's mmap region -- 21.5 GiB, none of it arena. This is why
// only the WoW64 lane has ever produced this error.
inline constexpr std::uint64_t kWow64LimitDefault = 0x80000000ULL;
inline constexpr std::uint64_t kWow64LimitLargeAddressAware = 0x100000000ULL;

// The lowest address Wine will place a view at (address_space_start keeps the
// DOS area free), and therefore where both the reservations and the census
// bands below begin.
inline constexpr std::uint64_t kGuestLowBandStart = 0x10000ULL;

static_assert(kWow64LimitDefault < kWow64LimitLargeAddressAware,
              "the large-address-aware ceiling is the higher of the two");
static_assert(kWow64LimitLargeAddressAware <= kGuestLowLimit,
              "both WoW64 ceilings must lie inside the low alias window, or "
              "raising an image's ceiling hands it addresses this address "
              "space would then refuse");
static_assert(guestRangeHostable(kGuestLowBandStart,
                                 kWow64LimitLargeAddressAware -
                                     kGuestLowBandStart),
              "the whole 4 GiB a large-address-aware 32-bit image allocates "
              "from has to be hostable in one lane");
static_assert(kWineArenaRanges[0].end < kWow64LimitDefault,
              "the DOS/low reservation is a fraction of the 2 GiB an ordinary "
              "32-bit image addresses; the rest of that band is reachable "
              "only through map_free_area, which cannot enter a reservation");
static_assert(kWineArenaRanges[1].end <= kWow64LimitDefault,
              "so is the sub-2-GiB TEB reservation");
static_assert(kWineArenaRanges[2].base >= kWow64LimitLargeAddressAware,
              "the top-down arena is above every 32-bit ceiling; it belongs "
              "to the 64-bit half and must not be counted against them");

// The bands the address-space census reports. The first three are the ranges
// ntdll reserves; the last two are not reservations at all but the two
// ceilings above, present so a log says how much of what the process could
// have used was actually in use. They deliberately overlap the first two.
inline constexpr GuestWineArenaRange kGuestCensusBands[5] = {
    {kWineArenaRanges[0].base, kWineArenaRanges[0].end, "dos-low"},
    {kWineArenaRanges[1].base, kWineArenaRanges[1].end, "teb"},
    {kWineArenaRanges[2].base, kWineArenaRanges[2].end, "top-down"},
    {kGuestLowBandStart, kWow64LimitDefault, "wow-2g"},
    {kGuestLowBandStart, kWow64LimitLargeAddressAware, "wow-4g"},
};

// ---------------------------------------------------------------------------
// Wine's builtin PE image bases, which are NOT the arena and must not be
// mistaken for it.
//
// A device log is full of
//
//     err:virtual:map_fixed_area out of memory for 0x6fffffc50000-0x6ffffffeb000
//
// and the address looks like a top-arena address with one nibble changed --
// 0x7fffffc50000 with the 7 turned into a 6. It is not. 0x6fffffc50000 is
// ntdll.dll's link-time ImageBase, and the same address appears in an
// interpreted process as a FILE-backed mapping naming
// .../x86_64-windows/ntdll.dll section by section at file offsets 0, 0x1000,
// 0x6d000 and so on. Every failing range is exactly one module long and they
// pack downwards from just under 0x700000000000, which is how Wine assigns
// builtin bases. The twin address 0x7fffffc50000 appears nowhere in any of
// those logs. Nothing truncates or rewrites anything: Wine asks for its own
// image bases, this address space cannot host them, and Wine relocates the
// module, which is what the ERR precedes.
//
// The band is stated here because admitting it would be a real bug rather
// than a fix. Its addresses have SOME of the clear-mask bits set and not
// others, so the two-instruction translation folds them onto the top arena's
// host image: the assertion below is that ntdll's own base and its
// mask-completed twin translate to the SAME host address. Serving both lanes
// at once would alias two different guest pages onto one host page. A third
// lane is not reachable by adding a case here; it needs a translation the
// ARM64 backend can emit without touching NZCV, and this one cannot express
// it.
inline constexpr std::uint64_t kWineBuiltinNtdllImageBase = 0x6FFFFFC50000ULL;
inline constexpr std::uint64_t kWineBuiltinImageBandBase = 0x6FFF00000000ULL;
inline constexpr std::uint64_t kWineBuiltinImageBandEnd = 0x700000000000ULL;

static_assert(kWineBuiltinNtdllImageBase >= kWineBuiltinImageBandBase &&
                  kWineBuiltinNtdllImageBase < kWineBuiltinImageBandEnd,
              "the observed builtin base must lie in the band that names it");
static_assert(!guestRangeHostable(kWineBuiltinImageBandBase,
                                  kWineBuiltinImageBandEnd -
                                      kWineBuiltinImageBandBase),
              "the builtin image band is outside every lane; Wine relocates");
static_assert(guestToHostAddress(kWineBuiltinNtdllImageBase) ==
                  guestToHostAddress(kWineBuiltinNtdllImageBase |
                                     kGuestTopClearMask),
              "a builtin image base and its mask-completed twin translate to "
              "one host address: admitting the band would alias it onto the "
              "top arena, which is why the refusal is correct");
static_assert((kWineBuiltinNtdllImageBase | kGuestTopClearMask) >
                  kGuestTopBase,
              "the twin the log does not contain would be a top-arena address");
// And the corollary, which is how the twin got into an OLDER log at all. A
// host address inside the arena's block canonicalises to the mask-SET form,
// so a 0x6ffff... guest address that ever reached a host-side report would be
// printed as its 0x7ffff... twin. Nothing maps there -- the range is refused
// before it can -- but two logs can spell the same bytes both ways, and a
// reader comparing them has to know that before concluding that something
// truncated an address.
static_assert(hostToGuestAddress(
                  guestToHostAddress(kWineBuiltinNtdllImageBase)) ==
                  (kWineBuiltinNtdllImageBase | kGuestTopClearMask),
              "a builtin image base round-trips through the alias as its "
              "top-arena twin; the two spellings are one host address");

} // namespace boxedvn

#endif // BOXEDWINE_GUEST_LOW_ALIAS_H
