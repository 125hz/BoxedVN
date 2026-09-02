/*
 * BoxedVN - optional FEX CPU64 process/thread adapter.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#include "BVNFEXBackend.h"

#if !defined(BOXEDVN_ENABLE_FEX64) || !defined(BOXEDWINE_GUEST_X64)

extern "C" BVNFEXCPU64Adapter* BVNFEXCPU64AdapterAttach(void*, void*) { return nullptr; }
extern "C" void BVNFEXCPU64AdapterDetach(BVNFEXCPU64Adapter*) {}
extern "C" bool BVNFEXCPU64AdapterEnter(BVNFEXCPU64Adapter*) { return false; }
extern "C" void BVNFEXCPU64AdapterLeave(BVNFEXCPU64Adapter*) {}
extern "C" BVNFEXCPU64Adapter* BVNFEXCPU64AdapterCurrent(void) { return nullptr; }
extern "C" bool BVNFEXCPU64AdapterBindFEX(BVNFEXCPU64Adapter*, void*, void*) {
    return false;
}
extern "C" bool BVNFEXCPU64AdapterSyncFromFEX(BVNFEXCPU64Adapter*, void*) { return false; }
extern "C" bool BVNFEXCPU64AdapterSyncToFEX(BVNFEXCPU64Adapter*, void*) { return false; }
extern "C" bool BVNFEXCPU64AdapterHandleHostFault(BVNFEXCPU64Adapter*, const void*,
                                                    int, void*, void*) { return false; }
extern "C" bool BVNFEXCPU64AdapterQueryExecutableRange(BVNFEXCPU64Adapter*, uint64_t,
                                                         uint64_t*, uint64_t*, bool*) { return false; }
extern "C" void BVNFEXCPU64AdapterResetAction(BVNFEXCPU64Adapter*) {}
extern "C" uint64_t BVNFEXCPU64AdapterHandleSyscall(BVNFEXCPU64Adapter*, void*, const uint64_t*) { return 0; }
extern "C" BVNFEXCPU64AdapterAction BVNFEXCPU64AdapterLastAction(const BVNFEXCPU64Adapter*) {
    return BVNFEXCPU64AdapterActionInvalid;
}
extern "C" void BVNFEXBackendPublishPendingIRCapTarget(uint64_t) {}
extern "C" uint64_t BVNFEXBackendTakePendingIRCapTarget(void) { return 0; }

#else

#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>

#ifdef FSCALE
#undef FSCALE
#endif
#include "boxedwine.h"
#include "boxedvn/fex_exit_dispatch_contract.h"
#include "cpu64.h"
#include "fex64loaderhandoff.h"
#include "kmemory64.h"
#include "kprocess.h"
#include "kthread.h"
#include "syscall64.h"
#include "wine_nt_syscall_memory.h"
#include "BVNFEXBackend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstddef>

#if defined(__APPLE__) && defined(__aarch64__)
#include <signal.h>
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE
#endif
#include <ucontext.h>
#include <mach/mach.h>
#include <cstdio>
#endif

extern "C" bool BVNFEXBackendOwnsHostCodeAddress(uint64_t address);

struct BVNFEXCPU64Adapter {
    KProcess* process = nullptr;
    KThread* thread = nullptr;
    CPU64* cpu = nullptr;
    FEXCore::Context::Context* context = nullptr;
    FEXCore::Core::InternalThreadState* fexThread = nullptr;
    BVNFEXCPU64AdapterAction lastAction = BVNFEXCPU64AdapterActionInvalid;
};

static thread_local BVNFEXCPU64Adapter* gCurrentAdapter = nullptr;
static std::atomic<uint32_t> gLiveSyscallTraceOrdinal {0};
static std::atomic<uint32_t> gSyscallRedirectReports {0};

// FEX stores x87 state in a stack-relative view (the MM array is indexed by
// physical stack slot, while AbridgedFTW bits are indexed by ST(i)). CPU64's
// soft FPU has the same physical register representation, so the bridge can
// preserve it without inventing a second floating-point ABI. CPU64 currently
// has no MXCSR member; its 0F AE handlers are deliberately no-ops, so there is
// no MXCSR state to copy here.
static void syncFPUFromFEX(CPU64* cpu, const FEXCore::Core::CPUState& state) {
    if (!cpu) return;
    cpu->fpu.SetCW(state.FCW);

    uint16_t status = 0;
    constexpr uint8_t exceptionBits[] = {
        FEXCore::X86State::X87FLAG_IE_LOC,
        FEXCore::X86State::X87FLAG_DE_LOC,
        FEXCore::X86State::X87FLAG_ZE_LOC,
        FEXCore::X86State::X87FLAG_OE_LOC,
        FEXCore::X86State::X87FLAG_UE_LOC,
        FEXCore::X86State::X87FLAG_PE_LOC,
    };
    constexpr uint16_t exceptionMasks[] = {
        FPU_SW_IE, FPU_SW_DE, FPU_SW_ZE, FPU_SW_OE, FPU_SW_UE, FPU_SW_PE,
    };
    for (size_t i = 0; i < std::size(exceptionBits); ++i) {
        if (state.flags[exceptionBits[i]] & 1u) status |= exceptionMasks[i];
    }
    if (state.flags[FEXCore::X86State::X87FLAG_ES_LOC] & 1u)
        status |= FPU_SW_ES;
    if (state.flags[FEXCore::X86State::X87FLAG_C0_LOC] & 1u)
        status |= 0x0100;
    if (state.flags[FEXCore::X86State::X87FLAG_C1_LOC] & 1u)
        status |= 0x0200;
    if (state.flags[FEXCore::X86State::X87FLAG_C2_LOC] & 1u)
        status |= 0x0400;
    if (state.flags[FEXCore::X86State::X87FLAG_C3_LOC] & 1u)
        status |= 0x4000;
    const uint16_t top =
        static_cast<uint16_t>((state.flags[FEXCore::X86State::X87FLAG_TOP_LOC] & 1u) |
                              ((state.flags[FEXCore::X86State::X87FLAG_TOP_LOC + 1] & 1u) << 1) |
                              ((state.flags[FEXCore::X86State::X87FLAG_TOP_LOC + 2] & 1u) << 2));
    status |= static_cast<uint16_t>((top & 7u) << 11);
    if (state.flags[FEXCore::X86State::X87FLAG_B_LOC] & 1u)
        status |= 0x8000;
    cpu->fpu.SetSW(status);

    for (uint32_t stackIndex = 0; stackIndex < 8; ++stackIndex) {
        const uint32_t physical = (stackIndex - top) & 7u;
        cpu->fpu.regs[physical].signif = state.mm[physical][0];
        cpu->fpu.regs[physical].signExp =
            static_cast<uint16_t>(state.mm[physical][1]);
        cpu->fpu.tags[physical] =
            (state.AbridgedFTW & (1u << stackIndex)) ? TAG_Valid : TAG_Empty;
        cpu->fpu.isRegCached[physical] = false;
    }
    cpu->fpu.isMMXInUse = false;
}

static void syncFEXFromFPU(FEXCore::Core::CPUState& state, CPU64* cpu) {
    if (!cpu) return;
    state.FCW = static_cast<uint16_t>(cpu->fpu.CW());
    const uint16_t status = static_cast<uint16_t>(cpu->fpu.SW());
    const uint8_t exceptionBits[] = {
        FEXCore::X86State::X87FLAG_IE_LOC,
        FEXCore::X86State::X87FLAG_DE_LOC,
        FEXCore::X86State::X87FLAG_ZE_LOC,
        FEXCore::X86State::X87FLAG_OE_LOC,
        FEXCore::X86State::X87FLAG_UE_LOC,
        FEXCore::X86State::X87FLAG_PE_LOC,
    };
    const uint16_t exceptionMasks[] = {
        FPU_SW_IE, FPU_SW_DE, FPU_SW_ZE, FPU_SW_OE, FPU_SW_UE, FPU_SW_PE,
    };
    for (size_t i = 0; i < std::size(exceptionBits); ++i)
        state.flags[exceptionBits[i]] = (status & exceptionMasks[i]) ? 1 : 0;
    state.flags[FEXCore::X86State::X87FLAG_ES_LOC] =
        (status & FPU_SW_ES) ? 1 : 0;
    state.flags[FEXCore::X86State::X87FLAG_C0_LOC] = (status >> 8) & 1;
    state.flags[FEXCore::X86State::X87FLAG_C1_LOC] = (status >> 9) & 1;
    state.flags[FEXCore::X86State::X87FLAG_C2_LOC] = (status >> 10) & 1;
    state.flags[FEXCore::X86State::X87FLAG_C3_LOC] = (status >> 14) & 1;
    const uint32_t top = (status >> 11) & 7u;
    state.flags[FEXCore::X86State::X87FLAG_TOP_LOC] = top & 1u;
    state.flags[FEXCore::X86State::X87FLAG_TOP_LOC + 1] = (top >> 1) & 1u;
    state.flags[FEXCore::X86State::X87FLAG_TOP_LOC + 2] = (top >> 2) & 1u;
    state.flags[FEXCore::X86State::X87FLAG_B_LOC] = (status >> 15) & 1;

    state.AbridgedFTW = 0;
    for (uint32_t stackIndex = 0; stackIndex < 8; ++stackIndex) {
        const uint32_t physical = (stackIndex - top) & 7u;
        state.mm[physical][0] = cpu->fpu.regs[physical].signif;
        state.mm[physical][1] = cpu->fpu.regs[physical].signExp;
        if (cpu->fpu.tags[physical] != TAG_Empty)
            state.AbridgedFTW |= static_cast<uint8_t>(1u << stackIndex);
    }
}

static bool validAdapter(const BVNFEXCPU64Adapter* adapter) {
    if (!adapter || !adapter->process || !adapter->thread || !adapter->cpu) {
        return false;
    }
    if (!adapter->process->is64Bit || !adapter->process->memory64 ||
        !adapter->process->memory64->nativeIdentityMode()) {
        return false;
    }
    if (!adapter->thread->process ||
        adapter->thread->process.get() != adapter->process) {
        return false;
    }
    CPU64* expected = adapter->thread->cpu64 ? adapter->thread->cpu64
                                             : adapter->process->cpu64;
    return expected == adapter->cpu && adapter->cpu->memory == adapter->process->memory64 &&
           adapter->cpu->thread == adapter->thread && adapter->context != nullptr &&
           adapter->fexThread != nullptr &&
           adapter->fexThread->CTX == adapter->context;
}

extern "C" BVNFEXCPU64Adapter* BVNFEXCPU64AdapterAttach(void* processPointer,
                                                          void* threadPointer) {
    auto* process = static_cast<KProcess*>(processPointer);
    auto* thread = static_cast<KThread*>(threadPointer);
    if (!process || !thread || !process->is64Bit || !process->memory64 ||
        !thread->process || thread->process.get() != process) {
        return nullptr;
    }
    CPU64* cpu = thread->cpu64 ? thread->cpu64 : process->cpu64;
    if (!cpu || cpu->memory != process->memory64 || cpu->thread != thread) {
        return nullptr;
    }
    BVNFEXCPU64Adapter* adapter = nullptr;
    try {
        adapter = new BVNFEXCPU64Adapter;
    } catch (...) {
        return nullptr;
    }
    adapter->process = process;
    adapter->thread = thread;
    adapter->cpu = cpu;
    adapter->lastAction = BVNFEXCPU64AdapterActionContinue;
    return adapter;
}

extern "C" void BVNFEXCPU64AdapterDetach(BVNFEXCPU64Adapter* adapter) {
    if (gCurrentAdapter == adapter) {
        gCurrentAdapter = nullptr;
    }
    delete adapter;
}

extern "C" bool BVNFEXCPU64AdapterBindFEX(
    BVNFEXCPU64Adapter* adapter, void* contextPointer, void* fexThreadPointer) {
    if (!adapter || !contextPointer || !fexThreadPointer) return false;
    auto* context = static_cast<FEXCore::Context::Context*>(contextPointer);
    auto* fexThread = static_cast<FEXCore::Core::InternalThreadState*>(
        fexThreadPointer);
    if (fexThread->CTX != context || fexThread->CurrentFrame == nullptr) {
        return false;
    }
    adapter->context = context;
    adapter->fexThread = fexThread;
    return true;
}

extern "C" bool BVNFEXCPU64AdapterEnter(BVNFEXCPU64Adapter* adapter) {
    if (!validAdapter(adapter) || gCurrentAdapter != nullptr) {
        return false;
    }
    adapter->lastAction = BVNFEXCPU64AdapterActionContinue;
    gCurrentAdapter = adapter;
    return true;
}

extern "C" void BVNFEXCPU64AdapterLeave(BVNFEXCPU64Adapter* adapter) {
    if (gCurrentAdapter == adapter) {
        gCurrentAdapter = nullptr;
    }
}

extern "C" BVNFEXCPU64Adapter* BVNFEXCPU64AdapterCurrent(void) {
    return gCurrentAdapter;
}

extern "C" void BVNFEXCPU64AdapterResetAction(BVNFEXCPU64Adapter* adapter) {
    if (adapter) {
        adapter->lastAction = BVNFEXCPU64AdapterActionContinue;
    }
}

extern "C" bool BVNFEXCPU64AdapterQueryExecutableRange(
    BVNFEXCPU64Adapter* adapter, uint64_t address, uint64_t* base,
    uint64_t* size, bool* writable) {
    if (!validAdapter(adapter) || !base || !size || !writable) return false;
    KMemory64* memory = adapter->process->memory64;
    const uint64_t page = address >> K64_PAGE_SHIFT;
    const uint32_t executable = K64_PAGE_MAPPED | K64_PAGE_EXEC;
    if ((memory->getPageFlags(page) & executable) != executable) return false;

    uint64_t first = page;
    while (first &&
           (memory->getPageFlags(first - 1) & executable) == executable) {
        --first;
    }
    uint64_t last = page;
    while ((memory->getPageFlags(last + 1) & executable) == executable) {
        ++last;
    }
    *base = first << K64_PAGE_SHIFT;
    *size = (last - first + 1) << K64_PAGE_SHIFT;
    *writable = (memory->getPageFlags(page) & K64_PAGE_WRITE) != 0;
    return true;
}

extern "C" bool BVNFEXCPU64AdapterSyncFromFEX(BVNFEXCPU64Adapter* adapter,
                                                void* framePointer) {
    if (!validAdapter(adapter) || !framePointer) return false;
    auto* frame = static_cast<FEXCore::Core::CpuStateFrame*>(framePointer);
    if (frame != adapter->fexThread->CurrentFrame ||
        frame->Thread != adapter->fexThread) return false;
    CPU64* cpu = adapter->cpu;
    for (unsigned i = 0; i < X64_REG_COUNT; ++i) {
        cpu->reg[i].setU64(frame->State.gregs[i]);
    }
    cpu->rip = frame->State.rip;
    cpu->rflags = adapter->context->ReconstructCompactedEFLAGS(
        adapter->fexThread, false, nullptr, 0);
    cpu->fsbase = frame->State.fs_cached;
    cpu->gsbase = frame->State.gs_cached;
    std::array<__uint128_t, 16> xmmLow {};
    adapter->context->ReconstructXMMRegisters(
        adapter->fexThread, xmmLow.data(), nullptr);
    for (unsigned i = 0; i < 16; ++i) {
        std::memcpy(&cpu->xmm[i], &xmmLow[i], sizeof(cpu->xmm[i]));
    }
    syncFPUFromFEX(cpu, frame->State);
    return true;
}

extern "C" bool BVNFEXCPU64AdapterSyncToFEX(BVNFEXCPU64Adapter* adapter,
                                              void* framePointer) {
    if (!validAdapter(adapter) || !framePointer) return false;
    auto* frame = static_cast<FEXCore::Core::CpuStateFrame*>(framePointer);
    if (frame != adapter->fexThread->CurrentFrame ||
        frame->Thread != adapter->fexThread) return false;
    CPU64* cpu = adapter->cpu;
    for (unsigned i = 0; i < X64_REG_COUNT; ++i) {
        frame->State.gregs[i] = cpu->reg[i].u64;
    }
    frame->State.rip = cpu->rip;
    adapter->context->SetFlagsFromCompactedEFLAGS(
        adapter->fexThread, cpu->rflags);
    frame->State.fs_cached = cpu->fsbase;
    frame->State.gs_cached = cpu->gsbase;
    std::array<__uint128_t, 16> xmmLow {};
    for (unsigned i = 0; i < 16; ++i) {
        std::memcpy(&xmmLow[i], &cpu->xmm[i], sizeof(cpu->xmm[i]));
    }
    adapter->context->SetXMMRegistersFromState(
        adapter->fexThread, xmmLow.data(), nullptr);
    syncFEXFromFPU(frame->State, cpu);
    return true;
}

#if defined(__APPLE__) && defined(__aarch64__)
static bool isKuserAddress(uint64_t address) {
    return address >= K64_KUSER_SHARED_BASE &&
           address < K64_KUSER_SHARED_BASE + K64_KUSER_SHARED_SIZE;
}

static uint32_t hostSignalTrapNumber(int signal) {
    switch (signal) {
        case SIGSEGV: return 14; // #PF
        case SIGBUS: return 17;  // #AC/#BUS as exposed by the Darwin bridge
        case SIGILL: return 6;   // #UD
        case SIGFPE: return 16;  // #MF/#XM
        default: return 0;
    }
}

// A fault FEX generated on purpose names an exact guest instruction, which is
// the single most useful thing a device log can carry about an untranslatable
// opcode. Report a bounded number of them with the guest RIP and the bytes the
// decoder choked on so the offending instruction can be identified offline.
static void reportGeneratedGuestFault(
    BVNFEXCPU64Adapter* adapter, FEXCore::Core::CpuStateFrame* frame,
    const FEXCore::Core::CpuStateFrame::SynchronousFaultDataStruct& faultData) {
    static std::atomic<uint32_t> reported {0};
    if (reported.fetch_add(1, std::memory_order_relaxed) >= 32) return;
    const uint64_t rip = frame->State.rip;
    char bytes[64];
    bytes[0] = 0;
    size_t used = 0;
    KMemory64* memory = adapter->process ? adapter->process->memory64 : nullptr;
    if (memory && memory->nativeIdentityMode() &&
        memory->nativeGuestRangeAllowed(rip, 16)) {
        uint8_t instruction[16] = {};
        vm_size_t read = 0;
        if (vm_read_overwrite(mach_task_self(), rip, sizeof(instruction),
                              reinterpret_cast<vm_address_t>(instruction),
                              &read) == KERN_SUCCESS &&
            read == sizeof(instruction)) {
            for (size_t i = 0; i < sizeof(instruction) && used + 4 < sizeof(bytes);
                 ++i) {
                const int written = snprintf(bytes + used, sizeof(bytes) - used,
                                             "%s%02x", i == 0 ? "" : " ",
                                             instruction[i]);
                if (written <= 0) break;
                used += static_cast<size_t>(written);
            }
        }
    }
    klog_fmt("BOXEDWINE_FEX64_GUEST_FAULT pid=%d tid=%d signal=%u trapno=%u "
             "si_code=%u err=%u rip=0x%llx bytes=[%s]",
             adapter->process ? adapter->process->id : -1,
             adapter->thread ? adapter->thread->id : -1,
             (unsigned)faultData.Signal, (unsigned)faultData.TrapNo,
             (unsigned)faultData.si_code, (unsigned)faultData.err_code,
             (unsigned long long)rip, bytes[0] ? bytes : "unreadable");
}

static uint32_t hostSignalGuestNumber(int signal) {
    // CPU64 builds a Linux guest siginfo frame, so do not pass Darwin's
    // host-numbered SIGBUS (10) through as the Linux guest's SIGBUS (7).
    switch (signal) {
        case SIGSEGV: return 11;
        case SIGBUS: return 7;
        case SIGILL: return 4;
        case SIGFPE: return 8;
        default: return static_cast<uint32_t>(signal);
    }
}

// Read one guest qword through the SAME canonical-to-host translation the
// translator emits, without ever dereferencing the canonical address.
//
// The device fault report could say that a RET's target was zero but not
// whether the memory it came from was zero: the popped value and the memory it
// was popped from are the two facts that separate "the guest really returned to
// zero" from "the pop read the wrong place". This re-reads that memory
// independently.
//
// vm_read_overwrite is the same checked read the host-code capture above uses:
// it reports failure for an unmapped page instead of faulting, which is what
// makes this usable from a signal handler. Nothing here allocates or locks.
struct GuestStackWord {
    uint64_t guest = 0;
    uint64_t host = 0;
    uint64_t value = 0;
    bool read = false;
};

static GuestStackWord readGuestStackWord(BVNFEXCPU64Adapter* adapter,
                                         uint64_t guestAddress) {
    GuestStackWord word;
    word.guest = guestAddress;
    if (!adapter || !adapter->process || !adapter->process->memory64 ||
        guestAddress == 0) {
        return word;
    }
    // Translation only; the canonical address is never dereferenced.
    word.host = adapter->process->memory64->nativeIdentityMode()
        ? k64GuestToHostAddress(guestAddress)
        : 0;
    if (word.host == 0) {
        return word;
    }
    uint64_t value = 0;
    vm_size_t bytes = 0;
    word.read = vm_read_overwrite(
        mach_task_self(), static_cast<vm_address_t>(word.host), sizeof(value),
        reinterpret_cast<vm_address_t>(&value), &bytes) == KERN_SUCCESS &&
        bytes == sizeof(value);
    if (word.read) {
        word.value = value;
    }
    return word;
}

static bool recoverTranslatedLoaderHandoff(
    BVNFEXCPU64Adapter* adapter, FEXCore::Core::CpuStateFrame* frame,
    const FEXCore::Core::CpuStateFrame::SynchronousFaultDataStruct& faultData,
    ucontext_t* context, const FEXCore::SignalDelegatorConfig* config) {
    if (!adapter || !adapter->process || !adapter->process->memory64 || !frame ||
        !context || !context->uc_mcontext || !config ||
        !faultData.FaultToTopAndGeneratedException ||
        hostSignalGuestNumber(faultData.Signal) != 11 ||
        !adapter->process->memory64->nativeIdentityMode()) {
        return false;
    }

    std::array<uint8_t, 4> faultBytes {};
    vm_size_t bytesRead = 0;
    if (vm_read_overwrite(
            mach_task_self(), frame->State.rip, faultBytes.size(),
            reinterpret_cast<vm_address_t>(faultBytes.data()),
            &bytesRead) != KERN_SUCCESS || bytesRead != faultBytes.size()) {
        return false;
    }
    const auto resume = boxedvn::validatedFex64LoaderRunnerResume(
        frame->State.rip, K64_NATIVE_GUEST_INTERP_BASE,
        adapter->process->entry64, faultBytes,
        frame->ReturningStackLocation, config->ThreadStopHandlerAddress);
    if (!resume.has_value()) return false;

    const uint64_t previousRIP = frame->State.rip;
    frame->State.rip = resume->guestEntry;
    frame->SynchronousFaultData.FaultToTopAndGeneratedException = 0;
    frame->InSyscallInfo = 0;
    // The runner boundary restores the authoritative guest RIP, but the FEX
    // thread still owns dispatcher and call/return state from the faulted
    // loader epoch. Route through the existing exec reset before translating
    // the recovered program entry; reusing that epoch dispatches target zero.
    adapter->lastAction = resume->resetContext
                              ? BVNFEXCPU64AdapterActionExec
                              : BVNFEXCPU64AdapterActionContinue;
    auto* machine = context->uc_mcontext;
    machine->__ss.__x[1] = 0;
    machine->__ss.__x[28] = reinterpret_cast<uint64_t>(frame);
    machine->__ss.__sp = resume->hostStack;
    machine->__ss.__pc = resume->hostPC;
    klog_fmt("BOXEDWINE_FEX64_LOADER_HANDOFF_RECOVERED pid=%d tid=%d "
             "fault_rip=0x%llx entry=0x%llx resume=runner reset=context "
             "host_sp=0x%llx host_pc=0x%llx",
             adapter->process->id,
             adapter->thread ? adapter->thread->id : -1,
             (unsigned long long)previousRIP,
             (unsigned long long)resume->guestEntry,
             (unsigned long long)resume->hostStack,
             (unsigned long long)resume->hostPC);
    return machine->__ss.__pc != 0;
}

// Published from the host signal handler, consumed from ordinary code on the
// next live x86-64 context creation. A relaxed atomic is the whole mechanism:
// the signal path must not lock, allocate, or call into FEX, and the value has
// to outlive teardown of the guest process that faulted.
static std::atomic<uint64_t> gPendingIRCapTarget {0};
static std::atomic<uint32_t> gExactRIPReports {0};
static constexpr uint32_t kExactRIPReportLimit = 8;

extern "C" void BVNFEXBackendPublishPendingIRCapTarget(uint64_t guestRIP) {
    if (guestRIP == 0) return;
    gPendingIRCapTarget.store(guestRIP, std::memory_order_relaxed);
}

extern "C" uint64_t BVNFEXBackendTakePendingIRCapTarget(void) {
    return gPendingIRCapTarget.exchange(0, std::memory_order_relaxed);
}

// FEX's own reconstruction, exported by the pinned translator. Takes the block
// header recorded in the frame rather than the thread's current block, which
// may already have moved on by the time the fault is examined.
extern "C" uint64_t ios_fex_rip_from_hostpc(uint64_t BlockBegin, uint64_t HostPC);

static bool containUnclassifiedFEXFault(
    BVNFEXCPU64Adapter* adapter, const FEXCore::SignalDelegatorConfig* config,
    ucontext_t* context, int signal, uint64_t faultAddress,
    bool inCodeBuffer) {
    auto* frame = adapter->fexThread->CurrentFrame;
    auto* machine = context->uc_mcontext;
    const uint64_t hostPC = machine->__ss.__pc;
    const uint64_t returningStack = frame->ReturningStackLocation;
    if (returningStack == 0 || config->ThreadStopHandlerAddress == 0) {
        klog_fmt("BOXEDWINE_FEX64_HOST_FAULT_UNWIND_UNAVAILABLE pid=%d tid=%d "
                 "signal=%d host_pc=0x%llx address=0x%llx returning_stack=0x%llx "
                 "stop_handler=0x%llx spill_handler=0x%llx frame_thread=%p expected_thread=%p",
                 adapter->process ? adapter->process->id : -1,
                 adapter->thread ? adapter->thread->id : -1, signal,
                 (unsigned long long)hostPC,
                 (unsigned long long)faultAddress,
                 (unsigned long long)returningStack,
                 (unsigned long long)config->ThreadStopHandlerAddress,
                 (unsigned long long)config->ThreadStopHandlerAddressSpillSRA,
                 static_cast<void*>(frame->Thread),
                 static_cast<void*>(adapter->fexThread));
        return false;
    }

    uint8_t hostCode[16] = {};
    vm_size_t hostCodeBytes = 0;
    bool hostCodeValid = false;
    if (hostPC != 0) {
        hostCodeValid = vm_read_overwrite(
            mach_task_self(), hostPC, sizeof(hostCode),
            reinterpret_cast<vm_address_t>(hostCode),
            &hostCodeBytes) == KERN_SUCCESS &&
            hostCodeBytes == sizeof(hostCode);
    }
    char encoded[64] = {};
    size_t used = 0;
    if (hostCodeValid) {
        for (size_t i = 0; i < sizeof(hostCode) && used + 4 < sizeof(encoded);
             ++i) {
            const int written = snprintf(encoded + used, sizeof(encoded) - used,
                                         "%s%02x", i == 0 ? "" : " ",
                                         hostCode[i]);
            if (written <= 0) break;
            used += static_cast<size_t>(written);
        }
    }

    uint8_t writableCode[16] = {};
    vm_size_t writableCodeBytes = 0;
    bool writableCodeValid = false;
    const uint64_t writablePC =
        BVNFEXBackendWritableHostCodeAddress(hostPC);
    if (writablePC != 0) {
        writableCodeValid = vm_read_overwrite(
            mach_task_self(), writablePC, sizeof(writableCode),
            reinterpret_cast<vm_address_t>(writableCode),
            &writableCodeBytes) == KERN_SUCCESS &&
            writableCodeBytes == sizeof(writableCode);
    }
    char writableEncoded[64] = {};
    used = 0;
    if (writableCodeValid) {
        for (size_t i = 0; i < sizeof(writableCode) &&
             used + 4 < sizeof(writableEncoded); ++i) {
            const int written = snprintf(
                writableEncoded + used, sizeof(writableEncoded) - used,
                "%s%02x", i == 0 ? "" : " ", writableCode[i]);
            if (written <= 0) break;
            used += static_cast<size_t>(written);
        }
    }
    const bool aliasesMatch = hostCodeValid && writableCodeValid &&
        memcmp(hostCode, writableCode, sizeof(hostCode)) == 0;

    // Reconstruct the EXACT faulting guest instruction before anything here
    // rewrites the machine context or the frame. frame->State.rip is only the
    // block boundary the dispatcher last published, so disassembling the guest
    // image at that address can name a different instruction entirely. This
    // reads the block header recorded in the frame and asks FEX's own RIP
    // table, never RestoreRIPFromHostPC, whose thread-current block may have
    // moved on since the fault was recorded.
    const uint64_t blockBegin = frame->State.InlineJITBlockHeader;
    uint64_t exactGuestRIP = 0;
    if (inCodeBuffer && blockBegin != 0 && hostPC != 0) {
        exactGuestRIP = ios_fex_rip_from_hostpc(blockBegin, hostPC);
    }
    if (gExactRIPReports.load(std::memory_order_relaxed) <
        kExactRIPReportLimit) {
        gExactRIPReports.fetch_add(1, std::memory_order_relaxed);
        klog_fmt("BOXEDWINE_FEX64_EXACT_RIP pid=%d tid=%d signal=%d "
                 "frame_rip=0x%llx exact_guest_rip=0x%llx block_begin=0x%llx "
                 "host_pc=0x%llx in_buffer=%d",
                 adapter->process ? adapter->process->id : -1,
                 adapter->thread ? adapter->thread->id : -1, signal,
                 (unsigned long long)frame->State.rip,
                 (unsigned long long)exactGuestRIP,
                 (unsigned long long)blockBegin,
                 (unsigned long long)hostPC, inCodeBuffer ? 1 : 0);
    }
    // Publish only. Arming FEX's capture, creating a context, or touching the
    // translator in any way is not permitted on this path.
    if (exactGuestRIP != 0) {
        BVNFEXBackendPublishPendingIRCapTarget(exactGuestRIP);
    }

    const auto faultData = frame->SynchronousFaultData;
    klog_fmt("BOXEDWINE_FEX64_HOST_FAULT_CONTAINED pid=%d tid=%d signal=%d "
             "host_pc=0x%llx address=0x%llx in_buffer=%d guest_rip=0x%llx "
             "generated=%u guest_signal=%u trapno=%u err=%u host_bytes=[%s] "
             "rw_pc=0x%llx rw_bytes=[%s] aliases_match=%d",
             adapter->process ? adapter->process->id : -1,
             adapter->thread ? adapter->thread->id : -1, signal,
             (unsigned long long)hostPC,
             (unsigned long long)faultAddress, inCodeBuffer ? 1 : 0,
             (unsigned long long)frame->State.rip,
             (unsigned)faultData.FaultToTopAndGeneratedException,
             (unsigned)faultData.Signal, (unsigned)faultData.TrapNo,
             (unsigned)faultData.err_code,
             encoded[0] ? encoded : "unreadable",
             (unsigned long long)writablePC,
             writableEncoded[0] ? writableEncoded : "unreadable",
             aliasesMatch ? 1 : 0);

    // The faulting effective address alone cannot say whether a guest base
    // register was zero or the displacement was applied to the wrong base.
    // Report the architectural state FEX reconstructed from the ucontext,
    // plus the host registers the translated address arithmetic actually
    // used, so the next log can distinguish a bad lowering from a guest
    // pointer that was never populated.
    if (gExactRIPReports.load(std::memory_order_relaxed) <=
        kExactRIPReportLimit) {
        const auto& guest = frame->State;
        klog_fmt("BOXEDWINE_FEX64_FAULT_STATE pid=%d tid=%d rip=0x%llx "
                 "rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx rsi=0x%llx "
                 "rdi=0x%llx rsp=0x%llx rbp=0x%llx r12=0x%llx r13=0x%llx "
                 "fs_cached=0x%llx gs_cached=0x%llx address=0x%llx",
                 adapter->process ? adapter->process->id : -1,
                 adapter->thread ? adapter->thread->id : -1,
                 (unsigned long long)guest.rip,
                 (unsigned long long)guest.gregs[FEXCore::X86State::REG_RAX],
                 (unsigned long long)guest.gregs[FEXCore::X86State::REG_RBX],
                 (unsigned long long)guest.gregs[FEXCore::X86State::REG_RCX],
                 (unsigned long long)guest.gregs[FEXCore::X86State::REG_RDX],
                 (unsigned long long)guest.gregs[FEXCore::X86State::REG_RSI],
                 (unsigned long long)guest.gregs[FEXCore::X86State::REG_RDI],
                 (unsigned long long)guest.gregs[FEXCore::X86State::REG_RSP],
                 (unsigned long long)guest.gregs[FEXCore::X86State::REG_RBP],
                 (unsigned long long)guest.gregs[FEXCore::X86State::REG_R12],
                 (unsigned long long)guest.gregs[FEXCore::X86State::REG_R13],
                 (unsigned long long)guest.fs_cached,
                 (unsigned long long)guest.gs_cached,
                 (unsigned long long)faultAddress);
        // The translated address arithmetic lives in host registers at the
        // fault, and the static register allocation moves between blocks, so
        // record the whole general file rather than guessing which pair the
        // captured `add xN, xM, xK, lsl #2` used.
        char hostRegisters[512] = {};
        size_t hostUsed = 0;
        for (int i = 0; i < 29 && hostUsed + 24 < sizeof(hostRegisters); ++i) {
            const int written = snprintf(
                hostRegisters + hostUsed, sizeof(hostRegisters) - hostUsed,
                "%sx%d=0x%llx", i == 0 ? "" : " ", i,
                (unsigned long long)machine->__ss.__x[i]);
            if (written <= 0) break;
            hostUsed += static_cast<size_t>(written);
        }
        klog_fmt("BOXEDWINE_FEX64_FAULT_HOSTREGS pid=%d tid=%d host_pc=0x%llx "
                 "host_sp=0x%llx %s",
                 adapter->process ? adapter->process->id : -1,
                 adapter->thread ? adapter->thread->id : -1,
                 (unsigned long long)hostPC,
                 (unsigned long long)machine->__ss.__sp, hostRegisters);

        // The guest stack the faulting block was standing on, read back
        // independently of whatever the translated code loaded from it. A RET
        // whose target came out zero is only half an answer: this says whether
        // the memory it was popped from held zero as well, and at which host
        // address that memory actually lives.
        //
        // RSP-8 is the slot a RET has already consumed, RSP is the next one,
        // and RBP / RBP+8 are the saved frame pointer and the return address of
        // the frame LEAVE was about to unwind -- the exact three qwords the
        // device's `leave; mov eax, edx; ret` epilogue touches.
        const uint64_t guestRSP = guest.gregs[FEXCore::X86State::REG_RSP];
        const uint64_t guestRBP = guest.gregs[FEXCore::X86State::REG_RBP];
        const GuestStackWord stackWords[] = {
            readGuestStackWord(adapter, guestRSP - 16),
            readGuestStackWord(adapter, guestRSP - 8),
            readGuestStackWord(adapter, guestRSP),
            readGuestStackWord(adapter, guestRSP + 8),
            readGuestStackWord(adapter, guestRBP),
            readGuestStackWord(adapter, guestRBP + 8),
        };
        static const char* const stackLabels[] = {
            "rsp-16", "rsp-8", "rsp", "rsp+8", "rbp", "rbp+8",
        };
        char stackSnapshot[640] = {};
        size_t stackUsed = 0;
        for (size_t i = 0; i < sizeof(stackWords) / sizeof(stackWords[0]) &&
             stackUsed + 96 < sizeof(stackSnapshot); ++i) {
            const int written = snprintf(
                stackSnapshot + stackUsed, sizeof(stackSnapshot) - stackUsed,
                "%s%s=[guest=0x%llx host=0x%llx read=%d value=0x%llx]",
                i == 0 ? "" : " ", stackLabels[i],
                (unsigned long long)stackWords[i].guest,
                (unsigned long long)stackWords[i].host,
                stackWords[i].read ? 1 : 0,
                (unsigned long long)stackWords[i].value);
            if (written <= 0) break;
            stackUsed += static_cast<size_t>(written);
        }
        klog_fmt("BOXEDWINE_FEX64_FAULT_STACK pid=%d tid=%d rsp=0x%llx "
                 "rbp=0x%llx native=%d %s",
                 adapter->process ? adapter->process->id : -1,
                 adapter->thread ? adapter->thread->id : -1,
                 (unsigned long long)guestRSP, (unsigned long long)guestRBP,
                 (adapter->process && adapter->process->memory64 &&
                  adapter->process->memory64->nativeIdentityMode()) ? 1 : 0,
                 stackSnapshot);
    }

    // This is an active FEX thread and the fault is either in BoxedVN's
    // executable pool or the result of a translated branch to address zero.
    // Do not let Darwin terminate the whole app. Stop only this guest through
    // the dispatcher's normal ExecuteThread unwind; the detailed marker above
    // retains the evidence needed to fix the translator path itself.
    const uint32_t guestSignal = hostSignalGuestNumber(signal);
    if (adapter->process) {
        adapter->process->signalProcess(guestSignal);
    }
    adapter->cpu->yield = true;
    // Fatal, not a guest exit. The guest never asked to stop and nothing has
    // published an exit status for it; reporting this as ProcessExit is what
    // left the process alive with no status while the session waited.
    adapter->lastAction = BVNFEXCPU64AdapterActionFatalExit;
    frame->InSyscallInfo = 0;
    machine->__ss.__x[28] = reinterpret_cast<uint64_t>(frame);
    machine->__ss.__sp = returningStack;
    machine->__ss.__pc = config->ThreadStopHandlerAddress;
    return true;
}
#endif

extern "C" bool BVNFEXCPU64AdapterHandleHostFault(
    BVNFEXCPU64Adapter* adapter, const void* signalConfigPointer, int signal,
    void* infoPointer, void* ucontextPointer) {
#if !defined(__APPLE__) || !defined(__aarch64__)
    (void)adapter;
    (void)signalConfigPointer;
    (void)signal;
    (void)infoPointer;
    (void)ucontextPointer;
    return false;
#else
    if (!validAdapter(adapter) || !signalConfigPointer || !infoPointer ||
        !ucontextPointer) {
        if (adapter) {
            klog_fmt("BOXEDWINE_FEX64_HOST_FAULT_ADAPTER_REJECTED signal=%d "
                     "config=%p info=%p ucontext=%p",
                     signal, signalConfigPointer, infoPointer, ucontextPointer);
        }
        return false;
    }

    const auto* config = static_cast<const FEXCore::SignalDelegatorConfig*>(
        signalConfigPointer);
    auto* context = static_cast<ucontext_t*>(ucontextPointer);
    if (!context->uc_mcontext || !adapter->fexThread->CurrentFrame ||
        adapter->fexThread->CurrentFrame->Thread != adapter->fexThread) {
        return false;
    }
    auto* machine = context->uc_mcontext;
    auto* siginfo = static_cast<siginfo_t*>(infoPointer);
    const uint64_t hostPC = machine->__ss.__pc;
    const uint64_t faultAddress = reinterpret_cast<uint64_t>(siginfo->si_addr);
    const bool inCodeBuffer =
        adapter->context->IsAddressInCodeBuffer(adapter->fexThread, hostPC);
    // FEX reports a synchronous guest fault (an invalid or unimplemented x86
    // instruction, a guest #GP, an int3) by writing SynchronousFaultData into
    // the frame and branching to one of the dispatcher's GuestSignal_* stubs.
    // Those stubs spill the static registers and then deliberately execute an
    // instruction that traps -- `hlt` for SIGILL, `brk` for SIGTRAP, a null
    // load for SIGSEGV -- so the fault arrives here with a PC inside the
    // dispatcher rather than inside a translated block. Without this branch the
    // trap is declined, nothing else in the process knows what it means, and
    // returning from the handler simply re-executes it: the guest thread then
    // spins in host signal delivery forever, which is what a stalled x64 guest
    // pinned at a constant host PC actually is.
    const auto& faultData = adapter->fexThread->CurrentFrame->SynchronousFaultData;
    const bool inDispatcher = !inCodeBuffer && config->DispatcherEnd > config->DispatcherBegin &&
        hostPC >= config->DispatcherBegin && hostPC < config->DispatcherEnd;
    const bool generatedException =
        inDispatcher && faultData.FaultToTopAndGeneratedException != 0;
    // A BoxedWine process may have ordinary host threads and native code in the
    // same address space. Only consume a fault whose PC belongs to this FEX
    // thread's translated code or to a fault FEX generated on purpose; every
    // other fault chains to the prior action.
    if (!inCodeBuffer && !generatedException) {
        const bool inExecutablePool =
            BVNFEXBackendOwnsHostCodeAddress(hostPC);
        const bool translatedNullBranch =
            signal == SIGSEGV && hostPC == 0 && faultAddress == 0;
        if ((inExecutablePool || translatedNullBranch) &&
            containUnclassifiedFEXFault(adapter, config, context, signal,
                                        faultAddress, inCodeBuffer)) {
            return true;
        }
        return false;
    }

    // iOS __PAGEZERO prevents the Windows KUSER_SHARED_DATA VA from being
    // mapped. Match the proven Darwin Wine workaround: rewrite only ARM64
    // base registers that carry a canonical 0x7ffe* pointer and leave PC
    // unchanged so the translated load/store retries in place.
    if (!generatedException && isKuserAddress(faultAddress)) {
        bool fixed = false;
        for (unsigned reg = 0; reg <= 28; ++reg) {
            const uint64_t guestAddress = machine->__ss.__x[reg];
            if (!isKuserAddress(guestAddress)) continue;
            const uint64_t alias = adapter->process->memory64->nativeAliasForGuest(
                guestAddress);
            if (!alias) continue;
            machine->__ss.__x[reg] = alias;
            fixed = true;
        }
        if (fixed) {
            // The host signal return path retries at the original JIT PC. Do
            // not spill or advance RIP: this is a host-address repair only.
            return true;
        }
    }

    auto* frame = adapter->fexThread->CurrentFrame;
    uint32_t guestSignal = hostSignalGuestNumber(signal);
    uint32_t guestTrapNumber = hostSignalTrapNumber(signal);
    uint32_t guestSignalCode = static_cast<uint32_t>(siginfo->si_code);
    uint64_t guestFaultAddress = faultAddress;

    if (generatedException) {
        // The GuestSignal_* stub already ran SpillStaticRegs, so the frame is
        // the authoritative guest state and the host registers are not. The
        // host siginfo describes the trapping stub instruction, not the guest
        // access, so take the architectural fault description from FEX.
        guestSignal = hostSignalGuestNumber(faultData.Signal);
        guestTrapNumber = faultData.TrapNo;
        guestSignalCode = faultData.si_code;
        guestFaultAddress = frame->State.rip;
        reportGeneratedGuestFault(adapter, frame, faultData);
        if (recoverTranslatedLoaderHandoff(
                adapter, frame, faultData, context, config)) {
            return true;
        }
        frame->SynchronousFaultData.FaultToTopAndGeneratedException = 0;
    } else {
        const uint32_t ignoreMask = frame->InSyscallInfo & 0xffffu;
        frame->State.rip = adapter->context->RestoreRIPFromHostPC(
            adapter->fexThread, hostPC);
        const size_t gprCount = std::min<size_t>(config->SRAGPRCount, 16);
        for (size_t i = 0; i < gprCount; ++i) {
            const uint8_t hostRegister = config->SRAGPRMapping[i];
            if (hostRegister >= 29 || (ignoreMask & (1u << hostRegister))) continue;
            frame->State.gregs[i] = machine->__ss.__x[hostRegister];
        }
        const size_t fprCount = std::min<size_t>(config->SRAFPRCount, 16);
        for (size_t i = 0; i < fprCount; ++i) {
            const uint8_t hostRegister = config->SRAFPRMapping[i];
            if (hostRegister >= 32) continue;
            std::memcpy(frame->State.xmm.sse.data[i],
                        &machine->__ns.__v[hostRegister],
                        sizeof(frame->State.xmm.sse.data[i]));
        }
        const uint32_t compactedFlags = adapter->context->ReconstructCompactedEFLAGS(
            adapter->fexThread, true, machine->__ss.__x, machine->__ss.__cpsr);
        adapter->context->SetFlagsFromCompactedEFLAGS(
            adapter->fexThread, compactedFlags);
    }

    // CPU64::raiseSyncFault constructs the guest signal frame and updates the
    // guest registers. It is intentionally called from this narrow signal
    // seam; the surrounding BoxedWine ARMv8 exception path uses the same
    // architectural operation. If the translated guest has no handler, stop
    // only that guest through ExecuteThread's normal unwind. Chaining a fault
    // whose PC is proven to be in this thread's code buffer would otherwise
    // terminate the entire iOS app and discard the generated-code evidence.
    if (!BVNFEXCPU64AdapterSyncFromFEX(adapter, frame)) {
        return false;
    }
    // Every device run of the x86-64 cube ended with Wine reporting a
    // "page fault on read access to 0" at RtlInterlockedPushEntrySList's
    // `lock cmpxchg16b [r8]` during process exit, with the loads of [r8]
    // just before it succeeding. Nothing on this side said which host signal
    // arrived, with which code, or what R8 held, so the first faults a guest
    // is handed are recorded here with the instruction bytes and registers.
    {
        static std::atomic<uint32_t> reported {0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
            const uint64_t rip = frame->State.rip;
            uint8_t bytes[16] = {0};
            const uint64_t hostRip =
                adapter->process->memory64->nativeAliasForGuest(rip);
            if (hostRip != 0 &&
                adapter->cpu->memory->nativeRangeCoversForPlan(hostRip, hostRip + 16)) {
                std::memcpy(bytes, reinterpret_cast<const void*>(hostRip), sizeof(bytes));
            }
            char hex[40];
            for (size_t i = 0; i < sizeof(bytes); ++i) {
                snprintf(hex + i * 2, sizeof(hex) - i * 2, "%02x", bytes[i]);
            }
            // A jump to 0 says nothing about who jumped; the return slot at
            // the top of the guest stack usually does (the desktop's explorer
            // took one 16 s in, from unix-side code, and carried on).
            uint64_t returnSlot[2] = {0, 0};
            const uint64_t guestRsp = frame->State.gregs[4];
            // The translator dereferences guest addresses through the
            // deterministic alias; the kuser alias helper answers zero for a
            // stack address, which used to print an empty return slot.
            const uint64_t hostRsp =
                adapter->cpu->memory->nativeIdentityMode()
                    ? k64GuestToHostAddress(guestRsp) : 0;
            if (hostRsp != 0 &&
                adapter->cpu->memory->nativeRangeCoversForPlan(hostRsp, hostRsp + 16)) {
                std::memcpy(returnSlot, reinterpret_cast<const void*>(hostRsp),
                            sizeof(returnSlot));
            }
            const auto& g = frame->State.gregs;
            klog_fmt("BOXEDWINE_FEX64_GUEST_FAULT_STACK rsp=0x%llx slot0=0x%llx slot1=0x%llx",
                     static_cast<unsigned long long>(guestRsp),
                     static_cast<unsigned long long>(returnSlot[0]),
                     static_cast<unsigned long long>(returnSlot[1]));
            klog_fmt("BOXEDWINE_FEX64_GUEST_FAULT pid=%d tid=%d host_signal=%d "
                     "si_code=%d fault=0x%llx host_pc=0x%llx in_code=%d generated=%d "
                     "guest_signal=%u trap=%u guest_rip=0x%llx bytes=%s",
                     adapter->process->id, adapter->thread->id, signal,
                     static_cast<int>(siginfo->si_code),
                     static_cast<unsigned long long>(faultAddress),
                     static_cast<unsigned long long>(hostPC), inCodeBuffer ? 1 : 0,
                     generatedException ? 1 : 0, guestSignal, guestTrapNumber,
                     static_cast<unsigned long long>(rip), hex);
            klog_fmt("BOXEDWINE_FEX64_GUEST_FAULT_GPRS rax=0x%llx rcx=0x%llx rdx=0x%llx "
                     "rbx=0x%llx rsp=0x%llx rbp=0x%llx rsi=0x%llx rdi=0x%llx "
                     "r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx "
                     "r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx",
                     (unsigned long long)g[0], (unsigned long long)g[1],
                     (unsigned long long)g[2], (unsigned long long)g[3],
                     (unsigned long long)g[4], (unsigned long long)g[5],
                     (unsigned long long)g[6], (unsigned long long)g[7],
                     (unsigned long long)g[8], (unsigned long long)g[9],
                     (unsigned long long)g[10], (unsigned long long)g[11],
                     (unsigned long long)g[12], (unsigned long long)g[13],
                     (unsigned long long)g[14], (unsigned long long)g[15]);
            if (rip == 0) {
                // A jump to zero out of a memory-indirect call: the slot the
                // call read is the memory that matters. Every device case so
                // far read a function table at +0x70 through rdi or r15, so
                // those slots are read back now through the kernel's view and
                // through the host alias. A non-zero value here means the
                // translated load misread; zero means the memory was zero.
                KMemory64* memory = adapter->cpu->memory;
                auto probe = [&](const char* name, uint64_t base) {
                    const uint64_t slot = base + 0x70;
                    uint64_t kernelView = 0;
                    uint64_t hostView = 0;
                    uint32_t flags = 0;
                    bool mapped = false;
                    if (memory && base != 0 && memory->isPageMapped(slot >> 12)) {
                        mapped = true;
                        flags = memory->getPageFlags(slot >> 12);
                        kernelView = memory->readq(slot);
                        const uint64_t host = k64GuestToHostAddress(slot);
                        if (memory->nativeRangeCoversForPlan(host, host + 8)) {
                            std::memcpy(&hostView, reinterpret_cast<const void*>(host), 8);
                        }
                    }
                    klog_fmt("BOXEDWINE_FEX64_NULL_TARGET_PROBE reg=%s base=0x%llx slot=0x%llx "
                             "mapped=%d flags=0x%x kernel=0x%llx host=0x%llx",
                             name, (unsigned long long)base, (unsigned long long)slot,
                             mapped ? 1 : 0, flags, (unsigned long long)kernelView,
                             (unsigned long long)hostView);
                };
                probe("rdi", g[7]);
                probe("r12", g[12]);
                probe("r15", g[15]);
            }
        }
    }
    if (!adapter->cpu->raiseSyncFault(
            guestSignal, guestTrapNumber,
            static_cast<int>(guestSignalCode), guestFaultAddress) ||
        !BVNFEXCPU64AdapterSyncToFEX(adapter, frame)) {
        return containUnclassifiedFEXFault(
            adapter, config, context, signal, guestFaultAddress,
            inCodeBuffer);
    }

    frame->InSyscallInfo = 0;
    machine->__ss.__x[1] = 0; // no single-instruction continuation
    machine->__ss.__x[28] = reinterpret_cast<uint64_t>(frame);
    machine->__ss.__pc = frame->Pointers.DispatcherLoopTopFillSRA;
    return machine->__ss.__pc != 0;
#endif
}

extern "C" uint64_t BVNFEXCPU64AdapterHandleSyscall(
    BVNFEXCPU64Adapter* adapter, void* framePointer, const uint64_t* arguments) {
    if (!validAdapter(adapter) || !framePointer || !arguments) {
        if (adapter) adapter->lastAction = BVNFEXCPU64AdapterActionInvalid;
        return static_cast<uint64_t>(-K_ENOSYS);
    }
    if (!BVNFEXCPU64AdapterSyncFromFEX(adapter, framePointer)) {
        adapter->lastAction = BVNFEXCPU64AdapterActionInvalid;
        return static_cast<uint64_t>(-K_EFAULT);
    }
    CPU64* cpu = adapter->cpu;
    const uint64_t syscallNumber = arguments[0];
    const uint32_t syscallOrdinal = gLiveSyscallTraceOrdinal.fetch_add(
        1, std::memory_order_relaxed);
    const bool traceSyscall = syscallOrdinal < 64;
    if (!adapter->process->fexLiveSyscallReported.exchange(
            true, std::memory_order_acq_rel)) {
        klog_fmt("BOXEDWINE_FEX64_LIVE_SYSCALL pid=%d tid=%d nr=%llu rip=0x%llx",
                 adapter->process->id, adapter->thread->id,
                 static_cast<unsigned long long>(syscallNumber),
                 static_cast<unsigned long long>(cpu->rip));
    }
    cpu->reg[X64_RAX].setU64(arguments[0]);
    cpu->reg[X64_RDI].setU64(arguments[1]);
    cpu->reg[X64_RSI].setU64(arguments[2]);
    cpu->reg[X64_RDX].setU64(arguments[3]);
    cpu->reg[X64_R10].setU64(arguments[4]);
    cpu->reg[X64_R8].setU64(arguments[5]);
    cpu->reg[X64_R9].setU64(arguments[6]);

    // Wine's PE ntdll reaches its Unix side through per-service thunks that
    // fall into a raw SYSCALL while KUSER_SHARED_DATA's SystemCall flag is
    // clear. RAX then holds a Windows NT ordinal, not a Linux syscall number,
    // and handing it to ksyscall64 answers the wrong question: the device log
    // shows NT 227 returning -ENOSYS to a caller that retried it 3,215,735
    // times. The interpreter already recognised these thunks; the FEX path
    // never did, and FEX is what actually executes x86-64 here.
    //
    // Recognition is the shared byte-and-control-flow matcher, so both
    // backends agree on what a thunk is.
    boxedvn::WineNtSyscallStub ntStub;
    if (boxedvn::matchWineNtSyscallStubInGuest(
            cpu->memory, cpu->rip, arguments[0], ntStub)) {
        boxedvn::setWineNtSystemCallFlag(cpu->memory);
        // FEX has already applied SYSCALL's architectural clobber: it stored
        // the post-instruction RIP into RCX before calling this handler.
        // Wine's indirect path is a call and performs no such clobber, and its
        // dispatcher still expects the NT call's first argument in RCX -- the
        // thunk's own `mov r10, rcx` is what preserved it, and arguments[4]
        // is that R10.
        cpu->reg[X64_RCX].setU64(arguments[4]);
        cpu->reg[X64_R11].setU64(cpu->rflags);
        // The dispatcher reads the NT ordinal from RAX.
        cpu->reg[X64_RAX].setU64(ntStub.ntOrdinal);
        cpu->syscallRip = ntStub.syscallAddress;
        cpu->rip = ntStub.indirectPath;
        cpu->yield = false;
        cpu->reExecuteSyscall = false;
        boxedvn::reportWineNtSyscallRedirect(
            ntStub, (uint32_t)adapter->process->id,
            (uint32_t)adapter->thread->id, "fex");
        // The first NT thunk this process takes is the end of the handoff:
        // Windows code is running. Stop the armed block trace here rather
        // than letting it spend its whole budget past the interesting part.
        BVNFEXBackendDisarmHandoffTrace((uint32_t)adapter->process->id);
        if (!BVNFEXCPU64AdapterSyncToFEX(adapter, framePointer)) {
            adapter->lastAction = BVNFEXCPU64AdapterActionInvalid;
            return static_cast<uint64_t>(-K_EFAULT);
        }
        // A changed frame RIP is NOT honoured by simply returning here. In the
        // pinned FEX, DEFAULT_SYSCALL_FLAGS omits FLAGS_BLOCK_END off Windows
        // (FEXCore/Source/Interface/Core/X86Tables/X86Tables.h), so SyscallOp
        // never emits the `ExitFunction(_LoadContextGPR(rip))` that would pick
        // the new RIP up -- the block just continues into the byte after the
        // SYSCALL. Yield is the contract that does honour it: BoxedWineSyscalls
        // ::HandleSyscall longjmps out of ExecuteThread on that action, and the
        // run loop's next iteration re-enters FEX after syncing cpu->rip back
        // into the frame. cpu->yield stays false, so the CPU64 scheduler
        // re-dispatches this thread immediately rather than parking it.
        //
        // The cost is one FEX exit and re-entry, and it is paid roughly once
        // per process: the SystemCall flag written above means every later
        // thunk branches to its indirect path without trapping at all.
        adapter->lastAction = BVNFEXCPU64AdapterActionYield;
        return ntStub.ntOrdinal;
    }

    // FEX invokes the handler with RIP at the SYSCALL instruction. Mirror the
    // architectural side effects that CPU64's interpreter performs before
    // entering ksyscall64: RCX receives the post-instruction RIP and R11 gets
    // the pre-syscall flags. Keep syscallRip separately for restartable
    // futex waits; the kernel dispatcher itself sees the post-instruction RIP.
    cpu->syscallRip = cpu->rip;
    const uint64_t postSyscallRip = cpu->syscallRip + 2;
    cpu->reg[X64_RCX].setU64(postSyscallRip);
    cpu->reg[X64_R11].setU64(cpu->rflags);
    cpu->rip = postSyscallRip;
    cpu->yield = false;
    cpu->reExecuteSyscall = false;
    if (traceSyscall) {
        klog_fmt("BOXEDWINE_FEX64_SYSCALL_ENTER ordinal=%u pid=%d tid=%d "
                 "nr=%llu a1=0x%llx a2=0x%llx rip=0x%llx",
                 syscallOrdinal, adapter->process->id, adapter->thread->id,
                 static_cast<unsigned long long>(syscallNumber),
                 static_cast<unsigned long long>(arguments[1]),
                 static_cast<unsigned long long>(arguments[2]),
                 static_cast<unsigned long long>(cpu->syscallRip));
    }
    ksyscall64(cpu);
    // Address space changes made by this syscall (munmap, MAP_FIXED over a
    // mapped range, exec's teardown) are applied to the translator here, with
    // no kernel lock held and before any translated code runs again.
    if (syscallNumber != 60 && syscallNumber != 231 && adapter->process &&
        adapter->process->memory64) {
        adapter->process->memory64->flushTranslatedCodeInvalidations();
    }
    if (traceSyscall) {
        klog_fmt("BOXEDWINE_FEX64_SYSCALL_RETURN ordinal=%u pid=%d tid=%d "
                 "nr=%llu result=0x%llx rip=0x%llx yield=%d restart=%d",
                 syscallOrdinal, adapter->process->id, adapter->thread->id,
                 static_cast<unsigned long long>(syscallNumber),
                 static_cast<unsigned long long>(cpu->reg[X64_RAX].u64),
                 static_cast<unsigned long long>(cpu->rip),
                 cpu->yield ? 1 : 0, cpu->reExecuteSyscall ? 1 : 0);
    }

    if (cpu->yield) {
        if (cpu->reExecuteSyscall) {
            cpu->reExecuteSyscall = false;
            cpu->rip = cpu->syscallRip;
        }
        // The runner is about to leave FEX through its jump guard. Preserve
        // the post-syscall frame for ordinary yields, or the rewound syscall
        // frame for a parked/restarted syscall, before that longjmp occurs.
        if (!BVNFEXCPU64AdapterSyncToFEX(adapter, framePointer)) {
            adapter->lastAction = BVNFEXCPU64AdapterActionInvalid;
            return static_cast<uint64_t>(-K_EFAULT);
        }
        if (syscallNumber == 231) {
            adapter->lastAction = BVNFEXCPU64AdapterActionProcessExit;
        } else if (syscallNumber == 60) {
            adapter->lastAction = BVNFEXCPU64AdapterActionThreadExit;
        } else {
            adapter->lastAction = BVNFEXCPU64AdapterActionYield;
        }
        return cpu->reg[X64_RAX].u64;
    }

    const bool execSucceeded = arguments[0] == 59 &&
        static_cast<S64>(cpu->reg[X64_RAX].u64) >= 0;

    // FEX's generic-host SYSCALL lowering is not a block end. A syscall that
    // replaces RIP therefore cannot return through the ordinary Continue
    // action: FEX would keep running the bytes after SYSCALL in the old block,
    // ignoring the state RIP we just restored. That is exactly what the device
    // logs show for rt_sigreturn: thousands of returns at Wine's restorer RIP,
    // with one signal frame lost from RSP on each pass, before exit_group(1).
    //
    // Leave this block and let ExecuteThread re-enter the dispatcher with the
    // synced RIP. This also covers synchronous self-signal delivery. exec keeps
    // its stronger action because it replaced the whole image, not only RIP.
    if (boxedvn::fexSyscallMustLeaveCurrentBlock(postSyscallRip, cpu->rip)) {
        if (!BVNFEXCPU64AdapterSyncToFEX(adapter, framePointer)) {
            adapter->lastAction = BVNFEXCPU64AdapterActionInvalid;
            return static_cast<uint64_t>(-K_EFAULT);
        }
        if ((syscallNumber == 15 || syscallNumber == 13 ||
             syscallNumber == 234 || syscallNumber == 200) &&
            gSyscallRedirectReports.fetch_add(
                1, std::memory_order_relaxed) < 32) {
            klog_fmt("BOXEDWINE_FEX64_SYSCALL_REDIRECT pid=%d tid=%d "
                     "nr=%llu from=0x%llx to=0x%llx rsp=0x%llx",
                     adapter->process->id, adapter->thread->id,
                     static_cast<unsigned long long>(syscallNumber),
                     static_cast<unsigned long long>(postSyscallRip),
                     static_cast<unsigned long long>(cpu->rip),
                     static_cast<unsigned long long>(
                         cpu->reg[X64_RSP].u64));
        }
        adapter->lastAction = execSucceeded
            ? BVNFEXCPU64AdapterActionExec
            : BVNFEXCPU64AdapterActionYield;
        return cpu->reg[X64_RAX].u64;
    }
    adapter->lastAction = execSucceeded
        ? BVNFEXCPU64AdapterActionExec
        : BVNFEXCPU64AdapterActionContinue;
    if (cpu->reExecuteSyscall) {
        cpu->reExecuteSyscall = false;
        cpu->rip = cpu->syscallRip;
    }
    if (!BVNFEXCPU64AdapterSyncToFEX(adapter, framePointer)) {
        adapter->lastAction = BVNFEXCPU64AdapterActionInvalid;
        return static_cast<uint64_t>(-K_EFAULT);
    }
    return cpu->reg[X64_RAX].u64;
}

extern "C" BVNFEXCPU64AdapterAction BVNFEXCPU64AdapterLastAction(
    const BVNFEXCPU64Adapter* adapter) {
    return adapter ? adapter->lastAction : BVNFEXCPU64AdapterActionInvalid;
}

#endif
