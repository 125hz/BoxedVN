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
#include "native_map_plan.h"
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

// ---- Guest page-flag provenance ------------------------------------------
//
// Every write of K64Page::flags stamps the page with the operation that made
// it. The stamp is a single wrapping counter shared by the whole process: it
// carries no meaning on its own, only order, which is all that is needed to
// say whether the mmap or the mprotect wrote last. Relaxed is enough -- the
// value is diagnostic and is read under pagesMutex, which already orders the
// flag write it accompanies.
static std::atomic<U32> g_k64PageWriteStamp {0};

static U16 k64NextPageWriteStamp() {
    return (U16)(g_k64PageWriteStamp.fetch_add(1, std::memory_order_relaxed) &
                 0xFFFFu);
}

const char* k64PageWriterName(U8 writer) {
    switch (writer) {
        case K64_WRITER_MMAP_ANON: return "mmap-anon";
        case K64_WRITER_MMAP_FILE: return "mmap-file";
        case K64_WRITER_MPROTECT:  return "mprotect";
        case K64_WRITER_MUNMAP:    return "munmap";
        case K64_WRITER_COMMIT:    return "commit";
        case K64_WRITER_PIN:       return "pin";
        case K64_WRITER_CLONE:     return "clone";
        default:                   return "none";
    }
}

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

// What the host VM actually permits at this address, as K_PROT_* bits, and
// whether a region exists there at all. This is the OTHER ledger: the page map
// says what the guest is entitled to, this says what the hardware will allow,
// and a translated guest access that faults inside a guest lane means the two
// disagree. Reading it is what makes the fault witness able to distinguish
// "no host page here" from "host page here, but PROT_NONE" -- two different
// bugs that produce the same guest symptom.
static U32 k64NativeHostProtOf(U64 address, bool& present) {
    present = false;
#if defined(__APPLE__)
    vm_address_t regionStart = (vm_address_t)address;
    vm_size_t regionSize = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t infoCount = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t objectName = MACH_PORT_NULL;
    const kern_return_t result = vm_region_64(
        mach_task_self(), &regionStart, &regionSize, VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info), &infoCount, &objectName);
    if (objectName != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), objectName);
    }
    if (result != KERN_SUCCESS || regionSize == 0) return 0;
    // vm_region_64 returns the NEXT region when the cursor sits in a hole, so
    // a region that starts above the address proves the address is unmapped.
    if ((U64)regionStart > address ||
        (U64)regionStart + (U64)regionSize <= address) {
        return 0;
    }
    present = true;
    U32 prot = 0;
    if (info.protection & VM_PROT_READ) prot |= K_PROT_READ;
    if (info.protection & VM_PROT_WRITE) prot |= K_PROT_WRITE;
    if (info.protection & VM_PROT_EXECUTE) prot |= K_PROT_EXEC;
    return prot;
#else
    (void)address;
    return 0;
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
    // KUSER_SHARED_DATA is a canonical low address like any other now, so the
    // deterministic low alias hosts it through the ordinary anonymous path.
    // Keeping the old dedicated alias as well would leave two host backings
    // for 0x7ffe0000 that could disagree.
    static_assert(K64_KUSER_SHARED_BASE + K64_KUSER_SHARED_SIZE <=
                  K64_NATIVE_LOW_GUEST_LIMIT,
                  "KUSER_SHARED_DATA must resolve through the low alias");
    (void)addr;
    (void)len;
    return false;
}

static U8* k64KuserAliasFor(U64 guestAddress) {
    // Superseded by the deterministic low alias. KUSER_SHARED_DATA is a
    // canonical low guest address like any other now, so it is backed by the
    // ordinary anonymous path through k64GuestToHostAddress. Returning null
    // here keeps every caller -- including the Darwin SA_SIGINFO path, which
    // must stay lock-free -- on that single backing rather than a second one.
    (void)guestAddress;
    return nullptr;
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
// All of them bump the global generation; a TLB hit requires a generation
// match. CPU64's instruction-fetch page cache validates against the SAME
// counter (k64PageCacheGeneration), so one increment retires both caches on
// every thread and no thread has to reach into another's.
// Entries are tagged with the owning KMemory64* because kernel code on one
// thread can touch another process's memory (fork ctid writes, ptrace-style
// peeks), and one host thread serves exactly one running guest thread.
#ifdef BOXEDWINE_BLOCK_CACHE_INFRA
bool bw64PageHoldsBlocks(U64 pageNum); // defined below, with the block tables
#endif

// The generation both host-pointer caches (this file's data TLB and CPU64's
// instruction-fetch page cache) validate against. See kmemory64.h. Starts at
// 1 so a zero-initialised cache entry can never look current.
std::atomic<U32> g_k64PageCacheGeneration{1};

namespace {
struct K64DTlbEntry { const void* mem; U64 page; U8* data; U32 gen; };
constexpr U32 K64_DTLB_SIZE = 64; // direct-mapped, per-thread (~2KB)
thread_local K64DTlbEntry g_k64DTlb[K64_DTLB_SIZE];

// A write whose page has no backing buffer (a failed or withdrawn commit)
// is dropped and reported rather than written through NULL. Eight lines,
// then one per 4096, so a runaway guest does not flood the log.
void k64ReportMissingBacking(int pid, U64 pageNum, const char* op);

inline U8* k64DTlbLookup(const void* mem, U64 pageNum) {
    const K64DTlbEntry& e = g_k64DTlb[pageNum & (K64_DTLB_SIZE - 1)];
    if (e.mem == mem && e.page == pageNum &&
        e.gen == k64PageCacheGeneration()) {
        return e.data;
    }
    return nullptr;
}
// `gen` must be read BEFORE the page is resolved: an invalidation that lands
// while we are looking the buffer up has to lose the race, or we would stamp
// the new generation onto a pointer it was meant to retire.
inline void k64DTlbInsert(const void* mem, U64 pageNum, U8* data, U32 gen) {
    // Never memoize a hole. A null entry would be handed straight back by the
    // next lookup and indexed by an in-page offset.
    if (!data) {
        return;
    }
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
    e.gen = gen;
}
inline void k64DTlbInvalidateAll() {
    k64InvalidatePageCaches();
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
    (void)process;
    // Retire every instruction-fetch page cache by bumping the shared
    // generation. This used to walk the process's threads and clear each
    // sibling's CPU64 cache fields directly, which is a data race against a
    // sibling that is running guest code: fetchByte compares the cached page
    // number and then loads the cached pointer, so a thread could match the
    // page an instant before the invalidator nulled the pointer and then index
    // NULL by the in-page offset. That is the host SIGSEGV at a bare page
    // offset (0x3c5, 0x5a2, 0x944, 0xbd8) inside CPU64::step. The generation
    // is process-wide by construction — one extra miss for unrelated address
    // spaces, and no thread ever writes into another thread's cache.
    k64InvalidatePageCaches();
}

static void (*g_k64TranslatedCodeInvalidator)(KProcess*, U64, U64) = nullptr;

void KMemory64::setTranslatedCodeInvalidator(
    void (*invalidator)(KProcess* process, U64 addr, U64 len)) {
    g_k64TranslatedCodeInvalidator = invalidator;
}

// Every path that changes what bytes a guest range holds ends here as well as
// in k64InvalidateProcessFetchCaches: a fresh mapping over pages that were
// mapped before, and an unmap. Queued, not applied: see the header. A queue
// that grows past a bound collapses into one whole-window entry rather than
// growing without limit while no translated thread drains it.
void KMemory64::queueTranslatedCodeInvalidation(U64 addr, U64 len) {
    if (!g_k64TranslatedCodeInvalidator || !process || len == 0) return;
    std::lock_guard<std::mutex> guard(pendingTranslatedInvalidationsMutex);
    if (pendingTranslatedInvalidations.size() >= 1024) {
        pendingTranslatedInvalidations.clear();
        pendingTranslatedInvalidations.emplace_back(0, ~0ULL);
        return;
    }
    if (!pendingTranslatedInvalidations.empty()) {
        auto& last = pendingTranslatedInvalidations.back();
        if (last.first + last.second == addr) {
            last.second += len;
            return;
        }
    }
    pendingTranslatedInvalidations.emplace_back(addr, len);
}

void KMemory64::flushTranslatedCodeInvalidations() {
    if (!g_k64TranslatedCodeInvalidator || !process) return;
    std::vector<std::pair<U64, U64>> pending;
    {
        std::lock_guard<std::mutex> guard(pendingTranslatedInvalidationsMutex);
        if (pendingTranslatedInvalidations.empty()) return;
        pending.swap(pendingTranslatedInvalidations);
    }
    for (const auto& range : pending) {
        g_k64TranslatedCodeInvalidator(process, range.first, range.second);
    }
}

// Monotonic across every address space this image ever builds, so an exec
// replacement is never confused with the address space it replaced.
static std::atomic<U64> g_addressSpaceGeneration {1};

KMemory64::KMemory64(KProcess* process, bool nativeIdentity) : process(process) {
    generation = g_addressSpaceGeneration.fetch_add(1, std::memory_order_relaxed);
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
    // Canonical low guest addresses are hostable: they are dereferenced
    // through the deterministic alias rather than at their own address. This
    // is what lets Wine reserve its TEB block below 2 GiB, keep
    // KUSER_SHARED_DATA at 0x7ffe0000 -- now served by the same alias rather
    // than a second mapping -- and load PE images at their preferred bases.
    if (end <= K64_NATIVE_LOW_GUEST_LIMIT) {
        return true;
    }
    // The identity lane is the block immediately above the alias window. A
    // guest address inside the alias window itself is refused: it would be
    // indistinguishable from the host address of some canonical low page.
    if (addr >= K64_NATIVE_GUEST_IMAGE_BASE &&
        end <= K64_NATIVE_GUEST_HIGH_END) {
        return true;
    }
    // Wine's top-down arena, served from its own host block. A range that
    // straddles the arena boundary is refused rather than split: one mapping
    // has to land in one host lane.
    return addr >= K64_NATIVE_TOP_GUEST_BASE &&
           end <= K64_NATIVE_TOP_GUEST_END;
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
    // The child inherits the parent's inaccessible reservations: they are part
    // of the address space's shape even though nothing is mapped in them.
    {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
        sparseReservations = from->sparseReservations;
    }

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
        copy->writeFlags(kv.second->flags, K64_WRITER_CLONE,
                         k64NextPageWriteStamp());
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
                copy->writeFlags(copy->flags & ~K64_PAGE_PINNED,
                                 K64_WRITER_CLONE, k64NextPageWriteStamp());
            } else {
                copy->writeFlags(
                    copy->flags & ~(K64_PAGE_SHARED | K64_PAGE_PINNED),
                    K64_WRITER_CLONE, k64NextPageWriteStamp());
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

K64Page* KMemory64::getOrAllocPage(U64 pageNum, U32 flagsIfNew, U8 writer) {
    // Lock spans find+emplace so a concurrent allocator can't rehash the map
    // under our iterator. The returned raw K64Page* stays valid after we drop
    // the lock because the unique_ptr payload never moves (see header note).
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
    auto it = pages.find(pageNum);
    if (it != pages.end()) {
        return it->second.get();
    }
    auto page = std::make_unique<K64Page>();
    page->writeFlags(flagsIfNew, writer, k64NextPageWriteStamp());
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
U8* KMemory64::commitPageLocked(U64 pageNum, U32 flagsIfNew, U8 writer) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
    // recursive mutex: re-enter OK
    K64Page* page = getOrAllocPage(pageNum, flagsIfNew, writer);
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
    return p ? p->hostData() : nullptr;
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

// Record exactly [start, start+length) as tracked with `prot`. The protection
// of a host page outside that interval is never touched -- the loops this
// replaces assigned the new protection to every range that merely OVERLAPPED
// the request, so a one-page mprotect rewrote the recorded protection of a
// whole multi-megabyte reservation. Coalescing with an adjacent entry of the
// same protection keeps a reservation that is reprotected host page by host
// page from becoming one ledger entry per 16 KiB.
void KMemory64::nativeTrackRangeLocked(U64 start, U64 length, U32 prot) {
    if (!length || start > UINT64_MAX - length) return;
    U64 mergedStart = start;
    U64 mergedEnd = start + length;
    nativeForgetRange(start, length);
    auto candidate = nativeRanges.lower_bound(mergedStart);
    if (candidate != nativeRanges.begin()) {
        auto previous = std::prev(candidate);
        if (previous->second.hostStart + previous->second.hostLength ==
                mergedStart &&
            previous->second.prot == prot) {
            mergedStart = previous->second.hostStart;
            nativeRanges.erase(previous);
        }
    }
    auto after = nativeRanges.find(mergedEnd);
    if (after != nativeRanges.end() && after->second.prot == prot) {
        mergedEnd = after->second.hostStart + after->second.hostLength;
        nativeRanges.erase(after);
    }
    nativeRanges.emplace(mergedStart,
                         NativeRange{mergedStart, mergedEnd - mergedStart, prot});
}

// The union of what the four guest subpages of ONE host page are entitled to.
// This is the single reading of the page map that decides a host protection;
// mmap, mprotect, munmap and the fault repair all go through it, so there is
// no path left that can compute a protection for one host page and apply it to
// another. `pages` is keyed by CANONICAL guest page, so the host page has to be
// translated back before its subpages can be named.
U32 KMemory64::nativeHostPageProtLocked(U64 hostPageStart, U64 hostPageSize,
                                        U64 excludePage,
                                        K64NativeFaultRepair* report) const {
    U32 unionProt = 0;
    const U64 guestOfHostPage = k64HostToGuestAddress(hostPageStart);
    for (U64 sub = 0; sub < hostPageSize; sub += K64_PAGE_SIZE) {
        const U64 pageNum = (guestOfHostPage + sub) >> K64_PAGE_SHIFT;
        auto it = pages.find(pageNum);
        if (it == pages.end()) continue;
        if (report && pageNum != excludePage &&
            it->second->lastWriter != K64_WRITER_NONE &&
            (U32)it->second->lastWriteStamp >= report->neighbourWriteStamp) {
            report->neighbourWriter = k64PageWriterName(it->second->lastWriter);
            report->neighbourWriteFlags = it->second->lastFlags;
            report->neighbourWriteStamp = it->second->lastWriteStamp;
        }
        // A pinned page's host pointer is held by a futex or atomic waiter
        // across a blocking wait, so its access cannot be withdrawn even when
        // the guest page itself grants nothing.
        if (it->second->flags & K64_PAGE_PINNED) unionProt |= 0x3u;
        if (!(it->second->flags & K64_PAGE_MAPPED)) continue;
        unionProt |= k64GuestProtFromPageFlags(it->second->flags);
        // K64_PAGE_EXEC is guest metadata, but the translator has to READ the
        // bytes of a page the guest may execute, and x86 paging has no
        // execute-without-read anyway. Without this an execute-only guest page
        // whose host page nothing else claims would become PROT_NONE and the
        // decoder would fault on its own instruction fetch.
        if (it->second->flags & K64_PAGE_EXEC) unionProt |= 0x1u;
        // A shared-file page is reachable through its canonical pointer by
        // sparse aliases, so it stays host-writable wherever it is mapped.
        if (it->second->dataShared) unionProt |= 0x3u;
    }
    return unionProt;
}

// The nearest READABLE guest page either side of a faulting one. A refusal
// says the guest was not entitled to the address; this says what it WAS
// entitled to nearby, which is the fact that separates the two ways a program
// reaches such an address.
//
// A loop walking a buffer off its end faults on the first page past it, so the
// last readable page below the fault ends exactly where the buffer ends and
// `committedBelow` names that boundary -- the size of the object the guest
// overran, computed from its own base. A pointer that never pointed at
// anything has no readable page below it at all within the window.
//
// `committedAbove` closes the other case: a small hole with committed memory
// on both sides is a page whose rights were lost, which is this file's own
// failure mode; a hole that runs to the end of the window is address space the
// guest genuinely has not committed.
void KMemory64::nativeCommittedNeighbourhoodLocked(
    U64 guestPageNumber, K64NativeFaultRepair& report) const {
    report.committedBelow = 0;
    report.committedAbove = 0;
    for (U64 step = 1; step <= K64_FAULT_NEIGHBOURHOOD_PAGES; step++) {
        if (step > guestPageNumber) break;
        auto it = pages.find(guestPageNumber - step);
        if (it == pages.end()) continue;
        if (!(it->second->flags & K64_PAGE_MAPPED)) continue;
        if (!(it->second->flags & K64_PAGE_READ)) continue;
        // The first address past the last page the guest could read.
        report.committedBelow = (guestPageNumber - step + 1) << K64_PAGE_SHIFT;
        break;
    }
    for (U64 step = 1; step <= K64_FAULT_NEIGHBOURHOOD_PAGES; step++) {
        auto it = pages.find(guestPageNumber + step);
        if (it == pages.end()) continue;
        if (!(it->second->flags & K64_PAGE_MAPPED)) continue;
        if (!(it->second->flags & K64_PAGE_READ)) continue;
        report.committedAbove = (guestPageNumber + step) << K64_PAGE_SHIFT;
        break;
    }
}

// Bring the host VM back into agreement with the page map over [hostStart,
// hostEnd), one host page at a time and in BOTH directions: a host page whose
// guest subpages all lost their rights becomes PROT_NONE, which is what makes
// a guest guard page that owns its host page genuinely fault, and a host page
// that gained a right is raised to exactly that.
//
// A host page this address space does not track is skipped rather than
// refused. Refusing was the defect: mprotect asked nativeRangeCovers about the
// whole HOST-rounded interval and returned -EINVAL for the entire request when
// any edge host page was untracked, so the guest pages it named kept the flags
// of the reservation they were being committed out of -- K64_PAGE_MAPPED with
// no access bits -- while Wine recorded the commit and carried on. The fault
// repair materialises an untracked host page later from this same union.
void KMemory64::nativeReconcileHostProt(U64 hostStart, U64 hostEnd) {
    const U64 hostPage = k64NativeHostPageSize();
    if (hostEnd <= hostStart) return;
    if (hostPage == 0 || (hostPage % K64_PAGE_SIZE) != 0) return;
    // Bounded: a host mprotect over our own mapping does not fail, so a line
    // here is a real defect and a handful of them is enough to see it.
    static std::atomic<U32> reprotectFailures {0};
    // Consecutive host pages that want the same protection are issued as one
    // call, so a multi-gigabyte reservation costs a couple of mprotects rather
    // than one per 16 KiB.
    U64 runStart = 0;
    U64 runEnd = 0;
    U32 runProt = 0;
    auto flush = [&]() {
        if (runEnd <= runStart) return;
        if (::mprotect((void*)(uintptr_t)runStart, (size_t)(runEnd - runStart),
                       k64NativeProt(runProt)) == 0) {
            nativeTrackRangeLocked(runStart, runEnd - runStart, runProt);
        } else if (reprotectFailures.fetch_add(1, std::memory_order_relaxed) <
                       16) {
            klog_fmt("BOXEDWINE_X64_HOST_REPROTECT host=[0x%llx,0x%llx) "
                     "prot=0x%x result=failed",
                     (unsigned long long)runStart, (unsigned long long)runEnd,
                     (unsigned)runProt);
        }
        runEnd = runStart;
    };
    for (U64 host = hostStart; host < hostEnd; host += hostPage) {
        // Only a host address this address space actually serves as the image
        // of a guest address may be reprotected here.
        if (k64GuestToHostAddress(k64HostToGuestAddress(host)) != host ||
            !nativeRangeCovers(host, host + hostPage)) {
            flush();
            runStart = host + hostPage;
            runEnd = runStart;
            continue;
        }
        U32 prot;
        {
            BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
            prot = nativeHostPageProtLocked(host, hostPage, (U64)-1, nullptr);
        }
        if (runEnd == host && runProt == prot && runEnd > runStart) {
            runEnd += hostPage;
            continue;
        }
        flush();
        runStart = host;
        runEnd = host + hostPage;
        runProt = prot;
    }
    flush();
}

namespace {

// Adapters so the planner can ask this address space its two questions
// without knowing anything about it.
struct NativeMapPlanContext {
    const KMemory64* memory;
};

bool nativeMapPlanCovered(void* context, U64 start, U64 end) {
    return static_cast<NativeMapPlanContext*>(context)->memory
        ->nativeRangeCoversForPlan(start, end);
}

bool nativeMapPlanFree(void* context, U64 start, U64 end) {
    (void)context;
    return k64NativeHostRangeFree(start, end - start);
}

// Bounded: a handful of lines naming the interval, what was reused and what
// was newly mapped. A guest heap grows constantly; this must not become a
// line per brk.
std::atomic<U32> gNativeExtendReports {0};
constexpr U32 kNativeExtendReportLimit = 16;

} // namespace

bool KMemory64::nativeRangeCoversForPlan(U64 start, U64 end) const {
    return nativeRangeCovers(start, end);
}

bool KMemory64::nativeMapAnonymous(U64 addr, U64 len, U32 prot, bool& fresh) {
    if (!nativeGuestRangeAllowed(addr, len)) {
        klog_fmt("KMemory64: native host MAP_FIXED refused outside guest "
                 "window addr=0x%llx len=0x%llx",
                 (unsigned long long)addr, (unsigned long long)len);
        return false;
    }
    const U64 hostPage = k64NativeHostPageSize();
    // Canonical guest addresses keep their values; only the host address the
    // bytes live at is translated. The alias is the identity for every
    // permitted high address, so this is a no-op for the ordinary lanes.
    const U64 hostAddr = k64GuestToHostAddress(addr);

    // A guest mapping is rounded out to the enclosing host pages, so a request
    // that starts mid-host-page shares that page with whatever the guest
    // already mapped there. glibc's heap does this on every 4 KiB brk growth.
    // Plan the interval page by page instead of treating it as all-or-nothing:
    // reuse what is already owned, map only what is genuinely free, and refuse
    // only when an untracked gap is occupied.
    NativeMapPlanContext planContext {this};
    const boxedvn::NativeMapPlan plan = boxedvn::planNativeAnonymousMap(
        hostAddr, len, hostPage, K64_PAGE_SIZE, &nativeMapPlanCovered,
        &nativeMapPlanFree, &planContext);
    if (!plan.ok()) {
        klog_fmt("BOXEDWINE_X64_NATIVE_EXTEND guest=[0x%llx,0x%llx) "
                 "host=[0x%llx,0x%llx) result=%s",
                 (unsigned long long)addr, (unsigned long long)(addr + len),
                 (unsigned long long)plan.hostStart,
                 (unsigned long long)plan.hostEnd,
                 plan.status == boxedvn::NativeMapPlanStatus::GapOccupied
                     ? "gap-occupied" : "invalid-request");
        return false;
    }

    const U64 hostStart = plan.hostStart;
    const U64 hostEnd = plan.hostEnd;
    const U64 hostLength = hostEnd - hostStart;

    // Reserve and map only the runs that are not already owned. Each is
    // recorded so a later failure can undo exactly what this call created and
    // leave every pre-existing tracked page untouched.
    struct CreatedRun { U64 start; U64 length; bool reserved; bool mapped; };
    std::vector<CreatedRun> created;
    auto rollback = [&created]() {
        for (auto it = created.rbegin(); it != created.rend(); ++it) {
            if (it->mapped) {
                ::munmap((void*)(uintptr_t)it->start, (size_t)it->length);
            }
            if (it->reserved) {
                k64NativeReleaseReservedHostRange(it->start, it->length);
            }
        }
    };

    for (const boxedvn::NativeHostRun& run : plan.runs) {
        if (run.reused) {
            continue;
        }
        // Do not issue Darwin MAP_FIXED against an untracked address without
        // holding it first: it could replace the executable arena, the app
        // image, or another runtime region. The Mach reservation is retained
        // until mmap consumes it, closing the preflight-to-map race.
        if (!k64NativeReserveHostRange(run.start, run.length)) {
            klog_fmt("KMemory64: native host reservation failed for "
                     "[0x%llx,0x%llx)", (unsigned long long)run.start,
                     (unsigned long long)(run.start + run.length));
            rollback();
            return false;
        }
        created.push_back(CreatedRun{run.start, run.length, true, false});
        const int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_BOXEDWINE;
        void* mapped = ::mmap((void*)(uintptr_t)run.start, (size_t)run.length,
                              PROT_READ | PROT_WRITE, flags, -1, 0);
        if (mapped == MAP_FAILED || (U64)(uintptr_t)mapped != run.start) {
            if (mapped != MAP_FAILED) ::munmap(mapped, (size_t)run.length);
            rollback();
            return false;
        }
        created.back().mapped = true;
        ::memset(mapped, 0, (size_t)run.length);
    }

    // MAP_FIXED|MAP_ANONYMOUS replaces the requested guest bytes with zeroes,
    // even when the requested protection is PROT_NONE. Make the whole interval
    // writable so that stays true across a reused host page whose other guest
    // subpages must survive.
    if (::mprotect((void*)(uintptr_t)hostStart, (size_t)hostLength,
                   PROT_READ | PROT_WRITE) != 0) {
        rollback();
        return false;
    }
    // Exactly the requested guest bytes, and nothing else. Zeroing the whole
    // enclosing host pages would destroy the neighbouring guest subpages this
    // request shares them with.
    ::memset((void*)(uintptr_t)hostAddr, 0, (size_t)len);

    // The interval stays readable and writable here, and the CALLER narrows it
    // once it has written the guest page map, host page by host page, through
    // nativeReconcileHostProt. Deciding the final protection here could only be
    // done from `plan.exactHostCover`, a property of the WHOLE interval: a
    // request whose length was not a multiple of the host page left every host
    // page in it read/write, so a PROT_NONE reservation was not PROT_NONE
    // anywhere, and a request that did cover it exactly applied the requested
    // protection to host pages whose other guest subpages were never named.
    // The union over each host page separately is the only value that is right
    // for all four of its guest pages at once.
    (void)prot;

    // One valid, non-overlapping entry across the completed interval, so
    // nativeRangeCovers succeeds over all of it. Forgetting first keeps any
    // tracked range that extends beyond this interval intact and prevents a
    // stale entry from surviving under the same key.
    nativeTrackRangeLocked(hostStart, hostLength, 0x3u);

    if (plan.mappedPages != 0 && plan.reusedPages != 0 &&
        gNativeExtendReports.fetch_add(1, std::memory_order_relaxed) <
            kNativeExtendReportLimit) {
        klog_fmt("BOXEDWINE_X64_NATIVE_EXTEND guest=[0x%llx,0x%llx) "
                 "host=[0x%llx,0x%llx) reused=%llu/%llu new=%llu/%llu "
                 "result=ok",
                 (unsigned long long)addr, (unsigned long long)(addr + len),
                 (unsigned long long)hostStart, (unsigned long long)hostEnd,
                 (unsigned long long)plan.reusedBytes,
                 (unsigned long long)plan.reusedPages,
                 (unsigned long long)plan.mappedBytes,
                 (unsigned long long)plan.mappedPages);
    }

    // Something new was mapped, so the caller need not zero the guest pages
    // again: the exact requested range was zeroed above.
    fresh = plan.mappedPages != 0;
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

// Reconcile the page map with the host VM for the host page a translated guest
// access faulted on. See the K64NativeFaultRepair comment in the header for why
// the two ledgers can disagree at all.
//
// The page map is the authority throughout: this raises the host page to
// exactly the union its guest subpages are entitled to and to nothing more, so
// a page the guest never mapped, or mapped PROT_NONE, still faults. It is
// deliberately the ONLY place that repairs, because every producing path
// (mmap/mprotect/munmap rounding a 4 KiB guest request out to a 16 KiB host
// page) would otherwise need its own inverse and a missed one is silent.
bool KMemory64::nativeRepairHostFault(U64 hostFaultAddress, U32 requiredProt,
                                      K64NativeFaultRepair& report) {
    report = K64NativeFaultRepair{};
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    if (!nativeIdentityMode()) {
        report.decision = "not-native";
        return false;
    }
    // Only a host address that is the image of a guest address THIS address
    // space serves may be repaired. The round trip is the whole test: it holds
    // for the low alias window, for the high identity lane (where it is the
    // identity) and for the top-arena alias, and fails for every host pointer
    // that is not a guest image -- the emulator's own heap, the translated code
    // arena, the separate KUSER alias mapping.
    const U64 guestAddress = k64HostToGuestAddress(hostFaultAddress);
    if (k64GuestToHostAddress(guestAddress) != hostFaultAddress) {
        report.decision = "outside-lane";
        return false;
    }
    const U64 guestPageAddress = guestAddress & ~K64_PAGE_MASK;
    if (!nativeGuestRangeAllowed(guestPageAddress, K64_PAGE_SIZE)) {
        report.decision = "outside-lane";
        return false;
    }
    report.inGuestLane = true;
    report.guestAddress = guestAddress;

    // Whether this page's bytes are supposed to live at the alias address at
    // all. A shared-file page that a sparse process demoted back to its
    // canonical heap buffer is mapped, but its contents are somewhere else, and
    // conjuring a zero page at the alias would hide that desync rather than
    // repair it.
    const U64 hostPage = k64NativeHostPageSize();
    if (hostPage == 0 || (hostPage % K64_PAGE_SIZE) != 0) {
        report.decision = "host-page-size";
        return false;
    }
    const U64 hostStart = k64NativeAlignDown(hostFaultAddress, hostPage);
    const U64 guestPageNumber = guestPageAddress >> K64_PAGE_SHIFT;

    // One host page carries four guest pages on iOS and they need not agree,
    // so the protection this page must carry is their union -- the same union
    // every producing path applies through nativeHostPageProtLocked, computed
    // from the same page map by the same code. Anything less would revoke a
    // neighbour; anything more would grant the guest access it does not have.
    //
    // It is computed BEFORE the refusal below so that a refused fault still
    // says what the rest of the host page holds: a guest page with no rights
    // beside three that have them is a granularity defect, and one whose whole
    // host page has none is a reservation the guest has genuinely not
    // committed. The witness could not tell those apart before.
    bool backedAtAlias = false;
    bool entitled = false;
    U32 unionProt = 0;
    {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
        auto it = pages.find(guestPageNumber);
        if (it == pages.end() || !(it->second->flags & K64_PAGE_MAPPED)) {
            // The guest never mapped this page. A real access violation, and
            // the guest has to see it as one.
            nativeCommittedNeighbourhoodLocked(guestPageNumber, report);
            report.decision = "guest-unmapped";
            return false;
        }
        report.pageMapped = true;
        report.guestProt = k64GuestProtFromPageFlags(it->second->flags);
        // A shared-file page is reachable through its canonical pointer by
        // sparse aliases, so it is kept host-writable wherever it is mapped.
        if (it->second->dataShared) report.guestProt |= K_PROT_READ | K_PROT_WRITE;
        // Which operation last wrote this page's rights, and what it wrote.
        report.lastWriter = k64PageWriterName(it->second->lastWriter);
        report.lastWriteFlags = it->second->lastFlags;
        report.lastWriteStamp = it->second->lastWriteStamp;
        // And the write before the last CHANGE, which is the only thing that
        // says whether these rights were ever anything else.
        report.priorWriter = k64PageWriterName(it->second->priorWriter);
        report.priorWriteFlags = it->second->priorFlags;
        report.priorWriteStamp = it->second->priorWriteStamp;
        backedAtAlias = it->second->dataNative &&
                        it->second->hostData() ==
                            (U8*)(uintptr_t)k64GuestToHostAddress(guestPageAddress);
        unionProt = nativeHostPageProtLocked(hostStart, hostPage,
                                             guestPageNumber, &report);
        // Decided under the same lock that read the numbers it is decided
        // from, so the refusal below and the neighbourhood scan that explains
        // it can never disagree about which case this is.
        entitled = report.guestProt != 0 &&
                   (report.guestProt & requiredProt) == requiredProt &&
                   (unionProt & requiredProt) == requiredProt;
        if (!entitled) {
            nativeCommittedNeighbourhoodLocked(guestPageNumber, report);
        }
    }
    report.hostPageStart = hostStart;
    report.hostPageLength = hostPage;
    report.hostPageProt = unionProt;
    if (!entitled) {
        // Mapped, but the guest itself revoked access. Also a real access
        // violation.
        report.decision = "guest-protected";
        return false;
    }

    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
    report.hostProtBefore = k64NativeHostProtOf(hostStart, report.hostPresent);
    report.tracked = nativeRangeCovers(hostStart, hostStart + hostPage);
    if (!report.hostPresent) {
        if (!backedAtAlias) {
            // Mapped, but its bytes do not live at the alias. A fresh zero page
            // would answer the read with the wrong contents, which is worse
            // than the fault.
            report.decision = "backing-elsewhere";
            return false;
        }
        // No host page at all behind an address the guest believes in. Claim it
        // the same way nativeMapAnonymous does -- reserve first, then map --
        // so a page this process does not own is never replaced. MAP_ANONYMOUS
        // already delivers zeroes, and the page is new, so nothing is erased.
        if (!k64NativeHostRangeFree(hostStart, hostPage) ||
            !k64NativeReserveHostRange(hostStart, hostPage)) {
            report.decision = "reserve-failed";
            return false;
        }
        const int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_BOXEDWINE;
        void* mapped = ::mmap((void*)(uintptr_t)hostStart, (size_t)hostPage,
                              PROT_READ | PROT_WRITE, flags, -1, 0);
        if (mapped == MAP_FAILED || (U64)(uintptr_t)mapped != hostStart) {
            if (mapped != MAP_FAILED) ::munmap(mapped, (size_t)hostPage);
            k64NativeReleaseReservedHostRange(hostStart, hostPage);
            report.decision = "map-failed";
            return false;
        }
        report.materialised = true;
    }
    if (::mprotect((void*)(uintptr_t)hostStart, (size_t)hostPage,
                   k64NativeProt(unionProt)) != 0) {
        report.decision = "mprotect-failed";
        return false;
    }
    report.reprotected = !report.materialised;
    // Keep nativeRanges honest about what is now mapped here, or the next
    // munmap would leave the host page behind. Exactly this host page, and
    // coalesced with a neighbour of the same protection so a region faulted in
    // page by page does not become one ledger entry per 16 KiB.
    nativeTrackRangeLocked(hostStart, hostPage, unionProt);
    k64DTlbInvalidateAll();
    report.decision = report.materialised ? "materialised" : "reprotected";
    return true;
#else
    (void)hostFaultAddress;
    (void)requiredProt;
    report.decision = "not-native";
    return false;
#endif
}

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
        const U16 stamp = k64NextPageWriteStamp();
        for (U64 i = 0; i < pageCount; i++) {
            K64Page* page = getOrAllocPage(pageStart + i, flags,
                                           K64_WRITER_MMAP_ANON);
            // Only the guest pages this request NAMES change rights, and the
            // enclosing host page is reconciled from the map afterwards.
            page->writeFlags(flags, K64_WRITER_MMAP_ANON, stamp);
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
            if (nativeIdentityMode()) {
                const U64 guestPageAddress = addr + (i << K64_PAGE_SHIFT);
                U8* nativeAddress = k64KuserAliasFor(guestPageAddress);
                if (prot && nativeAddress) page->adoptNative(nativeAddress);
                else if (prot) {
                    page->adoptNative(
                        (U8*)(uintptr_t)k64GuestToHostAddress(guestPageAddress));
                }
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
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    // The page map now says what every guest page in the request holds, so the
    // enclosing host pages can be brought to the union of their four guest
    // subpages -- which is how a PROT_NONE reservation that owns its host pages
    // becomes genuinely inaccessible without revoking a neighbour that shares
    // an edge host page with it.
    if (nativeIdentityMode() &&
        !k64IsKuserRange(addr, pageCount << K64_PAGE_SHIFT)) {
        const U64 hostPage = k64NativeHostPageSize();
        const U64 nativeLen = pageCount << K64_PAGE_SHIFT;
        if (hostPage && (hostPage % K64_PAGE_SIZE) == 0 &&
            addr <= UINT64_MAX - nativeLen &&
            addr + nativeLen <= UINT64_MAX - (hostPage - 1)) {
            BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
            nativeReconcileHostProt(
                k64NativeAlignDown(k64GuestToHostAddress(addr), hostPage),
                k64NativeAlignUp(k64GuestToHostAddress(addr) + nativeLen,
                                 hostPage));
        }
    }
#endif
    k64InvalidateProcessFetchCaches(process);
    queueTranslatedCodeInvalidation(addr, pageCount << K64_PAGE_SHIFT);
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
            const U8* fixed = (const U8*)(uintptr_t)k64GuestToHostAddress(
                (pageStart + i) << K64_PAGE_SHIFT);
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
        const U16 stamp = k64NextPageWriteStamp();
        for (U64 i = 0; i < pageCount; i++) {
            K64Page* page = getOrAllocPage(pageStart + i, flags,
                                           K64_WRITER_MMAP_FILE);
            page->writeFlags(flags, K64_WRITER_MMAP_FILE, stamp);
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
                    : (U8*)(uintptr_t)k64GuestToHostAddress(
                          (pageStart + i) << K64_PAGE_SHIFT);
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
                page->writeFlags(page->flags | K64_PAGE_PINNED,
                                 K64_WRITER_MMAP_FILE, stamp);
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
    queueTranslatedCodeInvalidation(addr, pageCount << K64_PAGE_SHIFT);
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    if (nativeIdentityMode()) {
        const U64 hostPage = k64NativeHostPageSize();
        if (!kuserRange && hostPage && (hostPage % K64_PAGE_SIZE) == 0 &&
            addr + len <= UINT64_MAX - (hostPage - 1)) {
            // Native shared pages can be reached through the canonical pointer
            // by sparse aliases, so the union keeps them host-writable whatever
            // the guest asked for -- nativeHostPageProtLocked ORs read/write for
            // any dataShared subpage. Reconciling host page by host page is what
            // stops that from being applied to a neighbouring host page that
            // holds no shared subpage at all; the loop this replaces assigned
            // `prot | 0x3` to every tracked range the interval merely touched.
            BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
            nativeReconcileHostProt(
                k64NativeAlignDown(k64GuestToHostAddress(addr), hostPage),
                k64NativeAlignUp(k64GuestToHostAddress(addr) + len, hostPage));
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

bool KMemory64::rangeCompletelyUnmapped(U64 addr, U64 len) const {
    if (len == 0) return true;
    if (addr > (U64)-1 - len) return false;
    const U64 pageStart = addr >> K64_PAGE_SHIFT;
    const U64 pageCount = (len + K64_PAGE_SIZE - 1) >> K64_PAGE_SHIFT;
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
    for (U64 i = 0; i < pageCount; i++) {
        auto it = pages.find(pageStart + i);
        if (it != pages.end() && (it->second->flags & K64_PAGE_MAPPED)) {
            return false;
        }
    }
    return true;
}

// Wine reserves inaccessible address space it never touches, at high
// addresses the native identity window cannot host. Refusing those made it
// halve the request and retry without end: 8,916,993 mmaps in one device run,
// 8,916,842 rejected, about 98% of a core. Such a range has nothing to read,
// nothing to execute and no pointer FEX could be handed, so it is recorded as
// one interval and nothing is mapped.
U64 KMemory64::reserveSparseNoReplace(U64 addr, U64 len) {
    if (len == 0) return (U64)-K_EINVAL;
    if (addr == 0 || (addr & K64_PAGE_MASK)) return (U64)-K_EINVAL;
    if (len > (U64)-1 - K64_PAGE_MASK || addr > (U64)-1 - len) {
        return (U64)-K_EINVAL;
    }
    const U64 pageStart = addr >> K64_PAGE_SHIFT;
    const U64 pageCount = (len + K64_PAGE_SIZE - 1) >> K64_PAGE_SHIFT;
    const U64 pageEnd = pageStart + pageCount;

    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
    // MAP_FIXED_NOREPLACE never replaces, so an overlap of any kind is EEXIST.
    auto it = sparseReservations.upper_bound(pageStart);
    if (it != sparseReservations.begin()) {
        auto previous = std::prev(it);
        if (previous->first + previous->second > pageStart) {
            return (U64)-K_EEXIST;
        }
    }
    if (it != sparseReservations.end() && it->first < pageEnd) {
        return (U64)-K_EEXIST;
    }
    // Real pages win over a reservation: something already lives there.
    for (U64 i = 0; i < pageCount; i++) {
        if (isPageMapped(pageStart + i)) {
            return (U64)-K_EEXIST;
        }
    }
    // Coalesce with an immediate neighbour so a walk that reserves adjacent
    // arenas does not accumulate one interval per call.
    U64 mergedStart = pageStart;
    U64 mergedEnd = pageEnd;
    auto before = sparseReservations.find(mergedStart);
    if (before == sparseReservations.end()) {
        auto candidate = sparseReservations.lower_bound(mergedStart);
        if (candidate != sparseReservations.begin()) {
            auto previous = std::prev(candidate);
            if (previous->first + previous->second == mergedStart) {
                mergedStart = previous->first;
                sparseReservations.erase(previous);
            }
        }
    }
    auto after = sparseReservations.find(mergedEnd);
    if (after != sparseReservations.end()) {
        mergedEnd = after->first + after->second;
        sparseReservations.erase(after);
    }
    sparseReservations[mergedStart] = mergedEnd - mergedStart;
    return addr;
}

bool KMemory64::sparseReservationOverlaps(U64 addr, U64 len) const {
    if (len == 0) return false;
    if (addr > (U64)-1 - len) return false;
    const U64 pageStart = addr >> K64_PAGE_SHIFT;
    const U64 pageCount = (len + K64_PAGE_SIZE - 1) >> K64_PAGE_SHIFT;
    const U64 pageEnd = pageStart + pageCount;
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
    auto it = sparseReservations.upper_bound(pageStart);
    if (it != sparseReservations.begin()) {
        auto previous = std::prev(it);
        if (previous->first + previous->second > pageStart) return true;
    }
    return it != sparseReservations.end() && it->first < pageEnd;
}

bool KMemory64::releaseSparseReservation(U64 addr, U64 len) {
    if (len == 0) return false;
    if (addr & K64_PAGE_MASK) return false;
    if (addr > (U64)-1 - len) return false;
    const U64 pageStart = addr >> K64_PAGE_SHIFT;
    const U64 pageCount = (len + K64_PAGE_SIZE - 1) >> K64_PAGE_SHIFT;
    const U64 pageEnd = pageStart + pageCount;

    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
    bool removed = false;
    auto it = sparseReservations.lower_bound(pageStart);
    if (it != sparseReservations.begin()) {
        // The interval starting before the request may still reach into it.
        --it;
    }
    while (it != sparseReservations.end() && it->first < pageEnd) {
        const U64 start = it->first;
        const U64 end = start + it->second;
        if (end <= pageStart) {
            ++it;
            continue;
        }
        removed = true;
        it = sparseReservations.erase(it);
        // Whatever of this interval lies outside the request survives, so a
        // munmap through the middle of a reservation splits it in two.
        if (start < pageStart) {
            sparseReservations[start] = pageStart - start;
        }
        if (end > pageEnd) {
            sparseReservations[pageEnd] = end - pageEnd;
            break;
        }
    }
    return removed;
}

U64 KMemory64::sparseReservationCount() const {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
    return (U64)sparseReservations.size();
}

U64 KMemory64::sparseReservationPages() const {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
    U64 total = 0;
    for (const auto& entry : sparseReservations) total += entry.second;
    return total;
}

U64 KMemory64::mmapAnonymousNoReplace(U64 addr, U64 len, U32 prot) {
    if (len == 0) return (U64)-K_EINVAL;
    if (addr == 0 || (addr & K64_PAGE_MASK)) return (U64)-K_EINVAL;
    if (len > (U64)-1 - K64_PAGE_MASK || addr > (U64)-1 - len) {
        return (U64)-K_EINVAL;
    }

    const U64 pageCount = (len + K64_PAGE_SIZE - 1) >> K64_PAGE_SHIFT;
    const U64 mapLen = pageCount << K64_PAGE_SHIFT;

    // Hold the allocator lock across the occupancy test AND the mapping. This
    // is the whole point of the API: an unlocked "check the pages, then
    // MAP_FIXED" sequence lets a sibling thread map the range in between, and
    // the loser then destroys the winner's mapping -- exactly the failure
    // mmapReserveAndMap already exists to avoid. mmapMutex is recursive, so
    // mmapAnonymousFixed may re-enter it below, and this is the same
    // mmapMutex-then-pagesMutex order mmapReserveAndMap already establishes.
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);

    // Occupancy first, so a range that is BOTH occupied and unhostable is
    // reported as occupied -- that is the stronger, more specific fact.
    if (!rangeCompletelyUnmapped(addr, mapLen) ||
        sparseReservationOverlaps(addr, mapLen)) {
        return (U64)-K_EEXIST;
    }

    if (nativeIdentityMode() && !nativeGuestRangeAllowed(addr, mapLen)) {
        // The range is empty; it is the ADDRESS this address space cannot
        // provide, and MAP_FIXED_NOREPLACE forbids relocating. EEXIST would
        // claim an occupant that does not exist, and a caller searching for a
        // free address answers EEXIST by stepping one granule and asking
        // again -- across a region where every address is equally unhostable
        // that is an unbounded walk. -ENOMEM is the true answer and ends the
        // search at the first probe.
        return (U64)-K_ENOMEM;
    }

    const U64 mapped = mmapAnonymousFixed(addr, mapLen, prot);
    if (mapped != addr) {
        // Never fall through to a destructive placement. Surface the address
        // space's own error when it produced one, otherwise report the range
        // as taken.
        return ((S64)mapped < 0) ? mapped : (U64)-K_EEXIST;
    }
    return addr;
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
    // Computed before the page map is written, applied after it. The host
    // protection is derived from the map, never from this request's own
    // protection: a value that is right for the four guest pages of one host
    // page is wrong for the four of the next.
    bool reconcileHost = false;
    U64 reconcileStart = 0;
    U64 reconcileEnd = 0;
    if (nativeIdentityMode()) {
        if (!nativeGuestRangeAllowed(addr, pageCount << K64_PAGE_SHIFT)) {
            klog_fmt("KMemory64: reject native mprotect outside guest window "
                     "addr=0x%llx len=0x%llx", (unsigned long long)addr,
                     (unsigned long long)len);
            return (U64)-K_EINVAL;
        }
        const bool kuserRange = k64IsKuserRange(addr, pageCount << K64_PAGE_SHIFT);
        const U64 hostPage = k64NativeHostPageSize();
        if (hostPage == 0 || (hostPage % K64_PAGE_SIZE) != 0) {
            return (U64)-K_EINVAL;
        }
        if (addr + len > UINT64_MAX - (hostPage - 1)) return (U64)-K_EINVAL;
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
            }
        }
        // The host interval is the enclosing one, but it is no longer a gate.
        // Asking nativeRangeCovers about the WHOLE of it and returning -EINVAL
        // when any edge host page was untracked is what made a committed page
        // keep the reservation's flags: the request failed after the guest
        // range was validated and before a single guest page was written, and
        // said nothing about it. Coverage is now a per-host-page question that
        // nativeReconcileHostProt answers by skipping, and an untracked host
        // page is materialised on its next fault from the same union.
        reconcileHost = !kuserRange;
        reconcileStart = k64NativeAlignDown(k64GuestToHostAddress(addr), hostPage);
        reconcileEnd =
            k64NativeAlignUp(k64GuestToHostAddress(addr) + len, hostPage);
    }
#endif

    U32 newFlags = K64_PAGE_MAPPED;
    if (prot & 0x1) newFlags |= K64_PAGE_READ;
    if (prot & 0x2) newFlags |= K64_PAGE_WRITE | K64_PAGE_READ; // write implies read
    if (prot & 0x4) newFlags |= K64_PAGE_EXEC;

    {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
        const U16 stamp = k64NextPageWriteStamp();
        for (U64 i = 0; i < pageCount; i++) {
            // Preserve SHARED bit if it was set; everything else is replaced
            // wholesale. Holes (no page) are skipped — see header comment.
            // Only the pages [pageStart, pageStart+pageCount) are touched: a
            // guest page's rights change only in an operation that names it.
            auto it = pages.find(pageStart + i);
            if (it == pages.end()) continue;
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
            if (nativeIdentityMode()) {
                if (prot && !it->second->dataShared) {
                    it->second->adoptNative((U8*)(uintptr_t)k64GuestToHostAddress(
                    addr + (i << K64_PAGE_SHIFT)));
                } else if (!prot && it->second->dataNative && !it->second->dataShared) {
                    it->second->decommit();
                }
            }
#endif
            U32 preserved = it->second->flags & (K64_PAGE_SHARED | K64_PAGE_PINNED);
            it->second->writeFlags(newFlags | preserved, K64_WRITER_MPROTECT,
                                   stamp);
        }
    }
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
    if (reconcileHost) {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
        nativeReconcileHostProt(reconcileStart, reconcileEnd);
    }
#endif
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

    // Sparse reservations sit outside the native window by construction, so
    // they have to be released before the window guard below refuses their
    // address. They have no pages and no host mapping: removing the interval
    // IS the unmap, and the address becomes reservable again.
    if (releaseSparseReservation(addr, pageCount << K64_PAGE_SHIFT)) {
        if (rangeCompletelyUnmapped(addr, pageCount << K64_PAGE_SHIFT)) {
            return 0;
        }
    }

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
        // Host addresses, so ::munmap and the nativeRanges keys agree. The
        // guest page map is indexed separately, in canonical space.
        const U64 hostStart =
            k64NativeAlignDown(k64GuestToHostAddress(addr), hostPage);
        const U64 hostEnd =
            k64NativeAlignUp(k64GuestToHostAddress(addr) + len, hostPage);
        {
            BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
            const U16 stamp = k64NextPageWriteStamp();
            for (U64 i = 0; i < pageCount; i++) {
                auto it = pages.find(pageStart + i);
                if (it != pages.end()) {
                    const bool pinned = (it->second->flags & K64_PAGE_PINNED) != 0;
                    if (!pinned) {
                        it->second->demoteNativeShared();
                        it->second->decommit();
                    }
                    it->second->writeFlags(pinned ? K64_PAGE_PINNED : 0,
                                           K64_WRITER_MUNMAP, stamp);
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
                // Which guest pages share this host page. Naming them by host
                // address would look up an unrelated entry for anything served
                // through an alias, so the host page is translated back first.
                const U64 guestOfHostPage = k64HostToGuestAddress(host);
                {
                    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
                    for (U64 sub = 0; sub < hostPage; sub += K64_PAGE_SIZE) {
                        auto it = pages.find((guestOfHostPage + sub) >> K64_PAGE_SHIFT);
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
                } else if (keepHostPage) {
                    // The host page survives because a neighbouring guest
                    // subpage still owns it, but the pages just unmapped no
                    // longer grant anything. Narrowing it to the new union is
                    // the other direction of the same rule: a host page carries
                    // exactly what its guest subpages hold, not what they held.
                    nativeReconcileHostProt(host, host + hostPage);
                }
            }
        }
        k64DTlbInvalidateAll();
        k64InvalidateProcessFetchCaches(process);
        queueTranslatedCodeInvalidation(addr, pageCount << K64_PAGE_SHIFT);
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
        const U32 gen = k64PageCacheGeneration();
        U8* data = commitPageLocked(pageNum, K64_PAGE_MAPPED | K64_PAGE_READ | K64_PAGE_WRITE);
        if (data) {
            k64DTlbInsert(this, pageNum, data, gen);
            ::memcpy(data + offsetInPage, s, (size_t)chunk); // commit-on-write (MT-safe)
        } else {
            k64ReportMissingBacking((int)(process ? process->id : -1), pageNum, "write");
        }
        dstGuest += chunk;
        s += chunk;
        len -= chunk;
    }
}

namespace {
void k64ReportMissingBacking(int pid, U64 pageNum, const char* op) {
    static std::atomic<U64> reports {0};
    const U64 n = reports.fetch_add(1, std::memory_order_relaxed);
    if (n < 8 || (n & 0xfff) == 0) {
        klog_fmt("BOXEDWINE_X64_SPARSE_PAGE_MISSING pid=%d op=%s page=0x%llx count=%llu",
                 pid, op,
                 (unsigned long long)pageNum, (unsigned long long)(n + 1));
    }
}
} // namespace

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
        const U32 gen = k64PageCacheGeneration();
        K64Page* page = getPage(pageNum);
        // Read the backing pointer once: committed() is the same load, and
        // a shared page's buffer can be withdrawn between the two.
        U8* host = page ? page->hostData() : nullptr;
        if (host) {
            k64DTlbInsert(this, pageNum, host, gen);
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
        if (data) {
            ::memset(data + offsetInPage, value, (size_t)chunk); // commit-on-write (MT-safe)
        } else {
            k64ReportMissingBacking((int)(process ? process->id : -1), pageNum, "memset");
        }
        dstGuest += chunk;
        len -= chunk;
    }
}

U8 KMemory64::readb(U64 addr) {
    U64 pageNum = addr >> K64_PAGE_SHIFT;
    if (U8* hit = k64DTlbLookup(this, pageNum)) return hit[addr & K64_PAGE_MASK];
    const U32 gen = k64PageCacheGeneration();
    K64Page* p = getPage(pageNum);
    U8* host = p ? p->hostData() : nullptr;
    if (host) {
        k64DTlbInsert(this, pageNum, host, gen);
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
    const U32 gen = k64PageCacheGeneration();
    U8* data = commitPageLocked(pageNum, K64_PAGE_MAPPED | K64_PAGE_READ | K64_PAGE_WRITE);
    if (!data) {
        // A page whose commit failed or was withdrawn (a shared slot the owner
        // nulled) has nowhere to put this byte. Drop and report it, the way the
        // bulk write paths do, rather than storing through NULL.
        k64ReportMissingBacking((int)(process ? process->id : -1), pageNum, "writeb");
        return;
    }
    k64DTlbInsert(this, pageNum, data, gen);
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
                                       K64_PAGE_MAPPED | K64_PAGE_READ | K64_PAGE_WRITE,
                                       K64_WRITER_PIN);
#if defined(BOXEDWINE_KMEMORY64_NATIVE_IDENTITY) && (defined(__APPLE__) || defined(__unix__))
        if (nativeIdentityMode() && (!page->hostData() || !page->dataNative)) {
            kpanic("KMemory64 native identity getRamPtr on an unmapped page");
            return nullptr;
        }
#endif
        page->writeFlags(page->flags | K64_PAGE_PINNED, K64_WRITER_PIN,
                         k64NextPageWriteStamp());
        data = page->commit();
    }
    return data + offsetInPage;
}

void KMemory64::writew(U64 addr, U16 value) { memcpyToGuest(addr, &value, 2); }
void KMemory64::writed(U64 addr, U32 value) { memcpyToGuest(addr, &value, 4); }
void KMemory64::writeq(U64 addr, U64 value) { memcpyToGuest(addr, &value, 8); }

#endif // BOXEDWINE_GUEST_X64
