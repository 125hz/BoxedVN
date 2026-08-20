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

#include <libkern/OSCacheControl.h>
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
#include <mutex>
#include <optional>
#include <string>

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

std::jmp_buf gExitJump;
std::atomic<bool> gCanJump {false};

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

    uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame*,
                           FEXCore::HLE::SyscallArguments* arguments) override {
        boxedvn::Linux64Syscall syscall;
        syscall.number = arguments->Argument[0];
        for (size_t index = 0; index < syscall.arguments.size(); ++index) {
            syscall.arguments[index] = arguments->Argument[index + 1];
        }
        const boxedvn::Linux64SyscallResult result = gKernel.dispatch(syscall);
        if (result.action != boxedvn::Linux64SyscallAction::Continue) {
            if (!gCanJump.load(std::memory_order_acquire)) {
                reportf("guest exit arrived outside the guarded execution window");
                return static_cast<uint64_t>(-1);
            }
            std::longjmp(gExitJump, 1);
        }
        return static_cast<uint64_t>(result.value);
    }

    FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(
        FEXCore::Core::InternalThreadState*, uint64_t address) override {
        const auto range = gAddressSpace.executableRange(address);
        if (!range.has_value()) {
            return {0, 0, false};
        }
        return {range->guestBase, range->size, false};
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

bool preparePool() {
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
    gGuestEntry = reinterpret_cast<uint64_t>(gGuestCode);
    return true;
}

bool mapGuestProbe() {
    if (mapBundledELFProbe()) {
        // The real ELF process owns its write buffer, so only a stack remains
        // to be supplied by the BoxedWine process bootstrap.
        gGuestStack = mmap(nullptr, kGuestStackBytes, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
        if (gGuestStack == MAP_FAILED ||
            !gAddressSpace.add({reinterpret_cast<uint64_t>(gGuestStack),
                                kGuestStackBytes,
                                reinterpret_cast<uintptr_t>(gGuestStack),
                                boxedvn::GuestMemoryRead |
                                boxedvn::GuestMemoryWrite})) {
            reportf("the ELF64 process stack could not be registered");
            return false;
        }
        return true;
    }
    reportf("bundled ELF64 process absent; using the instruction-stream fallback");
    return mapRawGuestProbe();
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

extern "C" BVNFEXBackendStage BVNFEXBackendProbe(void) {
    std::lock_guard<std::mutex> guard(gProbeMutex);
    if (gStage.load() == BVNFEXBackendStageExecuted) {
        return BVNFEXBackendStageExecuted;
    }
    if (!preparePool()) {
        return gStage.load();
    }
    gStage.store(BVNFEXBackendStageArenaReady, std::memory_order_release);

    FEXCore::Allocator::mmap = fexMmap;
    FEXCore::Allocator::munmap = fexMunmap;
    LogMan::Msg::InstallHandler(fexLog);
    LogMan::Throw::InstallHandler(fexThrow);
    FEXCore::Config::Initialize();
    FEXCore::Config::Set(FEXCore::Config::ConfigOption::CONFIG_IS64BIT_MODE, "1");

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

    static FEXCore::Core::CPUState::gdt_segment gdt[1] {};
    gdt[0].L = 1;
    gdt[0].P = 1;
    gdt[0].S = 1;
    gdt[0].Type = 0b1011;
    thread->CurrentFrame->State.segment_arrays[0] = gdt;
    thread->CurrentFrame->State.cs_idx = 0;

    gCanJump.store(true, std::memory_order_release);
    if (setjmp(gExitJump) == 0) {
        gContext->ExecuteThread(thread);
        gCanJump.store(false, std::memory_order_release);
        reportf("FEX returned without the guest issuing exit");
        return gStage.load();
    }
    gCanJump.store(false, std::memory_order_release);
    gStage.store(BVNFEXBackendStageKernelEntered, std::memory_order_release);
    if (!gKernel.exited() || gKernel.exitCode() != kExpectedExitCode) {
        reportf("BoxedWine kernel exit mismatch: expected 42, observed %d",
                gKernel.exitCode());
        return gStage.load();
    }
    reportf("x86-64 executed through FEX and returned through BoxedWine's "
            "Linux64 syscall adapter (translated pool used %zu KiB)",
            gPoolUsed.load() / 1024);
    gStage.store(BVNFEXBackendStageExecuted, std::memory_order_release);
    return BVNFEXBackendStageExecuted;
}

#endif
