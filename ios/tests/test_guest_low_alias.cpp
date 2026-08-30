#include "boxedvn_test.h"
#include "guest_low_alias.h"
#include "guest_mmap_placement.h"

using namespace boxedvn;

namespace {

// Canonical guest addresses Wine and the PE loader genuinely require.
constexpr std::uint64_t kTebBlock = 0x1e0000;
constexpr std::uint64_t kKuserSharedData = 0x7ffe0000;
constexpr std::uint64_t kPreferredImageBase = 0x140000000;
constexpr std::uint64_t kPreferredDllBase = 0x170000000;

} // namespace

BOXEDVN_TEST(guest_low_alias_translates_canonical_low_addresses) {
    CHECK(isLowAliasedGuestAddress(0));
    CHECK(isLowAliasedGuestAddress(kTebBlock));
    CHECK(isLowAliasedGuestAddress(kKuserSharedData));
    CHECK(isLowAliasedGuestAddress(kPreferredImageBase));
    CHECK(isLowAliasedGuestAddress(kPreferredDllBase));

    CHECK(guestToHostAddress(0) == kGuestLowAliasBase);
    CHECK(guestToHostAddress(kTebBlock) == kGuestLowAliasBase + kTebBlock);
    CHECK(guestToHostAddress(kKuserSharedData) ==
          kGuestLowAliasBase + kKuserSharedData);
    CHECK(guestToHostAddress(kPreferredImageBase) ==
          kGuestLowAliasBase + kPreferredImageBase);
}

BOXEDVN_TEST(guest_low_alias_or_equals_addition_across_the_low_range) {
    // The whole mechanism rests on the base being aligned to the limit, so OR
    // and addition agree for every canonical low address.
    for (std::uint64_t guest = 0; guest < kGuestLowLimit;
         guest = guest ? guest * 3 : 1) {
        CHECK(guestToHostAddress(guest) == kGuestLowAliasBase + guest);
    }
    const std::uint64_t last = kGuestLowLimit - 1;
    CHECK(guestToHostAddress(last) == kGuestLowAliasBase + last);
    CHECK(guestToHostAddress(last) == kGuestLowAliasEnd - 1);
}

BOXEDVN_TEST(guest_low_alias_boundaries_are_exact) {
    // 0 and limit-1 alias; limit itself is not a canonical low address, and an
    // address in the high identity lane is dereferenced at its own value.
    CHECK(isLowAliasedGuestAddress(0));
    CHECK(isLowAliasedGuestAddress(kGuestLowLimit - 1));
    CHECK(!isLowAliasedGuestAddress(kGuestLowLimit));
    CHECK(!isLowAliasedGuestAddress(kGuestHighBase));

    CHECK(guestToHostAddress(kGuestHighBase) == kGuestHighBase);
    CHECK(guestToHostAddress(kGuestHighEnd - 1) == kGuestHighEnd - 1);
    CHECK(guestToHostAddress(kGuestHighBase + 0x123456) ==
          kGuestHighBase + 0x123456);
}

BOXEDVN_TEST(guest_low_alias_is_identity_across_the_whole_high_lane) {
    // Every permitted high guest address must already carry the base's bits,
    // or the single OR would corrupt it. Walk the lane at host-page stride
    // near both ends and at a spread of interior points.
    for (std::uint64_t offset = 0; offset < 0x40000; offset += 0x4000) {
        CHECK(guestToHostAddress(kGuestHighBase + offset) ==
              kGuestHighBase + offset);
        CHECK(guestToHostAddress(kGuestHighEnd - 1 - offset) ==
              kGuestHighEnd - 1 - offset);
    }
    for (std::uint64_t step = 1; step < kGuestLowLimit; step *= 2) {
        const std::uint64_t guest = kGuestHighBase + step;
        CHECK(guestToHostAddress(guest) == guest);
    }
}

BOXEDVN_TEST(guest_low_alias_translation_is_idempotent) {
    // The translator applies the OR at each dereference site; applying it to
    // an already-translated value must not move the address again.
    CHECK(guestToHostAddress(guestToHostAddress(kTebBlock)) ==
          guestToHostAddress(kTebBlock));
    CHECK(guestToHostAddress(guestToHostAddress(kGuestHighBase)) ==
          kGuestHighBase);
}

BOXEDVN_TEST(guest_low_alias_recovers_canonical_addresses) {
    // A host fault address has to be reported back to the guest canonically.
    CHECK(hostToGuestAddress(guestToHostAddress(kTebBlock)) == kTebBlock);
    CHECK(hostToGuestAddress(guestToHostAddress(kKuserSharedData)) ==
          kKuserSharedData);
    CHECK(hostToGuestAddress(guestToHostAddress(0)) == 0);
    // Addresses outside the alias window pass through untouched, so this is
    // safe to apply to a fault address of unknown provenance.
    CHECK(hostToGuestAddress(kGuestHighBase) == kGuestHighBase);
    CHECK(hostToGuestAddress(kGuestLowAliasEnd) == kGuestLowAliasEnd);
}

BOXEDVN_TEST(guest_low_alias_hosts_the_ranges_wine_actually_requests) {
    // The reservation that previously failed: 2 MiB below 2 GiB.
    CHECK(guestRangeHostable(kTebBlock, 0x200000));
    // KUSER_SHARED_DATA and the standard PE bases.
    CHECK(guestRangeHostable(kKuserSharedData, 0x10000));
    CHECK(guestRangeHostable(kPreferredImageBase, 0x1000000));
    CHECK(guestRangeHostable(kPreferredDllBase, 0x1000000));
    // Wine's whole low reservation walk starts here.
    CHECK(guestRangeHostable(0x10000, 0x67ff0000));

    // The high identity lane stays hostable.
    CHECK(guestRangeHostable(kGuestHighBase, 0x1000));
    CHECK(guestRangeHostable(kGuestHighEnd - 0x1000, 0x1000));

    // A range that straddles the low limit is not hostable as one mapping,
    // and neither is the alias window itself: a guest address there would be
    // indistinguishable from the host address of some canonical low page.
    CHECK(!guestRangeHostable(kGuestLowLimit - 0x1000, 0x2000));
    CHECK(!guestRangeHostable(kGuestLowAliasBase, 0x1000));
    CHECK(!guestRangeHostable(kGuestHighEnd, 0x1000));
    CHECK(!guestRangeHostable(0x1000, 0));
}

BOXEDVN_TEST(guest_low_alias_makes_wine_low_reservations_placeable) {
    // With the low range hostable, the no-replace reservation that Wine's TEB
    // allocation depends on must be placed at exactly the canonical address
    // rather than refused.
    GuestMmapRequest request;
    request.address = kTebBlock;
    request.length = 0x200000;
    request.protection = 0;
    request.fixedNoReplace = true;
    request.nativeIdentity = true;
    request.exactRangeAllowed = guestRangeHostable(kTebBlock, 0x200000);
    request.exactRangeFree = true;
    request.exactRangeUnmapped = true;
    CHECK(chooseGuestMmapPlacement(request) ==
          GuestMmapPlacement::MapExactNoReplace);

    // An overlapping second request still reports the range as taken.
    request.exactRangeUnmapped = false;
    CHECK(chooseGuestMmapPlacement(request) == GuestMmapPlacement::FailExists);

    // And the low reservation walk no longer falls into the refusal rule that
    // existed only because low addresses could not be hosted at all.
    GuestMmapRequest probe;
    probe.address = 0x10000;
    probe.length = 0x67ff0000;
    probe.protection = 0;
    probe.nativeIdentity = true;
    probe.exactRangeAllowed = guestRangeHostable(0x10000, 0x67ff0000);
    probe.exactRangeFree = true;
    probe.exactRangeUnmapped = true;
    CHECK(chooseGuestMmapPlacement(probe) != GuestMmapPlacement::FailNoMemory);
    CHECK(chooseGuestMmapPlacement(probe) == GuestMmapPlacement::MapExact);
}
