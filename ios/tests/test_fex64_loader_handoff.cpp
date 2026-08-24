#include "boxedvn_test.h"
#include "fex64loaderhandoff.h"

#include <cstdint>
#include <limits>

using namespace boxedvn;

BOXEDVN_TEST(fex64_loader_handoff_builds_a_register_preserving_indirect_jump) {
    const auto patch = planFex64LoaderHandoffPatch(
        0x7e0000f5aaULL, 0x7e0001b198ULL, 0x70480014a0ULL);
    CHECK(patch.has_value());
    CHECK_EQ(patch->indirectJump[0], std::uint8_t{0xff});
    CHECK_EQ(patch->indirectJump[1], std::uint8_t{0x25});

    std::uint32_t displacement = 0;
    for (std::size_t byte = 0; byte < 4; ++byte) {
        displacement |= std::uint32_t{patch->indirectJump[2 + byte]}
                        << (byte * 8);
    }
    CHECK_EQ(static_cast<std::int32_t>(displacement),
             static_cast<std::int32_t>(0x1b198 - 0xf5b0));

    std::uint64_t entry = 0;
    for (std::size_t byte = 0; byte < 8; ++byte) {
        entry |= std::uint64_t{patch->entrySlot[byte]} << (byte * 8);
    }
    CHECK_EQ(entry, std::uint64_t{0x70480014a0ULL});
}

BOXEDVN_TEST(fex64_loader_handoff_accepts_the_negative_rel32_boundary) {
    const auto patch = planFex64LoaderHandoffPatch(
        0x900000000ULL, 0x880000006ULL, 0x700000001ULL);
    CHECK(patch.has_value());
    CHECK_EQ(patch->indirectJump[2], std::uint8_t{0x00});
    CHECK_EQ(patch->indirectJump[3], std::uint8_t{0x00});
    CHECK_EQ(patch->indirectJump[4], std::uint8_t{0x00});
    CHECK_EQ(patch->indirectJump[5], std::uint8_t{0x80});
}

BOXEDVN_TEST(fex64_loader_handoff_rejects_invalid_or_unreachable_slots) {
    CHECK(!planFex64LoaderHandoffPatch(0x1000, 0x2000, 0).has_value());
    CHECK(!planFex64LoaderHandoffPatch(
               std::numeric_limits<std::uint64_t>::max() - 4,
               0x2000, 0x3000)
               .has_value());
    CHECK(!planFex64LoaderHandoffPatch(
               0x1000, 0x1000 + 6 + 0x80000000ULL, 0x3000)
               .has_value());
    CHECK(!planFex64LoaderHandoffPatch(
               0x900000000ULL, 0x87ffffff5ULL, 0x3000)
               .has_value());
}
