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

// Native-identity guests are directly dereferenced by the ARM64 translator,
// so their guest addresses must be host-mappable.  On iOS the malloc xzone
// reserves the broad 4 GiB..64 GiB band, while the executable arena occupies
// the range immediately below this window.  Keep all native guest mappings in
// the empirically safe interval below.  Sparse/interpreter guests retain
// their normal Linux-style addresses and do not use these limits.
#define K64_NATIVE_GUEST_WINDOW_START 0x7048000000ULL
#define K64_NATIVE_GUEST_WINDOW_END   0x7fffff0000ULL
#define K64_NATIVE_GUEST_MMAP_BASE    K64_NATIVE_GUEST_WINDOW_START
#define K64_NATIVE_GUEST_IMAGE_BASE  (K64_NATIVE_GUEST_WINDOW_START)
#define K64_NATIVE_GUEST_INTERP_BASE (K64_NATIVE_GUEST_WINDOW_END - 0x200000000ULL)
#define K64_NATIVE_GUEST_STACK_TOP   (K64_NATIVE_GUEST_WINDOW_END - 0x100000ULL)
#define K64_NATIVE_GUEST_TLS_BASE    (K64_NATIVE_GUEST_STACK_TOP - 0x900000ULL)

#if defined(__cplusplus)
static_assert((K64_NATIVE_GUEST_WINDOW_START & K64_PAGE_MASK) == 0,
              "native guest window start must be guest-page aligned");
static_assert((K64_NATIVE_GUEST_WINDOW_END & K64_PAGE_MASK) == 0,
              "native guest window end must be guest-page aligned");
static_assert((K64_NATIVE_GUEST_WINDOW_START & 0x3FFFULL) == 0 &&
              (K64_NATIVE_GUEST_WINDOW_END & 0x3FFFULL) == 0,
              "native guest window must be 16 KiB host-page aligned");
static_assert(K64_NATIVE_GUEST_WINDOW_START < K64_NATIVE_GUEST_WINDOW_END,
              "native guest window must have positive size");
static_assert(K64_NATIVE_GUEST_INTERP_BASE > K64_NATIVE_GUEST_MMAP_BASE,
              "native interpreter base must follow the mmap region");
#endif

// A guest page slot. `data` is allocated lazily: a freshly reserved page (mmap
// of an address wine may never touch — its huge PROT_NONE reservations) carries
// no backing buffer (data==nullptr) and reads as zero, so host RAM tracks pages
// actually TOUCHED, not address space RESERVED. The buffer, once allocated, is
// never reallocated while live, so a host pointer handed out by getRamPtr (the
// 64-bit futex table keys on it) stays stable until the page is decommitted.
struct K64Page {
    U8* data = nullptr;
    U32 flags = 0;
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

class KMemory64 {
public:
    explicit KMemory64(KProcess* process, bool nativeIdentity = false);
    ~KMemory64();

    // True only when this build was explicitly compiled with native identity
    // mappings. The default sparse implementation returns false.
    bool nativeIdentityMode() const;

    // Guard a host MAP_FIXED operation before it reaches the OS. Sparse mode
    // accepts every canonical guest range; native identity mode accepts only
    // the proven iOS window (plus the special KUSER shared-data alias).
    bool nativeGuestRangeAllowed(U64 addr, U64 len) const;

    // Return the host alias for a canonical KUSER_SHARED_DATA address, or 0
    // when this address space is sparse / has not mapped the alias. This is
    // intentionally narrow; it is not a general guest-pointer translation.
    U64 nativeAliasForGuest(U64 guestAddress) const;

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
    BOXEDWINE_MUTEX mmapMutex;
    U64 mmapNext = 0;

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

#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    struct NativeRange { U64 hostStart; U64 hostLength; U32 prot; };
    std::map<U64, NativeRange> nativeRanges;
    bool nativeMapAnonymous(U64 addr, U64 len, U32 prot, bool& fresh);
    bool nativeRangeCovers(U64 start, U64 end) const;
    void nativeForgetRange(U64 start, U64 length);
    void nativeDemoteSharedPages();
    void nativeUnmapAll();
    bool nativeMapKuserAlias(U64 addr, U64 len, bool& fresh);
    void nativeReleaseKuserAlias();
    bool nativeKuserAliasHeld = false;
#endif

    // Add/trim/remove range bookkeeping. Callers must hold mmapMutex.
    void rangeInsertLocked(U64 startPage, U64 pageCount, U32 prot, U8 kind);
    void rangeRemoveLocked(U64 startPage, U64 pageCount);

    K64Page* getOrAllocPage(U64 pageNum, U32 flagsIfNew);
    // Allocate (if absent) AND commit a page's backing buffer atomically under
    // pagesMutex; returns the committed buffer. Closes a lost-write race where two
    // host threads first-touching the same page each `new` a buffer and one
    // thread's write lands in an orphaned buffer (see definition).
    U8* commitPageLocked(U64 pageNum, U32 flagsIfNew);
    K64Page* getPage(U64 pageNum) const;
};

// BW64_MEMRING dump (see kmemory64.cpp): print the recent wineserver guest-write
// ring, flagging any write near `nearAddr` (a malloc-dump corrupted-chunk
// candidate; pass 0 to skip correlation). Called from the abort path.
void kmemory64DumpMemRing(U64 nearAddr);

#endif // BOXEDWINE_GUEST_X64
#endif
