/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 *
 *  See BVNExecMemory.h for why this file exists.
 */

#include "BVNExecMemory.h"

#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "BVNRuntime.h"

namespace {

// ---------------------------------------------------------------------------
// Reading what the kernel ACTUALLY did
// ---------------------------------------------------------------------------

// Every return code in this file has been observed to lie at least once:
// mmap returns a valid pointer for a mapping it silently downgraded, and
// mprotect returns 0 for a change it silently refused.  vm_region_64 is the
// only source of truth here, and it reports the same numbers the crash
// reporter prints, so its output can be compared against a .ips directly.
bool queryProtection(void* address, vm_prot_t* current, vm_prot_t* maximum) {
    vm_address_t regionAddress = reinterpret_cast<vm_address_t>(address);
    vm_size_t regionSize = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t infoCount = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t objectName = MACH_PORT_NULL;

    const kern_return_t result =
        vm_region_64(mach_task_self(), &regionAddress, &regionSize,
                     VM_REGION_BASIC_INFO_64,
                     reinterpret_cast<vm_region_info_t>(&info), &infoCount,
                     &objectName);
    if (result != KERN_SUCCESS) {
        return false;
    }
    // vm_region_64 returns the first region at OR AFTER the address it is
    // given.  If it walked past our page, the answer describes something else
    // entirely and must not be reported as ours.
    if (regionAddress > reinterpret_cast<vm_address_t>(address)) {
        return false;
    }
    *current = info.protection;
    *maximum = info.max_protection;
    return true;
}

// "rw-/rwx", matching how the crash reporter and vmmap present a region.
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

bool isExecutable(void* address) {
    vm_prot_t current = 0;
    vm_prot_t maximum = 0;
    return queryProtection(address, &current, &maximum) &&
           (current & VM_PROT_EXECUTE) != 0;
}

bool isWritable(void* address) {
    vm_prot_t current = 0;
    vm_prot_t maximum = 0;
    return queryProtection(address, &current, &maximum) &&
           (current & VM_PROT_WRITE) != 0;
}

size_t pageSize() { return static_cast<size_t>(getpagesize()); }

// ---------------------------------------------------------------------------
// The strategies
// ---------------------------------------------------------------------------

struct StrategyResult {
    void* address;   // NULL if the allocation itself failed
    char note[96];   // what each call returned, for the report
};

// Allocates one candidate region using `strategy`, leaving it in exactly the
// state the live allocator would leave it in.  Deliberately NOT parameterised
// by "should I flip" - each strategy has one fixed meaning, so the probe and
// the live path cannot drift apart.
StrategyResult allocate(BVNExecMemStrategy strategy, size_t length) {
    StrategyResult result;
    result.address = nullptr;
    result.note[0] = '\0';

    const int rwx = PROT_READ | PROT_WRITE | PROT_EXEC;
    const int commonFlags = MAP_PRIVATE | MAP_ANONYMOUS;

    switch (strategy) {
        case BVNExecMemStrategyMmapRWX:
        case BVNExecMemStrategyMprotectRWX:
        case BVNExecMemStrategyMprotectFlip: {
            // PROT_EXEC is requested even where iOS will not honour it: the
            // mmap prot argument fixes the region's MAXIMUM protection, and
            // mprotect can never raise a region above its maximum.  Leaving it
            // out is what produced "actual r-- (max rw-)" on device.
            void* page = mmap(nullptr, length, rwx, commonFlags, -1, 0);
            if (page == MAP_FAILED) {
                snprintf(result.note, sizeof(result.note), "mmap failed: %s",
                         strerror(errno));
                return result;
            }
            result.address = page;
            if (strategy == BVNExecMemStrategyMmapRWX) {
                snprintf(result.note, sizeof(result.note), "mmap only");
            } else {
                const int wanted = (strategy == BVNExecMemStrategyMprotectRWX)
                                       ? rwx
                                       : (PROT_READ | PROT_EXEC);
                const bool ok = mprotect(page, length, wanted) == 0;
                snprintf(result.note, sizeof(result.note), "mprotect(%s) %s",
                         (wanted == rwx) ? "rwx" : "r-x",
                         ok ? "ok" : strerror(errno));
            }
            return result;
        }

        case BVNExecMemStrategyMapJitRWX:
        case BVNExecMemStrategyMapJitFlip: {
            // MAP_JIT normally requires the "dynamic-codesigning" entitlement,
            // which is browser-engine-only. It is tried anyway because the
            // rules for a CS_DEBUGGED process are not the rules for a normal
            // one, and an EPERM here is a one-line answer rather than a guess.
            void* page = mmap(nullptr, length, rwx, commonFlags | MAP_JIT, -1, 0);
            if (page == MAP_FAILED) {
                snprintf(result.note, sizeof(result.note),
                         "mmap(MAP_JIT) failed: %s", strerror(errno));
                return result;
            }
            result.address = page;
            if (strategy == BVNExecMemStrategyMapJitRWX) {
                snprintf(result.note, sizeof(result.note), "mmap(MAP_JIT) only");
            } else {
                const bool ok =
                    mprotect(page, length, PROT_READ | PROT_EXEC) == 0;
                snprintf(result.note, sizeof(result.note),
                         "mmap(MAP_JIT)+mprotect(r-x) %s",
                         ok ? "ok" : strerror(errno));
            }
            return result;
        }

        case BVNExecMemStrategyMachFlip: {
            // The Mach interface is not just a different spelling of mmap: it
            // can set a region's MAXIMUM protection explicitly, which the BSD
            // layer cannot do after the fact.  If iOS is capping the maximum,
            // this is the call that would get past it.
            vm_address_t address = 0;
            kern_return_t kr = vm_allocate(mach_task_self(), &address, length,
                                           VM_FLAGS_ANYWHERE);
            if (kr != KERN_SUCCESS) {
                snprintf(result.note, sizeof(result.note),
                         "vm_allocate failed (kr %d)", kr);
                return result;
            }
            result.address = reinterpret_cast<void*>(address);
            const kern_return_t krMax =
                vm_protect(mach_task_self(), address, length, TRUE,
                           VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
            const kern_return_t krCur =
                vm_protect(mach_task_self(), address, length, FALSE,
                           VM_PROT_READ | VM_PROT_EXECUTE);
            snprintf(result.note, sizeof(result.note),
                     "vm_protect max kr %d, cur kr %d", krMax, krCur);
            return result;
        }

        case BVNExecMemStrategyNone:
            break;
    }

    snprintf(result.note, sizeof(result.note), "no such strategy");
    return result;
}

void release(BVNExecMemStrategy strategy, void* address, size_t length) {
    if (address == nullptr) {
        return;
    }
    if (strategy == BVNExecMemStrategyMachFlip) {
        vm_deallocate(mach_task_self(),
                      reinterpret_cast<vm_address_t>(address), length);
    } else {
        munmap(address, length);
    }
}

// Whether a strategy leaves pages non-writable, so writes have to flip them.
bool strategyNeedsFlip(BVNExecMemStrategy strategy) {
    switch (strategy) {
        case BVNExecMemStrategyMapJitFlip:
        case BVNExecMemStrategyMprotectFlip:
        case BVNExecMemStrategyMachFlip:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Probe state
// ---------------------------------------------------------------------------

// Tried in this order deliberately: every strategy that leaves pages both
// writable and executable comes before every strategy that has to flip
// protection around each write.  Flipping works, but it is process-wide where
// macOS's equivalent is per-thread, so it opens a window in which one thread
// can execute a page another thread just made writable.  Avoiding that
// entirely is worth more than any of these being marginally faster.
const BVNExecMemStrategy kStrategyOrder[] = {
    BVNExecMemStrategyMmapRWX,
    BVNExecMemStrategyMprotectRWX,
    BVNExecMemStrategyMapJitRWX,
    BVNExecMemStrategyMapJitFlip,
    BVNExecMemStrategyMprotectFlip,
    BVNExecMemStrategyMachFlip,
};
const int kStrategyCount =
    static_cast<int>(sizeof(kStrategyOrder) / sizeof(kStrategyOrder[0]));

//   d2808b08   mov  x8, #0x4258
//   aa0803e0   mov  x0, x8
//   d65f03c0   ret
//
// Written as bytes so the encoding is auditable here and no assembler is
// needed at build time.  0x4258 is arbitrary but distinctive: a page that
// executes and returns something else means the instruction cache was not
// flushed properly, which is a silent-wrong-answer bug rather than a crash.
const uint32_t kProbeCode[] = {
    0xD2808B08,
    0xAA0803E0,
    0xD65F03C0,
};

BVNExecMemStrategy gStrategy = BVNExecMemStrategyNone;
bool gProbed = false;
bool gProbedWithExecute = false;
bool gExecutionConfirmed = false;
char gReport[1600] = "not probed yet";

void reportLine(const char* line) {
    // Logged as it is produced, not at the end: if the execution step kills
    // the process, the lines already written are the entire diagnosis.
    BVNLogWrite(BVNLogLevelInfo, "jit", line);
    const size_t used = strlen(gReport);
    snprintf(gReport + used, sizeof(gReport) - used, "%s%s",
             used > 0 ? "\n" : "", line);
}

void writeProbeCode(void* page, size_t length) {
    const bool flip = !isWritable(page);
    if (flip) {
        BVNExecMemSetWritable(page, length, true);
    }
    memcpy(page, kProbeCode, sizeof(kProbeCode));
    if (flip) {
        BVNExecMemSetWritable(page, length, false);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

extern "C" const char* BVNExecMemStrategyName(BVNExecMemStrategy strategy) {
    switch (strategy) {
        case BVNExecMemStrategyMmapRWX:      return "mmap(rwx)";
        case BVNExecMemStrategyMprotectRWX:  return "mmap+mprotect(rwx)";
        case BVNExecMemStrategyMapJitRWX:    return "mmap(MAP_JIT,rwx)";
        case BVNExecMemStrategyMapJitFlip:   return "mmap(MAP_JIT)+mprotect(r-x)";
        case BVNExecMemStrategyMprotectFlip: return "mmap+mprotect(r-x)";
        case BVNExecMemStrategyMachFlip:     return "vm_allocate+vm_protect(r-x)";
        case BVNExecMemStrategyNone:         return "none";
    }
    return "unknown";
}

extern "C" const char* BVNExecMemReport(void) { return gReport; }

extern "C" bool BVNExecMemExecutionConfirmed(void) { return gExecutionConfirmed; }

extern "C" bool BVNExecMemNeedsWriteFlip(void) {
    return strategyNeedsFlip(gStrategy);
}

extern "C" BVNExecMemStrategy BVNExecMemProbe(bool allowExecute) {
    // A probe that already ran WITH execution is final.  A probe that ran
    // without it still has something left to prove, so a later caller willing
    // to take the risk gets to run the execution step.
    if (gProbed && (gProbedWithExecute || !allowExecute)) {
        return gStrategy;
    }

    const size_t length = pageSize();

    if (!gProbed) {
        gReport[0] = '\0';
        gStrategy = BVNExecMemStrategyNone;

        void* chosenPage = nullptr;
        for (int i = 0; i < kStrategyCount; ++i) {
            const BVNExecMemStrategy strategy = kStrategyOrder[i];
            StrategyResult attempt = allocate(strategy, length);

            char line[220];
            if (attempt.address == nullptr) {
                snprintf(line, sizeof(line), "%d %s: %s", i + 1,
                         BVNExecMemStrategyName(strategy), attempt.note);
                reportLine(line);
                continue;
            }

            char protection[9];
            protectionText(attempt.address, protection);
            const bool executable = isExecutable(attempt.address);
            snprintf(line, sizeof(line), "%d %s: %s -> %s%s", i + 1,
                     BVNExecMemStrategyName(strategy), attempt.note,
                     protection, executable ? "  EXECUTABLE" : "");
            reportLine(line);

            // Keep the first winner mapped so the execution step below can use
            // the very page that was just measured, rather than a fresh one
            // that might differ.
            if (executable && gStrategy == BVNExecMemStrategyNone) {
                gStrategy = strategy;
                chosenPage = attempt.address;
            } else {
                release(strategy, attempt.address, length);
            }
        }

        gProbed = true;

        if (gStrategy == BVNExecMemStrategyNone) {
            reportLine("No strategy produced an executable page. The JIT "
                       "cannot run on this device in its current state.");
            return gStrategy;
        }

        char line[220];
        snprintf(line, sizeof(line),
                 "Selected %s; writes %s a protection flip.",
                 BVNExecMemStrategyName(gStrategy),
                 strategyNeedsFlip(gStrategy) ? "need" : "do not need");
        reportLine(line);

        if (!allowExecute) {
            release(gStrategy, chosenPage, length);
            reportLine("Execution not attempted here (the caller asked for the "
                       "safe path); the page is executable per the kernel but "
                       "that is not yet proof.");
            return gStrategy;
        }

        // Fall through with the winning page still mapped.
        writeProbeCode(chosenPage, length);
        sys_icache_invalidate(chosenPage, sizeof(kProbeCode));

        // THE UNSAFE STEP, and the reason every line above is logged before
        // reaching it: if this call faults, it faults synchronously with
        // nothing to catch it, and the log is all that survives.
        reportLine("About to execute the probe page.");
        gProbedWithExecute = true;

        using ProbeFunction = uint32_t (*)(void);
        ProbeFunction probe = reinterpret_cast<ProbeFunction>(chosenPage);
        const uint32_t value = probe();

        release(gStrategy, chosenPage, length);

        if (value == 0x4258) {
            gExecutionConfirmed = true;
            reportLine("Executed successfully and returned the expected value.");
        } else {
            gStrategy = BVNExecMemStrategyNone;
            snprintf(line, sizeof(line),
                     "Executed but returned 0x%08X instead of 0x00004258 - the "
                     "mapping or the cache flush is not behaving, so the JIT "
                     "would produce wrong results.", value);
            reportLine(line);
        }
        return gStrategy;
    }

    // Already probed without execution, and a caller now wants the proof.
    gProbedWithExecute = true;
    if (gStrategy == BVNExecMemStrategyNone) {
        return gStrategy;
    }

    void* page = BVNExecMemAlloc(length);
    if (page == nullptr) {
        gStrategy = BVNExecMemStrategyNone;
        reportLine("The selected strategy stopped working on a second "
                   "allocation.");
        return gStrategy;
    }
    if (!isExecutable(page)) {
        BVNExecMemFree(page, length);
        gStrategy = BVNExecMemStrategyNone;
        reportLine("A second allocation came back non-executable even though "
                   "the first did not.");
        return gStrategy;
    }

    writeProbeCode(page, length);
    sys_icache_invalidate(page, sizeof(kProbeCode));
    reportLine("About to execute the probe page.");

    using ProbeFunction = uint32_t (*)(void);
    ProbeFunction probe = reinterpret_cast<ProbeFunction>(page);
    const uint32_t value = probe();
    BVNExecMemFree(page, length);

    if (value == 0x4258) {
        gExecutionConfirmed = true;
        reportLine("Executed successfully and returned the expected value.");
    } else {
        gStrategy = BVNExecMemStrategyNone;
        char line[220];
        snprintf(line, sizeof(line),
                 "Executed but returned 0x%08X instead of 0x00004258.", value);
        reportLine(line);
    }
    return gStrategy;
}

extern "C" void* BVNExecMemAlloc(size_t length) {
    if (!gProbed) {
        // Probing without execution: an allocation must never be the thing
        // that risks the process.
        BVNExecMemProbe(false);
    }
    if (gStrategy == BVNExecMemStrategyNone) {
        return nullptr;
    }

    StrategyResult result = allocate(gStrategy, length);
    if (result.address == nullptr) {
        return nullptr;
    }
    // The strategy was chosen from a one-page probe.  Larger allocations go
    // through the identical calls, but "identical calls" has not been a
    // reliable predictor of "identical outcome" anywhere else in this file.
    if (!isExecutable(result.address)) {
        char line[220];
        char protection[9];
        protectionText(result.address, protection);
        snprintf(line, sizeof(line),
                 "%s produced a non-executable region (%s) for %zu bytes even "
                 "though it worked for one page.",
                 BVNExecMemStrategyName(gStrategy), protection, length);
        BVNLogWrite(BVNLogLevelError, "jit", line);
        release(gStrategy, result.address, length);
        return nullptr;
    }
    return result.address;
}

extern "C" void BVNExecMemFree(void* address, size_t length) {
    release(gStrategy, address, length);
}

extern "C" void BVNExecMemSetWritable(void* address, size_t length,
                                      bool writable) {
    if (!strategyNeedsFlip(gStrategy)) {
        return;
    }

    const uintptr_t mask = static_cast<uintptr_t>(pageSize()) - 1;
    const uintptr_t start = reinterpret_cast<uintptr_t>(address) & ~mask;
    const uintptr_t end =
        (reinterpret_cast<uintptr_t>(address) + length + mask) & ~mask;

    if (gStrategy == BVNExecMemStrategyMachFlip) {
        vm_protect(mach_task_self(), static_cast<vm_address_t>(start),
                   end - start, FALSE,
                   writable ? (VM_PROT_READ | VM_PROT_WRITE)
                            : (VM_PROT_READ | VM_PROT_EXECUTE));
        return;
    }

    mprotect(reinterpret_cast<void*>(start), end - start,
             writable ? (PROT_READ | PROT_WRITE) : (PROT_READ | PROT_EXEC));
}
