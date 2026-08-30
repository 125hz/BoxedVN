/*
 * BoxedWine - the Wine NT-stub redirect against real guest memory.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * wine_nt_syscall_stub.h holds the recognition rules and knows nothing about
 * BoxedWine, so they can be tested against a synthetic address space. This
 * binds them to KMemory64 and gives the interpreter (source/emulation/cpu/
 * cpu64.cpp) and the FEX adapter (ios/runtime/src/BVNFEXCPU64Adapter.mm) one
 * shared entry point, so the two CPU backends cannot drift into disagreeing
 * about what a Wine NT stub is.
 *
 * Both callers do the same three things on a match and then resume in the way
 * their own backend requires:
 *
 *   - set the SystemCall flag, so every later stub takes its indirect path on
 *     its own and never reaches this code again;
 *   - restore RCX from R10, because the indirect path is a call rather than a
 *     SYSCALL and Wine's dispatcher still expects the NT call's first
 *     argument there;
 *   - leave the NT ordinal in RAX and resume at the validated indirect path.
 */

#ifndef __WINE_NT_SYSCALL_MEMORY_H__
#define __WINE_NT_SYSCALL_MEMORY_H__

#include "wine_nt_syscall_stub.h"

#include "kmemory64.h"
#include "log.h"

#include <atomic>

namespace boxedvn {

// Reads canonical guest memory, refusing any range that touches an unmapped
// page. KMemory64::readb reports an absent page as zero, which would otherwise
// let a partly-unmapped region be inspected as though it were readable.
//
// The matcher runs ahead of every SYSCALL, including the hot Linux ones, so
// this checks whole pages rather than individual bytes: the stub spans at most
// two pages and isPageMapped can take the memory lock. The byte comparisons
// that follow are register work and reject an ordinary glibc syscall on the
// very first one.
inline bool readGuestBytesForNtStub(void* context, uint64_t address,
                                    uint8_t* out, unsigned length) {
    KMemory64* memory = static_cast<KMemory64*>(context);
    if (memory == nullptr || length == 0) {
        return false;
    }
    const uint64_t firstPage = address >> K64_PAGE_SHIFT;
    const uint64_t lastPage = (address + length - 1) >> K64_PAGE_SHIFT;
    for (uint64_t page = firstPage; page <= lastPage; ++page) {
        if (!memory->isPageMapped(page)) {
            return false;
        }
    }
    memory->memcpyFromGuest(out, address, length);
    return true;
}

inline bool matchWineNtSyscallStubInGuest(KMemory64* memory,
                                          uint64_t syscallAddress, uint64_t rax,
                                          WineNtSyscallStub& out) {
    if (memory == nullptr) {
        return false;
    }
    return matchWineNtSyscallStub(&readGuestBytesForNtStub, memory,
                                  syscallAddress, rax, out);
}

// Set KUSER_SHARED_DATA's SystemCall flag. Wine's own thunks then branch to
// their indirect path without this handler being involved at all, so the
// redirect below is paid once rather than per NT call.
inline void setWineNtSystemCallFlag(KMemory64* memory) {
    if (memory == nullptr) {
        return;
    }
    const uint8_t flag = memory->readb(K_WINE_KUSER_SYSTEM_CALL_FLAG);
    memory->writeb(K_WINE_KUSER_SYSTEM_CALL_FLAG, (uint8_t)(flag | 1));
}

// One line for the first few redirects. Bounded on purpose: the SystemCall
// flag means a healthy guest produces roughly one of these per process, and a
// guest that produces many is exactly the case that must not fill a log.
inline void reportWineNtSyscallRedirect(const WineNtSyscallStub& stub,
                                        uint32_t processId, uint32_t threadId,
                                        const char* path) {
    static std::atomic<unsigned> reported {0};
    if (reported.fetch_add(1, std::memory_order_relaxed) >= 8) {
        return;
    }
    klog_fmt("BOXEDWINE_X64_NT_REDIRECT pid=%u tid=%u nt=%u stub=0x%llx "
             "dispatcher=0x%llx resume=0x%llx path=%s",
             processId, threadId, stub.ntOrdinal,
             (unsigned long long)stub.syscallAddress,
             (unsigned long long)stub.dispatcher,
             (unsigned long long)stub.indirectPath, path);
}

} // namespace boxedvn

#endif
