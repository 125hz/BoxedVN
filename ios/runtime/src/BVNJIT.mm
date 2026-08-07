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
 *  On iOS a sideloaded app cannot map executable memory unless the kernel has
 *  granted it the dynamic-codesigning entitlement, which happens only while a
 *  debugger is attached.  StikDebug and equivalent tools do exactly that.
 *  BoxedVN cannot grant it to itself and does not pretend otherwise.
 *
 *  Rather than inferring availability from an entitlement string, this probe
 *  performs the entire operation Boxedwine's JIT depends on: mmap a MAP_JIT
 *  page, write ARM64 instructions into it, flush the instruction cache and
 *  call the result.  If that returns the expected value, the JIT will work.
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

    // Step 1: obtain executable memory the same way Platform::alloc64kBlock
    // does, with MAP_JIT.
    void* page = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    if (page == MAP_FAILED) {
        const int mmapErrno = errno;
        report.status = BVNJITStatusUnavailable;
        report.executableMemoryAvailable = false;
        if (!report.debuggerAttached) {
            setDetail("mmap(PROT_EXEC | MAP_JIT) failed: %s (errno %d). The "
                      "kernel does not have this process flagged as debugged "
                      "(CS_DEBUGGED is not set) - no JIT enabler has actually "
                      "attached yet, or it detached again. Attach StikDebug "
                      "(or an equivalent JIT enabler) to THIS app while it is "
                      "running, then check again; BoxedVN cannot enable JIT "
                      "by itself.",
                      strerror(mmapErrno), mmapErrno);
        } else {
            setDetail("mmap(PROT_EXEC | MAP_JIT) failed: %s (errno %d), even "
                      "though the kernel DOES have this process flagged as "
                      "debugged (CS_DEBUGGED is set). A JIT enabler attached "
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

    // iOS has no pthread_jit_write_protect_np: a MAP_JIT region is RWX here.
    // See the comment in platform/linux/platform.cpp.
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
    setDetail("MAP_JIT page mapped, written, cache-flushed and executed "
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
