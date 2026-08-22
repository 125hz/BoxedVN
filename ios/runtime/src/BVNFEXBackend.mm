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
extern "C" bool BVNFEXCPU64Run(void*, void*) { return false; }
extern "C" const char* BVNFEXBackendStageName(BVNFEXBackendStage stage) {
    return stage == BVNFEXBackendStageUnavailable ? "not linked" : "unknown";
}

#else

#include "boxedvn/fex64_kernel.h"
#include "boxedvn/guest_address_space.h"
#include "boxedvn/elf_inspector.h"

#import <Foundation/Foundation.h>

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
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
    sys_icache_invalidate(start,
        static_cast<size_t>(static_cast<char*>(end) - static_cast<char*>(start)));
}

int rpm_cas_snapshot_take(void* snapshot) {
    (void)snapshot;
    return 0;
}
}

namespace {

constexpr size_t kPageBytes = 0x4000;
constexpr size_t kPoolBytes = 64u * 1024u * 1024u;
constexpr size_t kMinimumPoolBytes = 4u * 1024u * 1024u;
constexpr size_t kGuestCodeBytes = kPageBytes;
constexpr size_t kGuestStackBytes = 256u * 1024u;
constexpr uint64_t kExpectedExitCode = 42;

std::mutex gProbeMutex;
std::mutex gReportMutex;
std::mutex gPoolMutex;
std::atomic<BVNFEXBackendStage> gStage {BVNFEXBackendStageIdle};
std::string gReport;

void* gPoolRX = nullptr;
void* gPoolRW = nullptr;
size_t gPoolSize = 0;
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
thread_local bool gCanJump = false;
std::unique_ptr<KMemory64> gProbeMemory;
std::unique_ptr<CPU64> gProbeCPU;
thread_local CPU64* gActiveCPU = nullptr;
std::atomic<bool> gProbeExited {false};
std::atomic<uint64_t> gProbeExitCode {0};

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
};

struct LiveProcessState {
    fextl::unique_ptr<FEXCore::Context::Context> context;
    std::unordered_map<void*, std::unique_ptr<LiveThreadState>> threads;
    std::atomic<size_t> activeRuns {0};
    std::atomic<bool> retiring {false};
    std::atomic<bool> entryReported {false};
};

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
    const size_t aligned = alignUp(length, kPageBytes);
    size_t offset = gPoolUsed.load(std::memory_order_relaxed);
    while (offset <= gPoolSize && aligned <= gPoolSize - offset) {
        if (gPoolUsed.compare_exchange_weak(offset, offset + aligned,
                                            std::memory_order_relaxed)) {
            return static_cast<uint8_t*>(gPoolRX) + offset;
        }
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
    return poolOwns(address) ? 0 : munmap(address, length);
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
    BoxedWineSyscalls() {
        OSABI = FEXCore::HLE::SyscallOSABI::OS_LINUX64;
    }

    uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame* frame,
                           FEXCore::HLE::SyscallArguments* arguments) override {
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
};

class BoxedWineSignals final : public FEXCore::SignalDelegator {};

BoxedWineSyscalls gSyscalls;
BoxedWineSignals gSignals;
fextl::unique_ptr<FEXCore::Context::Context> gContext;

constexpr std::array<int, 4> kFEXHostSignals {
    SIGSEGV, SIGBUS, SIGILL, SIGFPE,
};
std::array<struct sigaction, kFEXHostSignals.size()> gPreviousFEXSignals {};
std::once_flag gFEXSignalInstallOnce;
std::atomic<bool> gFEXSignalHandlersInstalled {false};
std::atomic<uint64_t> gFEXHostFaultCount {0};
std::atomic<uint64_t> gFEXKuserFaultCount {0};

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
    if (info && info->si_addr) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(info->si_addr);
        if (address >= K64_KUSER_SHARED_BASE &&
            address < K64_KUSER_SHARED_BASE + K64_KUSER_SHARED_SIZE) {
            gFEXKuserFaultCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
    BVNFEXCPU64Adapter* adapter = BVNFEXCPU64AdapterCurrent();
    if (adapter && BVNFEXCPU64AdapterHandleHostFault(
            adapter, &gSignals.GetConfig(), signal, info, ucontext)) {
        return;
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
        gFEXGlobalsReady = true;
    });
    return gFEXGlobalsReady;
}

bool mapBundledELFProbe() {
    NSString* path = [[NSBundle mainBundle]
        pathForResource:@"boxedvn-fex64-kernel-probe" ofType:nil];
    if (path == nil) {
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
    if (mapping == MAP_FAILED) {
        reportf("the bundled ELF64 load span could not be mapped");
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
    if (!gAddressSpace.add({reinterpret_cast<uint64_t>(mapping), mappingSize,
                            reinterpret_cast<uintptr_t>(mapping),
                            boxedvn::GuestMemoryRead |
                            boxedvn::GuestMemoryWrite |
                            boxedvn::GuestMemoryExecute})) {
        reportf("the bundled ELF64 mapping could not be registered");
        return false;
    }
    gGuestCode = mapping;
    gGuestEntry = reinterpret_cast<uint64_t>(mapping) + entryOffset;
    reportf("loaded bundled PIE ELF64 process: %zu segments, entry=%p",
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
    reportf("preparing a translated process against BoxedWine's CPU64 kernel state");
    return mapRawGuestProbe();
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
    // The probe uses the original global context rather than the live process
    // table. Preserve FEX's SMC invalidation semantics there as well; silently
    // dropping this callback leaves stale translated blocks after self-modifying
    // guest code.
    if (gContext != nullptr) {
        gContext->InvalidateThreadCachedCodeRange(thread, start, length);
    }
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
    state->context = FEXCore::Context::Context::CreateNewContext(appleHostFeatures());
    if (!state->context) return nullptr;
    state->context->SetSignalDelegator(&gSignals);
    state->context->SetSyscallHandler(&gSyscalls);
    state->context->SetHardwareTSOSupport(true);
    if (!state->context->InitCore()) return nullptr;
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
        return it->second.get();
    }

    auto state = std::make_unique<LiveThreadState>();
    state->fexThread = processState->context->CreateThread(0, 0);
    if (!state->fexThread) return nullptr;

    constexpr size_t shadowBytes =
        FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
    state->callRetMappingSize = shadowBytes + 2 * kPageBytes;
    state->callRetMapping = mmap(nullptr, state->callRetMappingSize, PROT_NONE,
                                 MAP_PRIVATE | MAP_ANON, -1, 0);
    if (state->callRetMapping == MAP_FAILED) {
        processState->context->DestroyThread(state->fexThread);
        state->callRetMapping = nullptr;
        state->fexThread = nullptr;
        return nullptr;
    }
    void* shadow = static_cast<uint8_t*>(state->callRetMapping) + kPageBytes;
    if (mprotect(shadow, shadowBytes, PROT_READ | PROT_WRITE) != 0) {
        munmap(state->callRetMapping, state->callRetMappingSize);
        processState->context->DestroyThread(state->fexThread);
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
    gLiveThreadContexts.emplace(state->fexThread, processState->context.get());
    state->active = true;
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
        processState->context->DestroyThread(retiredState->fexThread);
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
            state->context->DestroyThread(thread->fexThread);
            thread->fexThread = nullptr;
        }
        if (thread->callRetMapping) {
            munmap(thread->callRetMapping, thread->callRetMappingSize);
            thread->callRetMapping = nullptr;
        }
    }
    state->threads.clear();
}

void resetLiveThreadAfterExec(LiveProcessState* processState,
                              LiveThreadState* threadState) {
    if (!processState || !threadState || !threadState->fexThread) return;
    processState->context->ClearCodeCache(threadState->fexThread, true);
    if (threadState->callRetMapping && threadState->callRetMappingSize > 0) {
        void* shadow = static_cast<uint8_t*>(threadState->callRetMapping) +
                       kPageBytes;
        const size_t shadowBytes =
            FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
        // ClearCodeCache() already invokes FEX's ResetCallRetStack(), which
        // decommits the full reservation and lets the VM supply zero-fill-on-
        // demand pages. Do not memset the reservation here: that would make
        // every exec transition resident again and defeat FEX's reclaim path.
        threadState->fexThread->CallRetStackBase = shadow;
        threadState->fexThread->CurrentFrame->State.callret_sp =
            reinterpret_cast<uint64_t>(shadow) + shadowBytes / 4;
        threadState->fexThread->CurrentFrame->State.callret_sp_base =
            reinterpret_cast<uint64_t>(shadow);
    }
}

} // namespace

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
        char diagnostics[160];
        snprintf(diagnostics, sizeof(diagnostics),
                 "host_faults=%llu kuser_faults=%llu\n",
                 static_cast<unsigned long long>(hostFaults),
                 static_cast<unsigned long long>(kuserFaults));
        snapshot.append(diagnostics);
    }
    return snapshot.c_str();
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
        if (!threadState || !BVNFEXCPU64AdapterBindFEX(
                adapter, processState->context.get(), threadState->fexThread) ||
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
                processState->context->ExecuteThread(threadState->fexThread);
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
                resetLiveThreadAfterExec(processState, threadState);
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

    gContext = FEXCore::Context::Context::CreateNewContext(appleHostFeatures());
    if (!gContext) {
        reportf("FEX refused to create a translator context");
        return gStage.load();
    }
    gContext->SetSignalDelegator(&gSignals);
    gContext->SetSyscallHandler(&gSyscalls);
    gContext->SetHardwareTSOSupport(true);
    if (!gContext->InitCore()) {
        reportf("FEX failed to initialise its dispatcher");
        return gStage.load();
    }
    if (!installFEXHostSignalHandlers()) {
        reportf("FEX host fault handlers could not be installed");
        return gStage.load();
    }
    gStage.store(BVNFEXBackendStageContextReady, std::memory_order_release);

    if (!mapGuestProbe()) {
        return gStage.load();
    }
    const uint64_t stack = reinterpret_cast<uint64_t>(gGuestStack) +
                           kGuestStackBytes - 0x100;
    auto* thread = gContext->CreateThread(gGuestEntry, stack);
    if (thread == nullptr) {
        reportf("FEX failed to create the x86-64 guest thread");
        return gStage.load();
    }

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
    if (setjmp(gExitJump) == 0) {
        gContext->ExecuteThread(thread);
        gActiveCPU = nullptr;
        gCanJump = false;
        reportf("FEX returned without the guest issuing exit");
        return gStage.load();
    }
    gActiveCPU = nullptr;
    gCanJump = false;
    gStage.store(BVNFEXBackendStageKernelEntered, std::memory_order_release);
    if (!gProbeExited.load(std::memory_order_acquire) ||
        gProbeExitCode.load(std::memory_order_acquire) != kExpectedExitCode) {
        reportf("BoxedWine kernel exit mismatch: expected 42, observed %d",
                static_cast<int>(gProbeExitCode.load(std::memory_order_acquire)));
        return gStage.load();
    }
    reportf("x86-64 executed through FEX and returned through BoxedWine's "
            "CPU64 syscall dispatcher (translated pool used %zu KiB)",
            gPoolUsed.load() / 1024);
    gStage.store(BVNFEXBackendStageExecuted, std::memory_order_release);
    return BVNFEXBackendStageExecuted;
}

#endif
