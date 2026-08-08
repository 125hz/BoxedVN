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
 *  Real JIT availability detection.
 *
 *  What actually gates executable memory for a sideloaded third-party app on
 *  iOS is NOT the same thing as macOS's MAP_JIT.  MAP_JIT is gated behind
 *  Apple's "dynamic-codesigning" entitlement, which is approved only for
 *  browser engines and cannot be obtained by a sideloaded app no matter what
 *  debugger is attached - passing MAP_JIT here always fails with EPERM on
 *  iOS, attached debugger or not. (An earlier version of this file assumed
 *  CS_DEBUGGED unlocked MAP_JIT; that trick existed pre-iOS 14 and Apple
 *  patched it. The confusion is common enough that it is worth being
 *  explicit about here.)
 *
 *  What a legitimate third-party debugger attach - StikDebug or equivalent,
 *  over the real Developer Disk Image / debugserver channel, the same one
 *  Xcode's own "Debug > Attach to Process" uses - actually does is get the
 *  kernel to flag the process CS_DEBUGGED, which relaxes code-signing
 *  enforcement for plain mmap()/mprotect() PROT_EXEC transitions on THAT
 *  process. No MAP_JIT flag involved at all. BoxedVN cannot cause this
 *  itself; it requires get-task-allow in its own signature (present in
 *  ios/app/BoxedVN.entitlements, though a signing tool can strip it) and an
 *  external JIT enabler to actually attach.
 *
 *  This probe performs the exact allocation Platform::alloc64kBlock uses
 *  for Boxedwine's real JIT code buffers (see include/boxedwine.h's
 *  MAP_BOXEDWINE and platform/linux/platform.cpp): plain mmap with
 *  PROT_READ|PROT_WRITE|PROT_EXEC and no MAP_JIT, then a write, an
 *  instruction-cache flush, and a call through the result. If that returns
 *  the expected value, Boxedwine's JIT will work; if it fails, the kernel's
 *  reason (checked via csops() CS_DEBUGGED, independent of the mmap outcome)
 *  is reported alongside the raw errno rather than a guess.
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
// entitlement.  Declared here rather than pulled from a private header so the
// dependency is exactly one function prototype, auditable in place.
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

}  // namespace

extern "C" BVNJITReport BVNJITProbe(void) {
    BVNJITReport report;
    report.status = BVNJITStatusUnknown;
    report.executableMemoryAvailable = false;
    report.detail = gDetail;
    gDetail[0] = '\0';

#if defined(BOXEDWINE_JIT)
    report.jitCompiledIn = true;
#else
    report.jitCompiledIn = false;
#endif

    // Read directly from the kernel, independent of whether the mmap probe
    // below succeeds, so a failure can be told apart: no debugger ever
    // attached, versus a debugger attached but executable memory is still
    // unavailable (a signing/entitlement problem, not a JIT-enabler problem).
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
    // here rather than merely unnecessary.
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
    report.executableMemoryAvailable = true;

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
    // MAP_JIT in the first place: it is a plain RWX mapping, writable and
    // executable at once for as long as CS_DEBUGGED holds. See the comment
    // in platform/linux/platform.cpp.
    memcpy(page, kProbeCode, sizeof(kProbeCode));

    // Step 3: flush the instruction cache.  Skipping this is the classic
    // cause of a JIT that works in a debugger and crashes in the field, so it
    // is part of what the probe verifies.
    sys_icache_invalidate(page, sizeof(kProbeCode));

    // Step 4: execute it.
    using ProbeFunction = uint32_t (*)(void);
    ProbeFunction probe = reinterpret_cast<ProbeFunction>(page);
    const uint32_t result = probe();

    munmap(page, pageSize);

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
