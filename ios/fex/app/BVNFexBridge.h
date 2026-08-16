/*
 *  BoxedVN fex64 - bridge between the application and FEX.
 *  Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
 *
 *  Ladder A of docs/ARCHITECTURE_FEX64.md. The question this answers is not
 *  "can this process obtain an executable page" - the `ios` branch settled
 *  that - but "does a translator's generated code run from a debugger-prepared
 *  page on this device".
 *
 *  The executable memory comes from BVNExecMemory, unchanged: one arena
 *  prepared by StikDebug while it is attached, written through a separate
 *  writable alias while the address FEX branches to stays r-x.
 */

#ifndef BVN_FEX_BRIDGE_H
#define BVN_FEX_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// How far the stack got. Values are stable so they can appear in logs.
typedef enum {
    /// Nothing ran yet.
    BVNFexStageIdle = 0,
    /// The executable arena is prepared and a pool was taken from it.
    BVNFexStageArenaReady = 1,
    /// FEX's allocator hooks are pointed at that pool.
    BVNFexStageHooksInstalled = 2,
    /// A FEXCore context exists.
    BVNFexStageContextCreated = 3,
    /// Translated x86-64 executed from the arena and returned what it should.
    BVNFexStageExecuted = 4,
} BVNFexStage;

/// Runs as far as it can and stops at the first thing that fails. Safe to call
/// more than once; it does not repeat work that already succeeded.
///
/// Executes generated code, so it must not be called from anywhere that has to
/// stay alive if that goes wrong - a status refresh, say. It belongs behind a
/// deliberate action.
BVNFexStage BVNFexProbe(void);

/// The furthest stage reached so far, without running anything.
BVNFexStage BVNFexStageReached(void);

/// A human-readable account of every step, accumulated across calls. Never
/// NULL, and safe to read at any time.
const char* BVNFexReport(void);

/// What the probe is doing right now, or "" when it is not running. Safe to
/// read from any thread while BVNFexProbe is in progress - which is the point:
/// the steps that talk to the debugger can block indefinitely, so an interface
/// that can only report after they return cannot report a hang at all.
const char* BVNFexCurrentStep(void);

/// Seconds the current step has been running, or 0 when idle.
double BVNFexCurrentStepSeconds(void);

/// Name of a stage, for the interface and for logs.
const char* BVNFexStageName(BVNFexStage stage);

/// Bytes of the executable arena handed to FEX, and how much of it the
/// translator has taken. Either pointer may be NULL. False before the pool
/// exists.
bool BVNFexPoolStatus(size_t* poolBytes, size_t* usedBytes);

/// Distance from the executable view of the arena pool to its writable alias.
/// The ARM64EC translator publishes this to its own FEXCore instance when Wine
/// snapshots the process environment. Returns zero until the arena exists.
int64_t BVNFexWriteOffset(void);

#ifdef __cplusplus
}
#endif

#endif /* BVN_FEX_BRIDGE_H */
