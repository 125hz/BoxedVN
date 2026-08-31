#include "boxedvn_test.h"
#include "guest_mmap_placement.h"

#include <cstdint>
#include <map>
#include <vector>

using namespace boxedvn;

// Wine reserves multi-gigabyte inaccessible arenas at high addresses the
// native identity window cannot host. Refusing them made Wine halve the
// request and try again without end: one device run issued 8,916,993 mmaps,
// rejected 8,916,842 of them, and burned about 98% of a core doing it.
//
// The decision belongs here; the interval bookkeeping it implies is modelled
// below against the same rules KMemory64 implements, so the split/merge cases
// are exercised without an address space.

namespace {

// The first request the device log shows, at prot=0 with
// MAP_FIXED_NOREPLACE.
constexpr std::uint64_t kWineReservation = 0x7ffffe000000ULL;

GuestMmapRequest highReservation(std::uint64_t address, std::uint64_t length) {
    GuestMmapRequest request;
    request.address = address;
    request.length = length;
    request.protection = 0;
    request.fixedNoReplace = true;
    request.nativeIdentity = true;
    request.exactRangeAllowed = false;  // outside the identity window
    request.exactRangeFree = true;
    request.exactRangeUnmapped = true;
    request.anonymous = true;
    return request;
}

} // namespace

BOXEDVN_TEST(high_prot_none_no_replace_reservation_is_granted) {
    // The exact request that looped, at the exact address.
    const GuestMmapRequest request =
        highReservation(kWineReservation, 0x1ff0000);
    CHECK(chooseGuestMmapPlacement(request) ==
          GuestMmapPlacement::ReserveSparse);
    // A granted reservation is not a failure: nothing may treat it as one.
    CHECK(!guestMmapPlacementIsFailure(GuestMmapPlacement::ReserveSparse));
}

BOXEDVN_TEST(high_reservation_is_granted_at_every_halving_the_guest_tries) {
    // The device log shows the walk halving from 0x1ff0000 down. Every one of
    // those was rejected; the first must now succeed instead.
    for (std::uint64_t length : {0x1ff0000ULL, 0xff0000ULL, 0x7f0000ULL,
                                 0x3f0000ULL, 0x1000ULL}) {
        CHECK(chooseGuestMmapPlacement(
                  highReservation(kWineReservation, length)) ==
              GuestMmapPlacement::ReserveSparse);
    }
}

BOXEDVN_TEST(an_occupied_high_range_is_still_eexist) {
    // MAP_FIXED_NOREPLACE never replaces. An overlapping second request has to
    // fail, whether the occupant is a page or an earlier reservation.
    GuestMmapRequest request = highReservation(kWineReservation, 0x1ff0000);
    request.exactRangeUnmapped = false;
    CHECK(chooseGuestMmapPlacement(request) == GuestMmapPlacement::FailExists);
}

BOXEDVN_TEST(an_accessible_high_range_is_still_refused) {
    // Anything the guest could read, write or execute needs an identity-
    // mappable address. A sparse reservation has no backing store at all, so
    // it can never stand in for one.
    //
    // The refusal is -ENOMEM, not -EEXIST. The range is empty; what cannot be
    // provided is the address. A caller searching for somewhere to put an
    // accessible mapping reads EEXIST as "this spot is taken" and steps to the
    // next one, and across a region where every address is equally unhostable
    // that never terminates: the device log shows 4,194,305 such requests,
    // 4,193,283 of them outside every hostable lane, 4,194,054 refused, with
    // the guest RIP never leaving the syscall in the libc mmap wrapper.
    for (std::uint32_t protection : {1u, 2u, 3u, 4u, 5u, 7u}) {
        GuestMmapRequest request =
            highReservation(kWineReservation, 0x10000);
        request.protection = protection;
        const GuestMmapPlacement placement = chooseGuestMmapPlacement(request);
        CHECK(placement == GuestMmapPlacement::FailNoMemory);
        CHECK(placement != GuestMmapPlacement::FailExists);
        CHECK(placement != GuestMmapPlacement::ReserveSparse);
        CHECK(guestMmapPlacementIsFailure(placement));
    }
}

BOXEDVN_TEST(an_accessible_high_range_refusal_never_moves_with_the_address) {
    // Every step of a search across the unhostable region gets the same
    // answer, so the search cannot be encouraged to continue by any of them.
    for (std::uint64_t step = 0; step < 32; ++step) {
        GuestMmapRequest request = highReservation(
            kWineReservation + step * 0x10000ULL, 0x10000);
        request.protection = 3;
        CHECK(chooseGuestMmapPlacement(request) ==
              GuestMmapPlacement::FailNoMemory);
    }
}

BOXEDVN_TEST(a_file_backed_high_reservation_is_refused) {
    // A file mapping has contents to read; there is nothing to reserve. The
    // range is still empty, so the refusal is -ENOMEM for the same reason.
    GuestMmapRequest request = highReservation(kWineReservation, 0x10000);
    request.anonymous = false;
    CHECK(chooseGuestMmapPlacement(request) ==
          GuestMmapPlacement::FailNoMemory);
}

BOXEDVN_TEST(a_reservation_inside_the_native_window_is_mapped_normally) {
    // Inside the window nothing changes: the range is representable, so it is
    // mapped for real rather than recorded as metadata.
    GuestMmapRequest request = highReservation(0x7048000000ULL, 0x10000);
    request.exactRangeAllowed = true;
    CHECK(chooseGuestMmapPlacement(request) ==
          GuestMmapPlacement::MapExactNoReplace);
}

BOXEDVN_TEST(sparse_reservations_never_apply_without_the_no_replace_flag) {
    // A bare hint may be placed anywhere, and MAP_FIXED destroys. Neither is a
    // reservation the guest asked to own at one exact address.
    GuestMmapRequest hint = highReservation(kWineReservation, 0x10000);
    hint.fixedNoReplace = false;
    CHECK(chooseGuestMmapPlacement(hint) != GuestMmapPlacement::ReserveSparse);

    GuestMmapRequest fixed = hint;
    fixed.fixed = true;
    CHECK(chooseGuestMmapPlacement(fixed) != GuestMmapPlacement::ReserveSparse);
}

BOXEDVN_TEST(sparse_reservations_never_apply_in_sparse_address_spaces) {
    // Without native identity every canonical address is mappable, so the
    // ordinary no-replace path applies and nothing is special-cased.
    GuestMmapRequest request = highReservation(kWineReservation, 0x10000);
    request.nativeIdentity = false;
    request.exactRangeAllowed = true;
    CHECK(chooseGuestMmapPlacement(request) ==
          GuestMmapPlacement::MapExactNoReplace);
}

// ---------------------------------------------------------------------------
// The interval bookkeeping the decision implies. This mirrors
// KMemory64::reserveSparseNoReplace / releaseSparseReservation so the
// split/merge/reuse rules are exercised without an address space; the
// structural contract keeps the two in step.
// ---------------------------------------------------------------------------

namespace {

class ReservationIntervals {
public:
    // Returns false for EEXIST, exactly as the address space does.
    bool reserve(std::uint64_t startPage, std::uint64_t pageCount) {
        const std::uint64_t endPage = startPage + pageCount;
        auto it = intervals_.upper_bound(startPage);
        if (it != intervals_.begin()) {
            auto previous = std::prev(it);
            if (previous->first + previous->second > startPage) return false;
        }
        if (it != intervals_.end() && it->first < endPage) return false;

        std::uint64_t mergedStart = startPage;
        std::uint64_t mergedEnd = endPage;
        auto candidate = intervals_.lower_bound(mergedStart);
        if (candidate != intervals_.begin()) {
            auto previous = std::prev(candidate);
            if (previous->first + previous->second == mergedStart) {
                mergedStart = previous->first;
                intervals_.erase(previous);
            }
        }
        auto after = intervals_.find(mergedEnd);
        if (after != intervals_.end()) {
            mergedEnd = after->first + after->second;
            intervals_.erase(after);
        }
        intervals_[mergedStart] = mergedEnd - mergedStart;
        return true;
    }

    bool release(std::uint64_t startPage, std::uint64_t pageCount) {
        const std::uint64_t endPage = startPage + pageCount;
        bool removed = false;
        auto it = intervals_.lower_bound(startPage);
        if (it != intervals_.begin()) --it;
        while (it != intervals_.end() && it->first < endPage) {
            const std::uint64_t start = it->first;
            const std::uint64_t end = start + it->second;
            if (end <= startPage) { ++it; continue; }
            removed = true;
            it = intervals_.erase(it);
            if (start < startPage) intervals_[start] = startPage - start;
            if (end > endPage) { intervals_[endPage] = end - endPage; break; }
        }
        return removed;
    }

    bool overlaps(std::uint64_t startPage, std::uint64_t pageCount) const {
        const std::uint64_t endPage = startPage + pageCount;
        auto it = intervals_.upper_bound(startPage);
        if (it != intervals_.begin()) {
            auto previous = std::prev(it);
            if (previous->first + previous->second > startPage) return true;
        }
        return it != intervals_.end() && it->first < endPage;
    }

    std::size_t count() const { return intervals_.size(); }

    std::uint64_t pages() const {
        std::uint64_t total = 0;
        for (const auto& entry : intervals_) total += entry.second;
        return total;
    }

private:
    std::map<std::uint64_t, std::uint64_t> intervals_;
};

constexpr std::uint64_t kReservationPage = kWineReservation >> 12;

} // namespace

BOXEDVN_TEST(sparse_reservation_costs_one_interval_for_a_huge_range) {
    // The whole point: a 512 MiB reservation is one interval, not 131,072 page
    // objects.
    ReservationIntervals intervals;
    const std::uint64_t pageCount = 0x20000000ULL >> 12;
    CHECK(intervals.reserve(kReservationPage, pageCount));
    CHECK(intervals.count() == 1);
    CHECK(intervals.pages() == pageCount);
}

BOXEDVN_TEST(sparse_reservation_refuses_an_overlapping_request) {
    ReservationIntervals intervals;
    CHECK(intervals.reserve(kReservationPage, 0x100));
    // Identical, contained, straddling the start, straddling the end.
    CHECK(!intervals.reserve(kReservationPage, 0x100));
    CHECK(!intervals.reserve(kReservationPage + 0x10, 0x10));
    CHECK(!intervals.reserve(kReservationPage - 0x10, 0x20));
    CHECK(!intervals.reserve(kReservationPage + 0xF0, 0x20));
    CHECK(intervals.count() == 1);
    // Genuinely disjoint ranges are still granted.
    CHECK(intervals.reserve(kReservationPage + 0x200, 0x10));
}

BOXEDVN_TEST(sparse_reservation_merges_adjacent_intervals) {
    // A walk that reserves neighbouring arenas must not accumulate one
    // interval per call.
    ReservationIntervals intervals;
    for (std::uint64_t index = 0; index < 64; ++index) {
        CHECK(intervals.reserve(kReservationPage + index * 0x10, 0x10));
    }
    CHECK(intervals.count() == 1);
    CHECK(intervals.pages() == 64 * 0x10);
}

BOXEDVN_TEST(sparse_reservation_munmap_splits_an_interval) {
    ReservationIntervals intervals;
    CHECK(intervals.reserve(kReservationPage, 0x100));
    // Unmap a hole through the middle.
    CHECK(intervals.release(kReservationPage + 0x40, 0x20));
    CHECK(intervals.count() == 2);
    CHECK(intervals.pages() == 0x100 - 0x20);
    CHECK(intervals.overlaps(kReservationPage, 0x10));
    CHECK(!intervals.overlaps(kReservationPage + 0x40, 0x20));
    CHECK(intervals.overlaps(kReservationPage + 0x60, 0x10));
}

BOXEDVN_TEST(sparse_reservation_munmap_trims_and_removes) {
    ReservationIntervals intervals;
    CHECK(intervals.reserve(kReservationPage, 0x100));
    // Trim the front.
    CHECK(intervals.release(kReservationPage, 0x10));
    CHECK(intervals.count() == 1);
    CHECK(intervals.pages() == 0xF0);
    // Trim the back.
    CHECK(intervals.release(kReservationPage + 0xF0, 0x10));
    CHECK(intervals.pages() == 0xE0);
    // Remove what is left.
    CHECK(intervals.release(kReservationPage + 0x10, 0xE0));
    CHECK(intervals.count() == 0);
    CHECK(intervals.pages() == 0);
    // Releasing nothing reports nothing.
    CHECK(!intervals.release(kReservationPage, 0x100));
}

BOXEDVN_TEST(sparse_reservation_is_reusable_after_munmap) {
    // The address space has to be genuinely reusable, not merely forgotten.
    ReservationIntervals intervals;
    CHECK(intervals.reserve(kReservationPage, 0x100));
    CHECK(!intervals.reserve(kReservationPage, 0x100));
    CHECK(intervals.release(kReservationPage, 0x100));
    CHECK(intervals.reserve(kReservationPage, 0x100));
    CHECK(intervals.count() == 1);
    // And a hole punched through one can be re-reserved on its own.
    CHECK(intervals.release(kReservationPage + 0x40, 0x20));
    CHECK(intervals.reserve(kReservationPage + 0x40, 0x20));
    CHECK(intervals.count() == 1);  // the three pieces coalesce again
    CHECK(intervals.pages() == 0x100);
}

BOXEDVN_TEST(sparse_reservation_release_spanning_several_intervals) {
    ReservationIntervals intervals;
    CHECK(intervals.reserve(kReservationPage, 0x10));
    CHECK(intervals.reserve(kReservationPage + 0x100, 0x10));
    CHECK(intervals.reserve(kReservationPage + 0x200, 0x10));
    CHECK(intervals.count() == 3);
    CHECK(intervals.release(kReservationPage, 0x210));
    CHECK(intervals.count() == 0);
    CHECK(intervals.pages() == 0);
}

// ---------------------------------------------------------------------------
// The follow-up operations a guest performs inside a reservation it was
// granted. Granting the reservation is only correct if the guest can then do
// the valid things with that range without either being handed an invalid
// host pointer or being told something that makes it retry without end.
// ---------------------------------------------------------------------------

BOXEDVN_TEST(a_commit_inside_a_granted_reservation_is_never_sparse) {
    // Wine commits accessible subranges inside an arena it reserved. A commit
    // is accessible memory, and accessible memory is never represented by a
    // metadata-only reservation whatever the surrounding range is.
    GuestMmapRequest commit = highReservation(kWineReservation, 0x10000);
    commit.protection = 3;
    commit.exactRangeUnmapped = false;  // the reservation occupies it
    commit.exactRangeFree = true;       // but nothing accessible is there
    CHECK(chooseGuestMmapPlacement(commit) != GuestMmapPlacement::ReserveSparse);
    // Occupied by the reservation, so MAP_FIXED_NOREPLACE is EEXIST: that is
    // the truth about the range, and it is what the flag exists to report.
    CHECK(chooseGuestMmapPlacement(commit) == GuestMmapPlacement::FailExists);

    // The same commit as a destructive MAP_FIXED replaces the reservation,
    // which is exactly how a guest commits inside its own arena.
    commit.fixedNoReplace = false;
    commit.fixed = true;
    CHECK(chooseGuestMmapPlacement(commit) == GuestMmapPlacement::MapExact);
    CHECK(chooseGuestMmapPlacement(commit) != GuestMmapPlacement::ReserveSparse);
}

BOXEDVN_TEST(a_hint_inside_a_granted_reservation_is_taken_where_hostable) {
    // A bare hint ignores a PROT_NONE reservation on purpose: Wine maps
    // committed pages at hint addresses inside its own arenas, and relocating
    // those would desynchronise its view tree.
    GuestMmapRequest hint = highReservation(kWineReservation, 0x10000);
    hint.fixedNoReplace = false;
    hint.protection = 3;
    hint.exactRangeUnmapped = false;
    hint.exactRangeFree = true;
    hint.exactRangeAllowed = true;
    CHECK(chooseGuestMmapPlacement(hint) == GuestMmapPlacement::MapExact);
}

BOXEDVN_TEST(a_reservation_re_requested_after_release_is_granted_again) {
    // Releasing the interval is the unmap. The address is then reservable
    // again, and the decision has no memory of the earlier grant.
    const GuestMmapRequest request =
        highReservation(kWineReservation, 0x1ff0000);
    CHECK(chooseGuestMmapPlacement(request) ==
          GuestMmapPlacement::ReserveSparse);
    GuestMmapRequest occupied = request;
    occupied.exactRangeUnmapped = false;
    CHECK(chooseGuestMmapPlacement(occupied) == GuestMmapPlacement::FailExists);
    CHECK(chooseGuestMmapPlacement(request) ==
          GuestMmapPlacement::ReserveSparse);
}

BOXEDVN_TEST(no_refused_probe_ever_reaches_an_allocator) {
    // Neither refusal may consume a high-window allocation: the window is the
    // only region a direct FEX guest can execute from, and a search that
    // probes thousands of addresses would drain it.
    GuestMmapRequest accessible = highReservation(kWineReservation, 0x10000);
    accessible.protection = 3;
    CHECK(guestMmapPlacementIsFailure(chooseGuestMmapPlacement(accessible)));

    GuestMmapRequest occupied = highReservation(kWineReservation, 0x10000);
    occupied.exactRangeUnmapped = false;
    CHECK(guestMmapPlacementIsFailure(chooseGuestMmapPlacement(occupied)));

    GuestMmapRequest probe = highReservation(kWineReservation, 0x10000);
    probe.fixedNoReplace = false;
    CHECK(chooseGuestMmapPlacement(probe) == GuestMmapPlacement::FailNoMemory);
    CHECK(guestMmapPlacementIsFailure(chooseGuestMmapPlacement(probe)));
}

BOXEDVN_TEST(sparse_reservation_release_then_commit_leaves_no_reservation) {
    // munmap through a reservation, then a real mapping in the hole: the hole
    // must be genuinely free, so an exact no-replace mapping in it succeeds.
    ReservationIntervals intervals;
    CHECK(intervals.reserve(kReservationPage, 0x100));
    CHECK(intervals.release(kReservationPage + 0x40, 0x20));
    CHECK(!intervals.overlaps(kReservationPage + 0x40, 0x20));

    GuestMmapRequest commit =
        highReservation((kReservationPage + 0x40) << 12, 0x20000);
    commit.protection = 3;
    commit.exactRangeAllowed = true;   // inside the window after relocation
    commit.exactRangeUnmapped = true;  // the hole really is empty
    CHECK(chooseGuestMmapPlacement(commit) ==
          GuestMmapPlacement::MapExactNoReplace);
}
