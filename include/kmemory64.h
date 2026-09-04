/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef __KMEMORY64_H__
#define __KMEMORY64_H__

#ifdef BOXEDWINE_GUEST_X64

#include <memory>
#include <atomic>
#include <unordered_map>
#include <map>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

class KProcess;
class KThread;

// Minimal 64-bit guest memory for v1. Pages are allocated lazily on first
// touch via an unordered_map keyed by page number (vaddr >> 12). This is
// not the long-term design — a real two-level page table will replace it
// once the loader/decoder/syscall paths are exercising it. For Phase 1.5
// the priority is: enough surface to back ElfLoader64 segment mapping and
// 64-bit stack setup, with simple correctness over performance.
//
// Deliberately NOT supported in sparse v1:
//   - file-backed mappings (mmap with fd)
//   - MAP_SHARED, copy-on-write
//   - futex pages, JIT page-write tracking
//   - native host memory mapping (opt in with BOXEDWINE_KMEMORY64_NATIVE_IDENTITY)
// The native mode is intentionally narrow: it is for a single address space
// whose guest addresses must also be valid host addresses for a direct FEX
// load/store. Sparse mode remains the default.

#define K64_PAGE_SIZE  4096
#define K64_PAGE_SHIFT 12
#define K64_PAGE_MASK  0xFFFULL

// Permission bits — kept numerically identical to PAGE_READ/PAGE_WRITE/
// PAGE_EXEC/PAGE_MAPPED in kmemory.h so that future merging is trivial.
#define K64_PAGE_READ    0x01
#define K64_PAGE_WRITE   0x02
#define K64_PAGE_EXEC    0x04
#define K64_PAGE_SHARED  0x08
#define K64_PAGE_MAPPED  0x20
// Set when getRamPtr handed out this page's host pointer (futex table / atomic
// RMW hold it across a blocking wait). munmap/mprotect must not decommit a
// pinned page's buffer, or the held pointer would dangle. Outside the kmemory.h
// numbering — a 64-bit-only internal bit.
#define K64_PAGE_PINNED  0x40

// Windows reserves this 64 KiB region for KUSER_SHARED_DATA. iOS keeps the
// entire low 4 GiB behind __PAGEZERO, so native-identity FEX guests use a
// process-shared high host alias and repair the base register in the signal
// context before retrying the faulting instruction.
#define K64_KUSER_SHARED_BASE 0x7ffe0000ULL
#define K64_KUSER_SHARED_SIZE 0x10000ULL

// The host interval every native-identity mapping lands in.
//
// This was a pair of literals described as "the empirically safe interval on
// this host". Nothing in this tree ever established them: both arrived with
// the first commit of the FEX path, with no probe, no measurement and no test,
// and neither was ever revisited. The start, 0x7048000000, sits BELOW the
// alias base, so no lane has been able to reach it since the alias landed; the
// end, 0x7fffff0000, is the top of the alias block less one 64 KiB page. The
// real ceiling on the layout is not a property of iOS at all -- it is
// K64_NATIVE_ALIAS_BLOCK_END below, which the single-OR translation forces.
//
// So the window is derived now and states a fact rather than an assumption: it
// is exactly the span of host addresses this layout can name, from the alias
// base to the end of the relocated arena's host block. What the DEVICE will
// actually hand over inside that span is a different question, and one that
// can be tested instead of assumed: the startup witness in kmemory64.cpp
// probes it with the same non-destructive reservation primitive the mapping
// path uses, and prints what it found. Sparse/interpreter guests keep normal
// Linux-style addresses and do not use these limits.
#define K64_NATIVE_GUEST_WINDOW_START K64_NATIVE_LOW_ALIAS_BASE
#define K64_NATIVE_GUEST_WINDOW_END   K64_NATIVE_TOP_HOST_END

// Windows x86-64 binaries and Wine's loader genuinely require CANONICAL low
// guest addresses: the initial TEB block is reserved below 2 GiB, KUSER_SHARED
// _DATA lives at 0x7ffe0000, and ordinary PE images prefer 0x140000000. None
// of those can be host-mapped at their own address on iOS -- everything below
// 4 GiB is under __PAGEZERO and the band above it is taken. Refusing them made
// Wine's TEB reservation fail and left it dereferencing a null block.
//
// So the low canonical range keeps its guest addresses and is dereferenced
// through a deterministic high host alias. The alias base is chosen so that
// translation is a single OR:
//
//   host = guest | K64_NATIVE_LOW_ALIAS_BASE
//
// which is exact for both halves at once. Low guest addresses have no bits in
// common with the base, so OR is addition; every permitted HIGH guest address
// already contains the base's bits, so OR is the identity there and high
// mappings keep being dereferenced at their own address. One ARM64 `orr` with
// an encodable logical immediate covers every guest memory access, with no
// branch, no comparison and no extra register.
#define K64_NATIVE_LOW_ALIAS_BASE     0x7800000000ULL
#define K64_NATIVE_LOW_GUEST_LIMIT    0x200000000ULL
#define K64_NATIVE_LOW_ALIAS_END      (K64_NATIVE_LOW_ALIAS_BASE + \
                                       K64_NATIVE_LOW_GUEST_LIMIT)

// The whole host layout lives in ONE block: the interval that begins at the
// alias base and spans the base's LOWEST set bit. Every address in it already
// carries every bit of the base -- adding less than the lowest set bit cannot
// clear one -- and none carries a bit above the base's run, so the OR is the
// identity there and the arena mask is clear throughout. Nothing outside this
// block can ever be a host address for a guest lane, however much address
// space iOS would hand over: the arithmetic is the ceiling, not the host.
// [0x7800000000, 0x8000000000) -- 32 GiB, and that is the entire budget.
#define K64_NATIVE_ALIAS_BLOCK_SPAN   (K64_NATIVE_LOW_ALIAS_BASE & \
                                       (~K64_NATIVE_LOW_ALIAS_BASE + 1ULL))
#define K64_NATIVE_ALIAS_BLOCK_END    (K64_NATIVE_LOW_ALIAS_BASE + \
                                       K64_NATIVE_ALIAS_BLOCK_SPAN)

// The high identity lane moves above the alias window and runs to the base of
// the relocated arena's host block, which is the next thing in that 32 GiB
// block. It stopped one K64_NATIVE_LOW_GUEST_LIMIT above its base before --
// not because anything required that, but because the invariant was stated as
// "the lane must not cross an alias-base bit boundary", which holds for a lane
// of exactly that size and refuses every larger one that is equally exact.
// Sixteen GiB of the block were never offered to the guest as a result.
#define K64_NATIVE_GUEST_IMAGE_BASE   K64_NATIVE_LOW_ALIAS_END

// Wine's top-down arena and the host block it is served from. Wine reserves
// [K64_NATIVE_TOP_GUEST_BASE, K64_NATIVE_TOP_GUEST_END) PROT_NONE and then
// commits accessible subranges inside it. The single OR cannot reach it --
// those addresses already carry the alias base's bits, so the OR is the
// identity and the host would have to map 0x7ffffe000000 itself. Bits 39..46
// are set in every arena address and clear in every other hostable one, so
// clearing that field is the whole relocation and a no-op elsewhere.
//
// The base and K64_NATIVE_GUEST_HIGH_END are the same number under the mask:
// the arena's host block begins exactly where the identity lane ends, so
// widening one narrows the other byte for byte. What they share is fixed by
// the arithmetic -- the alias block, less the low alias window, less the
// 64 KiB the arena's fixed top end leaves unused above its image, which is
// 24 GiB less 64 KiB in total.
//
// The arena was 32 MiB of that, and a device log shows Wine's
// try_map_free_area walking down from its own user-space limit and running out
// 320 KiB below the old base: it wanted more of this range than was on offer.
// It is 2 GiB now and the identity lane has the other 22. Neither is a figure
// Wine asked for; they are a split, and the witness in kmemory64.cpp is what
// will say whether it is the right one.
#define K64_NATIVE_TOP_GUEST_BASE     0x7FFF80000000ULL
#define K64_NATIVE_TOP_GUEST_END      0x7FFFFFFF0000ULL
#define K64_NATIVE_TOP_GUEST_LENGTH   (K64_NATIVE_TOP_GUEST_END - \
                                       K64_NATIVE_TOP_GUEST_BASE)
#define K64_NATIVE_TOP_CLEAR_MASK     0x7F8000000000ULL
#define K64_NATIVE_TOP_HOST_BASE      (K64_NATIVE_TOP_GUEST_BASE & \
                                       ~K64_NATIVE_TOP_CLEAR_MASK)
#define K64_NATIVE_TOP_HOST_END       (K64_NATIVE_TOP_HOST_BASE + \
                                       K64_NATIVE_TOP_GUEST_LENGTH)

// Derived, not chosen: the identity lane ends exactly where the arena's host
// block begins, so the alias block holds no gap the guest cannot be offered.
#define K64_NATIVE_GUEST_HIGH_END     K64_NATIVE_TOP_HOST_BASE

// Keep automatic mappings out of the executable's program-break growth lane.
// Starting mmap(NULL, ...) at IMAGE_BASE let ld-linux place libc immediately
// above a small PIE, so glibc's first brk expansion collided with libc. One
// GiB is ample initial brk space; larger allocations naturally use mmap.
#define K64_NATIVE_GUEST_HEAP_LIMIT   (K64_NATIVE_GUEST_IMAGE_BASE + 0x40000000ULL)
#define K64_NATIVE_GUEST_MMAP_BASE    K64_NATIVE_GUEST_HEAP_LIMIT
// Darwin/iOS native mappings use 16 KiB host pages while the x86-64 guest uses
// 4 KiB pages. The initial break must not share the executable's final host
// page, because extending that partially tracked mapping cannot be done with a
// destructive MAP_FIXED operation.
#define K64_NATIVE_GUEST_LAYOUT_ALIGN 0x4000ULL
#define K64_NATIVE_GUEST_INTERP_BASE  (K64_NATIVE_GUEST_HIGH_END - 0x80000000ULL)
#define K64_NATIVE_GUEST_STACK_TOP    (K64_NATIVE_GUEST_HIGH_END - 0x100000ULL)
#define K64_NATIVE_GUEST_TLS_BASE     (K64_NATIVE_GUEST_STACK_TOP - 0x900000ULL)

#if defined(__cplusplus)
// The pure mirror the tests and the translator patch share. Cross-checked here
// so the layout below and the arithmetic they use cannot drift apart.
#include "guest_low_alias.h"
static_assert(K64_NATIVE_LOW_ALIAS_BASE == boxedvn::kGuestLowAliasBase,
              "alias base must match the shared translation contract");
static_assert(K64_NATIVE_LOW_GUEST_LIMIT == boxedvn::kGuestLowLimit,
              "low guest limit must match the shared translation contract");
static_assert(K64_NATIVE_GUEST_IMAGE_BASE == boxedvn::kGuestHighBase,
              "high lane base must match the shared translation contract");
static_assert(K64_NATIVE_GUEST_HIGH_END == boxedvn::kGuestHighEnd,
              "high lane end must match the shared translation contract");
static_assert((K64_NATIVE_GUEST_WINDOW_START & K64_PAGE_MASK) == 0,
              "native guest window start must be guest-page aligned");
static_assert((K64_NATIVE_GUEST_WINDOW_END & K64_PAGE_MASK) == 0,
              "native guest window end must be guest-page aligned");
static_assert((K64_NATIVE_GUEST_WINDOW_START & 0x3FFFULL) == 0 &&
              (K64_NATIVE_GUEST_WINDOW_END & 0x3FFFULL) == 0,
              "native guest window must be 16 KiB host-page aligned");
static_assert(K64_NATIVE_GUEST_WINDOW_START < K64_NATIVE_GUEST_WINDOW_END,
              "native guest window must have positive size");
// The whole aliased layout has to live inside the proven host window.
static_assert(K64_NATIVE_LOW_ALIAS_BASE >= K64_NATIVE_GUEST_WINDOW_START,
              "low alias window must start inside the proven host window");
static_assert(K64_NATIVE_GUEST_HIGH_END <= K64_NATIVE_GUEST_WINDOW_END,
              "aliased layout must end inside the proven host window");
static_assert(K64_NATIVE_LOW_ALIAS_END == K64_NATIVE_GUEST_IMAGE_BASE,
              "high identity lanes must begin exactly above the alias window");
// OR-translation correctness. Low guest addresses must share no bit with the
// alias base, so `guest | base` equals `guest + base`.
static_assert((K64_NATIVE_LOW_GUEST_LIMIT &
               (K64_NATIVE_LOW_GUEST_LIMIT - 1)) == 0,
              "low guest limit must be a power of two");
static_assert((K64_NATIVE_LOW_ALIAS_BASE &
               (K64_NATIVE_LOW_GUEST_LIMIT - 1)) == 0,
              "alias base must be aligned to the low guest limit so OR adds");
// Every permitted high guest address must already carry the base's bits, so
// OR is the identity for them and identity mapping is preserved.
static_assert((K64_NATIVE_GUEST_IMAGE_BASE & K64_NATIVE_LOW_ALIAS_BASE) ==
              K64_NATIVE_LOW_ALIAS_BASE,
              "high identity lane base must contain the alias base bits");
static_assert(((K64_NATIVE_GUEST_HIGH_END - 1) & K64_NATIVE_LOW_ALIAS_BASE) ==
              K64_NATIVE_LOW_ALIAS_BASE,
              "high identity lane end must contain the alias base bits");
// The identity lane's correctness proof, replacing the older
// "must not cross an alias-base bit boundary" test. For any A in
// [K64_NATIVE_LOW_ALIAS_BASE, K64_NATIVE_ALIAS_BLOCK_END),
// A = K64_NATIVE_LOW_ALIAS_BASE + d with d below the base's lowest set bit.
// Every base bit is at or above that bit, so the base's bits below it are zero
// across the whole of d and base + d == base | d; hence A & base == base and
// the OR is the identity for EVERY address in the lane, not merely at its two
// ends. A also stays below the block end, so it carries no bit above the
// base's run and the arena mask is clear throughout. The old test asserted a
// condition sufficient only for a lane of exactly K64_NATIVE_LOW_GUEST_LIMIT
// bytes; containment is the invariant that actually holds.
static_assert(K64_NATIVE_ALIAS_BLOCK_SPAN != 0 &&
              (K64_NATIVE_LOW_ALIAS_BASE &
               (K64_NATIVE_ALIAS_BLOCK_SPAN - 1)) == 0,
              "the block span must be the alias base's lowest set bit");
static_assert(K64_NATIVE_GUEST_IMAGE_BASE >= K64_NATIVE_LOW_ALIAS_BASE &&
              K64_NATIVE_GUEST_IMAGE_BASE < K64_NATIVE_GUEST_HIGH_END &&
              K64_NATIVE_GUEST_HIGH_END <= K64_NATIVE_ALIAS_BLOCK_END,
              "the identity lane must lie inside the alias base's own block");
static_assert(K64_NATIVE_ALIAS_BLOCK_SPAN == boxedvn::kGuestAliasBlockSpan,
              "the alias block span must match the shared translation "
              "contract");
static_assert(K64_NATIVE_ALIAS_BLOCK_END == boxedvn::kGuestAliasBlockEnd,
              "the alias block end must match the shared translation contract");
static_assert((K64_NATIVE_GUEST_MMAP_BASE & (K64_NATIVE_GUEST_LAYOUT_ALIGN - 1)) == 0,
              "native mmap base must satisfy the iOS host-page layout contract");
static_assert(K64_NATIVE_GUEST_IMAGE_BASE < K64_NATIVE_GUEST_HEAP_LIMIT,
              "native guest heap lane must have positive size");
static_assert(K64_NATIVE_GUEST_HEAP_LIMIT == K64_NATIVE_GUEST_MMAP_BASE,
              "native automatic mappings must begin after the heap lane");
static_assert(K64_NATIVE_GUEST_INTERP_BASE > K64_NATIVE_GUEST_MMAP_BASE,
              "native interpreter base must follow the mmap region");
static_assert(K64_NATIVE_GUEST_STACK_TOP > K64_NATIVE_GUEST_INTERP_BASE,
              "native stack must sit above the interpreter lane");
static_assert(K64_NATIVE_GUEST_TLS_BASE > K64_NATIVE_GUEST_INTERP_BASE,
              "native TLS lane must sit above the interpreter lane");
// KUSER_SHARED_DATA is inside the canonical low range, so it resolves through
// the same alias as every other low address rather than a second mapping.
static_assert(K64_KUSER_SHARED_BASE + K64_KUSER_SHARED_SIZE <=
              K64_NATIVE_LOW_GUEST_LIMIT,
              "KUSER_SHARED_DATA must resolve through the low alias");
// The top lane must agree with the shared translation header exactly, or the
// emulator and the translator would compute different host addresses.
static_assert(K64_NATIVE_TOP_GUEST_BASE == boxedvn::kGuestTopBase,
              "top arena base must match the shared alias contract");
static_assert(K64_NATIVE_TOP_GUEST_END == boxedvn::kGuestTopEnd,
              "top arena end must match the shared alias contract");
static_assert(K64_NATIVE_TOP_CLEAR_MASK == boxedvn::kGuestTopClearMask,
              "top clear mask must match the shared alias contract");
static_assert(K64_NATIVE_TOP_HOST_BASE == boxedvn::kGuestTopHostBase,
              "top host alias base must match the shared alias contract");
static_assert(K64_NATIVE_TOP_HOST_BASE >= K64_NATIVE_GUEST_HIGH_END,
              "the top host alias must sit above the identity lane");
static_assert(K64_NATIVE_TOP_HOST_END <= K64_NATIVE_GUEST_WINDOW_END,
              "the top host alias must stay inside the proven host window");
// The arena's translation is exact and injective for the same reason the
// identity lane's is. Every mask bit is set across the arena, so
// guest & ~mask == guest - K64_NATIVE_TOP_CLEAR_MASK, sweeping
// [K64_NATIVE_TOP_HOST_BASE, K64_NATIVE_TOP_HOST_END) one address for one.
// Requiring that image to sit inside the alias block makes
// (guest | base) & ~mask == (guest & ~mask) | base == guest & ~mask: the OR
// contributes nothing, the subtraction is the whole relocation, and no two
// arena addresses can land on one host address.
static_assert(K64_NATIVE_TOP_HOST_BASE >= K64_NATIVE_LOW_ALIAS_BASE &&
              K64_NATIVE_TOP_HOST_END <= K64_NATIVE_ALIAS_BLOCK_END,
              "the relocated arena's host image must lie inside the alias "
              "block, or clearing the mask would not be exact");
static_assert(K64_NATIVE_TOP_HOST_BASE == K64_NATIVE_GUEST_HIGH_END,
              "the identity lane must end exactly where the arena's host "
              "block begins");
static_assert(K64_NATIVE_TOP_GUEST_BASE > K64_NATIVE_GUEST_HIGH_END,
              "the arena must sit above the identity lane");
#endif

// Translate a canonical guest address to the host address its bytes live at.
// Identity for every permitted high address by construction; the single OR is
// what the ARM64 translator emits for guest dereferences as well.
static inline int k64IsTopArenaGuestAddress(U64 guestAddress) {
    return (guestAddress >= K64_NATIVE_TOP_GUEST_BASE &&
            guestAddress < K64_NATIVE_TOP_GUEST_END) ? 1 : 0;
}

static inline int k64IsTopArenaHostAddress(U64 hostAddress) {
    return (hostAddress >= K64_NATIVE_TOP_HOST_BASE &&
            hostAddress < K64_NATIVE_TOP_HOST_END) ? 1 : 0;
}

static inline U64 k64GuestToHostAddress(U64 guestAddress) {
    return (guestAddress | K64_NATIVE_LOW_ALIAS_BASE) &
           ~K64_NATIVE_TOP_CLEAR_MASK;
}

// Recover the canonical guest address from a host address inside the alias
// window. Host addresses outside it are returned unchanged, so this is safe to
// apply to a fault address of unknown provenance.
static inline U64 k64HostToGuestAddress(U64 hostAddress) {
    if (hostAddress >= K64_NATIVE_LOW_ALIAS_BASE &&
        hostAddress < K64_NATIVE_LOW_ALIAS_END) {
        return hostAddress - K64_NATIVE_LOW_ALIAS_BASE;
    }
    // The top alias sits above the identity lane, so it can never be mistaken
    // for a valid identity-lane guest address.
    if (k64IsTopArenaHostAddress(hostAddress)) {
        return hostAddress | K64_NATIVE_TOP_CLEAR_MASK;
    }
    return hostAddress;
}

// True when the canonical guest address is served through the alias rather
// than dereferenced at its own address.
static inline int k64IsLowAliasedGuestAddress(U64 guestAddress) {
    return guestAddress < K64_NATIVE_LOW_GUEST_LIMIT ? 1 : 0;
}

// ---- Host-pointer cache generation --------------------------------------
//
// Two caches hold a raw host pointer to a guest page's backing buffer: the
// per-thread data TLB in kmemory64.cpp and CPU64's instruction-fetch page
// cache. Both are keyed by page number, so an event that frees, moves or
// withdraws a buffer has to retire them.
//
// This counter is the ONLY cross-thread signal for that. Nothing ever reaches
// into another thread's cache to clear it: the old fetch-cache invalidation
// did exactly that (`fetchCachePage = -1; fetchCacheData = nullptr;` written
// into every sibling thread's CPU64 from whichever thread was mapping), and a
// sibling running guest code could read the still-matching page number and
// then the already-nulled data pointer — a NULL base indexed by an in-page
// offset, which is the bare page-offset SIGSEGV inside CPU64::step. A cache
// entry carries the generation it was filled at and is usable only while that
// still matches, so an invalidation is one atomic increment and every reader
// stays inside its own thread's memory.
// Defined in kmemory64.cpp. Read through the inline accessors below so the
// instruction-fetch fast path stays a load and a compare with no call.
extern std::atomic<U32> g_k64PageCacheGeneration;

inline U32 k64PageCacheGeneration() {
    return g_k64PageCacheGeneration.load(std::memory_order_acquire);
}
inline void k64InvalidatePageCaches() {
    g_k64PageCacheGeneration.fetch_add(1, std::memory_order_release);
}

// ---- Who last wrote a guest page's rights -------------------------------
//
// The failure this file keeps producing is always the same shape: a page the
// guest believes it committed reads back as K64_PAGE_MAPPED with no access
// bits, and nothing in the log says which call put it in that state. Every
// write of K64Page::flags therefore records its operation, the value it wrote
// and a wrapping order stamp -- four bytes per page, no log volume of its own.
// The bounded fault witness prints them when a guest access is finally
// refused, which is the only moment the answer is needed.
enum K64PageWriter : U8 {
    K64_WRITER_NONE = 0,
    K64_WRITER_MMAP_ANON = 1,   // mmapAnonymousFixed (incl. PROT_NONE reserve)
    K64_WRITER_MMAP_FILE = 2,   // mmapSharedFile
    K64_WRITER_MPROTECT = 3,
    K64_WRITER_MUNMAP = 4,
    K64_WRITER_COMMIT = 5,      // first touch through a kernel write path
    K64_WRITER_PIN = 6,         // getRamPtr handing out a host pointer
    K64_WRITER_CLONE = 7,       // fork snapshot
    K64_WRITER_COUNT = 8
};

const char* k64PageWriterName(U8 writer);

// A guest page slot. `data` is allocated lazily: a freshly reserved page (mmap
// of an address wine may never touch — its huge PROT_NONE reservations) carries
// no backing buffer (data==nullptr) and reads as zero, so host RAM tracks pages
// actually TOUCHED, not address space RESERVED. The buffer, once allocated, is
// never reallocated while live, so a host pointer handed out by getRamPtr (the
// 64-bit futex table keys on it) stays stable until the page is decommitted.
struct K64Page {
    U8* data = nullptr;
    U32 flags = 0;
    // Provenance of the last write to `flags`. Sits in the padding after it.
    U8  lastWriter = K64_WRITER_NONE;
    U8  lastFlags = 0;          // K64_PAGE_* fit in eight bits
    U16 lastWriteStamp = 0;     // wrapping; orders two recent writers
    // The write BEFORE the last one that actually changed the value. A page
    // reserved PROT_NONE and a page committed and then freed back to a
    // reservation are indistinguishable from lastWriter alone: both read
    // mmap-anon with K64_PAGE_MAPPED and nothing else. This says which, and
    // it is the difference between a wild guest pointer into address space
    // nothing ever used and a use-after-free of memory the guest DID commit.
    // Only a write that changes `flags` shifts it, so re-reserving an already
    // reserved page cannot erase the commit that came before.
    U8  priorWriter = K64_WRITER_NONE;
    U8  priorFlags = 0;
    U16 priorWriteStamp = 0;
    // When set, `data` is BORROWED from a process-shared file mapping (wine's
    // server shared-memory section: MAP_SHARED writable file). The buffer is
    // owned by g_sharedFileRegistry and aliased by every process that maps the
    // same file page, so wineserver's writes are visible to clients and vice
    // versa. We must NOT delete[] or reallocate a borrowed buffer.
    bool dataShared = false;
    // Shared mappings keep an indirection slot so an interpreter-backed page
    // can be promoted to the FEX process's fixed identity address later. All
    // sparse aliases then observe the new backing without retaining pointers
    // to destroyed K64Page objects.
    std::shared_ptr<std::atomic<U8*>> sharedData;
    // Stable canonical heap page for a shared-file mapping. Native identity
    // promotion temporarily points sharedData at a fixed host address; this
    // lets MAP_FIXED/munmap copy the page back before that address is reused.
    U8* sharedCanonical = nullptr;
    // When set, `data` points into an OS mmap at the guest address and is not
    // owned by this slot. The KMemory64 owner unmaps native ranges first.
    bool dataNative = false;
    ~K64Page() { if (!dataShared && !dataNative) delete[] data; }
    // The ONLY way `flags` is written. Going through one function is what makes
    // "which operation last changed this page's rights" answerable at all, and
    // what keeps a rights change tied to an operation that named the page.
    void writeFlags(U32 value, U8 writer, U16 stamp) {
        if (value != flags) {
            priorWriter = lastWriter;
            priorFlags = lastFlags;
            priorWriteStamp = lastWriteStamp;
        }
        flags = value;
        lastWriter = writer;
        lastFlags = (U8)(value & 0xFFu);
        lastWriteStamp = stamp;
    }
    U8* hostData() const { return sharedData ? sharedData->load(std::memory_order_acquire) : data; }
    // Allocate + zero the backing buffer on first write/commit. Idempotent.
    U8* commit() {
        if (!hostData()) {
            data = new U8[K64_PAGE_SIZE];
            ::memset(data, 0, K64_PAGE_SIZE);
        }
        return hostData();
    }
    // Point this page at a process-shared backing buffer (not owned here).
    void adoptShared(const std::shared_ptr<std::atomic<U8*>>& shared, bool native = false,
                     U8* canonical = nullptr) {
        if (data && !dataShared && !dataNative) delete[] data;
        sharedData = shared;
        data = shared ? shared->load(std::memory_order_acquire) : nullptr;
        dataShared = true;
        dataNative = native;
        sharedCanonical = canonical;
    }
    // Point this page at its identity-mapped host address (not owned here).
    void adoptNative(U8* native) {
        if (data && !dataShared && !dataNative) delete[] data;
        data = native;
        dataShared = false;
        dataNative = true;
        sharedData.reset();
        sharedCanonical = nullptr;
    }
    // Drop the backing buffer (native munmap or sparse PROT_NONE). The slot
    // survives; sparse mode later reads zero and re-commits on write. A
    // borrowed shared buffer is detached (not freed — the registry owns it).
    void decommit() {
        if (!dataShared && !dataNative) delete[] data;
        data = nullptr;
        dataShared = false;
        dataNative = false;
        sharedData.reset();
        sharedCanonical = nullptr;
    }
    // Copy an identity-promoted shared page back to its canonical heap page
    // before the fixed host mapping is unmapped or replaced.
    void demoteNativeShared() {
        if (!dataShared || !dataNative || !sharedData || !sharedCanonical) return;
        U8* current = sharedData->load(std::memory_order_acquire);
        if (current && current != sharedCanonical) {
            ::memcpy(sharedCanonical, current, K64_PAGE_SIZE);
        }
        sharedData->store(sharedCanonical, std::memory_order_release);
        data = sharedCanonical;
        dataNative = false;
    }
    bool committed() const { return hostData() != nullptr; }
};

// ---- Guest-lane host fault repair ---------------------------------------
//
// A native-identity address space keeps TWO ledgers for the same bytes and
// they have different granularity:
//
//   * `pages`, keyed by 4 KiB CANONICAL guest page, is what the guest -- and
//     every kernel path -- believes about a page: mapped or not, and with
//     which K64_PAGE_READ/WRITE bits.
//   * the host VM, plus `nativeRanges`, is what the hardware will actually
//     permit. iOS host pages are 16 KiB, so ONE host page carries FOUR guest
//     pages whose guest permissions need not agree, and mmap/mprotect/munmap
//     have to reconstruct a safe union for it every time any of the four
//     changes.
//
// FEX dereferences guest addresses directly through the alias (see
// guest_low_alias.h), so it consults neither `pages` nor the K64_PAGE_* bits:
// it takes whatever the host VM says. When the two ledgers disagree the guest
// gets a host fault for a page it is entitled to read, which Wine translates
// into STATUS_ACCESS_VIOLATION / STATUS_DATATYPE_MISALIGNMENT and the process
// dies. The interpreter never saw this: it reads through `pages` and merely
// logs BOXEDWINE_X64_SPARSE_PAGE_MISSING for a page with no buffer.
//
// nativeRepairHostFault is the reconciliation, driven from the signal handler
// with the page table as the authority: it raises the host page to exactly
// the union its guest subpages are entitled to and to nothing more, so a
// genuine guest access violation still reaches the guest as one.
//
// How far either side of a REFUSED fault the committed neighbourhood below is
// looked for: 1024 guest pages, 4 MiB. Big enough to bracket the heap block a
// walking loop overran, small enough that the bounded scan stays two thousand
// map lookups on a path that is about to kill the process anyway.
#define K64_FAULT_NEIGHBOURHOOD_PAGES 1024

// The report the repair fills in, whether it repairs or refuses.
struct K64NativeFaultRepair {
    // The canonical guest address the faulting host address is the image of.
    U64 guestAddress = 0;
    // The enclosing HOST page -- the granule the repair can act on.
    U64 hostPageStart = 0;
    U64 hostPageLength = 0;
    // The host address is the image of a guest address this address space
    // serves. False for the emulator's own heap and the translated code arena.
    bool inGuestLane = false;
    // The page table's verdict on that guest page, and the K_PROT_* bits it
    // grants. These are the numbers that decide whether a repair is legal.
    bool pageMapped = false;
    U32 guestProt = 0;
    // The union the whole host page is entitled to, over its guest subpages.
    U32 hostPageProt = 0;
    // What the OTHER ledger said before the repair: whether a host region
    // exists there at all, and what it permitted. This is what distinguishes
    // "no host page" from "host page present but PROT_NONE".
    bool hostPresent = false;
    U32 hostProtBefore = 0;
    // Whether nativeRanges already owned the host page.
    bool tracked = false;
    // The producing side: which operation last wrote the faulting guest page's
    // flags, what it wrote, and in what order relative to its neighbours. A
    // refusal fills these too -- that is the case they exist for.
    const char* lastWriter = "none";
    U32 lastWriteFlags = 0;
    U32 lastWriteStamp = 0;
    // The same three for the host page's other guest subpages, so a rights loss
    // confined to one 4 KiB page inside a 16 KiB one is visible as such.
    const char* neighbourWriter = "none";
    U32 neighbourWriteFlags = 0;
    U32 neighbourWriteStamp = 0;
    // The write before the last CHANGE to the faulting page's flags. lastWriter
    // says "mmap-anon wrote K64_PAGE_MAPPED" for a page that was reserved and
    // never used AND for a page that was committed, freed, and re-reserved --
    // the same three fields, the same values. These separate them: prior rights
    // that include K64_PAGE_READ mean the guest DID commit this page once and
    // something took it back, which is a use-after-free and not a wild pointer.
    const char* priorWriter = "none";
    U32 priorWriteFlags = 0;
    U32 priorWriteStamp = 0;
    // The committed neighbourhood of a REFUSED fault, within
    // K64_FAULT_NEIGHBOURHOOD_PAGES either way. `committedBelow` is the first
    // address the guest could NOT read below the fault -- the end of whatever
    // buffer a walking loop ran off -- and `committedAbove` is the base of the
    // next readable page. Zero means nothing readable within the window. A
    // fault whose two ends bracket a small hole is a loop that overran its
    // buffer; one with nothing either way is a pointer into address space the
    // guest never used at all.
    U64 committedBelow = 0;
    U64 committedAbove = 0;
    // What the repair did.
    bool materialised = false;   // a host page was created here
    bool reprotected = false;    // an existing host page's protection changed
    const char* decision = "none";
};

class KMemory64 {
public:
    explicit KMemory64(KProcess* process, bool nativeIdentity = false);
    // Registered by the translator backend. Receives every guest range that
    // has been unmapped or replaced, so translated blocks compiled from the
    // bytes that used to be at those addresses are dropped. The translator
    // caches blocks by guest address and never saw these changes before: a
    // library unmapped and another mapped in its place kept running the first
    // library's translation. The interpreter's fetch cache is invalidated
    // separately. Unregistered, or for a process the backend does not run,
    // nothing happens.
    static void setTranslatedCodeInvalidator(
        void (*invalidator)(KProcess* process, U64 addr, U64 len));
    // The mapping paths only queue: several of them run under mmapMutex, and
    // the backend's invalidation takes the translator's code lock, which a
    // compiling thread may hold while it asks this object about pages. The
    // syscall exit and the run entry drain the queue with no kernel lock held.
    void flushTranslatedCodeInvalidations();
    ~KMemory64();

    // True only when this build was explicitly compiled with native identity
    // mappings. The default sparse implementation returns false.
    bool nativeIdentityMode() const;

    // A process-unique, monotonically increasing id for THIS address space.
    // execve replaces the KMemory64 wholesale (see KProcess::exec), and fork
    // gives the child its own, so this is the exec generation: two runs of the
    // same pid before and after the Windows handoff are different ids.
    // Diagnostics key their budgets on it so a short-lived helper process
    // cannot spend the evidence budget belonging to the address space that is
    // actually failing.
    U64 addressSpaceGeneration() const { return generation; }

    // Guard a host MAP_FIXED operation before it reaches the OS. Sparse mode
    // accepts every canonical guest range; native identity mode accepts only
    // the proven iOS window (plus the special KUSER shared-data alias).
    bool nativeGuestRangeAllowed(U64 addr, U64 len) const;

    // Whether [start, end) of HOST address space is already tracked by this
    // address space. Public only so the mapping planner in native_map_plan.h
    // can ask; it is the same question nativeRangeCovers answers.
    bool nativeRangeCoversForPlan(U64 start, U64 end) const;

    // Return the host alias for a canonical KUSER_SHARED_DATA address, or 0
    // when this address space is sparse / has not mapped the alias. This is
    // intentionally narrow; it is not a general guest-pointer translation.
    U64 nativeAliasForGuest(U64 guestAddress) const;

    // Reconcile the two ledgers for the host page a translated guest access
    // faulted on. `hostFaultAddress` is the raw si_addr of the host fault;
    // `requiredProt` is the minimum K_PROT_* the guest page must already
    // grant for a repair to be legal (the translator cannot tell the signal
    // handler whether the access was a load or a store, so callers pass
    // K_PROT_READ and let a store to a read-only page fault again against the
    // restored, still-read-only host page).
    //
    // Returns true only when the faulting instruction should be retried in
    // place. Every refusal -- not native, not a guest-lane address, the guest
    // page genuinely unmapped, the guest page genuinely inaccessible, or the
    // host operation itself failing -- returns false so the fault stays the
    // guest fault it was. `report` is filled in either way and is what the
    // BOXEDWINE_X64_ALIAS_BACKING witness prints.
    bool nativeRepairHostFault(U64 hostFaultAddress, U32 requiredProt,
                               K64NativeFaultRepair& report);

    // mmap subset: anonymous + fixed only in v1. Returns the mapped guest
    // address, or (U64)-errno on failure. addr MUST be page-aligned and
    // non-zero (no address-picking in v1).
    U64 mmapAnonymousFixed(U64 addr, U64 len, U32 prot);

    // Map a MAP_SHARED file region so it is genuinely SHARED across every process
    // that maps the same file (keyed by path). Each guest page in [addr,addr+len)
    // adopts a process-global backing buffer from g_sharedFileRegistry, seeded
    // once from the file contents at [fileOffset,...). wine's server shared-memory
    // section (server-1-*/tmpmap-*) relies on this: wineserver writes object/
    // sequence state that clients read directly. Without real sharing the section
    // desyncs → wineserver release_object refcount underflow. addr is page-aligned.
    U64 mmapSharedFile(U64 addr, U64 len, U32 prot, const char* path, U64 fileOffset,
                       const U8* fileBytes, U64 fileBytesLen);

    // Map [addr, addr+len) at exactly that address, but only while every page
    // in it is unmapped. This is MAP_FIXED_NOREPLACE: it never relocates and
    // never replaces. The occupancy test and the mapping happen under the same
    // allocator lock, so a sibling thread cannot claim the range in between --
    // a check-then-MAP_FIXED sequence would be a TOCTOU race whose loser
    // silently destroys the winner's mapping.
    //
    // Returns addr on success, (U64)-K_EEXIST when any page of the range is
    // already mapped or reserved, (U64)-K_ENOMEM when (in native identity
    // mode) the exact range lies outside every hostable lane, and
    // (U64)-errno otherwise. The two refusals are deliberately distinct:
    // EEXIST means "something is there, try elsewhere" and a placement search
    // answers it by stepping to the next address, so it must never be used
    // for an empty range whose address simply cannot be backed.
    U64 mmapAnonymousNoReplace(U64 addr, U64 len, U32 prot);

    // True when no page in [addr, addr+len) is mapped at all, reservations
    // included. This is the occupancy question MAP_FIXED_NOREPLACE asks, and
    // it is deliberately stricter than the "no accessible page" test an
    // ordinary hint uses.
    bool rangeCompletelyUnmapped(U64 addr, U64 len) const;

    // ---- Sparse inaccessible reservations -------------------------------
    //
    // Wine reserves multi-gigabyte arenas it never touches, at high addresses
    // the native identity window cannot host. Refusing them is what made Wine
    // halve the request and retry without end: one device run issued
    // 8,916,993 mmaps and rejected 8,916,842 of them at about 98% of a core.
    //
    // An inaccessible anonymous range has nothing to read, nothing to execute
    // and no pointer FEX could ever be handed, so it needs no host mapping and
    // no K64Page per 4 KiB. It is recorded as one interval, whatever its size.
    // These live outside the native window by construction, so nothing here
    // changes identity mappings, the low alias, or any accessible mapping.

    // Record [addr, addr+len) as reserved. Returns `addr` on success, or
    // -K_EEXIST when any part of the range is already mapped or reserved.
    // Costs one interval regardless of length; no page objects are created.
    U64 reserveSparseNoReplace(U64 addr, U64 len);

    // True when any page of [addr, addr+len) falls inside a reservation.
    bool sparseReservationOverlaps(U64 addr, U64 len) const;

    // Remove [addr, addr+len) from the reservations, splitting an interval it
    // falls inside and trimming the ones it partially covers, so the address
    // space is genuinely reusable. Returns true when anything was removed.
    bool releaseSparseReservation(U64 addr, U64 len);

    // Live interval count and total reserved pages. For tests and diagnostics:
    // the interval count is what proves the representation is sparse.
    U64 sparseReservationCount() const;
    U64 sparseReservationPages() const;

    // Atomically pick a free address range AND map it, so two guest threads of
    // the same process (sharing this KMemory64) can never be handed overlapping
    // mmap(NULL,...) placements. The old split — allocMmapRange() scans for a
    // gap, returns, then the caller mmapAnonymousFixed()s it — is a TOCTOU race
    // under BOXEDWINE_MULTI_THREADED: both threads scan, both see the same gap
    // free (neither has mapped yet), both map there, and the second map ZEROES
    // the first → "malloc(): corrupted double linked list" in the guest heap
    // (wineserver during the boot storm). Holding mmapMutex across scan+map
    // closes it. `length` need not be page-aligned (rounded up). Returns the
    // page-aligned base. prot follows K_PROT_* (0x1/0x2/0x4).
    U64 mmapReserveAndMap(U64 length, U32 prot);

    // Change page permissions on already-mapped pages. addr must be page-
    // aligned. prot bits follow K_PROT_READ/WRITE/EXEC (0x1/0x2/0x4) — same
    // convention as mmapAnonymousFixed. Pages within [addr, addr+len) that
    // are NOT mapped are silently skipped (matches the Linux semantics
    // where partial mprotect on holes returns -ENOMEM, but our v1 callers
    // only invoke this against fully-mapped ranges — RELRO enforcement on
    // the GOT, and eventually JIT page-write tracking).
    //
    // Returns the requested addr on success, (U64)-errno on failure. Does NOT
    // create missing mappings. Used by the loader to honor PT_GNU_RELRO after
    // relocations.
    U64 mprotect(U64 addr, U64 len, U32 prot);

    // Unmap [addr, addr+len): drop the backing pages so the address range is
    // genuinely free for reuse. A no-op sparse munmap (the old behaviour)
    // breaks wine:
    // wine munmaps a view, removes it from its own views_tree, then maps a fresh
    // view at the SAME address — but if the pages are still mapped, wine's
    // create_view finds an overlapping NON-system view and aborts
    // (virtual.c:1578 "assert(view->protect & VPROT_SYSTEM)"). addr must be
    // page-aligned; partial pages at the tail are unmapped whole. Returns 0.
    U64 munmap(U64 addr, U64 len);

    // Permission query / page state.
    bool isPageMapped(U64 pageNum) const;
    U32  getPageFlags(U64 pageNum) const;

    // Return the committed backing buffer for `pageNum`, or nullptr if the page
    // is absent/uncommitted. Used by the CPU64 instruction-fetch cache to grab a
    // stable per-page pointer once (under the lock) and then read bytes from it
    // directly. The returned pointer stays valid until the page is decommitted
    // (native logical munmap may clear it) or the process tears down. Does NOT commit a
    // fresh page — an uncommitted page reads as zero, and code never executes
    // from a never-written page, so returning nullptr (caller falls back to
    // readb) is correct.
    U8* getCommittedPagePtr(U64 pageNum);

    // Bulk copy host -> guest and guest -> host. Allocates target pages
    // if not yet present (zero-fills). Caller is responsible for permission
    // checks at the kernel layer if it cares.
    void memcpyToGuest(U64 dstGuest, const void* src, U64 len);
    void memcpyFromGuest(void* dst, U64 srcGuest, U64 len);
    void memsetGuest(U64 dstGuest, U8 value, U64 len);

    // Scalar accessors. Single-byte/word/dword/qword. No alignment check;
    // crossing a page boundary is handled by going through memcpy.
    U8  readb(U64 addr);
    U16 readw(U64 addr);
    U32 readd(U64 addr);
    U64 readq(U64 addr);
    void writeb(U64 addr, U8 value);
    void writew(U64 addr, U16 value);
    void writed(U64 addr, U32 value);
    void writeq(U64 addr, U64 value);

    // Stable host pointer for a guest address range. The page backing store
    // is a per-page unique_ptr<K64Page> whose data[] never moves for the life
    // of the mapping, so the returned pointer is stable and usable as a
    // cross-thread key (this is what the futex table keys on). The page is
    // allocated on demand if not yet present. Returns nullptr only if the
    // range would cross a page boundary (callers — futex words — are always
    // 4-byte aligned within a 4096-byte page, so this never happens in
    // practice; the check guards against a misaligned caller).
    U8* getRamPtr(U64 addr, U32 len);

    // Deep-copy every mapped page from `from` into this (empty) address space.
    // Used by fork (KProcess::clone64 non-thread): the child gets an independent
    // snapshot of the parent's memory. Not copy-on-write — a plain page copy —
    // which is correct (if not minimal) and fine for wine's fork-then-execve
    // pattern where the child replaces its image almost immediately.
    void cloneFrom(const KMemory64* from);

    // Diagnostics
    U64 mappedPageCount() const { return (U64)pages.size(); }
    // Pages that hold a committed backing buffer. Until lazy commit (Phase 2)
    // every mapped page is committed, so this equals mappedPageCount(); afterward
    // it tracks only pages whose K64Page::data is non-null (i.e. actually touched).
    U64 committedPageCount() const;

#ifdef BOXEDWINE_BLOCK_CACHE_INFRA
    // ---- decoded-block-cache invalidation infra (Phase 1; see the block-
    // cache brief in the milestone notes). A future block executor caches
    // pre-decoded instruction runs keyed by RIP and must observe every store
    // that could touch cached code. Pages holding blocks REGISTER here; every
    // guest-write funnel calls noteGuestWrite*, which bumps the page slot's
    // generation. A cached block records its pages' generations at build time
    // and revalidates with one compare per page per execution. While no page
    // is registered (today: always), the write-path cost is a single relaxed
    // atomic load + branch. Slots are hashed, so collisions only cause false
    // invalidation — never a stale block.
    // Tables live at file scope in kmemory64.cpp (global, shared across all
    // address spaces — cross-process slot collisions only cause false
    // invalidation, never a stale block) so this object's hot-member layout
    // is untouched. The gate/check helpers are defined there too.
    static U32  blockSlot(U64 pageNum) { return (U32)((pageNum ^ (pageNum >> 12)) & 4095); }
    static void blockPageRegister(U64 pageNum);   // mark page as holding blocks
    static U32  blockPageGenOf(U64 pageNum);
    static void noteGuestWrite(U64 pageNum);
    static void noteGuestWriteRange(U64 addr, U64 len);
#else
    // Compiled out (measured ~4% on cpu_bench from code-layout displacement
    // alone — pay it only when the Phase 2 block executor buys it back).
    static void blockPageRegister(U64) {}
    static U32  blockPageGenOf(U64) { return 0; }
    static void noteGuestWrite(U64) {}
    static void noteGuestWriteRange(U64, U64) {}
#endif

    // BW64_STRAYWRITE tripwire (see kmemory64.cpp): logs a guest write whose
    // target page was never mapped/reserved — a stray write candidate for the
    // guest-heap corruption ASan can't see. No-op unless the env var is set.
    void strayWriteCheck(U64 dstGuest, U64 len);

    // BW64_MEMRING (see kmemory64.cpp): record a wineserver guest write + its
    // issuing RIP into a ring, dumped at the malloc/refcount abort to find the
    // write that corrupted a heap chunk. No-op unless the env var is set.
    void recordMemWrite(U64 addr, U64 len, U64 value);

private:
    KProcess* process;
    std::mutex pendingTranslatedInvalidationsMutex;
    std::vector<std::pair<U64, U64>> pendingTranslatedInvalidations;
    void queueTranslatedCodeInvalidation(U64 addr, U64 len);
    bool nativeIdentity = false;
    std::unordered_map<U64, std::unique_ptr<K64Page>> pages;
    // Guards every mutation/lookup of `pages`. In the multi-threaded build all
    // guest threads share one KMemory64 and fault in pages concurrently; an
    // unordered_map rehash on emplace racing a find from another thread is UB
    // (it manifested as one thread's guest store landing on a stale K64Page*,
    // corrupting another thread's stack → wild RIP / hang in mt_probe). The
    // page payload (K64Page::data) never moves once allocated, so we only need
    // to serialize map structure access, not the subsequent memcpy. Compiles
    // away to nothing in the single-threaded build. Mutable so the const
    // lookups (getPage/isPageMapped/getPageFlags) can lock.
    mutable BOXEDWINE_MUTEX pagesMutex;

    // Serializes mmap address-space allocation (mmapReserveAndMap): the gap scan
    // and the reservation map must be one atomic step across sibling threads.
    // Separate from pagesMutex so a long scan doesn't block unrelated page
    // faults. Process-wide bump cursor (replaces the old per-CPU64 mmapNext so
    // every thread of the process advances the same pointer). 0 = uninitialised.
    // Mutable for the same reason pagesMutex is: the const reservation
    // queries (sparseReservationOverlaps/Count/Pages) have to take it.
    mutable BOXEDWINE_MUTEX mmapMutex;
    U64 mmapNext = 0;

    // Assigned once at construction from a process-global counter. See
    // addressSpaceGeneration().
    U64 generation = 0;

    // Reserved address-space ranges drawn from the active mmap region (the
    // historical sparse base or K64_NATIVE_GUEST_MMAP_BASE).
    // Keyed by start PAGE number, ordered, so a gap search is O(log n + ranges
    // scanned) instead of the old O(pages) per-page isPageMapped() scan that
    // degraded badly as the page map grew (the boot slowdown). munmap removes /
    // trims entries here so the address space is genuinely reusable. Guarded by
    // mmapMutex (same lock as the gap scan it feeds). `start`/`pages` are page
    // units. kind distinguishes wine's PROT_NONE reservations from our committed
    // anon/file maps (used by Phase 2/3 to free backing store safely).
    enum MMapKind : U8 { MMAP_ANON = 0, MMAP_FILE = 1, MMAP_RESERVED = 2 };
    struct MMapRange { U64 startPage; U64 pageCount; U32 prot; U8 kind; };
    std::map<U64, MMapRange> ranges;

    // Inaccessible guest reservations outside the native window, as
    // non-overlapping intervals keyed by start PAGE number: startPage ->
    // pageCount. Deliberately separate from `ranges`, which feeds the
    // high-window gap scan; these addresses are never allocated from and must
    // not perturb that search. Guarded by mmapMutex.
    std::map<U64, U64> sparseReservations;

#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    struct NativeRange { U64 hostStart; U64 hostLength; U32 prot; };
    std::map<U64, NativeRange> nativeRanges;
    bool nativeMapAnonymous(U64 addr, U64 len, U32 prot, bool& fresh);
    bool nativeRangeCovers(U64 start, U64 end) const;
    void nativeForgetRange(U64 start, U64 length);
    // Record [start, start+length) as tracked with `prot`, coalescing with an
    // adjacent entry that already carries the same protection. Replaces the
    // "assign the new protection to every range that merely overlaps" loops,
    // which rewrote the protection of host pages the request never named.
    // Caller holds mmapMutex.
    void nativeTrackRangeLocked(U64 start, U64 length, U32 prot);
    // The union of what the guest subpages of ONE host page are entitled to.
    // The page map is the authority and this is its only reading: a host page
    // carries exactly this and nothing more, so a neighbour is never revoked
    // and the guest is never granted a right it does not hold. Caller holds
    // pagesMutex. `report` is optional and collects the provenance of the
    // subpages other than `excludePage` for the fault witness.
    U32 nativeHostPageProtLocked(U64 hostPageStart, U64 hostPageSize,
                                 U64 excludePage,
                                 K64NativeFaultRepair* report) const;
    // Fill report.committedBelow / report.committedAbove for a fault that is
    // about to be REFUSED: the nearest readable guest page on either side of
    // the faulting one, within K64_FAULT_NEIGHBOURHOOD_PAGES. Only the refusal
    // paths call it -- a repaired fault happens thousands of times a run and
    // must not pay for a scan. Caller holds pagesMutex.
    void nativeCommittedNeighbourhoodLocked(U64 guestPageNumber,
                                            K64NativeFaultRepair& report) const;
    // Bring every host page of [hostStart, hostEnd) to that union, one host
    // page at a time, after the guest page map has already been updated. Runs
    // of host pages that want the same protection are issued as one call. A
    // host page this address space does not track is SKIPPED, never failed:
    // the fault repair materialises it later from the very same union, and
    // refusing the whole request instead is how a committed page kept the
    // rights of the reservation it grew out of. Caller holds mmapMutex.
    void nativeReconcileHostProt(U64 hostStart, U64 hostEnd);
    void nativeDemoteSharedPages();
    void nativeUnmapAll();
    bool nativeMapKuserAlias(U64 addr, U64 len, bool& fresh);
    void nativeReleaseKuserAlias();
    bool nativeKuserAliasHeld = false;
#endif

    // Add/trim/remove range bookkeeping. Callers must hold mmapMutex.
    void rangeInsertLocked(U64 startPage, U64 pageCount, U32 prot, U8 kind);
    void rangeRemoveLocked(U64 startPage, U64 pageCount);

    // `writer` names the operation for the provenance record above; it applies
    // only when the page is actually created here.
    K64Page* getOrAllocPage(U64 pageNum, U32 flagsIfNew,
                            U8 writer = K64_WRITER_COMMIT);
    // Allocate (if absent) AND commit a page's backing buffer atomically under
    // pagesMutex; returns the committed buffer. Closes a lost-write race where two
    // host threads first-touching the same page each `new` a buffer and one
    // thread's write lands in an orphaned buffer (see definition).
    U8* commitPageLocked(U64 pageNum, U32 flagsIfNew,
                         U8 writer = K64_WRITER_COMMIT);
    K64Page* getPage(U64 pageNum) const;
};

// BW64_MEMRING dump (see kmemory64.cpp): print the recent wineserver guest-write
// ring, flagging any write near `nearAddr` (a malloc-dump corrupted-chunk
// candidate; pass 0 to skip correlation). Called from the abort path.
void kmemory64DumpMemRing(U64 nearAddr);

#endif // BOXEDWINE_GUEST_X64
#endif
