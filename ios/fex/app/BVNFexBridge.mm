/*
 *  BoxedVN fex64 - bridge between the application and FEX.
 *  Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
 *
 *  Portions derived from the Mythic project (github.com/willfaust/mythic),
 *  used under the MIT licence its author granted on 2026-08-16. Its
 *  JITAllocator is deliberately *not* carried over: BoxedVN already has a
 *  device-proven arena in BVNExecMemory, and two implementations of the same
 *  delicate thing would drift.
 */

#include "BVNFexBridge.h"
#include "BVNExecMemory.h"
#include "BVNRuntime.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CodeCache.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/DualMap.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/fextl/memory.h>

#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <atomic>
#include <csetjmp>
#include <ctime>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

// Two symbols FEXCore expects the surrounding programme to provide.
//
// __clear_cache is a GCC builtin that Apple's toolchain does not supply for
// iOS. FEX calls it after writing generated code, and on ARM64 getting this
// wrong does not fail loudly - it executes whatever was in the instruction
// cache, which is the worst kind of bug to chase. Darwin spells it
// sys_icache_invalidate.
//
// rpm_cas_snapshot_take is a diagnostic in the reference author's own rpmalloc
// revision - the commit GitHub answers with "not our ref", which is why this
// port pins upstream's instead. So the function this build needs exists
// nowhere reachable. It reports allocator compare-and-swap statistics and its
// caller skips the report when it returns zero, so declining is the correct
// answer rather than a workaround: the statistics genuinely are unavailable.
extern "C" {

void __clear_cache(void* start, void* end) {
    sys_icache_invalidate(start, static_cast<char*>(end) - static_cast<char*>(start));
}

int rpm_cas_snapshot_take(void* snapshot) {
    (void)snapshot;
    return 0;
}

} // extern "C"

namespace {

// 64 MiB, matching the reference port. The arena BVNExecMemory prepares is far
// larger, but the translator should be given a bounded slice of it: when this
// runs out the failure is "the translator exhausted its pool", which is a
// different problem from "the device refused executable memory", and the two
// need opposite responses.
constexpr size_t kJitPoolBytes = 64u * 1024u * 1024u;

// Below this the translator has no room to be interesting, and a pool this
// small means something is wrong with the arena rather than merely tight.
constexpr size_t kMinimumPoolBytes = 4u * 1024u * 1024u;

// iOS pages. The arena is prepared a page at a time and the pool has to align
// with that or the last page is half-owned.
constexpr size_t kPageBytes = 0x4000;

// Two locks, not one. g_probeMutex serialises BVNFexProbe and is held for its
// whole duration, including the steps that can block indefinitely; anything an
// interface polls must therefore never touch it, or observing a hang would
// join it.
std::mutex g_probeMutex;
std::mutex g_reportMutex;
std::atomic<BVNFexStage> g_stage {BVNFexStageIdle};
std::string g_report;

// Read without any lock, deliberately: an interface that had to take
// g_probeMutex to ask what was happening would block on exactly the thing it
// is trying to observe.
std::atomic<const char*> g_step {""};
std::atomic<double> g_stepStarted {0.0};

double nowSeconds() {
    return (double)clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1e9;
}

// Announces a step before entering it, so a step that never returns is still
// named. String literals only: the pointer outlives any reader.
struct Step {
    explicit Step(const char* description) {
        g_stepStarted.store(nowSeconds(), std::memory_order_relaxed);
        g_step.store(description, std::memory_order_release);
    }
    ~Step() {
        g_step.store("", std::memory_order_release);
        g_stepStarted.store(0.0, std::memory_order_relaxed);
    }
};

void* g_poolExecutable = nullptr;
void* g_poolWritable = nullptr;
std::atomic<size_t> g_poolBytes {0};
std::atomic<size_t> g_poolUsed {0};

void reportf(const char* format, ...) __attribute__((format(printf, 1, 2)));
void reportf(const char* format, ...) {
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);

    {
        // Under the report lock, not the probe lock. The interface reads this
        // while the probe is mid-step, so an unlocked append races a reader
        // that is polling precisely because the probe may never return.
        std::lock_guard<std::mutex> guard(g_reportMutex);
        g_report.append(line);
        g_report.append("\n");
    }
    BVNLogWrite(BVNLogLevelInfo, "fex", line);
}

size_t alignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// FEX asks for memory through these rather than calling mmap itself, which is
// the whole reason this port is tractable: every allocation the translator
// makes for generated code can be answered from the one arena the debugger
// prepared, without patching FEX's internals.
//
// A bump allocator is enough here. FEX frees its code buffers only at
// shutdown, and reuse would mean recycling pages that may still be branched
// to - which on a platform that cannot make new executable pages later is a
// far worse failure than running out.
void* poolAllocate(size_t length) {
    const size_t aligned = alignUp(length, kPageBytes);
    size_t offset = g_poolUsed.load(std::memory_order_relaxed);
    while (true) {
        if (offset + aligned > g_poolBytes.load(std::memory_order_relaxed)) {
            return nullptr;
        }
        if (g_poolUsed.compare_exchange_weak(offset, offset + aligned,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
            return static_cast<uint8_t*>(g_poolExecutable) + offset;
        }
    }
}

bool poolOwns(const void* address) {
    const auto* base = static_cast<const uint8_t*>(g_poolExecutable);
    const auto* candidate = static_cast<const uint8_t*>(address);
    return base != nullptr && candidate >= base &&
           candidate < base + g_poolBytes.load(std::memory_order_relaxed);
}

void* mmapHook(void* address, size_t length, int protection, int flags, int fd,
               off_t offset) {
    if (protection & PROT_EXEC) {
        void* result = poolAllocate(length);
        if (result == nullptr) {
            reportf("pool exhausted: asked for %zu bytes of %zu", length,
                    g_poolBytes.load(std::memory_order_relaxed));
            return MAP_FAILED;
        }
        return result;
    }
    return mmap(address, length, protection, flags, fd, offset);
}

int munmapHook(void* address, size_t length) {
    // Deliberately a no-op for pool addresses; see poolAllocate.
    if (poolOwns(address)) {
        return 0;
    }
    return munmap(address, length);
}

// FEX's own diagnostics. Without these it fails silently: everything it would
// have said about a bad configuration goes nowhere, which is most of why the
// first stall here was unreadable.
void fexMessage(LogMan::DebugLevels level, const char* message) {
    reportf("FEX[%u]: %s", (unsigned)level, message);
}

void fexThrow(const char* message) {
    reportf("FEX threw: %s", message);
}

// FEX cannot probe the host on iOS the way it does on Linux, so these are
// stated. Everything here is true of every Apple core this can run on - A12
// and later are ARMv8.3 or better, and the deployment target already excludes
// anything older - except the vector extensions, which Apple does not
// implement and which must be off or the translator emits SVE it cannot run.
FEXCore::HostFeatures hostFeatures() {
    FEXCore::HostFeatures features {};

    features.DCacheLineSize = 64;
    features.ICacheLineSize = 64;
    features.SupportsCacheMaintenanceOps = true;

    features.SupportsAES = true;
    features.SupportsCRC = true;
    features.SupportsSHA = true;
    features.SupportsPMULL_128Bit = true;

    features.SupportsAtomics = true;   // ARMv8.1 LSE
    features.SupportsRCPC = true;      // ARMv8.3
    features.SupportsTSOImm9 = true;   // ARMv8.4 RCPC2
    features.SupportsFCMA = true;
    features.SupportsFlagM = true;
    features.SupportsFlagM2 = true;

    features.SupportsAVX = false;
    features.SupportsSVE128 = false;
    features.SupportsSVE256 = false;

    // An empty MIDR list means "no cores" to code that iterates it. Take the
    // real count rather than assuming a core layout that changes every year.
    int cores = 0;
    size_t length = sizeof(cores);
    if (sysctlbyname("hw.ncpu", &cores, &length, nullptr, 0) != 0 || cores <= 0) {
        cores = 4;
    }
    features.CPUMIDRs.resize((size_t)cores, 0x611F0000);

    return features;
}

// The smallest x86-64 programme that can prove anything: set the exit code,
// select sys_exit, and leave through a syscall. It has to leave through a
// syscall rather than simply returning, because a return has nowhere to go -
// there is no caller in the guest, and no loaded image to return into.
//
//   48 C7 C7 2A 00 00 00   mov rdi, 42       ; the value we look for
//   48 C7 C0 3C 00 00 00   mov rax, 60       ; sys_exit
//   0F 05                  syscall
constexpr uint8_t kGuestProgram[] = {
    0x48, 0xC7, 0xC7, 0x2A, 0x00, 0x00, 0x00,
    0x48, 0xC7, 0xC0, 0x3C, 0x00, 0x00, 0x00,
    0x0F, 0x05,
};
constexpr uint64_t kGuestExpectedExit = 42;

constexpr size_t kGuestCodeBytes = 0x4000;
constexpr size_t kGuestStackBytes = 0x40000;
constexpr size_t kCallRetGuardBytes = kPageBytes;

void* g_guestCode = nullptr;
void* g_guestStack = nullptr;
void* g_callRetAllocation = nullptr;

std::jmp_buf g_guestExit;
std::atomic<uint64_t> g_guestExitCode {0};
std::atomic<bool> g_guestExited {false};

// Linux is the ABI the guest programme is written against, and sys_exit is the
// only call it makes. Everything else is refused loudly rather than silently
// returning success, because a syscall this does not implement is a fact worth
// having and not an error to paper over.
class GuestSyscalls final : public FEXCore::HLE::SyscallHandler {
public:
    GuestSyscalls() {
        OSABI = FEXCore::HLE::SyscallOSABI::OS_LINUX64;
    }

    uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame* Frame,
                           FEXCore::HLE::SyscallArguments* Args) override {
        const uint64_t number = Args->Argument[0];
        if (number == 60 || number == 231) { // exit, exit_group
            g_guestExitCode.store(Args->Argument[1], std::memory_order_release);
            g_guestExited.store(true, std::memory_order_release);
            // The only way out. Returning would resume the guest at the
            // instruction after the syscall, and there is nothing there.
            std::longjmp(g_guestExit, 1);
        }
        reportf("guest made syscall %llu, which this probe does not implement",
                (unsigned long long)number);
        return (uint64_t)-1;
    }

    // The guest is one page of code this probe wrote itself, so the honest
    // answer to "what is executable around this address" is that page. Saying
    // "everything" would let the translator cache across memory it has no
    // reason to trust; saying "nothing" would stop it translating at all.
    FEXCore::HLE::ExecutableRangeInfo
    QueryGuestExecutableRange(FEXCore::Core::InternalThreadState* Thread,
                              uint64_t Address) override {
        return {(uint64_t)(uintptr_t)g_guestCode, kGuestCodeBytes, false};
    }

    // No files are mapped: the guest was written into anonymous memory rather
    // than loaded from an image, so there is no section to name.
    std::optional<FEXCore::ExecutableFileSectionInfo>
    LookupExecutableFileSection(FEXCore::Core::InternalThreadState* Thread,
                                uint64_t GuestAddr) override {
        return std::nullopt;
    }
};

class GuestSignals final : public FEXCore::SignalDelegator {
};

GuestSyscalls g_syscalls;
GuestSignals g_signals;

// Held for the life of the process. The thread FEX creates refers to it, and
// tearing it down while generated code may still be reachable from the arena
// would be worse than leaking it.
fextl::unique_ptr<FEXCore::Context::Context> g_context;

// Whether a 32-bit Windows guest could ever run here.
//
// Wine's WoW64 runs an i386 PE side whose pointers are 32 bits, so every
// image, heap and stack it owns has to live below 4 GiB. FEX already ships
// the other half of that route -- libwow64fex.dll, the same BTCpu interface
// the ARM64EC emulator implements, built for an aarch64 host and an x86
// guest -- so the build work is real and bounded. What is not known is
// whether this process can address the low 4 GiB at all: a 64-bit Mach-O
// gets a __PAGEZERO of exactly that size unless it is linked with
// -pagezero_size, and nothing in this app has ever asked for an address
// under it.
//
// Report the answer rather than guessing at it. Reading the region at 0
// gives __PAGEZERO's real extent, and one fixed allocation attempt above it
// says whether the space beyond is usable or merely unclaimed. Both are
// read-only in effect: vm_allocate with VM_FLAGS_FIXED fails rather than
// relocating the request, and anything it does get is handed straight back.
void reportLowAddressSpace() {
    vm_address_t address = 0;
    vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;

    const kern_return_t region = vm_region_64(
        mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (region != KERN_SUCCESS) {
        reportf("low address space: the region at 0 could not be read (%d)",
                region);
        return;
    }

    // A 4 GiB reserved region starting at 0 is __PAGEZERO at its default
    // size, which is the whole of a 32-bit guest's address space.
    const bool pagezeroCoversFourGiB =
        address == 0 && size >= 0x100000000ull;

    // Try for a page in the middle of the low range. 256 MiB is above
    // anything a shrunk __PAGEZERO would keep and below the 4 GiB line.
    vm_address_t candidate = 0x10000000ull;
    const vm_size_t page = static_cast<vm_size_t>(getpagesize());
    const kern_return_t claimed =
        vm_allocate(mach_task_self(), &candidate, page, VM_FLAGS_FIXED);
    if (claimed == KERN_SUCCESS) {
        vm_deallocate(mach_task_self(), candidate, page);
    }

    reportf("low address space: first region 0x%llx+0x%llx prot=%d/%d, "
            "fixed page at 0x10000000 %s -- 32-bit guests are %s",
            (unsigned long long)address, (unsigned long long)size,
            info.protection, info.max_protection,
            claimed == KERN_SUCCESS ? "granted" : "refused",
            claimed == KERN_SUCCESS
                ? "addressable; the WoW64 route is open"
                : (pagezeroCoversFourGiB
                       ? "unreachable: __PAGEZERO is the full 4 GiB, so the "
                         "app has to be linked with -pagezero_size before "
                         "WoW64 is worth building"
                       : "unreachable for a reason other than __PAGEZERO"));
}

bool prepareArena() {
    if (g_poolExecutable != nullptr) {
        return true;
    }

    // Costs one region read and one page; the answer decides whether any of
    // the 32-bit work is worth starting, and it can only be measured here.
    static std::once_flag lowAddressOnce;
    std::call_once(lowAddressOnce, reportLowAddressSpace);

    if (!BVNExecMemArenaPrepared()) {
        // The header is explicit that this can be stopped indefinitely when a
        // debugger is attached without a working script: it asks StikDebug to
        // prepare every page and waits. There is no way to cancel it from
        // outside, so the most that can be done is name it while it runs.
        Step step("asking StikDebug to prepare the executable arena");
        if (!BVNExecMemPrepareArena()) {
            reportf("the executable arena was refused. Is this running through "
                    "StikDebug with universal.js assigned?");
            return false;
        }
    }
    reportf("arena prepared");

    // Preparing the arena is not enough to allocate from it. BVNExecMemAlloc
    // refuses until the arena has been *confirmed* - a small function written
    // through the writable alias, called through the executable address, and
    // seen to return the value it should. Until that has happened the arena is
    // memory the debugger says is executable, which is not the same as memory
    // this device will actually execute.
    //
    // That confirmation is the one step that can take the process down with no
    // recovery, which is why it is here, behind a deliberate action, and not
    // at launch.
    if (!BVNExecMemExecutionConfirmed()) {
        Step step("executing the arena's own confirmation code");
        const BVNExecMemStrategy strategy = BVNExecMemProbe(true);
        reportf("arena strategy: %s", BVNExecMemStrategyName(strategy));
        if (!BVNExecMemExecutionConfirmed()) {
            reportf("the arena did not execute its own probe, so nothing can "
                    "be allocated from it:\n%s", BVNExecMemReport());
            return false;
        }
        reportf("arena confirmed: code written through the alias executed from "
                "the r-x address");
    }

    // Segments are a fixed size and an allocation has to fit inside one, so
    // asking for exactly a segment's worth is the request most likely to be
    // refused by a byte of bookkeeping. Take the largest pool the arena will
    // actually give rather than failing on a round number.
    void* executable = nullptr;
    size_t poolBytes = 0;
    for (size_t candidate = kJitPoolBytes; candidate >= kMinimumPoolBytes;
         candidate /= 2) {
        executable = BVNExecMemAlloc(candidate);
        if (executable != nullptr) {
            poolBytes = candidate;
            break;
        }
        reportf("arena declined %zu MiB; trying half", candidate / (1024 * 1024));
    }

    if (executable == nullptr) {
        size_t capacity = 0;
        size_t available = 0;
        size_t segments = 0;
        BVNExecMemArenaStatus(&capacity, &available, &segments);
        reportf("the arena is confirmed but would give nothing down to %zu MiB "
                "(%zu MiB free of %zu MiB across %zu segments)",
                kMinimumPoolBytes / (1024 * 1024), available / (1024 * 1024),
                capacity / (1024 * 1024), segments);
        return false;
    }

    const size_t kJitPoolBytesActual = poolBytes;

    void* writable = BVNExecMemWritableAddress(executable, kJitPoolBytesActual);
    if (writable == nullptr) {
        reportf("no writable alias for the pool; generated code could not be "
                "written");
        BVNExecMemFree(executable, kJitPoolBytesActual);
        return false;
    }

    g_poolExecutable = executable;
    g_poolWritable = writable;
    g_poolBytes.store(kJitPoolBytesActual, std::memory_order_release);
    g_poolUsed.store(0, std::memory_order_relaxed);

    // Hand FEX the distance between the two views. This is the whole contract
    // of a dual-mapped pool: FEX treats the r-x address as canonical and adds
    // this offset whenever it writes, so with the default of zero every write
    // of generated code goes to the executable mapping - which is read-only,
    // and on this device stops the thread rather than returning an error.
    //
    // It has to be set before FEXCore initialises, because the dispatcher is
    // emitted during context creation and captures the value then.
    FEXCore::DualMap::WriteOffset =
        static_cast<int64_t>(static_cast<uint8_t*>(writable) -
                             static_cast<uint8_t*>(executable));

    // The alias is placed wherever the kernel puts it, so this is reported
    // rather than assumed anywhere.
    reportf("arena pool %zu MiB at rx=%p rw=%p (rw-rx=%+lld)",
            kJitPoolBytesActual / (1024 * 1024), executable, writable,
            (long long)((intptr_t)writable - (intptr_t)executable));
    return true;
}

} // namespace

extern "C" BVNFexStage BVNFexStageReached(void) {
    return g_stage.load(std::memory_order_acquire);
}

extern "C" const char* BVNFexReport(void) {
    // Copied under the report lock into storage the caller can read after it
    // is released; returning g_report.c_str() would hand out a pointer that
    // the next reportf could reallocate underneath a reader.
    static std::string snapshot;
    std::lock_guard<std::mutex> guard(g_reportMutex);
    snapshot = g_report;
    return snapshot.c_str();
}

extern "C" bool BVNFexPoolStatus(size_t* poolBytes, size_t* usedBytes) {
    const size_t bytes = g_poolBytes.load(std::memory_order_acquire);
    if (bytes == 0) {
        return false;
    }
    if (poolBytes != nullptr) {
        *poolBytes = bytes;
    }
    if (usedBytes != nullptr) {
        *usedBytes = g_poolUsed.load(std::memory_order_relaxed);
    }
    return true;
}

extern "C" int64_t BVNFexWriteOffset(void) {
    if (g_poolExecutable == nullptr || g_poolWritable == nullptr) {
        return 0;
    }
    return static_cast<int64_t>(static_cast<uint8_t*>(g_poolWritable) -
                                static_cast<uint8_t*>(g_poolExecutable));
}

extern "C" const char* BVNFexCurrentStep(void) {
    return g_step.load(std::memory_order_acquire);
}

extern "C" double BVNFexCurrentStepSeconds(void) {
    const double started = g_stepStarted.load(std::memory_order_relaxed);
    return started == 0.0 ? 0.0 : nowSeconds() - started;
}

extern "C" const char* BVNFexStageName(BVNFexStage stage) {
    switch (stage) {
        case BVNFexStageIdle: return "nothing run yet";
        case BVNFexStageArenaReady: return "arena ready";
        case BVNFexStageHooksInstalled: return "allocator hooks installed";
        case BVNFexStageContextCreated: return "FEXCore context created";
        case BVNFexStageExecuted: return "translated code executed";
    }
    return "unknown";
}

extern "C" BVNFexStage BVNFexProbe(void) {
    std::lock_guard<std::mutex> guard(g_probeMutex);

    if (g_stage.load() < BVNFexStageArenaReady) {
        if (!prepareArena()) {
            return g_stage.load();
        }
        g_stage.store(BVNFexStageArenaReady, std::memory_order_release);
    }

    if (g_stage.load() < BVNFexStageHooksInstalled) {
        FEXCore::Allocator::mmap = mmapHook;
        FEXCore::Allocator::munmap = munmapHook;
        reportf("allocator hooks point at the arena pool");
        g_stage.store(BVNFexStageHooksInstalled, std::memory_order_release);
    }

    if (g_stage.load() < BVNFexStageContextCreated) {
        {
            // Install these before anything else touches FEX. Without them
            // everything FEX would say about a bad configuration goes nowhere,
            // which is most of why the first stall here was unreadable.
            Step step("installing FEX log handlers");
            LogMan::Msg::InstallHandler(fexMessage);
            LogMan::Throw::InstallHandler(fexThrow);
        }

        {
            // Creating a context without this stops dead rather than failing:
            // the configuration subsystem has to exist before anything reads a
            // value out of it, and the guest is x86-64, which is not the
            // default.
            Step step("initialising FEX configuration");
            FEXCore::Config::Initialize();
            FEXCore::Config::Set(FEXCore::Config::ConfigOption::CONFIG_IS64BIT_MODE, "1");
            reportf("FEX configuration initialised, 64-bit guest");
        }

        Step step("creating a FEXCore context");
        FEXCore::HostFeatures features = hostFeatures();
        reportf("host features stated for %zu cores", features.CPUMIDRs.size());
        g_context = FEXCore::Context::Context::CreateNewContext(features);
        if (!g_context) {
            reportf("FEXCore refused to create a context");
            return g_stage.load();
        }
        reportf("FEXCore context created");
        g_stage.store(BVNFexStageContextCreated, std::memory_order_release);

        size_t used = g_poolUsed.load(std::memory_order_relaxed);
        reportf("pool used after context creation: %zu KiB", used / 1024);
    }

    if (g_stage.load() < BVNFexStageExecuted) {
        {
            Step step("initialising the translator core");
            g_context->SetSignalDelegator(&g_signals);
            g_context->SetSyscallHandler(&g_syscalls);
            // Apple's cores implement total store ordering in hardware, which
            // is the single largest reason x86 emulation is viable here: the
            // alternative is a barrier around every guest memory access.
            g_context->SetHardwareTSOSupport(true);
            if (!g_context->InitCore()) {
                reportf("InitCore refused; the dispatcher was not emitted");
                return g_stage.load();
            }
            size_t used = g_poolUsed.load(std::memory_order_relaxed);
            reportf("core initialised; pool used %zu KiB", used / 1024);
        }

        {
            // Guest memory is ordinary mmap. Only the translator's *output*
            // has to live in the arena; the x86-64 the guest executes is data
            // as far as this device is concerned, because FEX reads it rather
            // than jumping to it.
            Step step("mapping guest memory");
            g_guestCode = mmap(nullptr, kGuestCodeBytes, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANON, -1, 0);
            g_guestStack = mmap(nullptr, kGuestStackBytes, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANON, -1, 0);
            if (g_guestCode == MAP_FAILED || g_guestStack == MAP_FAILED) {
                reportf("could not map guest code or stack");
                return g_stage.load();
            }
            memcpy(g_guestCode, kGuestProgram, sizeof(kGuestProgram));
            reportf("guest code at %p, stack at %p", g_guestCode, g_guestStack);
        }

        Step step("translating and executing x86-64");
        const uint64_t entry = (uint64_t)(uintptr_t)g_guestCode;
        const uint64_t stackTop =
            (uint64_t)(uintptr_t)g_guestStack + kGuestStackBytes - 0x100;

        auto* thread = g_context->CreateThread(entry, stackTop);
        if (thread == nullptr) {
            reportf("CreateThread returned nothing");
            return g_stage.load();
        }
        reportf("FEX thread created");

        // FEX's Linux thread manager normally supplies these two pieces of
        // state. This probe talks to FEXCore directly, so it has to do that
        // manager's work itself before entering the dispatcher.
        //
        // The shadow stack is used by translated call/ret instructions. Guard
        // pages on both ends turn an overflow into a named fault rather than
        // allowing it to corrupt unrelated application memory.
        constexpr size_t callRetBytes =
            FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
        constexpr size_t callRetAllocationBytes =
            callRetBytes + 2 * kCallRetGuardBytes;
        g_callRetAllocation = mmap(nullptr, callRetAllocationBytes, PROT_NONE,
                                   MAP_PRIVATE | MAP_ANON, -1, 0);
        if (g_callRetAllocation == MAP_FAILED) {
            g_callRetAllocation = nullptr;
            reportf("could not map the FEX call/return shadow stack");
            return g_stage.load();
        }

        void* callRetBase =
            static_cast<uint8_t*>(g_callRetAllocation) + kCallRetGuardBytes;
        if (mprotect(callRetBase, callRetBytes, PROT_READ | PROT_WRITE) != 0) {
            reportf("could not make the FEX call/return shadow stack writable");
            return g_stage.load();
        }
        thread->CallRetStackBase = callRetBase;
        thread->CurrentFrame->State.callret_sp =
            (uint64_t)(uintptr_t)callRetBase + callRetBytes / 4;

        // The decoder reads CS through segment_arrays to select long mode.
        // CreateThread does not install a GDT when FEXCore is used without its
        // Linux thread manager; leaving this null makes the first decode read
        // through a null segment table instead of decoding x86-64.
        static FEXCore::Core::CPUState::gdt_segment gdt[1] {};
        gdt[0].L = 1;
        gdt[0].D = 0;
        gdt[0].P = 1;
        gdt[0].S = 1;
        gdt[0].Type = 0b1011; // executable, readable and accessed
        thread->CurrentFrame->State.segment_arrays[0] = gdt;
        thread->CurrentFrame->State.cs_idx = 0;
        reportf("guest thread state ready: call/return stack at %p, 64-bit GDT installed",
                callRetBase);

        // The guest leaves through sys_exit, and the handler longjmps here.
        // There is no other way back: returning from the handler would resume
        // the guest after the syscall, where there is nothing.
        if (setjmp(g_guestExit) == 0) {
            reportf("entering FEX ExecuteThread");
            g_context->ExecuteThread(thread);
            reportf("ExecuteThread returned without the guest exiting");
            return g_stage.load();
        }

        const uint64_t code = g_guestExitCode.load(std::memory_order_acquire);
        const size_t used = g_poolUsed.load(std::memory_order_relaxed);
        reportf("guest exited with %llu after translating into %zu KiB",
                (unsigned long long)code, used / 1024);

        if (code != kGuestExpectedExit) {
            reportf("expected %llu; the translation ran but produced the wrong "
                    "value, which is a correctness fault rather than a setup one",
                    (unsigned long long)kGuestExpectedExit);
            return g_stage.load();
        }

        g_stage.store(BVNFexStageExecuted, std::memory_order_release);
        reportf("x86-64 translated by FEX executed from the arena and returned "
                "what it should");
    }

    return g_stage.load();
}
