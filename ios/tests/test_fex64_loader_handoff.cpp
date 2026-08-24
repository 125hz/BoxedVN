#include "boxedvn_test.h"
#include "fex64loaderhandoff.h"

#include <cstdint>
#include <limits>

using namespace boxedvn;

BOXEDVN_TEST(fex64_loader_handoff_builds_a_near_immediate_trampoline) {
    const auto patch = planFex64LoaderHandoffPatch(
        0x7e0000f5aaULL, 0x7e0001b198ULL, 0x70480014a0ULL);
    CHECK(patch.has_value());
    CHECK_EQ(patch->branchToTrampoline[0], std::uint8_t{0xe9});
    CHECK_EQ(patch->branchToTrampoline[5], std::uint8_t{0x90});

    std::uint32_t displacement = 0;
    for (std::size_t byte = 0; byte < 4; ++byte) {
        displacement |= std::uint32_t{patch->branchToTrampoline[1 + byte]}
                        << (byte * 8);
    }
    CHECK_EQ(static_cast<std::int32_t>(displacement),
             static_cast<std::int32_t>(0x1b198 - 0xf5af));

    CHECK_EQ(patch->trampoline[0], std::uint8_t{0x49});
    CHECK_EQ(patch->trampoline[1], std::uint8_t{0xbb});
    CHECK_EQ(patch->trampoline[10], std::uint8_t{0x41});
    CHECK_EQ(patch->trampoline[11], std::uint8_t{0xff});
    CHECK_EQ(patch->trampoline[12], std::uint8_t{0xe3});

    std::uint64_t entry = 0;
    for (std::size_t byte = 0; byte < 8; ++byte) {
        entry |= std::uint64_t{patch->trampoline[2 + byte]} << (byte * 8);
    }
    CHECK_EQ(entry, std::uint64_t{0x70480014a0ULL});
}

BOXEDVN_TEST(fex64_loader_handoff_accepts_the_negative_rel32_boundary) {
    const auto patch = planFex64LoaderHandoffPatch(
        0x900000000ULL, 0x880000005ULL, 0x700000001ULL);
    CHECK(patch.has_value());
    CHECK_EQ(patch->branchToTrampoline[1], std::uint8_t{0x00});
    CHECK_EQ(patch->branchToTrampoline[2], std::uint8_t{0x00});
    CHECK_EQ(patch->branchToTrampoline[3], std::uint8_t{0x00});
    CHECK_EQ(patch->branchToTrampoline[4], std::uint8_t{0x80});
}

BOXEDVN_TEST(fex64_loader_handoff_rejects_invalid_or_unreachable_slots) {
    CHECK(!planFex64LoaderHandoffPatch(0x1000, 0x2000, 0).has_value());
    CHECK(!planFex64LoaderHandoffPatch(
               std::numeric_limits<std::uint64_t>::max() - 4,
               0x2000, 0x3000)
               .has_value());
    CHECK(!planFex64LoaderHandoffPatch(
               0x1000, 0x1000 + 5 + 0x80000000ULL, 0x3000)
               .has_value());
    CHECK(!planFex64LoaderHandoffPatch(
               0x900000000ULL, 0x87ffffff4ULL, 0x3000)
               .has_value());
}

BOXEDVN_TEST(fex64_loader_handoff_recovers_only_the_interpreter_elf_base) {
    constexpr std::array<std::uint8_t, 4> elfMagic {0x7f, 'E', 'L', 'F'};
    constexpr std::array<std::uint8_t, 4> nonElf {0x90, 0x90, 0x90, 0x90};
    const auto recovered = validatedFex64LoaderFallbackEntry(
        0x7dffff0000ULL, 0x7dffff0000ULL, 0x70480014a0ULL, elfMagic);
    CHECK(recovered.has_value());
    CHECK_EQ(*recovered, std::uint64_t{0x70480014a0ULL});

    CHECK(!validatedFex64LoaderFallbackEntry(
               0x7dffff1000ULL, 0x7dffff0000ULL,
               0x70480014a0ULL, elfMagic).has_value());
    CHECK(!validatedFex64LoaderFallbackEntry(
               0x7dffff0000ULL, 0x7dffff0000ULL,
               0x70480014a0ULL, nonElf).has_value());
    CHECK(!validatedFex64LoaderFallbackEntry(
               0x7dffff0000ULL, 0x7dffff0000ULL, 0, elfMagic).has_value());
}

BOXEDVN_TEST(fex64_loader_handoff_returns_through_the_runner_boundary) {
    constexpr std::array<std::uint8_t, 4> elfMagic {0x7f, 'E', 'L', 'F'};
    const auto resume = validatedFex64LoaderRunnerResume(
        0x7dffff0000ULL, 0x7dffff0000ULL, 0x70480014a0ULL,
        elfMagic, 0x16b5f25f0ULL, 0x119426000ULL);
    CHECK(resume.has_value());
    CHECK_EQ(resume->guestEntry, std::uint64_t{0x70480014a0ULL});
    CHECK_EQ(resume->hostStack, std::uint64_t{0x16b5f25f0ULL});
    CHECK_EQ(resume->hostPC, std::uint64_t{0x119426000ULL});

    CHECK(!validatedFex64LoaderRunnerResume(
               0x7dffff0000ULL, 0x7dffff0000ULL, 0x70480014a0ULL,
               elfMagic, 0, 0x119426000ULL).has_value());
    CHECK(!validatedFex64LoaderRunnerResume(
               0x7dffff0000ULL, 0x7dffff0000ULL, 0x70480014a0ULL,
               elfMagic, 0x16b5f25f0ULL, 0).has_value());
}
