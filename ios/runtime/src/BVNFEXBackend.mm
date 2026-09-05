/*
 * BoxedVN - optional FEX CPU backend diagnostics.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * This is a translator/kernel seam, not a second application runtime. FEX
 * executes x86-64 instructions; Linux syscalls return to FEX64Kernel, which is
 * owned by BoxedWine. Wine64 and DXMT are later consumers of this same seam.
 */

#include "BVNFEXBackend.h"
#include "BVNExecMemory.h"
#include "BVNRuntime.h"

#if !defined(BOXEDVN_ENABLE_FEX64)

extern "C" bool BVNFEXBackendBuilt(void) { return false; }
extern "C" BVNFEXBackendStage BVNFEXBackendProbe(void) {
    return BVNFEXBackendStageUnavailable;
}
extern "C" BVNFEXBackendStage BVNFEXBackendStageReached(void) {
    return BVNFEXBackendStageUnavailable;
}
extern "C" const char* BVNFEXBackendReport(void) {
    return "FEX was not linked into this build.";
}
extern "C" void BVNFEXBackendPollExecutionTrace(void) {}
extern "C" uint64_t BVNFEXBackendWritableHostCodeAddress(uint64_t) {
    return 0;
}
extern "C" bool BVNFEXCPU64Run(void*, void*, BVNFEXCPU64RunOutcome* outcome) {
    if (outcome) *outcome = BVNFEXCPU64RunOutcomeFatal;
    return false;
}
extern "C" void BVNFEXBackendArmHandoffTrace(unsigned) {}
extern "C" void BVNFEXBackendDisarmHandoffTrace(unsigned) {}
extern "C" const char* BVNFEXBackendStageName(BVNFEXBackendStage stage) {
    return stage == BVNFEXBackendStageUnavailable ? "not linked" : "unknown";
}

#else

#include "boxedvn/fex64_kernel.h"
#include "boxedvn/fex_code_buffer_layout.h"
#include "boxedvn/fex_code_segments.h"
#include <new>
#include "boxedvn/fex_exit_dispatch_contract.h"
#include "boxedvn/fex_guest_mode_policy.h"
#include "guest_low_alias.h"
#include "guest_segment_table.h"
#include "boxedvn/guest_address_space.h"
#include "boxedvn/elf_inspector.h"

#import <Foundation/Foundation.h>

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/DualMap.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/fextl/memory.h>

// BoxedWine exposes FD as a legacy macro, so its umbrella header must come
// after FEX's public headers have parsed their ordinary `FD` parameter names.
// Apple's sys/param.h does the reverse with FSCALE, which is also the name of
// BoxedWine's x87 instruction handler.
#ifdef FSCALE
#undef FSCALE
#endif
#include "boxedwine.h"
#include "cpu64.h"
#include "kmemory64.h"
#include "syscall64.h"

#include <libkern/OSCacheControl.h>
#include <mach/arm/thread_status.h>
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/sysctl.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
void __clear_cache(void* start, void* end) {
    const size_t length =
        static_cast<size_t>(static_cast<char*>(end) - static_cast<char*>(start));
    sys_dcache_flush(start, length);
    sys_icache_invalidate(start, length);
}

int rpm_cas_snapshot_take(void* snapshot) {
    (void)snapshot;
    return 0;
}
}

namespace {

constexpr size_t kPageBytes = 0x4000;
constexpr size_t kFEXPageBytes = 0x1000;
static_assert(FEXCore::Utils::FEX_PAGE_SIZE == kFEXPageBytes,
              "executable layout must match FEX's guard-page size");
constexpr size_t kPoolBytes = 64u * 1024u * 1024u;
constexpr size_t kMinimumPoolBytes = 4u * 1024u * 1024u;
constexpr size_t kGuestCodeBytes = kPageBytes;
constexpr size_t kGuestStackBytes = 256u * 1024u;
// The cold exec-replacement unwind is a one-shot sequence per launch. Bound it
// so a looping scheduler cannot flood the device log with the same transition.
constexpr uint32_t kUnwindReportLimit = 24;
// Deterministic canonical guest ranges for the FEX correctness probe. The
// image sits in the canonical low range so the probe exercises the host
// alias the live guest depends on; the stack sits in the high identity lane,
// matching where the live guest's stack lane lives. Both are clear of zero,
// of KUSER_SHARED_DATA and of the addresses Wine prefers for PE images.
constexpr uint64_t kProbeGuestImageBase = 0x00e0000000ULL;
constexpr uint64_t kProbeGuestImageSpan = 16ull * 1024ull * 1024ull;
constexpr uint64_t kProbeGuestStackBase =
    boxedvn::kGuestHighBase + 0x20000000ULL;
static_assert(kProbeGuestImageBase + kProbeGuestImageSpan <
              boxedvn::kGuestLowLimit,
              "probe image must stay inside the canonical low range");
static_assert(kProbeGuestImageBase > 0x7ffe0000ULL + 0x10000ULL,
              "probe image must clear KUSER_SHARED_DATA");
// The low-alias contract is a property of the whole address space, so it
// is reported once rather than per context.
std::atomic<bool> gLowAliasReported {false};
constexpr uint64_t kExpectedExitCode = 47;

std::mutex gProbeMutex;
// FEX's guest-bitness option is process-global. Context creation, including
// the first thread, must therefore be serialized even when BoxedWine has
// multiple guest processes entering the optional backend concurrently.
std::mutex gFEXContextConstructionMutex;
std::mutex gReportMutex;
std::mutex gPoolMutex;
std::atomic<BVNFEXBackendStage> gStage {BVNFEXBackendStageIdle};
std::string gReport;

boxedvn::FexCodeSegments gCodeSegments;
std::array<boxedvn::FexCodeBufferPool, boxedvn::FexCodeSegments::maximum> gCodePools;
std::atomic<size_t> gPoolUsed {0};

void* gGuestCode = nullptr;
void* gGuestStack = nullptr;
void* gGuestMessage = nullptr;
void* gCallRetMapping = nullptr;
uint64_t gGuestEntry = 0;

boxedvn::GuestAddressSpace64 gAddressSpace;
boxedvn::FEX64Kernel gKernel(gAddressSpace,
    [](int descriptor, std::string_view bytes) {
        std::string message(bytes);
        BVNLogWrite(descriptor == 2 ? BVNLogLevelWarning : BVNLogLevelInfo,
                    "fex64-guest", message.c_str());
    });

thread_local std::jmp_buf gExitJump;
// Enough blocks to cross the dispatcher return and reach Windows code,
// small enough that an armed phase cannot become a firehose.
constexpr unsigned kHandoffTraceBlockBudget = 128;
thread_local bool gCanJump = false;
std::unique_ptr<KMemory64> gProbeMemory;
std::unique_ptr<CPU64> gProbeCPU;
thread_local CPU64* gActiveCPU = nullptr;
thread_local FEXCore::SignalDelegator* gActiveSignals = nullptr;
std::atomic<bool> gProbeExited {false};
std::atomic<uint64_t> gProbeExitCode {0};

struct FEXContextBundle {
    boxedvn::FexGuestMode mode = boxedvn::FexGuestMode::X86_64;
    std::unique_ptr<FEXCore::SignalDelegator> signals;
    std::unique_ptr<FEXCore::HLE::SyscallHandler> syscalls;
    fextl::unique_ptr<FEXCore::Context::Context> context;
    // The factory may create the first thread while the mode lock is held.
    // Ownership stays here until the caller transfers it to its thread table.
    FEXCore::Core::InternalThreadState* initialThread = nullptr;

    ~FEXContextBundle() {
        if (initialThread != nullptr && context != nullptr) {
            context->DestroyThread(initialThread);
            initialThread = nullptr;
        }
    }
};

// An exec boundary replaces the entire FEX execution epoch while BoxedWine is
// still inside BVNFEXCPU64Run. The retired context therefore still owns the
// host frames that call must return through, so its thread and context are
// retained together here instead of being destroyed at the swap point.
struct RetiredFEXEpoch {
    std::unique_ptr<FEXContextBundle> bundle;
    FEXCore::Core::InternalThreadState* thread = nullptr;

    ~RetiredFEXEpoch() {
        // Members are destroyed after this body runs, so the bundle -- and
        // with it the context that owns `thread` -- is still alive here.
        if (thread != nullptr && bundle != nullptr &&
            bundle->context != nullptr && bundle->initialThread != thread) {
            bundle->context->DestroyThread(thread);
        }
        thread = nullptr;
    }
};

std::unique_ptr<FEXContextBundle> gProbeContext;

// The descriptor table a thread's segment selectors index into.
//
// This was one entry. Every selector a 64-bit guest had loaded until now was
// zero, so index zero was the only one ever read and one entry was enough.
// Wine's dispatcher return is the first instruction in the whole process to
// load a non-zero selector: `iretq` takes __USER_CS = 0x33 and
// __USER_DS = 0x2b off its frame, which are indices 6 and 5.
//
// Neither the processor nor FEX bounds-checks that. FEX indexes the array
// directly, in two places that matter:
//
//   - the frontend reads the CS descriptor's L bit on the host side to decide
//     whether to decode the next block as 64-bit or 32-bit code, and
//   - the translated `iretq` reads the descriptor to recompute the cached
//     segment base.
//
// Reading index 6 of a one-entry array returns whatever follows it in this
// structure, so the decode mode for Windows code was being taken from
// unrelated bytes. FEX's own Linux thread manager allocates thirty-two and
// mirrors the LDT onto the same array; this now matches it.
//
// Every entry is a present, flat, long-mode descriptor. In long mode the
// base and limit of CS, SS, DS and ES are ignored and only L is consulted, so
// a uniform table cannot change any program's meaning -- and it means a
// selector this port did not anticipate keeps the decoder in 64-bit mode
// instead of reading past the end of the array.
//
// One exception, and only when the WoW64 mode switch is armed: the 32-bit
// code selector. The translator takes a block's decode width from the L bit
// of the descriptor CS names, so with a uniformly long-mode table the first
// block of 32-bit ntdll -- reached through wow64cpu's far jump, which changes
// CS and nothing else -- was decoded as 64-bit code and faulted at an address
// no 32-bit instruction can form. Giving that one descriptor L=0 and D=1 is
// the whole of the mode switch on this side; the descriptor is flat and
// present like every other, so it changes nothing for a process that never
// loads it.
inline void initialiseGuestDescriptorTable(
    FEXCore::Core::CPUState::gdt_segment* table,
    bool wow64ModeSwitch) {
    for (unsigned entry = 0; entry < K_GUEST_SEGMENT_TABLE_ENTRIES; ++entry) {
        table[entry] = {};
        FEXCore::Core::CPUState::SetGDTBase(&table[entry], 0);
        FEXCore::Core::CPUState::SetGDTLimit(&table[entry], 0xF'FFFFU);
        table[entry].P = 1;    // present
        table[entry].S = 1;    // code/data, not a system descriptor
        table[entry].Type = 0b1011;
        table[entry].L = 1;    // long mode
        table[entry].D = 0;    // reserved when L is set
    }
    if (!wow64ModeSwitch) {
        return;
    }
    const unsigned wow64Code =
        boxedvn::decodeGuestSegmentSelector(K_WINE_X86_CODE_SELECTOR).index;
    if (wow64Code < K_GUEST_SEGMENT_TABLE_ENTRIES) {
        table[wow64Code].L = 0;  // compatibility mode
        table[wow64Code].D = 1;  // 32-bit default operand and address size
    }
}

// The WoW64 mode switch: the 32-bit code descriptor above, and the
// translator's permission to decode a block for the descriptor its CS names
// rather than for the mode the context was created with. One decision, so the
// two halves cannot disagree. On by default because it is inert for a process
// that never loads a 32-bit code selector -- which is every process the lane
// has run so far -- and the environment variable is the off switch, so an A/B
// on device needs no second build.
inline bool guestWow64ModeSwitchEnabled() {
    static const bool enabled =
        getenv("BOXEDVN_FEX64_NO_MODE_SWITCH") == nullptr;
    return enabled;
}

// Point both tables at one array, the way FEX's Linux thread manager does.
inline void publishGuestDescriptorTable(
    FEXCore::Core::CPUState& state,
    FEXCore::Core::CPUState::gdt_segment* table) {
    initialiseGuestDescriptorTable(table, guestWow64ModeSwitchEnabled());
    state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] =
        table;
    // The LDT was left null. A selector with its table indicator set would
    // have dereferenced it; mirroring is what FEX does for the same reason.
    state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] =
        table;
    // The selectors a thread starts with, and all of them.
    //
    // This published CS alone, as FEX's own DEFAULT_USER_CS shifted into a
    // selector (0x30), and left SS, DS and ES at zero -- the null selector.
    // That is not what a Linux x86-64 thread starts with, and Wine reads the
    // difference rather than assuming it: `signal_init_threading` in
    // dlls/ntdll/unix/signal_x86_64.c does `movw %cs,cs64_sel` and
    // `movw %ss,ds64_sel`, and those two variables are what the WoW64 layer
    // then writes into every 32-bit context it builds --
    // `call_init_thunk` sets SegSs, SegDs, SegEs and SegGs to ds64_sel and
    // wow64cpu's bop thunk far-jumps back to cs64_sel. A device run shows the
    // consequence exactly: `IRET_POST ... cs=0x30 ss=0x0` for Wine's own
    // dispatcher return, because Wine saved and restored the pair this side
    // handed it, and `GUEST_FAULT ... cs=0x23 ss=0x0` for the fault in 32-bit
    // code, whose SS came from a 32-bit context built out of a null ds64_sel.
    //
    // So publish the pair Wine's unix side expects to read back: __USER_CS
    // and __USER_DS, the two selectors this port's table was widened for in
    // the first place. Every descriptor is flat and long-mode, so the bases
    // are zero either way and nothing about a 64-bit thread's addressing
    // changes; what changes is that `mov %ss,...` and `push %ss` now yield a
    // selector the WoW64 layer can propagate instead of a null one.
    state.cs_idx = K_WINE_X64_CODE_SELECTOR;
    state.ss_idx = K_WINE_X64_DATA_SELECTOR;
    state.ds_idx = K_WINE_X64_DATA_SELECTOR;
    state.es_idx = K_WINE_X64_DATA_SELECTOR;
    state.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(
        *FEXCore::Core::CPUState::GetSegmentFromIndex(state, state.cs_idx));
    state.ss_cached = FEXCore::Core::CPUState::CalculateGDTBase(
        *FEXCore::Core::CPUState::GetSegmentFromIndex(state, state.ss_idx));
    state.ds_cached = state.ss_cached;
    state.es_cached = state.ss_cached;
}

struct LiveThreadState {
    FEXCore::Core::InternalThreadState* fexThread = nullptr;
    void* callRetMapping = nullptr;
    size_t callRetMappingSize = 0;
    FEXCore::Core::CPUState::gdt_segment gdt[K_GUEST_SEGMENT_TABLE_ENTRIES] {};
    // Context::ExecuteThread mutates CurrentFrame, the JIT backend, and the
    // call/return stack. A KThread must therefore have at most one active
    // FEX execution at a time, even if a scheduler bug attempts to dispatch
    // the same opaque thread concurrently.
    bool active = false;
    mach_port_t hostMachThread = MACH_PORT_NULL;
    uint64_t sampleLastHostPC = 0;
    uint64_t sampleLastGuestRIP = 0;
    uint64_t sampleLastReportPoll = 0;
    uint64_t sampleLastWarningPoll = 0;
    uint64_t sampleLastFaultCount = 0;
    uint32_t sampleStablePolls = 0;
    size_t sampleCursor = 0;
    size_t sampleCount = 0;
    std::array<uint64_t, 8> sampleGuestRIPs {};
};

struct LiveProcessState {
    std::unique_ptr<FEXContextBundle> fex;
    std::unordered_map<void*, std::unique_ptr<LiveThreadState>> threads;
    // Execution epochs retired by exec. Owned until process teardown; never
    // destroyed from inside a BVNFEXCPU64Run that is still running on them.
    std::vector<std::unique_ptr<RetiredFEXEpoch>> retiredEpochs;
    // Bounded identity for the cold exec-replacement unwind trace: which epoch
    // a runner entered, returned through, and was re-entered on.
    std::atomic<uint32_t> execEpoch {0};
    std::atomic<uint32_t> unwindReports {0};
    std::atomic<size_t> activeRuns {0};
    std::atomic<bool> retiring {false};
    std::atomic<bool> entryReported {false};
};

uint64_t dispatchExitWithoutBlockLinking(
    FEXCore::Core::CpuStateFrame* frame, void* recordPointer) noexcept {
    if (!frame || !recordPointer) {
        BVNLogWrite(BVNLogLevelError, "fex64",
                    "FEX exit callback received invalid ABI inputs");
        abort();
    }
    const auto* record =
        static_cast<const boxedvn::FexExitFunctionLinkData*>(recordPointer);
    const auto transition = boxedvn::dispatchWithoutBlockLinking(
        *record, frame->Pointers.DispatcherLoopTop);
    frame->State.rip = transition.guestRIP;
    return transition.hostTarget;
}

void disableLiveBlockLinking(FEXCore::Core::InternalThreadState* thread) {
    if (!thread || !thread->CurrentFrame) return;
    thread->CurrentFrame->Pointers.ExitFunctionLink =
        reinterpret_cast<uint64_t>(&dispatchExitWithoutBlockLinking);
}

// Published by the maintained downstream translator patch. Arming is only ever
// called from ordinary code, with no FEX translation in flight; the self-test
// is compiled into the same libFEXCore.a the application links, so running it
// here proves this binary's production encoder rather than a host rebuild.
extern "C" void FEX_BoxedWineIRCapArm(uint64_t target);
extern "C" uint64_t FEX_BoxedWineIRCapCurrentTarget(void);
extern "C" uint32_t FEX_BoxedWineEmitterSelfTest(uint32_t* actual);

std::mutex gLiveMutex;
std::unordered_map<void*, std::unique_ptr<LiveProcessState>> gLiveProcesses;
std::unordered_map<FEXCore::Core::InternalThreadState*,
                   FEXCore::Context::Context*> gLiveThreadContexts;

FEXCore::HLE::ExecutableRangeInfo queryLiveExecutableRange(uint64_t address);
void invalidateLiveExecutableRange(FEXCore::Core::InternalThreadState* thread,
                                   uint64_t start, uint64_t length);

void reportf(const char* format, ...) __attribute__((format(printf, 1, 2)));
void reportf(const char* format, ...) {
    char line[768];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    {
        std::lock_guard<std::mutex> guard(gReportMutex);
        gReport.append(line);
        gReport.push_back('\n');
    }
    BVNLogWrite(BVNLogLevelInfo, "fex64", line);
}

size_t alignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool poolOwns(const void* pointer) {
    return gCodeSegments.find(reinterpret_cast<uintptr_t>(pointer)) != boxedvn::FexCodeSegments::maximum;
}

int64_t codeWriteOffset(const void* pointer) {
    const uintptr_t rx = reinterpret_cast<uintptr_t>(pointer);
    const uintptr_t rw = gCodeSegments.writable(rx);
    return rw ? static_cast<int64_t>(rw) - static_cast<int64_t>(rx) : 0;
}

// Called under gPoolMutex. Published mappings remain immutable, including
// while signal handlers translate RX addresses without acquiring a lock.
bool addCodeSegment(size_t minimumRequest) {
    if (gCodeSegments.size() == boxedvn::FexCodeSegments::maximum) return false;
    for (size_t candidate = kPoolBytes; candidate >= kMinimumPoolBytes; candidate /= 2) {
        if (!boxedvn::planFexCodeBufferLayout(0, minimumRequest, candidate, kPageBytes, kFEXPageBytes)) continue;
        void* rx = BVNExecMemAlloc(candidate);
        if (!rx) continue;
        void* rw = BVNExecMemWritableAddress(rx, candidate);
        if (!rw || !gCodeSegments.append({reinterpret_cast<uintptr_t>(rx), reinterpret_cast<uintptr_t>(rw), candidate})) {
            BVNExecMemReleaseIfOwned(rx, candidate);
            return false;
        }
        reportf("BOXEDWINE_FEX64_CODE_SEGMENT index=%zu bytes=%zu rx=%p rw=%p",
                gCodeSegments.size() - 1, candidate, rx, rw);
        return true;
    }
    return false;
}

void* allocateTranslatedCode(size_t length) {
    std::lock_guard<std::mutex> guard(gPoolMutex);
    for (size_t i = 0;; ++i) {
        if (i == gCodeSegments.size() && !addCodeSegment(length)) break;
        const auto& segment = gCodeSegments.at(i);
        const auto before = gCodePools[i].cursor();
        const auto allocation = gCodePools[i].allocate(length, segment.size, kPageBytes, kFEXPageBytes);
        if (!allocation) continue;
        gPoolUsed.fetch_add(gCodePools[i].cursor() - before, std::memory_order_relaxed);
        static std::atomic<uint32_t> reported {0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 32) {
            reportf("FEX executable %s segment=%zu request=0x%zx rx_offset=0x%zx guard_offset=0x%zx next_offset=0x%zx",
                    allocation->reused ? "reuse" : "carve", i, length,
                    allocation->layout.allocationOffset, allocation->layout.guardOffset, allocation->layout.nextOffset);
        }
        return reinterpret_cast<void*>(segment.rx + allocation->layout.allocationOffset);
    }
    reportf("translator executable pool exhausted request=%zu segments=%zu used=%zu",
            length, gCodeSegments.size(), gPoolUsed.load());
    // MAP_FAILED is non-null and FEX would emit through it. Propagate failure.
    throw std::bad_alloc();
}

void* fexMmap(void* address, size_t length, int protection, int flags,
              int descriptor, off_t offset) {
    if ((protection & PROT_EXEC) != 0) {
        return allocateTranslatedCode(length);
    }
    return mmap(address, length, protection, flags, descriptor, offset);
}

int fexMunmap(void* address, size_t length) {
    if (!poolOwns(address)) return munmap(address, length);
    std::lock_guard<std::mutex> guard(gPoolMutex);
    const auto index = gCodeSegments.find(reinterpret_cast<uintptr_t>(address));
    const size_t offset = reinterpret_cast<uintptr_t>(address) - gCodeSegments.at(index).rx;
    if (!gCodePools[index].release(offset, length)) {
        reportf("FEX executable release did not match a live carve request=0x%zx segment=%zu offset=0x%zx",
                length, index, offset);
    }
    return 0;
}

void fexLog(LogMan::DebugLevels level, const char* message) {
    reportf("FEX[%u] %s", static_cast<unsigned>(level), message);
}

void fexThrow(const char* message) {
    reportf("FEX failure: %s", message);
}

FEXCore::HostFeatures appleHostFeatures() {
    FEXCore::HostFeatures features {};
    features.DCacheLineSize = 64;
    features.ICacheLineSize = 64;
    features.SupportsCacheMaintenanceOps = true;
    features.SupportsAES = true;
    features.SupportsCRC = true;
    features.SupportsSHA = true;
    features.SupportsPMULL_128Bit = true;
    features.SupportsAtomics = true;
    features.SupportsRCPC = true;
    features.SupportsTSOImm9 = true;
    features.SupportsFCMA = true;
    features.SupportsFlagM = true;
    features.SupportsFlagM2 = true;
    // AVX does not need SVE on this host, and saying otherwise made cpuid
    // describe a CPU no modern program will run on.
    //
    // FEX carries a second VEX decode table for exactly this case:
    // Decoder::Decoder picks VEXTableOps_AVX128 when SupportsAVX is set and
    // SupportsSVE256 is not, and the AVX-128 dispatcher lowers every 256-bit
    // operation to two 128-bit halves. The JIT then cannot emit a 256-bit
    // instruction at all, because HostSupportsAVX256 is
    // `SupportsAVX && SupportsSVE256`; SVE is used only to make a handful of
    // gathers faster, never as a requirement. Upstream's own host-feature
    // probe sets SupportsAVX unconditionally for that reason.
    //
    // With SupportsAVX false, FEX's CPUID reported no AVX (leaf 1 ECX bit
    // 28), no FMA3 (bit 12), no F16C (bit 29), no XSAVE/OSXSAVE (bits 26/27),
    // and no AVX2, BMI1 or BMI2 (leaf 7 EBX bits 5, 3 and 8), and XCR0 kept
    // only its x87 and SSE bits. A program that gates on any of those decides
    // the machine is unsupported before it opens a file.
    //
    // The 256-bit halves live in State.avx_high inside the frame rather than
    // in a static host register -- Arm64Emitter spills and fills only
    // State.xmm.sse.data unless SVE256 is present. The CPU64 adapter exchanges
    // both halves through FEX's reconstruction APIs so a Linux signal frame
    // can preserve AVX state and honor Wine's changes to the saved context.
    features.SupportsAVX = true;
    features.SupportsSVE128 = false;
    features.SupportsSVE256 = false;
    // Mirrors upstream: VAES (leaf 7 ECX bit 9) is the 256-bit spelling of
    // the AES instructions, and the AVX-128 dispatcher implements each of
    // them as two 128-bit AES operations.
    features.SupportsAES256 = features.SupportsAVX && features.SupportsAES;
    int cores = 0;
    size_t length = sizeof(cores);
    if (sysctlbyname("hw.ncpu", &cores, &length, nullptr, 0) != 0 || cores < 1) {
        cores = 4;
    }
    features.CPUMIDRs.resize(static_cast<size_t>(cores), 0x611F0000);
    return features;
}

class BoxedWineSyscalls final : public FEXCore::HLE::SyscallHandler {
public:
    explicit BoxedWineSyscalls(boxedvn::FexGuestMode mode)
        : mode_(mode) {
        OSABI = mode == boxedvn::FexGuestMode::X86_32
            ? FEXCore::HLE::SyscallOSABI::OS_LINUX32
            : FEXCore::HLE::SyscallOSABI::OS_LINUX64;
    }

    uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame* frame,
                           FEXCore::HLE::SyscallArguments* arguments) override {
        // The IA-32 register/syscall adapter is intentionally not admitted by
        // createFEXContext yet. Keep this guard as a second line of defence
        // so a future caller cannot accidentally route 32-bit state through
        // the CPU64 bridge below.
        if (mode_ != boxedvn::FexGuestMode::X86_64) {
            reportf("FEX x86-32 syscall dispatch rejected: Linux32 adapter is not implemented");
            return static_cast<uint64_t>(-38); // ENOSYS
        }
        if (BVNFEXCPU64Adapter* adapter = BVNFEXCPU64AdapterCurrent()) {
            const uint64_t result = BVNFEXCPU64AdapterHandleSyscall(
                adapter, frame, arguments->Argument);
            const BVNFEXCPU64AdapterAction action =
                BVNFEXCPU64AdapterLastAction(adapter);
            if (action == BVNFEXCPU64AdapterActionYield ||
                action == BVNFEXCPU64AdapterActionThreadExit ||
                action == BVNFEXCPU64AdapterActionProcessExit ||
                action == BVNFEXCPU64AdapterActionExec ||
                action == BVNFEXCPU64AdapterActionInvalid) {
                if (!gCanJump) {
                    reportf("BoxedWine requested a guest stop outside the guarded execution window");
                    return static_cast<uint64_t>(-1);
                }
                frame->InSyscallInfo = 0;
                std::longjmp(gExitJump, 1);
            }
            return result;
        }
        if (gActiveCPU != nullptr) {
            CPU64* cpu = gActiveCPU;
            cpu->reg[X64_RAX].setU64(arguments->Argument[0]);
            cpu->reg[X64_RDI].setU64(arguments->Argument[1]);
            cpu->reg[X64_RSI].setU64(arguments->Argument[2]);
            cpu->reg[X64_RDX].setU64(arguments->Argument[3]);
            cpu->reg[X64_R10].setU64(arguments->Argument[4]);
            cpu->reg[X64_R8].setU64(arguments->Argument[5]);
            cpu->reg[X64_R9].setU64(arguments->Argument[6]);
            const uint64_t syscallNumber = arguments->Argument[0];
            const uint64_t exitCode = arguments->Argument[1];
            ksyscall64(cpu);
            if (cpu->yield) {
                if (syscallNumber == 60 || syscallNumber == 231) {
                    gProbeExitCode.store(exitCode, std::memory_order_release);
                    gProbeExited.store(true, std::memory_order_release);
                }
                if (!gCanJump) {
                    reportf("BoxedWine requested a guest stop outside the guarded execution window");
                    return static_cast<uint64_t>(-1);
                }
                frame->InSyscallInfo = 0;
                std::longjmp(gExitJump, 1);
            }
            return cpu->reg[X64_RAX].u64;
        }
        boxedvn::Linux64Syscall syscall;
        syscall.number = arguments->Argument[0];
        for (size_t index = 0; index < syscall.arguments.size(); ++index) {
            syscall.arguments[index] = arguments->Argument[index + 1];
        }
        const boxedvn::Linux64SyscallResult result = gKernel.dispatch(syscall);
        if (result.action != boxedvn::Linux64SyscallAction::Continue) {
            if (!gCanJump) {
                reportf("guest exit arrived outside the guarded execution window");
                return static_cast<uint64_t>(-1);
            }
            frame->InSyscallInfo = 0;
            std::longjmp(gExitJump, 1);
        }
        return static_cast<uint64_t>(result.value);
    }

    FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(
        FEXCore::Core::InternalThreadState*, uint64_t address) override {
        if (BVNFEXCPU64AdapterCurrent() != nullptr) {
            return queryLiveExecutableRange(address);
        }
        const auto range = gAddressSpace.executableRange(address);
        if (!range.has_value()) {
            return {0, 0, false};
        }
        return {range->guestBase, range->size, false};
    }

    void InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState* thread,
                                  uint64_t start, uint64_t length) override {
        invalidateLiveExecutableRange(thread, start, length);
    }

    std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(
        FEXCore::Core::InternalThreadState*, uint64_t) override {
        return std::nullopt;
    }

private:
    boxedvn::FexGuestMode mode_;
};

class BoxedWineSignals final : public FEXCore::SignalDelegator {
public:
    explicit BoxedWineSignals(boxedvn::FexGuestMode mode) : mode_(mode) {}

private:
    boxedvn::FexGuestMode mode_;
};

std::unique_ptr<FEXContextBundle> createFEXContext(
    boxedvn::FexGuestMode mode,
    bool (*createInitialThread)(FEXContextBundle&));

FEXCore::Core::InternalThreadState* createFEXThread(
    FEXContextBundle& bundle, uint64_t rip, uint64_t stack,
    const FEXCore::Core::CPUState* state = nullptr);

constexpr std::array<int, 4> kFEXHostSignals {
    SIGSEGV, SIGBUS, SIGILL, SIGFPE,
};
std::array<struct sigaction, kFEXHostSignals.size()> gPreviousFEXSignals {};
std::once_flag gFEXSignalInstallOnce;
std::atomic<bool> gFEXSignalHandlersInstalled {false};
std::atomic<uint64_t> gFEXHostFaultCount {0};
std::atomic<uint64_t> gFEXKuserFaultCount {0};
std::atomic<uint64_t> gFEXHandledHostFaultCount {0};
std::atomic<uint64_t> gFEXLastHostSignal {0};
std::atomic<uint64_t> gFEXLastHostFaultAddress {0};
std::atomic<uint64_t> gFEXLastHostFaultPC {0};
std::atomic<uint64_t> gExecutionTracePoll {0};

struct ExecutionTraceSnapshot {
    uint64_t poll = 0;
    U32 processId = 0;
    U32 threadId = 0;
    uint64_t processEntryRIP = 0;
    integer_t runState = 0;
    integer_t cpuUsage = 0;
    uint64_t hostPC = 0;
    uint64_t hostLR = 0;
    uint64_t hostSP = 0;
    std::array<uint64_t, 8> hostArguments {};
    std::array<uint64_t, 10> hostCallRegisters {};
    uint64_t hostCodeAddress = 0;
    std::array<uint32_t, 8> hostCode {};
    bool hostCodeValid = false;
    uint64_t hostCallGuestRIP = 0;
    uint64_t hostCallCodeAddress = 0;
    std::array<uint32_t, 8> hostCallCode {};
    bool hostCallCodeValid = false;
    uint64_t hostReturnCodeAddress = 0;
    std::array<uint32_t, 16> hostReturnCode {};
    bool hostReturnCodeValid = false;
    bool hostReturnInPool = false;
    uint64_t hostReturnPoolOffset = 0;
    uint64_t hostReturnWritableAlias = 0;
    uint64_t guestRIP = 0;
    uint64_t frameRIP = 0;
    uint64_t fsBase = 0;
    uint64_t gsBase = 0;
    uint64_t inSyscallInfo = 0;
    std::array<uint64_t, 16> guestGPRs {};
    uint32_t liveGPRMask = 0;
    uint32_t memoryValueMask = 0;
    uint64_t chunkSizeWord = 0;
    uint64_t chunkUserWord = 0;
    uint64_t chunkKeyWord = 0;
    uint64_t arenaLockWord = 0;
    uint64_t arenaSystemMem = 0;
    uint64_t tlsOperandWord = 0;
    uint64_t hostFaults = 0;
    uint64_t handledFaults = 0;
    uint64_t newFaults = 0;
    uint64_t lastSignal = 0;
    uint64_t lastFaultAddress = 0;
    uint64_t lastFaultPC = 0;
    uint32_t stablePolls = 0;
    bool emitSample = false;
    bool emitWarning = false;
    size_t historyCount = 0;
    std::array<uint64_t, 8> guestHistory {};
};

enum ExecutionTraceMemoryValue : uint32_t {
    TraceChunkSizeWord = 1u << 0,
    TraceChunkUserWord = 1u << 1,
    TraceChunkKeyWord = 1u << 2,
    TraceArenaLockWord = 1u << 3,
    TraceArenaSystemMem = 1u << 4,
    TraceTlsOperandWord = 1u << 5,
};

bool readNativeGuestQword(KMemory64* memory, uint64_t address,
                          uint64_t& value) {
    if (!memory || address > std::numeric_limits<uint64_t>::max() - 7) {
        return false;
    }
    if (!memory->nativeIdentityMode() ||
        !memory->nativeGuestRangeAllowed(address, sizeof(value))) {
        return false;
    }
    vm_size_t bytesRead = 0;
    return vm_read_overwrite(
        mach_task_self(), k64GuestToHostAddress(address), sizeof(value),
        reinterpret_cast<vm_address_t>(&value), &bytesRead) ==
            KERN_SUCCESS &&
        bytesRead == sizeof(value);
}

const char* machRunStateName(integer_t state) {
    switch (state) {
        case TH_STATE_RUNNING: return "running";
        case TH_STATE_STOPPED: return "stopped";
        case TH_STATE_WAITING: return "waiting";
        case TH_STATE_UNINTERRUPTIBLE: return "uninterruptible";
        case TH_STATE_HALTED: return "halted";
        default: return "unknown";
    }
}

size_t fexSignalIndex(int signal) {
    for (size_t index = 0; index < kFEXHostSignals.size(); ++index) {
        if (kFEXHostSignals[index] == signal) return index;
    }
    return kFEXHostSignals.size();
}

void chainFEXHostSignal(int signal, siginfo_t* info, void* ucontext) {
    const size_t index = fexSignalIndex(signal);
    if (index == kFEXHostSignals.size()) return;
    const struct sigaction& previous = gPreviousFEXSignals[index];
    const uintptr_t previousAction =
        reinterpret_cast<uintptr_t>(previous.sa_sigaction);
    if ((previous.sa_flags & SA_SIGINFO) != 0 && previousAction > 1) {
        previous.sa_sigaction(signal, info, ucontext);
        return;
    }
    if (previous.sa_handler == SIG_IGN) return;
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != nullptr) {
        previous.sa_handler(signal);
        return;
    }
    // Restore the action that was installed before FEX and re-raise so a
    // non-FEX fault retains BoxedWine/Darwin's original default semantics.
    sigaction(signal, &previous, nullptr);
    raise(signal);
}

extern "C" void boxedwineDxmtReportRecentCalls(void);

// A declined fault is about to end the process. Two device runs declined a
// SIGSEGV at the same shared-cache PC reading a guest address, and the PC
// alone named nothing: every translated pointer site had been audited. The
// symbolicated frame chain says which host function was reading, and which
// unix call it was reached from; the ring of recent DXMT calls says what the
// guest asked for last. Frame records are copied with vm_read_overwrite so
// a corrupt chain stops the walk instead of faulting inside the handler.
static void reportDeclinedFaultContext(uint64_t pc, ucontext_t* context) {
    auto describe = [](const char* tag, unsigned frame, uint64_t address) {
        Dl_info image {};
        if (address != 0 &&
            dladdr(reinterpret_cast<const void*>(address), &image) != 0) {
            const uint64_t imageBase = reinterpret_cast<uint64_t>(image.dli_fbase);
            const uint64_t symbolBase = reinterpret_cast<uint64_t>(image.dli_saddr);
            reportf("%s frame=%u pc=0x%llx image=%s image_offset=0x%llx symbol=%s "
                    "symbol_offset=0x%llx",
                    tag, frame, static_cast<unsigned long long>(address),
                    image.dli_fname ? image.dli_fname : "(unknown)",
                    static_cast<unsigned long long>(address - imageBase),
                    image.dli_sname ? image.dli_sname : "(unknown)",
                    static_cast<unsigned long long>(
                        symbolBase != 0 ? address - symbolBase : 0));
        } else {
            reportf("%s frame=%u pc=0x%llx image=(none)", tag, frame,
                    static_cast<unsigned long long>(address));
        }
    };
    describe("BOXEDWINE_FEX64_FAULT_DECLINED_FRAME", 0, pc);
    if (context == nullptr || context->uc_mcontext == nullptr) return;
    uint64_t lr = context->uc_mcontext->__ss.__lr;
    uint64_t fp = context->uc_mcontext->__ss.__fp;
    describe("BOXEDWINE_FEX64_FAULT_DECLINED_FRAME", 1, lr);
    for (unsigned frame = 2; frame < 24; ++frame) {
        if (fp == 0 || (fp & 0xf) != 0) break;
        uint64_t record[2] = {0, 0};
        vm_size_t copied = 0;
        if (vm_read_overwrite(mach_task_self(), static_cast<vm_address_t>(fp),
                              sizeof(record),
                              reinterpret_cast<vm_address_t>(record),
                              &copied) != KERN_SUCCESS ||
            copied != sizeof(record)) {
            break;
        }
        if (record[1] == 0) break;
        describe("BOXEDWINE_FEX64_FAULT_DECLINED_FRAME", frame, record[1]);
        if (record[0] <= fp) break;
        fp = record[0];
    }
    boxedwineDxmtReportRecentCalls();
}

void fexHostSignalHandler(int signal, siginfo_t* info, void* ucontext) {
    gFEXHostFaultCount.fetch_add(1, std::memory_order_relaxed);
    gFEXLastHostSignal.store(static_cast<uint64_t>(signal),
                             std::memory_order_relaxed);
    if (info && info->si_addr) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(info->si_addr);
        gFEXLastHostFaultAddress.store(address, std::memory_order_relaxed);
        if (address >= K64_KUSER_SHARED_BASE &&
            address < K64_KUSER_SHARED_BASE + K64_KUSER_SHARED_SIZE) {
            gFEXKuserFaultCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
    auto* context = static_cast<ucontext_t*>(ucontext);
    if (context && context->uc_mcontext) {
        gFEXLastHostFaultPC.store(context->uc_mcontext->__ss.__pc,
                                  std::memory_order_relaxed);
    }
    BVNFEXCPU64Adapter* adapter = BVNFEXCPU64AdapterCurrent();
    FEXCore::SignalDelegator* signals = gActiveSignals;
    if (signals == nullptr && gProbeContext != nullptr) {
        signals = gProbeContext->signals.get();
    }
    if (signals == nullptr) {
        chainFEXHostSignal(signal, info, ucontext);
        return;
    }
    if (adapter && BVNFEXCPU64AdapterHandleHostFault(
            adapter, &signals->GetConfig(), signal, info, ucontext)) {
        gFEXHandledHostFaultCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Declining means this fault leaves FEX's world. Say so once per handful,
    // with the host PC and where it sits relative to the dispatcher, because a
    // declined fault taken inside FEX's own code is a translator defect and is
    // otherwise indistinguishable from an unrelated host crash.
    if (adapter != nullptr) {
        static std::atomic<uint32_t> declined {0};
        if (declined.fetch_add(1, std::memory_order_relaxed) < 8) {
            const auto& config = signals->GetConfig();
            const uint64_t pc = context && context->uc_mcontext
                ? context->uc_mcontext->__ss.__pc : 0;
            const bool inDispatcher = config.DispatcherEnd > config.DispatcherBegin &&
                pc >= config.DispatcherBegin && pc < config.DispatcherEnd;
            reportf("BOXEDWINE_FEX64_FAULT_DECLINED signal=%d host_pc=0x%llx "
                    "address=0x%llx dispatcher=%d dispatcher_range=[0x%llx,0x%llx)",
                    signal, static_cast<unsigned long long>(pc),
                    static_cast<unsigned long long>(
                        reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr)),
                    inDispatcher ? 1 : 0,
                    static_cast<unsigned long long>(config.DispatcherBegin),
                    static_cast<unsigned long long>(config.DispatcherEnd));
            reportDeclinedFaultContext(pc, context);
        }
    } else {
        static std::atomic<uint32_t> hostFaults {0};
        if (hostFaults.fetch_add(1, std::memory_order_relaxed) < 4) {
            const uint64_t pc = context && context->uc_mcontext
                ? context->uc_mcontext->__ss.__pc : 0;
            reportf("BOXEDWINE_HOST_FAULT signal=%d host_pc=0x%llx address=0x%llx "
                    "mach_thread=0x%x main=%d",
                    signal, static_cast<unsigned long long>(pc),
                    static_cast<unsigned long long>(
                        reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr)),
                    static_cast<unsigned>(pthread_mach_thread_np(pthread_self())),
                    pthread_main_np() ? 1 : 0);
            if (context) reportDeclinedFaultContext(pc, context);
        }
    }
    chainFEXHostSignal(signal, info, ucontext);
}

static void hostUncaughtExceptionHandler(NSException* exception) {
    reportf("BOXEDWINE_HOST_EXCEPTION name=%s reason=%s",
            exception.name.UTF8String ?: "?",
            exception.reason.UTF8String ?: "?");
    NSUInteger index = 0;
    for (NSString* frame in exception.callStackSymbols) {
        if (index++ >= 32) break;
        reportf("BOXEDWINE_HOST_EXCEPTION_FRAME %s", frame.UTF8String);
    }
}

// An abort (a failed assertion, std::terminate, a libc++ hard error) ended
// the app with nothing in the session log: the translator's handlers cover
// the fault signals only. The abort now names its thread and frames the
// same way a declined fault does, then continues to the default action.
static void hostAbortHandler(int signal, siginfo_t*, void* ucontext) {
    ucontext_t* context = static_cast<ucontext_t*>(ucontext);
    const uint64_t pc = context ? context->uc_mcontext->__ss.__pc : 0;
    reportf("BOXEDWINE_HOST_ABORT signal=%d pc=0x%llx mach_thread=0x%x",
            signal, static_cast<unsigned long long>(pc),
            static_cast<unsigned>(pthread_mach_thread_np(pthread_self())));
    if (context) reportDeclinedFaultContext(pc, context);
    struct sigaction restore {};
    restore.sa_handler = SIG_DFL;
    sigemptyset(&restore.sa_mask);
    sigaction(signal, &restore, nullptr);
    raise(signal);
}

bool installFEXHostSignalHandlers() {
    std::call_once(gFEXSignalInstallOnce, [] {
        bool installed = true;
        size_t installedCount = 0;
        for (size_t index = 0; index < kFEXHostSignals.size(); ++index) {
            struct sigaction action {};
            sigemptyset(&action.sa_mask);
            action.sa_sigaction = fexHostSignalHandler;
            action.sa_flags = SA_SIGINFO;
            if (sigaction(kFEXHostSignals[index], &action,
                          &gPreviousFEXSignals[index]) != 0) {
                installed = false;
                break;
            }
            ++installedCount;
        }
        if (!installed) {
            for (size_t index = 0; index < installedCount; ++index) {
                sigaction(kFEXHostSignals[index], &gPreviousFEXSignals[index],
                          nullptr);
            }
            return;
        }
        struct sigaction abortAction {};
        sigemptyset(&abortAction.sa_mask);
        abortAction.sa_sigaction = hostAbortHandler;
        abortAction.sa_flags = SA_SIGINFO;
        sigaction(SIGABRT, &abortAction, nullptr);
        NSSetUncaughtExceptionHandler(&hostUncaughtExceptionHandler);
        gFEXSignalHandlersInstalled.store(true, std::memory_order_release);
    });
    return gFEXSignalHandlersInstalled.load(std::memory_order_acquire);
}

bool preparePool() {
    std::lock_guard<std::mutex> guard(gPoolMutex);
    if (gCodeSegments.size() != 0) return true;
    if (!BVNExecMemArenaPrepared() && !BVNExecMemPrepareArena()) {
        reportf("StikDebug did not prepare the shared executable arena");
        return false;
    }
    if (!BVNExecMemExecutionConfirmed()) {
        BVNExecMemProbe(true);
        if (!BVNExecMemExecutionConfirmed()) {
            reportf("the shared executable arena failed its execution check: %s",
                    BVNExecMemReport());
            return false;
        }
    }
    if (!addCodeSegment(kPageBytes)) {
        reportf("the shared executable arena has no segment for FEX");
        return false;
    }
    FEXCore::DualMap::WriteOffsetLookup = &codeWriteOffset;
    return true;
}

std::once_flag gFEXGlobalsOnce;
bool gFEXGlobalsReady = false;

bool initializeFEXGlobals() {
    if (!preparePool()) return false;
    std::call_once(gFEXGlobalsOnce, [] {
        // The pinned iOS FEX fork carries an environment-gated escape hatch
        // for a known non-converging DeadFlagCalculationEliminination pass.
        // BoxedWine's ELF loader reaches that complex control-flow path before
        // Wine can issue its second syscall, so keep the rest of the optimiser
        // enabled while omitting only the unstable pass.
        if (setenv("MYTHIC_NO_DFE", "1", 1) != 0) {
            reportf("could not disable the unstable FEX dead-flag pass");
            return;
        }
        reportf("FEX dead-flag optimisation disabled for deterministic guest startup");
        FEXCore::Allocator::mmap = fexMmap;
        FEXCore::Allocator::munmap = fexMunmap;
        LogMan::Msg::InstallHandler(fexLog);
        LogMan::Throw::InstallHandler(fexThrow);
        FEXCore::Config::Initialize();
        FEXCore::Config::Set(FEXCore::Config::ConfigOption::CONFIG_IS64BIT_MODE, "1");
        // The bundled ELF loader exercises dense, cyclic control flow before
        // Wine reaches its first process boundary.  Keep each compiled unit to
        // one basic block while the BoxedWine backend is being brought up.  It
        // avoids optimizer/linker ambiguity without disabling the JIT, and is
        // still an optional FEX-only policy.
        FEXCore::Config::Set(FEXCore::Config::ConfigOption::CONFIG_MULTIBLOCK, "0");
        reportf("FEX multiblock compilation disabled for deterministic guest control flow");
        // Do not use CONFIG_MAXINST as a live-guest diagnostic. Forcing every
        // instruction through a separate exit/link sequence amplified the iOS
        // linker-lock defect and changed a later stall into an immediate host
        // fault. The ordinary decoder limit preserves the last known-good
        // execution boundary while that linker path is diagnosed directly.
        // Prove this binary's own encoder once. The device reported an
        // invalid host word for an unscaled 128-bit vector store while CI
        // proved the encoding correct from a separate host build; running the
        // production helper from the linked libFEXCore.a removes that
        // ambiguity from the next device log.
        uint32_t emitted = 0;
        const uint32_t expected = FEX_BoxedWineEmitterSelfTest(&emitted);
        reportf("BOXEDWINE_FEX64_EMITTER_SELFTEST actual=0x%08x "
                "expected=0x%08x pass=%d",
                emitted, expected, emitted == expected ? 1 : 0);
        gFEXGlobalsReady = true;
    });
    return gFEXGlobalsReady;
}

class FEXGuestModeConfigScope final {
public:
    explicit FEXGuestModeConfigScope(boxedvn::FexGuestMode mode) {
        if (auto current = FEXCore::Config::Get(
                FEXCore::Config::ConfigOption::CONFIG_IS64BIT_MODE);
            current.has_value() && *current != nullptr) {
            previous_ = std::string(**current);
        }
        FEXCore::Config::Set(
            FEXCore::Config::ConfigOption::CONFIG_IS64BIT_MODE,
            mode == boxedvn::FexGuestMode::X86_64 ? "1" : "0");
    }

    ~FEXGuestModeConfigScope() {
        if (previous_.has_value()) {
            FEXCore::Config::Set(
                FEXCore::Config::ConfigOption::CONFIG_IS64BIT_MODE,
                *previous_);
        } else {
            FEXCore::Config::Erase(
                FEXCore::Config::ConfigOption::CONFIG_IS64BIT_MODE);
        }
    }

    FEXGuestModeConfigScope(const FEXGuestModeConfigScope&) = delete;
    FEXGuestModeConfigScope& operator=(const FEXGuestModeConfigScope&) = delete;

private:
    std::optional<std::string> previous_;
};

std::unique_ptr<FEXContextBundle> createFEXContext(
    boxedvn::FexGuestMode mode,
    bool (*createInitialThread)(FEXContextBundle&)) {
    if (!boxedvn::fexGuestModeAdmitted(mode)) {
        reportf("FEX context rejected mode=%s: Linux32 syscall adapter is not implemented",
                boxedvn::fexGuestModeName(mode));
        return nullptr;
    }
    if (!initializeFEXGlobals()) {
        reportf("FEX context rejected mode=%s: global initialization failed",
                boxedvn::fexGuestModeName(mode));
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(gFEXContextConstructionMutex);
    // This scope covers handler construction, InitCore, and the optional
    // initial CreateThread callback. FEX consults the process-global bitness
    // option during these operations, so restoring it only after InitCore is
    // insufficient for a concurrent 32/64-bit context factory.
    FEXGuestModeConfigScope modeScope(mode);
    auto bundle = std::make_unique<FEXContextBundle>();
    bundle->mode = mode;
    bundle->signals = std::make_unique<BoxedWineSignals>(mode);
    bundle->syscalls = std::make_unique<BoxedWineSyscalls>(mode);
    const FEXCore::HostFeatures hostFeatures = appleHostFeatures();
    bundle->context = FEXCore::Context::Context::CreateNewContext(hostFeatures);
    if (!bundle->context) {
        reportf("FEX refused to create a %s translator context",
                boxedvn::fexGuestModeName(mode));
        return nullptr;
    }
    // What the guest's own cpuid will answer, printed once per context and
    // read straight out of the emulator rather than recomputed here, so the
    // line cannot drift from what a program actually sees. CPUIDEmu is built
    // by the context constructor from the very HostFeatures above, so these
    // are valid before any guest code is translated.
    //
    // A program that shows a bare "unsupported system" message having opened
    // no file and loaded no further module has read something like this and
    // nothing else; leaf 1 ECX and leaf 7 EBX are where the whole
    // AVX/AVX2/FMA/F16C/BMI question lands, and XCR0 is the second half of
    // the answer for any caller that checks OSXSAVE and then xgetbv.
    {
        const FEXCore::CPUID::FunctionResults leaf1 =
            bundle->context->RunCPUIDFunction(1, 0);
        const FEXCore::CPUID::FunctionResults leaf7 =
            bundle->context->RunCPUIDFunction(7, 0);
        const FEXCore::CPUID::XCRResults xcr0 =
            bundle->context->RunXCRFunction(0);
        reportf("BOXEDWINE_FEX64_CPUID mode=%s leaf1_ecx=0x%08x "
                "leaf1_edx=0x%08x leaf7_ebx=0x%08x leaf7_ecx=0x%08x "
                "xcr0=0x%08x%08x cores=%zu avx=%u avx2=%u fma=%u f16c=%u "
                "bmi1=%u bmi2=%u movbe=%u xsave=%u osxsave=%u sse42=%u",
                boxedvn::fexGuestModeName(mode),
                leaf1.ecx, leaf1.edx, leaf7.ebx, leaf7.ecx,
                xcr0.edx, xcr0.eax,
                hostFeatures.CPUMIDRs.size(),
                (leaf1.ecx >> 28) & 1u, (leaf7.ebx >> 5) & 1u,
                (leaf1.ecx >> 12) & 1u, (leaf1.ecx >> 29) & 1u,
                (leaf7.ebx >> 3) & 1u, (leaf7.ebx >> 8) & 1u,
                (leaf1.ecx >> 22) & 1u, (leaf1.ecx >> 26) & 1u,
                (leaf1.ecx >> 27) & 1u, (leaf1.ecx >> 20) & 1u);
    }
    bundle->context->SetSignalDelegator(bundle->signals.get());
    bundle->context->SetSyscallHandler(bundle->syscalls.get());
    // The translator emits plain loads and stores when told the hardware
    // orders memory the x86 way. iOS apps cannot enable Apple's TSO mode, so
    // that leaves lock-free traffic between Wine and DXMT threads weakly
    // ordered. Strict ordering makes FEX emit acquire/release accesses
    // instead; unaligned ones then trap once and are backpatched.
    const bool strictOrdering = [[NSUserDefaults standardUserDefaults]
        boolForKey:@"BoxedVN.fex64.strictMemoryOrdering"];
    bundle->context->SetHardwareTSOSupport(!strictOrdering);
    reportf("FEX memory ordering: %s",
            strictOrdering ? "emulated TSO (strict)" : "hardware TSO assumed");
    // Canonical low guest addresses -- Wine's below-2-GiB TEB reservation,
    // KUSER_SHARED_DATA, and ordinary PE image bases -- cannot be host-mapped
    // at their own address on iOS. KMemory64 backs them through a
    // deterministic high alias, so the translator has to dereference them
    // there too. Published before InitCore, so no guest code is ever
    // translated without it. Guest RIPs, register values and syscall
    // arguments stay canonical; only host dereferences are translated.
    bundle->context->SetGuestLowAlias(boxedvn::kGuestLowAliasBase,
                                      boxedvn::kGuestLowLimit);
    // Wine's top-down arena is the one lane the OR cannot reach, so it is
    // relocated by clearing one bit field of the address. Published beside the
    // alias base and before InitCore for the same reason: no guest code may be
    // translated without it, or a block would dereference the canonical arena
    // at an address the host cannot map.
    bundle->context->SetGuestTopClearMask(boxedvn::kGuestTopClearMask);
    // Every CALL then records the return address it pushed, the canonical and
    // host addresses it pushed it to, and what an immediate read-back of that
    // same host address returned. Nothing is printed until a return actually
    // fails, at which point the ring names the CALL that was supposed to have
    // written the slot.
    //
    // This is on for the device path on purpose: two runs ended with a guest
    // frame chain whose saved frame pointers were all correct and whose return
    // addresses were all zero, and nothing observable at RET time can say
    // whether the push never landed or the slot was cleared afterwards. The
    // environment variable is the off switch, so a throughput measurement does
    // not need a different build.
    const bool callWitness = getenv("BW64_NO_CALL_WITNESS") == nullptr;
    bundle->context->SetBoxedWineCallWitness(callWitness);
    reportf("BOXEDWINE_FEX64_CALL_WITNESS armed=%d", callWitness ? 1 : 0);
    // Wine's WoW64 layer far-jumps a 64-bit thread into a 32-bit code segment
    // and back, so within one context and one thread the two decode modes
    // alternate. The translator already derives each block's width from the L
    // bit of the descriptor CS names; this lets that answer differ from the
    // mode the context was created with, and makes the translator invalidate
    // a guest code page that changes bitness, since its block cache is keyed
    // by RIP alone.
    //
    // Only for the 64-bit context: a 32-bit context has no 64-bit descriptor
    // to jump to, and its blocks are already decoded the one way it needs.
    // Published before InitCore, beside the alias, for the same reason -- no
    // guest code may be translated with the decision half made.
    const bool perBlockDecodeMode =
        mode == boxedvn::FexGuestMode::X86_64 && guestWow64ModeSwitchEnabled();
    bundle->context->SetGuestPerBlockDecodeMode(perBlockDecodeMode);
    reportf("BOXEDWINE_FEX64_MODE_SWITCH_ARMED armed=%d cs32=0x%x",
            perBlockDecodeMode ? 1 : 0,
            static_cast<unsigned>(K_WINE_X86_CODE_SELECTOR));
    if (!gLowAliasReported.exchange(true, std::memory_order_relaxed)) {
        reportf("BOXEDWINE_FEX64_LOW_ALIAS guest=[0x0,0x%llx) "
                "host=[0x%llx,0x%llx) high_identity=[0x%llx,0x%llx)",
                static_cast<unsigned long long>(boxedvn::kGuestLowLimit),
                static_cast<unsigned long long>(boxedvn::kGuestLowAliasBase),
                static_cast<unsigned long long>(boxedvn::kGuestLowAliasEnd),
                static_cast<unsigned long long>(boxedvn::kGuestHighBase),
                static_cast<unsigned long long>(boxedvn::kGuestHighEnd));
        reportf("BOXEDWINE_FEX64_TOP_ALIAS guest=[0x%llx,0x%llx) "
                "host=[0x%llx,0x%llx) clear_mask=0x%llx",
                static_cast<unsigned long long>(boxedvn::kGuestTopBase),
                static_cast<unsigned long long>(boxedvn::kGuestTopEnd),
                static_cast<unsigned long long>(boxedvn::kGuestTopHostBase),
                static_cast<unsigned long long>(boxedvn::kGuestTopHostEnd),
                static_cast<unsigned long long>(boxedvn::kGuestTopClearMask));
    }
    if (!bundle->context->InitCore()) {
        reportf("FEX failed to initialise its %s dispatcher",
                boxedvn::fexGuestModeName(mode));
        return nullptr;
    }
    if (createInitialThread != nullptr && !createInitialThread(*bundle)) {
        reportf("FEX failed to create the initial %s guest thread",
                boxedvn::fexGuestModeName(mode));
        return nullptr;
    }
    reportf("FEX context constructed mode=%s initial_thread=%p",
            boxedvn::fexGuestModeName(mode),
            static_cast<void*>(bundle->initialThread));
    return bundle;
}

FEXCore::Core::InternalThreadState* createFEXThread(
    FEXContextBundle& bundle, uint64_t rip, uint64_t stack,
    const FEXCore::Core::CPUState* state) {
    if (!boxedvn::fexGuestModeAdmitted(bundle.mode) ||
        bundle.context == nullptr) {
        reportf("FEX thread rejected mode=%s context=%p",
                boxedvn::fexGuestModeName(bundle.mode),
                static_cast<void*>(bundle.context.get()));
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(gFEXContextConstructionMutex);
    FEXGuestModeConfigScope modeScope(bundle.mode);
    return bundle.context->CreateThread(rip, stack, state);
}

bool primeFEXThread(FEXContextBundle& bundle,
                    FEXCore::Core::InternalThreadState* thread,
                    uint64_t rip) {
    if (!thread || !bundle.context || rip == 0) return false;
    std::lock_guard<std::mutex> lock(gFEXContextConstructionMutex);
    FEXGuestModeConfigScope modeScope(bundle.mode);
    bundle.context->CompileRIP(thread, rip);
    return true;
}

bool mapBundledELFProbe() {
    NSString* path = [[NSBundle mainBundle]
        pathForResource:@"boxedvn-fex64-kernel-probe" ofType:nil];
    if (path == nil) {
        reportf("the bundled ELF64 correctness probe is missing");
        return false;
    }
    NSData* data = [NSData dataWithContentsOfFile:path];
    if (data == nil) {
        reportf("the bundled ELF64 kernel process could not be read");
        return false;
    }
    const boxedvn::ELFImageInfo image = boxedvn::inspectELF(
        static_cast<const uint8_t*>(data.bytes), data.length);
    if (!image.valid ||
        image.architecture != boxedvn::ELFGuestArchitecture::X86_64) {
        reportf("the bundled ELF64 kernel process is invalid: %s",
                image.error.c_str());
        return false;
    }

    uint64_t first = std::numeric_limits<uint64_t>::max();
    uint64_t last = 0;
    for (const boxedvn::ELFLoadSegment& segment : image.loadSegments) {
        const uint64_t start = segment.virtualAddress & ~(uint64_t(kPageBytes) - 1);
        const uint64_t end = alignUp(segment.virtualAddress + segment.memorySize,
                                     kPageBytes);
        first = std::min(first, start);
        last = std::max(last, end);
    }
    if (first >= last || last - first > 16u * 1024u * 1024u) {
        reportf("the bundled ELF64 load span is invalid or unexpectedly large");
        return false;
    }
    const size_t mappingSize = static_cast<size_t>(last - first);

    // The probe deliberately runs from a CANONICAL LOW guest address so it
    // validates the alias the live guest depends on. The old code mmap'd
    // anonymously and used the iOS-selected host pointer as the guest address;
    // once instruction fetch started translating, that address had nothing
    // mapped behind it and the entry block was reported NoExec.
    //
    // The stack stays in the high identity lane on purpose: that is where the
    // live guest's stack lane lives too, and the stack write-back paths
    // (push/pop/call/ret) are not translated yet.
    const uint64_t guestImageBase = kProbeGuestImageBase;
    const uint64_t guestStackBase = kProbeGuestStackBase;
    if (mappingSize > kProbeGuestImageSpan) {
        reportf("the bundled ELF64 load span exceeds the probe image window");
        return false;
    }

    // KMemory64 owns the host mapping: in native-identity mode it maps the
    // canonical range through the alias and adopts the translated backing for
    // every page, so destroying it later releases exactly what it created.
    gProbeMemory = std::make_unique<KMemory64>(nullptr, /*nativeIdentity*/ true);
    gProbeCPU = std::make_unique<CPU64>(gProbeMemory.get());
    const uint64_t mappedImage = gProbeMemory->mmapAnonymousFixed(
        guestImageBase, mappingSize, 0x3);
    const uint64_t mappedStack = gProbeMemory->mmapAnonymousFixed(
        guestStackBase, kGuestStackBytes, 0x3);
    if (mappedImage != guestImageBase || mappedStack != guestStackBase) {
        reportf("the probe guest mapping failed: image=0x%llx stack=0x%llx",
                static_cast<unsigned long long>(mappedImage),
                static_cast<unsigned long long>(mappedStack));
        return false;
    }

    // Host backings are reached only through the translation; the guest never
    // sees these values.
    uint8_t* const hostImage = reinterpret_cast<uint8_t*>(
        static_cast<uintptr_t>(boxedvn::guestToHostAddress(guestImageBase)));
    uint8_t* const hostStack = reinterpret_cast<uint8_t*>(
        static_cast<uintptr_t>(boxedvn::guestToHostAddress(guestStackBase)));
    std::memset(hostImage, 0, mappingSize);
    for (const boxedvn::ELFLoadSegment& segment : image.loadSegments) {
        const uint64_t destinationOffset = segment.virtualAddress - first;
        if (destinationOffset > mappingSize ||
            segment.fileSize > mappingSize - destinationOffset) {
            reportf("the bundled ELF64 segment escaped its checked load span");
            return false;
        }
        std::memcpy(hostImage + destinationOffset,
                    static_cast<const uint8_t*>(data.bytes) + segment.fileOffset,
                    static_cast<size_t>(segment.fileSize));
    }
    const uint64_t entryOffset = image.entry - first;
    if (image.entry < first || entryOffset >= mappingSize) {
        reportf("the bundled ELF64 entry lies outside its load span");
        return false;
    }

    // The syscall dispatcher reads guest buffers through gAddressSpace, so it
    // must map the CANONICAL guest range onto the translated host backing.
    const auto addAliased = [](uint64_t guestAddress, uint8_t* hostBacking,
                               size_t size, uint8_t access) {
        return gAddressSpace.add({guestAddress, size,
                                  reinterpret_cast<uintptr_t>(hostBacking),
                                  access});
    };
    if (!addAliased(guestImageBase, hostImage, mappingSize,
                    boxedvn::GuestMemoryRead |
                    boxedvn::GuestMemoryWrite |
                    boxedvn::GuestMemoryExecute) ||
        !addAliased(guestStackBase, hostStack, kGuestStackBytes,
                    boxedvn::GuestMemoryRead | boxedvn::GuestMemoryWrite)) {
        reportf("the probe guest ranges could not be registered");
        return false;
    }

    // Only canonical values ever reach CPU64 or FEX.
    gGuestCode = reinterpret_cast<void*>(static_cast<uintptr_t>(guestImageBase));
    gGuestStack = reinterpret_cast<void*>(static_cast<uintptr_t>(guestStackBase));
    gGuestEntry = guestImageBase + entryOffset;
    gProbeCPU->rip = gGuestEntry;
    gProbeCPU->reg[X64_RSP].setU64(guestStackBase + kGuestStackBytes - 0x100);
    reportf("BOXEDWINE_FEX64_PROBE_MAP guest_image=0x%llx host_image=0x%llx "
            "guest_stack=0x%llx host_stack=0x%llx alias=1",
            static_cast<unsigned long long>(guestImageBase),
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(hostImage)),
            static_cast<unsigned long long>(guestStackBase),
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(hostStack)));

    // The translator asks this address space whether the canonical entry is
    // executable, using the canonical address. Prove that here rather than
    // discovering it as a NoExec entry block six seconds later.
    const auto entryRange = gAddressSpace.executableRange(gGuestEntry);
    if (!entryRange) {
        reportf("BOXEDWINE_FEX64_PROBE_ENTRY_UNMAPPED entry=0x%llx",
                static_cast<unsigned long long>(gGuestEntry));
        return false;
    }
    if (!entryRange->contains(gGuestEntry) ||
        entryRange->hostBase !=
            static_cast<uintptr_t>(
                boxedvn::guestToHostAddress(entryRange->guestBase))) {
        reportf("BOXEDWINE_FEX64_PROBE_ENTRY_MISMATCH entry=0x%llx "
                "range=[0x%llx,0x%llx) host=0x%llx expected_host=0x%llx",
                static_cast<unsigned long long>(gGuestEntry),
                static_cast<unsigned long long>(entryRange->guestBase),
                static_cast<unsigned long long>(entryRange->end()),
                static_cast<unsigned long long>(entryRange->hostBase),
                static_cast<unsigned long long>(
                    boxedvn::guestToHostAddress(entryRange->guestBase)));
        return false;
    }
    reportf("BOXEDWINE_FEX64_PROBE_ENTRY entry=0x%llx range=[0x%llx,0x%llx) "
            "host=0x%llx executable=1",
            static_cast<unsigned long long>(gGuestEntry),
            static_cast<unsigned long long>(entryRange->guestBase),
            static_cast<unsigned long long>(entryRange->end()),
            static_cast<unsigned long long>(entryRange->hostBase));
    reportf("loaded bundled PIE ELF64 correctness process: %zu segments, entry=%p",
            image.loadSegments.size(), reinterpret_cast<void*>(gGuestEntry));
    return true;
}

bool mapRawGuestProbe() {
    static constexpr char message[] = "BoxedWine x86-64 kernel entry reached";
    gGuestCode = mmap(nullptr, kGuestCodeBytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    gGuestStack = mmap(nullptr, kGuestStackBytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
    gGuestMessage = mmap(nullptr, kPageBytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    if (gGuestCode == MAP_FAILED || gGuestStack == MAP_FAILED ||
        gGuestMessage == MAP_FAILED) {
        reportf("failed to allocate direct guest code, stack, or data mapping");
        return false;
    }
    std::memcpy(gGuestMessage, message, sizeof(message) - 1);

    // write(1, message, length); exit(42). The absolute message pointer is
    // patched into MOV RSI, imm64 because FEX uses identity-mapped guest VAs.
    std::array<uint8_t, 49> code {
        0x48,0xC7,0xC0,0x01,0x00,0x00,0x00,
        0x48,0xC7,0xC7,0x01,0x00,0x00,0x00,
        0x48,0xBE,0,0,0,0,0,0,0,0,
        0x48,0xC7,0xC2,static_cast<uint8_t>(sizeof(message)-1),0,0,0,
        0x0F,0x05,
        0x48,0xC7,0xC7,0x2A,0x00,0x00,0x00,
        0x48,0xC7,0xC0,0x3C,0x00,0x00,0x00,
        0x0F,0x05,
    };
    const uint64_t messageAddress = reinterpret_cast<uint64_t>(gGuestMessage);
    std::memcpy(code.data() + 16, &messageAddress, sizeof(messageAddress));
    std::memcpy(gGuestCode, code.data(), code.size());

    const auto addIdentity = [](void* pointer, size_t size, uint8_t access) {
        return gAddressSpace.add({reinterpret_cast<uint64_t>(pointer), size,
                                  reinterpret_cast<uintptr_t>(pointer), access});
    };
    if (!addIdentity(gGuestCode, kGuestCodeBytes,
                     boxedvn::GuestMemoryRead | boxedvn::GuestMemoryExecute) ||
        !addIdentity(gGuestStack, kGuestStackBytes,
                     boxedvn::GuestMemoryRead | boxedvn::GuestMemoryWrite) ||
        !addIdentity(gGuestMessage, kPageBytes, boxedvn::GuestMemoryRead)) {
        reportf("direct guest mappings overlapped or could not be registered");
        return false;
    }
    gProbeMemory = std::make_unique<KMemory64>(nullptr);
    gProbeCPU = std::make_unique<CPU64>(gProbeMemory.get());
    gProbeMemory->mmapAnonymousFixed(
        reinterpret_cast<uint64_t>(gGuestCode), kGuestCodeBytes, 0x5);
    gProbeMemory->mmapAnonymousFixed(
        reinterpret_cast<uint64_t>(gGuestStack), kGuestStackBytes, 0x3);
    gProbeMemory->mmapAnonymousFixed(
        reinterpret_cast<uint64_t>(gGuestMessage), kPageBytes, 0x1);
    gProbeMemory->memcpyToGuest(
        reinterpret_cast<uint64_t>(gGuestCode), code.data(), code.size());
    gProbeMemory->memcpyToGuest(
        reinterpret_cast<uint64_t>(gGuestMessage), message, sizeof(message) - 1);
    gProbeCPU->rip = reinterpret_cast<uint64_t>(gGuestCode);
    gProbeCPU->reg[X64_RSP].setU64(
        reinterpret_cast<uint64_t>(gGuestStack) + kGuestStackBytes - 0x100);
    gGuestEntry = reinterpret_cast<uint64_t>(gGuestCode);
    return true;
}

bool mapGuestProbe() {
    reportf("preparing the bundled SSE2/REP-STOS/call-ret process against BoxedWine's CPU64 kernel state");
    return mapBundledELFProbe();
}

FEXCore::HLE::ExecutableRangeInfo queryLiveExecutableRange(uint64_t address) {
    BVNFEXCPU64Adapter* adapter = BVNFEXCPU64AdapterCurrent();
    uint64_t base = 0;
    uint64_t size = 0;
    bool writable = false;
    if (!adapter || !BVNFEXCPU64AdapterQueryExecutableRange(
            adapter, address, &base, &size, &writable)) {
        return {0, 0, false};
    }
    return {base, size, writable};
}

void invalidateLiveExecutableRange(FEXCore::Core::InternalThreadState* thread,
                                   uint64_t start, uint64_t length) {
    std::lock_guard<std::mutex> guard(gLiveMutex);
    auto it = gLiveThreadContexts.find(thread);
    if (it != gLiveThreadContexts.end() && it->second != nullptr) {
        it->second->InvalidateThreadCachedCodeRange(thread, start, length);
        return;
    }
    // The probe uses its own mode-explicit context rather than the live
    // process table. Preserve FEX's SMC invalidation semantics there as well;
    // silently dropping this callback leaves stale translated blocks after
    // self-modifying guest code.
    if (gProbeContext != nullptr && gProbeContext->context != nullptr) {
        gProbeContext->context->InvalidateThreadCachedCodeRange(
            thread, start, length);
    }
}

std::atomic<uint32_t> gCodeInvalidationReports {0};
constexpr uint32_t kCodeInvalidationReportLimit = 16;

// The kernel's mmap/munmap hook. The translator keeps blocks and lookup
// entries by guest address across all of a process's threads; Boxedwine's
// address space changes never reached it before, so a library unmapped and
// another mapped at the same address kept executing the first library's
// blocks (a `ret` from the old code where the new code has a `call`). This
// mirrors the frontend the translator ships with: the invalidation mutex is
// taken uniquely, every code buffer drops the range, then every thread's
// cached entries and call-return stack follow. Only a live translated process
// has anything to drop; the interpreter lane returns at the lookup.
void invalidateTranslatedGuestCode(KProcess* process, U64 start, U64 length) {
    std::lock_guard<std::mutex> guard(gLiveMutex);
    auto it = gLiveProcesses.find(process);
    if (it == gLiveProcesses.end() || !it->second || !it->second->fex ||
        !it->second->fex->context || it->second->retiring.load(std::memory_order_acquire)) {
        return;
    }
    FEXCore::Context::Context* context = it->second->fex->context.get();
    std::scoped_lock codeLock(context->GetCodeInvalidationMutex());
    context->InvalidateCodeBuffersCodeRange(start, length);
    uint32_t threads = 0;
    for (auto& entry : it->second->threads) {
        if (!entry.second || !entry.second->fexThread) continue;
        context->InvalidateThreadCachedCodeRange(entry.second->fexThread, start, length);
        ++threads;
    }
    if (gCodeInvalidationReports.fetch_add(1, std::memory_order_relaxed) <
        kCodeInvalidationReportLimit) {
        reportf("BOXEDWINE_FEX64_CODE_INVALIDATE pid=%u start=0x%llx len=0x%llx threads=%u",
                static_cast<unsigned>(process->id),
                static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(length), threads);
    }
}

const bool gTranslatedCodeInvalidatorRegistered = [] {
    KMemory64::setTranslatedCodeInvalidator(&invalidateTranslatedGuestCode);
    return true;
}();

bool createLiveInitialThread(FEXContextBundle& bundle) {
    bundle.initialThread = bundle.context->CreateThread(0, 0);
    return bundle.initialThread != nullptr;
}

bool createProbeInitialThread(FEXContextBundle& bundle) {
    const uint64_t stack = reinterpret_cast<uint64_t>(gGuestStack) +
                           kGuestStackBytes - 0x100;
    bundle.initialThread = bundle.context->CreateThread(gGuestEntry, stack);
    return bundle.initialThread != nullptr;
}

LiveProcessState* getLiveProcessState(void* process) {
    std::lock_guard<std::mutex> guard(gLiveMutex);
    auto it = gLiveProcesses.find(process);
    if (it != gLiveProcesses.end()) {
        if (it->second->retiring.load(std::memory_order_acquire)) return nullptr;
        it->second->activeRuns.fetch_add(1, std::memory_order_relaxed);
        return it->second.get();
    }

    const bool firstLiveProcess = gLiveProcesses.empty();
    auto state = std::make_unique<LiveProcessState>();
    state->fex = createFEXContext(boxedvn::FexGuestMode::X86_64,
                                  &createLiveInitialThread);
    if (!state->fex) return nullptr;
    if (!installFEXHostSignalHandlers()) {
        reportf("FEX host fault handlers could not be installed");
        return nullptr;
    }
    // A previous launch localized a faulting guest instruction exactly. Arm
    // FEX's targeted IR capture on it now: the context exists but has compiled
    // nothing, this runs under gLiveMutex with no other live process, and the
    // startup probe never reaches here. Consuming the slot arms exactly once
    // per pending target; creating a thread does not translate guest code.
    const uint64_t pendingIRCapTarget = firstLiveProcess
        ? BVNFEXBackendTakePendingIRCapTarget(
              static_cast<KProcess*>(process)->commandLine.c_str())
        : 0;
    if (pendingIRCapTarget != 0) {
        FEX_BoxedWineIRCapArm(pendingIRCapTarget);
        reportf("BOXEDWINE_FEX64_IRCAP_ARMED target=0x%llx current=0x%llx",
                static_cast<unsigned long long>(pendingIRCapTarget),
                static_cast<unsigned long long>(
                    FEX_BoxedWineIRCapCurrentTarget()));
    }
    LiveProcessState* result = state.get();
    result->activeRuns.store(1, std::memory_order_relaxed);
    gLiveProcesses.emplace(process, std::move(state));
    return result;
}

LiveThreadState* getLiveThreadState(LiveProcessState* processState,
                                    void* thread) {
    std::lock_guard<std::mutex> guard(gLiveMutex);
    auto it = processState->threads.find(thread);
    if (it != processState->threads.end()) {
        if (it->second->active) return nullptr;
        it->second->active = true;
        it->second->hostMachThread = pthread_mach_thread_np(pthread_self());
        return it->second.get();
    }

    auto state = std::make_unique<LiveThreadState>();
    if (processState->fex->initialThread != nullptr) {
        state->fexThread = processState->fex->initialThread;
        processState->fex->initialThread = nullptr;
    } else {
        state->fexThread = createFEXThread(*processState->fex, 0, 0);
    }
    if (!state->fexThread) return nullptr;
    // The iOS host currently wedges when FEX's first-time direct block linker
    // recursively waits on its lookup-cache writer lock. Bounce link misses
    // back through the ordinary dispatcher instead. Translation and L1/L2
    // cache lookup remain active; only direct call-site patching is disabled.
    disableLiveBlockLinking(state->fexThread);
    reportf("FEX live direct block linking disabled; exits use the dispatcher cache");

    constexpr size_t shadowBytes =
        FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
    state->callRetMappingSize = shadowBytes + 2 * kPageBytes;
    state->callRetMapping = mmap(nullptr, state->callRetMappingSize, PROT_NONE,
                                 MAP_PRIVATE | MAP_ANON, -1, 0);
    if (state->callRetMapping == MAP_FAILED) {
        processState->fex->context->DestroyThread(state->fexThread);
        state->callRetMapping = nullptr;
        state->fexThread = nullptr;
        return nullptr;
    }
    void* shadow = static_cast<uint8_t*>(state->callRetMapping) + kPageBytes;
    if (mprotect(shadow, shadowBytes, PROT_READ | PROT_WRITE) != 0) {
        munmap(state->callRetMapping, state->callRetMappingSize);
        processState->fex->context->DestroyThread(state->fexThread);
        state->callRetMapping = nullptr;
        state->fexThread = nullptr;
        return nullptr;
    }
    state->fexThread->CallRetStackBase = shadow;
    state->fexThread->CurrentFrame->State.callret_sp =
        reinterpret_cast<uint64_t>(shadow) + shadowBytes / 4;
    state->fexThread->CurrentFrame->State.callret_sp_base =
        reinterpret_cast<uint64_t>(shadow);
    publishGuestDescriptorTable(state->fexThread->CurrentFrame->State,
                                state->gdt);
    gLiveThreadContexts.emplace(state->fexThread, processState->fex->context.get());
    state->active = true;
    state->hostMachThread = pthread_mach_thread_np(pthread_self());
    LiveThreadState* result = state.get();
    processState->threads.emplace(thread, std::move(state));
    return result;
}

void releaseLiveThreadRun(LiveProcessState* processState,
                          LiveThreadState* threadState,
                          bool retire) {
    if (!processState || !threadState) return;
    std::unique_ptr<LiveThreadState> retiredState;
    {
        std::lock_guard<std::mutex> guard(gLiveMutex);
        for (auto it = processState->threads.begin();
             it != processState->threads.end(); ++it) {
            if (it->second.get() != threadState) continue;
            if (!retire) {
                it->second->active = false;
                it->second->hostMachThread = MACH_PORT_NULL;
                return;
            }
            if (it->second->fexThread) {
                gLiveThreadContexts.erase(it->second->fexThread);
            }
            retiredState = std::move(it->second);
            processState->threads.erase(it);
            break;
        }
    }
    if (!retiredState) return;
    if (retiredState->fexThread) {
        processState->fex->context->DestroyThread(retiredState->fexThread);
        retiredState->fexThread = nullptr;
    }
    if (retiredState->callRetMapping) {
        munmap(retiredState->callRetMapping,
               retiredState->callRetMappingSize);
        retiredState->callRetMapping = nullptr;
    }
}

void releaseLiveProcessRun(void* process, LiveProcessState* expected,
                           bool retire) {
    std::unique_ptr<LiveProcessState> state;
    {
        std::lock_guard<std::mutex> guard(gLiveMutex);
        auto it = gLiveProcesses.find(process);
        if (it == gLiveProcesses.end() || it->second.get() != expected) return;
        if (retire) {
            it->second->retiring.store(true, std::memory_order_release);
        }
        const size_t previous = it->second->activeRuns.fetch_sub(
            1, std::memory_order_acq_rel);
        if (previous != 1 ||
            !it->second->retiring.load(std::memory_order_acquire)) {
            return;
        }
        state = std::move(it->second);
        gLiveProcesses.erase(it);
        for (const auto& entry : state->threads) {
            if (entry.second && entry.second->fexThread) {
                gLiveThreadContexts.erase(entry.second->fexThread);
            }
        }
    }

    // ExecuteThread has already returned through the guarded handoff when
    // this is called. Destroy every FEX thread before releasing its context;
    // otherwise a later process reusing the same opaque KProcess address could
    // accidentally inherit stale translated blocks and call-ret state.
    for (auto& entry : state->threads) {
        LiveThreadState* thread = entry.second.get();
        if (thread->fexThread) {
            state->fex->context->DestroyThread(thread->fexThread);
            thread->fexThread = nullptr;
        }
        if (thread->callRetMapping) {
            munmap(thread->callRetMapping, thread->callRetMappingSize);
            thread->callRetMapping = nullptr;
        }
    }
    state->threads.clear();
    // Exec-retired epochs are released last. They are unreachable once the
    // process left gLiveProcesses, and each one destroys its own retired
    // thread before its context, so nothing outlives the epoch that owns it.
    state->retiredEpochs.clear();
}

bool recreateLiveContextAfterExec(LiveProcessState* processState,
                                  LiveThreadState* threadState,
                                  BVNFEXCPU64Adapter* adapter,
                                  uint64_t resumeRIP) {
    if (!processState || !processState->fex || !threadState ||
        !threadState->fexThread || !threadState->fexThread->CurrentFrame ||
        !adapter) {
        return false;
    }

    // ClearCodeCache() and replacing only InternalThreadState are both too
    // narrow for an exec boundary. The former retained a lookup path to guest
    // RIP zero, while the latter entered a dispatcher whose continuation target
    // was null. Replace the process context as one unit so the dispatcher,
    // lookup cache, JIT backend, signal delegator, and thread all begin in the
    // same execution epoch.
    FEXCore::Core::CPUState savedState {};
    std::memcpy(&savedState,
                &threadState->fexThread->CurrentFrame->State,
                sizeof(savedState));
    savedState.rip = resumeRIP;
    savedState.InlineJITBlockHeader = 0;
    savedState.callret_sp = 0;
    savedState.callret_sp_base = 0;

    auto replacementBundle = createFEXContext(
        processState->fex->mode, nullptr);
    if (!replacementBundle) return false;
    auto* replacement = createFEXThread(
        *replacementBundle, resumeRIP,
        savedState.gregs[FEXCore::X86State::REG_RSP], &savedState);
    if (!replacement) return false;
    // Until ownership is transferred to the live thread table, let the bundle
    // clean this thread up on every early-return path.
    replacementBundle->initialThread = replacement;
    if (!replacement->CurrentFrame) return false;

    disableLiveBlockLinking(replacement);
    if (threadState->callRetMapping && threadState->callRetMappingSize > 0) {
        void* shadow = static_cast<uint8_t*>(threadState->callRetMapping) +
                       kPageBytes;
        const size_t shadowBytes =
            FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
        (void)madvise(shadow, shadowBytes, MADV_DONTNEED);
        replacement->CallRetStackBase = shadow;
        replacement->CurrentFrame->State.callret_sp =
            reinterpret_cast<uint64_t>(shadow) + shadowBytes / 4;
        replacement->CurrentFrame->State.callret_sp_base =
            reinterpret_cast<uint64_t>(shadow);
    }
    publishGuestDescriptorTable(replacement->CurrentFrame->State,
                                threadState->gdt);

    // A newly-created dispatcher normally compiles its first block on a cache
    // miss. At the exec recovery boundary the iOS dispatcher instead observed
    // the empty L1 entry as a host target and branched to zero. Seed only the
    // validated program entry before publishing this execution epoch; later
    // blocks continue to use FEX's ordinary compile-on-demand path.
    if (!primeFEXThread(*replacementBundle, replacement, resumeRIP)) {
        return false;
    }
    reportf("BOXEDWINE_FEX64_CONTEXT_RESET_PRIMED rip=0x%llx",
            static_cast<unsigned long long>(resumeRIP));

    auto* retired = threadState->fexThread;
    size_t retainedEpochs = 0;
    {
        std::lock_guard<std::mutex> guard(gLiveMutex);
        if (processState->threads.size() != 1 ||
            processState->threads.begin()->second.get() != threadState ||
            processState->fex == nullptr ||
            !BVNFEXCPU64AdapterBindFEX(
                adapter, replacementBundle->context.get(), replacement)) {
            return false;
        }
        gActiveSignals = replacementBundle->signals.get();
        // Stop routing the retired thread before the replacement becomes
        // reachable: both epochs describe the same opaque BoxedWine thread.
        gLiveThreadContexts.erase(retired);
        gLiveThreadContexts.emplace(replacement,
                                    replacementBundle->context.get());
        threadState->fexThread = replacement;
        replacementBundle->initialThread = nullptr;
        // Destroying the retired thread or its context here tore down the
        // dispatcher and code buffers that this still-running BVNFEXCPU64Run
        // returns through, which faulted at host_pc=0. Retain the whole epoch
        // -- thread and context as one unit -- until the process is retired.
        auto retiredEpoch = std::make_unique<RetiredFEXEpoch>();
        retiredEpoch->thread = retired;
        retiredEpoch->bundle = std::move(processState->fex);
        processState->retiredEpochs.push_back(std::move(retiredEpoch));
        retainedEpochs = processState->retiredEpochs.size();
        processState->fex = std::move(replacementBundle);
        processState->execEpoch.fetch_add(1, std::memory_order_acq_rel);
    }
    reportf("BOXEDWINE_FEX64_CONTEXT_RESET_RETAINED epochs=%zu thread=%p",
            retainedEpochs, static_cast<void*>(retired));
    return true;
}

} // namespace

extern "C" bool BVNFEXBackendOwnsHostCodeAddress(uint64_t address) {
    return poolOwns(reinterpret_cast<const void*>(
        static_cast<uintptr_t>(address)));
}

extern "C" uint64_t BVNFEXBackendWritableHostCodeAddress(uint64_t address) {
    return gCodeSegments.writable(static_cast<uintptr_t>(address));
}

extern "C" bool BVNFEXBackendBuilt(void) { return true; }

extern "C" BVNFEXBackendStage BVNFEXBackendStageReached(void) {
    return gStage.load(std::memory_order_acquire);
}

extern "C" const char* BVNFEXBackendReport(void) {
    static std::string snapshot;
    std::lock_guard<std::mutex> guard(gReportMutex);
    snapshot = gReport;
    const uint64_t hostFaults = gFEXHostFaultCount.load(std::memory_order_relaxed);
    const uint64_t kuserFaults = gFEXKuserFaultCount.load(std::memory_order_relaxed);
    if (hostFaults != 0 || kuserFaults != 0) {
        char diagnostics[320];
        snprintf(diagnostics, sizeof(diagnostics),
                 "host_faults=%llu handled_faults=%llu kuser_faults=%llu "
                 "last_signal=%llu last_address=0x%llx last_pc=0x%llx\n",
                 static_cast<unsigned long long>(hostFaults),
                 static_cast<unsigned long long>(
                     gFEXHandledHostFaultCount.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(kuserFaults),
                 static_cast<unsigned long long>(
                     gFEXLastHostSignal.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     gFEXLastHostFaultAddress.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     gFEXLastHostFaultPC.load(std::memory_order_relaxed)));
        snapshot.append(diagnostics);
    }
    return snapshot.c_str();
}

extern "C" void BVNFEXBackendPollExecutionTrace(void) {
    constexpr size_t kMaximumSnapshots = 8;
    constexpr uint64_t kSampleReportInterval = 5;
    constexpr uint64_t kStallWarningInterval = 30;
    constexpr uint32_t kStableSamplesBeforeWarning = 3;
    std::array<ExecutionTraceSnapshot, kMaximumSnapshots> snapshots {};
    size_t snapshotCount = 0;
    const uint64_t poll = gExecutionTracePoll.fetch_add(
        1, std::memory_order_relaxed) + 1;
    const mach_port_t pollingThread = pthread_mach_thread_np(pthread_self());

    {
        std::lock_guard<std::mutex> guard(gLiveMutex);
        for (const auto& processEntry : gLiveProcesses) {
            if (snapshotCount == snapshots.size()) break;
            auto* process = static_cast<KProcess*>(processEntry.first);
            LiveProcessState* processState = processEntry.second.get();
            if (!process || !processState) continue;
            // Hash-map order used to hide the main/render thread once a
            // process had more than eight workers. Always sample its oldest
            // live thread, then rotate the remaining slots across workers.
            std::vector<std::pair<KThread*, LiveThreadState*>> candidates;
            for (const auto& entry : processState->threads) {
                auto* candidate = static_cast<KThread*>(entry.first);
                auto* state = entry.second.get();
                if (candidate && state && state->active &&
                    state->hostMachThread != MACH_PORT_NULL &&
                    state->hostMachThread != pollingThread &&
                    state->fexThread && state->fexThread->CurrentFrame) {
                    candidates.emplace_back(candidate, state);
                }
            }
            std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
                return a.first->id < b.first->id;
            });
            if (candidates.size() > kMaximumSnapshots) {
                const size_t offset = ((poll - 1) * (kMaximumSnapshots - 1)) %
                                      (candidates.size() - 1);
                std::rotate(candidates.begin() + 1,
                            candidates.begin() + 1 + offset, candidates.end());
            }
            for (const auto& threadEntry : candidates) {
                if (snapshotCount == snapshots.size()) break;
                auto* thread = static_cast<KThread*>(threadEntry.first);
                LiveThreadState* threadState = threadEntry.second;
                if (!thread || !threadState || !threadState->active ||
                    threadState->hostMachThread == MACH_PORT_NULL ||
                    threadState->hostMachThread == pollingThread ||
                    !threadState->fexThread ||
                    !threadState->fexThread->CurrentFrame) {
                    continue;
                }

                thread_basic_info_data_t basic {};
                mach_msg_type_number_t basicCount = THREAD_BASIC_INFO_COUNT;
                const kern_return_t infoResult = thread_info(
                    threadState->hostMachThread, THREAD_BASIC_INFO,
                    reinterpret_cast<thread_info_t>(&basic), &basicCount);
                const kern_return_t suspendResult = thread_suspend(
                    threadState->hostMachThread);
                if (suspendResult != KERN_SUCCESS) continue;

                arm_thread_state64_t registers {};
                mach_msg_type_number_t registerCount = ARM_THREAD_STATE64_COUNT;
                const kern_return_t stateResult = thread_get_state(
                    threadState->hostMachThread, ARM_THREAD_STATE64,
                    reinterpret_cast<thread_state_t>(&registers),
                    &registerCount);

                ExecutionTraceSnapshot snapshot {};
                snapshot.poll = poll;
                snapshot.processId = process->id;
                snapshot.threadId = thread->id;
                snapshot.processEntryRIP = process->entry64;
                if (infoResult == KERN_SUCCESS) {
                    snapshot.runState = basic.run_state;
                    snapshot.cpuUsage = basic.cpu_usage;
                }
                snapshot.frameRIP =
                    threadState->fexThread->CurrentFrame->State.rip;
                snapshot.guestRIP = snapshot.frameRIP;
                auto& fexFrame = *threadState->fexThread->CurrentFrame;
                snapshot.fsBase = fexFrame.State.fs_cached;
                snapshot.gsBase = fexFrame.State.gs_cached;
                snapshot.inSyscallInfo = fexFrame.InSyscallInfo;
                std::copy_n(fexFrame.State.gregs,
                            snapshot.guestGPRs.size(),
                            snapshot.guestGPRs.begin());
                if (stateResult == KERN_SUCCESS) {
                    snapshot.hostPC = arm_thread_state64_get_pc(registers);
                    snapshot.hostLR = arm_thread_state64_get_lr(registers);
                    snapshot.hostSP = arm_thread_state64_get_sp(registers);
                    std::copy_n(registers.__x, snapshot.hostArguments.size(),
                                snapshot.hostArguments.begin());
                    std::copy_n(registers.__x + 8,
                                snapshot.hostCallRegisters.size(),
                                snapshot.hostCallRegisters.begin());
                    snapshot.hostCodeAddress = snapshot.hostPC & ~uint64_t {3};
                    vm_size_t hostCodeBytes = 0;
                    snapshot.hostCodeValid = vm_read_overwrite(
                        mach_task_self(), snapshot.hostCodeAddress,
                        sizeof(snapshot.hostCode),
                        reinterpret_cast<vm_address_t>(
                            snapshot.hostCode.data()),
                        &hostCodeBytes) == KERN_SUCCESS &&
                        hostCodeBytes == sizeof(snapshot.hostCode);
                    if (snapshot.hostLR >= 32) {
                        snapshot.hostReturnCodeAddress =
                            (snapshot.hostLR - 32) & ~uint64_t {3};
                        vm_size_t hostReturnCodeBytes = 0;
                        snapshot.hostReturnCodeValid = vm_read_overwrite(
                            mach_task_self(), snapshot.hostReturnCodeAddress,
                            sizeof(snapshot.hostReturnCode),
                            reinterpret_cast<vm_address_t>(
                                snapshot.hostReturnCode.data()),
                            &hostReturnCodeBytes) == KERN_SUCCESS &&
                            hostReturnCodeBytes ==
                                sizeof(snapshot.hostReturnCode);

                        const uintptr_t returnAddress =
                            static_cast<uintptr_t>(snapshot.hostLR - 4);
                        snapshot.hostReturnInPool =
                            poolOwns(reinterpret_cast<const void*>(returnAddress));
                        if (snapshot.hostReturnInPool) {
                            const auto index = gCodeSegments.find(returnAddress);
                            snapshot.hostReturnPoolOffset = returnAddress - gCodeSegments.at(index).rx;
                            snapshot.hostReturnWritableAlias = gCodeSegments.writable(returnAddress);
                        }
                    }
                    // Never call FEX's code-buffer queries while suspended.
                    // CPUBackend::IsAddressInCodeBuffer takes IosMigrateLock;
                    // the sampled thread may own it and cannot release it
                    // until we resume it. The fixed executable pool needs no
                    // lock. Keep the frame RIP as a sample, not an exact PC
                    // reconstruction through mutable JIT metadata.
                    if (poolOwns(reinterpret_cast<const void*>(snapshot.hostPC))) {

                        // Static-register allocation keeps live guest GPRs in
                        // ARM64 registers while translated code is running.
                        // The frame copy above is therefore stale for those
                        // registers. Reconstruct the same view used by FEX's
                        // signal bridge while the host thread is suspended.
                        const auto& signalConfig = processState->fex->signals->GetConfig();
                        const uint32_t ignoreMask =
                            static_cast<uint32_t>(snapshot.inSyscallInfo & 0xffffu);
                        const size_t gprCount = std::min<size_t>(
                            signalConfig.SRAGPRCount,
                            snapshot.guestGPRs.size());
                        for (size_t gpr = 0; gpr < gprCount; ++gpr) {
                            const uint8_t hostRegister =
                                signalConfig.SRAGPRMapping[gpr];
                            if (hostRegister >= 29 ||
                                (ignoreMask & (1u << hostRegister))) {
                                continue;
                            }
                            snapshot.guestGPRs[gpr] =
                                registers.__x[hostRegister];
                            snapshot.liveGPRMask |= 1u << gpr;
                        }
                    }
                    if (snapshot.hostLR >= 4 &&
                        poolOwns(reinterpret_cast<const void*>(snapshot.hostLR - 4))) {
                        snapshot.hostCallCodeAddress =
                            (snapshot.hostLR - 16) & ~uint64_t {3};
                        vm_size_t hostCallCodeBytes = 0;
                        snapshot.hostCallCodeValid = vm_read_overwrite(
                            mach_task_self(), snapshot.hostCallCodeAddress,
                            sizeof(snapshot.hostCallCode),
                            reinterpret_cast<vm_address_t>(
                                snapshot.hostCallCode.data()),
                            &hostCallCodeBytes) == KERN_SUCCESS &&
                            hostCallCodeBytes == sizeof(snapshot.hostCallCode);
                    }
                }

                // These are the operands used by glibc's allocator/free path
                // in the pinned x64 rootfs. Reads are diagnostic only, are
                // bounded to already mapped readable guest pages, and happen
                // while the sole translated thread is suspended.
                KMemory64* memory = process->memory64;
                const uint64_t rsi = snapshot.guestGPRs[
                    FEXCore::X86State::REG_RSI];
                const uint64_t rdi = snapshot.guestGPRs[
                    FEXCore::X86State::REG_RDI];
                const uint64_t rbx = snapshot.guestGPRs[
                    FEXCore::X86State::REG_RBX];
                if (readNativeGuestQword(memory, rsi + 8,
                                         snapshot.chunkSizeWord)) {
                    snapshot.memoryValueMask |= TraceChunkSizeWord;
                }
                if (readNativeGuestQword(memory, rsi + 0x10,
                                         snapshot.chunkUserWord)) {
                    snapshot.memoryValueMask |= TraceChunkUserWord;
                }
                if (readNativeGuestQword(memory, rsi + 0x18,
                                         snapshot.chunkKeyWord)) {
                    snapshot.memoryValueMask |= TraceChunkKeyWord;
                }
                if (readNativeGuestQword(memory, rbx + 8,
                                         snapshot.arenaLockWord)) {
                    snapshot.memoryValueMask |= TraceArenaLockWord;
                }
                if (readNativeGuestQword(memory, rbx + 0x888,
                                         snapshot.arenaSystemMem)) {
                    snapshot.memoryValueMask |= TraceArenaSystemMem;
                }
                if (readNativeGuestQword(memory, snapshot.fsBase + rdi,
                                         snapshot.tlsOperandWord)) {
                    snapshot.memoryValueMask |= TraceTlsOperandWord;
                }

                const kern_return_t resumeResult = thread_resume(
                    threadState->hostMachThread);
                if (resumeResult != KERN_SUCCESS) {
                    // This line is intentionally direct: leaving a translated
                    // thread suspended is terminal and must not be hidden by
                    // the normal heartbeat throttle.
                    reportf("BOXEDWINE_FEX64_SAMPLE thread_resume failed pid=%u tid=%u status=%d",
                            process->id, thread->id, resumeResult);
                }

                snapshot.hostFaults = gFEXHostFaultCount.load(
                    std::memory_order_relaxed);
                snapshot.handledFaults = gFEXHandledHostFaultCount.load(
                    std::memory_order_relaxed);
                snapshot.lastSignal = gFEXLastHostSignal.load(
                    std::memory_order_relaxed);
                snapshot.lastFaultAddress = gFEXLastHostFaultAddress.load(
                    std::memory_order_relaxed);
                snapshot.lastFaultPC = gFEXLastHostFaultPC.load(
                    std::memory_order_relaxed);
                snapshot.newFaults = snapshot.hostFaults >=
                    threadState->sampleLastFaultCount
                    ? snapshot.hostFaults - threadState->sampleLastFaultCount
                    : snapshot.hostFaults;
                threadState->sampleLastFaultCount = snapshot.hostFaults;

                if (snapshot.hostPC != 0 &&
                    snapshot.hostPC == threadState->sampleLastHostPC &&
                    snapshot.guestRIP == threadState->sampleLastGuestRIP) {
                    ++threadState->sampleStablePolls;
                } else {
                    threadState->sampleStablePolls = 1;
                }
                threadState->sampleLastHostPC = snapshot.hostPC;
                threadState->sampleLastGuestRIP = snapshot.guestRIP;
                snapshot.stablePolls = threadState->sampleStablePolls;

                threadState->sampleGuestRIPs[threadState->sampleCursor] =
                    snapshot.guestRIP;
                threadState->sampleCursor = (threadState->sampleCursor + 1) %
                    threadState->sampleGuestRIPs.size();
                threadState->sampleCount = std::min(
                    threadState->sampleCount + 1,
                    threadState->sampleGuestRIPs.size());
                snapshot.historyCount = threadState->sampleCount;
                const size_t first = threadState->sampleCount ==
                    threadState->sampleGuestRIPs.size()
                    ? threadState->sampleCursor : 0;
                for (size_t index = 0; index < snapshot.historyCount; ++index) {
                    snapshot.guestHistory[index] =
                        threadState->sampleGuestRIPs[
                            (first + index) %
                            threadState->sampleGuestRIPs.size()];
                }

                snapshot.emitSample = threadState->sampleLastReportPoll == 0 ||
                    poll - threadState->sampleLastReportPoll >=
                        kSampleReportInterval;
                if (snapshot.emitSample) {
                    threadState->sampleLastReportPoll = poll;
                }
                snapshot.emitWarning =
                    threadState->sampleStablePolls >=
                        kStableSamplesBeforeWarning &&
                    (threadState->sampleLastWarningPoll == 0 ||
                     poll - threadState->sampleLastWarningPoll >=
                         kStallWarningInterval);
                if (snapshot.emitWarning) {
                    threadState->sampleLastWarningPoll = poll;
                }
                snapshots[snapshotCount++] = snapshot;
            }
        }
    }

    for (size_t index = 0; index < snapshotCount; ++index) {
        const ExecutionTraceSnapshot& snapshot = snapshots[index];
        char history[256] {};
        size_t used = 0;
        for (size_t item = 0; item < snapshot.historyCount &&
                              used < sizeof(history); ++item) {
            const int written = snprintf(
                history + used, sizeof(history) - used,
                "%s0x%llx", item == 0 ? "" : ",",
                static_cast<unsigned long long>(snapshot.guestHistory[item]));
            if (written < 0 ||
                static_cast<size_t>(written) >= sizeof(history) - used) {
                break;
            }
            used += static_cast<size_t>(written);
        }
        if (snapshot.emitSample) {
            const double cpuPercent = snapshot.cpuUsage * 100.0 /
                static_cast<double>(TH_USAGE_SCALE);
            reportf("BOXEDWINE_FEX64_SAMPLE poll=%llu pid=%u tid=%u state=%s cpu=%.1f%% host_pc=0x%llx host_lr=0x%llx host_sp=0x%llx guest_rip=0x%llx rip_source=frame entry_rip=0x%llx frame_rip=0x%llx faults=%llu handled=%llu new_faults=%llu last_signal=%llu last_address=0x%llx last_fault_pc=0x%llx history=[%s]",
                    static_cast<unsigned long long>(snapshot.poll),
                    snapshot.processId, snapshot.threadId,
                    machRunStateName(snapshot.runState), cpuPercent,
                    static_cast<unsigned long long>(snapshot.hostPC),
                    static_cast<unsigned long long>(snapshot.hostLR),
                    static_cast<unsigned long long>(snapshot.hostSP),
                    static_cast<unsigned long long>(snapshot.guestRIP),
                    static_cast<unsigned long long>(snapshot.processEntryRIP),
                    static_cast<unsigned long long>(snapshot.frameRIP),
                    static_cast<unsigned long long>(snapshot.hostFaults),
                    static_cast<unsigned long long>(snapshot.handledFaults),
                    static_cast<unsigned long long>(snapshot.newFaults),
                    static_cast<unsigned long long>(snapshot.lastSignal),
                    static_cast<unsigned long long>(snapshot.lastFaultAddress),
                    static_cast<unsigned long long>(snapshot.lastFaultPC),
                    history);
            // A thread that is running with the CPU pinned while its host PC sits
            // outside translated code is spinning in the host, not in the guest.
            // Name the host routine and its caller now: the stall report only
            // covers a PC that stays put, and a spin's PC moves.
            if (snapshot.runState == TH_STATE_RUNNING && cpuPercent >= 25.0 &&
                snapshot.hostPC != 0) {
                Dl_info pcInfo {};
                Dl_info lrInfo {};
                const bool havePC =
                    dladdr(reinterpret_cast<const void*>(snapshot.hostPC), &pcInfo) != 0;
                const bool haveLR = snapshot.hostLR != 0 &&
                    dladdr(reinterpret_cast<const void*>(snapshot.hostLR), &lrInfo) != 0;
                auto baseName = [](const char* path) -> const char* {
                    if (!path) return "(unknown)";
                    const char* slash = strrchr(path, '/');
                    return slash ? slash + 1 : path;
                };
                auto offsetIn = [](uint64_t address, const Dl_info& info) -> uint64_t {
                    const uint64_t base = reinterpret_cast<uint64_t>(info.dli_fbase);
                    return base != 0 && address >= base ? address - base : 0;
                };
                auto symbolOffset = [](uint64_t address, const Dl_info& info) -> uint64_t {
                    const uint64_t base = reinterpret_cast<uint64_t>(info.dli_saddr);
                    return base != 0 && address >= base ? address - base : 0;
                };
                reportf("BOXEDWINE_FEX64_BUSY_HOST poll=%llu pid=%u tid=%u cpu=%.1f%% "
                        "pc=0x%llx pc_image=%s pc_offset=0x%llx pc_symbol=%s+0x%llx "
                        "lr=0x%llx lr_image=%s lr_offset=0x%llx lr_symbol=%s+0x%llx",
                        static_cast<unsigned long long>(snapshot.poll),
                        snapshot.processId, snapshot.threadId, cpuPercent,
                        static_cast<unsigned long long>(snapshot.hostPC),
                        havePC ? baseName(pcInfo.dli_fname) : "(unknown)",
                        static_cast<unsigned long long>(
                            havePC ? offsetIn(snapshot.hostPC, pcInfo) : 0),
                        havePC && pcInfo.dli_sname ? pcInfo.dli_sname : "(unknown)",
                        static_cast<unsigned long long>(
                            havePC ? symbolOffset(snapshot.hostPC, pcInfo) : 0),
                        static_cast<unsigned long long>(snapshot.hostLR),
                        haveLR ? baseName(lrInfo.dli_fname) : "(unknown)",
                        static_cast<unsigned long long>(
                            haveLR ? offsetIn(snapshot.hostLR, lrInfo) : 0),
                        haveLR && lrInfo.dli_sname ? lrInfo.dli_sname : "(unknown)",
                        static_cast<unsigned long long>(
                            haveLR ? symbolOffset(snapshot.hostLR, lrInfo) : 0));
            }
        }
        if (snapshot.emitWarning) {
            reportf("BOXEDWINE_FEX64_STALL pid=%u tid=%u stable_samples=%u state=%s host_pc=0x%llx host_lr=0x%llx host_sp=0x%llx guest_rip=0x%llx rip_source=frame entry_rip=0x%llx faults=%llu handled=%llu last_signal=%llu last_address=0x%llx history=[%s]",
                    snapshot.processId, snapshot.threadId,
                    snapshot.stablePolls,
                    machRunStateName(snapshot.runState),
                    static_cast<unsigned long long>(snapshot.hostPC),
                    static_cast<unsigned long long>(snapshot.hostLR),
                    static_cast<unsigned long long>(snapshot.hostSP),
                    static_cast<unsigned long long>(snapshot.guestRIP),
                    static_cast<unsigned long long>(snapshot.processEntryRIP),
                    static_cast<unsigned long long>(snapshot.hostFaults),
                    static_cast<unsigned long long>(snapshot.handledFaults),
                    static_cast<unsigned long long>(snapshot.lastSignal),
                    static_cast<unsigned long long>(snapshot.lastFaultAddress),
                    history);
            // A stalled thread is very often parked in FUTEX_WAIT, and the
            // register dump says which word it is waiting on but nothing about
            // whether that wait can ever end: who else is parked, whether the
            // word still holds what the waiter expects, whether any wake has
            // named it. The guest futex table answers all three and is
            // otherwise printed only by the Vulkan first-frame watchdog, which
            // never arms for a program that wedges before it presents. Bounded
            // so a thread that stalls repeatedly cannot fill a device's disk.
            // Safe from here: every sampled thread has been resumed by now, so
            // this cannot block on a lock held by a suspended thread.
            {
                static std::atomic<unsigned> futexSnapshots {0};
                if (futexSnapshots.fetch_add(1, std::memory_order_relaxed) < 8) {
                    KThread::logFutexSnapshot();
                }
            }
            const auto& gpr = snapshot.guestGPRs;
            reportf("BOXEDWINE_FEX64_STALL_GPRS_A pid=%u tid=%u live_mask=0x%x syscall_info=0x%llx rax=0x%llx rcx=0x%llx rdx=0x%llx rbx=0x%llx rsp=0x%llx rbp=0x%llx rsi=0x%llx rdi=0x%llx",
                    snapshot.processId, snapshot.threadId,
                    snapshot.liveGPRMask,
                    static_cast<unsigned long long>(snapshot.inSyscallInfo),
                    static_cast<unsigned long long>(gpr[FEXCore::X86State::REG_RAX]),
                    static_cast<unsigned long long>(gpr[FEXCore::X86State::REG_RCX]),
                    static_cast<unsigned long long>(gpr[FEXCore::X86State::REG_RDX]),
                    static_cast<unsigned long long>(gpr[FEXCore::X86State::REG_RBX]),
                    static_cast<unsigned long long>(gpr[FEXCore::X86State::REG_RSP]),
                    static_cast<unsigned long long>(gpr[FEXCore::X86State::REG_RBP]),
                    static_cast<unsigned long long>(gpr[FEXCore::X86State::REG_RSI]),
                    static_cast<unsigned long long>(gpr[FEXCore::X86State::REG_RDI]));
            reportf("BOXEDWINE_FEX64_STALL_GPRS_B pid=%u tid=%u r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx fs=0x%llx gs=0x%llx",
                    snapshot.processId, snapshot.threadId,
                    static_cast<unsigned long long>(gpr[FEXCore::X86State::REG_R8]),
                    static_cast<unsigned long long>(gpr[FEXCore::X86State::REG_R9]),
                    static_cast<unsigned long long>(gpr[FEXCore::X86State::REG_R10]),
                    static_cast<unsigned long long>(gpr[FEXCore::X86State::REG_R11]),
                    static_cast<unsigned long long>(gpr[FEXCore::X86State::REG_R12]),
                    static_cast<unsigned long long>(gpr[FEXCore::X86State::REG_R13]),
                    static_cast<unsigned long long>(gpr[FEXCore::X86State::REG_R14]),
                    static_cast<unsigned long long>(gpr[FEXCore::X86State::REG_R15]),
                    static_cast<unsigned long long>(snapshot.fsBase),
                    static_cast<unsigned long long>(snapshot.gsBase));
            reportf("BOXEDWINE_FEX64_STALL_HOST_ARGS pid=%u tid=%u x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx x4=0x%llx x5=0x%llx x6=0x%llx x7=0x%llx call_guest_rip=0x%llx",
                    snapshot.processId, snapshot.threadId,
                    static_cast<unsigned long long>(snapshot.hostArguments[0]),
                    static_cast<unsigned long long>(snapshot.hostArguments[1]),
                    static_cast<unsigned long long>(snapshot.hostArguments[2]),
                    static_cast<unsigned long long>(snapshot.hostArguments[3]),
                    static_cast<unsigned long long>(snapshot.hostArguments[4]),
                    static_cast<unsigned long long>(snapshot.hostArguments[5]),
                    static_cast<unsigned long long>(snapshot.hostArguments[6]),
                    static_cast<unsigned long long>(snapshot.hostArguments[7]),
                    static_cast<unsigned long long>(snapshot.hostCallGuestRIP));
            reportf("BOXEDWINE_FEX64_STALL_HOST_CALL_REGS pid=%u tid=%u x8=0x%llx x9=0x%llx x10=0x%llx x11=0x%llx x12=0x%llx x13=0x%llx x14=0x%llx x15=0x%llx x16=0x%llx x17=0x%llx",
                    snapshot.processId, snapshot.threadId,
                    static_cast<unsigned long long>(snapshot.hostCallRegisters[0]),
                    static_cast<unsigned long long>(snapshot.hostCallRegisters[1]),
                    static_cast<unsigned long long>(snapshot.hostCallRegisters[2]),
                    static_cast<unsigned long long>(snapshot.hostCallRegisters[3]),
                    static_cast<unsigned long long>(snapshot.hostCallRegisters[4]),
                    static_cast<unsigned long long>(snapshot.hostCallRegisters[5]),
                    static_cast<unsigned long long>(snapshot.hostCallRegisters[6]),
                    static_cast<unsigned long long>(snapshot.hostCallRegisters[7]),
                    static_cast<unsigned long long>(snapshot.hostCallRegisters[8]),
                    static_cast<unsigned long long>(snapshot.hostCallRegisters[9]));
            reportf("BOXEDWINE_FEX64_STALL_MEMORY pid=%u tid=%u valid_mask=0x%x chunk_size=0x%llx chunk_user=0x%llx chunk_key=0x%llx arena_lock=0x%llx arena_system=0x%llx tls_address=0x%llx tls_value=0x%llx",
                    snapshot.processId, snapshot.threadId,
                    snapshot.memoryValueMask,
                    static_cast<unsigned long long>(snapshot.chunkSizeWord),
                    static_cast<unsigned long long>(snapshot.chunkUserWord),
                    static_cast<unsigned long long>(snapshot.chunkKeyWord),
                    static_cast<unsigned long long>(snapshot.arenaLockWord),
                    static_cast<unsigned long long>(snapshot.arenaSystemMem),
                    static_cast<unsigned long long>(snapshot.fsBase +
                        gpr[FEXCore::X86State::REG_RDI]),
                    static_cast<unsigned long long>(snapshot.tlsOperandWord));
            if (snapshot.hostCodeValid) {
                reportf("BOXEDWINE_FEX64_STALL_HOST_CODE pid=%u tid=%u address=0x%llx words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
                        snapshot.processId, snapshot.threadId,
                        static_cast<unsigned long long>(snapshot.hostCodeAddress),
                        snapshot.hostCode[0], snapshot.hostCode[1],
                        snapshot.hostCode[2], snapshot.hostCode[3],
                        snapshot.hostCode[4], snapshot.hostCode[5],
                        snapshot.hostCode[6], snapshot.hostCode[7]);
            }
            if (snapshot.hostCallCodeValid) {
                reportf("BOXEDWINE_FEX64_STALL_CALL_CODE pid=%u tid=%u address=0x%llx lr=0x%llx words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
                        snapshot.processId, snapshot.threadId,
                        static_cast<unsigned long long>(
                            snapshot.hostCallCodeAddress),
                        static_cast<unsigned long long>(snapshot.hostLR),
                        snapshot.hostCallCode[0], snapshot.hostCallCode[1],
                        snapshot.hostCallCode[2], snapshot.hostCallCode[3],
                        snapshot.hostCallCode[4], snapshot.hostCallCode[5],
                        snapshot.hostCallCode[6], snapshot.hostCallCode[7]);
            }
            if (snapshot.hostReturnCodeValid) {
                reportf("BOXEDWINE_FEX64_STALL_RETURN_CODE pid=%u tid=%u address=0x%llx lr=0x%llx in_pool=%u pool_offset=0x%llx writable_alias=0x%llx words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
                        snapshot.processId, snapshot.threadId,
                        static_cast<unsigned long long>(
                            snapshot.hostReturnCodeAddress),
                        static_cast<unsigned long long>(snapshot.hostLR),
                        snapshot.hostReturnInPool ? 1u : 0u,
                        static_cast<unsigned long long>(
                            snapshot.hostReturnPoolOffset),
                        static_cast<unsigned long long>(
                            snapshot.hostReturnWritableAlias),
                        snapshot.hostReturnCode[0], snapshot.hostReturnCode[1],
                        snapshot.hostReturnCode[2], snapshot.hostReturnCode[3],
                        snapshot.hostReturnCode[4], snapshot.hostReturnCode[5],
                        snapshot.hostReturnCode[6], snapshot.hostReturnCode[7],
                        snapshot.hostReturnCode[8], snapshot.hostReturnCode[9],
                        snapshot.hostReturnCode[10], snapshot.hostReturnCode[11],
                        snapshot.hostReturnCode[12], snapshot.hostReturnCode[13],
                        snapshot.hostReturnCode[14], snapshot.hostReturnCode[15]);
            }
            Dl_info hostImage {};
            if (snapshot.hostPC != 0 &&
                dladdr(reinterpret_cast<const void*>(snapshot.hostPC),
                       &hostImage) != 0) {
                const uint64_t imageBase = reinterpret_cast<uint64_t>(
                    hostImage.dli_fbase);
                const uint64_t symbolBase = reinterpret_cast<uint64_t>(
                    hostImage.dli_saddr);
                reportf("BOXEDWINE_FEX64_STALL_HOST_IMAGE pid=%u tid=%u image=%s image_base=0x%llx image_offset=0x%llx symbol=%s symbol_base=0x%llx symbol_offset=0x%llx",
                        snapshot.processId, snapshot.threadId,
                        hostImage.dli_fname ? hostImage.dli_fname : "(unknown)",
                        static_cast<unsigned long long>(imageBase),
                        static_cast<unsigned long long>(snapshot.hostPC - imageBase),
                        hostImage.dli_sname ? hostImage.dli_sname : "(unknown)",
                        static_cast<unsigned long long>(symbolBase),
                        static_cast<unsigned long long>(symbolBase != 0
                            ? snapshot.hostPC - symbolBase : 0));

                // iOS may redact dli_sname even for an exported system
                // routine. Resolve a small set of plausible synchronization
                // and memory helpers directly so the next device log can
                // identify the callee without relying on dyld local symbols.
                static constexpr const char* hostCandidates[] = {
                    "memset",
                    "_platform_memset",
                    "__ulock_wait",
                    "__ulock_wait2",
                    "os_sync_wait_on_address",
                    "os_sync_wait_on_address_with_deadline",
                    "os_unfair_lock_lock",
                    "os_unfair_lock_lock_with_options",
                };
                for (const char* candidate : hostCandidates) {
                    void* address = dlsym(RTLD_DEFAULT, candidate);
                    if (address == nullptr) continue;
                    const uint64_t candidateAddress =
                        reinterpret_cast<uint64_t>(address);
                    if (snapshot.hostPC >= candidateAddress &&
                        snapshot.hostPC - candidateAddress < 0x1000) {
                        reportf("BOXEDWINE_FEX64_STALL_HOST_CANDIDATE pid=%u tid=%u name=%s address=0x%llx offset=0x%llx",
                                snapshot.processId, snapshot.threadId,
                                candidate,
                                static_cast<unsigned long long>(
                                    candidateAddress),
                                static_cast<unsigned long long>(
                                    snapshot.hostPC - candidateAddress));
                    }
                }
            }
        }
    }
}

extern "C" const char* BVNFEXBackendStageName(BVNFEXBackendStage stage) {
    switch (stage) {
        case BVNFEXBackendStageUnavailable: return "not linked";
        case BVNFEXBackendStageIdle: return "ready to probe";
        case BVNFEXBackendStageArenaReady: return "executable arena ready";
        case BVNFEXBackendStageContextReady: return "FEX context ready";
        case BVNFEXBackendStageKernelEntered: return "BoxedWine kernel entered";
        case BVNFEXBackendStageExecuted: return "x86-64 probe completed";
    }
    return "unknown";
}

// The translator's block trace normally spends a global budget during
// startup, hundreds of blocks before anything interesting. These arm it again
// for one process and a bounded number of blocks, over the handoff that
// follows the Wine-server reply carrying the process's entry point.
extern "C" void BVNFEXBackendArmHandoffTrace(unsigned processId) {
    FEXCore::Context::BoxedWineArmBlockTrace(processId,
                                             kHandoffTraceBlockBudget);
    reportf("BOXEDWINE_FEX64_HANDOFF_TRACE_ARMED pid=%u budget=%u", processId,
            kHandoffTraceBlockBudget);
}

extern "C" void BVNFEXBackendDisarmHandoffTrace(unsigned processId) {
    if (FEXCore::Context::BoxedWineDisarmBlockTrace(processId)) {
        reportf("BOXEDWINE_FEX64_HANDOFF_TRACE_DONE pid=%u reason=nt-redirect",
                processId);
    }
}

// The session's launched program is the translated process whose parent is the
// boot process: exactly one process per session is translated, and it is that
// one. Its ending is what ends the session; a helper's is not.
//
// The frontend has no other way to learn it. boxedmain() keeps the main thread
// while wineserver, services.exe and winedevice are alive, so a program that
// died in its first seconds left the app presenting a live view of nothing,
// with the runtime still reporting a running session - see BVNRuntime.h.
// Reported here rather than at the guest's exit_group because this is where
// the process has both a terminal status and the loader's search trace, which
// is what names the module a STATUS_DLL_NOT_FOUND was about.
static void reportLaunchedProcessRetired(void* process) {
    KProcess* guest = static_cast<KProcess*>(process);
    // `terminated` is what makes exitCode real. A run that retires the process
    // because the host side failed has not been through exitgroup yet, and the
    // fatal path ends the session on its own with its own report.
    if (guest == nullptr || !guest->terminated || guest->parentId > 1) {
        return;
    }
    char module[BVN_MAX_MODULE_NAME];
    module[0] = '\0';
    guest->dllSearch.lastUnresolvedModule(module, sizeof(module));
    BVNRuntimeNoteLaunchedProcessExited(static_cast<uint32_t>(guest->id),
                                        static_cast<uint32_t>(guest->exitCode),
                                        module);
}

extern "C" bool BVNFEXCPU64Run(void* process, void* thread,
                               BVNFEXCPU64RunOutcome* outcome) {
    // Written on every return path below. A caller that cannot tell a fatal
    // ending from a guest exit_group either leaves a dead process with no exit
    // status or reports a clean exit as a failure; both have happened.
    auto publish = [&](BVNFEXCPU64RunOutcome value) {
        if (outcome) {
            *outcome = value;
        }
    };
    publish(BVNFEXCPU64RunOutcomeFatal);
    reportf("BOXEDWINE_FEX64_RUN attach process=%p thread=%p", process, thread);
    BVNFEXCPU64Adapter* adapter = BVNFEXCPU64AdapterAttach(process, thread);
    if (!adapter) {
        reportf("BOXEDWINE_FEX64_RUN adapter attach failed");
        return false;
    }
    LiveProcessState* processState = nullptr;
    LiveThreadState* threadState = nullptr;
    bool adapterEntered = false;
    auto finish = [&](bool retireThread, bool retireProcess) {
        gCanJump = false;
        gActiveCPU = nullptr;
        if (adapterEntered) {
            BVNFEXCPU64AdapterLeave(adapter);
            adapterEntered = false;
        }
        gActiveSignals = nullptr;
        BVNFEXCPU64AdapterDetach(adapter);
        adapter = nullptr;
        if (processState) {
            releaseLiveThreadRun(processState, threadState, retireThread);
            releaseLiveProcessRun(process, processState, retireProcess);
        }
    };

    if (!initializeFEXGlobals()) {
        reportf("BOXEDWINE_FEX64_RUN global initialization failed");
        finish(false, false);
        return false;
    }
    bool cleanReturn = false;
    BVNFEXCPU64AdapterAction terminalAction =
        BVNFEXCPU64AdapterActionInvalid;
    try {
        processState = getLiveProcessState(process);
        threadState = processState
            ? getLiveThreadState(processState, thread)
            : nullptr;
        if (threadState && processState->fex->signals) {
            gActiveSignals = processState->fex->signals.get();
        }
        if (!threadState || !BVNFEXCPU64AdapterBindFEX(
                adapter, processState->fex->context.get(), threadState->fexThread) ||
            !BVNFEXCPU64AdapterEnter(adapter)) {
            reportf("FEX could not attach a live BoxedWine CPU64 thread");
            finish(true, true);
            return false;
        }
        adapterEntered = true;
        cleanReturn = true;
        gCanJump = true;
        gActiveCPU = nullptr;

        while (true) {
            if (!BVNFEXCPU64AdapterSyncToFEX(
                    adapter, threadState->fexThread->CurrentFrame)) {
                cleanReturn = false;
                reportf("BoxedWine CPU64 state became invalid before FEX entry");
                break;
            }
            if (!processState->entryReported.exchange(
                    true, std::memory_order_acq_rel)) {
                reportf("BOXEDWINE_FEX64_LIVE_ENTER rip=0x%llx",
                        static_cast<unsigned long long>(
                            threadState->fexThread->CurrentFrame->State.rip));
            }
            const uint32_t enteredEpoch =
                processState->execEpoch.load(std::memory_order_acquire);
            if (enteredEpoch != 0 &&
                processState->unwindReports.load(std::memory_order_relaxed) <
                    kUnwindReportLimit) {
                processState->unwindReports.fetch_add(
                    1, std::memory_order_relaxed);
                reportf("BOXEDWINE_FEX64_UNWIND_REENTER epoch=%u context=%p "
                        "thread=%p rip=0x%llx",
                        enteredEpoch,
                        static_cast<void*>(processState->fex->context.get()),
                        static_cast<void*>(threadState->fexThread),
                        static_cast<unsigned long long>(
                            threadState->fexThread->CurrentFrame->State.rip));
            }
            bool returnedByGuard = false;
            auto* enteredContext = processState->fex->context.get();
            auto* enteredThread = threadState->fexThread;
            const int jumpResult = setjmp(gExitJump);
            if (jumpResult == 0) {
                processState->fex->context->ExecuteThread(threadState->fexThread);
            } else {
                returnedByGuard = true;
            }

            const BVNFEXCPU64AdapterAction action =
                BVNFEXCPU64AdapterLastAction(adapter);
            if (processState->unwindReports.load(std::memory_order_relaxed) <
                kUnwindReportLimit) {
                processState->unwindReports.fetch_add(
                    1, std::memory_order_relaxed);
                reportf("BOXEDWINE_FEX64_UNWIND_RETURN epoch=%u "
                        "entered_context=%p live_context=%p entered_thread=%p "
                        "live_thread=%p action=%d guard=%d rip=0x%llx",
                        processState->execEpoch.load(std::memory_order_acquire),
                        static_cast<void*>(enteredContext),
                        static_cast<void*>(processState->fex->context.get()),
                        static_cast<void*>(enteredThread),
                        static_cast<void*>(threadState->fexThread),
                        static_cast<int>(action), returnedByGuard ? 1 : 0,
                        static_cast<unsigned long long>(
                            threadState->fexThread->CurrentFrame->State.rip));
            }
            terminalAction = action;
            if (action == BVNFEXCPU64AdapterActionInvalid) {
                cleanReturn = false;
                reportf("FEX live CPU64 execution returned an invalid action");
                break;
            }
            if (action == BVNFEXCPU64AdapterActionThreadExit ||
                action == BVNFEXCPU64AdapterActionProcessExit) {
                break;
            }
            if (!BVNFEXCPU64AdapterSyncFromFEX(
                    adapter, threadState->fexThread->CurrentFrame)) {
                cleanReturn = false;
                reportf("FEX returned with an invalid BoxedWine CPU64 state");
                break;
            }
            if (action == BVNFEXCPU64AdapterActionExec) {
                const uint64_t resumeRIP =
                    threadState->fexThread->CurrentFrame->State.rip;
                reportf("BOXEDWINE_FEX64_CONTEXT_RESET rip=0x%llx "
                        "reason=exec-boundary context=recreated",
                        static_cast<unsigned long long>(
                            threadState->fexThread->CurrentFrame->State.rip));
                if (!recreateLiveContextAfterExec(
                        processState, threadState, adapter, resumeRIP)) {
                    cleanReturn = false;
                    reportf("FEX could not recreate the loader execution epoch");
                    break;
                }
                if (!BVNFEXCPU64AdapterSyncFromFEX(
                        adapter, threadState->fexThread->CurrentFrame)) {
                    cleanReturn = false;
                    reportf("FEX could not restore the recovered loader entry");
                    break;
                }
                reportf("BOXEDWINE_FEX64_CONTEXT_RESET_DONE frame_rip=0x%llx",
                        static_cast<unsigned long long>(
                            threadState->fexThread->CurrentFrame->State.rip));
                BVNFEXCPU64AdapterResetAction(adapter);
                // The old dispatcher's stop handler returned through this
                // ExecuteThread invocation. Let that runner and its signal
                // epoch unwind completely before entering the replacement
                // context. BoxedWine's CPU64 scheduler immediately dispatches
                // the still-live thread again; re-entering here occasionally
                // branched through a stale null continuation on the first run.
                reportf("BOXEDWINE_FEX64_CONTEXT_RESET_DEFERRED rip=0x%llx",
                        static_cast<unsigned long long>(
                            threadState->fexThread->CurrentFrame->State.rip));
                break;
            }
            if (returnedByGuard || action == BVNFEXCPU64AdapterActionYield) {
                break;
            }
            break;
        }
    } catch (...) {
        cleanReturn = false;
        reportf("FEX live CPU64 execution raised an exception");
    }

    const bool retireThread =
        !cleanReturn ||
        terminalAction == BVNFEXCPU64AdapterActionThreadExit ||
        terminalAction == BVNFEXCPU64AdapterActionProcessExit ||
        terminalAction == BVNFEXCPU64AdapterActionFatalExit ||
        static_cast<KThread*>(thread)->terminating;
    const bool retireProcess =
        !cleanReturn ||
        terminalAction == BVNFEXCPU64AdapterActionProcessExit ||
        terminalAction == BVNFEXCPU64AdapterActionFatalExit ||
        static_cast<KProcess*>(process)->terminated;
    // A contained host fault returns cleanly through the dispatcher's own
    // unwind, so `cleanReturn` says nothing about whether the guest is still
    // viable. Only the action does.
    const bool fatal =
        !cleanReturn || terminalAction == BVNFEXCPU64AdapterActionFatalExit;
    publish(fatal ? BVNFEXCPU64RunOutcomeFatal
                  : (terminalAction == BVNFEXCPU64AdapterActionThreadExit ||
                     terminalAction == BVNFEXCPU64AdapterActionProcessExit)
                        ? BVNFEXCPU64RunOutcomeGuestExit
                        : BVNFEXCPU64RunOutcomeYield);
    if (processState &&
        processState->unwindReports.load(std::memory_order_relaxed) <
            kUnwindReportLimit) {
        processState->unwindReports.fetch_add(1, std::memory_order_relaxed);
        reportf("BOXEDWINE_FEX64_UNWIND_RUN_RETURN epoch=%u context=%p "
                "thread=%p action=%d clean=%d retire_thread=%d "
                "retire_process=%d rip=0x%llx",
                processState->execEpoch.load(std::memory_order_acquire),
                static_cast<void*>(processState->fex
                                       ? processState->fex->context.get()
                                       : nullptr),
                static_cast<void*>(threadState ? threadState->fexThread
                                               : nullptr),
                static_cast<int>(terminalAction), cleanReturn ? 1 : 0,
                retireThread ? 1 : 0, retireProcess ? 1 : 0,
                static_cast<unsigned long long>(
                    (threadState && threadState->fexThread &&
                     threadState->fexThread->CurrentFrame)
                        ? threadState->fexThread->CurrentFrame->State.rip
                        : 0));
    }
    if (retireProcess) {
        reportLaunchedProcessRetired(process);
    }
    finish(retireThread, retireProcess);
    return cleanReturn;
}

extern "C" BVNFEXBackendStage BVNFEXBackendProbe(void) {
    std::lock_guard<std::mutex> guard(gProbeMutex);
    if (gStage.load() == BVNFEXBackendStageExecuted) {
        return BVNFEXBackendStageExecuted;
    }
    if (!initializeFEXGlobals()) {
        return gStage.load();
    }
    gStage.store(BVNFEXBackendStageArenaReady, std::memory_order_release);

    if (!mapGuestProbe()) {
        return gStage.load();
    }
    // Probe only: one guest instruction per translated block. Compilation
    // "leave" proves the translator emitted something, not that the guest
    // retired it. At one instruction per block the bounded block trace names
    // the exact last instruction that actually executed, which is the
    // difference between a faulting load, a bad vector mask, a lost flag and
    // a mis-taken backedge. The live guest keeps the ordinary decoder limit;
    // this is restored immediately after the probe thread stops.
    FEXCore::Config::Set(FEXCore::Config::ConfigOption::CONFIG_MAXINST, "1");
    reportf("BOXEDWINE_FEX64_PROBE_TRACE maxinst=1 scope=probe");
    // Every early return below must put the decoder limit back before the
    // live guest is created.
    struct ProbeInstructionLimitScope final {
        ~ProbeInstructionLimitScope() {
            FEXCore::Config::Erase(
                FEXCore::Config::ConfigOption::CONFIG_MAXINST);
        }
    } probeInstructionLimit;
    gProbeContext = createFEXContext(boxedvn::FexGuestMode::X86_64,
                                     &createProbeInitialThread);
    if (!gProbeContext) {
        return gStage.load();
    }
    if (!installFEXHostSignalHandlers()) {
        reportf("FEX host fault handlers could not be installed");
        return gStage.load();
    }
    gStage.store(BVNFEXBackendStageContextReady, std::memory_order_release);
    auto* thread = gProbeContext->initialThread;
    if (thread == nullptr) return gStage.load();

    constexpr size_t shadowBytes =
        FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
    gCallRetMapping = mmap(nullptr, shadowBytes + 2 * kPageBytes, PROT_NONE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
    if (gCallRetMapping == MAP_FAILED) {
        reportf("failed to reserve FEX's call/return shadow stack");
        return gStage.load();
    }
    void* shadow = static_cast<uint8_t*>(gCallRetMapping) + kPageBytes;
    if (mprotect(shadow, shadowBytes, PROT_READ | PROT_WRITE) != 0) {
        reportf("failed to make FEX's call/return shadow stack writable");
        return gStage.load();
    }
    thread->CallRetStackBase = shadow;
    thread->CurrentFrame->State.callret_sp =
        reinterpret_cast<uint64_t>(shadow) + shadowBytes / 4;
    thread->CurrentFrame->State.callret_sp_base =
        reinterpret_cast<uint64_t>(shadow);

    static FEXCore::Core::CPUState::gdt_segment
        gdt[K_GUEST_SEGMENT_TABLE_ENTRIES] {};
    publishGuestDescriptorTable(thread->CurrentFrame->State, gdt);

    gCanJump = true;
    gProbeExited.store(false, std::memory_order_release);
    gProbeExitCode.store(0, std::memory_order_release);
    gActiveCPU = gProbeCPU.get();
    gActiveSignals = gProbeContext->signals.get();
    if (setjmp(gExitJump) == 0) {
        gProbeContext->context->ExecuteThread(thread);
        gActiveCPU = nullptr;
        gActiveSignals = nullptr;
        gCanJump = false;
        reportf("FEX returned without the guest issuing exit");
        return gStage.load();
    }
    gActiveCPU = nullptr;
    gActiveSignals = nullptr;
    gCanJump = false;
    gStage.store(BVNFEXBackendStageKernelEntered, std::memory_order_release);
    if (!gProbeExited.load(std::memory_order_acquire) ||
        gProbeExitCode.load(std::memory_order_acquire) != kExpectedExitCode) {
        reportf("BoxedWine kernel exit mismatch: expected 47, observed %d",
                static_cast<int>(gProbeExitCode.load(std::memory_order_acquire)));
        return gStage.load();
    }
    reportf("x86-64 SSE2/REP-STOS/call-ret probe passed through FEX and "
            "returned through BoxedWine's CPU64 syscall dispatcher "
            "(translated pool used %zu KiB)",
            gPoolUsed.load() / 1024);
    gStage.store(BVNFEXBackendStageExecuted, std::memory_order_release);
    return BVNFEXBackendStageExecuted;
}

#endif
