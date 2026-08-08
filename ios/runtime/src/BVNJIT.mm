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
 *  mmap() with PROT_EXEC does not return an error when execution is not
 *  permitted. It clamps the region's *current* protection to rw-, leaves rwx
 *  as the maximum, and hands back a perfectly valid pointer. Jumping into
 *  that page then takes an instruction-abort permission fault. Confirmed from
 *  a device crash report: pc equal to the mapped address, esr "(Instruction
 *  Abort) Permission fault", region listed as "rw-/rwx". The call the kernel
 *  actually honours for a CS_DEBUGGED process is a subsequent mprotect()
 *  adding PROT_EXEC.
 *
 *  So the sequence - mirrored exactly by Platform::alloc64kBlock on iOS, see
 *  platform/linux/platform.cpp - is: mmap rw-, mprotect to add execute, then
 *  VERIFY with vm_region_64 that the execute bit is genuinely set before
 *  trusting it. The verification is not paranoia: both mmap and mprotect can
 *  report success while leaving the page non-executable, and the only other
 *  way to discover that is a fault that cannot be caught.
 *
 *  BVNJITProbeStatus() never attempts execution at all - it reports only
 *  whether JIT is compiled in and whether CS_DEBUGGED is set, which is
 *  necessary but not sufficient. BVNJITProbeExecute() does the full sequence
 *  above and is gated to call sites that accept the residual risk on purpose.
 *  See BVNRuntime.h for which those are.
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

// Reads the kernel's ACTUAL current and maximum protection for the region
// containing `address`.
//
// This exists because mmap() lying is the entire problem: asking for
// PROT_READ|PROT_WRITE|PROT_EXEC on iOS does not fail when execution is not
// permitted - it silently returns a valid pointer to a page whose *current*
// protection is only rw-, with rwx merely as the maximum. Jumping into that
// page then takes an instruction-abort permission fault (confirmed on a real
// device: pc == the mapped address, esr "(Instruction Abort) Permission
// fault", region "rw-/rwx"). Checking the real protection turns that crash
// into a diagnostic.
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
    *current = info.protection;
    *maximum = info.max_protection;
    return true;
}

// "rwx"-style rendering of a vm_prot_t, matching how the crash reporter and
// vmmap present it, so log output can be compared against a crash report
// directly.
void formatProtection(vm_prot_t protection, char out[4]) {
    out[0] = (protection & VM_PROT_READ) ? 'r' : '-';
    out[1] = (protection & VM_PROT_WRITE) ? 'w' : '-';
    out[2] = (protection & VM_PROT_EXECUTE) ? 'x' : '-';
    out[3] = '\0';
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

    // Step 1: map the page WITHOUT PROT_EXEC, then add execute permission
    // with mprotect() - the same sequence Platform::alloc64kBlock uses on
    // iOS.
    //
    // Requesting PROT_EXEC directly in the mmap does not work here and, worse,
    // does not fail: iOS clamps the *current* protection to rw- while leaving
    // rwx as the maximum, hands back a perfectly valid pointer, and only
    // faults later when something jumps into it. mprotect() afterwards is the
    // call the kernel actually honours for a CS_DEBUGGED process.
    void* page = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        const int mmapErrno = errno;
        report.status = BVNJITStatusUnavailable;
        report.executableMemoryAvailable = false;
        setDetail("mmap(PROT_READ | PROT_WRITE) failed: %s (errno %d). This "
                  "is a plain anonymous mapping with no execute permission "
                  "requested, so this failure is not about JIT at all - the "
                  "process is out of address space or memory.",
                  strerror(mmapErrno), mmapErrno);
        return report;
    }

    const bool mprotectOK =
        mprotect(page, pageSize, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
    const int mprotectErrno = mprotectOK ? 0 : errno;

    // Step 1b: do NOT trust either call. Ask the kernel what the protection
    // actually is now. mmap lies by clamping, and mprotect can report success
    // while the region still lacks execute permission.
    vm_prot_t currentProtection = 0;
    vm_prot_t maximumProtection = 0;
    const bool queried = queryProtection(page, &currentProtection,
                                         &maximumProtection);
    char currentText[4] = "???";
    char maximumText[4] = "???";
    if (queried) {
        formatProtection(currentProtection, currentText);
        formatProtection(maximumProtection, maximumText);
    }

    if (!queried || (currentProtection & VM_PROT_EXECUTE) == 0) {
        munmap(page, pageSize);
        report.status = BVNJITStatusUnavailable;
        report.executableMemoryAvailable = false;
        if (!report.debuggerAttached) {
            setDetail("The page is not executable (protection %s, max %s)%s. "
                      "The kernel does not have this process flagged as "
                      "debugged (CS_DEBUGGED is not set) - no JIT enabler has "
                      "attached yet, or it detached again. Attach StikDebug "
                      "(or an equivalent JIT enabler) to THIS app while it is "
                      "running, then check again; BoxedVN cannot enable JIT "
                      "by itself.",
                      currentText, maximumText,
                      mprotectOK ? "" : " and mprotect(PROT_EXEC) failed");
        } else if (!mprotectOK) {
            setDetail("mprotect(PROT_EXEC) failed: %s (errno %d), even though "
                      "the kernel DOES have this process flagged as debugged "
                      "(CS_DEBUGGED is set). Region protection is %s, max %s. "
                      "A JIT enabler attached, but this process still cannot "
                      "obtain executable memory - check that the installed "
                      "IPA kept the 'get-task-allow' entitlement through "
                      "signing.",
                      strerror(mprotectErrno), mprotectErrno, currentText,
                      maximumText);
        } else {
            setDetail("mprotect(PROT_EXEC) reported success, but the region's "
                      "actual protection is %s (max %s) - the execute bit was "
                      "not granted. Refusing to jump into a non-executable "
                      "page; doing so is an instruction-abort permission "
                      "fault, not a recoverable error.",
                      currentText, maximumText);
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
    setDetail("Executable memory obtained (mmap rw- then mprotect to %s), "
              "written, cache-flushed and executed successfully. Boxedwine's "
              "ARM64 JIT can run.",
              currentText);
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
