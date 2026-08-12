/*
 * BoxedVN - how much executable memory to ask StikDebug to prepare.
 * GPLv2; see license.txt.
 */

#ifndef BVN_JIT_ARENA_PLAN_H
#define BVN_JIT_ARENA_PLAN_H

#include <cstddef>
#include <cstdint>

// Every executable page BoxedVN will ever use has to be prepared during the
// one startup handshake: asking StikDebug for another region after the guest
// is running would stop the guest at a live debugger breakpoint, and by then
// StikDebug may be suspended in the background. So the whole arena budget is
// decided here, before anything else runs, from the process memory budget.
//
// The arena is prepared as several equally sized segments rather than one
// mapping. Two reasons, both practical:
//
//   * A single half-gigabyte contiguous reservation is much easier for the
//     kernel to refuse than eight smaller ones, and refusing the first one
//     used to mean no JIT at all. Segments degrade: whatever was prepared
//     before a failure stays usable.
//   * Each segment carries its own free list, so a released 64 KiB block
//     coalesces with its neighbours instead of with unrelated code.
struct BVNJitArenaPlan {
    std::size_t segmentBytes = 0;
    std::size_t segmentCount = 0;

    std::size_t totalBytes() const { return segmentBytes * segmentCount; }
};

// The smallest arena BoxedVN will attempt. Build 23 proved 128 MiB carries a
// Direct3D 9 visual novel plus a fully attached WineDbg, and it is what every
// device result up to build 89 was measured against, so it stays the floor
// even when the memory probe reports nothing usable.
inline constexpr std::size_t kBVNJitArenaMinimumBytes = 128u * 1024u * 1024u;

// The largest arena BoxedVN will attempt. Preparation writes through
// debugserver page by page, so this is paid in startup time and in resident
// memory that the guest can then never use.
inline constexpr std::size_t kBVNJitArenaMaximumBytes = 512u * 1024u * 1024u;

// One segment. Small enough that a partial failure loses little, large enough
// that a game needing hundreds of megabytes does not accumulate a long list.
inline constexpr std::size_t kBVNJitArenaSegmentBytes = 64u * 1024u * 1024u;

// Fraction of the process memory budget the arena may claim. The guest still
// needs the rest: BVNGuestReportedTotalMemory advertises up to 3 GB to a
// 32-bit Wine, and an arena that crowds that out trades one failure for
// another.
inline constexpr std::uint64_t kBVNJitArenaBudgetDivisor = 8;

// `availableBytes` is os_proc_available_memory() and `physicalBytes` is the
// device's RAM; either may be 0 when the probe could not read it, in which
// case only the floor applies. `pageSize` is the host page size every segment
// must be a multiple of.
BVNJitArenaPlan BVNPlanJitArena(std::uint64_t availableBytes,
                                std::uint64_t physicalBytes,
                                std::size_t pageSize);

#endif  // BVN_JIT_ARENA_PLAN_H
