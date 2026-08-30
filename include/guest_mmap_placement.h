/*
 * BoxedWine - placement policy for 64-bit guest mmap requests.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * Linux distinguishes three placements that BoxedWine's 64-bit syscall layer
 * previously collapsed into two:
 *
 *   - MAP_FIXED           exact address, destroying whatever is already there;
 *   - MAP_FIXED_NOREPLACE exact address, but EEXIST rather than destroying;
 *   - a bare hint         advisory, the kernel may place it anywhere.
 *
 * Only MAP_FIXED was recognised, so a non-fixed low hint that the native
 * identity window cannot host was relocated into the bounded high guest
 * window. Wine's address-space reservation walks the whole low 2 GiB by
 * recursive halving, so every probe consumed a fresh high-window reservation:
 * one launch produced 53,753 relocations and drained the only region a direct
 * FEX guest can actually execute from.
 *
 * The decision is isolated here so it can be exercised exhaustively without a
 * process, an address space, or a host mapping. Nothing in this header touches
 * BoxedWine state; callers supply the facts and act on the verdict.
 */

#ifndef BOXEDWINE_GUEST_MMAP_PLACEMENT_H
#define BOXEDWINE_GUEST_MMAP_PLACEMENT_H

#include <cstdint>

namespace boxedvn {

enum class GuestMmapPlacement : std::uint8_t {
    // Map at exactly the requested address, replacing any existing mapping.
    MapExact = 0,
    // Map at exactly the requested address, but only while it is entirely
    // unmapped. The caller must perform the test and the mapping atomically.
    MapExactNoReplace = 1,
    // Let the bounded allocator choose an address inside the guest window.
    RelocateHighWindow = 2,
    // Exact placement was demanded and cannot be provided: -EEXIST. Nothing is
    // mapped, and no allocator cursor advances.
    FailExists = 3,
    // A reservation probe that this address space can never host: -ENOMEM.
    // Nothing is mapped, and no allocator cursor advances.
    FailNoMemory = 4,
};

struct GuestMmapRequest {
    // Page-aligned requested address; zero means "caller has no preference".
    std::uint64_t address = 0;
    // Page-rounded length.
    std::uint64_t length = 0;
    // K_PROT_READ/WRITE/EXEC. Zero is a pure reservation.
    std::uint32_t protection = 0;
    bool fixed = false;
    bool fixedNoReplace = false;
    // True when the address space maps guest addresses onto identical host
    // addresses, which restricts usable guest memory to one proven window.
    bool nativeIdentity = false;
    // nativeGuestRangeAllowed(address, length) for the exact requested range.
    bool exactRangeAllowed = false;
    // No page in the range carries read, write or execute permission. Reserved
    // PROT_NONE pages do not count as occupied: Wine maps committed pages at
    // hint addresses inside its own reservations and relocating those would
    // desynchronise its view tree.
    bool exactRangeFree = false;
    // No page in the range is mapped at all, reservations included. This is the
    // occupancy test MAP_FIXED_NOREPLACE is defined against.
    bool exactRangeUnmapped = false;
};

// Decide where one anonymous or file-backed guest mmap request belongs.
inline GuestMmapPlacement chooseGuestMmapPlacement(
    const GuestMmapRequest& request) noexcept {
    if (request.address == 0) {
        // No preference: the bounded allocator owns the choice.
        return GuestMmapPlacement::RelocateHighWindow;
    }

    if (request.fixedNoReplace) {
        // MAP_FIXED_NOREPLACE never relocates and never destroys. An occupied
        // range is EEXIST even when the occupant is a bare PROT_NONE
        // reservation, which is precisely what the flag exists to detect.
        if (!request.exactRangeUnmapped) {
            return GuestMmapPlacement::FailExists;
        }
        if (request.nativeIdentity && !request.exactRangeAllowed) {
            // Direct execution needs identity-mappable guest addresses, so an
            // address outside the proven window can never be provided AT the
            // address the caller demanded. Relocating is forbidden by the flag,
            // so model it the way Linux models an unavailable exact range:
            // fail, map nothing, and leave the high-window cursor alone.
            return GuestMmapPlacement::FailExists;
        }
        return GuestMmapPlacement::MapExactNoReplace;
    }

    if (request.fixed) {
        // Destructive exact placement keeps its existing behaviour; the address
        // space applies its own native-window guard underneath.
        return GuestMmapPlacement::MapExact;
    }

    // A bare hint. Taking it is only correct when the range is both usable and
    // genuinely free; otherwise Linux is free to place the mapping elsewhere.
    if (request.exactRangeFree && request.exactRangeAllowed) {
        return GuestMmapPlacement::MapExact;
    }

    if (request.nativeIdentity && !request.exactRangeAllowed &&
        request.protection == 0) {
        // A pure reservation probe outside the identity window. Relocating it
        // hands back an address the caller never asked about while permanently
        // consuming part of the only window the guest can execute from, and
        // the caller learns nothing about the address it was probing. Report
        // the range as unavailable instead: a reservation walk then narrows
        // and moves on, exactly as it does against a real kernel that refuses
        // the area. Mappings the guest can actually use keep relocating.
        return GuestMmapPlacement::FailNoMemory;
    }

    return GuestMmapPlacement::RelocateHighWindow;
}

// True when the verdict must not touch the address space or any allocator.
inline bool guestMmapPlacementIsFailure(GuestMmapPlacement placement) noexcept {
    return placement == GuestMmapPlacement::FailExists ||
           placement == GuestMmapPlacement::FailNoMemory;
}

} // namespace boxedvn

#endif // BOXEDWINE_GUEST_MMAP_PLACEMENT_H
