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

#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

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
    U8 localData[K64_PAGE_SIZE];
    std::shared_ptr<std::atomic<U8*>> data;
    SharedFilePage() : data(std::make_shared<std::atomic<U8*>>(localData)) {}
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
    ::memset(page->localData, 0, K64_PAGE_SIZE);
    if (seed && seedLen) {
        U64 n = seedLen < K64_PAGE_SIZE ? seedLen : K64_PAGE_SIZE;
        ::memcpy(page->localData, seed, (size_t)n);
    }
    g_sharedFileRegistry[key] = page;
    created = true;
    return page;
}

std::shared_ptr<SharedFilePage> findSharedFilePage(const std::string& path, U64 offsetPage) {
    std::string key = path;
    key.push_back('\0');
    key += std::to_string(offsetPage);
    std::lock_guard<std::mutex> lk(g_sharedFileMutex);
    auto it = g_sharedFileRegistry.find(key);
    return it == g_sharedFileRegistry.end() ? nullptr : it->second;
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
#ifndef K_EBUSY
#define K_EBUSY 16
#endif
#ifndef K_ENOSYS
#define K_ENOSYS 38
#endif

#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
namespace {
static U64 k64NativeHostPageSize() {
    static U64 size = []() -> U64 {
        long value = ::sysconf(_SC_PAGESIZE);
        return value > 0 ? (U64)value : 4096;
    }();
    return size;
}

static U64 k64NativeAlignDown(U64 value, U64 alignment) {
    return value & ~(alignment - 1);
}

static U64 k64NativeAlignUp(U64 value, U64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static int k64NativeProt(U32 prot) {
    int result = 0;
    if (prot & 0x1) result |= PROT_READ;
    if (prot & 0x2) result |= PROT_READ | PROT_WRITE;
    // Guest EXEC is metadata for FEX's decoder. The translated ARM64 lives
    // in the separate executable arena; never request host PROT_EXEC for a
    // guest page (iOS W^X/debug policy rejects or constrains that transition).
    return result;
}

static U32 k64GuestProtFromPageFlags(U32 flags) {
    U32 prot = 0;
    if (flags & K64_PAGE_READ) prot |= 0x1;
    if (flags & K64_PAGE_WRITE) prot |= 0x2;
    // K64_PAGE_EXEC intentionally remains guest metadata only.
    return prot;
}

// Reserve a previously unused host interval without overwriting an existing
// VM region. On Darwin vm_allocate(VM_FLAGS_FIXED) is the atomic ownership
// check; the following mmap replaces only that reservation. Linux uses
// MAP_FIXED_NOREPLACE when available. Older POSIX hosts have no non-clobbering
// primitive, so the native identity backend remains opt-in there and relies on
// the platform's preflight result.
static bool k64NativeReserveHostRange(U64 start, U64 length) {
    if (!length || start > UINT64_MAX - length) return false;
#if defined(__APPLE__)
    vm_address_t address = (vm_address_t)start;
    const kern_return_t result = vm_allocate(mach_task_self(), &address,
                                             (vm_size_t)length, VM_FLAGS_FIXED);
    if (result != KERN_SUCCESS || address != (vm_address_t)start) {
        if (result == KERN_SUCCESS) {
            vm_deallocate(mach_task_self(), address, (vm_size_t)length);
        }
        return false;
    }
    return true;
#elif defined(MAP_FIXED_NOREPLACE)
    void* mapped = ::mmap((void*)(uintptr_t)start, (size_t)length,
                          PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                          -1, 0);
    if (mapped == MAP_FAILED || (U64)(uintptr_t)mapped != start) {
        if (mapped != MAP_FAILED) ::munmap(mapped, (size_t)length);
        return false;
    }
    return true;
#else
    // This fallback is retained only for non-Darwin POSIX build hosts that do
    // not expose MAP_FIXED_NOREPLACE. It cannot close the preflight race, but
    // still rejects ranges already known to be occupied by the OS.
    return true;
#endif
}

static void k64NativeReleaseReservedHostRange(U64 start, U64 length) {
#if defined(__APPLE__)
    vm_deallocate(mach_task_self(), (vm_address_t)start, (vm_size_t)length);
#else
    (void)start;
    (void)length;
#endif
}

// Check whether the interval is free before asking the fixed reservation
// primitive for it. vm_region_64 returns the next region when the cursor is in
// a hole, so a region beginning before the requested end is sufficient proof
// that this interval cannot be claimed safely.
static bool k64NativeHostRangeFree(U64 start, U64 length) {
    if (!length || start > UINT64_MAX - length) return false;
#if defined(__APPLE__)
    const U64 end = start + length;
    vm_address_t cursor = (vm_address_t)start;
    while ((U64)cursor < end) {
        vm_size_t regionSize = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t infoCount = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t objectName = MACH_PORT_NULL;
        vm_address_t regionStart = cursor;
        const kern_return_t result = vm_region_64(
            mach_task_self(), &regionStart, &regionSize,
            VM_REGION_BASIC_INFO_64,
            reinterpret_cast<vm_region_info_t>(&info), &infoCount,
            &objectName);
        if (objectName != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), objectName);
        }
        if (result == KERN_INVALID_ADDRESS) return true;
        if (result != KERN_SUCCESS || regionSize == 0) return false;
        const U64 region = (U64)regionStart;
        if (region >= end) return true;
        // The query starts at cursor. Any returned region that begins at or
        // before cursor and extends into the interval is occupied. A region
        // beginning in the requested interval is occupied as well.
        return false;
    }
    return true;
#else
    (void)start;
    (void)length;
    return true;
#endif
}

// The canonical Windows shared-data VA is below iOS's mandatory __PAGEZERO.
// All emulated processes live in this host process, so one high anonymous
// mapping can be shared by every KMemory64 instance. The signal bridge rewrites
// a faulting ARM64 base register from 0x7ffe* into this alias and retries the
// exact instruction; guest-visible page metadata remains canonical.
struct K64KuserAlias {
    std::atomic<void*> host {nullptr};
    U32 refs = 0;
};
std::mutex g_k64KuserAliasMutex;
K64KuserAlias g_k64KuserAlias;

static bool k64IsKuserRange(U64 addr, U64 len) {
    return addr >= K64_KUSER_SHARED_BASE &&
           addr <= K64_KUSER_SHARED_BASE + K64_KUSER_SHARED_SIZE &&
           len <= K64_KUSER_SHARED_BASE + K64_KUSER_SHARED_SIZE - addr;
}

static U8* k64KuserAliasFor(U64 guestAddress) {
    if (guestAddress < K64_KUSER_SHARED_BASE ||
        guestAddress >= K64_KUSER_SHARED_BASE + K64_KUSER_SHARED_SIZE) {
        return nullptr;
    }
    // This helper is also called from the Darwin SA_SIGINFO path. The alias
    // lifetime is protected by each owning KMemory64 and the release path
    // publishes nullptr only after the last owner has stopped executing, so a
    // lock-free acquire is required here: taking a pthread mutex in a signal
    // handler can deadlock if the interrupted thread held it.
    void* host = g_k64KuserAlias.host.load(std::memory_order_acquire);
    if (!host) return nullptr;
    return static_cast<U8*>(host) +
           (guestAddress - K64_KUSER_SHARED_BASE);
}
}
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
// Why so little invalidation is needed in sparse mode (all verified against this file):
//   - sparse munmap frees ADDRESS SPACE only; page buffers deliberately survive.
//   - sparse mprotect only rewrites flags; native mode also updates host
//     protections, while guest metadata remains the 4 KiB source of truth.
//   - mmap over an existing committed page zeroes IN PLACE (buffer stable).
//   - K64Page::commit() never reallocates an existing buffer.
// The only events that free/replace a committed buffer are the KMemory64
// destructor (execve reset, process exit — also covers heap-address reuse of
// the KMemory64* tag), native logical unmap, and K64Page::adoptShared (the
// shared-file mmap path).
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

static void k64InvalidateProcessFetchCaches(KProcess* process) {
    if (!process) return;
    process->iterateThreads([](KThread* thread) {
        if (thread && thread->cpu64) {
            thread->cpu64->invalidateFetchCache();
        }
        return true;
    });
    // The initial thread normally aliases this object, but invalidate the
    // process-owned CPU explicitly as well so early-loader mappings are safe
    // before the thread has been bound to it.
    if (process->cpu64) {
        process->cpu64->invalidateFetchCache();
    }
}

KMemory64::KMemory64(KProcess* process, bool nativeIdentity) : process(process) {
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    this->nativeIdentity = nativeIdentity;
#else
    (void)nativeIdentity;
#endif
}
KMemory64::~KMemory64() {
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    nativeDemoteSharedPages();
    nativeUnmapAll();
#endif
    // Page buffers die with the object — invalidate every thread's data TLB
    // (execve gives the process a fresh KMemory64; a later allocation could
    // also reuse this heap address as a tag).
    k64DTlbInvalidateAll();
}

bool KMemory64::nativeIdentityMode() const {
    return nativeIdentity;
}

bool KMemory64::nativeGuestRangeAllowed(U64 addr, U64 len) const {
    if (!nativeIdentityMode()) return true;
    if (len == 0 || addr > UINT64_MAX - len) return false;
    const U64 end = addr + len;
    // KUSER_SHARED_DATA is deliberately backed by a high host alias because
    // iOS leaves the canonical low address below __PAGEZERO unavailable.
    if (addr >= K64_KUSER_SHARED_BASE &&
        end <= K64_KUSER_SHARED_BASE + K64_KUSER_SHARED_SIZE) {
        return true;
    }
    return addr >= K64_NATIVE_GUEST_WINDOW_START &&
           end <= K64_NATIVE_GUEST_WINDOW_END;
}

U64 KMemory64::nativeAliasForGuest(U64 guestAddress) const {
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    if (!nativeIdentityMode() || !nativeKuserAliasHeld) return 0;
    U8* alias = k64KuserAliasFor(guestAddress);
    return reinterpret_cast<U64>(alias);
#else
    (void)guestAddress;
    return 0;
#endif
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
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    // Two KMemory64 objects in this host process cannot own independent
    // snapshots at the same fixed guest addresses. Sharing the pointers would
    // violate fork isolation, while MAP_FIXED would clobber the parent.
    if (nativeIdentityMode()) {
        kpanic("KMemory64 native identity mode does not support cloneFrom");
        return;
    }
#endif
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
        if ((kv.second->flags & K64_PAGE_MAPPED) && kv.second->committed()) {
            // A native identity page must never be shared into the child:
            // its backing pointer is an address in the parent's fixed host
            // mapping, so aliasing it would make fork writes cross-process.
            // Plain sparse shared pages retain the registry alias.
            if ((kv.second->flags & K64_PAGE_SHARED) && kv.second->sharedData &&
                !kv.second->dataNative) {
                copy->adoptShared(kv.second->sharedData, false, kv.second->sharedCanonical);
                copy->flags &= ~K64_PAGE_PINNED;
            } else {
                copy->flags &= ~(K64_PAGE_SHARED | K64_PAGE_PINNED);
                ::memcpy(copy->commit(), kv.second->hostData(), K64_PAGE_SIZE);
            }
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
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    if (nativeIdentityMode() && (!page->hostData() || !page->dataNative)) {
        // A native address space has no lazy heap buffers. A missing mapping
        // must remain a host fault rather than becoming a non-identity buffer.
        kpanic("KMemory64 native identity write to an unmapped page");
        return nullptr;
    }
#endif
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
    return (p && p->committed()) ? p->hostData() : nullptr;
}

U32 KMemory64::getPageFlags(U64 pageNum) const {
    K64Page* p = getPage(pageNum);
    return p ? p->flags : 0;
}

// Process-wide mmap base. Sparse mode retains its historical address; native
// identity mode starts in the proven iOS host window. Anonymous and file-backed
// mmap(NULL,...) both draw from the same per-process cursor.
#define K64_SPARSE_MMAP_BASE 0x700000000ULL
#define K64_SPARSE_MMAP_BASE_PAGE (K64_SPARSE_MMAP_BASE >> K64_PAGE_SHIFT)

static U64 k64MmapBase(const KMemory64* memory) {
    return memory->nativeIdentityMode()
        ? K64_NATIVE_GUEST_MMAP_BASE
        : K64_SPARSE_MMAP_BASE;
}

static U64 k64MmapBasePage(const KMemory64* memory) {
    return k64MmapBase(memory) >> K64_PAGE_SHIFT;
}

#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
bool KMemory64::nativeRangeCovers(U64 start, U64 end) const {
    if (end <= start) return true;
    U64 cursor = start;
    auto it = nativeRanges.upper_bound(start);
    if (it != nativeRanges.begin()) --it;
    for (; it != nativeRanges.end() && cursor < end; ++it) {
        const NativeRange& range = it->second;
        const U64 rangeEnd = range.hostStart + range.hostLength;
        if (rangeEnd <= cursor) continue;
        if (range.hostStart > cursor) return false;
        cursor = rangeEnd > cursor ? rangeEnd : cursor;
    }
    return cursor >= end;
}

void KMemory64::nativeForgetRange(U64 start, U64 length) {
    if (!length) return;
    const U64 end = start + length;
    auto it = nativeRanges.upper_bound(start);
    if (it != nativeRanges.begin()) --it;
    while (it != nativeRanges.end() && it->second.hostStart < end) {
        const U64 rangeStart = it->second.hostStart;
        const U64 rangeEnd = rangeStart + it->second.hostLength;
        if (rangeEnd <= start) { ++it; continue; }
        const U32 prot = it->second.prot;
        it = nativeRanges.erase(it);
        if (rangeStart < start) {
            nativeRanges.emplace(rangeStart,
                                 NativeRange{rangeStart, start - rangeStart, prot});
        }
        if (rangeEnd > end) {
            nativeRanges.emplace(end, NativeRange{end, rangeEnd - end, prot});
            break;
        }
    }
}

bool KMemory64::nativeMapAnonymous(U64 addr, U64 len, U32 prot, bool& fresh) {
    if (!nativeGuestRangeAllowed(addr, len)) {
        klog_fmt("KMemory64: native host MAP_FIXED refused outside guest "
                 "window addr=0x%llx len=0x%llx",
                 (unsigned long long)addr, (unsigned long long)len);
        return false;
    }
    const U64 hostPage = k64NativeHostPageSize();
    if ((hostPage & (hostPage - 1)) != 0 || hostPage < K64_PAGE_SIZE) return false;
    if (addr > UINT64_MAX - len) return false;
    const U64 guestEnd = addr + len;
    if (guestEnd > UINT64_MAX - (hostPage - 1)) return false;
    const U64 hostStart = k64NativeAlignDown(addr, hostPage);
    const U64 hostEnd = k64NativeAlignUp(guestEnd, hostPage);
    const U64 hostLength = hostEnd - hostStart;

    if (nativeRangeCovers(hostStart, hostEnd)) {
        // MAP_FIXED|MAP_ANONYMOUS replaces the requested guest bytes with
        // zeroes, even when the requested protection is PROT_NONE.  Temporarily
        // make the enclosing host pages writable so this remains correct on a
        // 16 KiB host page when only one 4 KiB guest subpage is remapped.
        if (::mprotect((void*)(uintptr_t)hostStart, (size_t)hostLength,
                       PROT_READ | PROT_WRITE) != 0) {
            return false;
        }
        ::memset((void*)(uintptr_t)addr, 0, (size_t)len);
        // A partial 4 KiB remap shares this host page with neighboring guest
        // pages. Keep the host page readable/writable in that case; guest
        // permissions remain in the per-4 KiB flags and enforcing PROT_NONE
        // here would revoke access from the neighboring subpages.
        const bool partialHostPage = (hostStart != addr || hostEnd != guestEnd);
        const int finalProt = partialHostPage ? (PROT_READ | PROT_WRITE)
                                              : k64NativeProt(prot);
        if (::mprotect((void*)(uintptr_t)hostStart, (size_t)hostLength,
                       finalProt) != 0) {
            return false;
        }
        for (auto& entry : nativeRanges) {
            NativeRange& range = entry.second;
            if (range.hostStart < hostEnd && range.hostStart + range.hostLength > hostStart) {
                range.prot = prot;
            }
        }
        fresh = false;
        return true;
    }

    // Do not issue Darwin MAP_FIXED against an untracked address: it can
    // replace the executable arena, the app image, or another runtime region.
    // The Mach reservation is retained until mmap consumes it, closing the
    // preflight-to-map race for the first identity mapping in this process.
    bool reservedHostRange = false;
    if (!nativeRangeCovers(hostStart, hostEnd)) {
        // A partially tracked overlap cannot be safely replaced as one
        // MAP_FIXED operation: it might contain an untracked host region in
        // the gap. Native mappings are host-page aligned, so a partial overlap
        // is a bookkeeping error and is rejected rather than guessed around.
        for (const auto& entry : nativeRanges) {
            const U64 rangeStart = entry.second.hostStart;
            const U64 rangeEnd = rangeStart + entry.second.hostLength;
            if (rangeStart < hostEnd && rangeEnd > hostStart) {
                klog_fmt("KMemory64: refusing partially tracked native map "
                         "[0x%llx,0x%llx)", (unsigned long long)hostStart,
                         (unsigned long long)hostEnd);
                return false;
            }
        }
        if (!k64NativeHostRangeFree(hostStart, hostLength)) {
            klog_fmt("KMemory64: native host range is occupied, refusing map "
                     "[0x%llx,0x%llx)", (unsigned long long)hostStart,
                     (unsigned long long)hostEnd);
            return false;
        }
        if (!k64NativeReserveHostRange(hostStart, hostLength)) {
            klog_fmt("KMemory64: native host reservation failed for "
                     "[0x%llx,0x%llx)", (unsigned long long)hostStart,
                     (unsigned long long)hostEnd);
            return false;
        }
        reservedHostRange = true;
    }

    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_BOXEDWINE;
    void* mapped = ::mmap((void*)(uintptr_t)hostStart, (size_t)hostLength,
                          PROT_READ | PROT_WRITE, flags, -1, 0);
    if (mapped == MAP_FAILED || (U64)(uintptr_t)mapped != hostStart) {
        if (mapped != MAP_FAILED) ::munmap(mapped, (size_t)hostLength);
        if (reservedHostRange) {
            k64NativeReleaseReservedHostRange(hostStart, hostLength);
        }
        return false;
    }
    ::memset(mapped, 0, (size_t)hostLength);
    const int finalProt = k64NativeProt(prot);
    if (finalProt != (PROT_READ | PROT_WRITE) &&
        ::mprotect(mapped, (size_t)hostLength, finalProt) != 0) {
        ::munmap(mapped, (size_t)hostLength);
        return false;
    }
    // MAP_FIXED may have replaced a partially overlapping tracked range. Do
    // not leave stale/overlapping NativeRange entries behind (emplace would
    // silently retain an old entry with the same key).
    nativeForgetRange(hostStart, hostLength);
    nativeRanges.emplace(hostStart, NativeRange{hostStart, hostLength, prot});
    fresh = true;
    return true;
}

bool KMemory64::nativeMapKuserAlias(U64 addr, U64 len, bool& fresh) {
    if (!k64IsKuserRange(addr, len)) return false;
    std::lock_guard<std::mutex> lock(g_k64KuserAliasMutex);
    if (!g_k64KuserAlias.host.load(std::memory_order_acquire)) {
        void* mapped = ::mmap(nullptr, (size_t)K64_KUSER_SHARED_SIZE,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped == MAP_FAILED || (uintptr_t)mapped < 0x100000000ULL) {
            if (mapped != MAP_FAILED) {
                ::munmap(mapped, (size_t)K64_KUSER_SHARED_SIZE);
            }
            return false;
        }
        g_k64KuserAlias.host.store(mapped, std::memory_order_release);
        fresh = true;
    } else {
        fresh = false;
    }
    if (!nativeKuserAliasHeld) {
        ++g_k64KuserAlias.refs;
        nativeKuserAliasHeld = true;
    }
    return true;
}

void KMemory64::nativeReleaseKuserAlias() {
    if (!nativeKuserAliasHeld) return;
    std::lock_guard<std::mutex> lock(g_k64KuserAliasMutex);
    nativeKuserAliasHeld = false;
    if (g_k64KuserAlias.refs > 0) --g_k64KuserAlias.refs;
    void* host = g_k64KuserAlias.host.load(std::memory_order_acquire);
    if (g_k64KuserAlias.refs == 0 && host) {
        // Shared-file registry entries outlive individual address spaces. Do
        // not leave them pointing into an alias that is about to disappear;
        // copy the final bytes back to the canonical process-shared page and
        // publish that pointer before unmapping the alias.
        {
            std::lock_guard<std::mutex> sharedLock(g_sharedFileMutex);
            const uintptr_t aliasStart = (uintptr_t)host;
            const uintptr_t aliasEnd = aliasStart + K64_KUSER_SHARED_SIZE;
            for (auto& entry : g_sharedFileRegistry) {
                SharedFilePage& shared = *entry.second;
                U8* current = shared.data->load(std::memory_order_acquire);
                const uintptr_t currentAddress = (uintptr_t)current;
                if (currentAddress >= aliasStart &&
                    currentAddress + K64_PAGE_SIZE <= aliasEnd) {
                    const size_t offset = currentAddress - aliasStart;
                    ::memcpy(shared.localData,
                             static_cast<const U8*>(host) + offset,
                             K64_PAGE_SIZE);
                    shared.data->store(shared.localData, std::memory_order_release);
                }
            }
        }
        ::munmap(host, (size_t)K64_KUSER_SHARED_SIZE);
        g_k64KuserAlias.host.store(nullptr, std::memory_order_release);
    }
}

void KMemory64::nativeUnmapAll() {
    for (const auto& entry : nativeRanges) {
        ::munmap((void*)(uintptr_t)entry.second.hostStart,
                 (size_t)entry.second.hostLength);
    }
    nativeRanges.clear();
    nativeReleaseKuserAlias();
}

void KMemory64::nativeDemoteSharedPages() {
    if (!nativeIdentityMode() || nativeRanges.empty()) return;
    std::lock_guard<std::mutex> lock(g_sharedFileMutex);
    for (auto& entry : g_sharedFileRegistry) {
        SharedFilePage& shared = *entry.second;
        U8* current = shared.data->load(std::memory_order_acquire);
        if (!current || current == shared.localData) continue;
        const U64 address = (U64)(uintptr_t)current;
        bool owned = false;
        for (const auto& native : nativeRanges) {
            const U64 end = native.second.hostStart + native.second.hostLength;
            if (address >= native.second.hostStart && address < end) {
                owned = true;
                break;
            }
        }
        if (!owned) continue;
        const U64 hostPage = k64NativeHostPageSize();
        const U64 hostStart = k64NativeAlignDown(address, hostPage);
        ::mprotect((void*)(uintptr_t)hostStart, (size_t)hostPage,
                   PROT_READ | PROT_WRITE);
        ::memcpy(shared.localData, current, K64_PAGE_SIZE);
        shared.data->store(shared.localData, std::memory_order_release);
    }
    k64DTlbInvalidateAll();
}
#endif

U64 KMemory64::mmapAnonymousFixed(U64 addr, U64 len, U32 prot) {
    if (len == 0) return (U64)-K_EINVAL;
    if (addr & K64_PAGE_MASK) return (U64)-K_EINVAL;
    if (len > (U64)-1 - K64_PAGE_MASK ||
        addr > (U64)-1 - len) return (U64)-K_EINVAL;

    U64 pageStart = addr >> K64_PAGE_SHIFT;
    U64 pageCount = (len + K64_PAGE_SIZE - 1) >> K64_PAGE_SHIFT;

    if (nativeIdentityMode() &&
        !nativeGuestRangeAllowed(addr, pageCount << K64_PAGE_SHIFT)) {
        klog_fmt("KMemory64: reject native MAP_FIXED anon outside guest window "
                 "addr=0x%llx len=0x%llx window=[0x%llx,0x%llx)",
                 (unsigned long long)addr, (unsigned long long)len,
                 (unsigned long long)K64_NATIVE_GUEST_WINDOW_START,
                 (unsigned long long)K64_NATIVE_GUEST_WINDOW_END);
        return (U64)-K_EINVAL;
    }

    U32 flags = K64_PAGE_MAPPED;
    if (prot & 0x1) flags |= K64_PAGE_READ;
    if (prot & 0x2) flags |= K64_PAGE_WRITE | K64_PAGE_READ; // x86: write implies read
    if (prot & 0x4) flags |= K64_PAGE_EXEC;

#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    bool nativeFresh = false;
    if (nativeIdentityMode()) {
        {
            BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
            for (U64 i = 0; i < pageCount; i++) {
                auto it = pages.find(pageStart + i);
                if (it != pages.end() && (it->second->flags & K64_PAGE_PINNED)) {
                    // A host pointer handed to a futex/atomic caller cannot be
                    // invalidated by MAP_FIXED while that caller may use it.
                    return (U64)-K_EBUSY;
                }
                if (it != pages.end()) it->second->demoteNativeShared();
            }
        }
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
        const U64 nativeLen = pageCount << K64_PAGE_SHIFT;
        const bool kuserRange = k64IsKuserRange(addr, nativeLen);
        if (kuserRange
                ? !nativeMapKuserAlias(addr, nativeLen, nativeFresh)
                : !nativeMapAnonymous(addr, nativeLen, prot, nativeFresh)) {
            return (U64)-K_ENOMEM;
        }
    }
#endif

    {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
        for (U64 i = 0; i < pageCount; i++) {
            K64Page* page = getOrAllocPage(pageStart + i, flags);
            page->flags = flags;
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
            if (nativeIdentityMode()) {
                const U64 guestPageAddress = addr + (i << K64_PAGE_SHIFT);
                U8* nativeAddress = k64KuserAliasFor(guestPageAddress);
                if (prot && nativeAddress) page->adoptNative(nativeAddress);
                else if (prot) page->adoptNative((U8*)(uintptr_t)guestPageAddress);
                else if (page->dataNative || page->dataShared) page->decommit();
                if (prot && !nativeFresh && (prot & 0x2)) {
                    ::memset(page->hostData(), 0, K64_PAGE_SIZE);
                }
            }
            if (!nativeIdentityMode()) {
#endif
            // Lazy commit: a FRESH reservation gets NO backing buffer — it reads as
            // zero (memcpyFromGuest zero-fills an uncommitted page) and only commits
            // on first write. This is the leak fix: wine's huge PROT_NONE spans no
            // longer cost a 4 KB buffer per page. An ALREADY-COMMITTED page being
            // re-mapped (overlap) is zeroed in place to preserve MAP_ANONYMOUS
            // semantics and keep the file-mmap head/tail preservation logic valid.
            if (page->committed()) {
                noteGuestWrite(pageStart + i); // zero-in-place = content change
                ::memset(page->hostData(), 0, K64_PAGE_SIZE);
            }
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
            }
#endif
        }
    }
    k64InvalidateProcessFetchCaches(process);
    // Keep the reservation map authoritative for the mmap region. A MAP_FIXED
    // landing here (wine remapping inside an earlier reservation, or the
    // allocator's own commit) updates `ranges` so later gap searches see the
    // real occupancy. rangeInsertLocked ignores addresses below the base and
    // splits any range this one overwrites. mmapMutex is recursive, so this is
    // safe whether or not the caller (mmapReserveAndMap) already holds it.
    if (pageStart >= k64MmapBasePage(this)) {
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
    if (len > (U64)-1 - K64_PAGE_MASK ||
        addr > (U64)-1 - len) return (U64)-K_EINVAL;

    U64 pageStart = addr >> K64_PAGE_SHIFT;
    U64 pageCount = (len + K64_PAGE_SIZE - 1) >> K64_PAGE_SHIFT;

    if (nativeIdentityMode() &&
        !nativeGuestRangeAllowed(addr, pageCount << K64_PAGE_SHIFT)) {
        klog_fmt("KMemory64: reject native shared map outside guest window "
                 "addr=0x%llx len=0x%llx", (unsigned long long)addr,
                 (unsigned long long)len);
        return (U64)-K_EINVAL;
    }

#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    const bool kuserRange = k64IsKuserRange(addr, pageCount << K64_PAGE_SHIFT);
    if (nativeIdentityMode()) {
        // One registry page can have only one direct host pointer. Refuse a
        // second identity mapping at a different guest address rather than
        // retargeting the pointer and silently redirecting the first mapping.
        // Sparse aliases continue to use the canonical shared page.
        const std::string requestedPath(path);
        for (U64 i = 0; i < pageCount && !kuserRange; i++) {
            const U64 fpage = (fileOffset >> K64_PAGE_SHIFT) + i;
            std::shared_ptr<SharedFilePage> shared = findSharedFilePage(requestedPath, fpage);
            if (!shared) continue;
            const U8* current = shared->data->load(std::memory_order_acquire);
            const U8* fixed = (const U8*)(uintptr_t)((pageStart + i) << K64_PAGE_SHIFT);
            if (current != shared->localData && current != fixed) {
                return (U64)-K_EBUSY;
            }
        }
        {
            BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
            for (U64 i = 0; i < pageCount; i++) {
                auto it = pages.find(pageStart + i);
                if (it != pages.end() && (it->second->flags & K64_PAGE_PINNED)) {
                    return (U64)-K_EBUSY;
                }
                if (it != pages.end()) it->second->demoteNativeShared();
            }
        }
    }
    bool nativeFresh = false;
    if (nativeIdentityMode()) {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
        if (kuserRange
                ? !nativeMapKuserAlias(addr, pageCount << K64_PAGE_SHIFT,
                                       nativeFresh)
                : !nativeMapAnonymous(addr, pageCount << K64_PAGE_SHIFT,
                                      prot | 0x3u, nativeFresh)) {
            return (U64)-K_ENOMEM;
        }
    }
#endif

    U32 flags = K64_PAGE_MAPPED | K64_PAGE_SHARED;
    if (prot & 0x1) flags |= K64_PAGE_READ;
    if (prot & 0x2) flags |= K64_PAGE_WRITE | K64_PAGE_READ;
    if (prot & 0x4) flags |= K64_PAGE_EXEC;

    std::string p(path);
    {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
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
            bool nativeShared = false;
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
            if (nativeIdentityMode()) {
                U8* fixed = kuserRange
                    ? k64KuserAliasFor((pageStart + i) << K64_PAGE_SHIFT)
                    : (U8*)(uintptr_t)((pageStart + i) << K64_PAGE_SHIFT);
                {
                    std::lock_guard<std::mutex> sharedLock(g_sharedFileMutex);
                    U8* previous = shared->data->load(std::memory_order_acquire);
                    if (previous != fixed) {
                        ::memcpy(fixed, previous, K64_PAGE_SIZE);
                        shared->data->store(fixed, std::memory_order_release);
                        k64DTlbInvalidateAll();
                    }
                }
                nativeShared = true;
                // A sparse wineserver can retain this alias after the FEX client
                // logically unmaps it. Keep the host page until the identity
                // address space itself is destroyed.
                page->flags |= K64_PAGE_PINNED;
            }
#endif
            page->adoptShared(shared->data, nativeShared, shared->localData);
            k64DTlbInvalidateAll();
            if (std::getenv("BW64_SHAREMAP")) {
                klog_fmt("SHAREMAP: pid=%u %s fpage=%llu guest=0x%llx path='%s'",
                         (unsigned)(process ? process->id : 0), created ? "CREATE" : "ALIAS ",
                         (unsigned long long)fpage,
                         (unsigned long long)((pageStart + i) << K64_PAGE_SHIFT), p.c_str());
            }
        }
    }
    k64InvalidateProcessFetchCaches(process);
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    if (nativeIdentityMode()) {
        if (!kuserRange) {
            const U64 hostPage = k64NativeHostPageSize();
            const U64 hostStart = k64NativeAlignDown(addr, hostPage);
            const U64 hostEnd = k64NativeAlignUp(addr + len, hostPage);
            // Native shared pages can be reached through the canonical pointer
            // by sparse aliases. Keep their host backing writable even when the
            // guest requested read-only/execute permissions.
            BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
            if (::mprotect((void*)(uintptr_t)hostStart,
                           (size_t)(hostEnd - hostStart),
                           k64NativeProt(prot | 0x3u)) != 0) {
                return (U64)-K_EINVAL;
            }
            for (auto& entry : nativeRanges) {
                NativeRange& range = entry.second;
                if (range.hostStart < hostEnd &&
                    range.hostStart + range.hostLength > hostStart) {
                    range.prot = prot | 0x3u;
                }
            }
        }
    }
#endif
    if (pageStart >= k64MmapBasePage(this)) {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
        rangeInsertLocked(pageStart, pageCount, prot, (U8)MMAP_ANON);
    }
    return addr;
}

// Record a reservation in the ordered `ranges` map. Caller holds mmapMutex.
// Splits/clamps any pre-existing ranges that the new one overwrites so the map
// stays a non-overlapping cover of the live mmap-region reservations (wine
// MAP_FIXED-remaps inside an earlier reservation constantly). Only the mmap
// region (>= the active mmap base) is tracked; the loader's fixed low maps don't need
// allocation bookkeeping because the gap search never looks below the base.
void KMemory64::rangeInsertLocked(U64 startPage, U64 pageCount, U32 prot, U8 kind) {
    if (pageCount == 0 || startPage < k64MmapBasePage(this)) return;
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
    if (length > (U64)-1 - K64_PAGE_MASK) return (U64)-K_EINVAL;
    U64 pageCount = (length + K64_PAGE_MASK) >> K64_PAGE_SHIFT;
    if (pageCount == 0) pageCount = 1;

    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
    const U64 mmapBase = k64MmapBase(this);
    const U64 mmapBasePage = mmapBase >> K64_PAGE_SHIFT;
    if (mmapNext == 0) mmapNext = mmapBase;
    U64 cursor = mmapNext >> K64_PAGE_SHIFT;
    if (cursor < mmapBasePage) cursor = mmapBasePage;

    // Darwin maps memory at the host page granularity (16 KiB on current iOS
    // devices), while the x86-64 guest uses 4 KiB pages.  An automatic mmap
    // placed immediately after a 4 KiB-aligned ELF image can therefore share
    // a host page with that image.  nativeMapAnonymous must reject extending
    // such a partially tracked host page because MAP_FIXED would overwrite
    // the executable bytes already living there.  Automatic Linux mappings
    // are free to choose any page-aligned address, so keep their starts on a
    // host-page boundary in native-identity mode and leave the unusable tail
    // of the preceding host page as padding.
    U64 allocationAlignmentPages = 1;
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    if (nativeIdentityMode()) {
        allocationAlignmentPages = k64NativeHostPageSize() >> K64_PAGE_SHIFT;
        if (allocationAlignmentPages == 0) allocationAlignmentPages = 1;
    }
#endif
    auto alignCandidate = [allocationAlignmentPages](U64 value) {
        if (allocationAlignmentPages <= 1) return value;
        const U64 mask = allocationAlignmentPages - 1;
        if (value > UINT64_MAX - mask) return UINT64_MAX;
        return (value + mask) & ~mask;
    };

    // Gap search over the ordered reservation map: O(log n + ranges scanned),
    // not O(pages). Walk ranges from `cursor`; the first hole of `pageCount`
    // pages between consecutive reservations (or after the last one) wins.
    U64 candidate = alignCandidate(cursor);
    auto it = ranges.lower_bound(candidate);
    // A range starting before `candidate` may still cover it — back up one and
    // push `candidate` past its end if so.
    if (it != ranges.begin()) {
        auto prev = std::prev(it);
        U64 prevEnd = prev->second.startPage + prev->second.pageCount;
        if (prevEnd > candidate) candidate = alignCandidate(prevEnd);
    }
    for (; it != ranges.end(); ++it) {
        U64 gapEnd = it->second.startPage; // exclusive
        if (gapEnd >= candidate + pageCount) break; // fits before this range
        U64 rangeEnd = it->second.startPage + it->second.pageCount;
        if (rangeEnd > candidate) {
            candidate = alignCandidate(rangeEnd); // jump past it
        }
    }
    // `candidate` now points at a hole large enough (either before `it` or past
    // the last range — address space above is effectively unbounded for us).
    const U64 windowEndPage = K64_NATIVE_GUEST_WINDOW_END >> K64_PAGE_SHIFT;
    if (nativeIdentityMode() &&
        (candidate > windowEndPage || pageCount > windowEndPage - candidate)) {
        return (U64)-K_ENOMEM;
    }
    U64 addr = candidate << K64_PAGE_SHIFT;
    // Map the pages NOW, under mmapMutex (recursive), so a concurrent sibling's
    // allocation sees them taken. mmapAnonymousFixed registers the reservation
    // in `ranges` itself, so the gap is claimed before we drop the lock.
    U64 mapped = mmapAnonymousFixed(addr, pageCount << K64_PAGE_SHIFT, prot);
    if (mapped != addr) return mapped;
    mmapNext = alignCandidate(candidate + pageCount) << K64_PAGE_SHIFT;
    return addr;
}

U64 KMemory64::mprotect(U64 addr, U64 len, U32 prot) {
    if (addr & K64_PAGE_MASK) return (U64)-K_EINVAL;
    if (len == 0) return addr;
    if (addr > (U64)-1 - len) return (U64)-K_EINVAL;
    if (len > (U64)-1 - K64_PAGE_MASK) return (U64)-K_EINVAL;
    U64 pageStart = addr >> K64_PAGE_SHIFT;
    U64 pageCount = (len + K64_PAGE_SIZE - 1) >> K64_PAGE_SHIFT;

#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    if (nativeIdentityMode()) {
        if (!nativeGuestRangeAllowed(addr, pageCount << K64_PAGE_SHIFT)) {
            klog_fmt("KMemory64: reject native mprotect outside guest window "
                     "addr=0x%llx len=0x%llx", (unsigned long long)addr,
                     (unsigned long long)len);
            return (U64)-K_EINVAL;
        }
        const bool kuserRange = k64IsKuserRange(addr, pageCount << K64_PAGE_SHIFT);
        const U64 hostPage = k64NativeHostPageSize();
        const U64 hostStart = k64NativeAlignDown(addr, hostPage);
        if (addr + len > UINT64_MAX - (hostPage - 1)) return (U64)-K_EINVAL;
        const U64 hostEnd = k64NativeAlignUp(addr + len, hostPage);
        U32 hostProtFlags = prot & 0x3u;
        {
            BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
            for (U64 i = 0; i < pageCount; i++) {
                auto it = pages.find(pageStart + i);
                if (it == pages.end() || !(it->second->flags & K64_PAGE_MAPPED)) {
                    // mprotect changes permissions; it never creates a mapping.
                    return (U64)-K_ENOMEM;
                }
                if (!prot && (it->second->flags & K64_PAGE_PINNED)) {
                    // getRamPtr may have handed this backing page to a futex or
                    // atomic waiter. Removing its host access would leave that
                    // caller with a dangling or faulting pointer.
                    return (U64)-K_EBUSY;
                }
                if (it->second->dataShared) hostProtFlags |= 0x3u;
            }
            // Host pages are 16 KiB on current iOS devices. Preserve the
            // permissions of mapped guest subpages that share the first or
            // last host page, and keep a shared-file page writable so sparse
            // aliases that use its canonical pointer cannot fault. Guest EXEC
            // remains only in K64_PAGE_EXEC metadata.
            auto includePage = [&](U64 pageNum, bool target) {
                auto it = pages.find(pageNum);
                if (it == pages.end() || !(it->second->flags & K64_PAGE_MAPPED)) return;
                if (!target) hostProtFlags |= k64GuestProtFromPageFlags(it->second->flags);
                if (it->second->dataShared) hostProtFlags |= 0x3u;
            };
            const U64 hostPages = hostPage >> K64_PAGE_SHIFT;
            for (U64 sub = 0; sub < hostPages; sub++) {
                const U64 firstPage = (hostStart >> K64_PAGE_SHIFT) + sub;
                includePage(firstPage, firstPage >= pageStart && firstPage < pageStart + pageCount);
                if (hostEnd > hostStart + hostPage) {
                    const U64 lastPage = ((hostEnd - hostPage) >> K64_PAGE_SHIFT) + sub;
                    includePage(lastPage, lastPage >= pageStart && lastPage < pageStart + pageCount);
                }
            }
        }
        if (!kuserRange) {
            BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
            if (!nativeRangeCovers(hostStart, hostEnd) ||
                ::mprotect((void*)(uintptr_t)hostStart, (size_t)(hostEnd - hostStart),
                           k64NativeProt(hostProtFlags)) != 0) {
                return (U64)-K_EINVAL;
            }
            for (auto& entry : nativeRanges) {
                NativeRange& range = entry.second;
                if (range.hostStart < hostEnd && range.hostStart + range.hostLength > hostStart) {
                    range.prot = hostProtFlags;
                }
            }
        }
    }
#endif

    U32 newFlags = K64_PAGE_MAPPED;
    if (prot & 0x1) newFlags |= K64_PAGE_READ;
    if (prot & 0x2) newFlags |= K64_PAGE_WRITE | K64_PAGE_READ; // write implies read
    if (prot & 0x4) newFlags |= K64_PAGE_EXEC;

    {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
        for (U64 i = 0; i < pageCount; i++) {
            // Preserve SHARED bit if it was set; everything else is replaced
            // wholesale. Holes (no page) are skipped — see header comment.
            auto it = pages.find(pageStart + i);
            if (it == pages.end()) continue;
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
            if (nativeIdentityMode()) {
                if (prot && !it->second->dataShared) {
                    it->second->adoptNative((U8*)(uintptr_t)(addr + (i << K64_PAGE_SHIFT)));
                } else if (!prot && it->second->dataNative && !it->second->dataShared) {
                    it->second->decommit();
                }
            }
#endif
            U32 preserved = it->second->flags & (K64_PAGE_SHARED | K64_PAGE_PINNED);
            it->second->flags = newFlags | preserved;
        }
    }
    k64InvalidateProcessFetchCaches(process);
    return addr;
}

U64 KMemory64::munmap(U64 addr, U64 len) {
    if (addr & K64_PAGE_MASK) return (U64)-K_EINVAL;
    if (len == 0) return 0;
    if (addr > (U64)-1 - len) return (U64)-K_EINVAL;
    if (len > (U64)-1 - K64_PAGE_MASK) return (U64)-K_EINVAL;
    U64 pageStart = addr >> K64_PAGE_SHIFT;
    U64 pageCount = (len + K64_PAGE_SIZE - 1) >> K64_PAGE_SHIFT;

#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    if (nativeIdentityMode()) {
        if (!nativeGuestRangeAllowed(addr, pageCount << K64_PAGE_SHIFT)) {
            klog_fmt("KMemory64: reject native munmap outside guest window "
                     "addr=0x%llx len=0x%llx", (unsigned long long)addr,
                     (unsigned long long)len);
            return (U64)-K_EINVAL;
        }
        const bool kuserRange = k64IsKuserRange(addr, pageCount << K64_PAGE_SHIFT);
        const U64 hostPage = k64NativeHostPageSize();
        if ((hostPage % K64_PAGE_SIZE) != 0) return (U64)-K_EINVAL;
        if (addr + len > UINT64_MAX - (hostPage - 1)) return (U64)-K_EINVAL;
        const U64 hostStart = k64NativeAlignDown(addr, hostPage);
        const U64 hostEnd = k64NativeAlignUp(addr + len, hostPage);
        {
            BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
            for (U64 i = 0; i < pageCount; i++) {
                auto it = pages.find(pageStart + i);
                if (it != pages.end()) {
                    const bool pinned = (it->second->flags & K64_PAGE_PINNED) != 0;
                    if (!pinned) {
                        it->second->demoteNativeShared();
                        it->second->decommit();
                    }
                    it->second->flags = pinned ? K64_PAGE_PINNED : 0;
                }
            }
        }
        // A host page can contain four guest pages on iOS. Keep it mapped while
        // any guest subpage (or pinned native pointer) still owns that host page;
        // otherwise release exactly one host page and split the tracking range.
        if (!kuserRange) {
            BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
            for (U64 host = hostStart; host < hostEnd; host += hostPage) {
                bool keepHostPage = false;
                {
                    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
                    for (U64 sub = 0; sub < hostPage; sub += K64_PAGE_SIZE) {
                        auto it = pages.find((host + sub) >> K64_PAGE_SHIFT);
                        if (it != pages.end() &&
                            (it->second->flags & (K64_PAGE_MAPPED | K64_PAGE_PINNED))) {
                            keepHostPage = true;
                            break;
                        }
                    }
                }
                if (!keepHostPage && nativeRangeCovers(host, host + hostPage)) {
                    if (::munmap((void*)(uintptr_t)host, (size_t)hostPage) == 0) {
                        nativeForgetRange(host, hostPage);
                    }
                }
            }
        }
        k64DTlbInvalidateAll();
        k64InvalidateProcessFetchCaches(process);
    }
#endif

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
            U8* host = page->hostData();
            k64DTlbInsert(this, pageNum, host);
            ::memcpy(d, host + offsetInPage, (size_t)chunk);
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
        U8* host = p->hostData();
        k64DTlbInsert(this, pageNum, host);
        return host[addr & K64_PAGE_MASK];
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
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
        if (nativeIdentityMode() && (!page->hostData() || !page->dataNative)) {
            kpanic("KMemory64 native identity getRamPtr on an unmapped page");
            return nullptr;
        }
#endif
        page->flags |= K64_PAGE_PINNED;
        data = page->commit();
    }
    return data + offsetInPage;
}

void KMemory64::writew(U64 addr, U16 value) { memcpyToGuest(addr, &value, 2); }
void KMemory64::writed(U64 addr, U32 value) { memcpyToGuest(addr, &value, 4); }
void KMemory64::writeq(U64 addr, U64 value) { memcpyToGuest(addr, &value, 8); }

#endif // BOXEDWINE_GUEST_X64
