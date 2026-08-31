/*
 * BoxedWine - a bounded record of how the guest loader looks for its modules.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * "could not load kernel32.dll, status c0000135" says only that the search
 * failed. It does not say which directories were searched, whether they opened,
 * or what enumerating them returned -- and the file is present in the archive,
 * so the interesting question is what the guest saw, not what was packaged.
 *
 * The existing BW64_DLLPATH / BW64_DLLTRACE / BW64_DIRTRACE switches answer
 * that but are unbounded: they log every matching path operation for the whole
 * run. A guest that retries one missing path in a loop has already written
 * 468,768 identical lines into a device log once.
 *
 * So the decision of what to report lives here, apart from the syscall layer,
 * and it is bounded twice over: only path operations that could be part of a
 * module search are considered at all, and each process gets a fixed budget of
 * them. When the budget runs out the recorder says so once and goes quiet. The
 * syscalls themselves are untouched -- this bounds the diagnostics, not the
 * behaviour.
 */

#ifndef __DLL_SEARCH_TRACE_H__
#define __DLL_SEARCH_TRACE_H__

// Enough operations to cover a full module search -- the loader probes a
// handful of directories and stats a few names in each -- and few enough that
// the whole trace is a readable block in a device log rather than its bulk.
#define K_DLL_SEARCH_TRACE_BUDGET 128

#if defined(__cplusplus)
#include <atomic>
#include <cstddef>

namespace boxedvn {

// True when a path could be part of a module search. Deliberately narrow: a
// module search touches the Wine module trees, the Windows system directory,
// or a file named like a module. Everything else -- the prefix, the guest's
// own data, /proc -- is not what this is for.
inline bool dllSearchPathIsInteresting(const char* path) {
    if (path == nullptr) {
        return false;
    }
    static const char* const markers[] = {
        "x86_64-windows",  // the builtin PE tree
        "x86_64-unix",     // its Unix half
        "/wine",           // any module root, canonical or compatibility
        "system32",        // the prefix's Windows system directory
        ".dll",
        ".exe",
        ".so",
    };
    for (const char* marker : markers) {
        for (const char* cursor = path; *cursor; ++cursor) {
            const char* left = cursor;
            const char* right = marker;
            while (*right && *left == *right) {
                ++left;
                ++right;
            }
            if (*right == 0) {
                return true;
            }
        }
    }
    return false;
}

// Armed once, for the whole emulator, by a 64-bit Wine launch. A 32-bit
// launch never sets it, so the IA-32 path is untouched -- and because it is
// consulted rather than copied, every process the launch goes on to create,
// including the ones Wine execs and forks for wineboot and its helpers,
// records under it without any of them having to be told.
inline std::atomic<bool> gDllSearchTraceEnabled {false};

inline void setDllSearchTraceEnabled(bool enabled) {
    gDllSearchTraceEnabled.store(enabled, std::memory_order_release);
}

inline bool dllSearchTraceEnabled() {
    return gDllSearchTraceEnabled.load(std::memory_order_acquire);
}

// One process's share of the trace. The budget is per process on purpose: a
// module search that fails does so inside one process, and a shared budget
// would let an earlier process spend it before the failing one starts.
class DllSearchTrace final {
public:
    enum class Decision {
        // Not a module-search path, or the trace is not enabled.
        Silent,
        // Report this operation in full.
        Report,
        // The budget just ran out. Report this once, saying so, then go quiet.
        Exhausted,
    };

    bool isArmed() const {
        return dllSearchTraceEnabled() &&
               budget.load(std::memory_order_relaxed) > 0;
    }

    unsigned remaining() const {
        return budget.load(std::memory_order_relaxed);
    }

    // Records one path operation. `path` may be null, which is never
    // interesting. The budget is spent only on operations that are.
    Decision record(const char* path) {
        if (!dllSearchTraceEnabled() || !dllSearchPathIsInteresting(path)) {
            return Decision::Silent;
        }
        unsigned current = budget.load(std::memory_order_relaxed);
        while (current != 0) {
            if (budget.compare_exchange_weak(current, current - 1,
                                             std::memory_order_relaxed)) {
                return current == 1 ? Decision::Exhausted : Decision::Report;
            }
        }
        return Decision::Silent;
    }

private:
    std::atomic<unsigned> budget {K_DLL_SEARCH_TRACE_BUDGET};
};

} // namespace boxedvn

#endif // __cplusplus

#endif
