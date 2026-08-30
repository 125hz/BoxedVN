#include "boxedvn_test.h"
#include "guest_low_alias.h"

#include <cstdint>
#include <map>
#include <vector>

using namespace boxedvn;

// Wine reserves [0x7ffffe000000, 0x7fffffff0000) PROT_NONE with
// MAP_FIXED_NOREPLACE and then commits accessible subranges inside it. The
// device log shows three of them -- 0x7fffffdb0000+0x240000,
// 0x7ffffe000000+0x100000 and 0x7fffffc50000+0x39b000 -- every one refused as
// outside the identity lane, followed by an address-space search that reached
// 8,388,609 mmaps at about 98% of a core.
//
// The single OR cannot serve that range: those guest addresses already carry
// the alias base's bits, so the OR is the identity and the host would have to
// map the arena at its own address. The second lane clears one bit field
// instead, which is exact because the field is set in every arena address and
// clear in every other hostable one.

namespace {

// The three commits the device asked for, verbatim.
struct Commit {
    std::uint64_t address;
    std::uint64_t length;
};

constexpr Commit kDeviceCommits[] = {
    {0x7fffffdb0000ULL, 0x240000ULL},
    {0x7ffffe000000ULL, 0x100000ULL},
    {0x7fffffc50000ULL, 0x39b000ULL},
};

// The reservation itself.
constexpr std::uint64_t kReservationLength = 0x1ff0000ULL;

} // namespace

BOXEDVN_TEST(top_alias_covers_the_reservation_the_device_asked_for) {
    CHECK(kGuestTopBase == 0x7ffffe000000ULL);
    CHECK(kGuestTopEnd == 0x7fffffff0000ULL);
    CHECK(kGuestTopLength == kReservationLength);
    CHECK(guestRangeHostable(kGuestTopBase, kReservationLength));
}

BOXEDVN_TEST(top_alias_hosts_every_commit_the_device_asked_for) {
    // All three were refused before. Each has to be hostable, and each has to
    // land inside the host block rather than at its canonical address.
    for (const Commit& commit : kDeviceCommits) {
        CHECK(guestRangeHostable(commit.address, commit.length));
        CHECK(isTopArenaGuestAddress(commit.address));
        CHECK(isTopArenaGuestAddress(commit.address + commit.length - 1));
        const std::uint64_t host = guestToHostAddress(commit.address);
        CHECK(isTopArenaHostAddress(host));
        CHECK(host == kGuestTopHostBase + (commit.address - kGuestTopBase));
        // Contiguous: the whole commit maps to a contiguous host run.
        CHECK(guestToHostAddress(commit.address + commit.length - 1) ==
              host + commit.length - 1);
    }
}

BOXEDVN_TEST(top_alias_translates_all_three_address_categories) {
    // Canonical low: the OR is addition.
    CHECK(guestToHostAddress(0) == kGuestLowAliasBase);
    CHECK(guestToHostAddress(0x7ffe0308ULL) == kGuestLowAliasBase + 0x7ffe0308ULL);
    CHECK(guestToHostAddress(0x140000000ULL) ==
          kGuestLowAliasBase + 0x140000000ULL);
    CHECK(guestToHostAddress(kGuestLowLimit - 1) == kGuestLowAliasEnd - 1);

    // The high lane: the identity.
    CHECK(guestToHostAddress(kGuestHighBase) == kGuestHighBase);
    CHECK(guestToHostAddress(kGuestHighEnd - 1) == kGuestHighEnd - 1);
    CHECK(guestToHostAddress(kGuestHighBase + 0x123456ULL) ==
          kGuestHighBase + 0x123456ULL);

    // The top arena: relocated into its own block.
    CHECK(guestToHostAddress(kGuestTopBase) == kGuestTopHostBase);
    CHECK(guestToHostAddress(kGuestTopEnd - 1) == kGuestTopHostEnd - 1);
    CHECK(guestToHostAddress(kGuestTopBase + 0x1000ULL) ==
          kGuestTopHostBase + 0x1000ULL);
}

BOXEDVN_TEST(top_alias_translation_is_the_two_instructions_the_jit_emits) {
    // The emulator and the translator must not merely agree in intent: they
    // have to compute the same bits. This is the exact ARM64 pair --
    // `orr Xd, Xn, #kGuestLowAliasBase` then `bic Xd, Xd, #kGuestTopClearMask`.
    auto emitted = [](std::uint64_t guest) {
        return (guest | kGuestLowAliasBase) & ~kGuestTopClearMask;
    };
    const std::uint64_t probes[] = {
        0, 1, 0xFFFULL, 0x7ffe0308ULL, 0x140000000ULL, 0x170000000ULL,
        kGuestLowLimit - 1, kGuestHighBase, kGuestHighBase + 0x40000000ULL,
        kGuestHighEnd - 1, kGuestTopBase, kGuestTopBase + 0x1000ULL,
        kGuestTopBase + 0xFFFFFULL, kGuestTopEnd - 1,
        0x7fffffdb0000ULL, 0x7fffffc50000ULL,
    };
    for (std::uint64_t guest : probes) {
        CHECK(emitted(guest) == guestToHostAddress(guest));
    }
    // And across the whole arena at page granularity.
    for (std::uint64_t offset = 0; offset < kGuestTopLength;
         offset += 0x1000ULL) {
        CHECK(emitted(kGuestTopBase + offset) ==
              kGuestTopHostBase + offset);
    }
}

BOXEDVN_TEST(top_alias_selector_bits_separate_the_lanes) {
    // The relocation is only exact because the cleared field is set in every
    // arena address and clear in every other hostable one. If that ever stops
    // being true the translation silently corrupts pointers, so it is asserted
    // rather than assumed.
    CHECK((kGuestTopBase & kGuestTopClearMask) == kGuestTopClearMask);
    CHECK(((kGuestTopEnd - 1) & kGuestTopClearMask) == kGuestTopClearMask);
    for (std::uint64_t offset = 0; offset < kGuestTopLength;
         offset += 0x10000ULL) {
        CHECK(((kGuestTopBase + offset) & kGuestTopClearMask) ==
              kGuestTopClearMask);
    }
    CHECK((0ULL & kGuestTopClearMask) == 0);
    CHECK(((kGuestLowLimit - 1) & kGuestTopClearMask) == 0);
    CHECK(((kGuestLowAliasEnd - 1) & kGuestTopClearMask) == 0);
    CHECK(((kGuestHighEnd - 1) & kGuestTopClearMask) == 0);
}

BOXEDVN_TEST(top_alias_does_not_overlap_the_other_lanes) {
    // Three host lanes, and they have to tile without touching.
    CHECK(kGuestLowAliasEnd == kGuestHighBase);
    CHECK(kGuestTopHostBase >= kGuestHighEnd);
    CHECK(kGuestTopHostEnd > kGuestTopHostBase);
    // No host address belongs to two lanes.
    CHECK(!isTopArenaHostAddress(kGuestLowAliasBase));
    CHECK(!isTopArenaHostAddress(kGuestLowAliasEnd - 1));
    CHECK(!isTopArenaHostAddress(kGuestHighBase));
    CHECK(!isTopArenaHostAddress(kGuestHighEnd - 1));
    CHECK(isTopArenaHostAddress(kGuestTopHostBase));
    CHECK(isTopArenaHostAddress(kGuestTopHostEnd - 1));
    CHECK(!isTopArenaHostAddress(kGuestTopHostEnd));
    // And the host block cannot collide with the canonical arena it serves.
    CHECK(kGuestTopHostEnd <= kGuestTopBase);
}

BOXEDVN_TEST(top_alias_round_trips_without_confusing_the_lanes) {
    // A fault address of unknown provenance is canonicalised through this, so
    // the two alias windows must be distinguishable from each other and from a
    // valid identity-lane guest address.
    CHECK(hostToGuestAddress(guestToHostAddress(0)) == 0);
    CHECK(hostToGuestAddress(guestToHostAddress(0x7ffe0308ULL)) == 0x7ffe0308ULL);
    CHECK(hostToGuestAddress(guestToHostAddress(kGuestTopBase)) == kGuestTopBase);
    CHECK(hostToGuestAddress(guestToHostAddress(kGuestTopEnd - 1)) ==
          kGuestTopEnd - 1);
    for (const Commit& commit : kDeviceCommits) {
        CHECK(hostToGuestAddress(guestToHostAddress(commit.address)) ==
              commit.address);
    }
    // The identity lane belongs to neither window and is returned unchanged.
    CHECK(hostToGuestAddress(kGuestHighBase) == kGuestHighBase);
    CHECK(hostToGuestAddress(kGuestHighEnd - 1) == kGuestHighEnd - 1);
    // An address in neither window is left alone.
    CHECK(hostToGuestAddress(0x100000000000ULL) == 0x100000000000ULL);
}

BOXEDVN_TEST(top_alias_rejects_boundary_and_overflow_ranges) {
    // A range that straddles the arena boundary would need two host lanes for
    // one mapping, so it is refused rather than split.
    CHECK(!guestRangeHostable(kGuestTopBase - 0x1000ULL, 0x2000ULL));
    CHECK(!guestRangeHostable(kGuestTopEnd - 0x1000ULL, 0x2000ULL));
    CHECK(!guestRangeHostable(kGuestHighEnd - 0x1000ULL, 0x2000ULL));
    CHECK(!guestRangeHostable(kGuestLowLimit - 0x1000ULL, 0x2000ULL));
    // Above the arena is not hostable at all.
    CHECK(!guestRangeHostable(kGuestTopEnd, 0x1000ULL));
    CHECK(!guestRangeHostable(0x800000000000ULL, 0x1000ULL));
    // Between the identity lane and the arena is a hole.
    CHECK(!guestRangeHostable(kGuestHighEnd, 0x1000ULL));
    CHECK(!guestRangeHostable(0x7F0000000000ULL, 0x1000ULL));
    // Degenerate and wrapping ranges.
    CHECK(!guestRangeHostable(kGuestTopBase, 0));
    CHECK(!guestRangeHostable(kGuestTopBase, ~(std::uint64_t)0));
    CHECK(!guestRangeHostable(~(std::uint64_t)0, 2));
    // Exactly the arena, and exactly one page at each end, are hostable.
    CHECK(guestRangeHostable(kGuestTopBase, kGuestTopLength));
    CHECK(guestRangeHostable(kGuestTopBase, 0x1000ULL));
    CHECK(guestRangeHostable(kGuestTopEnd - 0x1000ULL, 0x1000ULL));
}

BOXEDVN_TEST(top_alias_leaves_the_existing_lanes_unchanged) {
    // The whole point of clearing a field the other lanes never touch: adding
    // this must not have moved anything that already worked.
    CHECK(kGuestLowLimit == 0x200000000ULL);
    CHECK(kGuestLowAliasBase == 0x7800000000ULL);
    CHECK(kGuestLowAliasEnd == 0x7A00000000ULL);
    CHECK(kGuestHighBase == 0x7A00000000ULL);
    CHECK(kGuestHighEnd == 0x7C00000000ULL);
    CHECK(guestRangeHostable(0, kGuestLowLimit));
    CHECK(guestRangeHostable(kGuestHighBase, kGuestLowLimit));
    CHECK(isLowAliasedGuestAddress(0x7ffe0308ULL));
    CHECK(!isLowAliasedGuestAddress(kGuestTopBase));
}

// ---------------------------------------------------------------------------
// The address-space behaviour the lane implies, modelled against the same
// rules KMemory64 implements, so reserve/commit/split/release are exercised
// without an address space. The structural contract keeps the two in step.
// ---------------------------------------------------------------------------

namespace {

class HostedRanges {
public:
    // MAP_FIXED_NOREPLACE: exact address, EEXIST when anything is already
    // there, and no relocation ever.
    bool reserveNoReplace(std::uint64_t address, std::uint64_t length) {
        if (!guestRangeHostable(address, length)) return false;
        for (std::uint64_t page = address; page < address + length;
             page += 0x1000ULL) {
            if (pages_.count(page)) return false;
        }
        for (std::uint64_t page = address; page < address + length;
             page += 0x1000ULL) {
            pages_[page] = 0;
        }
        return true;
    }

    // MAP_FIXED: exact address, replacing whatever is there.
    bool mapFixed(std::uint64_t address, std::uint64_t length,
                  std::uint32_t protection) {
        if (!guestRangeHostable(address, length)) return false;
        for (std::uint64_t page = address; page < address + length;
             page += 0x1000ULL) {
            pages_[page] = protection;
        }
        return true;
    }

    bool protect(std::uint64_t address, std::uint64_t length,
                 std::uint32_t protection) {
        if (!guestRangeHostable(address, length)) return false;
        for (std::uint64_t page = address; page < address + length;
             page += 0x1000ULL) {
            if (!pages_.count(page)) return false;
            pages_[page] = protection;
        }
        return true;
    }

    void unmap(std::uint64_t address, std::uint64_t length) {
        for (std::uint64_t page = address; page < address + length;
             page += 0x1000ULL) {
            pages_.erase(page);
        }
    }

    bool mapped(std::uint64_t address) const { return pages_.count(address) != 0; }

    std::uint32_t protectionAt(std::uint64_t address) const {
        auto found = pages_.find(address);
        return found == pages_.end() ? 0xFFFFFFFFu : found->second;
    }

    std::size_t pageCount() const { return pages_.size(); }

private:
    std::map<std::uint64_t, std::uint32_t> pages_;
};

} // namespace

BOXEDVN_TEST(top_alias_reservation_succeeds_once_then_returns_eexist) {
    HostedRanges space;
    CHECK(space.reserveNoReplace(kGuestTopBase, kReservationLength));
    CHECK(space.pageCount() == kReservationLength / 0x1000ULL);
    // The second identical request is EEXIST -- this is what stopped Wine's
    // search last time, and it must still be refused.
    CHECK(!space.reserveNoReplace(kGuestTopBase, kReservationLength));
    // So is any overlapping one.
    CHECK(!space.reserveNoReplace(kGuestTopBase, 0x1000ULL));
    CHECK(!space.reserveNoReplace(kGuestTopEnd - 0x1000ULL, 0x1000ULL));
    CHECK(!space.reserveNoReplace(0x7fffffdb0000ULL, 0x240000ULL));
}

BOXEDVN_TEST(top_alias_map_fixed_replaces_a_subrange_of_the_reservation) {
    HostedRanges space;
    CHECK(space.reserveNoReplace(kGuestTopBase, kReservationLength));
    for (const Commit& commit : kDeviceCommits) {
        CHECK(space.mapFixed(commit.address, commit.length, 0x3));
        CHECK(space.protectionAt(commit.address) == 0x3);
        CHECK(space.protectionAt(commit.address + commit.length - 0x1000ULL) ==
              0x3);
    }
    // The reservation around the commits is untouched: still present, still
    // inaccessible.
    CHECK(space.mapped(kGuestTopBase + 0x100000ULL));
    CHECK(space.protectionAt(kGuestTopBase + 0x100000ULL) == 0);
    CHECK(space.pageCount() == kReservationLength / 0x1000ULL);
}

BOXEDVN_TEST(top_alias_mprotect_and_munmap_operate_on_the_translated_range) {
    HostedRanges space;
    CHECK(space.reserveNoReplace(kGuestTopBase, kReservationLength));
    CHECK(space.mapFixed(kGuestTopBase, 0x100000ULL, 0x3));
    // mprotect narrows to read-only.
    CHECK(space.protect(kGuestTopBase, 0x100000ULL, 0x1));
    CHECK(space.protectionAt(kGuestTopBase) == 0x1);
    // Whatever the guest asked for, the host operation has to happen on the
    // translated address; the guest never sees it.
    CHECK(guestToHostAddress(kGuestTopBase) == kGuestTopHostBase);
    CHECK(guestToHostAddress(kGuestTopBase + 0xFF000ULL) ==
          kGuestTopHostBase + 0xFF000ULL);
    // mprotect on an unmapped page inside the arena fails rather than creating.
    space.unmap(kGuestTopBase, kReservationLength);
    CHECK(!space.protect(kGuestTopBase, 0x1000ULL, 0x3));
}

BOXEDVN_TEST(top_alias_splitting_preserves_the_surrounding_reservation) {
    HostedRanges space;
    CHECK(space.reserveNoReplace(kGuestTopBase, kReservationLength));
    // Punch a hole through the middle.
    const std::uint64_t hole = kGuestTopBase + 0x800000ULL;
    space.unmap(hole, 0x10000ULL);
    CHECK(!space.mapped(hole));
    CHECK(space.mapped(hole - 0x1000ULL));
    CHECK(space.mapped(hole + 0x10000ULL));
    CHECK(space.pageCount() == (kReservationLength - 0x10000ULL) / 0x1000ULL);
    // The hole is reusable, and only the hole.
    CHECK(space.reserveNoReplace(hole, 0x10000ULL));
    CHECK(!space.reserveNoReplace(hole - 0x1000ULL, 0x2000ULL));
}

BOXEDVN_TEST(top_alias_refuses_ranges_it_cannot_host) {
    HostedRanges space;
    // Straddling the arena boundary, and entirely outside it.
    CHECK(!space.reserveNoReplace(kGuestTopBase - 0x1000ULL, 0x2000ULL));
    CHECK(!space.reserveNoReplace(kGuestTopEnd, 0x1000ULL));
    CHECK(!space.mapFixed(kGuestTopEnd, 0x1000ULL, 0x3));
    CHECK(space.pageCount() == 0);
    // Inside the identity lane nothing changed.
    CHECK(space.reserveNoReplace(kGuestHighBase, 0x1000ULL));
}
