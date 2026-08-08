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
 *  Executable memory for the JIT, on a platform that will not say how to
 *  get it.
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
 *  Guessing a fourth time is not a strategy.  This module instead probes
 *  several allocation strategies in one pass, records what the kernel
 *  actually did for each - read back with vm_region_64, not assumed from
 *  return codes, all of which have lied at least once - and then uses
 *  whichever one genuinely produced an executable page.
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
    /// mmap(rwx) came back executable as asked.  No W^X dance needed.
    BVNExecMemStrategyMmapRWX = 1,
    /// mmap(rwx) then mprotect(rwx); the page stays writable AND executable.
    BVNExecMemStrategyMprotectRWX = 2,
    /// mmap(rwx, MAP_JIT) came back executable.  No W^X dance needed.
    BVNExecMemStrategyMapJitRWX = 3,
    /// mmap(rwx, MAP_JIT) then mprotect(r-x); writes need a flip to rw-.
    BVNExecMemStrategyMapJitFlip = 4,
    /// mmap(rwx) then mprotect(r-x); writes need a flip to rw-.
    BVNExecMemStrategyMprotectFlip = 5,
    /// vm_allocate + vm_protect(r-x); writes need a flip to rw-.
    BVNExecMemStrategyMachFlip = 6,
} BVNExecMemStrategy;

/// Runs the strategy matrix once and caches the result; later calls are free.
///
/// `allowExecute` controls the one step that cannot be made safe: actually
/// calling into a page that the kernel *claims* is executable.  Every
/// protection check happens either way.  Pass false from anywhere that must
/// not risk the process (app launch, a status refresh); pass true only where
/// a caller has accepted that risk on purpose - see BVNRuntime.h.
///
/// Returns the strategy in use, or BVNExecMemStrategyNone.
BVNExecMemStrategy BVNExecMemProbe(bool allowExecute);

/// The result of the probe, one line per strategy, in the form
/// "name: cur/max -> action -> cur/max EXECUTABLE".  Written to the session
/// log as it is produced, so it survives even if a later step kills the
/// process.  Never NULL; reads "not probed yet" before the first probe.
const char* BVNExecMemReport(void);

/// True once a page has been mapped, written, made executable, called, and
/// seen to return the expected value.  Anything less than that is not proof.
bool BVNExecMemExecutionConfirmed(void);

/// Allocates `length` bytes (must be page-aligned) of memory the JIT can
/// execute from, using the strategy the probe selected.  Returns NULL if no
/// strategy works or the allocation fails; the caller decides how loudly to
/// fail.  Probes first if that has not happened yet, without executing.
void* BVNExecMemAlloc(size_t length);

void BVNExecMemFree(void* address, size_t length);

/// Makes a range temporarily writable, then executable again.
///
/// A no-op for strategies whose pages are writable and executable at the same
/// time.  Where it is not a no-op, note that this changes protection for the
/// whole process, unlike macOS's per-thread pthread_jit_write_protect_np: a
/// thread executing from these pages while another thread writes to them can
/// fault.  See docs/KNOWN_LIMITATIONS_IOS.md.
void BVNExecMemSetWritable(void* address, size_t length, bool writable);

/// Whether BVNExecMemSetWritable does anything, i.e. whether the process-wide
/// W^X window above is a live concern for this device.
bool BVNExecMemNeedsWriteFlip(void);

/// Human-readable name of a strategy, for logs and the UI.
const char* BVNExecMemStrategyName(BVNExecMemStrategy strategy);

#ifdef __cplusplus
}
#endif

#endif  /* BVN_EXEC_MEMORY_H */
