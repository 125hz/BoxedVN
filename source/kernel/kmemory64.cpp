/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"
#include "kmemory64.h"
#include "cpu64.h"   // CPU64 full def — BW64_MEMRING reads the running thread's rip

#ifdef BOXEDWINE_GUEST_X64

#include <string.h>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>

// ---------------------------------------------------------------------------
// Process-shared file mappings (MAP_SHARED). A single host buffer per
// (file path, page-aligned file offset) is aliased by every process that maps
// that file page MAP_SHARED — so a write by one process (e.g. wineserver
// updating its shared object/sequence section) is seen by all others, exactly
// like a real shared mmap. Without this, the 64-bit file-mmap path handed each
// process a private copy and wine's server shared-memory section desynced,
// crashing wineserver with release_object refcount underflow. Mirrors the
// 32-bit KMemory::MappedFileCache (keyed by node->path). Pages live for the
// life of the process (server section is tiny + long-lived); we never evict.
// ---------------------------------------------------------------------------
namespace {
struct SharedFilePage {
    U8 data[K64_PAGE_SIZE];
};
std::mutex g_sharedFileMutex;
// key: path + "\0" + decimal page-aligned file offset -> one shared page buffer.
std::unordered_map<std::string, std::shared_ptr<SharedFilePage>> g_sharedFileRegistry;

std::shared_ptr<SharedFilePage> getSharedFilePage(const std::string& path, U64 offsetPage,
                                                  const U8* seed, U64 seedLen, bool& created) {
    std::string key = path;
    key.push_back('\0');
    key += std::to_string(offsetPage);
    std::lock_guard<std::mutex> lk(g_sharedFileMutex);
    auto it = g_sharedFileRegistry.find(key);
    if (it != g_sharedFileRegistry.end()) { created = false; return it->second; }
    auto page = std::make_shared<SharedFilePage>();
    ::memset(page->data, 0, K64_PAGE_SIZE);
    if (seed && seedLen) {
        U64 n = seedLen < K64_PAGE_SIZE ? seedLen : K64_PAGE_SIZE;
        ::memcpy(page->data, seed, (size_t)n);
    }
    g_sharedFileRegistry[key] = page;
    created = true;
    return page;
}
} // namespace

// K_EINVAL / K_ENOMEM live in the existing kernel headers. We return
// (U64)-errno from mmap-style calls following the 32-bit convention.
#ifndef K_EINVAL
#define K_EINVAL 22
#endif
#ifndef K_ENOMEM
#define K_ENOMEM 12
#endif

// ---------------------------------------------------------------------------
// Per-thread data-page TLB. Every guest DATA access (operand reads/writes,
// push/pop) used to take pagesMutex + an unordered_map lookup via getPage /
// commitPageLocked — a native profile of a CPU-bound guest loop showed ~21%
// of interpreter time in pthread mutex slow paths from exactly this. The
// instruction-FETCH side already has its per-CPU page cache (CPU64::fetchByte);
// this is the data-side equivalent: a small direct-mapped, thread-local
// page -> host-buffer cache consulted before the locked slow path.
//
// Why so little invalidation is needed (all verified against this file):
//   - munmap frees ADDRESS SPACE only; page buffers deliberately survive.
//   - mprotect only rewrites flags; reads ignore flags entirely and writes
//     commit-on-write regardless, so cached data* stays behaviorally exact.
//   - mmap over an existing committed page zeroes IN PLACE (buffer stable).
//   - K64Page::commit() never reallocates an existing buffer.
// The only events that free/replace a committed buffer are the KMemory64
// destructor (execve reset, process exit — also covers heap-address reuse of
// the KMemory64* tag) and K64Page::adoptShared (the shared-file mmap path).
// Both bump the global generation; a TLB hit requires a generation match.
// Entries are tagged with the owning KMemory64* because kernel code on one
// thread can touch another process's memory (fork ctid writes, ptrace-style
// peeks), and one host thread serves exactly one running guest thread.
#ifdef BOXEDWINE_BLOCK_CACHE_INFRA
bool bw64PageHoldsBlocks(U64 pageNum); // defined below, with the block tables
#endif

namespace {
struct K64DTlbEntry { const void* mem; U64 page; U8* data; U32 gen; };
constexpr U32 K64_DTLB_SIZE = 64; // direct-mapped, per-thread (~2KB)
thread_local K64DTlbEntry g_k64DTlb[K64_DTLB_SIZE];
std::atomic<U32> g_k64DTlbGen{1};

inline U8* k64DTlbLookup(const void* mem, U64 pageNum) {
    const K64DTlbEntry& e = g_k64DTlb[pageNum & (K64_DTLB_SIZE - 1)];
    if (e.mem == mem && e.page == pageNum &&
        e.gen == g_k64DTlbGen.load(std::memory_order_acquire)) {
        return e.data;
    }
    return nullptr;
}
inline void k64DTlbInsert(const void* mem, U64 pageNum, U8* data) {
#ifdef BOXEDWINE_BLOCK_CACHE_INFRA
    // Block-cache invalidation contract: a page registered as holding decoded
    // blocks is barred from the data TLB. All its accesses then take the slow
    // (locked) paths, and only the WRITE slow paths bump its generation — so
    // the scorching read/write fast paths carry zero extra cost.
    if (bw64PageHoldsBlocks(pageNum)) {
        return;
    }
#endif
    K64DTlbEntry& e = g_k64DTlb[pageNum & (K64_DTLB_SIZE - 1)];
    e.mem = mem;
    e.page = pageNum;
    e.data = data;
    e.gen = g_k64DTlbGen.load(std::memory_order_acquire);
}
inline void k64DTlbInvalidateAll() {
    g_k64DTlbGen.fetch_add(1, std::memory_order_release);
}
} // namespace

#ifdef BOXEDWINE_BLOCK_CACHE_INFRA
// Block-cache invalidation tables (see kmemory64.h). File-scope globals:
// zero impact on KMemory64's object layout, shared across address spaces.
static std::atomic<U32> g_blockPagesRegistered{0};
static std::atomic<U32> g_blockPageGen[4096];
static std::atomic<U32> g_blockPageMark[128];

bool bw64PageHoldsBlocks(U64 pageNum) {
    if (g_blockPagesRegistered.load(std::memory_order_relaxed) == 0) {
        return false;
    }
    U32 slot = KMemory64::blockSlot(pageNum);
    return (g_blockPageMark[slot >> 5].load(std::memory_order_relaxed) & (1u << (slot & 31))) != 0;
}

void KMemory64::blockPageRegister(U64 pageNum) {
    U32 slot = blockSlot(pageNum);
    U32 word = slot >> 5, bit = 1u << (slot & 31);
    if (!(g_blockPageMark[word].load(std::memory_order_relaxed) & bit)) {
        g_blockPageMark[word].fetch_or(bit, std::memory_order_release);
        g_blockPagesRegistered.fetch_add(1, std::memory_order_release);
        // Evict every thread's cached entry for (any page aliasing) this slot,
        // so writes that were fast-pathing through the TLB start missing.
        k64DTlbInvalidateAll();
    }
}

U32 KMemory64::blockPageGenOf(U64 pageNum) {
    return g_blockPageGen[blockSlot(pageNum)].load(std::memory_order_acquire);
}

static inline void bw64NoteWriteSlow(U64 pageNum) {
    U32 slot = KMemory64::blockSlot(pageNum);
    if (g_blockPageMark[slot >> 5].load(std::memory_order_relaxed) & (1u << (slot & 31))) {
        g_blockPageGen[slot].fetch_add(1, std::memory_order_release);
    }
}

void KMemory64::noteGuestWrite(U64 pageNum) {
    if (g_blockPagesRegistered.load(std::memory_order_relaxed) == 0) return;
    bw64NoteWriteSlow(pageNum);
}

void KMemory64::noteGuestWriteRange(U64 addr, U64 len) {
    if (g_blockPagesRegistered.load(std::memory_order_relaxed) == 0 || len == 0) return;
    for (U64 pn = addr >> K64_PAGE_SHIFT, last = (addr + len - 1) >> K64_PAGE_SHIFT; pn <= last; pn++) {
        bw64NoteWriteSlow(pn);
    }
}
#endif // BOXEDWINE_BLOCK_CACHE_INFRA

KMemory64::KMemory64(KProcess* process) : process(process) {}
KMemory64::~KMemory64() {
    // Page buffers die with the object — invalidate every thread's data TLB
    // (execve gives the process a fresh KMemory64; a later allocation could
    // also reuse this heap address as a tag).
    k64DTlbInvalidateAll();
}

// BW64_STRAYWRITE tripwire: ASan cannot see writes that land inside the
// emulated guest address space (one big host allocation), so a syscall handler
// that computes a wrong guest destination and scribbles into e.g. wineserver's
// malloc arena is invisible to ASan yet shows up later as glibc "unaligned
// tcache"/"corrupted double-linked list". This logs a guest WRITE whose target
// page had no prior map entry at all — i.e. the process never reserved/mmap'd
// it, so writing there is a stray write (legit lazy-commit always targets a
// page that mmapAnonymousFixed already entered with K64_PAGE_MAPPED). Cheap:
// one map lookup, only when the env var is set.
static bool g_strayInit = false, g_strayOn = false;
void KMemory64::strayWriteCheck(U64 dstGuest, U64 len) {
    if (!g_strayInit) { g_strayOn = std::getenv("BW64_STRAYWRITE") != nullptr; g_strayInit = true; }
    if (!g_strayOn) return;
    U64 firstPage = dstGuest >> K64_PAGE_SHIFT;
    U64 lastPage  = (dstGuest + (len ? len - 1 : 0)) >> K64_PAGE_SHIFT;
    for (U64 pn = firstPage; pn <= lastPage; pn++) {
        K64Page* p = getPage(pn);
        if (!p || !(p->flags & K64_PAGE_MAPPED)) {
            klog_fmt("STRAYWRITE: pid=%u write to UNMAPPED guest page 0x%llx (addr=0x%llx len=%llu) — corruption candidate",
                     (unsigned)(process ? process->id : 0),
                     (unsigned long long)(pn << K64_PAGE_SHIFT),
                     (unsigned long long)dstGuest, (unsigned long long)len);
            return; // one report per write is enough
        }
    }
}

void KMemory64::cloneFrom(const KMemory64* from) {
    // Lock both sides: `from` may be a live address space (the forking parent),
    // and we're populating `this` (the fresh child). The parent could fault new
    // pages concurrently in MT mode; take its lock for a consistent snapshot.
    // recursive_mutex is fine even if from==this (it never is for fork). Use
    // explicit guards (the CRITICAL_SECTION macro hard-codes the name `lock`, so
    // it can't be used twice in one scope).
#ifdef BOXEDWINE_MULTI_THREADED
    std::lock_guard<std::recursive_mutex> lockFrom(from->pagesMutex);
    std::lock_guard<std::recursive_mutex> lockThis(pagesMutex);
#endif
    pages.clear();
    pages.reserve(from->pages.size());
    for (const auto& kv : from->pages) {
        auto copy = std::make_unique<K64Page>();
        copy->flags = kv.second->flags;
        // Only copy backing store for committed pages; an uncommitted page in
        // the parent (reserved but never touched) stays uncommitted in the child.
        if (kv.second->committed()) {
            ::memcpy(copy->commit(), kv.second->data, K64_PAGE_SIZE);
        }
        pages.emplace(kv.first, std::move(copy));
    }
}

K64Page* KMemory64::getPage(U64 pageNum) const {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
    auto it = pages.find(pageNum);
    return it == pages.end() ? nullptr : it->second.get();
}

K64Page* KMemory64::getOrAllocPage(U64 pageNum, U32 flagsIfNew) {
    // Lock spans find+emplace so a concurrent allocator can't rehash the map
    // under our iterator. The returned raw K64Page* stays valid after we drop
    // the lock because the unique_ptr payload never moves (see header note).
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
    auto it = pages.find(pageNum);
    if (it != pages.end()) {
        return it->second.get();
    }
    auto page = std::make_unique<K64Page>();
    page->flags = flagsIfNew;
    K64Page* raw = page.get();
    pages.emplace(pageNum, std::move(page));
    return raw;
}

// Allocate (if needed) AND commit a page's backing buffer atomically under
// pagesMutex, returning the committed buffer. MT-CRITICAL: wine processes are
// multi-threaded (each guest process runs every thread on its own host thread,
// all sharing this KMemory64). The old write path did getOrAllocPage() under the
// lock, then called page->commit() LOCK-FREE. Two host threads writing the same
// not-yet-committed page would both see data==nullptr and both `new` a buffer:
// one alloc wins the `data=` store, the other thread then memcpy's into an
// ORPHANED buffer — a silently LOST WRITE. During the boot storm that lost write
// landed in a freshly-loaded image (a relocated IAT slot / function pointer), so
// the page later read back stale/zero -> a wild indirect call into garbage
// (RIP=0x10270 / data executed as code) -> "could not load kernel32.dll" ->
// cascading wineserver heap corruption. Folding commit() into the locked region
// makes first-touch commit atomic so no write is lost. (Once committed, `data`
// is stable — munmap/PROT_NONE only decommit unpinned pages wine isn't actively
// writing — so the subsequent lock-free memcpy is safe.)
U8* KMemory64::commitPageLocked(U64 pageNum, U32 flagsIfNew) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
    K64Page* page = getOrAllocPage(pageNum, flagsIfNew); // recursive mutex: re-enter OK
    return page->commit();
}

U64 KMemory64::committedPageCount() const {
    // Pages that actually hold a backing buffer (touched), as opposed to merely
    // reserved address space (data==nullptr). The gap between this and
    // mappedPageCount() is the memory lazy commit saves on wine's huge
    // PROT_NONE reservations.
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
    U64 n = 0;
    for (const auto& kv : pages) {
        if (kv.second->committed()) n++;
    }
    return n;
}

bool KMemory64::isPageMapped(U64 pageNum) const {
    K64Page* p = getPage(pageNum);
    return p && (p->flags & K64_PAGE_MAPPED);
}

U8* KMemory64::getCommittedPagePtr(U64 pageNum) {
    K64Page* p = getPage(pageNum);
    return (p && p->committed()) ? p->data : nullptr;
}

U32 KMemory64::getPageFlags(U64 pageNum) const {
    K64Page* p = getPage(pageNum);
    return p ? p->flags : 0;
}

// Process-wide mmap base — must match MMAP64_BASE in syscall64.cpp. Anonymous
// and file-backed mmap(NULL,...) both draw from here so they can't collide.
#define K64_MMAP_BASE 0x700000000ULL
#define K64_MMAP_BASE_PAGE (K64_MMAP_BASE >> K64_PAGE_SHIFT)

U64 KMemory64::mmapAnonymousFixed(U64 addr, U64 len, U32 prot) {
    if (len == 0) return (U64)-K_EINVAL;
    if (addr & K64_PAGE_MASK) return (U64)-K_EINVAL;

    U64 pageStart = addr >> K64_PAGE_SHIFT;
    U64 pageCount = (len + K64_PAGE_SIZE - 1) >> K64_PAGE_SHIFT;

    U32 flags = K64_PAGE_MAPPED;
    if (prot & 0x1) flags |= K64_PAGE_READ;
    if (prot & 0x2) flags |= K64_PAGE_WRITE | K64_PAGE_READ; // x86: write implies read
    if (prot & 0x4) flags |= K64_PAGE_EXEC;

    for (U64 i = 0; i < pageCount; i++) {
        K64Page* page = getOrAllocPage(pageStart + i, flags);
        page->flags = flags;
        // Lazy commit: a FRESH reservation gets NO backing buffer — it reads as
        // zero (memcpyFromGuest zero-fills an uncommitted page) and only commits
        // on first write. This is the leak fix: wine's huge PROT_NONE spans no
        // longer cost a 4 KB buffer per page. An ALREADY-COMMITTED page being
        // re-mapped (overlap) is zeroed in place to preserve MAP_ANONYMOUS
        // semantics and keep the file-mmap head/tail preservation logic valid.
        if (page->committed()) {
            noteGuestWrite(pageStart + i); // zero-in-place = content change
            ::memset(page->data, 0, K64_PAGE_SIZE);
        }
    }
    // Keep the reservation map authoritative for the mmap region. A MAP_FIXED
    // landing here (wine remapping inside an earlier reservation, or the
    // allocator's own commit) updates `ranges` so later gap searches see the
    // real occupancy. rangeInsertLocked ignores addresses below the base and
    // splits any range this one overwrites. mmapMutex is recursive, so this is
    // safe whether or not the caller (mmapReserveAndMap) already holds it.
    if (pageStart >= K64_MMAP_BASE_PAGE) {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
        rangeInsertLocked(pageStart, pageCount, prot,
                          prot ? (U8)MMAP_ANON : (U8)MMAP_RESERVED);
    }
    return addr;
}

U64 KMemory64::mmapSharedFile(U64 addr, U64 len, U32 prot, const char* path,
                              U64 fileOffset, const U8* fileBytes, U64 fileBytesLen) {
    if (len == 0) return (U64)-K_EINVAL;
    if (addr & K64_PAGE_MASK) return (U64)-K_EINVAL;
    if (!path) return (U64)-K_EINVAL;

    U64 pageStart = addr >> K64_PAGE_SHIFT;
    U64 pageCount = (len + K64_PAGE_SIZE - 1) >> K64_PAGE_SHIFT;

    U32 flags = K64_PAGE_MAPPED | K64_PAGE_SHARED;
    if (prot & 0x1) flags |= K64_PAGE_READ;
    if (prot & 0x2) flags |= K64_PAGE_WRITE | K64_PAGE_READ;
    if (prot & 0x4) flags |= K64_PAGE_EXEC;

    std::string p(path);
    for (U64 i = 0; i < pageCount; i++) {
        K64Page* page = getOrAllocPage(pageStart + i, flags);
        page->flags = flags;
        // Each guest page adopts the one shared buffer for this file page. The
        // FIRST process to map a given file page seeds it from the file bytes we
        // were handed; later processes (and later maps) alias the same buffer and
        // must NOT re-seed (that would clobber writes already made through it,
        // e.g. wineserver's live server section). So only the registry-created
        // buffer takes the seed.
        U64 fpage = (fileOffset >> K64_PAGE_SHIFT) + i;
        const U8* seed = nullptr; U64 seedLen = 0;
        U64 thisPageFileStart = (U64)i * K64_PAGE_SIZE;
        if (fileBytes && fileBytesLen > thisPageFileStart) {
            seed = fileBytes + thisPageFileStart;
            seedLen = fileBytesLen - thisPageFileStart;
            if (seedLen > K64_PAGE_SIZE) seedLen = K64_PAGE_SIZE;
        }
        bool created = false;
        std::shared_ptr<SharedFilePage> shared = getSharedFilePage(p, fpage, seed, seedLen, created);
        noteGuestWrite(pageStart + i); // buffer replaced = cached blocks stale
        // adoptShared REPLACES (and may free) the page's buffer — stale data
        // TLB entries for this page must not survive.
        page->adoptShared(shared->data);
        k64DTlbInvalidateAll();
        if (std::getenv("BW64_SHAREMAP")) {
            klog_fmt("SHAREMAP: pid=%u %s fpage=%llu guest=0x%llx path='%s'",
                     (unsigned)(process ? process->id : 0), created ? "CREATE" : "ALIAS ",
                     (unsigned long long)fpage,
                     (unsigned long long)((pageStart + i) << K64_PAGE_SHIFT), p.c_str());
        }
    }
    if (pageStart >= K64_MMAP_BASE_PAGE) {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
        rangeInsertLocked(pageStart, pageCount, prot, (U8)MMAP_ANON);
    }
    return addr;
}

// Record a reservation in the ordered `ranges` map. Caller holds mmapMutex.
// Splits/clamps any pre-existing ranges that the new one overwrites so the map
// stays a non-overlapping cover of the live mmap-region reservations (wine
// MAP_FIXED-remaps inside an earlier reservation constantly). Only the mmap
// region (>= K64_MMAP_BASE) is tracked; the loader's fixed low maps don't need
// allocation bookkeeping because the gap search never looks below the base.
void KMemory64::rangeInsertLocked(U64 startPage, U64 pageCount, U32 prot, U8 kind) {
    if (pageCount == 0 || startPage < K64_MMAP_BASE_PAGE) return;
    rangeRemoveLocked(startPage, pageCount); // clear any overlap first
    ranges[startPage] = MMapRange{ startPage, pageCount, prot, kind };
}

// Remove/trim ranges overlapping [startPage, startPage+pageCount). Caller holds
// mmapMutex. A range straddling either edge is split into the surviving
// non-overlapping remnant(s). This is the address-space free that makes the
// region reusable; it does NOT touch page backing store (that is Phase 2).
void KMemory64::rangeRemoveLocked(U64 startPage, U64 pageCount) {
    if (pageCount == 0) return;
    U64 hole0 = startPage;
    U64 hole1 = startPage + pageCount; // exclusive
    // Start from the last range that could overlap: the one beginning at or
    // before hole0. std::map is ordered so we can walk forward from there.
    auto it = ranges.upper_bound(hole0);
    if (it != ranges.begin()) --it;
    while (it != ranges.end() && it->second.startPage < hole1) {
        U64 r0 = it->second.startPage;
        U64 r1 = r0 + it->second.pageCount; // exclusive
        if (r1 <= hole0) { ++it; continue; } // entirely before the hole
        // Overlaps. Drop it, then re-insert any surviving head/tail remnants.
        U32 prot = it->second.prot; U8 kind = it->second.kind;
        it = ranges.erase(it);
        if (r0 < hole0) ranges[r0] = MMapRange{ r0, hole0 - r0, prot, kind };
        if (r1 > hole1) ranges[hole1] = MMapRange{ hole1, r1 - hole1, prot, kind };
    }
}

U64 KMemory64::mmapReserveAndMap(U64 length, U32 prot) {
    U64 pageCount = (length + K64_PAGE_MASK) >> K64_PAGE_SHIFT;
    if (pageCount == 0) pageCount = 1;

    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
    if (mmapNext == 0) mmapNext = K64_MMAP_BASE;
    U64 cursor = mmapNext >> K64_PAGE_SHIFT;
    if (cursor < K64_MMAP_BASE_PAGE) cursor = K64_MMAP_BASE_PAGE;

    // Gap search over the ordered reservation map: O(log n + ranges scanned),
    // not O(pages). Walk ranges from `cursor`; the first hole of `pageCount`
    // pages between consecutive reservations (or after the last one) wins.
    U64 candidate = cursor;
    auto it = ranges.lower_bound(candidate);
    // A range starting before `candidate` may still cover it — back up one and
    // push `candidate` past its end if so.
    if (it != ranges.begin()) {
        auto prev = std::prev(it);
        U64 prevEnd = prev->second.startPage + prev->second.pageCount;
        if (prevEnd > candidate) candidate = prevEnd;
    }
    for (; it != ranges.end(); ++it) {
        U64 gapEnd = it->second.startPage; // exclusive
        if (gapEnd >= candidate + pageCount) break; // fits before this range
        U64 rangeEnd = it->second.startPage + it->second.pageCount;
        if (rangeEnd > candidate) candidate = rangeEnd; // jump past it
    }
    // `candidate` now points at a hole large enough (either before `it` or past
    // the last range — address space above is effectively unbounded for us).
    U64 addr = candidate << K64_PAGE_SHIFT;
    // Map the pages NOW, under mmapMutex (recursive), so a concurrent sibling's
    // allocation sees them taken. mmapAnonymousFixed registers the reservation
    // in `ranges` itself, so the gap is claimed before we drop the lock.
    mmapAnonymousFixed(addr, pageCount << K64_PAGE_SHIFT, prot);
    mmapNext = (candidate + pageCount) << K64_PAGE_SHIFT;
    return addr;
}

U64 KMemory64::mprotect(U64 addr, U64 len, U32 prot) {
    if (addr & K64_PAGE_MASK) return (U64)-K_EINVAL;
    if (len == 0) return addr;

    U32 newFlags = K64_PAGE_MAPPED;
    if (prot & 0x1) newFlags |= K64_PAGE_READ;
    if (prot & 0x2) newFlags |= K64_PAGE_WRITE | K64_PAGE_READ; // write implies read
    if (prot & 0x4) newFlags |= K64_PAGE_EXEC;

    U64 pageStart = addr >> K64_PAGE_SHIFT;
    U64 pageCount = (len + K64_PAGE_SIZE - 1) >> K64_PAGE_SHIFT;
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
    for (U64 i = 0; i < pageCount; i++) {
        // Preserve SHARED bit if it was set; everything else is replaced
        // wholesale. Holes (no page) are skipped — see header comment.
        auto it = pages.find(pageStart + i);
        if (it == pages.end()) continue;
        U32 preserved = it->second->flags & (K64_PAGE_SHARED | K64_PAGE_PINNED);
        it->second->flags = newFlags | preserved;
    }
    return addr;
}

U64 KMemory64::munmap(U64 addr, U64 len) {
    if (addr & K64_PAGE_MASK) return (U64)-K_EINVAL;
    if (len == 0) return 0;
    U64 pageStart = addr >> K64_PAGE_SHIFT;
    U64 pageCount = (len + K64_PAGE_SIZE - 1) >> K64_PAGE_SHIFT;

    // Free the ADDRESS SPACE only — drop/trim the reservation record so the
    // range is reusable and the gap search stays fast.
    //
    // We deliberately do NOT decommit (free) the page buffers here, even though
    // that would reclaim more memory. Decommitting was measured to REGRESS boot:
    // wine/wineserver munmap a region and then lazily re-read it; with the
    // buffer freed those reads return ZERO instead of the stale-but-present
    // bytes, which tripped `wineserver: release_object: Assertion obj->refcount`
    // deterministically (refcount read back as 0). This is the same lazy-touch
    // hazard that sank the earlier pages.erase() real-munmap (commit 3ae9ca3a,
    // reverted). The big leak win comes from LAZY COMMIT on the mmap side (fresh
    // reservations carry no buffer at all — wine's 1.9 GB PROT_NONE spans now
    // cost ~60 MB), not from freeing on munmap, so address-space-only munmap
    // keeps essentially all the benefit with none of the breakage.
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
    rangeRemoveLocked(pageStart, pageCount);
    return 0;
}

// BW64_WATCH=0xADDR[,len] — log any guest write that overlaps [ADDR, ADDR+len).
// Used to find who fills (or fails to fill) a runtime callback slot. The host
// callstack isn't captured, but the value + the fact that a write happened at
// all distinguishes "never written" from "written with garbage". Parsed once.
static U64 g_watchAddr = 0, g_watchLen = 0;
static bool g_watchInit = false;
static void initWatch() {
    g_watchInit = true;
    const char* e = std::getenv("BW64_WATCH");
    if (!e) return;
    g_watchAddr = std::strtoull(e, nullptr, 0);
    const char* comma = std::strchr(e, ',');
    g_watchLen = comma ? std::strtoull(comma + 1, nullptr, 0) : 8;
    if (g_watchLen == 0) g_watchLen = 8;
}

// BW64_MEMRING: a ring of the most recent guest writes made BY THE WINESERVER
// process, each tagged with the guest RIP that issued it (recovered from the
// running thread's CPU64). Bug #2's dominant faces are glibc malloc-metadata
// corruption ("unaligned tcache chunk" / "corrupted ... double linked list") —
// a stray/overshooting write landed in a heap chunk's metadata word. At the
// abort, BW64_MALLOCDUMP prints candidate corrupted-chunk addresses; dumping
// this ring lets us find the WRITE (and its RIP) that hit that address. Gated;
// when off, recordMemWrite is a couple of cheap checks. Only wineserver writes
// are recorded to keep the ring signal-dense (the bug is in its own heap).
struct MemRingRec { U64 rip; U64 addr; U32 len; U64 value; };
static MemRingRec g_memRing[256];
static U32        g_memRingNext = 0;
static std::mutex g_memRingMutex;
static bool       g_memRingInit = false, g_memRingOn = false;

void KMemory64::recordMemWrite(U64 addr, U64 len, U64 value) {
    if (!g_memRingInit) { g_memRingOn = std::getenv("BW64_MEMRING") != nullptr; g_memRingInit = true; }
    if (!g_memRingOn) return;
    if (!process || !process->exe.contains("wineserver")) return;
    U64 rip = 0;
    KThread* t = KThread::currentThread();
    if (t && t->cpu64) rip = t->cpu64->rip;
    std::lock_guard<std::mutex> lk(g_memRingMutex);
    g_memRing[g_memRingNext % 256] = { rip, addr, (U32)len, value };
    g_memRingNext++;
}

// Dump the ring newest-first, flagging any entry whose target is at/near `near`
// (0 = no correlation filter). Called from the abort path in syscall64.cpp.
void kmemory64DumpMemRing(U64 nearAddr) {
    if (!g_memRingOn) return;
    std::lock_guard<std::mutex> lk(g_memRingMutex);
    klog_fmt("MEMRING: last %u wineserver guest writes (newest first)%s:",
             (g_memRingNext < 256 ? g_memRingNext : 256),
             nearAddr ? " [* = within 64B of a malloc-dump candidate]" : "");
    U32 n = (g_memRingNext < 256) ? g_memRingNext : 256;
    for (U32 k = 0; k < n; k++) {
        U32 idx = (g_memRingNext + 256 - 1 - k) % 256;
        const MemRingRec& r = g_memRing[idx];
        bool hot = nearAddr && (r.addr <= nearAddr + 64 && r.addr + r.len + 64 > nearAddr);
        klog_fmt("MEMRING:  %s rip=0x%llx -> [0x%llx] len=%u val=0x%llx",
                 hot ? "*" : " ", (unsigned long long)r.rip,
                 (unsigned long long)r.addr, r.len, (unsigned long long)r.value);
    }
}

void KMemory64::memcpyToGuest(U64 dstGuest, const void* src, U64 len) {
    // Data-TLB fast path. Only when every gated write diagnostic (straywrite /
    // memring / watch) is confirmed OFF — the slow path is what initializes
    // those gates, so the first few writes always go the long way. Writes
    // ignore page flags (commit-on-write), so a cached committed buffer is
    // behaviorally identical to the locked path.
    U64 offsetInPage = dstGuest & K64_PAGE_MASK;
    if (len && offsetInPage + len <= K64_PAGE_SIZE &&
        g_strayInit && !g_strayOn && g_memRingInit && !g_memRingOn &&
        g_watchInit && !g_watchAddr) {
        if (U8* hit = k64DTlbLookup(this, dstGuest >> K64_PAGE_SHIFT)) {
            ::memcpy(hit + offsetInPage, src, (size_t)len);
            return;
        }
    }
    const U8* s = (const U8*)src;
    noteGuestWriteRange(dstGuest, len); // slow path only
    strayWriteCheck(dstGuest, len);
    {
        U64 v = 0; if (len >= 8) std::memcpy(&v, src, 8); else std::memcpy(&v, src, (size_t)len);
        recordMemWrite(dstGuest, len, v);
    }
    if (!g_watchInit) initWatch();
    if (g_watchAddr && dstGuest < g_watchAddr + g_watchLen && dstGuest + len > g_watchAddr) {
        U64 v = 0;
        U64 within = (g_watchAddr >= dstGuest) ? (g_watchAddr - dstGuest) : 0;
        if (within + 8 <= len) std::memcpy(&v, (const U8*)src + within, 8);
        klog_fmt("BW64_WATCH: pid=%d write to 0x%llx (watch 0x%llx) len=%llu first8=0x%llx",
                 (int)(process ? process->id : -1),
                 (unsigned long long)dstGuest, (unsigned long long)g_watchAddr,
                 (unsigned long long)len, (unsigned long long)v);
    }
    while (len) {
        U64 pageNum = dstGuest >> K64_PAGE_SHIFT;
        offsetInPage = dstGuest & K64_PAGE_MASK;
        U64 chunk = K64_PAGE_SIZE - offsetInPage;
        if (chunk > len) chunk = len;
        U8* data = commitPageLocked(pageNum, K64_PAGE_MAPPED | K64_PAGE_READ | K64_PAGE_WRITE);
        k64DTlbInsert(this, pageNum, data);
        ::memcpy(data + offsetInPage, s, (size_t)chunk); // commit-on-write (MT-safe)
        dstGuest += chunk;
        s += chunk;
        len -= chunk;
    }
}

void KMemory64::memcpyFromGuest(void* dst, U64 srcGuest, U64 len) {
    // Data-TLB fast path: the whole span inside one already-seen page (the
    // overwhelmingly common case — every operand read lands here) skips the
    // pagesMutex + map lookup entirely.
    U64 offsetInPage = srcGuest & K64_PAGE_MASK;
    if (len && offsetInPage + len <= K64_PAGE_SIZE) {
        if (U8* hit = k64DTlbLookup(this, srcGuest >> K64_PAGE_SHIFT)) {
            ::memcpy(dst, hit + offsetInPage, (size_t)len);
            return;
        }
    }
    U8* d = (U8*)dst;
    while (len) {
        U64 pageNum = srcGuest >> K64_PAGE_SHIFT;
        offsetInPage = srcGuest & K64_PAGE_MASK;
        U64 chunk = K64_PAGE_SIZE - offsetInPage;
        if (chunk > len) chunk = len;
        K64Page* page = getPage(pageNum);
        if (page && page->committed()) {
            k64DTlbInsert(this, pageNum, page->data);
            ::memcpy(d, page->data + offsetInPage, (size_t)chunk);
        } else {
            // Absent slot OR reserved-but-uncommitted page → reads as zero.
            // (Not cached: a later write commits a buffer the TLB must see.)
            ::memset(d, 0, (size_t)chunk);
        }
        srcGuest += chunk;
        d += chunk;
        len -= chunk;
    }
}

void KMemory64::memsetGuest(U64 dstGuest, U8 value, U64 len) {
    noteGuestWriteRange(dstGuest, len);
    strayWriteCheck(dstGuest, len);
    while (len) {
        U64 pageNum = dstGuest >> K64_PAGE_SHIFT;
        U64 offsetInPage = dstGuest & K64_PAGE_MASK;
        U64 chunk = K64_PAGE_SIZE - offsetInPage;
        if (chunk > len) chunk = len;
        U8* data = commitPageLocked(pageNum, K64_PAGE_MAPPED | K64_PAGE_READ | K64_PAGE_WRITE);
        ::memset(data + offsetInPage, value, (size_t)chunk); // commit-on-write (MT-safe)
        dstGuest += chunk;
        len -= chunk;
    }
}

U8 KMemory64::readb(U64 addr) {
    U64 pageNum = addr >> K64_PAGE_SHIFT;
    if (U8* hit = k64DTlbLookup(this, pageNum)) return hit[addr & K64_PAGE_MASK];
    K64Page* p = getPage(pageNum);
    if (p && p->committed()) {
        k64DTlbInsert(this, pageNum, p->data);
        return p->data[addr & K64_PAGE_MASK];
    }
    return 0;
}

U16 KMemory64::readw(U64 addr) {
    U16 v; memcpyFromGuest(&v, addr, 2); return v;
}

U32 KMemory64::readd(U64 addr) {
    U32 v; memcpyFromGuest(&v, addr, 4); return v;
}

U64 KMemory64::readq(U64 addr) {
    U64 v; memcpyFromGuest(&v, addr, 8); return v;
}

void KMemory64::writeb(U64 addr, U8 value) {
    U64 pageNum = addr >> K64_PAGE_SHIFT;
    if (g_strayInit && !g_strayOn && g_memRingInit && !g_memRingOn) {
        if (U8* hit = k64DTlbLookup(this, pageNum)) {
            hit[addr & K64_PAGE_MASK] = value;
            return;
        }
    }
    strayWriteCheck(addr, 1);
    recordMemWrite(addr, 1, value);
    noteGuestWrite(pageNum); // slow path only — fast path is barred for block pages
    U8* data = commitPageLocked(pageNum, K64_PAGE_MAPPED | K64_PAGE_READ | K64_PAGE_WRITE);
    k64DTlbInsert(this, pageNum, data);
    data[addr & K64_PAGE_MASK] = value; // commit-on-write (MT-safe)
}

U8* KMemory64::getRamPtr(U64 addr, U32 len) {
    U64 offsetInPage = addr & K64_PAGE_MASK;
    if (offsetInPage + len > K64_PAGE_SIZE) {
        // Would span two pages — host pointer wouldn't be contiguous.
        return nullptr;
    }
    // Commit + PIN atomically under pagesMutex. Callers (the 64-bit futex table,
    // atomic RMW in common_lock) keep this raw host pointer across a blocking
    // wait. If a concurrent munmap decommitted the buffer, a re-commit would move
    // it and the futex wake would target a stale address. The pin flag tells
    // munmap/mprotect to free the address space but LEAVE this page's buffer in
    // place. Once allocated, the buffer is never reallocated, so the returned
    // pointer stays valid. The commit must be locked (see commitPageLocked) so a
    // sibling thread's first-touch of the same page can't orphan our buffer.
    // Conservative for the block cache: a raw host pointer can be written
    // through at any later time (futex words, LOCK RMW), so treat the handout
    // itself as a write to the page.
    noteGuestWrite(addr >> K64_PAGE_SHIFT);
    U8* data;
    {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
        K64Page* page = getOrAllocPage(addr >> K64_PAGE_SHIFT,
                                       K64_PAGE_MAPPED | K64_PAGE_READ | K64_PAGE_WRITE);
        page->flags |= K64_PAGE_PINNED;
        data = page->commit();
    }
    return data + offsetInPage;
}

void KMemory64::writew(U64 addr, U16 value) { memcpyToGuest(addr, &value, 2); }
void KMemory64::writed(U64 addr, U32 value) { memcpyToGuest(addr, &value, 4); }
void KMemory64::writeq(U64 addr, U64 value) { memcpyToGuest(addr, &value, 8); }

#endif // BOXEDWINE_GUEST_X64
