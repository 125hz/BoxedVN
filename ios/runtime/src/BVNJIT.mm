/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 *
 *  ---------------------------------------------------------------------
 *  Real JIT availability detection - split into a safe check and an unsafe
 *  one, because "unsafe" here specifically means "can SIGKILL the process
 *  with no recovery possible." See BVNRuntime.h for the full contract of
 *  each; this file is the implementation and the mechanism explanation.
 *
 *  What actually gates executable memory for a sideloaded third-party app on
 *  iOS is NOT the same thing as macOS's MAP_JIT.  MAP_JIT is gated behind
 *  Apple's "dynamic-codesigning" entitlement, which is approved only for
 *  browser engines and cannot be obtained by a sideloaded app no matter what
 *  debugger is attached - passing MAP_JIT here always fails with EPERM on
 *  iOS, attached debugger or not. (An earlier version of this file assumed
 *  CS_DEBUGGED unlocked MAP_JIT; that trick existed pre-iOS 14 and Apple
 *  patched it.)
 *
 *  What a legitimate third-party debugger attach - StikDebug or equivalent,
 *  over the real Developer Disk Image / debugserver channel - does is get the
 *  kernel to flag the process CS_DEBUGGED. What CS_DEBUGGED actually buys a
 *  getting executable memory is a TWO step operation, and the first step lies
 *  about failing.
 *
 *  process is executable memory, and getting it is where three separate
 *  attempts have now failed on device - mmap lying about PROT_EXEC, iOS
 *  enforcing W^X silently, and the region maximum being fixed at mmap time.
 *  Rather than encode a fourth guess, the mechanics live in BVNExecMemory,
 *  which tries several strategies, reads back what the kernel ACTUALLY did
 *  with vm_region_64, and uses whichever one works.  Read that header for the
 *  full history; this file only decides what to tell the user.
 *
 *  Platform::alloc64kBlock on iOS goes through the same module, so a probe
 *  that passes is evidence about the code that really runs the guest.  It was
 *  a hand-written copy of the sequence before, and copies drift - that is
 *  precisely how failure three happened.
 *
 *  BVNJITProbeStatus() never attempts execution at all - it reports only
 *  whether JIT is compiled in and whether CS_DEBUGGED is set, which is
 *  necessary but not sufficient. BVNJITProbeExecute() runs the full matrix
 *  including the call into freshly written memory, and is gated to call sites
 *  that accept the residual risk on purpose.  See BVNRuntime.h for which
 *  those are.
 *  ---------------------------------------------------------------------
 */

#import <Foundation/Foundation.h>

#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "BVNExecMemory.h"
#include "BVNRuntime.h"

// csops() is not declared in a public iOS SDK header, but it is a real,
// stable libSystem entry point - the same syscall wrapper Apple's own
// debugserver and every JIT-enabler / anti-debugging library on iOS uses to
// query a process's own code-signing status.  Querying your own pid needs no
// entitlement and touches no guest memory, so it is always safe.  Declared
// here rather than pulled from a private header so the dependency is exactly
// one function prototype, auditable in place.
extern "C" int csops(pid_t pid, unsigned int ops, void* useraddr,
                     size_t usersize);

namespace {
constexpr unsigned int kCSOpsStatus = 0;   // CS_OPS_STATUS
constexpr uint32_t kCSDebugged = 0x10000000;  // CS_DEBUGGED
}  // namespace

#if TARGET_CPU_ARM64 || defined(__aarch64__)
#define BVN_PROBE_ARM64 1
#endif

namespace {

// Storage for the `detail` string handed back to the caller.  Single-slot on
// purpose: BVNJITReport documents that it is valid only until the next probe.
// Large enough to carry BVNExecMemory's whole strategy table, which is the
// only place the full picture exists when something goes wrong on a device
// that is not attached to a debugger.
char gDetail[2048];

void setDetail(const char* format, ...) __attribute__((format(printf, 1, 2)));

void setDetail(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(gDetail, sizeof(gDetail), format, args);
    va_end(args);
}

bool isDebugged() {
    uint32_t flags = 0;
    // A nonzero return means the query itself failed (e.g. bad arguments);
    // that is not the same as "not debugged" and is treated as false rather
    // than risk a false positive.
    if (csops(getpid(), kCSOpsStatus, &flags, sizeof(flags)) != 0) {
        return false;
    }
    return (flags & kCSDebugged) != 0;
}

bool jitCompiledIn() {
#if defined(BOXEDWINE_JIT)
    return true;
#else
    return false;
#endif
}

}  // namespace

extern "C" BVNJITReport BVNJITProbeStatus(void) {
    BVNJITReport report;
    report.executableMemoryAvailable = false;
    report.detail = gDetail;
    gDetail[0] = '\0';

    report.jitCompiledIn = jitCompiledIn();
    report.debuggerAttached = isDebugged();

#if !defined(BVN_PROBE_ARM64)
    report.status = BVNJITStatusUnavailable;
    setDetail("This build is not ARM64, so Boxedwine's ARM64 JIT backend is "
              "not present. BoxedVN only supports ARM64 devices.");
#elif !defined(BOXEDWINE_JIT)
    report.status = BVNJITStatusUnavailable;
    setDetail("This binary was built without BOXEDWINE_JIT, so there is no "
              "ARM64 code generator to use even if executable memory becomes "
              "available.");
#else
    if (report.debuggerAttached) {
        report.status = BVNJITStatusLikelyAvailable;
        setDetail("The kernel has this process flagged CS_DEBUGGED, so a JIT "
                  "enabler has genuinely attached. Executable memory has not "
                  "been tested here - actually attempting it can crash the "
                  "process on some iOS versions if it turns out not to work, "
                  "so that is deferred to the moment a guest actually starts. "
                  "Boxedwine's JIT is expected to work.");
    } else {
        report.status = BVNJITStatusUnavailable;
        setDetail("The kernel does not have this process flagged as debugged "
                  "(CS_DEBUGGED is not set). Attach StikDebug (or an "
                  "equivalent JIT enabler) to THIS app while it is running, "
                  "then check again; BoxedVN cannot enable JIT by itself.");
    }
#endif

    return report;
}

extern "C" BVNJITReport BVNJITProbeExecute(void) {
    BVNJITReport report;
    report.status = BVNJITStatusUnknown;
    report.executableMemoryAvailable = false;
    report.detail = gDetail;
    gDetail[0] = '\0';

    report.jitCompiledIn = jitCompiledIn();
    report.debuggerAttached = isDebugged();

#if !defined(BVN_PROBE_ARM64)
    report.status = BVNJITStatusUnavailable;
    setDetail("This build is not ARM64, so Boxedwine's ARM64 JIT backend is "
              "not present. BoxedVN only supports ARM64 devices.");
    return report;
#else
    // Everything below is delegated to BVNExecMemory, which is also what
    // Platform::alloc64kBlock uses on iOS. That is the point: three separate
    // bugs in this file came from the probe and the live allocator being two
    // hand-synchronised copies of the same sequence that quietly diverged.
    // There is now one implementation, so a probe that passes is evidence
    // about the code that actually runs the guest.
    const BVNExecMemStrategy strategy = BVNExecMemProbe(/*allowExecute=*/true);

    if (strategy == BVNExecMemStrategyNone) {
        report.status = BVNJITStatusUnavailable;
        report.executableMemoryAvailable = false;
        if (!report.debuggerAttached) {
            setDetail("No way of obtaining executable memory works, and the "
                      "kernel does not have this process flagged as debugged "
                      "(CS_DEBUGGED is not set) - no JIT enabler has attached "
                      "yet, or it detached again. Attach StikDebug (or an "
                      "equivalent) to THIS app while it is running, then try "
                      "again; BoxedVN cannot enable JIT by itself.\n\n%s",
                      BVNExecMemReport());
        } else {
            setDetail("A JIT enabler IS attached (CS_DEBUGGED is set), but no "
                      "way of obtaining executable memory works on this "
                      "device. Every allocation strategy and what the kernel "
                      "actually did with it:\n\n%s",
                      BVNExecMemReport());
        }
        return report;
    }

    report.executableMemoryAvailable = true;

    if (!BVNExecMemExecutionConfirmed()) {
        // Reached only if the page was executable by every measure and the
        // call still did not produce the expected value.
        report.status = BVNJITStatusUnavailable;
        setDetail("Executable memory was obtained but did not behave "
                  "correctly:\n\n%s", BVNExecMemReport());
        return report;
    }

    if (!report.jitCompiledIn) {
        report.status = BVNJITStatusUnavailable;
        setDetail("Executable memory works (%s), but this binary was built "
                  "without BOXEDWINE_JIT, so there is no ARM64 code generator "
                  "to use it.", BVNExecMemStrategyName(strategy));
        return report;
    }

    report.status = BVNJITStatusAvailable;
    setDetail("Executable memory obtained with %s, written, cache-flushed and "
              "executed successfully%s. Boxedwine's ARM64 JIT can run.\n\n%s",
              BVNExecMemStrategyName(strategy),
              BVNExecMemNeedsWriteFlip()
                  ? " (writes need a process-wide protection flip, which is a "
                    "known multithreading hazard - see "
                    "docs/KNOWN_LIMITATIONS_IOS.md)"
                  : "",
              BVNExecMemReport());
    return report;
#endif
}

extern "C" const char* BVNRuntimeBoxedwineVersion(void) {
    // Mirrors BOXEDWINE_VERSION_STR in include/boxedwine.h.  Defined by the
    // build so this file does not have to include the whole Boxedwine header
    // set.
#ifdef BVN_BOXEDWINE_VERSION
    return BVN_BOXEDWINE_VERSION;
#else
    return "unknown";
#endif
}
