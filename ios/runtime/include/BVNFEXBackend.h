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

typedef struct BVNFEXCPU64Adapter BVNFEXCPU64Adapter;

typedef enum {
    BVNFEXCPU64AdapterActionInvalid = -1,
    BVNFEXCPU64AdapterActionContinue = 0,
    BVNFEXCPU64AdapterActionYield = 1,
    BVNFEXCPU64AdapterActionExec = 2,
    BVNFEXCPU64AdapterActionThreadExit = 3,
    BVNFEXCPU64AdapterActionProcessExit = 4,
} BVNFEXCPU64AdapterAction;

// True only when the FEX iPhoneOS archives are linked into this app.
bool BVNFEXBackendBuilt(void);

// Deliberate device-only self-test. It executes an x86-64 Linux PIE containing
// the loader's SSE2 string-mask loop and guest call/return flow through FEX,
// then requires write/exit to return through BoxedWine's CPU64/KMemory64
// syscall path. It does not claim Wine64 is ready.
BVNFEXBackendStage BVNFEXBackendProbe(void);
BVNFEXBackendStage BVNFEXBackendStageReached(void);
const char* BVNFEXBackendStageName(BVNFEXBackendStage stage);
const char* BVNFEXBackendReport(void);

// Samples the active FEX host thread without relying on UIKit's main run loop.
// Safe from BoxedVN's detached diagnostics watcher; a non-FEX build is a
// no-op. Output is throttled and repeated stable samples produce one bounded
// stall line rather than flooding the session log.
void BVNFEXBackendPollExecutionTrace(void);

// The adapter borrows a live KProcess/KThread and their CPU64 state. The
// caller must keep both objects alive and must leave the adapter before the
// thread exits. execve is handled by the syscall path while the adapter is
// active; the caller must not detach until that callback returns. It never
// creates or owns KMemory64, CPU64, KProcess, or KThread objects.
BVNFEXCPU64Adapter* BVNFEXCPU64AdapterAttach(void* process, void* thread);
void BVNFEXCPU64AdapterDetach(BVNFEXCPU64Adapter* adapter);
bool BVNFEXCPU64AdapterEnter(BVNFEXCPU64Adapter* adapter);
void BVNFEXCPU64AdapterLeave(BVNFEXCPU64Adapter* adapter);
BVNFEXCPU64Adapter* BVNFEXCPU64AdapterCurrent(void);

// Bind the adapter to the FEX context/thread that owns the opaque frame
// passed to the sync functions. The adapter does not retain ownership.
bool BVNFEXCPU64AdapterBindFEX(BVNFEXCPU64Adapter* adapter,
                               void* context,
                               void* fexThread);

// `frame` is FEXCore::Core::CpuStateFrame*. The opaque type keeps the public
// C ABI independent of the optional FEX headers while retaining a complete
// architectural handoff in the implementation.
bool BVNFEXCPU64AdapterSyncFromFEX(BVNFEXCPU64Adapter* adapter, void* frame);
bool BVNFEXCPU64AdapterSyncToFEX(BVNFEXCPU64Adapter* adapter, void* frame);
// Handle one Darwin SA_SIGINFO fault while FEX is executing this adapter.
// `signalConfig` points to FEXCore::SignalDelegatorConfig in the implementation
// and is intentionally opaque here. Returns true only when the ucontext has
// been repaired (including a translated guest signal frame) and may resume.
bool BVNFEXCPU64AdapterHandleHostFault(BVNFEXCPU64Adapter* adapter,
                                        const void* signalConfig,
                                        int signal,
                                        void* info,
                                        void* ucontext);
bool BVNFEXCPU64AdapterQueryExecutableRange(BVNFEXCPU64Adapter* adapter,
                                            uint64_t address,
                                            uint64_t* base,
                                            uint64_t* size,
                                            bool* writable);
void BVNFEXCPU64AdapterResetAction(BVNFEXCPU64Adapter* adapter);

// `arguments` contains the Linux x86-64 syscall number followed by its six
// register arguments, matching FEXCore::HLE::SyscallArguments::Argument.
uint64_t BVNFEXCPU64AdapterHandleSyscall(BVNFEXCPU64Adapter* adapter,
                                         void* frame,
                                         const uint64_t* arguments);
BVNFEXCPU64AdapterAction BVNFEXCPU64AdapterLastAction(
    const BVNFEXCPU64Adapter* adapter);

// Execute one live BoxedWine-owned x86-64 process thread through FEX. Returns
// false when the process is not a native-identity 64-bit process or FEX is not
// linked/initialised. A true return means the thread stopped at a syscall
// boundary, yield, exec replacement, or normal FEX return.
bool BVNFEXCPU64Run(void* process, void* thread);

#ifdef __cplusplus
}
#endif

#endif
