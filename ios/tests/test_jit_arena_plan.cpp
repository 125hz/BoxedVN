/* BoxedVN - JIT arena sizing regression tests. GPLv2. */

#include "boxedvn_test.h"

#include "BVNJitArenaPlan.h"

namespace {

constexpr std::uint64_t kMiB = 1024ull * 1024ull;
constexpr std::uint64_t kGiB = 1024ull * kMiB;

// iOS pages are 16 KiB on every device BoxedVN supports.
constexpr std::size_t kPageSize = 16u * 1024u;

}  // namespace

BOXEDVN_TEST(jit_arena_plan_never_goes_below_the_proven_floor) {
    // A small, busy device: an eighth of the budget is well under the floor.
    const BVNJitArenaPlan plan = BVNPlanJitArena(256 * kMiB, 2 * kGiB,
                                                kPageSize);
    CHECK_EQ(plan.totalBytes(), kBVNJitArenaMinimumBytes);
}

BOXEDVN_TEST(jit_arena_plan_falls_back_to_the_floor_when_memory_is_unknown) {
    // Both probes returning 0 means "could not read it", not "nothing left".
    const BVNJitArenaPlan plan = BVNPlanJitArena(0, 0, kPageSize);
    CHECK_EQ(plan.totalBytes(), kBVNJitArenaMinimumBytes);
    CHECK(plan.segmentCount >= 1);
}

BOXEDVN_TEST(jit_arena_plan_grows_with_the_process_budget) {
    // The device in the Chromium-guest logs: 5.99 GB of headroom, 8 GB RAM.
    // The old fixed 128 MiB ran dry there after roughly 2,180 blocks.
    const BVNJitArenaPlan plan = BVNPlanJitArena(6134 * kMiB, 8 * kGiB,
                                                kPageSize);
    CHECK(plan.totalBytes() > kBVNJitArenaMinimumBytes);
    CHECK_EQ(plan.totalBytes(), kBVNJitArenaMaximumBytes);
}

BOXEDVN_TEST(jit_arena_plan_is_capped_so_the_guest_keeps_its_memory) {
    // A hypothetical device with far more headroom must not hand most of it
    // to native code the guest cannot address.
    const BVNJitArenaPlan plan = BVNPlanJitArena(64 * kGiB, 64 * kGiB,
                                                kPageSize);
    CHECK_EQ(plan.totalBytes(), kBVNJitArenaMaximumBytes);
}

BOXEDVN_TEST(jit_arena_plan_respects_the_smaller_of_the_two_budgets) {
    // Plenty of RAM but almost none of it left to this process: the
    // per-process limit is what actually terminates the app, so it wins.
    const BVNJitArenaPlan generous = BVNPlanJitArena(6 * kGiB, 8 * kGiB,
                                                    kPageSize);
    const BVNJitArenaPlan constrained = BVNPlanJitArena(1 * kGiB, 8 * kGiB,
                                                       kPageSize);
    CHECK(constrained.totalBytes() < generous.totalBytes());
    CHECK(constrained.totalBytes() >= kBVNJitArenaMinimumBytes);
}

BOXEDVN_TEST(jit_arena_segment_holds_two_guest_processes_of_pe_copies) {
    // The fex64 Wine side copies every PE image it runs into one leased
    // segment, and each guest process gets its own copies -- including its own
    // 39 MiB copy of the ARM64EC emulator DLL. The device logs that ended in a
    // ten-second stack fault had 46 images and about 224 MiB of demand across
    // two guest processes. A segment smaller than that reproduces the fault
    // exactly, and the failure names a stack overflow rather than the pool, so
    // guard the number here where it is legible.
    constexpr std::size_t kTwoProcessDemandBytes = 224u * kMiB;
    CHECK(kBVNJitArenaSegmentBytes >= kTwoProcessDemandBytes);

    // And the floor has to leave a whole segment free after the emulator has
    // taken part of the first one, or Wine's lease fails outright.
    CHECK(kBVNJitArenaMinimumBytes >= 2 * kBVNJitArenaSegmentBytes);
}

BOXEDVN_TEST(jit_arena_plan_segments_are_page_aligned_and_cover_the_total) {
    const BVNJitArenaPlan plan = BVNPlanJitArena(4 * kGiB, 8 * kGiB,
                                                kPageSize);
    CHECK_EQ(plan.segmentBytes % kPageSize, static_cast<std::size_t>(0));
    CHECK(plan.segmentCount > 1);
    // Segments are prepared one at a time, so a plan whose product falls
    // short of the floor would silently ship less than the proven capacity.
    CHECK(plan.totalBytes() >= kBVNJitArenaMinimumBytes);
}
