#include "boxedvn_test.h"
#include "guest_mmap_placement.h"

#include <cstdint>

using namespace boxedvn;

namespace {

// The proven identity window; anything below it is native-ineligible.
constexpr std::uint64_t kWindowStart = 0x7048000000ULL;
constexpr std::uint64_t kWindowEnd = 0x7fffff0000ULL;

constexpr std::uint32_t kProtNone = 0;
constexpr std::uint32_t kProtReadWrite = 0x1 | 0x2;

bool insideWindow(std::uint64_t address, std::uint64_t length) {
    return address >= kWindowStart && address + length <= kWindowEnd;
}

// Model one request the way the syscall layer builds it, so the tests exercise
// the same field derivation the caller performs rather than a hand-tuned
// struct.
GuestMmapRequest makeRequest(std::uint64_t address, std::uint64_t length,
                             std::uint32_t protection, bool fixed,
                             bool fixedNoReplace, bool nativeIdentity,
                             bool anyAccessiblePage, bool anyMappedPage,
                             bool anonymous = false) {
    GuestMmapRequest request;
    request.address = address;
    request.length = length;
    request.protection = protection;
    request.fixed = fixed;
    request.fixedNoReplace = fixedNoReplace;
    request.nativeIdentity = nativeIdentity;
    request.exactRangeAllowed =
        !nativeIdentity || insideWindow(address, length);
    request.exactRangeFree = !anyAccessiblePage;
    request.exactRangeUnmapped = !anyMappedPage;
    request.anonymous = anonymous;
    return request;
}

} // namespace

BOXEDVN_TEST(guest_mmap_no_replace_takes_a_free_allowed_range) {
    const auto request =
        makeRequest(kWindowStart + 0x10000, 0x2000, kProtReadWrite,
                    /*fixed*/ false, /*fixedNoReplace*/ true,
                    /*nativeIdentity*/ true,
                    /*anyAccessiblePage*/ false, /*anyMappedPage*/ false);
    CHECK(chooseGuestMmapPlacement(request) ==
          GuestMmapPlacement::MapExactNoReplace);
    CHECK(!guestMmapPlacementIsFailure(chooseGuestMmapPlacement(request)));
}

BOXEDVN_TEST(guest_mmap_no_replace_reports_an_occupied_range_as_existing) {
    // A second request overlapping the first must fail rather than relocate or
    // replace, and must not reach any allocator.
    const auto request =
        makeRequest(kWindowStart + 0x10000, 0x2000, kProtReadWrite,
                    /*fixed*/ false, /*fixedNoReplace*/ true,
                    /*nativeIdentity*/ true,
                    /*anyAccessiblePage*/ true, /*anyMappedPage*/ true);
    const auto placement = chooseGuestMmapPlacement(request);
    CHECK(placement == GuestMmapPlacement::FailExists);
    CHECK(guestMmapPlacementIsFailure(placement));
}

BOXEDVN_TEST(guest_mmap_no_replace_counts_a_bare_reservation_as_occupied) {
    // MAP_FIXED_NOREPLACE is defined against total occupancy. A PROT_NONE
    // reservation is mapped even though no page is accessible, so it must
    // block the request; an ordinary hint deliberately ignores it.
    const auto noReplace =
        makeRequest(kWindowStart + 0x20000, 0x1000, kProtReadWrite,
                    /*fixed*/ false, /*fixedNoReplace*/ true,
                    /*nativeIdentity*/ true,
                    /*anyAccessiblePage*/ false, /*anyMappedPage*/ true);
    CHECK(chooseGuestMmapPlacement(noReplace) ==
          GuestMmapPlacement::FailExists);

    const auto hint =
        makeRequest(kWindowStart + 0x20000, 0x1000, kProtReadWrite,
                    /*fixed*/ false, /*fixedNoReplace*/ false,
                    /*nativeIdentity*/ true,
                    /*anyAccessiblePage*/ false, /*anyMappedPage*/ true);
    CHECK(chooseGuestMmapPlacement(hint) == GuestMmapPlacement::MapExact);
}

BOXEDVN_TEST(guest_mmap_no_replace_never_consumes_the_high_window) {
    // A low, native-ineligible exact request cannot be satisfied at the
    // address demanded. It must fail outright: relocating is forbidden by the
    // flag, and drawing from the bounded window would consume the only region
    // the guest can execute from. A file-backed reservation cannot be recorded
    // as metadata either, so it is refused -- with -ENOMEM, because the range
    // is empty and it is the address that cannot be provided.
    const auto request =
        makeRequest(0x10000, 0x67ff0000, kProtNone,
                    /*fixed*/ false, /*fixedNoReplace*/ true,
                    /*nativeIdentity*/ true,
                    /*anyAccessiblePage*/ false, /*anyMappedPage*/ false);
    const auto placement = chooseGuestMmapPlacement(request);
    CHECK(placement == GuestMmapPlacement::FailNoMemory);
    CHECK(placement != GuestMmapPlacement::RelocateHighWindow);
    CHECK(placement != GuestMmapPlacement::ReserveSparse);
    CHECK(guestMmapPlacementIsFailure(placement));
}

BOXEDVN_TEST(guest_mmap_no_replace_on_an_unhostable_free_range_is_enomem) {
    // The exact request the device shows repeating: MAP_FIXED_NOREPLACE,
    // anonymous, accessible, on a completely unmapped range no hostable lane
    // covers. EEXIST would assert an occupant that is not there, and a
    // placement search answers EEXIST by stepping one granule and asking
    // again -- 4,194,305 times in the run that produced this test, with the
    // guest RIP parked on the syscall in the libc mmap wrapper throughout.
    const std::uint32_t accessible[] = {0x1u, 0x2u, 0x3u, 0x4u, 0x5u, 0x7u};
    for (std::uint32_t protection : accessible) {
        const auto request =
            makeRequest(0x300000000ULL, 0x10000, protection,
                        /*fixed*/ false, /*fixedNoReplace*/ true,
                        /*nativeIdentity*/ true,
                        /*anyAccessiblePage*/ false, /*anyMappedPage*/ false,
                        /*anonymous*/ true);
        const auto placement = chooseGuestMmapPlacement(request);
        CHECK(placement == GuestMmapPlacement::FailNoMemory);
        CHECK(placement != GuestMmapPlacement::FailExists);
        CHECK(placement != GuestMmapPlacement::ReserveSparse);
        CHECK(placement != GuestMmapPlacement::RelocateHighWindow);
        CHECK(guestMmapPlacementIsFailure(placement));
    }
}

BOXEDVN_TEST(guest_mmap_no_replace_walk_never_reports_a_phantom_occupant) {
    // A search stepping across the unhostable region must be told the same
    // true thing at every step, and never that the range is occupied. If any
    // step answered EEXIST the walk would continue; ENOMEM ends it at the
    // first probe.
    const std::uint64_t step = 0x10000;
    int probes = 0;
    for (std::uint64_t address = 0x300000000ULL;
         address < 0x300000000ULL + 64 * step; address += step) {
        const auto request =
            makeRequest(address, step, kProtReadWrite, /*fixed*/ false,
                        /*fixedNoReplace*/ true, /*nativeIdentity*/ true,
                        /*anyAccessiblePage*/ false, /*anyMappedPage*/ false,
                        /*anonymous*/ true);
        CHECK(chooseGuestMmapPlacement(request) ==
              GuestMmapPlacement::FailNoMemory);
        ++probes;
    }
    CHECK(probes == 64);
}

BOXEDVN_TEST(guest_mmap_no_replace_still_reports_a_real_occupant_as_existing) {
    // Occupancy is the stronger fact and keeps its own answer, whether the
    // range is hostable or not. Only an EMPTY unhostable range became ENOMEM.
    const auto outside =
        makeRequest(0x300000000ULL, 0x10000, kProtReadWrite, /*fixed*/ false,
                    /*fixedNoReplace*/ true, /*nativeIdentity*/ true,
                    /*anyAccessiblePage*/ false, /*anyMappedPage*/ true,
                    /*anonymous*/ true);
    CHECK(chooseGuestMmapPlacement(outside) == GuestMmapPlacement::FailExists);

    const auto inside =
        makeRequest(kWindowStart + 0x40000, 0x10000, kProtReadWrite,
                    /*fixed*/ false, /*fixedNoReplace*/ true,
                    /*nativeIdentity*/ true,
                    /*anyAccessiblePage*/ false, /*anyMappedPage*/ true,
                    /*anonymous*/ true);
    CHECK(chooseGuestMmapPlacement(inside) == GuestMmapPlacement::FailExists);
}

BOXEDVN_TEST(guest_mmap_sparse_address_space_behaviour_is_unchanged) {
    // A sparse (non-identity) address space hosts every canonical address, so
    // no request in it can reach the identity-window rules at all. This is
    // what the 32-bit guest and every helper process runs with.
    const bool noReplaceChoices[] = {false, true};
    const std::uint32_t protections[] = {kProtNone, kProtReadWrite};
    for (bool fixedNoReplace : noReplaceChoices) {
        for (std::uint32_t protection : protections) {
            const auto request = makeRequest(
                0x10000, 0x67ff0000, protection, /*fixed*/ false,
                fixedNoReplace, /*nativeIdentity*/ false,
                /*anyAccessiblePage*/ false, /*anyMappedPage*/ false,
                /*anonymous*/ true);
            const auto placement = chooseGuestMmapPlacement(request);
            CHECK(placement == (fixedNoReplace
                                    ? GuestMmapPlacement::MapExactNoReplace
                                    : GuestMmapPlacement::MapExact));
            CHECK(!guestMmapPlacementIsFailure(placement));
        }
    }
}

BOXEDVN_TEST(guest_mmap_ordinary_hint_behaviour_is_unchanged) {
    // A usable, free hint is still taken exactly.
    const auto usable =
        makeRequest(kWindowStart + 0x100000, 0x4000, kProtReadWrite,
                    /*fixed*/ false, /*fixedNoReplace*/ false,
                    /*nativeIdentity*/ true,
                    /*anyAccessiblePage*/ false, /*anyMappedPage*/ false);
    CHECK(chooseGuestMmapPlacement(usable) == GuestMmapPlacement::MapExact);

    // An occupied hint still relocates rather than destroying the occupant.
    const auto occupied =
        makeRequest(kWindowStart + 0x100000, 0x4000, kProtReadWrite,
                    /*fixed*/ false, /*fixedNoReplace*/ false,
                    /*nativeIdentity*/ true,
                    /*anyAccessiblePage*/ true, /*anyMappedPage*/ true);
    CHECK(chooseGuestMmapPlacement(occupied) ==
          GuestMmapPlacement::RelocateHighWindow);

    // A low hint for memory the guest will actually use still relocates: that
    // mapping is usable at some address, so refusing it would break the guest.
    const auto lowUsable =
        makeRequest(0x140000000, 0x10000, kProtReadWrite,
                    /*fixed*/ false, /*fixedNoReplace*/ false,
                    /*nativeIdentity*/ true,
                    /*anyAccessiblePage*/ false, /*anyMappedPage*/ false);
    CHECK(chooseGuestMmapPlacement(lowUsable) ==
          GuestMmapPlacement::RelocateHighWindow);

    // No preferred address always goes to the allocator.
    const auto anywhere =
        makeRequest(0, 0x1000, kProtReadWrite, /*fixed*/ false,
                    /*fixedNoReplace*/ false, /*nativeIdentity*/ true,
                    /*anyAccessiblePage*/ false, /*anyMappedPage*/ false);
    CHECK(chooseGuestMmapPlacement(anywhere) ==
          GuestMmapPlacement::RelocateHighWindow);
}

BOXEDVN_TEST(guest_mmap_low_reservation_probes_do_not_drain_the_window) {
    // The reservation walk that produced 53,753 relocations per launch: a
    // recursive halving of the low address space, every probe PROT_NONE and
    // native-ineligible. None of them may allocate.
    std::uint64_t length = 0x67ff0000;
    int probes = 0;
    while (length >= 0x10000) {
        const auto request =
            makeRequest(0x10000, length, kProtNone, /*fixed*/ false,
                        /*fixedNoReplace*/ false, /*nativeIdentity*/ true,
                        /*anyAccessiblePage*/ false, /*anyMappedPage*/ false);
        const auto placement = chooseGuestMmapPlacement(request);
        CHECK(placement == GuestMmapPlacement::FailNoMemory);
        CHECK(placement != GuestMmapPlacement::RelocateHighWindow);
        CHECK(guestMmapPlacementIsFailure(placement));
        length /= 2;
        ++probes;
    }
    CHECK(probes > 10);

    // The same probe near the top of the low space behaves identically.
    const auto high =
        makeRequest(0x7ffd0000, 0x20000, kProtNone, /*fixed*/ false,
                    /*fixedNoReplace*/ false, /*nativeIdentity*/ true,
                    /*anyAccessiblePage*/ false, /*anyMappedPage*/ false);
    CHECK(chooseGuestMmapPlacement(high) == GuestMmapPlacement::FailNoMemory);
}

BOXEDVN_TEST(guest_mmap_reservation_rule_is_scoped_to_native_identity) {
    // Without identity mapping every canonical address is usable, so a low
    // PROT_NONE reservation is taken exactly as the caller asked.
    const auto sparse =
        makeRequest(0x10000, 0x10000, kProtNone, /*fixed*/ false,
                    /*fixedNoReplace*/ false, /*nativeIdentity*/ false,
                    /*anyAccessiblePage*/ false, /*anyMappedPage*/ false);
    CHECK(chooseGuestMmapPlacement(sparse) == GuestMmapPlacement::MapExact);

    // And a low PROT_NONE reservation inside the window is equally fine under
    // identity mapping: the rule keys on the range being unusable, not on the
    // protection alone.
    const auto insideWindowReservation =
        makeRequest(kWindowStart + 0x8000, 0x10000, kProtNone,
                    /*fixed*/ false, /*fixedNoReplace*/ false,
                    /*nativeIdentity*/ true,
                    /*anyAccessiblePage*/ false, /*anyMappedPage*/ false);
    CHECK(chooseGuestMmapPlacement(insideWindowReservation) ==
          GuestMmapPlacement::MapExact);
}

BOXEDVN_TEST(guest_mmap_destructive_fixed_placement_is_preserved) {
    // MAP_FIXED keeps its existing meaning; the address space applies its own
    // native-window guard underneath rather than silently relocating.
    const auto fixedInside =
        makeRequest(kWindowStart + 0x30000, 0x1000, kProtReadWrite,
                    /*fixed*/ true, /*fixedNoReplace*/ false,
                    /*nativeIdentity*/ true,
                    /*anyAccessiblePage*/ true, /*anyMappedPage*/ true);
    CHECK(chooseGuestMmapPlacement(fixedInside) ==
          GuestMmapPlacement::MapExact);

    const auto fixedOutside =
        makeRequest(0x10000, 0x1000, kProtReadWrite, /*fixed*/ true,
                    /*fixedNoReplace*/ false, /*nativeIdentity*/ true,
                    /*anyAccessiblePage*/ false, /*anyMappedPage*/ false);
    CHECK(chooseGuestMmapPlacement(fixedOutside) ==
          GuestMmapPlacement::MapExact);

    // MAP_FIXED_NOREPLACE wins over MAP_FIXED when a caller sets both, which
    // is what Linux does: the no-replace contract is the stricter one.
    const auto both =
        makeRequest(kWindowStart + 0x30000, 0x1000, kProtReadWrite,
                    /*fixed*/ true, /*fixedNoReplace*/ true,
                    /*nativeIdentity*/ true,
                    /*anyAccessiblePage*/ true, /*anyMappedPage*/ true);
    CHECK(chooseGuestMmapPlacement(both) == GuestMmapPlacement::FailExists);
}
