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
extern "C" uint64_t BVNFEXBackendTakePendingIRCapTarget(const char*) { return 0; }

#else

#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#import <Foundation/Foundation.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>

#ifdef FSCALE
#undef FSCALE
#endif
#include "boxedwine.h"
#include "boxedvn/fex_exit_dispatch_contract.h"
#include "cpu64.h"
#include "guest_segment_table.h"
#include "fex64loaderhandoff.h"
#include "kmemory64.h"
#include "kprocess.h"
#include "ksignal.h"   // K_SIGSEGV / K_SEGV_* / K_BUS_ADRALN
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
    // The selectors travel with the registers. Nothing in the interpreter
    // reads them; the signal path does, because the translator takes a
    // block's decode width from the descriptor cs_idx names and a handler
    // entered with the 32-bit code selector still loaded is decoded as
    // 32-bit code.
    cpu->seg.cs = frame->State.cs_idx;
    cpu->seg.ss = frame->State.ss_idx;
    cpu->seg.ds = frame->State.ds_idx;
    cpu->seg.es = frame->State.es_idx;
    cpu->seg.fs = frame->State.fs_idx;
    cpu->seg.gs = frame->State.gs_idx;
    cpu->seg.valid = true;
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
    // Only once this CPU64 has read a frame. The first entry into FEX copies
    // this state into a frame publishGuestDescriptorTable has just written,
    // and the defaults here must not overwrite the selectors it published --
    // nor those of a fresh execution epoch after exec, which republishes them
    // and is re-read before the next entry.
    //
    // The cached bases are deliberately not recomputed from the descriptors:
    // every descriptor in this port's table is flat, so cs/ss/ds/es bases are
    // zero either way, and fs_cached/gs_cached are owned by the host's
    // selector-write trap and by arch_prctl, both of which have already
    // written cpu->fsbase/gsbase above.
    if (cpu->seg.valid) {
        frame->State.cs_idx = cpu->seg.cs;
        frame->State.ss_idx = cpu->seg.ss;
        frame->State.ds_idx = cpu->seg.ds;
        frame->State.es_idx = cpu->seg.es;
        frame->State.fs_idx = cpu->seg.fs;
        frame->State.gs_idx = cpu->seg.gs;
    }
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

// A translated guest access takes the HOST's verdict on a guest address, never
// the page map's: FEX dereferences the alias directly (see guest_low_alias.h),
// so a page the guest has mapped whose 16 KiB host page lost its backing or its
// protection arrives here as a host fault on the alias image of that address.
//
// Neither the signal number nor si_code identifies the case. Darwin reports an
// access a region forbids as SIGBUS -- with si_code hard-wired to BUS_ADRALN by
// the arm64 sendsig(), so it says nothing about alignment -- and an access to
// an address with no region at all as SIGSEGV/KERN_INVALID_ADDRESS. Only the
// page map knows what the guest was entitled to, so ask it, repair no more than
// it entitles the guest to, and retry the instruction in place.
struct AliasBackingRepairGuard {
    uint64_t address = 0;
    uint64_t hostPC = 0;
    bool armed = false;
};
// Per guest thread. A repaired instruction is retried at the SAME host PC, so a
// repair that did not actually unblock it -- a store to a page the guest itself
// only granted read, say -- must not be attempted twice: nothing runs between
// the two faults, so nothing can have changed, and the second one is the guest
// fault it always was. This is what bounds the retry loop.
static thread_local AliasBackingRepairGuard gAliasBackingRepairGuard;
static std::atomic<uint32_t> gAliasBackingReports {0};

static bool repairGuestLaneHostFault(BVNFEXCPU64Adapter* adapter, int signal,
                                     uint64_t faultAddress, uint64_t hostPC) {
    if (signal != SIGBUS && signal != SIGSEGV) return false;
    if (!adapter->process || !adapter->process->memory64) return false;

    K64NativeFaultRepair report;
    const bool repeat = gAliasBackingRepairGuard.armed &&
                        gAliasBackingRepairGuard.address == faultAddress &&
                        gAliasBackingRepairGuard.hostPC == hostPC;
    bool repaired = false;
    if (repeat) {
        report.decision = "repeat";
    } else {
        // The translator cannot tell this handler whether the access was a load
        // or a store, so the page only has to be READABLE for a repair to be
        // legal. A store to a page the guest granted read alone is restored
        // read-only, faults again at the same PC, and the guard above hands it
        // to the guest.
        repaired = adapter->process->memory64->nativeRepairHostFault(
            faultAddress, K_PROT_READ, report);
    }

    // The witness for this failure mode: which guest address the host address
    // was the image of, what each of the two ledgers said about it, and what
    // was done. A run that still dies here says which ledger is wrong.
    if ((report.inGuestLane || repeat) &&
        gAliasBackingReports.fetch_add(1, std::memory_order_relaxed) < 64) {
        // last_op / last_flags name the operation that last wrote the faulting
        // guest page's rights and the value it wrote; nb_op / nb_flags do the
        // same for the most recently written OTHER guest subpage of the same
        // host page. Together they separate the two failures that look
        // identical from the guest: a page whose whole host page was never
        // committed, and a page that lost rights its 16 KiB neighbours kept.
        //
        // prior_op / prior_flags are the write before the last CHANGE to the
        // faulting page. A reservation the guest never used and a block the
        // guest committed, freed, and had re-reserved under it both read
        // last_op=mmap-anon last_flags=0x20; only the prior write says which,
        // and only one of the two is a use-after-free.
        //
        // commit_below / commit_above bracket the refusal with the nearest
        // memory the guest COULD read. A loop walking off the end of a buffer
        // faults on the first page past it, so commit_below is that buffer's
        // end; a pointer that never addressed anything has neither.
        klog_fmt("BOXEDWINE_X64_ALIAS_BACKING pid=%d tid=%d signal=%d "
                 "fault=0x%llx guest=0x%llx mapped=%d guest_prot=0x%x "
                 "host=[0x%llx,0x%llx) host_present=%d host_prot=0x%x "
                 "tracked=%d page_prot=0x%x materialised=%d reprotected=%d "
                 "last_op=%s last_flags=0x%x last_seq=%u "
                 "nb_op=%s nb_flags=0x%x nb_seq=%u "
                 "prior_op=%s prior_flags=0x%x prior_seq=%u "
                 "commit_below=0x%llx commit_above=0x%llx "
                 "host_pc=0x%llx decision=%s",
                 adapter->process ? adapter->process->id : -1,
                 adapter->thread ? adapter->thread->id : -1, signal,
                 (unsigned long long)faultAddress,
                 (unsigned long long)report.guestAddress,
                 report.pageMapped ? 1 : 0, (unsigned)report.guestProt,
                 (unsigned long long)report.hostPageStart,
                 (unsigned long long)(report.hostPageStart +
                                      report.hostPageLength),
                 report.hostPresent ? 1 : 0, (unsigned)report.hostProtBefore,
                 report.tracked ? 1 : 0, (unsigned)report.hostPageProt,
                 report.materialised ? 1 : 0, report.reprotected ? 1 : 0,
                 report.lastWriter, (unsigned)report.lastWriteFlags,
                 (unsigned)report.lastWriteStamp,
                 report.neighbourWriter, (unsigned)report.neighbourWriteFlags,
                 (unsigned)report.neighbourWriteStamp,
                 report.priorWriter, (unsigned)report.priorWriteFlags,
                 (unsigned)report.priorWriteStamp,
                 (unsigned long long)report.committedBelow,
                 (unsigned long long)report.committedAbove,
                 (unsigned long long)hostPC, report.decision);
    }

    gAliasBackingRepairGuard.armed = repaired;
    gAliasBackingRepairGuard.address = faultAddress;
    gAliasBackingRepairGuard.hostPC = hostPC;
    return repaired;
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

// How a translated memory fault is described to the guest.
struct GuestMemoryFaultClass {
    uint32_t signal = 0;
    uint32_t trapNumber = 0;
    uint32_t code = 0;
    uint64_t address = 0;
    // The page map, not the host signal, decided this. False means the fault
    // is not a guest-lane memory access and the host's own verdict stands.
    bool fromPageMap = false;
};

// Classify a translated guest memory fault the way Linux would, from the page
// map rather than from the host signal number.
//
// Linux raises SIGSEGV for BOTH memory faults a guest can recover from: an
// address with no mapping is SEGV_MAPERR, and a mapped page whose protection
// forbids the access is SEGV_ACCERR. Both carry trap 14 (#PF), and Wine turns
// both into STATUS_ACCESS_VIOLATION -- the status its guard pages, its stack
// growth and its commit-on-demand all run from. A 32-bit WoW64 view that
// NtAllocateVirtualMemory reserved but has not committed is precisely the
// second case: mapped, with no access rights at all, waiting for Wine's own
// page-fault handler to commit it.
//
// Darwin describes the same fault as SIGBUS, and its arm64 sendsig() hard-wires
// si_code to BUS_ADRALN for EVERY SIGBUS, so neither the host signal number nor
// its code carries any information about which fault this was. Passing them
// through delivered the guest SIGBUS with trap 17, which Wine reads as
// STATUS_DATATYPE_MISALIGNMENT: the page-fault handler never ran and the
// process died on its own reservation.
//
// The page map is the only ledger that knows what the guest was entitled to,
// so it is what classifies the fault -- exactly as it is what authorises the
// repair above. Only a fault on a page the map says the access WAS entitled to
// can be an alignment fault, and that one keeps SIGBUS and trap 17; FEX's own
// unaligned handler has already had first refusal on it at the top of the
// fault path, so nothing that FEX can emulate in place reaches here at all.
//
// The address handed to the guest is the canonical guest address, never the
// host alias the translator dereferenced.
static GuestMemoryFaultClass classifyGuestMemoryFault(
    BVNFEXCPU64Adapter* adapter, int signal, int hostSignalCode,
    uint64_t faultAddress) {
    GuestMemoryFaultClass result;
    result.signal = hostSignalGuestNumber(signal);
    result.trapNumber = hostSignalTrapNumber(signal);
    result.code = static_cast<uint32_t>(hostSignalCode);
    result.address = faultAddress;
    if (signal != SIGBUS && signal != SIGSEGV) return result;

    KMemory64* memory = adapter->process ? adapter->process->memory64 : nullptr;
    if (!memory || !memory->nativeIdentityMode()) return result;

    // The same membership test nativeRepairHostFault applies: only a host
    // address that is the image of a guest address THIS address space serves
    // can be described in guest terms at all. Everything else -- the
    // emulator's own heap, the translated code arena, the kuser alias -- keeps
    // the host's verdict and its host address.
    const uint64_t guestAddress = k64HostToGuestAddress(faultAddress);
    if (k64GuestToHostAddress(guestAddress) != faultAddress) return result;
    const uint64_t guestPageAddress = guestAddress & ~K64_PAGE_MASK;
    if (!memory->nativeGuestRangeAllowed(guestPageAddress, K64_PAGE_SIZE)) {
        return result;
    }
    result.address = guestAddress;
    result.fromPageMap = true;

    const uint64_t pageNumber = guestAddress >> K64_PAGE_SHIFT;
    const bool mapped = memory->isPageMapped(pageNumber);
    // The translator cannot tell this handler whether the access was a load or
    // a store, so read is the only right the page has to grant for the access
    // to have been entitled -- the same requiredProt the repair asks for.
    const bool readable =
        mapped && (memory->getPageFlags(pageNumber) & K64_PAGE_READ) != 0;
    if (!mapped || !readable) {
        result.signal = K_SIGSEGV;
        result.trapNumber = 14; // #PF
        result.code = mapped ? K_SEGV_ACCERR : K_SEGV_MAPERR;
        return result;
    }
    if (signal == SIGBUS) {
        // The map entitles the access, so this is not a protection fault and
        // not a missing translation. Alignment is what is left, and it is a
        // genuine one: FEX declined to emulate it.
        result.signal = K_SIGBUS;
        result.trapNumber = 17;
        result.code = K_BUS_ADRALN;
        return result;
    }
    // A host SIGSEGV on a page the map says is readable is the two ledgers
    // disagreeing about a page that exists, which the repair above already
    // declined to reconcile. It is still a page fault to the guest, and the
    // page is mapped, so it is an access error rather than a missing one.
    result.signal = K_SIGSEGV;
    result.trapNumber = 14; // #PF
    result.code = K_SEGV_ACCERR;
    return result;
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

// A command line reduced to the program image it names: everything after the
// last separator of either kind. The stored command line is read from the
// process that faulted and carries the loader path and the guest path it was
// given ("<loader>d:\\...\\program.exe"); the launching one is read while the
// process is still being built, so the prefix, the separators and the argument
// tail can all differ between two launches of the same program. The image name
// is the part that makes a fixed instruction address meaningful across runs.
static NSString* ircapProgramImageName(NSString* command) {
    if (command.length == 0) return @"";
    NSCharacterSet* const separators =
        [NSCharacterSet characterSetWithCharactersInString:@"\\/"];
    const NSRange separator = [command rangeOfCharacterFromSet:separators
                                                      options:NSBackwardsSearch];
    if (separator.location == NSNotFound) return command;
    return [command substringFromIndex:NSMaxRange(separator)];
}

extern "C" void BVNFEXBackendPublishPendingIRCapTarget(uint64_t guestRIP) {
    if (guestRIP == 0) return;
    gPendingIRCapTarget.store(guestRIP, std::memory_order_relaxed);
    // Persisted, so the arming survives an app restart: the runs that fault
    // here have not been followed by a second launch in the same process.
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    [defaults setObject:@(static_cast<unsigned long long>(guestRIP))
                 forKey:@"BoxedVN.fex64.pendingIRCapTarget"];
    // Scoped to the program that faulted: program images sit at fixed
    // addresses across runs of the same program, not across programs, and
    // the desktop launch had consumed a target the cube had armed.
    BVNFEXCPU64Adapter* adapter = BVNFEXCPU64AdapterCurrent();
    const char* command = adapter && adapter->process
        ? adapter->process->commandLine.c_str() : "";
    [defaults setObject:@(command) forKey:@"BoxedVN.fex64.pendingIRCapCommand"];
}

extern "C" uint64_t BVNFEXBackendTakePendingIRCapTarget(const char* commandLine) {
    uint64_t target = gPendingIRCapTarget.exchange(0, std::memory_order_relaxed);
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    NSString* const key = @"BoxedVN.fex64.pendingIRCapTarget";
    NSString* const commandKey = @"BoxedVN.fex64.pendingIRCapCommand";
    if (target == 0) {
        id stored = [defaults objectForKey:key];
        if ([stored respondsToSelector:@selector(unsignedLongLongValue)]) {
            target = [stored unsignedLongLongValue];
        }
        NSString* storedCommand = [defaults stringForKey:commandKey] ?: @"";
        NSString* currentCommand = @"";
        if (commandLine != nullptr) {
            currentCommand = [NSString stringWithUTF8String:commandLine] ?: @"";
        }
        NSString* const storedImage = ircapProgramImageName(storedCommand);
        NSString* const currentImage = ircapProgramImageName(currentCommand);
        // Only a *named* other program defers the target. An empty current
        // command line is not evidence of one: the take happens on the first
        // live process, before the launching command line is necessarily set,
        // and refusing on that alone deferred the target forever. Comparing
        // the image names rather than the whole strings survives a differing
        // loader path or argument tail between the faulting run and this one.
        const bool otherProgram = currentImage.length > 0 &&
                                  storedImage.length > 0 &&
                                  ![storedImage isEqualToString:currentImage];
        if (target != 0 && otherProgram) {
            // Another program is launching; the target stays for its own.
            klog_fmt("BOXEDWINE_FEX64_IRCAP_TARGET_DEFERRED rip=0x%llx "
                     "for='%s' current='%s' stored_image='%s' current_image='%s'",
                     static_cast<unsigned long long>(target),
                     storedCommand.UTF8String ?: "",
                     currentCommand.UTF8String ?: "",
                     storedImage.UTF8String ?: "",
                     currentImage.UTF8String ?: "");
            return 0;
        }
    }
    if ([defaults objectForKey:key] != nil) {
        [defaults removeObjectForKey:key];
        [defaults removeObjectForKey:commandKey];
    }
    if (target != 0) {
        klog_fmt("BOXEDWINE_FEX64_IRCAP_TARGET_TAKEN rip=0x%llx",
                 static_cast<unsigned long long>(target));
    }
    return target;
}

// FEX's own reconstruction, exported by the pinned translator. Takes the block
// header recorded in the frame rather than the thread's current block, which
// may already have moved on by the time the fault is examined.
extern "C" uint64_t ios_fex_rip_from_hostpc(uint64_t BlockBegin, uint64_t HostPC);

// Wine's 64-bit unix ntdll allocates the 32-bit FS selector of a WoW64
// thread with set_thread_area through the legacy `int 0x80` gate
// (dlls/ntdll/unix/signal_x86_64.c, alloc_fs_sel). The translator refuses
// that gate in 64-bit mode and raises a general protection fault. The few
// legacy calls WoW64 setup needs are served from that fault: the descriptor
// lands in the thread's own segment table, which the translator already
// reads for every segment-relative access, and execution resumes after the
// two-byte instruction with the 32-bit ABI result in eax.
static bool emulateLegacySyscall(BVNFEXCPU64Adapter* adapter,
                                 FEXCore::Core::CpuStateFrame* frame) {
    CPU64* cpu = adapter ? adapter->cpu : nullptr;
    KMemory64* memory = cpu ? cpu->memory : nullptr;
    if (!cpu || !memory || !frame) return false;
    uint8_t opcode[2] = {0, 0};
    memory->memcpyFromGuest(opcode, cpu->rip, sizeof(opcode));
    if (opcode[0] != 0xcd || opcode[1] != 0x80) return false;
    // The gate's semantics, the descriptor bookkeeping and the diagnostics live
    // in source/kernel/legacysyscall64.cpp so the interpreter, which decodes
    // this opcode itself, cannot answer it differently. eax is written there;
    // only the translator's own descriptor table is published from here.
    const boxedvn::LegacySyscallResult legacy =
        boxedvn::kernelLegacySyscall64(cpu);
    if (legacy.descriptorInstalled) {
        FEXCore::Core::CPUState::gdt_segment* segment =
            FEXCore::Core::CPUState::GetSegmentFromIndex(
                frame->State, legacy.descriptor.selector);
        *segment = {};
        FEXCore::Core::CPUState::SetGDTBase(segment, legacy.descriptor.base);
        FEXCore::Core::CPUState::SetGDTLimit(segment, legacy.descriptor.limit);
        segment->G = legacy.descriptor.granularity ? 1 : 0;
        segment->D = legacy.descriptor.defaultBig ? 1 : 0;
        segment->P = legacy.descriptor.present ? 1 : 0;
        segment->S = 1;
        segment->DPL = 3;
        segment->Type = 0x3; // data, read/write, accessed
    }
    cpu->rip += legacy.instructionLength;
    return true;
}

// `mov fs, r/m16` / `mov gs, r/m16`, and `pop fs` / `pop gs`. In either
// decode mode the translator stores the selector and traps (trap 13,
// si_code 0x80) at the instruction; the segment base comes from the
// descriptor the selector names, which is what a Linux process gets when it
// reloads FS with an LDT/GDT selector. Wine's WoW64 layer writes FS with the
// `mov` form after set_thread_area, then far-jumps into 32-bit code that
// addresses its TEB through FS; the `pop` form is the only other opcode that
// can write these two registers (FEX leaves LFS and LGS unimplemented), and
// its stack update has already been performed by the translated block.
static bool emulateSegmentSelectorWrite(BVNFEXCPU64Adapter* adapter,
                                        FEXCore::Core::CpuStateFrame* frame) {
    CPU64* cpu = adapter ? adapter->cpu : nullptr;
    KMemory64* memory = cpu ? cpu->memory : nullptr;
    if (!cpu || !memory || !frame) return false;
    uint8_t bytes[12] = {0};
    memory->memcpyFromGuest(bytes, cpu->rip, sizeof(bytes));
    uint32_t i = 0;
    // Operand-size, address-size, segment and REX prefixes; Wine's own
    // write is `mov fs, word ptr gs:[0x338]` (65 8e 24 25 38 03 00 00).
    while (i < 5 && (bytes[i] == 0x66 || bytes[i] == 0x67 || bytes[i] == 0x26 ||
                     bytes[i] == 0x2e || bytes[i] == 0x36 || bytes[i] == 0x3e ||
                     bytes[i] == 0x64 || bytes[i] == 0x65 ||
                     (bytes[i] & 0xf0) == 0x40)) {
        ++i;
    }
    bool fs = false;
    uint32_t length = 0;
    const char* form = "mov";
    if (bytes[i] == 0x0f && (bytes[i + 1] == 0xa1 || bytes[i + 1] == 0xa9)) {
        // `pop fs` (0f a1) and `pop gs` (0f a9). Nothing is popped here: the
        // block already adjusted the stack pointer and stored the selector.
        fs = bytes[i + 1] == 0xa1;
        length = i + 2;
        form = "pop";
    } else if (bytes[i] == 0x8e) {
        const uint8_t modrm = bytes[i + 1];
        const uint32_t reg = (modrm >> 3) & 7;
        const uint32_t mod = modrm >> 6;
        const uint32_t rm = modrm & 7;
        if (reg != 4 && reg != 5) return false; // FS, GS
        fs = reg == 4;
        length = i + 2;
        if (mod != 3) {
            if (rm == 4) {
                const uint8_t sib = bytes[i + 2];
                length += 1;
                if (mod == 0 && (sib & 7) == 5) length += 4;
            } else if (mod == 0 && rm == 5) {
                length += 4;
            }
            if (mod == 1) length += 1;
            if (mod == 2) length += 4;
        }
    } else {
        return false;
    }
    const uint16_t selector = fs ? frame->State.fs_idx : frame->State.gs_idx;
    uint32_t base = 0;
    if ((selector & ~3u) != 0 && (selector >> 3) < K_GUEST_SEGMENT_TABLE_ENTRIES) {
        FEXCore::Core::CPUState::gdt_segment* segment =
            FEXCore::Core::CPUState::GetSegmentFromIndex(frame->State, selector);
        base = FEXCore::Core::CPUState::CalculateGDTBase(*segment);
    }
    // The adapter copies these into fs_cached/gs_cached on the way back.
    if (fs) {
        cpu->fsbase = base;
    } else {
        cpu->gsbase = base;
    }
    static std::atomic<uint32_t> reports {0};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 32) {
        klog_fmt("BOXEDWINE_X64_SEGMENT_WRITE pid=%d tid=%d seg=%s form=%s "
                 "selector=0x%x base=0x%x length=%u cs=0x%x rip=0x%llx",
                 adapter->process ? adapter->process->id : -1,
                 adapter->thread ? adapter->thread->id : -1, fs ? "fs" : "gs",
                 form, selector, base, length,
                 static_cast<unsigned>(frame->State.cs_idx),
                 static_cast<unsigned long long>(cpu->rip));
    }
    cpu->rip += length;
    return true;
}

// A jump to zero out of a memory-indirect call leaves the call's return
// address on the guest stack. The call instruction ends there; its length is
// found by decoding `ff /2` backwards, its operand names the slot the call
// read, and that slot is read back now. The instruction's address is also
// published as the pending IR capture target: the next launch in this app
// session compiles it with the capture armed, which prints the IR and the
// host words of the load that returned zero. Program images sit at fixed
// addresses across runs, which is what makes that arming useful.
struct NullCallSite {
    uint64_t instruction = 0;
    uint32_t length = 0;
    bool memoryOperand = false;
    uint64_t operandAddress = 0;
    uint64_t slotValue = 0;
    bool slotReadable = false;
    uint8_t bytes[8] = {0};
};

static bool decodeNullCallSite(BVNFEXCPU64Adapter* adapter,
                               const uint64_t* g, uint64_t returnAddress,
                               NullCallSite* site) {
    KMemory64* memory = adapter && adapter->cpu ? adapter->cpu->memory : nullptr;
    if (!memory || returnAddress < 8) return false;
    uint8_t window[8] = {0};
    if (!memory->isPageMapped((returnAddress - 8) >> 12) ||
        !memory->isPageMapped((returnAddress - 1) >> 12)) return false;
    memory->memcpyFromGuest(window, returnAddress - 8, sizeof(window));
    for (uint32_t length = 2; length <= 8; ++length) {
        const uint8_t* code = window + (8 - length);
        uint32_t index = 0;
        uint8_t rex = 0;
        if ((code[index] & 0xf0) == 0x40) rex = code[index++];
        if (index >= length || code[index] != 0xff) continue;
        ++index;
        if (index >= length) continue;
        const uint8_t modrm = code[index++];
        const uint32_t mod = modrm >> 6, reg = (modrm >> 3) & 7;
        uint32_t rm = modrm & 7;
        if (reg != 2) continue;
        uint64_t address = 0;
        bool memoryOperand = true;
        if (mod == 3) {
            memoryOperand = false;
            address = g[rm | ((rex & 1) << 3)];
        } else {
            bool ripRelative = false;
            if (rm == 4) {
                if (index >= length) continue;
                const uint8_t sib = code[index++];
                const uint32_t scale = sib >> 6, indexReg = ((sib >> 3) & 7) | ((rex & 2) << 2);
                const uint32_t baseReg = (sib & 7) | ((rex & 1) << 3);
                if (indexReg != 4) address += g[indexReg] << scale;
                if ((sib & 7) == 5 && mod == 0) { /* disp32, no base */ }
                else address += g[baseReg];
            } else if (rm == 5 && mod == 0) {
                ripRelative = true;
            } else {
                address += g[rm | ((rex & 1) << 3)];
            }
            if (mod == 1) {
                if (index + 1 > length) continue;
                address += (int64_t)(int8_t)code[index]; index += 1;
            } else if (mod == 2 || (mod == 0 && (rm == 5 || (rm == 4 && (code[index - 1] & 7) == 5)))) {
                if (index + 4 > length) continue;
                int32_t disp = 0; std::memcpy(&disp, code + index, 4); index += 4;
                address += (int64_t)disp;
            }
            if (ripRelative) address += returnAddress;
        }
        if (index != length) continue;
        site->instruction = returnAddress - length;
        site->length = length;
        site->memoryOperand = memoryOperand;
        site->operandAddress = address;
        std::memcpy(site->bytes, code, length);
        if (memoryOperand && memory->isPageMapped(address >> 12) &&
            memory->isPageMapped((address + 7) >> 12)) {
            site->slotValue = memory->readq(address);
            site->slotReadable = true;
        }
        return true;
    }
    return false;
}

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
    // An x86 atomic on an unaligned address (and, with strict memory
    // ordering, any unaligned load-acquire/store-release) raises SIGBUS
    // BUS_ADRALN inside translated code. FEX emulates the atomic in place or
    // backpatches the access, and returns how far the host PC moves; without
    // this the fault was handed to the guest as SIGBUS and re-taken forever.
    if (signal == SIGBUS && siginfo->si_code == BUS_ADRALN && inCodeBuffer) {
        const uint32_t instruction = *reinterpret_cast<const uint32_t*>(hostPC);
        const auto handled = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
            adapter->fexThread,
            FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier,
            hostPC, machine->__ss.__x, true);
        static std::atomic<uint32_t> reports {0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 16) {
            klog_fmt("BOXEDWINE_FEX64_UNALIGNED pid=%d tid=%d host_pc=0x%llx "
                     "instruction=0x%08x address=0x%llx handled=%d advance=%d",
                     adapter->process ? adapter->process->id : -1,
                     adapter->thread ? adapter->thread->id : -1,
                     (unsigned long long)hostPC, instruction,
                     (unsigned long long)faultAddress, handled.has_value() ? 1 : 0,
                     handled.has_value() ? (int)*handled : 0);
        }
        if (handled.has_value()) {
            machine->__ss.__pc = hostPC + (int64_t)*handled;
            return true;
        }
    }
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

    // The page map, not the host VM, is the authority on what a guest address
    // may do. A translated access that faulted on the alias image of a guest
    // page the page map says is readable is the two ledgers disagreeing, not a
    // guest fault: reconcile the host page and retry the instruction in place.
    // Every other case -- a page the guest never mapped, one it revoked access
    // to, a host address that is not a guest image at all -- falls through and
    // reaches the guest as the fault it is.
    if (!generatedException &&
        repairGuestLaneHostFault(adapter, signal, faultAddress, hostPC)) {
        return true;
    }

    auto* frame = adapter->fexThread->CurrentFrame;
    // The host signal number does not classify a memory fault on this
    // platform; the page map does. See classifyGuestMemoryFault. A fault FEX
    // generated on purpose is excluded: its si_addr describes the trapping
    // stub rather than a guest access, and the branch below takes the whole
    // architectural description from FEX instead.
    GuestMemoryFaultClass faultClass;
    faultClass.signal = hostSignalGuestNumber(signal);
    faultClass.trapNumber = hostSignalTrapNumber(signal);
    faultClass.code = static_cast<uint32_t>(siginfo->si_code);
    faultClass.address = faultAddress;
    if (!generatedException) {
        faultClass = classifyGuestMemoryFault(adapter, signal,
                                              siginfo->si_code, faultAddress);
    }
    uint32_t guestSignal = faultClass.signal;
    uint32_t guestTrapNumber = faultClass.trapNumber;
    uint32_t guestSignalCode = faultClass.code;
    uint64_t guestFaultAddress = faultClass.address;

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
    // Faults the host serves in place (int 0x80, long-mode selector writes)
    // are not guest faults. They are handled before the fault report budget
    // and the null-target probes, which otherwise spent themselves on the
    // selector write Wine repeats on every WoW64 syscall return.
    if (generatedException && guestTrapNumber == 13 &&
        (emulateLegacySyscall(adapter, frame) ||
         emulateSegmentSelectorWrite(adapter, frame))) {
        if (!BVNFEXCPU64AdapterSyncToFEX(adapter, frame)) {
            return containUnclassifiedFEXFault(
                adapter, config, context, signal, guestFaultAddress,
                inCodeBuffer);
        }
        frame->InSyscallInfo = 0;
        machine->__ss.__x[1] = 0;
        machine->__ss.__x[28] = reinterpret_cast<uint64_t>(frame);
        machine->__ss.__pc = frame->Pointers.DispatcherLoopTopFillSRA;
        return machine->__ss.__pc != 0;
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
            // The alias reader answers for the identity window only. A guest
            // address below 4 GiB -- where the WoW64 layer maps the 32-bit
            // modules and their stack -- is not in that window, so the first
            // fault taken in 32-bit code printed sixteen zero bytes and an
            // empty return slot: no instruction, no caller. The kernel's own
            // page table covers every mapped guest page and is the fallback.
            KMemory64* const faultMemory = adapter->cpu->memory;
            auto readGuestBytes = [&](uint64_t address, uint8_t* out,
                                      size_t length) -> bool {
                if (faultMemory == nullptr) return false;
                bool any = false;
                for (size_t i = 0; i < length; ++i) {
                    const uint64_t at = address + i;
                    if (!faultMemory->isPageMapped(at >> 12)) break;
                    out[i] = faultMemory->readb(at);
                    any = true;
                }
                return any;
            };
            uint8_t bytes[16] = {0};
            const uint64_t hostRip =
                adapter->process->memory64->nativeAliasForGuest(rip);
            if (hostRip != 0 &&
                adapter->cpu->memory->nativeRangeCoversForPlan(hostRip, hostRip + 16)) {
                std::memcpy(bytes, reinterpret_cast<const void*>(hostRip), sizeof(bytes));
            } else {
                readGuestBytes(rip, bytes, sizeof(bytes));
            }
            char hex[40];
            for (size_t i = 0; i < sizeof(bytes); ++i) {
                snprintf(hex + i * 2, sizeof(hex) - i * 2, "%02x", bytes[i]);
            }
            // A jump to 0 says nothing about who jumped; the return slot at
            // the top of the guest stack usually does (the desktop's explorer
            // took one 16 s in, from unix-side code, and carried on).
            // Four slots, not two. In 32-bit code the interesting words sit
            // ABOVE the return address: an i386 caller stores its outgoing
            // arguments at [esp] before the call, so the callee's [ebp+8]
            // and [ebp+0xc] land two and three slots up from the faulting
            // frame's stack pointer. A run that ended on a null first
            // argument printed only as far as the return address and could
            // not show the argument itself.
            uint64_t returnSlot[4] = {0, 0, 0, 0};
            const uint64_t guestRsp = frame->State.gregs[4];
            // The translator dereferences guest addresses through the
            // deterministic alias; the kuser alias helper answers zero for a
            // stack address, which used to print an empty return slot.
            const uint64_t hostRsp =
                adapter->cpu->memory->nativeIdentityMode()
                    ? k64GuestToHostAddress(guestRsp) : 0;
            if (hostRsp != 0 &&
                adapter->cpu->memory->nativeRangeCoversForPlan(hostRsp,
                                                              hostRsp + sizeof(returnSlot))) {
                std::memcpy(returnSlot, reinterpret_cast<const void*>(hostRsp),
                            sizeof(returnSlot));
            } else {
                uint8_t slotBytes[sizeof(returnSlot)] = {0};
                if (readGuestBytes(guestRsp, slotBytes, sizeof(slotBytes))) {
                    std::memcpy(returnSlot, slotBytes, sizeof(returnSlot));
                }
            }
            const auto& g = frame->State.gregs;
            klog_fmt("BOXEDWINE_FEX64_GUEST_FAULT_STACK rsp=0x%llx slot0=0x%llx slot1=0x%llx "
                     "slot2=0x%llx slot3=0x%llx",
                     static_cast<unsigned long long>(guestRsp),
                     static_cast<unsigned long long>(returnSlot[0]),
                     static_cast<unsigned long long>(returnSlot[1]),
                     static_cast<unsigned long long>(returnSlot[2]),
                     static_cast<unsigned long long>(returnSlot[3]));
            // cs and ss say which code segment the faulting instruction was
            // decoded for, and `decode` says what the translator made of it:
            // the L bit of the descriptor cs names, which is the same bit the
            // decoder reads when it picks a block's width. decode=32 with a
            // 32-bit cs means the mode switch took effect and the fault is a
            // real one inside 32-bit code; decode=64 with cs=0x23 means the
            // descriptor never got its L bit cleared, which is the failure
            // this lane started from.
            unsigned faultDecodeWidth = 64;
            if (frame->State.segment_arrays[(frame->State.cs_idx >> 2) & 1] !=
                nullptr) {
                const auto* faultCS =
                    FEXCore::Core::CPUState::GetSegmentFromIndex(
                        frame->State, frame->State.cs_idx);
                faultDecodeWidth = faultCS->L == 1 ? 64 : 32;
            }
            klog_fmt("BOXEDWINE_FEX64_GUEST_FAULT pid=%d tid=%d host_signal=%d "
                     "si_code=%d fault=0x%llx host_pc=0x%llx in_code=%d generated=%d "
                     "guest_signal=%u trap=%u guest_si_code=%u "
                     "guest_fault=0x%llx pagemap=%d guest_rip=0x%llx cs=0x%x ss=0x%x "
                     "decode=%u bytes=%s",
                     adapter->process->id, adapter->thread->id, signal,
                     static_cast<int>(siginfo->si_code),
                     static_cast<unsigned long long>(faultAddress),
                     static_cast<unsigned long long>(hostPC), inCodeBuffer ? 1 : 0,
                     generatedException ? 1 : 0, guestSignal, guestTrapNumber,
                     guestSignalCode,
                     static_cast<unsigned long long>(guestFaultAddress),
                     faultClass.fromPageMap ? 1 : 0,
                     static_cast<unsigned long long>(rip),
                     static_cast<unsigned>(frame->State.cs_idx),
                     static_cast<unsigned>(frame->State.ss_idx),
                     faultDecodeWidth, hex);
            // A fault whose address, taken back through the alias, lands
            // inside FEX's own CPU state is not a guest fault at all: it is
            // translated code dereferencing a HOST context address as if it
            // were a guest one, which the alias then moves into the reserved
            // window. That is a whole bug class -- an IR pass forming a
            // context address and handing it to a guest memory op -- and it
            // reads as an ordinary wild pointer otherwise. The x87 stack
            // pass's &State.mm[top] cost a full device run to attribute by
            // hand; this names the offset so the next one is a lookup in
            // CoreState.h. Fault path only, inside the existing budget.
            const uint64_t unaliased = k64HostToGuestAddress(faultAddress);
            const int64_t stateDelta =
                static_cast<int64_t>(unaliased) -
                static_cast<int64_t>(reinterpret_cast<uint64_t>(frame));
            if (stateDelta >= -4096 && stateDelta < 65536) {
                klog_fmt("BOXEDWINE_FEX64_GUEST_FAULT_CONTEXT_ALIAS pid=%d tid=%d "
                         "fault=0x%llx unaliased=0x%llx frame=0x%llx "
                         "state_offset=%lld thread=0x%llx",
                         adapter->process->id, adapter->thread->id,
                         static_cast<unsigned long long>(faultAddress),
                         static_cast<unsigned long long>(unaliased),
                         static_cast<unsigned long long>(
                             reinterpret_cast<uint64_t>(frame)),
                         static_cast<long long>(stateDelta),
                         static_cast<unsigned long long>(
                             reinterpret_cast<uint64_t>(adapter->fexThread)));
            }
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
            if (faultDecodeWidth == 32) {
                // The segmented half of a 32-bit fault, which the line above
                // carries only two selectors of. A 32-bit block adds
                // fs_cached or gs_cached at GPR width to an FS- or
                // GS-prefixed address and nothing at all to an unprefixed
                // one, so these six values decide whether a faulting address
                // is a real null in guest code or a base that never arrived.
                // Same marker the translator prints at the mode change, so a
                // log can be read as entry-then-fault.
                klog_fmt("BOXEDWINE_FEX64_SEG32 rip=0x%llx mode=32 cs=0x%x "
                         "ss=0x%x ds=0x%x es=0x%x fs=0x%x gs=0x%x "
                         "cs_base=0x%llx ss_base=0x%llx ds_base=0x%llx "
                         "es_base=0x%llx fs_base=0x%llx gs_base=0x%llx",
                         static_cast<unsigned long long>(rip),
                         static_cast<unsigned>(frame->State.cs_idx),
                         static_cast<unsigned>(frame->State.ss_idx),
                         static_cast<unsigned>(frame->State.ds_idx),
                         static_cast<unsigned>(frame->State.es_idx),
                         static_cast<unsigned>(frame->State.fs_idx),
                         static_cast<unsigned>(frame->State.gs_idx),
                         static_cast<unsigned long long>(frame->State.cs_cached),
                         static_cast<unsigned long long>(frame->State.ss_cached),
                         static_cast<unsigned long long>(frame->State.ds_cached),
                         static_cast<unsigned long long>(frame->State.es_cached),
                         static_cast<unsigned long long>(frame->State.fs_cached),
                         static_cast<unsigned long long>(frame->State.gs_cached));
            }
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
                NullCallSite site;
                if (returnSlot[0] != 0 &&
                    decodeNullCallSite(adapter, g, returnSlot[0], &site)) {
                    char hexBytes[24];
                    for (uint32_t i = 0; i < site.length && i < 8; ++i) {
                        snprintf(hexBytes + i * 2, sizeof(hexBytes) - i * 2, "%02x",
                                 site.bytes[i]);
                    }
                    klog_fmt("BOXEDWINE_FEX64_NULL_CALL_SITE rip=0x%llx bytes=%s "
                             "memory=%d operand=0x%llx slot_readable=%d slot_value=0x%llx "
                             "ircap_armed_for_next_launch=1",
                             (unsigned long long)site.instruction, hexBytes,
                             site.memoryOperand ? 1 : 0,
                             (unsigned long long)site.operandAddress,
                             site.slotReadable ? 1 : 0,
                             (unsigned long long)site.slotValue);
                    BVNFEXBackendPublishPendingIRCapTarget(site.instruction);
                }
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
        adapter->thread && !adapter->thread->terminating &&
        !adapter->process->terminated && adapter->process->memory64) {
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
