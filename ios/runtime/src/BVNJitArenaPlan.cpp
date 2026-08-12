/*
 * BoxedVN - how much executable memory to ask StikDebug to prepare.
 * GPLv2; see license.txt.
 */

#include "BVNJitArenaPlan.h"

#include <algorithm>

namespace {

std::size_t roundUpToPage(std::size_t value, std::size_t pageSize) {
    if (pageSize == 0) {
        return value;
    }
    const std::size_t remainder = value % pageSize;
    return remainder == 0 ? value : value + (pageSize - remainder);
}

}  // namespace

BVNJitArenaPlan BVNPlanJitArena(std::uint64_t availableBytes,
                                std::uint64_t physicalBytes,
                                std::size_t pageSize) {
    BVNJitArenaPlan plan;
    plan.segmentBytes = roundUpToPage(kBVNJitArenaSegmentBytes, pageSize);

    // A zero from either probe means "not known", not "nothing left". Treat it
    // as no constraint rather than as a reason to build the smallest arena.
    std::uint64_t budget = UINT64_MAX;
    if (availableBytes != 0) {
        budget = std::min(budget, availableBytes / kBVNJitArenaBudgetDivisor);
    }
    if (physicalBytes != 0) {
        budget = std::min(budget, physicalBytes / kBVNJitArenaBudgetDivisor);
    }
    if (budget == UINT64_MAX) {
        budget = kBVNJitArenaMinimumBytes;
    }

    budget = std::max<std::uint64_t>(budget, kBVNJitArenaMinimumBytes);
    budget = std::min<std::uint64_t>(budget, kBVNJitArenaMaximumBytes);

    // Round the segment count up: the floor is a promise about capacity, and
    // rounding down would quietly break it when the segment size does not
    // divide it evenly.
    plan.segmentCount = static_cast<std::size_t>(
        (budget + plan.segmentBytes - 1) / plan.segmentBytes);
    if (plan.segmentCount == 0) {
        plan.segmentCount = 1;
    }
    return plan;
}
