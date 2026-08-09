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
// StikDebug is background-suspended). Prepare one modest arena during the
// explicit startup probe, then service live JIT allocations without another
// external handshake.
// Wine's debugger pushed the Song of Saya launch beyond the original 64 MiB
// arena on-device.  Prepare one larger region up front: requesting another
// StikDebug breakpoint while the guest is running is not safe.
constexpr size_t kJitArenaSize = 128u * 1024u * 1024u;

std::mutex gArenaMutex;
DualMapping gArena;
BVNRangeAllocator gArenaAllocator;
size_t gAllocationCount = 0;
bool gProbed = false;
bool gExecutionConfirmed = false;
char gReport[1600] = "not probed yet";

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

bool startsInArena(void* address) {
    if (!gArena.executable || !address) {
        return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t base = reinterpret_cast<uintptr_t>(gArena.executable);
    return start >= base && start - base < gArena.length;
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

extern "C" BVNExecMemStrategy BVNExecMemProbe(bool allowExecute) {
    if (gProbed) {
        return gExecutionConfirmed ? BVNExecMemStrategyStikDebugDualMap
                                   : BVNExecMemStrategyNone;
    }
    if (!allowExecute) {
        return BVNExecMemStrategyNone;
    }

    gReport[0] = '\0';
    char error[512] = {};
    char line[256];
    snprintf(line, sizeof(line),
             "Requesting one %zu MiB executable arena from StikDebug; live "
             "Wine JIT allocations will be suballocated without more "
             "debugger breakpoints.",
             kJitArenaSize / (1024u * 1024u));
    reportLine(line);

    DualMapping mapping =
        allocatePrepared(kJitArenaSize, error, sizeof(error));
    if (!mapping.executable) {
        reportLine(error);
        gProbed = true;
        return BVNExecMemStrategyNone;
    }

    char rxProtection[9];
    char rwProtection[9];
    protectionText(mapping.executable, rxProtection);
    protectionText(mapping.writable, rwProtection);
    snprintf(line, sizeof(line),
             "StikDebug prepared the %zu MiB arena; dual mapping is rx %s, "
             "rw %s.",
             kJitArenaSize / (1024u * 1024u), rxProtection, rwProtection);
    reportLine(line);

    memcpy(mapping.writable, kProbeCode, sizeof(kProbeCode));
    sys_icache_invalidate(mapping.executable, sizeof(kProbeCode));
    reportLine("About to execute through the r-x mapping after writing only "
               "through its r-w alias.");

#if defined(__aarch64__)
    using ProbeFunction = uint32_t (*)(void);
    const uint32_t value =
        reinterpret_cast<ProbeFunction>(mapping.executable)();
#else
    const uint32_t value = 0;
#endif
    gProbed = true;

    if (value != kProbeExpectedValue) {
        snprintf(line, sizeof(line),
                 "Probe returned 0x%08X instead of 0x%08X.", value,
                 kProbeExpectedValue);
        reportLine(line);
        releaseMapping(mapping);
        return BVNExecMemStrategyNone;
    }

    {
        std::lock_guard<std::mutex> lock(gArenaMutex);
        gArena = mapping;
        gArenaAllocator.reset(mapping.length);
        gAllocationCount = 0;
    }
    gExecutionConfirmed = true;
    reportLine("Probe executed successfully. The prepared arena is retained "
               "for Wine and no further StikDebug stops are required.");
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
    size_t offset = 0;
    if (!gArena.executable ||
        !gArenaAllocator.allocate(length, pageSize(), &offset)) {
        char error[320];
        snprintf(error, sizeof(error),
                 "JIT arena exhausted: requested %zu bytes with %zu of %zu "
                 "bytes free. Refusing to issue another debugger breakpoint "
                 "during guest execution.",
                 length, gArenaAllocator.available(),
                 gArenaAllocator.capacity());
        BVNLogWrite(BVNLogLevelError, "jit", error);
        return nullptr;
    }

    gAllocationCount++;
    if (gAllocationCount <= 8 || gAllocationCount % 64 == 0) {
        char line[256];
        snprintf(line, sizeof(line),
                 "JIT arena allocation #%zu: %zu bytes, %zu bytes remain; "
                 "no StikDebug breakpoint issued.",
                 gAllocationCount, length, gArenaAllocator.available());
        BVNLogWrite(BVNLogLevelInfo, "jit", line);
        BVNGuestLoadingUpdateJITProgress(gAllocationCount);
    }
    return gArena.executable + offset;
}

extern "C" void* BVNExecMemWritableAddress(void* address, size_t length) {
    std::lock_guard<std::mutex> lock(gArenaMutex);
    if (!containsRange(gArena, address, length)) {
        return nullptr;
    }
    const size_t offset = reinterpret_cast<uintptr_t>(address) -
                          reinterpret_cast<uintptr_t>(gArena.executable);
    if (!gArenaAllocator.contains(offset, length)) {
        return nullptr;
    }
    return gArena.writable + offset;
}

extern "C" bool BVNExecMemReleaseIfOwned(void* address, size_t length) {
    std::lock_guard<std::mutex> lock(gArenaMutex);
    if (!startsInArena(address)) {
        return false;
    }

    const size_t offset = reinterpret_cast<uintptr_t>(address) -
                          reinterpret_cast<uintptr_t>(gArena.executable);
    if (!gArenaAllocator.release(offset, length)) {
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
