/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 *
 *  See BVNExecMemory.h for the iOS 26/27 StikDebug and TXM contract.
 */

#include "BVNExecMemory.h"
#include "BVNJitArenaPlan.h"
#include "BVNRangeAllocator.h"

#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mutex>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "BVNRuntime.h"

namespace {

struct DualMapping {
    uint8_t* executable = nullptr;
    uint8_t* writable = nullptr;
    size_t length = 0;
};

// Preparing executable memory requires stopping at StikDebug's universal
// breakpoint. Doing that for every 64 KiB Boxedwine code block makes Wine
// startup spend nearly all its time stopped in debugserver (and fails once
// StikDebug is background-suspended). Prepare the whole arena during the
// explicit startup probe, then service live JIT allocations without another
// external handshake.
//
// How large that arena is, and why it is a list of segments rather than one
// mapping, is decided by BVNPlanJitArena - see BVNJitArenaPlan.h. A single
// fixed 128 MiB was enough for a Direct3D 9 visual novel plus WineDbg, but
// not for a guest that runs a browser engine: a Chromium/NW.js title starts
// six to ten guest processes, each translating its own copy of the engine,
// and ran the arena dry mid-launch after roughly 2,180 blocks. Every later
// allocation then failed and the guest wedged with a message about the JIT
// enabler that had, in fact, worked perfectly.
struct ArenaSegment {
    DualMapping mapping;
    BVNRangeAllocator allocator;
};

std::mutex gArenaMutex;
std::vector<ArenaSegment> gSegments;
size_t gAllocationCount = 0;
// Segments have been obtained from StikDebug. Set only on success, so a
// refusal never stops a later attempt from asking again.
bool gPrepared = false;
// The execution test has run. Separate from gPrepared because the two happen
// at different moments for different reasons: preparation must occur while
// StikDebug is still attached, and executing generated code must occur only
// when the user has asked for something that needs it.
bool gProbed = false;
bool gExecutionConfirmed = false;
char gReport[1600] = "not probed yet";

size_t arenaCapacity() {
    size_t total = 0;
    for (const ArenaSegment& segment : gSegments) {
        total += segment.allocator.capacity();
    }
    return total;
}

size_t arenaAvailable() {
    size_t total = 0;
    for (const ArenaSegment& segment : gSegments) {
        total += segment.allocator.available();
    }
    return total;
}

size_t pageSize() { return static_cast<size_t>(getpagesize()); }

void reportLine(const char* line) {
    BVNLogWrite(BVNLogLevelInfo, "jit", line);
    const size_t used = strlen(gReport);
    if (used >= sizeof(gReport) - 1) {
        return;
    }
    snprintf(gReport + used, sizeof(gReport) - used, "%s%s",
             used > 0 ? "\n" : "", line);
}

bool queryProtection(void* address, vm_prot_t* current, vm_prot_t* maximum) {
    vm_address_t regionAddress = reinterpret_cast<vm_address_t>(address);
    vm_size_t regionSize = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t infoCount = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t objectName = MACH_PORT_NULL;

    const kern_return_t result = vm_region_64(
        mach_task_self(), &regionAddress, &regionSize,
        VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info), &infoCount, &objectName);
    if (result != KERN_SUCCESS ||
        regionAddress > reinterpret_cast<vm_address_t>(address)) {
        return false;
    }
    *current = info.protection;
    *maximum = info.max_protection;
    return true;
}

void protectionText(void* address, char out[9]) {
    vm_prot_t current = 0;
    vm_prot_t maximum = 0;
    if (!queryProtection(address, &current, &maximum)) {
        snprintf(out, 9, "?/?");
        return;
    }
    snprintf(out, 9, "%c%c%c/%c%c%c",
             (current & VM_PROT_READ) ? 'r' : '-',
             (current & VM_PROT_WRITE) ? 'w' : '-',
             (current & VM_PROT_EXECUTE) ? 'x' : '-',
             (maximum & VM_PROT_READ) ? 'r' : '-',
             (maximum & VM_PROT_WRITE) ? 'w' : '-',
             (maximum & VM_PROT_EXECUTE) ? 'x' : '-');
}

#if defined(__aarch64__)
bool hasProtection(void* address, vm_prot_t required,
                   vm_prot_t forbidden) {
    vm_prot_t current = 0;
    vm_prot_t maximum = 0;
    return queryProtection(address, &current, &maximum) &&
           (current & required) == required &&
           (current & forbidden) == 0;
}

// StikDebug's universal JIT script treats brk #0xf00d as a syscall and x16=1
// as "prepare this executable region". x0/x1 are already the first two C ABI
// arguments, and the script returns the prepared address in x0.
__attribute__((noinline, optnone, naked))
void* stikDebugPrepareRegion(void* address, size_t length) {
    __asm__ volatile(
        "mov x16, #1\n"
        "brk #0xf00d\n"
        "ret\n");
}

// A reinstall or re-sign creates a new StikDebug target. CS_DEBUGGED can
// still be present even when universal.js is no longer assigned to that new
// target. In that state the deliberate brk above becomes an ordinary SIGTRAP
// and iOS terminates the whole app before the six-second timeout can help.
// Recover only while this thread is executing the one expected handshake;
// an active universal.js consumes the Mach exception before POSIX signal
// delivery, prepares the region and resumes normally.
thread_local sigjmp_buf* gStikDebugTrapRecovery = nullptr;

void stikDebugTrapHandler(int signalNumber) {
    if (signalNumber == SIGTRAP && gStikDebugTrapRecovery != nullptr) {
        siglongjmp(*gStikDebugTrapRecovery, 1);
    }

    // This handler is installed for only the synchronous handshake call. An
    // unrelated trap must retain the platform's normal fatal behavior.
    signal(signalNumber, SIG_DFL);
    raise(signalNumber);
}

bool safelyPrepareWithStikDebug(void* address, size_t length,
                               void** prepared, char* error,
                               size_t errorSize) {
    struct sigaction action{};
    struct sigaction previous{};
    action.sa_handler = stikDebugTrapHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGTRAP, &action, &previous) != 0) {
        snprintf(error, errorSize,
                 "Could not install the guarded StikDebug handshake: %s",
                 strerror(errno));
        return false;
    }

    sigjmp_buf recovery;
    gStikDebugTrapRecovery = &recovery;
    const int trapped = sigsetjmp(recovery, 1);
    if (trapped == 0) {
        *prepared = stikDebugPrepareRegion(address, length);
    }
    gStikDebugTrapRecovery = nullptr;

    const int restoreResult = sigaction(SIGTRAP, &previous, nullptr);
    if (trapped != 0) {
        snprintf(error, errorSize,
                 "StikDebug did not handle BoxedVN's universal JIT "
                 "breakpoint. After reinstalling or re-signing BoxedVN, "
                 "assign universal.js to the newly installed app, launch "
                 "through StikDebug, and keep its script session running.");
        return false;
    }
    if (restoreResult != 0) {
        snprintf(error, errorSize,
                 "Could not restore the SIGTRAP handler after the StikDebug "
                 "handshake: %s", strerror(errno));
        return false;
    }
    return true;
}
#endif

DualMapping allocatePrepared(size_t length, char* error, size_t errorSize) {
    DualMapping mapping;

#if !defined(__aarch64__)
    snprintf(error, errorSize,
             "StikDebug executable-memory preparation requires ARM64");
    return mapping;
#else
    if (length == 0 || length % pageSize() != 0) {
        snprintf(error, errorSize,
                 "length %zu is not aligned to the %zu-byte iOS page size",
                 length, pageSize());
        return mapping;
    }

    void* rx = mmap(nullptr, length, PROT_READ | PROT_EXEC,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (rx == MAP_FAILED) {
        snprintf(error, errorSize, "mmap(r-x) failed: %s", strerror(errno));
        return mapping;
    }

    // On iOS 26/27 this is the operation that makes the page executable.
    // CS_DEBUGGED alone is insufficient. StikDebug must be running its
    // universal script and will write each 16 KiB page through debugserver.
    void* prepared = nullptr;
    if (!safelyPrepareWithStikDebug(rx, length, &prepared, error,
                                   errorSize)) {
        munmap(rx, length);
        return mapping;
    }
    if (prepared != rx) {
        snprintf(error, errorSize,
                 "StikDebug did not prepare the requested region (returned "
                 "%p for %p). Assign and run StikDebug's universal JIT "
                 "script for BoxedVN.", prepared, rx);
        munmap(rx, length);
        return mapping;
    }

    vm_address_t rwAddress = 0;
    vm_prot_t current = 0;
    vm_prot_t maximum = 0;
    const kern_return_t remapResult = vm_remap(
        mach_task_self(), &rwAddress, length, 0, VM_FLAGS_ANYWHERE,
        mach_task_self(), reinterpret_cast<vm_address_t>(rx), FALSE,
        &current, &maximum, VM_INHERIT_DEFAULT);
    if (remapResult != KERN_SUCCESS) {
        snprintf(error, errorSize, "vm_remap failed: kern_return_t %d",
                 remapResult);
        munmap(rx, length);
        return mapping;
    }

    void* rw = reinterpret_cast<void*>(rwAddress);
    if (mprotect(rw, length, PROT_READ | PROT_WRITE) != 0) {
        snprintf(error, errorSize, "mprotect(rw alias) failed: %s",
                 strerror(errno));
        vm_deallocate(mach_task_self(), rwAddress, length);
        munmap(rx, length);
        return mapping;
    }

    if (!hasProtection(rx, VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_WRITE) ||
        !hasProtection(rw, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_EXECUTE)) {
        char rxProtection[9];
        char rwProtection[9];
        protectionText(rx, rxProtection);
        protectionText(rw, rwProtection);
        snprintf(error, errorSize,
                 "dual mapping has unsafe protections (rx %s, rw %s)",
                 rxProtection, rwProtection);
        vm_deallocate(mach_task_self(), rwAddress, length);
        munmap(rx, length);
        return mapping;
    }

    mapping.executable = static_cast<uint8_t*>(rx);
    mapping.writable = static_cast<uint8_t*>(rw);
    mapping.length = length;
    return mapping;
#endif
}

void releaseMapping(const DualMapping& mapping) {
    if (mapping.writable) {
        vm_deallocate(
            mach_task_self(),
            reinterpret_cast<vm_address_t>(mapping.writable),
            mapping.length);
    }
    if (mapping.executable) {
        munmap(mapping.executable, mapping.length);
    }
}

bool containsRange(const DualMapping& mapping, void* address, size_t length) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t base = reinterpret_cast<uintptr_t>(mapping.executable);
    if (start < base || length > mapping.length) {
        return false;
    }
    const uintptr_t offset = start - base;
    return offset <= mapping.length - length;
}

// The segment whose executable mapping `address` starts inside, or nullptr.
// Segments are separate mappings at unrelated addresses, so this is a scan
// over a handful of entries rather than arithmetic on one base.
ArenaSegment* segmentStartingAt(void* address) {
    if (!address) {
        return nullptr;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    for (ArenaSegment& segment : gSegments) {
        if (!segment.mapping.executable) {
            continue;
        }
        const uintptr_t base =
            reinterpret_cast<uintptr_t>(segment.mapping.executable);
        if (start >= base && start - base < segment.mapping.length) {
            return &segment;
        }
    }
    return nullptr;
}

constexpr uint32_t kProbeExpectedValue = 0x4258;

constexpr uint32_t encodeMovzX(uint8_t destination, uint16_t immediate) {
    return 0xD2800000u | (static_cast<uint32_t>(immediate) << 5) |
           destination;
}

constexpr uint32_t kProbeCode[] = {
    encodeMovzX(8, kProbeExpectedValue),  // movz x8, #0x4258
    0xAA0803E0,  // mov x0, x8
    0xD65F03C0,  // ret
};

static_assert(kProbeCode[0] == 0xD2884B08,
              "ARM64 probe MOVZ encoding must return 0x4258");

}  // namespace

extern "C" const char* BVNExecMemStrategyName(BVNExecMemStrategy strategy) {
    switch (strategy) {
        case BVNExecMemStrategyStikDebugDualMap:
            return "StikDebug universal + dual-map RX/RW";
        case BVNExecMemStrategyMmapRWX: return "mmap(rwx) [retired unsafe]";
        case BVNExecMemStrategyMprotectRWX:
            return "mmap+mprotect(rwx) [retired unsafe]";
        case BVNExecMemStrategyMapJitRWX:
            return "mmap(MAP_JIT,rwx) [retired unsupported]";
        case BVNExecMemStrategyMapJitFlip:
            return "mmap(MAP_JIT)+mprotect(r-x) [retired unsupported]";
        case BVNExecMemStrategyMprotectFlip:
            return "mmap+mprotect(r-x) [retired unsafe]";
        case BVNExecMemStrategyMachFlip:
            return "vm_allocate+vm_protect(r-x) [retired unsafe]";
        case BVNExecMemStrategyNone: return "none";
    }
    return "unknown";
}

extern "C" const char* BVNExecMemReport(void) { return gReport; }

extern "C" bool BVNExecMemExecutionConfirmed(void) {
    return gExecutionConfirmed;
}

extern "C" bool BVNExecMemNeedsWriteFlip(void) { return false; }

extern "C" bool BVNExecMemArenaPrepared(void) { return gPrepared; }

extern "C" bool BVNExecMemPrepareArena(void) {
    if (gPrepared) {
        return true;
    }

    gReport[0] = '\0';
    char error[512] = {};
    char line[256];

    const BVNMemoryReport memory = BVNMemoryProbe();
    const BVNJitArenaPlan plan = BVNPlanJitArena(
        memory.availableBytes, memory.physicalMemoryBytes, pageSize());
    constexpr size_t kMiB = 1024u * 1024u;

    snprintf(line, sizeof(line),
             "Requesting %zu MiB of executable memory from StikDebug as %zu "
             "segment(s) of %zu MiB; live Wine JIT allocations will be "
             "suballocated without more debugger breakpoints.",
             plan.totalBytes() / kMiB, plan.segmentCount,
             plan.segmentBytes / kMiB);
    reportLine(line);

    DualMapping mapping =
        allocatePrepared(plan.segmentBytes, error, sizeof(error));
    if (!mapping.executable) {
        reportLine(error);
        // Deliberately NOT sticky. A user who opens BoxedVN before attaching
        // StikDebug must be able to attach it and launch, so a refusal here
        // has to leave the next caller free to ask again. Only success is
        // remembered.
        return false;
    }

    char rxProtection[9];
    char rwProtection[9];
    protectionText(mapping.executable, rxProtection);
    protectionText(mapping.writable, rwProtection);
    snprintf(line, sizeof(line),
             "StikDebug prepared the first %zu MiB segment; dual mapping is "
             "rx %s, rw %s.",
             plan.segmentBytes / kMiB, rxProtection, rwProtection);
    reportLine(line);

    // The remaining segments are prepared here, inside the one deliberate
    // handshake, because there is no later moment when asking is safe. A
    // refusal part way through is not fatal: the segments already prepared
    // are a working arena, and saying so beats losing the JIT over capacity
    // the guest may never have needed.
    {
        std::lock_guard<std::mutex> lock(gArenaMutex);
        gSegments.clear();
        gSegments.reserve(plan.segmentCount);
        gSegments.push_back(ArenaSegment{});
        gSegments.back().mapping = mapping;
        gSegments.back().allocator.reset(mapping.length);

        for (size_t i = 1; i < plan.segmentCount; ++i) {
            char segmentError[512] = {};
            DualMapping extra =
                allocatePrepared(plan.segmentBytes, segmentError,
                                 sizeof(segmentError));
            if (!extra.executable) {
                snprintf(line, sizeof(line),
                         "Segment %zu of %zu could not be prepared (%s); "
                         "continuing with the %zu MiB already prepared.",
                         i + 1, plan.segmentCount, segmentError,
                         arenaCapacity() / kMiB);
                reportLine(line);
                break;
            }
            gSegments.push_back(ArenaSegment{});
            gSegments.back().mapping = extra;
            gSegments.back().allocator.reset(extra.length);
        }
        gAllocationCount = 0;

        snprintf(line, sizeof(line),
                 "%zu MiB in %zu segment(s) is retained for Wine and no "
                 "further StikDebug stops are required. Execution through it "
                 "is confirmed separately, when a guest is about to start.",
                 arenaCapacity() / kMiB, gSegments.size());
    }
    gPrepared = true;
    reportLine(line);
    return true;
}

extern "C" BVNExecMemStrategy BVNExecMemProbe(bool allowExecute) {
    if (gProbed) {
        return gExecutionConfirmed ? BVNExecMemStrategyStikDebugDualMap
                                   : BVNExecMemStrategyNone;
    }
    if (!allowExecute) {
        return BVNExecMemStrategyNone;
    }

    // Ordinarily the arena is already here: BVNGuestMain prepares it during
    // app startup, while StikDebug is still attached. This call covers the
    // case where that did not happen or was refused - StikDebug attached
    // after the app opened, most obviously - and keeps this function's old
    // behaviour for any caller that never pre-prepared.
    if (!gPrepared && !BVNExecMemPrepareArena()) {
        gProbed = true;
        return BVNExecMemStrategyNone;
    }

    constexpr size_t kMiB = 1024u * 1024u;
    char line[256];
    DualMapping first;
    {
        std::lock_guard<std::mutex> lock(gArenaMutex);
        if (gSegments.empty() || !gSegments.front().mapping.executable) {
            gProbed = true;
            reportLine("The arena reports itself prepared but holds no "
                       "segment, so there is nothing to execute from.");
            return BVNExecMemStrategyNone;
        }
        first = gSegments.front().mapping;
    }

    // Write at offset 0 of the first segment and execute there. Nothing has
    // been handed out yet - BVNExecMemAlloc refuses until gExecutionConfirmed
    // - so this cannot land on live guest code, and the allocators are reset
    // below so the probe's three instructions are simply overwritten by the
    // first real translation.
    memcpy(first.writable, kProbeCode, sizeof(kProbeCode));
    sys_icache_invalidate(first.executable, sizeof(kProbeCode));
    reportLine("About to execute through the r-x mapping after writing only "
               "through its r-w alias.");

#if defined(__aarch64__)
    using ProbeFunction = uint32_t (*)(void);
    const uint32_t value =
        reinterpret_cast<ProbeFunction>(first.executable)();
#else
    const uint32_t value = 0;
#endif
    gProbed = true;

    if (value != kProbeExpectedValue) {
        snprintf(line, sizeof(line),
                 "Probe returned 0x%08X instead of 0x%08X.", value,
                 kProbeExpectedValue);
        reportLine(line);
        {
            std::lock_guard<std::mutex> lock(gArenaMutex);
            for (ArenaSegment& segment : gSegments) {
                releaseMapping(segment.mapping);
            }
            gSegments.clear();
        }
        gPrepared = false;
        return BVNExecMemStrategyNone;
    }

    {
        std::lock_guard<std::mutex> lock(gArenaMutex);
        for (ArenaSegment& segment : gSegments) {
            segment.allocator.reset(segment.mapping.length);
        }
        gAllocationCount = 0;
        snprintf(line, sizeof(line),
                 "Probe executed successfully. %zu MiB in %zu segment(s) is "
                 "retained for Wine.",
                 arenaCapacity() / kMiB, gSegments.size());
    }
    gExecutionConfirmed = true;
    reportLine(line);
    return BVNExecMemStrategyStikDebugDualMap;
}

extern "C" void* BVNExecMemAlloc(size_t length) {
    if (!gExecutionConfirmed) {
        return nullptr;
    }

    if (length == 0 || length % pageSize() != 0) {
        char error[256];
        snprintf(error, sizeof(error),
                 "JIT arena allocation length %zu is not aligned to the "
                 "%zu-byte iOS page size",
                 length, pageSize());
        BVNLogWrite(BVNLogLevelError, "jit", error);
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(gArenaMutex);
    // First fit across the segments. A block that no segment can hold is a
    // genuinely exhausted arena; a block that only a later segment can hold
    // is the normal case once the earlier ones fill up.
    for (ArenaSegment& segment : gSegments) {
        size_t offset = 0;
        if (!segment.mapping.executable ||
            !segment.allocator.allocate(length, pageSize(), &offset)) {
            continue;
        }

        gAllocationCount++;
        if (gAllocationCount <= 8 || gAllocationCount % 64 == 0) {
            char line[256];
            snprintf(line, sizeof(line),
                     "JIT arena allocation #%zu: %zu bytes, %zu bytes remain "
                     "across %zu segment(s); no StikDebug breakpoint issued.",
                     gAllocationCount, length, arenaAvailable(),
                     gSegments.size());
            BVNLogWrite(BVNLogLevelInfo, "jit", line);
            BVNGuestLoadingUpdateJITProgress(gAllocationCount);
        }
        return segment.mapping.executable + offset;
    }

    char error[384];
    snprintf(error, sizeof(error),
             "JIT arena exhausted: requested %zu bytes with %zu of %zu bytes "
             "free across %zu segment(s), none of them contiguous enough. "
             "Refusing to issue another debugger breakpoint during guest "
             "execution.",
             length, arenaAvailable(), arenaCapacity(), gSegments.size());
    BVNLogWrite(BVNLogLevelError, "jit", error);
    return nullptr;
}

extern "C" void* BVNExecMemWritableAddress(void* address, size_t length) {
    std::lock_guard<std::mutex> lock(gArenaMutex);
    ArenaSegment* segment = segmentStartingAt(address);
    if (!segment || !containsRange(segment->mapping, address, length)) {
        return nullptr;
    }
    const size_t offset =
        reinterpret_cast<uintptr_t>(address) -
        reinterpret_cast<uintptr_t>(segment->mapping.executable);
    if (!segment->allocator.contains(offset, length)) {
        return nullptr;
    }
    return segment->mapping.writable + offset;
}

extern "C" bool BVNExecMemReleaseIfOwned(void* address, size_t length) {
    std::lock_guard<std::mutex> lock(gArenaMutex);
    ArenaSegment* segment = segmentStartingAt(address);
    if (!segment) {
        return false;
    }

    const size_t offset =
        reinterpret_cast<uintptr_t>(address) -
        reinterpret_cast<uintptr_t>(segment->mapping.executable);
    if (!segment->allocator.release(offset, length)) {
        // The pointer still belongs to the arena. Report the caller bug but
        // claim ownership so Platform::releaseNativeMemory does not munmap a
        // slice out of the shared executable mapping.
        char error[256];
        snprintf(error, sizeof(error),
                 "Invalid JIT arena release at offset %zu with length %zu; "
                 "the shared arena was left mapped.",
                 offset, length);
        BVNLogWrite(BVNLogLevelError, "jit", error);
    }
    return true;
}

extern "C" bool BVNExecMemArenaStatus(size_t* capacityBytes,
                                      size_t* availableBytes,
                                      size_t* segmentCount) {
    std::lock_guard<std::mutex> lock(gArenaMutex);
    if (capacityBytes) {
        *capacityBytes = arenaCapacity();
    }
    if (availableBytes) {
        *availableBytes = arenaAvailable();
    }
    if (segmentCount) {
        *segmentCount = gSegments.size();
    }
    return !gSegments.empty();
}

extern "C" void BVNExecMemFree(void* address, size_t length) {
    if (!BVNExecMemReleaseIfOwned(address, length) && address) {
        BVNLogWrite(BVNLogLevelWarning, "jit",
                    "BVNExecMemFree received an unknown executable mapping");
    }
}

extern "C" void BVNExecMemSetWritable(void* address, size_t length,
                                      bool writable) {
    (void)address;
    (void)length;
    (void)writable;
}
