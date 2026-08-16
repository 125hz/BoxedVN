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

#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/DualMap.h>

#include <libkern/OSCacheControl.h>
#include <sys/mman.h>

#include <atomic>
#include <ctime>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
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

    g_report.append(line);
    g_report.append("\n");
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

bool prepareArena() {
    if (g_poolExecutable != nullptr) {
        return true;
    }

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
        // Everything above this line is BoxedVN's own code and is understood.
        // This is the first call into FEX, and it is also what forces the
        // translator's archives to be linked at all - a probe that only
        // touched our own arena would prove nothing about them.
        Step step("creating a FEXCore context");
        FEXCore::HostFeatures features {};
        auto context = FEXCore::Context::Context::CreateNewContext(features);
        if (!context) {
            reportf("FEXCore refused to create a context");
            return g_stage.load();
        }
        reportf("FEXCore context created");
        g_stage.store(BVNFexStageContextCreated, std::memory_order_release);

        size_t used = g_poolUsed.load(std::memory_order_relaxed);
        reportf("pool used after context creation: %zu KiB", used / 1024);
    }

    // BVNFexStageExecuted is not reachable yet: running x86-64 needs a loaded
    // image and a syscall handler, which is the next piece of work. Stopping
    // here and saying so is better than a stage that reports success for
    // something it did not do.
    reportf("stopping at '%s'; executing translated code is not wired up yet",
            BVNFexStageName(g_stage.load()));
    return g_stage.load();
}
