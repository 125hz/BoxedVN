/*
 * BoxedWine - planning a host mapping whose pages are only partly tracked.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * The x86-64 guest uses 4 KiB pages; iOS uses 16 KiB. A guest mapping is
 * therefore rounded out to the enclosing host pages, and consecutive guest
 * requests routinely share a host page with the one before them.
 *
 * glibc's heap makes this ordinary. A process starts with a host-page-aligned
 * break and then grows it at 4 KiB boundaries: one device run held the host
 * page ending at 0x7a0002c000 and then asked for [0x7a00029000,0x7a0004a000),
 * whose enclosing host interval is [0x7a00028000,0x7a0004c000). The prefix was
 * already tracked and the suffix was new. That mixture was refused outright --
 * "refusing partially tracked native map" -- and brk failed with -ENOMEM.
 *
 * Refusing it was not paranoia: one MAP_FIXED across the whole interval would
 * destroy whatever already lives in the tracked prefix, and might land on
 * untracked host memory in a gap. The answer is to plan the interval at host
 * page granularity and act only on the parts that are genuinely free.
 *
 * The plan is computed here, apart from the address space, so every case --
 * fresh, fully covered, prefix overlap, suffix overlap, an occupied gap -- can
 * be exercised deterministically with a synthetic host page size on any host.
 */

#ifndef __NATIVE_MAP_PLAN_H__
#define __NATIVE_MAP_PLAN_H__

#include <cstdint>

#if defined(__cplusplus)
#include <vector>

namespace boxedvn {

// One maximal run of host pages that are either all already tracked or all
// currently unmapped.
struct NativeHostRun {
    std::uint64_t start = 0;
    std::uint64_t length = 0;
    // True when this run is already owned and must be preserved rather than
    // replaced. False when it has to be reserved and mapped.
    bool reused = false;
};

enum class NativeMapPlanStatus : std::uint8_t {
    Ok = 0,
    // Zero length, a wrapping range, or a host page size that is not a power
    // of two at least as large as the guest page.
    InvalidRequest = 1,
    // An untracked host page in the interval is occupied by something else.
    // Mapping over it would destroy memory this address space does not own.
    GapOccupied = 2,
};

struct NativeMapPlan {
    NativeMapPlanStatus status = NativeMapPlanStatus::InvalidRequest;
    // The enclosing host-page-aligned interval.
    std::uint64_t hostStart = 0;
    std::uint64_t hostEnd = 0;
    // True when the request covers the enclosing interval exactly, so no
    // neighbouring guest subpage shares a host page with it. When false the
    // host protection has to stay the safe union those neighbours need.
    bool exactHostCover = false;
    std::uint64_t reusedBytes = 0;
    std::uint64_t mappedBytes = 0;
    std::uint64_t reusedPages = 0;
    std::uint64_t mappedPages = 0;
    // In address order, tiling [hostStart, hostEnd) with no gaps.
    std::vector<NativeHostRun> runs;

    bool ok() const { return status == NativeMapPlanStatus::Ok; }
    // Nothing to map: the caller can take the cheaper already-owned path.
    bool fullyReused() const { return ok() && mappedPages == 0; }
};

// True when [start, end) is already tracked by the address space.
using NativeRangeCoveredFn = bool (*)(void* context, std::uint64_t start,
                                      std::uint64_t end);
// True when [start, end) is genuinely unmapped on the host.
using NativeRangeFreeFn = bool (*)(void* context, std::uint64_t start,
                                   std::uint64_t end);

inline NativeMapPlan planNativeAnonymousMap(std::uint64_t hostAddr,
                                            std::uint64_t length,
                                            std::uint64_t hostPageSize,
                                            std::uint64_t guestPageSize,
                                            NativeRangeCoveredFn covered,
                                            NativeRangeFreeFn free,
                                            void* context) {
    NativeMapPlan plan;
    if (length == 0 || covered == nullptr || free == nullptr) {
        return plan;
    }
    if (hostPageSize == 0 || (hostPageSize & (hostPageSize - 1)) != 0 ||
        hostPageSize < guestPageSize) {
        return plan;
    }
    if (hostAddr > UINT64_MAX - length) {
        return plan;
    }
    const std::uint64_t guestEnd = hostAddr + length;
    if (guestEnd > UINT64_MAX - (hostPageSize - 1)) {
        return plan;
    }

    plan.hostStart = hostAddr & ~(hostPageSize - 1);
    plan.hostEnd = (guestEnd + hostPageSize - 1) & ~(hostPageSize - 1);
    plan.exactHostCover =
        plan.hostStart == hostAddr && plan.hostEnd == guestEnd;

    // Walk host pages, grouping consecutive pages that agree about whether
    // they are already tracked. Coverage is normally one contiguous prefix,
    // but nothing here assumes that.
    for (std::uint64_t page = plan.hostStart; page < plan.hostEnd;
         page += hostPageSize) {
        const bool isReused = covered(context, page, page + hostPageSize);
        if (!plan.runs.empty() && plan.runs.back().reused == isReused &&
            plan.runs.back().start + plan.runs.back().length == page) {
            plan.runs.back().length += hostPageSize;
        } else {
            NativeHostRun run;
            run.start = page;
            run.length = hostPageSize;
            run.reused = isReused;
            plan.runs.push_back(run);
        }
        if (isReused) {
            plan.reusedBytes += hostPageSize;
            ++plan.reusedPages;
        } else {
            plan.mappedBytes += hostPageSize;
            ++plan.mappedPages;
        }
    }

    // Every run that is not already owned has to be genuinely free before it
    // can be reserved. An occupied gap is refused: nothing here may replace
    // memory this address space does not own.
    for (const NativeHostRun& run : plan.runs) {
        if (run.reused) {
            continue;
        }
        if (!free(context, run.start, run.start + run.length)) {
            plan.status = NativeMapPlanStatus::GapOccupied;
            return plan;
        }
    }

    plan.status = NativeMapPlanStatus::Ok;
    return plan;
}

} // namespace boxedvn

#endif // __cplusplus

#endif
