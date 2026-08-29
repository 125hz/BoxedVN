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
extern "C" bool BVNFEXCPU64Run(void*, void*) { return false; }
extern "C" const char* BVNFEXBackendStageName(BVNFEXBackendStage stage) {
    return stage == BVNFEXBackendStageUnavailable ? "not linked" : "unknown";
}

#else

#include "boxedvn/fex64_kernel.h"
#include "boxedvn/fex_code_buffer_layout.h"
#include "boxedvn/fex_exit_dispatch_contract.h"
#include "boxedvn/fex_guest_mode_policy.h"
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

void* gPoolRX = nullptr;
void* gPoolRW = nullptr;
size_t gPoolSize = 0;
std::atomic<size_t> gPoolUsed {0};
boxedvn::FexCodeBufferPool gCodeBufferPool;

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

std::unique_ptr<FEXContextBundle> gProbeContext;

struct LiveThreadState {
    FEXCore::Core::InternalThreadState* fexThread = nullptr;
    void* callRetMapping = nullptr;
    size_t callRetMappingSize = 0;
    FEXCore::Core::CPUState::gdt_segment gdt[1] {};
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
    const uintptr_t base = reinterpret_cast<uintptr_t>(gPoolRX);
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    return gPoolRX != nullptr && address >= base && address < base + gPoolSize;
}

void* allocateTranslatedCode(size_t length) {
    std::lock_guard<std::mutex> guard(gPoolMutex);
    const auto allocation = gCodeBufferPool.allocate(
        length, gPoolSize, kPageBytes, kFEXPageBytes);
    if (allocation.has_value()) {
        const auto& layout = allocation->layout;
        gPoolUsed.store(gCodeBufferPool.cursor(), std::memory_order_relaxed);
        static std::atomic<uint32_t> reported {0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
            reportf("FEX executable %s request=0x%zx rx_offset=0x%zx "
                    "guard_offset=0x%zx next_offset=0x%zx host_page=0x%zx",
                    allocation->reused ? "reuse" : "carve", length,
                    layout.allocationOffset, layout.guardOffset,
                    layout.nextOffset, kPageBytes);
        }
        return static_cast<uint8_t*>(gPoolRX) + layout.allocationOffset;
    }
    reportf("translator executable pool exhausted by a %zu-byte request", length);
    return MAP_FAILED;
}

void* fexMmap(void* address, size_t length, int protection, int flags,
              int descriptor, off_t offset) {
    if ((protection & PROT_EXEC) != 0) {
        return allocateTranslatedCode(length);
    }
    return mmap(address, length, protection, flags, descriptor, offset);
}

int fexMunmap(void* address, size_t length) {
    if (!poolOwns(address)) {
        return munmap(address, length);
    }
    std::lock_guard<std::mutex> guard(gPoolMutex);
    const size_t offset = static_cast<uint8_t*>(address) -
                          static_cast<uint8_t*>(gPoolRX);
    if (!gCodeBufferPool.release(offset, length)) {
        reportf("FEX executable release did not match a live carve "
                "request=0x%zx rx_offset=0x%zx", length, offset);
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
    features.SupportsAVX = false;
    features.SupportsSVE128 = false;
    features.SupportsSVE256 = false;
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
        mach_task_self(), address, sizeof(value),
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
        }
    }
    chainFEXHostSignal(signal, info, ucontext);
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
        gFEXSignalHandlersInstalled.store(true, std::memory_order_release);
    });
    return gFEXSignalHandlersInstalled.load(std::memory_order_acquire);
}

bool preparePool() {
    std::lock_guard<std::mutex> guard(gPoolMutex);
    if (gPoolRX != nullptr) {
        return true;
    }
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
    for (size_t candidate = kPoolBytes; candidate >= kMinimumPoolBytes;
         candidate /= 2) {
        gPoolRX = BVNExecMemAlloc(candidate);
        if (gPoolRX != nullptr) {
            gPoolSize = candidate;
            break;
        }
    }
    if (gPoolRX == nullptr) {
        reportf("the shared executable arena has no segment for FEX");
        return false;
    }
    gPoolRW = BVNExecMemWritableAddress(gPoolRX, gPoolSize);
    if (gPoolRW == nullptr) {
        reportf("the FEX executable pool has no writable alias");
        return false;
    }
    FEXCore::DualMap::WriteOffset =
        static_cast<int64_t>(static_cast<uint8_t*>(gPoolRW) -
                             static_cast<uint8_t*>(gPoolRX));
    reportf("shared arena assigned %zu MiB to FEX (rx=%p rw=%p)",
            gPoolSize / (1024 * 1024), gPoolRX, gPoolRW);
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
    bundle->context = FEXCore::Context::Context::CreateNewContext(
        appleHostFeatures());
    if (!bundle->context) {
        reportf("FEX refused to create a %s translator context",
                boxedvn::fexGuestModeName(mode));
        return nullptr;
    }
    bundle->context->SetSignalDelegator(bundle->signals.get());
    bundle->context->SetSyscallHandler(bundle->syscalls.get());
    bundle->context->SetHardwareTSOSupport(true);
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
    void* mapping = mmap(nullptr, mappingSize, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    gGuestStack = mmap(nullptr, kGuestStackBytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED || gGuestStack == MAP_FAILED) {
        reportf("the bundled ELF64 load span or stack could not be mapped");
        return false;
    }
    std::memset(mapping, 0, mappingSize);
    for (const boxedvn::ELFLoadSegment& segment : image.loadSegments) {
        const uint64_t destinationOffset = segment.virtualAddress - first;
        if (destinationOffset > mappingSize ||
            segment.fileSize > mappingSize - destinationOffset) {
            reportf("the bundled ELF64 segment escaped its checked load span");
            return false;
        }
        std::memcpy(static_cast<uint8_t*>(mapping) + destinationOffset,
                    static_cast<const uint8_t*>(data.bytes) + segment.fileOffset,
                    static_cast<size_t>(segment.fileSize));
    }
    const uint64_t entryOffset = image.entry - first;
    if (image.entry < first || entryOffset >= mappingSize) {
        reportf("the bundled ELF64 entry lies outside its load span");
        return false;
    }
    const auto addIdentity = [](void* pointer, size_t size, uint8_t access) {
        return gAddressSpace.add({reinterpret_cast<uint64_t>(pointer), size,
                                  reinterpret_cast<uintptr_t>(pointer), access});
    };
    if (!addIdentity(mapping, mappingSize,
                     boxedvn::GuestMemoryRead |
                     boxedvn::GuestMemoryWrite |
                     boxedvn::GuestMemoryExecute) ||
        !addIdentity(gGuestStack, kGuestStackBytes,
                     boxedvn::GuestMemoryRead | boxedvn::GuestMemoryWrite)) {
        reportf("the bundled ELF64 mapping or stack could not be registered");
        return false;
    }

    // FEX fetches and accesses the identity-mapped host image directly.  The
    // BoxedWine CPU64 mirror is still required because write/exit return
    // through the real syscall dispatcher, which reads guest buffers through
    // KMemory64 rather than through FEX's host pointer.
    gProbeMemory = std::make_unique<KMemory64>(nullptr);
    gProbeCPU = std::make_unique<CPU64>(gProbeMemory.get());
    const uint64_t mappingAddress = reinterpret_cast<uint64_t>(mapping);
    const uint64_t stackAddress = reinterpret_cast<uint64_t>(gGuestStack);
    const uint64_t mirroredImage = gProbeMemory->mmapAnonymousFixed(
        mappingAddress, mappingSize, 0x7);
    const uint64_t mirroredStack = gProbeMemory->mmapAnonymousFixed(
        stackAddress, kGuestStackBytes, 0x3);
    if (mirroredImage != mappingAddress || mirroredStack != stackAddress) {
        reportf("the CPU64 correctness mirror failed: image=0x%llx stack=0x%llx",
                static_cast<unsigned long long>(mirroredImage),
                static_cast<unsigned long long>(mirroredStack));
        return false;
    }
    gProbeMemory->memcpyToGuest(
        mappingAddress, mapping, mappingSize);

    gGuestCode = mapping;
    gGuestEntry = reinterpret_cast<uint64_t>(mapping) + entryOffset;
    gProbeCPU->rip = gGuestEntry;
    gProbeCPU->reg[X64_RSP].setU64(
        reinterpret_cast<uint64_t>(gGuestStack) + kGuestStackBytes - 0x100);
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

    auto state = std::make_unique<LiveProcessState>();
    state->fex = createFEXContext(boxedvn::FexGuestMode::X86_64,
                                  &createLiveInitialThread);
    if (!state->fex) return nullptr;
    if (!installFEXHostSignalHandlers()) {
        reportf("FEX host fault handlers could not be installed");
        return nullptr;
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
    state->gdt[0].L = 1;
    state->gdt[0].P = 1;
    state->gdt[0].S = 1;
    state->gdt[0].Type = 0b1011;
    state->fexThread->CurrentFrame->State.segment_arrays[0] = state->gdt;
    state->fexThread->CurrentFrame->State.cs_idx = 0;
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
    replacement->CurrentFrame->State.segment_arrays[0] = threadState->gdt;
    replacement->CurrentFrame->State.cs_idx = 0;

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
    std::unique_ptr<FEXContextBundle> retiredBundle;
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
        gLiveThreadContexts.erase(retired);
        gLiveThreadContexts.emplace(replacement,
                                    replacementBundle->context.get());
        threadState->fexThread = replacement;
        replacementBundle->initialThread = nullptr;
        retiredBundle = std::move(processState->fex);
        processState->fex = std::move(replacementBundle);
    }
    retiredBundle->context->DestroyThread(retired);
    return true;
}

} // namespace

extern "C" bool BVNFEXBackendOwnsHostCodeAddress(uint64_t address) {
    return poolOwns(reinterpret_cast<const void*>(
        static_cast<uintptr_t>(address)));
}

extern "C" uint64_t BVNFEXBackendWritableHostCodeAddress(uint64_t address) {
    if (!poolOwns(reinterpret_cast<const void*>(
            static_cast<uintptr_t>(address))) || gPoolRW == nullptr) {
        return 0;
    }
    const uintptr_t offset = static_cast<uintptr_t>(address) -
        reinterpret_cast<uintptr_t>(gPoolRX);
    return reinterpret_cast<uintptr_t>(gPoolRW) + offset;
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
            for (const auto& threadEntry : processState->threads) {
                if (snapshotCount == snapshots.size()) break;
                auto* thread = static_cast<KThread*>(threadEntry.first);
                LiveThreadState* threadState = threadEntry.second.get();
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

                        const uintptr_t poolBase =
                            reinterpret_cast<uintptr_t>(gPoolRX);
                        const uintptr_t returnAddress =
                            static_cast<uintptr_t>(snapshot.hostLR - 4);
                        snapshot.hostReturnInPool =
                            poolOwns(reinterpret_cast<const void*>(returnAddress));
                        if (snapshot.hostReturnInPool) {
                            snapshot.hostReturnPoolOffset =
                                returnAddress - poolBase;
                            if (gPoolRW != nullptr) {
                                snapshot.hostReturnWritableAlias =
                                    reinterpret_cast<uintptr_t>(gPoolRW) +
                                    snapshot.hostReturnPoolOffset;
                            }
                        }
                    }
                    if (processState->fex->context->IsAddressInCodeBuffer(
                            threadState->fexThread, snapshot.hostPC)) {
                        const uint64_t restored =
                            processState->fex->context->RestoreRIPFromHostPC(
                                threadState->fexThread, snapshot.hostPC);
                        if (restored != 0) snapshot.guestRIP = restored;

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
                        processState->fex->context->IsAddressInCodeBuffer(
                            threadState->fexThread, snapshot.hostLR - 4)) {
                        snapshot.hostCallGuestRIP =
                            processState->fex->context->RestoreRIPFromHostPC(
                                threadState->fexThread, snapshot.hostLR - 4);
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
            reportf("BOXEDWINE_FEX64_SAMPLE poll=%llu pid=%u tid=%u state=%s cpu=%.1f%% host_pc=0x%llx host_lr=0x%llx host_sp=0x%llx guest_rip=0x%llx entry_rip=0x%llx frame_rip=0x%llx faults=%llu handled=%llu new_faults=%llu last_signal=%llu last_address=0x%llx last_fault_pc=0x%llx history=[%s]",
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
        }
        if (snapshot.emitWarning) {
            reportf("BOXEDWINE_FEX64_STALL pid=%u tid=%u stable_samples=%u state=%s host_pc=0x%llx host_lr=0x%llx host_sp=0x%llx guest_rip=0x%llx entry_rip=0x%llx faults=%llu handled=%llu last_signal=%llu last_address=0x%llx history=[%s]",
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

extern "C" bool BVNFEXCPU64Run(void* process, void* thread) {
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
            bool returnedByGuard = false;
            const int jumpResult = setjmp(gExitJump);
            if (jumpResult == 0) {
                processState->fex->context->ExecuteThread(threadState->fexThread);
            } else {
                returnedByGuard = true;
            }

            const BVNFEXCPU64AdapterAction action =
                BVNFEXCPU64AdapterLastAction(adapter);
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
                continue;
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
        static_cast<KThread*>(thread)->terminating;
    const bool retireProcess =
        !cleanReturn ||
        terminalAction == BVNFEXCPU64AdapterActionProcessExit ||
        static_cast<KProcess*>(process)->terminated;
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

    static FEXCore::Core::CPUState::gdt_segment gdt[1] {};
    gdt[0].L = 1;
    gdt[0].P = 1;
    gdt[0].S = 1;
    gdt[0].Type = 0b1011;
    thread->CurrentFrame->State.segment_arrays[0] = gdt;
    thread->CurrentFrame->State.cs_idx = 0;

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
