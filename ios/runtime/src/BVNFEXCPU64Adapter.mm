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
#include "cpu64.h"
#include "fex64loaderhandoff.h"
#include "kmemory64.h"
#include "kprocess.h"
#include "kthread.h"
#include "syscall64.h"

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
    adapter->lastAction = BVNFEXCPU64AdapterActionProcessExit;
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
