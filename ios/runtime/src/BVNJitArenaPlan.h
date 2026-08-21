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
// device result up to build 89 was measured against, so it stayed the floor
// for as long as one segment was 128 MiB.
//
// The floor is expressed in segments, not in megabytes: it has to leave the
// fex64 Wine side a whole empty segment after the emulator has taken part of
// the first one, or Wine's lease fails and nothing starts. Two segments is
// that promise, so the floor moves with the segment size below.
inline constexpr std::size_t kBVNJitArenaMinimumBytes = 512u * 1024u * 1024u;

// The largest arena BoxedVN will attempt. Preparation writes through
// debugserver page by page, so this is paid in startup time and in resident
// memory that the guest can then never use.
//
// How much of it is really resident is worth checking before raising this
// again. The [phys-map] bands line reports the leased Wine pool dirty end to
// end while the FEX band beside it reads 0/0, which is what it would look
// like if preparation only reserved and Wine had genuinely filled all
// 128 MiB -- the reading that says the old pool was starved. It is not what
// it would look like if preparation dirtied every page. A device log at the
// larger size settles it either way.
inline constexpr std::size_t kBVNJitArenaMaximumBytes = 768u * 1024u * 1024u;

// One segment, and also the largest single lease: allocation is first fit
// within a segment and never spans two, so a caller that needs more than this
// cannot be served however much of the arena is free.
//
// The fex64 Wine side leases one segment for its PE-image copier, and 64 MiB
// stopped being enough as soon as the graphics stack loaded. That run had
// 0x2fc4000 in use and 0x1004000 held back for the dispatcher tail, so d3d11
// asking for 0x460000 overflowed a 64 MiB segment by a few megabytes -- about
// 70 MiB wanted against 64 available. Trimming the tail would have fit that
// one image and broken on the next, so size the segment for the whole stack
// instead. The floor rises with it, because a single-segment arena leaves the
// Wine side nothing once the emulator has taken the first one.
//
// 128 MiB then stopped being enough as soon as a guest started a SECOND guest
// process. Every Windows "process" here is a thread in the one Mach task, but
// each still gets its own PE views and therefore its own pool copies, and the
// ARM64EC emulator DLL alone is 0x2713000 -- a little over 39 MiB -- of that
// copy. Two device logs of a D3D11 title whose launcher execs a second binary
// end identically: 46 images copied, the pool at 0x5ef0000 of 0x8000000 with
// 0x1004000 held back for the dispatcher tail, and the second process's copy
// of the emulator refused for want of about 22 MiB. Nothing recovers from
// that. The image stays non-executable, every entry into it faults
// c0000005 at the same guest PC, the redelivery storm walks the thread's
// 1 MiB stack off its bottom, and the session dies about ten seconds in with
// a stack fault that names none of this.
//
// So the segment has to hold the whole stack TWICE over: roughly 104 MiB of
// first-process images, then the second process's emulator copy and its own
// kernel32/kernelbase/ucrtbase/graphics set. That measured out at about
// 224 MiB including the tail reserve. 256 MiB carries it with room to spare;
// a third concurrent guest process would not fit, and would fail the same
// way. Dedup of the duplicate emulator copies is the fix that scales past
// two -- see docs/ARCHITECTURE_FEX64.md -- and this is the sizing that makes
// two work now.
//
// This segment is not only the image copier's. FEX's translated code buffers
// carve from the SAME pool, from the far end: the log's DUAL_MAP_SANITY lines
// put its cursor inside the leased range, and the head allocator holds back
// whatever the tail has reserved. The tail is capped at half the pool and a
// refusal there is survivable -- FEX halves its request down and keeps going,
// so a thread gets a smaller code buffer rather than none -- but it is paid
// in recompilation. 256 MiB leaves about 50 MiB for that after two guest
// processes' images, which is thin: the upstream tuning constants in
// virtual_ios.c were measured against a 896 MiB head and a 256 MiB tail. If a
// device log shows code buffers rotating (the "[jit-pool] tail CAP" and
// "Failed to mprotect last page of code buffer" lines), this is the number to
// raise, and the [phys-map] bands line is what says whether the arena is
// actually resident or merely reserved.
inline constexpr std::size_t kBVNJitArenaSegmentBytes = 256u * 1024u * 1024u;

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
