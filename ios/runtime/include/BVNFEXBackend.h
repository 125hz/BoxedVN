/*
 * BoxedVN - optional FEX CPU backend diagnostics.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BVN_FEX_BACKEND_H
#define BVN_FEX_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BVNFEXBackendStageUnavailable = 0,
    BVNFEXBackendStageIdle = 1,
    BVNFEXBackendStageArenaReady = 2,
    BVNFEXBackendStageContextReady = 3,
    BVNFEXBackendStageKernelEntered = 4,
    BVNFEXBackendStageExecuted = 5,
} BVNFEXBackendStage;

// True only when the FEX iPhoneOS archives are linked into this app.
bool BVNFEXBackendBuilt(void);

// Deliberate device-only self-test. It executes an x86-64 Linux instruction
// stream through FEX and requires its write/exit syscalls to return through
// BoxedWine's CPU64/KMemory64 syscall path. It does not claim Wine64 is ready.
BVNFEXBackendStage BVNFEXBackendProbe(void);
BVNFEXBackendStage BVNFEXBackendStageReached(void);
const char* BVNFEXBackendStageName(BVNFEXBackendStage stage);
const char* BVNFEXBackendReport(void);

#ifdef __cplusplus
}
#endif

#endif
