/* BoxedVN - DXMT guest pointer translation matches the translator's rule. GPLv2. */

#include "boxedvn_test.h"

#include "boxedwine_dxmt_guest_pointer.h"
#include "guest_low_alias.h"

// The C helper force-included into DXMT's unix sources must compute exactly
// what the translator computes for every guest address it can be handed.
BOXEDVN_TEST(dxmt_guest_pointer_translation_matches_the_alias_contract) {
    CHECK_EQ((uint64_t)BOXEDWINE_DXMT_GUEST_LOW_ALIAS_BASE, boxedvn::kGuestLowAliasBase);
    CHECK_EQ((uint64_t)BOXEDWINE_DXMT_GUEST_TOP_CLEAR_MASK, boxedvn::kGuestTopClearMask);

    const uint64_t samples[] = {
        0x7ffffe1ff4e8ULL,              // a Wine thread stack slot (top arena)
        boxedvn::kGuestTopBase,
        boxedvn::kGuestTopEnd - 16,
        0x7a00094050ULL,                // guest heap in the identity lane
        boxedvn::kGuestHighBase,
        boxedvn::kGuestHighEnd - 4096,
        0x140001000ULL,                 // a PE image below 4 GiB (low alias)
        0x7ffe0000ULL,                  // KUSER_SHARED_DATA
        0x10000ULL,
    };
    for (uint64_t guest : samples) {
        CHECK_EQ((uint64_t)boxedwine_dxmt_host_pointer((uintptr_t)guest),
                 boxedvn::guestToHostAddress(guest));
    }
}

BOXEDVN_TEST(dxmt_guest_pointer_translation_keeps_null_and_identity) {
    // A null WMTMemoryPointer must stay null: the unix side tests it.
    CHECK_EQ((uint64_t)boxedwine_dxmt_host_pointer(0), 0ULL);
    // The identity lane is untouched, so heap pointers keep working.
    CHECK_EQ((uint64_t)boxedwine_dxmt_host_pointer(0x7a00094050ULL), 0x7a00094050ULL);
    // The top arena is relocated by clearing the field, which is where the
    // parameter block from the device run lives.
    CHECK_EQ((uint64_t)boxedwine_dxmt_host_pointer(0x7ffffe1ff4e8ULL), 0x7ffe1ff4e8ULL);
    // The low range is aliased high.
    CHECK_EQ((uint64_t)boxedwine_dxmt_host_pointer(0x140001000ULL), 0x7940001000ULL);
}

BOXEDVN_TEST(dxmt_guest_pointer_macro_preserves_the_pointer_type) {
    const char* guestString = (const char*)(uintptr_t)0x7ffffe1ff600ULL;
    const char* host = BOXEDWINE_GUEST_PTR(guestString);
    CHECK_EQ((uint64_t)(uintptr_t)host, 0x7ffe1ff600ULL);
    uint64_t raw = 0x7ffffe1ff700ULL;
    CHECK_EQ(BOXEDWINE_GUEST_PTR(raw), 0x7ffe1ff700ULL);
}
