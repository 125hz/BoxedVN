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
 *  plain (non-MAP_JIT) mmap(PROT_EXEC) is NOT "mmap fails safely if
 *  unavailable" the way MAP_JIT did: mmap can return a valid, non-MAP_FAILED
 *  pointer with rwx permissions requested regardless of CS_DEBUGGED, because
 *  the code-signing check on Apple platforms is enforced lazily, at first
 *  INSTRUCTION FETCH from the page, not at mmap() time. If that lazy check
 *  fails, the kernel does not return an error to this process - it delivers
 *  an uncatchable SIGKILL (CODESIGNING / "Invalid Page") on the spot. This is
 *  confirmed from a real device crash: mmap(PROT_EXEC) succeeded, the process
 *  proceeded to call through the resulting pointer, and was killed instantly
 *  with exactly that reason.
 *
 *  This means there is no way to safely "test" whether execution will work
 *  without accepting the same risk Boxedwine's real JIT would hit anyway.
 *  BVNJITProbeStatus() therefore never attempts execution - it can only ever
 *  report that JIT is compiled in and whether CS_DEBUGGED is set, which is
 *  necessary but provably not sufficient. BVNJITProbeExecute() is the one
 *  that actually knows, and it is gated to call sites that accept the risk on
 *  purpose. See BVNRuntime.h for exactly which call sites those are.
 *  ---------------------------------------------------------------------
 */

#import <Foundation/Foundation.h>

#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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
char gDetail[512];

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
    const size_t pageSize = static_cast<size_t>(getpagesize());

    // Step 1: obtain executable memory the same way
    // Platform::alloc64kBlock does on iOS - MAP_BOXEDWINE is 0 there, not
    // MAP_JIT.  See the file header for why MAP_JIT is specifically wrong
    // here rather than merely unnecessary.  This mmap call itself is safe:
    // it is Step 4 (actually calling through the pointer) that can crash.
    void* page = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        const int mmapErrno = errno;
        report.status = BVNJITStatusUnavailable;
        report.executableMemoryAvailable = false;
        if (!report.debuggerAttached) {
            setDetail("mmap(PROT_EXEC) failed: %s (errno %d). The kernel "
                      "does not have this process flagged as debugged "
                      "(CS_DEBUGGED is not set) - no JIT enabler has actually "
                      "attached yet, or it detached again. Attach StikDebug "
                      "(or an equivalent JIT enabler) to THIS app while it is "
                      "running, then check again; BoxedVN cannot enable JIT "
                      "by itself.",
                      strerror(mmapErrno), mmapErrno);
        } else {
            setDetail("mmap(PROT_EXEC) failed: %s (errno %d), even though "
                      "the kernel DOES have this process flagged as debugged "
                      "(CS_DEBUGGED is set). A JIT enabler attached "
                      "successfully, but the process still cannot get "
                      "executable memory. This points at the app's own "
                      "signature: confirm the installed IPA was signed with "
                      "the 'get-task-allow' entitlement present (it is in "
                      "ios/app/BoxedVN.entitlements, but some signing tools "
                      "strip entitlements they don't recognise) and that no "
                      "other MDM/supervision restriction on this device "
                      "blocks debugger-granted executable memory.",
                      strerror(mmapErrno), mmapErrno);
        }
        return report;
    }

    // Step 2: write a function that returns 0x4258 ("BX").
    //
    //   d2808b08   mov  x8, #0x4258
    //   aa0803e0   mov  x0, x8
    //   d65f03c0   ret
    //
    // Written as bytes so the encoding is auditable and no assembler is
    // needed at build time.
    static const uint32_t kProbeCode[] = {
        0xD2808B08,  // mov x8, #0x4258
        0xAA0803E0,  // mov x0, x8
        0xD65F03C0,  // ret
    };

    // iOS has no pthread_jit_write_protect_np and this region was never
    // MAP_JIT in the first place: it is a plain mapping, requested rwx at
    // once. See the comment in platform/linux/platform.cpp.
    memcpy(page, kProbeCode, sizeof(kProbeCode));

    // Step 3: flush the instruction cache.  Skipping this is a classic cause
    // of a JIT that appears to work and then produces garbage.
    sys_icache_invalidate(page, sizeof(kProbeCode));

    // Step 4: THE UNSAFE STEP. mmap succeeding above does not mean this call
    // will not be met with an immediate, uncatchable SIGKILL - see the file
    // header. There is no way to guard this call; if it is going to crash,
    // it crashes here, synchronously, with no exception to catch.
    using ProbeFunction = uint32_t (*)(void);
    ProbeFunction probe = reinterpret_cast<ProbeFunction>(page);
    const uint32_t result = probe();

    // Everything below only runs if the call above did not get the process
    // killed - i.e. execution is now proven to actually work.
    munmap(page, pageSize);
    report.executableMemoryAvailable = true;

    if (result != 0x4258) {
        report.status = BVNJITStatusUnavailable;
        setDetail("Executable memory was mapped and called, but returned "
                  "0x%08X instead of 0x00004258. The instruction cache flush "
                  "or the memory mapping is not behaving as expected; the JIT "
                  "would produce wrong results.",
                  result);
        return report;
    }

    if (!report.jitCompiledIn) {
        report.status = BVNJITStatusUnavailable;
        setDetail("Executable memory works, but this binary was built without "
                  "BOXEDWINE_JIT, so there is no ARM64 code generator to use "
                  "it.");
        return report;
    }

    report.status = BVNJITStatusAvailable;
    setDetail("Executable memory mapped, written, cache-flushed and executed "
              "successfully. Boxedwine's ARM64 JIT can run.");
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
