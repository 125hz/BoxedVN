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
 *  Executable memory for the JIT on iOS 26 and newer.
 *
 *  Three separate attempts at this have now failed on device, each for a
 *  different reason, and each failure was only discoverable by shipping a
 *  build and reading what came back:
 *
 *    1. mmap(PROT_EXEC) returns a valid pointer and a page that is not
 *       executable.  It does not fail.
 *    2. mprotect(PROT_READ | PROT_WRITE | PROT_EXEC) returns 0 and withholds
 *       the execute bit, because iOS enforces W^X.
 *    3. Dropping PROT_EXEC from the mmap to satisfy (2) caps the region's
 *       *maximum* protection at rw-, after which no mprotect can ever add
 *       execute.
 *
 *  The missing step was outside the process.  On iOS 26/27, CS_DEBUGGED is
 *  necessary but does not make a page executable by itself.  The app must
 *  issue StikDebug's universal JIT-region request (x16=1, brk #0xf00d), and
 *  StikDebug/debugserver must prepare every 16 KiB page.  Code is then
 *  written through a separate rw- alias of the same physical pages while the
 *  address returned to Boxedwine remains r-x.  This is the same TXM-aware
 *  design used by current iOS emulator ports.
 *
 *  Both the diagnostic (BVNJITProbeExecute) and the live allocator
 *  (Platform::alloc64kBlock on iOS) go through here, so the thing that gets
 *  measured is exactly the thing that gets used.  Keeping those two in sync
 *  by hand is what made mistake (3) possible.
 *  ---------------------------------------------------------------------
 */

#ifndef BVN_EXEC_MEMORY_H
#define BVN_EXEC_MEMORY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// How executable memory is obtained on this device.  Values are stable so
/// they can appear in logs and bug reports.
typedef enum {
    /// Nothing worked; the JIT cannot run.
    BVNExecMemStrategyNone = 0,
    /// Retired unsafe strategy value retained for old logs.
    BVNExecMemStrategyMmapRWX = 1,
    /// Retired unsafe strategy value retained for old logs.
    BVNExecMemStrategyMprotectRWX = 2,
    /// Retired unsupported strategy value retained for old logs.
    BVNExecMemStrategyMapJitRWX = 3,
    /// Retired unsupported strategy value retained for old logs.
    BVNExecMemStrategyMapJitFlip = 4,
    /// Retired unsafe strategy value retained for old logs.
    BVNExecMemStrategyMprotectFlip = 5,
    /// vm_allocate + vm_protect(r-x); retained for log compatibility only.
    BVNExecMemStrategyMachFlip = 6,
    /// StikDebug prepares an r-x mapping; BoxedVN writes through an rw- alias.
    BVNExecMemStrategyStikDebugDualMap = 7,
} BVNExecMemStrategy;

/// Runs the StikDebug/TXM handshake once and caches the result; later calls
/// are free.
///
/// `allowExecute` controls the one step that cannot be made safe: actually
/// calling into the newly prepared arena. Pass false from anywhere that must not
/// issue StikDebug's breakpoint request (app launch, a status refresh); pass
/// true only from the deliberate guest-launch path - see BVNRuntime.h.
///
/// Returns the strategy in use, or BVNExecMemStrategyNone.
BVNExecMemStrategy BVNExecMemProbe(bool allowExecute);

/// A detailed result of the handshake, dual mapping, write and execution
/// test. Written to the session log as it is produced. Never NULL.
const char* BVNExecMemReport(void);

/// True once the arena has been mapped, written, made executable, called, and
/// seen to return the expected value. Anything less than that is not proof.
bool BVNExecMemExecutionConfirmed(void);

/// Suballocates `length` bytes (must be page-aligned) from the executable
/// arena prepared and retained by the probe. No debugger breakpoint is issued
/// here. Returns NULL if the probe failed or the bounded arena is exhausted.
void* BVNExecMemAlloc(size_t length);

/// Current arena totals, summed over the prepared segments. Any output
/// pointer may be NULL. Returns false when no segment was ever prepared,
/// which is how a caller tells "the JIT never started" apart from "the JIT
/// started and ran out of room" - two failures that need opposite advice.
bool BVNExecMemArenaStatus(size_t* capacityBytes, size_t* availableBytes,
                           size_t* segmentCount);

void BVNExecMemFree(void* address, size_t length);

/// Returns the writable alias for an executable range.  `address` remains
/// the r-x pointer used for calls and instruction-cache invalidation; all
/// generated-code writes must target the pointer returned here.
void* BVNExecMemWritableAddress(void* address, size_t length);

/// Returns an allocation to the retained arena when `address` belongs to this
/// allocator. The arena's dual mapping remains alive for the process.
bool BVNExecMemReleaseIfOwned(void* address, size_t length);

/// Legacy compatibility entry point. The dual-mapping implementation never
/// changes the executable mapping's protection, so this is now a no-op.
void BVNExecMemSetWritable(void* address, size_t length, bool writable);

/// Always false for the dual-mapping implementation.
bool BVNExecMemNeedsWriteFlip(void);

/// Human-readable name of a strategy, for logs and the UI.
const char* BVNExecMemStrategyName(BVNExecMemStrategy strategy);

#ifdef __cplusplus
}
#endif

#endif  /* BVN_EXEC_MEMORY_H */
