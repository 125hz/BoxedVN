#include "boxedvn_test.h"
#include "native_map_plan.h"

#include <cstdint>
#include <vector>

using namespace boxedvn;

// The x86-64 guest uses 4 KiB pages; iOS uses 16 KiB. A guest mapping is
// rounded out to the enclosing host pages, so consecutive guest requests
// routinely share a host page with the one before them. glibc's heap does
// exactly that on every 4 KiB brk growth, and the mixture of already-tracked
// and untracked host pages was refused outright:
//
//   KMemory64: refusing partially tracked native map [0x7a00028000,0x7a0004c000)
//   sys_brk64: failed to map growth range [0x7a00029000,0x7a0004a000), error=-12
//
// The host page size is a parameter here, so the 16 KiB behaviour is exercised
// deterministically on any host, including a Windows test runner with 4 KiB
// pages of its own.

namespace {

constexpr std::uint64_t kHostPage = 0x4000;   // the iOS host page
constexpr std::uint64_t kGuestPage = 0x1000;  // the x86-64 guest page

// A synthetic address space: which host intervals are tracked, and which are
// occupied by something this address space does not own.
class FakeHostSpace {
public:
    void track(std::uint64_t start, std::uint64_t end) {
        tracked_.push_back({start, end});
    }

    void occupy(std::uint64_t start, std::uint64_t end) {
        occupied_.push_back({start, end});
    }

    static bool covered(void* context, std::uint64_t start,
                        std::uint64_t end) {
        auto* space = static_cast<FakeHostSpace*>(context);
        for (const auto& range : space->tracked_) {
            if (start >= range.first && end <= range.second) {
                return true;
            }
        }
        return false;
    }

    static bool free(void* context, std::uint64_t start, std::uint64_t end) {
        auto* space = static_cast<FakeHostSpace*>(context);
        for (const auto& range : space->occupied_) {
            if (start < range.second && end > range.first) {
                return false;
            }
        }
        for (const auto& range : space->tracked_) {
            if (start < range.second && end > range.first) {
                return false;
            }
        }
        return true;
    }

private:
    std::vector<std::pair<std::uint64_t, std::uint64_t>> tracked_;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> occupied_;
};

NativeMapPlan plan(FakeHostSpace& space, std::uint64_t address,
                   std::uint64_t length) {
    return planNativeAnonymousMap(address, length, kHostPage, kGuestPage,
                                  &FakeHostSpace::covered,
                                  &FakeHostSpace::free, &space);
}

// The device's own addresses.
constexpr std::uint64_t kBreakGrowthStart = 0x7a00029000ULL;
constexpr std::uint64_t kBreakGrowthEnd = 0x7a0004a000ULL;
constexpr std::uint64_t kAlreadyOwnedEnd = 0x7a0002c000ULL;

} // namespace

BOXEDVN_TEST(native_map_plan_reproduces_the_device_break_growth) {
    // The break already owns through 0x7a0002c000, and glibc asks for
    // [0x7a00029000, 0x7a0004a000). The enclosing host interval is
    // [0x7a00028000, 0x7a0004c000): a tracked prefix and an untracked suffix.
    FakeHostSpace space;
    space.track(0x7a00008000ULL, kAlreadyOwnedEnd);

    const NativeMapPlan result =
        plan(space, kBreakGrowthStart, kBreakGrowthEnd - kBreakGrowthStart);
    CHECK(result.ok());
    CHECK(result.hostStart == 0x7a00028000ULL);
    CHECK(result.hostEnd == 0x7a0004c000ULL);

    // Exactly one reused run and one new run, in that order.
    CHECK(result.runs.size() == 2);
    CHECK(result.runs[0].reused);
    CHECK(result.runs[0].start == 0x7a00028000ULL);
    CHECK(result.runs[0].length == kAlreadyOwnedEnd - 0x7a00028000ULL);
    CHECK(!result.runs[1].reused);
    CHECK(result.runs[1].start == kAlreadyOwnedEnd);
    CHECK(result.runs[1].length == 0x7a0004c000ULL - kAlreadyOwnedEnd);

    // One host page reused, the rest mapped.
    CHECK(result.reusedBytes == 0x4000);
    CHECK(result.reusedPages == 1);
    CHECK(result.mappedBytes == 0x7a0004c000ULL - kAlreadyOwnedEnd);
    CHECK(result.mappedPages == result.mappedBytes / kHostPage);
    // The request starts and ends mid-host-page, so the host protection has to
    // stay the union its neighbours need.
    CHECK(!result.exactHostCover);
    CHECK(!result.fullyReused());
}

BOXEDVN_TEST(native_map_plan_tiles_the_interval_without_gaps) {
    // Whatever the shape, the runs have to cover the enclosing interval
    // exactly once: a gap would leave host pages neither reused nor mapped.
    FakeHostSpace space;
    space.track(0x7a00008000ULL, kAlreadyOwnedEnd);
    const NativeMapPlan result =
        plan(space, kBreakGrowthStart, kBreakGrowthEnd - kBreakGrowthStart);
    CHECK(result.ok());

    std::uint64_t cursor = result.hostStart;
    for (const NativeHostRun& run : result.runs) {
        CHECK(run.start == cursor);
        CHECK(run.length != 0);
        CHECK(run.length % kHostPage == 0);
        cursor += run.length;
    }
    CHECK(cursor == result.hostEnd);
    CHECK(result.reusedBytes + result.mappedBytes ==
          result.hostEnd - result.hostStart);
}

BOXEDVN_TEST(native_map_plan_handles_a_fully_fresh_request) {
    FakeHostSpace space;
    const NativeMapPlan result = plan(space, 0x7a00100000ULL, 0x8000);
    CHECK(result.ok());
    CHECK(result.hostStart == 0x7a00100000ULL);
    CHECK(result.hostEnd == 0x7a00108000ULL);
    CHECK(result.runs.size() == 1);
    CHECK(!result.runs[0].reused);
    CHECK(result.reusedPages == 0);
    CHECK(result.mappedPages == 2);
    // Host-page aligned at both ends, so the requested protection can be
    // applied to the whole interval.
    CHECK(result.exactHostCover);
    CHECK(!result.fullyReused());
}

BOXEDVN_TEST(native_map_plan_handles_a_fully_covered_request) {
    FakeHostSpace space;
    space.track(0x7a00100000ULL, 0x7a00108000ULL);
    const NativeMapPlan result = plan(space, 0x7a00101000ULL, 0x1000);
    CHECK(result.ok());
    CHECK(result.fullyReused());
    CHECK(result.mappedPages == 0);
    CHECK(result.reusedPages == 1);
    CHECK(result.runs.size() == 1);
    CHECK(result.runs[0].reused);
    // One guest page inside a host page: the neighbours share it.
    CHECK(!result.exactHostCover);
}

BOXEDVN_TEST(native_map_plan_handles_a_prefix_overlap) {
    // The tracked pages come first, which is the ordinary heap-growth shape.
    FakeHostSpace space;
    space.track(0x7a00100000ULL, 0x7a00108000ULL);
    const NativeMapPlan result = plan(space, 0x7a00100000ULL, 0x10000);
    CHECK(result.ok());
    CHECK(result.runs.size() == 2);
    CHECK(result.runs[0].reused);
    CHECK(result.runs[0].length == 0x8000);
    CHECK(!result.runs[1].reused);
    CHECK(result.runs[1].length == 0x8000);
    CHECK(result.reusedPages == 2);
    CHECK(result.mappedPages == 2);
}

BOXEDVN_TEST(native_map_plan_handles_a_suffix_overlap) {
    // Tracked pages at the end instead, so the walk cannot assume a prefix.
    FakeHostSpace space;
    space.track(0x7a00108000ULL, 0x7a00110000ULL);
    const NativeMapPlan result = plan(space, 0x7a00100000ULL, 0x10000);
    CHECK(result.ok());
    CHECK(result.runs.size() == 2);
    CHECK(!result.runs[0].reused);
    CHECK(result.runs[0].start == 0x7a00100000ULL);
    CHECK(result.runs[0].length == 0x8000);
    CHECK(result.runs[1].reused);
    CHECK(result.runs[1].start == 0x7a00108000ULL);
    CHECK(result.runs[1].length == 0x8000);
}

BOXEDVN_TEST(native_map_plan_handles_tracked_pages_in_the_middle) {
    FakeHostSpace space;
    space.track(0x7a00104000ULL, 0x7a00108000ULL);
    const NativeMapPlan result = plan(space, 0x7a00100000ULL, 0x10000);
    CHECK(result.ok());
    CHECK(result.runs.size() == 3);
    CHECK(!result.runs[0].reused);
    CHECK(result.runs[1].reused);
    CHECK(!result.runs[2].reused);
    CHECK(result.reusedPages == 1);
    CHECK(result.mappedPages == 3);
}

BOXEDVN_TEST(native_map_plan_refuses_an_occupied_gap) {
    // An untracked host page in the interval belongs to something else --
    // the executable arena, the app image, another runtime region. Mapping
    // over it would destroy memory this address space does not own, so the
    // whole request is refused rather than partially applied.
    FakeHostSpace space;
    space.track(0x7a00100000ULL, 0x7a00104000ULL);
    space.occupy(0x7a00108000ULL, 0x7a0010c000ULL);
    const NativeMapPlan result = plan(space, 0x7a00100000ULL, 0x10000);
    CHECK(!result.ok());
    CHECK(result.status == NativeMapPlanStatus::GapOccupied);

    // Without the occupant the same request is fine, so the refusal is about
    // the occupant and not about the shape.
    FakeHostSpace clear;
    clear.track(0x7a00100000ULL, 0x7a00104000ULL);
    CHECK(plan(clear, 0x7a00100000ULL, 0x10000).ok());
}

BOXEDVN_TEST(native_map_plan_refuses_an_occupied_page_the_request_starts_on) {
    FakeHostSpace space;
    space.occupy(0x7a00100000ULL, 0x7a00104000ULL);
    const NativeMapPlan result = plan(space, 0x7a00101000ULL, 0x1000);
    CHECK(!result.ok());
    CHECK(result.status == NativeMapPlanStatus::GapOccupied);
}

BOXEDVN_TEST(native_map_plan_rejects_invalid_requests) {
    FakeHostSpace space;
    // Zero length.
    CHECK(plan(space, 0x7a00100000ULL, 0).status ==
          NativeMapPlanStatus::InvalidRequest);
    // A wrapping range.
    CHECK(plan(space, ~(std::uint64_t)0 - 0x10, 0x1000).status ==
          NativeMapPlanStatus::InvalidRequest);
    // A host page size that is not a power of two, or smaller than the guest
    // page: neither can round an interval correctly.
    CHECK(planNativeAnonymousMap(0x7a00100000ULL, 0x1000, 0x3000, kGuestPage,
                                 &FakeHostSpace::covered,
                                 &FakeHostSpace::free, &space).status ==
          NativeMapPlanStatus::InvalidRequest);
    CHECK(planNativeAnonymousMap(0x7a00100000ULL, 0x1000, 0x800, kGuestPage,
                                 &FakeHostSpace::covered,
                                 &FakeHostSpace::free, &space).status ==
          NativeMapPlanStatus::InvalidRequest);
    // Missing callbacks.
    CHECK(planNativeAnonymousMap(0x7a00100000ULL, 0x1000, kHostPage, kGuestPage,
                                 nullptr, &FakeHostSpace::free, &space).status ==
          NativeMapPlanStatus::InvalidRequest);
}

BOXEDVN_TEST(native_map_plan_exact_cover_only_when_host_page_aligned) {
    FakeHostSpace space;
    // Aligned at both ends.
    CHECK(plan(space, 0x7a00100000ULL, 0x4000).exactHostCover);
    // Aligned start, unaligned end.
    CHECK(!plan(space, 0x7a00100000ULL, 0x1000).exactHostCover);
    // Unaligned start, aligned end.
    CHECK(!plan(space, 0x7a00101000ULL, 0x3000).exactHostCover);
    // The device's request: unaligned at both ends.
    CHECK(!plan(space, kBreakGrowthStart,
                kBreakGrowthEnd - kBreakGrowthStart).exactHostCover);
}

BOXEDVN_TEST(native_map_plan_maps_no_page_twice) {
    // Every host page appears in exactly one run, so nothing is reserved or
    // mapped twice and the rollback list cannot double-free.
    FakeHostSpace space;
    space.track(0x7a00104000ULL, 0x7a00108000ULL);
    space.track(0x7a00110000ULL, 0x7a00114000ULL);
    const NativeMapPlan result = plan(space, 0x7a00100000ULL, 0x18000);
    CHECK(result.ok());
    std::vector<std::uint64_t> seen;
    for (const NativeHostRun& run : result.runs) {
        for (std::uint64_t page = run.start; page < run.start + run.length;
             page += kHostPage) {
            for (std::uint64_t other : seen) {
                CHECK(other != page);
            }
            seen.push_back(page);
        }
    }
    CHECK(seen.size() ==
          (result.hostEnd - result.hostStart) / kHostPage);
    CHECK(seen.size() == result.reusedPages + result.mappedPages);
}

// The plan decides which host pages are reused; what the address space then
// does with them is the other half of the fix. These two model that
// application against a byte array, because the properties that matter are
// about data, not about intervals: a reused host page carries a neighbouring
// guest subpage that must survive, and the requested guest bytes must read as
// zero however the interval was assembled.

namespace {

// What KMemory64::nativeMapAnonymous does once the plan is in hand: fresh
// runs arrive zeroed, and then exactly the requested guest bytes are zeroed --
// never the enclosing host pages.
void applyPlan(const NativeMapPlan& result, std::vector<std::uint8_t>& memory,
               std::uint64_t base, std::uint64_t requestAddress,
               std::uint64_t requestLength) {
    for (const NativeHostRun& run : result.runs) {
        if (run.reused) {
            continue;
        }
        for (std::uint64_t offset = 0; offset < run.length; ++offset) {
            memory[run.start - base + offset] = 0;
        }
    }
    for (std::uint64_t offset = 0; offset < requestLength; ++offset) {
        memory[requestAddress - base + offset] = 0;
    }
}

} // namespace

BOXEDVN_TEST(native_map_plan_application_preserves_the_reused_neighbour) {
    // The device's shape. The tracked host page [0x...28000, 0x...2c000)
    // carries the guest pages below the new break, which glibc is still
    // using: zeroing the whole host page would destroy the heap.
    const std::uint64_t base = 0x7a00028000ULL;
    const std::uint64_t span = 0x7a0004c000ULL - base;
    std::vector<std::uint8_t> memory(span, 0xCD);

    FakeHostSpace space;
    space.track(0x7a00008000ULL, kAlreadyOwnedEnd);
    const NativeMapPlan result =
        plan(space, kBreakGrowthStart, kBreakGrowthEnd - kBreakGrowthStart);
    CHECK(result.ok());

    applyPlan(result, memory, base, kBreakGrowthStart,
              kBreakGrowthEnd - kBreakGrowthStart);

    // Everything below the request, inside the reused host page, is untouched.
    for (std::uint64_t address = base; address < kBreakGrowthStart; ++address) {
        CHECK(memory[address - base] == 0xCD);
    }
    // The byte immediately before the request is the one most at risk.
    CHECK(memory[kBreakGrowthStart - 1 - base] == 0xCD);
}

BOXEDVN_TEST(native_map_plan_application_zeroes_exactly_the_request) {
    const std::uint64_t base = 0x7a00028000ULL;
    const std::uint64_t span = 0x7a0004c000ULL - base;
    std::vector<std::uint8_t> memory(span, 0xCD);

    FakeHostSpace space;
    space.track(0x7a00008000ULL, kAlreadyOwnedEnd);
    const NativeMapPlan result =
        plan(space, kBreakGrowthStart, kBreakGrowthEnd - kBreakGrowthStart);
    CHECK(result.ok());
    applyPlan(result, memory, base, kBreakGrowthStart,
              kBreakGrowthEnd - kBreakGrowthStart);

    // Every requested byte reads as zero, which is what MAP_ANONYMOUS means.
    for (std::uint64_t address = kBreakGrowthStart; address < kBreakGrowthEnd;
         ++address) {
        CHECK(memory[address - base] == 0);
    }
    // The tail of the last host page is beyond the request but was newly
    // mapped, so it is zero too -- it belongs to nobody else.
    for (std::uint64_t address = kBreakGrowthEnd; address < 0x7a0004c000ULL;
         ++address) {
        CHECK(memory[address - base] == 0);
    }
}
