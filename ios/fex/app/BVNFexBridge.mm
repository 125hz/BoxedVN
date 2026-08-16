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

#include <libkern/OSCacheControl.h>
#include <sys/mman.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

// 64 MiB, matching the reference port. The arena BVNExecMemory prepares is far
// larger, but the translator should be given a bounded slice of it: when this
// runs out the failure is "the translator exhausted its pool", which is a
// different problem from "the device refused executable memory", and the two
// need opposite responses.
constexpr size_t kJitPoolBytes = 64u * 1024u * 1024u;

// iOS pages. The arena is prepared a page at a time and the pool has to align
// with that or the last page is half-owned.
constexpr size_t kPageBytes = 0x4000;

std::mutex g_mutex;
BVNFexStage g_stage = BVNFexStageIdle;
std::string g_report;

void* g_poolExecutable = nullptr;
void* g_poolWritable = nullptr;
size_t g_poolBytes = 0;
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
        if (offset + aligned > g_poolBytes) {
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
    return base != nullptr && candidate >= base && candidate < base + g_poolBytes;
}

void* mmapHook(void* address, size_t length, int protection, int flags, int fd,
               off_t offset) {
    if (protection & PROT_EXEC) {
        void* result = poolAllocate(length);
        if (result == nullptr) {
            reportf("pool exhausted: asked for %zu bytes of %zu", length,
                    g_poolBytes);
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

    if (!BVNExecMemArenaPrepared() && !BVNExecMemPrepareArena()) {
        reportf("the executable arena was refused. Is BoxedVN running through "
                "StikDebug with universal.js assigned?");
        return false;
    }

    void* executable = BVNExecMemAlloc(kJitPoolBytes);
    if (executable == nullptr) {
        size_t capacity = 0;
        size_t available = 0;
        BVNExecMemArenaStatus(&capacity, &available, nullptr);
        reportf("the arena is prepared but would not give %zu MiB "
                "(%zu MiB of %zu MiB free)",
                kJitPoolBytes / (1024 * 1024), available / (1024 * 1024),
                capacity / (1024 * 1024));
        return false;
    }

    void* writable = BVNExecMemWritableAddress(executable, kJitPoolBytes);
    if (writable == nullptr) {
        reportf("no writable alias for the pool; generated code could not be "
                "written");
        BVNExecMemFree(executable, kJitPoolBytes);
        return false;
    }

    g_poolExecutable = executable;
    g_poolWritable = writable;
    g_poolBytes = kJitPoolBytes;
    g_poolUsed.store(0, std::memory_order_relaxed);

    // The distance between the two views is what generated code needs in order
    // to write through the alias. It is not a fixed offset - the alias is
    // placed wherever the kernel puts it - so it is reported rather than
    // assumed anywhere.
    reportf("arena pool %zu MiB at rx=%p rw=%p (rw-rx=%+lld)",
            kJitPoolBytes / (1024 * 1024), executable, writable,
            (long long)((intptr_t)writable - (intptr_t)executable));
    return true;
}

} // namespace

extern "C" BVNFexStage BVNFexStageReached(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_stage;
}

extern "C" const char* BVNFexReport(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_report.c_str();
}

extern "C" bool BVNFexPoolStatus(size_t* poolBytes, size_t* usedBytes) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_poolExecutable == nullptr) {
        return false;
    }
    if (poolBytes != nullptr) {
        *poolBytes = g_poolBytes;
    }
    if (usedBytes != nullptr) {
        *usedBytes = g_poolUsed.load(std::memory_order_relaxed);
    }
    return true;
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
    std::lock_guard<std::mutex> guard(g_mutex);

    if (g_stage < BVNFexStageArenaReady) {
        if (!prepareArena()) {
            return g_stage;
        }
        g_stage = BVNFexStageArenaReady;
    }

    if (g_stage < BVNFexStageHooksInstalled) {
        FEXCore::Allocator::mmap = mmapHook;
        FEXCore::Allocator::munmap = munmapHook;
        reportf("allocator hooks point at the arena pool");
        g_stage = BVNFexStageHooksInstalled;
    }

    if (g_stage < BVNFexStageContextCreated) {
        // Everything above this line is BoxedVN's own code and is understood.
        // This is the first call into FEX, and it is also what forces the
        // translator's archives to be linked at all - a probe that only
        // touched our own arena would prove nothing about them.
        FEXCore::HostFeatures features {};
        auto context = FEXCore::Context::Context::CreateNewContext(features);
        if (!context) {
            reportf("FEXCore refused to create a context");
            return g_stage;
        }
        reportf("FEXCore context created");
        g_stage = BVNFexStageContextCreated;

        size_t used = g_poolUsed.load(std::memory_order_relaxed);
        reportf("pool used after context creation: %zu KiB", used / 1024);
    }

    // BVNFexStageExecuted is not reachable yet: running x86-64 needs a loaded
    // image and a syscall handler, which is the next piece of work. Stopping
    // here and saying so is better than a stage that reports success for
    // something it did not do.
    reportf("stopping at '%s'; executing translated code is not wired up yet",
            BVNFexStageName(g_stage));
    return g_stage;
}
