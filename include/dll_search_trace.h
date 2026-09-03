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
//
// 128 was sized for one module. A real program's import tree is dozens: a
// device session in which the user's 64-bit program exited
// STATUS_DLL_NOT_FOUND spent the whole budget on the first twelve imports and
// went quiet forty operations before the one that failed, so the log named
// every module that loaded and not the one that did not. At roughly 130 bytes
// a line this is still under 150KB for a process that spends all of it, and
// only a 64-bit launch arms the trace at all.
#define K_DLL_SEARCH_TRACE_BUDGET 1024

// A module name as it appears at the end of a search path. Wine's longest
// builtin is well under this; a name that does not fit is not recorded rather
// than truncated into something that names a different module.
#define K_DLL_SEARCH_MODULE_NAME_MAX 64

// How many distinct module names one process remembers. The loader
// interleaves searches -- an import of an import is looked for in the middle
// of its parent's search -- so a single slot would report a module that was
// found moments later. Eight covers the interleaving every device log shows.
#define K_DLL_SEARCH_MODULE_SLOTS 8

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

// The module name a search path ends in, lowercased, or false when the path
// does not name one. Only ".dll" counts: an import failure is always a DLL,
// and the main image's own .exe is not an import.
inline bool dllSearchModuleName(const char* path, char* out, std::size_t capacity) {
    if (path == nullptr || out == nullptr || capacity == 0) {
        return false;
    }
    std::size_t length = 0;
    while (path[length] != 0) {
        ++length;
    }
    if (length < 4) {
        return false;
    }
    const char* suffix = path + (length - 4);
    const char* dot = ".dll";
    for (int i = 0; i < 4; ++i) {
        char left = suffix[i];
        if (left >= 'A' && left <= 'Z') {
            left = (char)(left - 'A' + 'a');
        }
        if (left != dot[i]) {
            return false;
        }
    }
    std::size_t start = length;
    while (start > 0 && path[start - 1] != '/' && path[start - 1] != '\\') {
        --start;
    }
    const std::size_t nameLength = length - start;
    if (nameLength == 0 || nameLength + 1 > capacity) {
        return false;
    }
    for (std::size_t i = 0; i < nameLength; ++i) {
        char c = path[start + i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        out[i] = c;
    }
    out[nameLength] = 0;
    return true;
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

    // Remembers whether the module a path names has been found yet. Called
    // for every module-search operation, budget or no budget: the budget
    // bounds what is *printed*, and the one thing a spent budget must not
    // cost is the name of the module the search was for.
    //
    // `result` is the operation's return: negative means the path was not
    // there. A module is "unresolved" until some path for that name comes
    // back non-negative, because the loader stops at the first hit -- so the
    // misses that precede it say nothing on their own.
    void noteResult(const char* path, long long result) {
        if (!dllSearchTraceEnabled()) {
            return;
        }
        char name[K_DLL_SEARCH_MODULE_NAME_MAX];
        if (!dllSearchModuleName(path, name, sizeof(name))) {
            return;
        }
        Guard guard(lock);
        Slot* slot = nullptr;
        Slot* oldest = &slots[0];
        for (Slot& candidate : slots) {
            if (candidate.sequence != 0 && sameName(candidate.name, name)) {
                slot = &candidate;
                break;
            }
            if (candidate.sequence < oldest->sequence) {
                oldest = &candidate;
            }
        }
        if (slot == nullptr) {
            slot = oldest;
            slot->resolved = false;
            std::size_t i = 0;
            for (; name[i] != 0 && i + 1 < sizeof(slot->name); ++i) {
                slot->name[i] = name[i];
            }
            slot->name[i] = 0;
        }
        slot->sequence = ++sequenceCounter;
        if (result >= 0) {
            slot->resolved = true;
        }
        ++probeCount;
    }

    // The module searched for most recently that no search path has yet
    // produced. Empty when every module the loader looked for was found --
    // which, at an exit that says otherwise, is itself worth reporting.
    //
    // Copies into the caller's buffer: the slot can be rewritten by another
    // thread, and a diagnostic must not hand out a pointer into it.
    void lastUnresolvedModule(char* out, std::size_t capacity) const {
        if (out == nullptr || capacity == 0) {
            return;
        }
        out[0] = 0;
        Guard guard(lock);
        const Slot* newest = nullptr;
        for (const Slot& candidate : slots) {
            if (candidate.sequence == 0 || candidate.resolved) {
                continue;
            }
            if (newest == nullptr || candidate.sequence > newest->sequence) {
                newest = &candidate;
            }
        }
        if (newest == nullptr) {
            return;
        }
        std::size_t i = 0;
        for (; newest->name[i] != 0 && i + 1 < capacity; ++i) {
            out[i] = newest->name[i];
        }
        out[i] = 0;
    }

    // How many module-search operations this process performed, whether or
    // not they were reported. Says how much of the search the budget covered.
    unsigned probes() const {
        return probeCount.load(std::memory_order_relaxed);
    }

private:
    struct Slot {
        char name[K_DLL_SEARCH_MODULE_NAME_MAX] = {0};
        bool resolved = false;
        // 0 means the slot was never used; otherwise the order it was last
        // touched in, so the oldest slot is the one to reuse.
        unsigned long long sequence = 0;
    };

    // The loader searches under its own lock, so this is uncontended in
    // practice; it exists so that a second thread reading the table during an
    // exit dump sees whole names rather than half-written ones.
    class Guard {
    public:
        explicit Guard(std::atomic<bool>& held) : held(held) {
            bool expected = false;
            while (!held.compare_exchange_weak(expected, true,
                                               std::memory_order_acquire,
                                               std::memory_order_relaxed)) {
                expected = false;
            }
        }
        ~Guard() { held.store(false, std::memory_order_release); }
    private:
        std::atomic<bool>& held;
    };

    static bool sameName(const char* left, const char* right) {
        for (std::size_t i = 0;; ++i) {
            if (left[i] != right[i]) {
                return false;
            }
            if (left[i] == 0) {
                return true;
            }
        }
    }

    std::atomic<unsigned> budget {K_DLL_SEARCH_TRACE_BUDGET};
    std::atomic<unsigned> probeCount {0};
    mutable std::atomic<bool> lock {false};
    Slot slots[K_DLL_SEARCH_MODULE_SLOTS];
    unsigned long long sequenceCounter = 0;
};

} // namespace boxedvn

#endif // __cplusplus

#endif
