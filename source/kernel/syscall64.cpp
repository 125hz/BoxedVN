/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"

#ifdef BOXEDWINE_GUEST_X64

#include "syscall64.h"
#include "cpu64.h"
#include "kmemory64.h"
#include "knativesystem.h"
#include "guest_mmap_diagnostics.h"
#include "guest_mmap_placement.h"
#include "wine_server_reply.h"
#include "dll_search_trace.h"
#ifdef BOXEDWINE_FEX64_BACKEND
#include "BVNFEXBackend.h"
#endif
#include "krandom.h"
#include "boxedwine_x64_hostcall.h"
#include "boxedwine_x64_x11_bridge.h"
#include "../x11/x11bridge64.h"
#include "kevent.h"
#include "ksocket.h"
#include <cstring>
#include <thread>   // std::this_thread::yield() for sched_yield
#include <mutex>    // std::recursive_mutex for BW64_SERIAL_TEARDOWN
#include <atomic>   // bounded guest mmap placement counters
#include "kunixsocket.h"
#include "kpoll.h"
#ifdef BOXEDWINE_OPENGL
#include "../opengl/gl64bridge.h"
#include "../opengl/gl64bridge_abi.h"
#endif
#if defined(BOXEDWINE_DXMT_NATIVE)
#include "BVNFrameRate.h"
#include "BVNDXMTDisplay.h"
extern "C" const void *dxmt_winemetal_unix_call_funcs[];
#endif

// x86-64 Linux syscall numbers used here. The canonical table lives in
// arch/x86/entry/syscalls/syscall_64.tbl in the Linux source; the values
// below are stable Linux ABI and never change.
#define X64_SYS_read              0
#define X64_SYS_write             1
#define X64_SYS_open              2
#define X64_SYS_close             3
#define X64_SYS_stat              4
#define X64_SYS_fstat             5
#define X64_SYS_lstat             6
#define X64_SYS_poll              7
#define X64_SYS_ppoll             271
#define X64_SYS_pselect6          270
#define X64_SYS_lseek             8
#define X64_SYS_mmap              9
#define X64_SYS_mprotect          10
#define X64_SYS_munmap            11
#define X64_SYS_brk               12
#define X64_SYS_shmget            29
#define X64_SYS_shmat             30
#define X64_SYS_shmctl            31
#define X64_SYS_shmdt             67
#define X64_SYS_rt_sigaction      13
#define X64_SYS_rt_sigprocmask    14
#define X64_SYS_ioctl             16
#define X64_SYS_pread64           17
#define X64_SYS_writev            20
#define X64_SYS_access            21
#define X64_SYS_readlink          89
#define X64_SYS_readlinkat        267
#define X64_SYS_faccessat         269
#define X64_SYS_faccessat2        439
#define X64_SYS_mkdir             83
#define X64_SYS_mkdirat           258
#define X64_SYS_symlink           88
#define X64_SYS_symlinkat         266
#define X64_SYS_link              86
#define X64_SYS_linkat            265
#define X64_SYS_utimensat         280
#define X64_SYS_socketpair        53
#define X64_SYS_shutdown          48
#define X64_SYS_socket            41
#define X64_SYS_connect           42
#define X64_SYS_accept            43
#define X64_SYS_bind              49
#define X64_SYS_listen            50
#define X64_SYS_getsockname       51
#define X64_SYS_getpeername       52
#define X64_SYS_setsockopt        54
#define X64_SYS_getsockopt        55
#define X64_SYS_sendmsg           46
#define X64_SYS_recvmsg           47
#define X64_SYS_sendmmsg          307
#define X64_SYS_recvmmsg          299
#define X64_SYS_sendto            44
#define X64_SYS_recvfrom          45
#define X64_SYS_unlink            87
#define X64_SYS_unlinkat          263
#define X64_SYS_rename            82
#define X64_SYS_renameat          264
#define X64_SYS_renameat2         316
#define X64_SYS_fchdir            81
#define X64_SYS_ftruncate         77
#define X64_SYS_umask             95
#define X64_SYS_setsid            112
#define X64_SYS_getpid            39
#define X64_SYS_exit              60
#define X64_SYS_uname             63
#define X64_SYS_setuid            105
#define X64_SYS_setgid            106
#define X64_SYS_getuid            102
#define X64_SYS_time              201
#define X64_SYS_getgid            104
#define X64_SYS_geteuid           107
#define X64_SYS_getegid           108
#define X64_SYS_prctl             157
#define X64_SYS_arch_prctl        158
#define X64_SYS_gettid            186
#define X64_SYS_futex             202
#define X64_SYS_set_tid_address   218
#define X64_SYS_clock_gettime     228
#define X64_SYS_exit_group        231
#define X64_SYS_openat            257
#define X64_SYS_newfstatat        262
#define X64_SYS_set_robust_list   273
#define X64_SYS_madvise           28
#define X64_SYS_fadvise64         221
#define X64_SYS_mremap            25
#define X64_SYS_sigaltstack       131
#define X64_SYS_rt_sigreturn      15
#define X64_SYS_dup               32
#define X64_SYS_dup2              33
#define X64_SYS_getcwd            79
#define X64_SYS_chdir             80
#define X64_SYS_fcntl             72
#define X64_SYS_pipe              22
#define X64_SYS_pipe2             293
#define X64_SYS_getdents64        217
#define X64_SYS_tgkill            234
#define X64_SYS_tkill             200
#define X64_SYS_prlimit64         302
#define X64_SYS_getrlimit         97
#define X64_SYS_setrlimit         160
#define X64_SYS_sched_getscheduler 145
#define X64_SYS_sched_setscheduler 144
#define X64_SYS_sched_getparam    143
#define X64_SYS_mlockall          151
#define X64_SYS_munlockall        152
#define X64_SYS_mlock             149
#define X64_SYS_munlock           150
#define X64_SYS_getrandom         318
#define X64_SYS_sched_yield       24
#define X64_SYS_sched_setaffinity 203
#define X64_SYS_sched_getaffinity 204
#define X64_SYS_statfs            137
#define X64_SYS_fstatfs           138
#define X64_SYS_kill              62
#define X64_SYS_gettimeofday      96
#define X64_SYS_getrusage         98
#define X64_SYS_sysinfo           99
#define X64_SYS_getppid           110
#define X64_SYS_getpgrp           111
#define X64_SYS_getpgid           121
#define X64_SYS_getsid            124
#define X64_SYS_clock_getres      229
#define X64_SYS_clock_nanosleep   230
#define X64_SYS_nanosleep         35
#define X64_SYS_rseq              334
#define X64_SYS_clone3            435
#define X64_SYS_eventfd2          290
#define X64_SYS_epoll_create1     291
#define X64_SYS_epoll_create      213
#define X64_SYS_epoll_ctl         233
#define X64_SYS_epoll_wait        232
#define X64_SYS_pwrite64          18
#define X64_SYS_readv             19
#define X64_SYS_select            23
#define X64_SYS_chmod             90
#define X64_SYS_fchmod            91
#define X64_SYS_clone             56
#define X64_SYS_fork              57
#define X64_SYS_vfork             58
#define X64_SYS_execve            59
#define X64_SYS_wait4             61
#define X64_SYS_pause             34
#define X64_SYS_getitimer         36
#define X64_SYS_setitimer         38
#define X64_SYS_rt_sigpending     127
#define X64_SYS_rt_sigtimedwait   128
#define X64_SYS_rt_sigqueueinfo   129
#define X64_SYS_rt_sigsuspend     130

// arch_prctl subfunctions
#define X64_ARCH_SET_GS  0x1001
#define X64_ARCH_SET_FS  0x1002
#define X64_ARCH_GET_FS  0x1003
#define X64_ARCH_GET_GS  0x1004

// Linux returns errno as the negated value in RAX. We use the same K_*
// constants as the 32-bit path so error semantics stay aligned.
#ifndef K_ENOSYS
#define K_ENOSYS 38
#endif
#ifndef K_EINVAL
#define K_EINVAL 22
#endif
#ifndef K_EFAULT
#define K_EFAULT 14
#endif
#ifndef K_EAGAIN
#define K_EAGAIN 11
#endif
#ifndef K_ENOMEM
#define K_ENOMEM 12
#endif
#ifndef K_ECHILD
#define K_ECHILD 10
#endif
#ifndef K_EINTR
#define K_EINTR 4
#endif
#ifndef K_EBADF
#define K_EBADF 9
#endif
#ifndef K_EMFILE
#define K_EMFILE 24
#endif
#ifndef K_ESRCH
#define K_ESRCH 3
#endif
#ifndef K_ENODATA
#define K_ENODATA 61
#endif
#ifndef K_ENOTSUP
#define K_ENOTSUP 95
#endif

// FUTEX_* op codes from <linux/futex.h>. We handle WAIT/WAKE + their
// BITSET variants (glibc 2.35+ uses WAKE_BITSET for pthread_cond_signal)
// and recognize REQUEUE/CMP_REQUEUE/WAKE_OP so we don't reject them with
// EINVAL — they degrade to "no waiters woken" which is correct for our
// single-threaded world. The CLOCK_REALTIME bit (0x100) is ignored
// because we don't block anyway.
#define X64_FUTEX_WAIT             0
#define X64_FUTEX_WAKE             1
#define X64_FUTEX_REQUEUE          3
#define X64_FUTEX_CMP_REQUEUE      4
#define X64_FUTEX_WAKE_OP          5
#define X64_FUTEX_WAIT_BITSET      9
#define X64_FUTEX_WAKE_BITSET      10
#define X64_FUTEX_PRIVATE_FLAG     128
#define X64_FUTEX_CLOCK_REALTIME   256
#define X64_FUTEX_WAIT_PRIVATE     (X64_FUTEX_WAIT | X64_FUTEX_PRIVATE_FLAG)
#define X64_FUTEX_WAKE_PRIVATE     (X64_FUTEX_WAKE | X64_FUTEX_PRIVATE_FLAG)

// MAP_* bits used by mmap. Kept local to avoid pulling kernel.h here.
#ifndef K_MAP_ANONYMOUS
#define K_MAP_ANONYMOUS 0x20
#endif
#ifndef K_MAP_FIXED
#define K_MAP_FIXED 0x10
#endif

// mmap(NULL, ...) placement is selected by KMemory64. Sparse guests retain
// the historical base; native-identity guests use the guarded iOS window.

// Address-space allocation for mmap(NULL,...) now lives in
// KMemory64::mmapReserveAndMap, which scans for a free gap and maps it as ONE
// atomic step under the process mmap lock. The old split here (allocMmapRange
// returns an address, the caller maps it afterwards) was a TOCTOU race in the
// multi-threaded build: two sibling threads scanned, both saw the same gap
// free, both mapped there, and the second map zeroed the first → guest-heap
// corruption ("malloc(): corrupted double linked list") in wineserver during
// the boot storm. The shared bump cursor moved to KMemory64::mmapNext.

// BW64_CRASHRING: a tiny lock-light ring of the last few socket writes (pid +
// fd + first 16 bytes = the wineserver request header / opcode). When a guest
// process prints an abort to stderr (e.g. wineserver's "release_object:
// Assertion 'obj->refcount' failed"), we dump the ring so the request sequence
// that led to the crash is visible WITHOUT the multi-GB BW64_IPCDUMP firehose.
// Zero cost when the env var is unset.
// kind: 'W'=write, 'R'=read, 'M'=recvmsg, 'F'=scm_rights fd delivered (recv),
//       'S'=sendmsg scm_rights fd sent (e0=#fds, e1=first guest fd).
// extra0/extra1 carry per-kind context (see crashRingRecordRead callers).
struct CrashRingEntry { char kind; U32 pid; U32 fd; U32 len; U32 extra0; U32 extra1; U8 hdr[16]; };
// Serializes guest file reads (sys_read64 + sys_mmap64_file) against the shared
// per-zip decompression stream. wine's DLLs live in wine64.zip; every
// FsZipOpenNode of the same entry shares ONE unzFile + position state, and the
// seek()+readNative() that maps a DLL segment is several un-co-locked calls. Two
// wine processes loading the same DLL on separate host threads otherwise
// interleave seek/read and each reads from the OTHER's file position — splatter-
// ing a DLL's MZ header over its own .text/.data/IAT, corrupting the IAT, and
// producing a wild indirect call into low memory (RIP=0x10270) -> "could not load
// kernel32.dll" -> cascading wineserver heap corruption. Holding this lock across
// the whole read makes file reads atomic. Not perf-critical (DLL load path).
static std::mutex g_fileReadMutex;

static CrashRingEntry g_crashRing[64];
static std::atomic<U32> g_crashRingPos{0};
static bool g_crashRingOn = false;
static bool g_crashRingChecked = false;
// BW64_WSREAD: also record the READ side (read/recvmsg/scm) of wineserver
// sockets into the same ring, so the dump shows the request/reply/fd-pass
// sequence — needed to catch a duplicated or mis-framed request that drives
// wineserver's release_object double-free. Independent gate, zero cost when off.
static bool g_wsReadOn = false;
static bool g_wsReadChecked = false;
static bool wsReadEnabled() {
    if (!g_wsReadChecked) { g_wsReadOn = getenv("BW64_WSREAD") != nullptr; g_wsReadChecked = true; }
    return g_wsReadOn;
}
static void crashRingPut(char kind, U32 pid, U32 fd, const U8* data, U32 count, U32 extra0, U32 extra1) {
    U32 i = g_crashRingPos.fetch_add(1) % 64;
    g_crashRing[i].kind = kind;
    g_crashRing[i].pid = pid; g_crashRing[i].fd = fd; g_crashRing[i].len = count;
    g_crashRing[i].extra0 = extra0; g_crashRing[i].extra1 = extra1;
    U32 n = count < 16 ? count : 16;
    for (U32 j = 0; j < 16; j++) g_crashRing[i].hdr[j] = (data && j < n) ? data[j] : 0;
}
static void crashRingRecord(U32 pid, U32 fd, const U8* data, U32 count) {
    if (!g_crashRingChecked) { g_crashRingOn = getenv("BW64_CRASHRING") != nullptr; g_crashRingChecked = true; }
    if (!g_crashRingOn) return;
    crashRingPut('W', pid, fd, data, count, 0, 0);
}
// BW64_WSCONC: concurrency witness for the bug #2 teardown crash. wine's
// wineserver is designed single-threaded (one epoll loop processes every
// client request + disconnect strictly serially). If our MT model ever has two
// HOST threads inside wineserver's own request processing at the same instant,
// a close_handle's release_object can race the disconnect-cascade's release of
// the same object -> refcount underflow -> the object.c:443 assert. This counts
// how many host threads are concurrently reading wineserver's (pid 14) socket;
// any value >1 is the smoking gun. Keyed by the READING process id so it only
// arms for wineserver itself. Zero cost unless BW64_WSCONC is set.
static bool g_wsConcInit = false, g_wsConcOn = false;
static std::atomic<int> g_wsConcInflight{0};
static std::atomic<int> g_wsConcMax{0};
static bool wsConcEnabled() {
    if (!g_wsConcInit) { g_wsConcOn = getenv("BW64_WSCONC") != nullptr; g_wsConcInit = true; }
    return g_wsConcOn;
}
// Read-side recorder (used by sys_read64 / sys_recvmsg64 / the scm witness).
// Gated by BW64_WSREAD AND the same ring being on so the dump path fires.
static void crashRingRecordRead(char kind, U32 pid, U32 fd, const U8* data, U32 count, U32 extra0, U32 extra1) {
    if (!g_crashRingChecked) { g_crashRingOn = getenv("BW64_CRASHRING") != nullptr; g_crashRingChecked = true; }
    if (!g_crashRingOn || !wsReadEnabled()) return;
    crashRingPut(kind, pid, fd, data, count, extra0, extra1);
}
static void crashRingDump(const char* why) {
    if (!g_crashRingOn) return;
    klog_fmt("CRASHRING dump (%s) — last 64 socket events, oldest first:", why);
    U32 start = g_crashRingPos.load();
    for (U32 k = 0; k < 64; k++) {
        CrashRingEntry& e = g_crashRing[(start + k) % 64];
        if (e.kind == 0) continue;
        klog_fmt("  CR[%u] %c pid=%u fd=%u len=%u e0=%u e1=%u  [%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x]",
                 k, e.kind, e.pid, e.fd, e.len, e.extra0, e.extra1,
                 e.hdr[0],e.hdr[1],e.hdr[2],e.hdr[3],e.hdr[4],e.hdr[5],e.hdr[6],e.hdr[7],
                 e.hdr[8],e.hdr[9],e.hdr[10],e.hdr[11],e.hdr[12],e.hdr[13],e.hdr[14],e.hdr[15]);
    }
}

// A guest that exits non-zero after a blocked read leaves almost nothing
// behind. This prints, once, everything that was already being collected: the
// register state at the exit, the tail of that process's syscalls, the socket
// ring, and what became of its descendants. Nothing here runs for a process
// that exits cleanly.
// Defined with the dispatcher below; the exit tail names its syscalls.
static const char* x64SyscallName(U64 nr);

static const char* x64SyscallTailStateName(boxedvn::SyscallTailState state) {
    switch (state) {
        case boxedvn::SyscallTailState::Pending: return "pending";
        case boxedvn::SyscallTailState::Completed: return "ok";
        case boxedvn::SyscallTailState::Restarted: return "restart";
        case boxedvn::SyscallTailState::Unused: break;
    }
    return "unused";
}

static void dumpX64DescendantSnapshot(KProcess* parent) {
    if (!parent) return;
    // Bounded: the descendants of one process, one line each, plus a line per
    // live thread. Wine forks a handful; a runaway is capped anyway.
    constexpr U32 kMaxProcesses = 32;
    U32 reported = 0;
    for (U32 id : KSystem::getProcessIdsWithThreads()) {
        KProcessPtr child = KSystem::getProcess(id);
        if (!child || child->parentId != parent->id) continue;
        if (reported++ >= kMaxProcesses) {
            klog_fmt("  X64_EXIT_DESC ... more descendants not listed");
            break;
        }
        // KProcess already publishes this behind its own lock; reaching for
        // the private map would only duplicate it.
        const U32 threadCount = child->getThreadCount();
        klog_fmt("  X64_EXIT_DESC pid=%u parent=%u exe='%s' cmd='%s' "
                 "state=%s exit=%u threads=%u",
                 (unsigned)child->id, (unsigned)child->parentId,
                 child->exe.c_str(), child->commandLine.c_str(),
                 child->terminated ? "terminated" : "running",
                 (unsigned)child->exitCode, (unsigned)threadCount);
    }
    if (reported == 0) {
        // Precisely what was measured, and nothing more. A daemon that
        // reparented itself away is not a direct child any more, so its
        // absence here says nothing about whether it is alive -- the socket
        // ring is what shows that.
        klog_fmt("  X64_EXIT_DESC none: no live DIRECT child of pid=%u "
                 "(a reparented daemon would not appear here)",
                 (unsigned)parent->id);
    }
}

// Wine's exit code when the loader could not resolve an import:
// STATUS_DLL_NOT_FOUND, which the shell reports as 3221225781. It names the
// module in an err:module:import_dll line on stderr -- but two device
// sessions carried no Wine debug output at all, so that line is not something
// this log can rely on. The module-search recorder saw the same search from
// the syscall side and knows which name never resolved.
#define K_X64_STATUS_DLL_NOT_FOUND 0xC0000135u

// One line, once, at the exit that needs it: the module the loader searched
// for and never found. Unbudgeted -- it is a single line per process, and the
// process is already exiting.
static void reportX64MissingImport(KProcess* process, U64 status) {
    if (!process || (U32)status != K_X64_STATUS_DLL_NOT_FOUND) {
        return;
    }
    char module[K_DLL_SEARCH_MODULE_NAME_MAX];
    process->dllSearch.lastUnresolvedModule(module, sizeof(module));
    klog_fmt("BOXEDWINE_X64_IMPORT_MISSING pid=%u module=%s status=0x%08x "
             "probes=%u trace_budget_left=%u",
             (unsigned)process->id,
             module[0] ? module : "(none recorded)",
             (unsigned)(U32)status, process->dllSearch.probes(),
             process->dllSearch.remaining());
}

static void dumpX64ExitDiagnostics(CPU64* cpu, U64 status) {
    if (!cpu || !cpu->thread || !cpu->thread->process) return;
    KProcess* process = cpu->thread->process.get();
    if (!process->syscallTail.claimDump()) return;

    reportX64MissingImport(process, status);

    klog_fmt("X64_EXIT_DIAG pid=%u status=%llu exe='%s' cmd='%s' "
             "rip=0x%llx syscall_rip=0x%llx rsp=0x%llx rax=0x%llx "
             "fs=0x%llx gs=0x%llx",
             (unsigned)process->id, (unsigned long long)status,
             process->exe.c_str(), process->commandLine.c_str(),
             (unsigned long long)cpu->rip,
             (unsigned long long)cpu->syscallRip,
             (unsigned long long)cpu->reg[X64_RSP].u64,
             (unsigned long long)cpu->reg[X64_RAX].u64,
             (unsigned long long)cpu->fsbase,
             (unsigned long long)cpu->gsbase);
    klog_fmt("X64_EXIT_TAIL pid=%u recorded=%llu (oldest first)",
             (unsigned)process->id,
             (unsigned long long)process->syscallTail.recorded());
    process->syscallTail.forEach([&](const boxedvn::SyscallTailRecord& r) {
        // A pending record is a syscall that never came back. Printing a
        // result for it would invent one.
        if (r.state == boxedvn::SyscallTailState::Completed) {
            klog_fmt("  X64_TAIL[%llu] pid=%u tid=%u nr=%llu (%s) "
                     "rip=0x%llx a=(0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx) "
                     "-> %lld state=ok",
                     (unsigned long long)r.sequence, (unsigned)r.processId,
                     (unsigned)r.threadId, (unsigned long long)r.number,
                     x64SyscallName(r.number), (unsigned long long)r.syscallRip,
                     (unsigned long long)r.arguments[0],
                     (unsigned long long)r.arguments[1],
                     (unsigned long long)r.arguments[2],
                     (unsigned long long)r.arguments[3],
                     (unsigned long long)r.arguments[4],
                     (unsigned long long)r.arguments[5],
                     (long long)r.result);
        } else {
            klog_fmt("  X64_TAIL[%llu] pid=%u tid=%u nr=%llu (%s) "
                     "rip=0x%llx a=(0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx) "
                     "-> none state=%s",
                     (unsigned long long)r.sequence, (unsigned)r.processId,
                     (unsigned)r.threadId, (unsigned long long)r.number,
                     x64SyscallName(r.number), (unsigned long long)r.syscallRip,
                     (unsigned long long)r.arguments[0],
                     (unsigned long long)r.arguments[1],
                     (unsigned long long)r.arguments[2],
                     (unsigned long long)r.arguments[3],
                     (unsigned long long)r.arguments[4],
                     (unsigned long long)r.arguments[5],
                     x64SyscallTailStateName(r.state));
        }
    });
    // The socket ring was previously printed only when a guest wrote an abort
    // message to stderr. A process that exits non-zero without saying anything
    // is exactly the case that needed it.
    crashRingDump("non-zero exit");
    dumpX64DescendantSnapshot(process);
}

// Report one module-search path operation, or don't. Everything about the
// bounding lives in DllSearchTrace; this is only the wording.
//
// `detail` is whatever distinguishes this operation -- the flags an open used,
// the result a stat returned, what enumerating a directory produced. Callers
// pass a formatted fragment rather than a second format string so that every
// line has the same shape and stays greppable.
static void reportDllSearch(KProcess* process, const char* op, const char* path,
                            long long result, const char* detail) {
    if (!process) {
        return;
    }
    // Before the budget is consulted: which module the search was for has to
    // survive a spent budget, because that is exactly when the log stops
    // saying it. See BOXEDWINE_X64_IMPORT_MISSING at exit.
    process->dllSearch.noteResult(path, result);
    const boxedvn::DllSearchTrace::Decision decision =
        process->dllSearch.record(path);
    if (decision == boxedvn::DllSearchTrace::Decision::Silent) {
        return;
    }
    klog_fmt("BOXEDWINE_X64_DLL_SEARCH pid=%u op=%s path='%s' result=%lld %s%s",
             (unsigned)process->id, op, path ? path : "(null)", result,
             detail ? detail : "",
             decision == boxedvn::DllSearchTrace::Decision::Exhausted
                 ? " [budget spent; further module-search paths are not reported]"
                 : "");
}

static U64 sys_write64(CPU64* cpu, U64 fd, U64 buf, U64 count) {
    if (count == 0) return 0;
    // Ceiling only (was a 1MB cap that silently truncated large writes for any
    // caller that doesn't loop on a short return). 256MB is plenty for the temp
    // buffer while still bounding a pathological count.
    if (count > (256ULL << 20)) count = 256ULL << 20;
    std::vector<U8> buffer((size_t)count + 1);
    cpu->memory->memcpyFromGuest(buffer.data(), buf, count);
    buffer[count] = 0;
    // Wine's protocol union is a fixed size in both directions. Remember the
    // opcode of the last whole request this process sent: the reply comes back
    // on a different descriptor and carries no opcode of its own, so this is
    // the only thing that identifies which exchange it belongs to.
    if (cpu->thread && cpu->thread->process) {
        const int opcode =
            boxedvn::wineServerRequestOpcode(buffer.data(), count);
        if (opcode >= 0) {
            cpu->thread->process->lastServerRequestOpcode = opcode;
            cpu->thread->process->lastServerRequestFd = (U32)fd;
        }
    }
    if (fd == 1 || fd == 2) {
        // Tee stdout/stderr to host console so ld-linux + glibc diagnostics
        // surface immediately. Also forward to the kobject so anything
        // tailing the host FS still sees it.
        U32 wpid = (cpu->thread && cpu->thread->process) ? cpu->thread->process->id : 0;
        const char* wexe = (cpu->thread && cpu->thread->process) ? cpu->thread->process->name.c_str() : "?";
        klog_fmt("[guest fd=%llu pid=%u %s] %s", (unsigned long long)fd, (unsigned)wpid, wexe, (const char*)buffer.data());
        // On a guest abort, dump the request ring so we can see the wineserver
        // request sequence that triggered the double-release.
        if (fd == 2 && (strstr((const char*)buffer.data(), "release_object") ||
                        strstr((const char*)buffer.data(), "Assertion") ||
                        strstr((const char*)buffer.data(), "tcache") ||
                        strstr((const char*)buffer.data(), "unaligned") ||
                        strstr((const char*)buffer.data(), "unimplemented function") ||
                        strstr((const char*)buffer.data(), "corrupted"))) {
            crashRingDump((const char*)buffer.data());
            // BW64_WSBT: at the assert WRITE (glibc prints the message BEFORE
            // abort()), wineserver's guest stack is still intact. Scan it for
            // code-looking return addresses so addr2line on the wineserver binary
            // can name the release_object CALLER (the buggy teardown path). The
            // assert chain is __assert_fail -> release_object -> <culprit>. We
            // print RIP + any stack word that lands in the wineserver text range
            // [base, base+size). The image base is logged once at load (BW64_WSBT
            // also enables that). Cheap: only on the abort path.
            if (getenv("BW64_WSBT")) {
                auto rdq = [&](U64 a) -> U64 {
                    U64 v = 0;
                    for (int b = 0; b < 8; b++) v |= ((U64)cpu->memory->readb(a + b)) << (b * 8);
                    return v;
                };
                klog_fmt("WSBT: assert in pid=%u %s  RIP=%llx RSP=%llx RBP=%llx",
                         (unsigned)wpid, wexe,
                         (unsigned long long)cpu->rip,
                         (unsigned long long)cpu->reg[X64_RSP].u64,
                         (unsigned long long)cpu->reg[X64_RBP].u64);
                U64 sp = cpu->reg[X64_RSP].u64;
                // Walk a wide stack window; print every word that could be a
                // return address into a mapped, executable-ish region. We don't
                // know the exact text range here, so print all plausible code
                // pointers (high-half canonical, page-aligned-ish) and let
                // post-processing filter by the wineserver mmap range.
                for (int i = 0; i < 160; i++) {
                    U64 v = rdq(sp + (U64)i * 8);
                    // Heuristic: wineserver/glibc code lives at low-ish 47-bit
                    // addresses; skip obvious data (small ints, ASCII, stack ptrs
                    // near sp). Print candidates with the 'CODE?' tag.
                    if (v > 0x10000 && v < 0x800000000000ULL &&
                        (v < sp - 0x100000 || v > sp + 0x100000)) {
                        klog_fmt("WSBT:   [rsp+%03x] = 0x%llx", i * 8, (unsigned long long)v);
                    }
                }
            }
            // BW64_MALLOCDUMP: for the glibc malloc-METADATA faces ("unaligned
            // tcache chunk" / "corrupted double-linked list" / "corrupted
            // unsorted chunks") — the dominant faces of bug #2 — REFWATCH can't
            // fire (no release_object). glibc's malloc_printerr(msg) is reached
            // from the freelist-walk that detected the bad chunk, and the bad
            // chunk pointer is usually still live in a callee-saved register
            // (rbx/rbp/r12-r15, preserved across __libc_message) or a nearby
            // stack slot. Dump them + flag stack words that point into the
            // wineserver heap range so we can correlate the corrupted address
            // against recent writes (BW64_MEMRING). Cheap: abort path only.
            if (getenv("BW64_MALLOCDUMP") &&
                (strstr((const char*)buffer.data(), "tcache") ||
                 strstr((const char*)buffer.data(), "corrupted") ||
                 strstr((const char*)buffer.data(), "unaligned"))) {
                auto rdq = [&](U64 a) -> U64 {
                    U64 v = 0;
                    for (int b = 0; b < 8; b++) v |= ((U64)cpu->memory->readb(a + b)) << (b * 8);
                    return v;
                };
                klog_fmt("MALLOCDUMP pid=%u %s RIP=%llx RSP=%llx RBP=%llx RBX=%llx R12=%llx R13=%llx R14=%llx R15=%llx RDI=%llx RSI=%llx",
                         (unsigned)wpid, wexe, (unsigned long long)cpu->rip,
                         (unsigned long long)cpu->reg[X64_RSP].u64, (unsigned long long)cpu->reg[X64_RBP].u64,
                         (unsigned long long)cpu->reg[X64_RBX].u64, (unsigned long long)cpu->reg[X64_R12].u64,
                         (unsigned long long)cpu->reg[X64_R13].u64, (unsigned long long)cpu->reg[X64_R14].u64,
                         (unsigned long long)cpu->reg[X64_R15].u64, (unsigned long long)cpu->reg[X64_RDI].u64,
                         (unsigned long long)cpu->reg[X64_RSI].u64);
                // Heap candidates: glibc's main arena heap lives well above the
                // PE/text mappings and below the stack. Print stack words that
                // look like heap data pointers (not text <0x500000000, not near
                // rsp), and dump 16 bytes around each — a corrupted chunk shows a
                // mis-aligned or wild fd/bk.
                U64 sp = cpu->reg[X64_RSP].u64;
                for (int i = 0; i < 64; i++) {
                    U64 v = rdq(sp + (U64)i * 8);
                    if (v > 0x500000000ULL && v < 0x800000000000ULL &&
                        (v < sp - 0x200000 || v > sp + 0x200000)) {
                        klog_fmt("MALLOCDUMP   [rsp+%03x]=0x%llx  chunk?[-8..+16]= %016llx | %016llx %016llx %016llx",
                                 i * 8, (unsigned long long)v,
                                 (unsigned long long)rdq(v - 8), (unsigned long long)rdq(v),
                                 (unsigned long long)rdq(v + 8), (unsigned long long)rdq(v + 16));
                    }
                }
                // Correlate against the wineserver write ring (BW64_MEMRING).
                kmemory64DumpMemRing(0);
            }
        }
        // BW64_STUBDUMP: when wine reports a call to an unimplemented function,
        // dump the live register/stack state so we can identify the stub and the
        // ABI register the dll/func name strings actually landed in. The abort
        // message itself loses the names (they print as ".garbage").
        if (fd == 2 && getenv("BW64_STUBDUMP") &&
            strstr((const char*)buffer.data(), "unimplemented function")) {
            auto rdStr = [&](U64 a) -> std::string {
                std::string s;
                if (!a) return "<null>";
                for (int i = 0; i < 64; i++) {
                    U8 c = cpu->memory->readb(a + i);
                    if (!c) break;
                    if (c < 32 || c > 126) { s += '?'; } else s += (char)c;
                }
                return s.empty() ? "<empty>" : s;
            };
            klog_fmt("STUBDUMP pid=%u RIP=%llx RSP=%llx RAX=%llx RCX=%llx RDX=%llx R8=%llx R9=%llx RDI=%llx RSI=%llx",
                     (unsigned)wpid, (unsigned long long)cpu->rip,
                     (unsigned long long)cpu->reg[X64_RSP].u64, (unsigned long long)cpu->reg[X64_RAX].u64,
                     (unsigned long long)cpu->reg[X64_RCX].u64, (unsigned long long)cpu->reg[X64_RDX].u64,
                     (unsigned long long)cpu->reg[X64_R8].u64, (unsigned long long)cpu->reg[X64_R9].u64,
                     (unsigned long long)cpu->reg[X64_RDI].u64, (unsigned long long)cpu->reg[X64_RSI].u64);
            klog_fmt("STUBDUMP  RCX->\"%s\" RDX->\"%s\" R8->\"%s\" RDI->\"%s\" RSI->\"%s\"",
                     rdStr(cpu->reg[X64_RCX].u64).c_str(), rdStr(cpu->reg[X64_RDX].u64).c_str(),
                     rdStr(cpu->reg[X64_R8].u64).c_str(), rdStr(cpu->reg[X64_RDI].u64).c_str(),
                     rdStr(cpu->reg[X64_RSI].u64).c_str());
            auto rdq = [&](U64 a) -> U64 {
                U64 v = 0;
                for (int b = 0; b < 8; b++) v |= ((U64)cpu->memory->readb(a + b)) << (b * 8);
                return v;
            };
            U64 sp = cpu->reg[X64_RSP].u64;
            for (int i = 0; i < 12; i++) {
                U64 v = rdq(sp + (U64)i * 8);
                klog_fmt("STUBDUMP  [rsp+%02x]=%llx  ->\"%s\"", i * 8, (unsigned long long)v, rdStr(v).c_str());
            }
        }
    } else if (cpu->thread && cpu->thread->process) {
        crashRingRecord(cpu->thread->process->id, (U32)fd, buffer.data(), (U32)count);
    }
    if (!cpu->thread || !cpu->thread->process) {
        return count;
    }
    KFileDescriptorPtr fdesc = cpu->thread->process->getFileDescriptor((FD)fd);
    if (!fdesc) {
        if (fd == 1 || fd == 2) return count; // already klog'd
        return (U64)-9; // -EBADF
    }
    if (!fdesc->canWrite()) {
        return (U64)-K_EINVAL;
    }
    U32 wrote = fdesc->kobject->writeNative(buffer.data(), (U32)count);
    // A connected stream socket whose peer object is TRANSIENTLY unreferenced
    // under boot-time fd-table churn returns -K_EWOULDBLOCK (see
    // KUnixSocketObject::writeNative). On a blocking fd this must not surface to
    // the guest (wine treats a short/odd write to its wineserver request socket
    // as fatal -> "partial write"). Retry a bounded number of host yields so the
    // peer's strong ref reappears; only give up (real -EPIPE) if it never does.
    if ((S32)wrote == -K_EWOULDBLOCK && fdesc->kobject->isBlocking()) {
        for (int spin = 0; spin < 1000 && (S32)wrote == -K_EWOULDBLOCK; spin++) {
            std::this_thread::yield();
            wrote = fdesc->kobject->writeNative(buffer.data(), (U32)count);
        }
        if ((S32)wrote == -K_EWOULDBLOCK) {
            wrote = (U32)-K_EPIPE; // peer truly gone after retries
        }
    }
    if (getenv("BW64_IPCDUMP")) {
        char hex[64] = {0}; int n = (int)((count < 16) ? count : 16);
        for (int i = 0; i < n; i++) snprintf(hex + i*3, 4, "%02x ", buffer[i]);
        klog_fmt("IPC [pid=%d] write(fd=%d,count=%llu) -> %d  [%s]",
                 (int)cpu->thread->process->id, (int)fd,
                 (unsigned long long)count, (int)(S32)wrote, hex);
    }
    return (S32)wrote < 0 ? (U64)(S64)(S32)wrote : (U64)wrote;
}

static U64 sys_arch_prctl64(CPU64* cpu, U64 code, U64 addr) {
    static std::atomic<U32> logCount{0};
    const U32 ordinal = logCount.fetch_add(1, std::memory_order_relaxed);
    const bool logCall = ordinal < 16;
    const U32 processId = cpu && cpu->thread && cpu->thread->process
        ? cpu->thread->process->id : 0;
    const bool validOperation = code == X64_ARCH_SET_FS ||
        code == X64_ARCH_SET_GS || code == X64_ARCH_GET_FS ||
        code == X64_ARCH_GET_GS;
    const char* operation = code == X64_ARCH_SET_FS ? "set-fs"
        : code == X64_ARCH_SET_GS ? "set-gs"
        : code == X64_ARCH_GET_FS ? "get-fs"
        : code == X64_ARCH_GET_GS ? "get-gs" : "invalid";
    if (logCall || !validOperation) {
        klog_fmt("BOXEDWINE_X64_ARCH_PRCTL ordinal=%u pid=%u op=%s addr=0x%llx fs=0x%llx gs=0x%llx",
                 ordinal, processId, operation, (unsigned long long)addr,
                 (unsigned long long)cpu->fsbase,
                 (unsigned long long)cpu->gsbase);
    }
    switch (code) {
        case X64_ARCH_SET_FS:
            cpu->fsbase = addr;
            return 0;
        case X64_ARCH_SET_GS:
            cpu->gsbase = addr;
            return 0;
        case X64_ARCH_GET_FS:
            cpu->memory->writeq(addr, cpu->fsbase);
            return 0;
        case X64_ARCH_GET_GS:
            cpu->memory->writeq(addr, cpu->gsbase);
            return 0;
        default:
            return (U64)-K_EINVAL;
    }
}

// brk(0) → returns current break. brk(new) → tries to set break, returns
// new break on success or old break on failure (Linux contract). Stored
// per-process on KProcess::brkEnd in the 32-bit path; we put it on the
// thread's process the same way.
static U64 sys_brk64(CPU64* cpu, U64 newBrk) {
    // Resolve where the current break lives. Full kernel: KProcess::brkEnd64.
    // Standalone runner (no process): CPU64::runnerBrk, seeded by the runner
    // to just past the loaded image. If neither is available there's no heap
    // to manage, so report 0 (the historical no-process behaviour).
    bool hasProcess = (cpu->thread && cpu->thread->process);
    U64 oldBrk;
    if (hasProcess) {
        oldBrk = cpu->thread->process->brkEnd64;
    } else if (cpu->runnerBrk) {
        oldBrk = cpu->runnerBrk;
    } else {
        return 0;
    }

    if (newBrk == 0 || newBrk < oldBrk) {
        return oldBrk;
    }
    // Map the gap as anonymous RW. mmapAnonymousFixed expects page-aligned
    // addresses; round oldBrk up and newBrk up to page boundaries.
    U64 alignedOld = (oldBrk + 0xFFF) & ~0xFFFULL;
    U64 alignedNew = (newBrk + 0xFFF) & ~0xFFFULL;
    if (alignedNew > alignedOld) {
        const U64 firstPage = alignedOld >> K64_PAGE_SHIFT;
        const U64 pageCount = (alignedNew - alignedOld) >> K64_PAGE_SHIFT;
        for (U64 i = 0; i < pageCount; ++i) {
            if (cpu->memory->getPageFlags(firstPage + i) & K64_PAGE_MAPPED) {
                klog_fmt("sys_brk64: refusing occupied growth range "
                         "[0x%llx,0x%llx) at page 0x%llx",
                         (unsigned long long)alignedOld,
                         (unsigned long long)alignedNew,
                         (unsigned long long)((firstPage + i) << K64_PAGE_SHIFT));
                return oldBrk;
            }
        }
        U64 ret = cpu->memory->mmapAnonymousFixed(alignedOld, alignedNew - alignedOld, 0x3); // PROT_READ|WRITE
        if ((S64)ret < 0) {
            klog_fmt("sys_brk64: failed to map growth range [0x%llx,0x%llx), error=%lld",
                     (unsigned long long)alignedOld,
                     (unsigned long long)alignedNew,
                     (long long)(S64)ret);
            return oldBrk;
        }
    }
    if (hasProcess) cpu->thread->process->brkEnd64 = newBrk;
    else            cpu->runnerBrk = newBrk;
    return newBrk;
}

// ---- Bounded guest mmap placement diagnostics -----------------------------
// The relocation storm that drained the guest window was invisible except as
// 161,259 identical log lines with no flags, no protection and no outcome. The
// counters below are process-global and lock-free; the per-call lines stop
// after a small prefix so the same build can prove the incoming contract
// without producing a log nobody can read.
namespace {

struct GuestMmapCounters {
    std::atomic<U64> calls {0};
    std::atomic<U64> lowIneligibleHints {0};
    std::atomic<U64> fixedNoReplaceRequests {0};
    std::atomic<U64> highWindowRelocations {0};
    std::atomic<U64> rejectedWithoutAllocating {0};
    std::atomic<U64> highWindowFailures {0};
    std::atomic<U64> sparseReservations {0};
};

GuestMmapCounters gGuestMmapCounters;

// Per-address-space detail budgeting and repeat tracking. The old global
// ordinal spent the whole detail budget on the loader and the short-lived
// helper processes, so the requests the failing process made after the
// Windows PE handoff were never described at all. These two are plain value
// types (see guest_mmap_diagnostics.h) guarded by one mutex, because the
// mmap path has no allocator lock of its own to borrow.
std::mutex gGuestMmapDiagnosticsMutex;
boxedvn::GuestMmapDetailBudget gGuestMmapDetailBudget;
boxedvn::GuestMmapRepeatTracker gGuestMmapRepeats;

constexpr U64 kGuestMmapDetailLines = 32;
// Bounded on purpose: Wine reserves the arena once and commits a handful of
// subranges inside it. A run that produces many of these is the failure this
// exists to make visible, not something to narrate line by line.
constexpr U64 kGuestTopAliasReports = 12;
std::atomic<U64> gGuestTopAliasReports {0};

// One line when the guest's top-down arena is reserved or committed, naming
// both the canonical address the guest asked for and the host address its
// bytes live at. Which of the two a log is showing was previously left to be
// inferred.
void reportGuestTopAlias(CPU64* cpu, const char* what, U64 guestAddress,
                         U64 length, U32 protection) {
    if (!boxedvn::isTopArenaGuestAddress(guestAddress)) {
        return;
    }
    if (gGuestTopAliasReports.fetch_add(1, std::memory_order_relaxed) >=
        kGuestTopAliasReports) {
        return;
    }
    klog_fmt("BOXEDWINE_X64_TOP_ALIAS_%s pid=%d tid=%d guest=0x%llx "
             "host=0x%llx len=0x%llx prot=0x%x",
             what,
             (int)(cpu->thread ? cpu->thread->process->id : -1),
             (int)(cpu->thread ? cpu->thread->id : -1),
             (unsigned long long)guestAddress,
             (unsigned long long)boxedvn::guestToHostAddress(guestAddress),
             (unsigned long long)length, (unsigned)protection);
}

const char* guestMmapPlacementName(boxedvn::GuestMmapPlacement placement) {
    switch (placement) {
        case boxedvn::GuestMmapPlacement::MapExact: return "map-exact";
        case boxedvn::GuestMmapPlacement::MapExactNoReplace:
            return "map-exact-noreplace";
        case boxedvn::GuestMmapPlacement::RelocateHighWindow:
            return "relocate-high-window";
        case boxedvn::GuestMmapPlacement::FailExists: return "fail-eexist";
        case boxedvn::GuestMmapPlacement::FailNoMemory: return "fail-enomem";
        case boxedvn::GuestMmapPlacement::ReserveSparse: return "reserve-sparse";
    }
    return "unknown";
}

void reportGuestMmapSummary(const char* reason) {
    klog_fmt("BOXEDWINE_X64_MMAP_SUMMARY reason=%s calls=%llu "
             "low_ineligible=%llu noreplace=%llu relocations=%llu "
             "rejected=%llu window_failures=%llu sparse=%llu",
             reason,
             (unsigned long long)gGuestMmapCounters.calls.load(
                 std::memory_order_relaxed),
             (unsigned long long)gGuestMmapCounters.lowIneligibleHints.load(
                 std::memory_order_relaxed),
             (unsigned long long)gGuestMmapCounters.fixedNoReplaceRequests.load(
                 std::memory_order_relaxed),
             (unsigned long long)gGuestMmapCounters.highWindowRelocations.load(
                 std::memory_order_relaxed),
             (unsigned long long)
                 gGuestMmapCounters.rejectedWithoutAllocating.load(
                     std::memory_order_relaxed),
             (unsigned long long)gGuestMmapCounters.highWindowFailures.load(
                 std::memory_order_relaxed),
             (unsigned long long)gGuestMmapCounters.sparseReservations.load(
                 std::memory_order_relaxed));
}

// After the detailed prefix, report only at powers of two. The old fixed
// stride of one summary per 4096 calls still produced 2,177 lines against a
// guest issuing nine million mmaps -- bounded in principle, unreadable in
// practice. This is diagnostics only: nothing about which mappings are made,
// attempted or refused changes, and the first requests are still printed in
// full.
bool guestMmapSummaryDue(U64 ordinal) {
    return ordinal >= kGuestMmapDetailLines && (ordinal & (ordinal - 1)) == 0;
}

} // namespace

// The return address of whoever called the libc mmap wrapper, when it can be
// had. glibc's wrapper issues SYSCALL with the caller's return address still
// on top of the stack, so [rsp] identifies the caller in the common case. It
// is a hint, not a contract: a wrapper that pushed anything, or a stack page
// that is not mapped, yields zero rather than a guess.
static U64 guestCallerReturnAddress(CPU64* cpu) {
    if (!cpu || !cpu->memory) return 0;
    const U64 rsp = cpu->reg[X64_RSP].u64;
    if (rsp == 0 || (rsp & 0x7ULL) != 0) return 0;
    if (!cpu->memory->isPageMapped(rsp >> K64_PAGE_SHIFT)) return 0;
    if (((rsp + 7) >> K64_PAGE_SHIFT) != (rsp >> K64_PAGE_SHIFT) &&
        !cpu->memory->isPageMapped((rsp + 7) >> K64_PAGE_SHIFT)) {
        return 0;
    }
    return cpu->memory->readq(rsp);
}

// One line describing a single mmap request in full. Used both for the
// per-address-space detailed prefix and for the bounded repeat reports, so a
// reader never has to correlate two different formats.
static void reportGuestMmapRequest(CPU64* cpu, const char* kind,
                                   const boxedvn::GuestMmapSignature& signature,
                                   const boxedvn::GuestMmapRequest& request,
                                   boxedvn::GuestMmapPlacement placement,
                                   bool sparseOverlap, U64 sparseCount,
                                   U64 ret, U64 occurrences) {
    klog_fmt("BOXEDWINE_X64_MMAP_%s pid=%llu tid=%llu space=%llu rip=0x%llx "
             "caller=0x%llx addr=0x%llx len=0x%llx prot=0x%x flags=0x%x "
             "fixed=%d noreplace=%d native=%d allowed=%d free=%d unmapped=%d "
             "sparse_overlap=%d sparse_count=%llu placement=%s ret=0x%llx "
             "errno=%lld repeat=%llu",
             kind, (unsigned long long)signature.processId,
             (unsigned long long)signature.threadId,
             (unsigned long long)signature.addressSpace,
             (unsigned long long)(cpu ? cpu->rip : 0),
             (unsigned long long)guestCallerReturnAddress(cpu),
             (unsigned long long)signature.address,
             (unsigned long long)signature.length,
             (unsigned)signature.protection, (unsigned)signature.flags,
             request.fixed ? 1 : 0, request.fixedNoReplace ? 1 : 0,
             request.nativeIdentity ? 1 : 0, request.exactRangeAllowed ? 1 : 0,
             request.exactRangeFree ? 1 : 0, request.exactRangeUnmapped ? 1 : 0,
             sparseOverlap ? 1 : 0, (unsigned long long)sparseCount,
             guestMmapPlacementName(placement), (unsigned long long)ret,
             (long long)((S64)ret < 0 ? -(S64)ret : 0),
             (unsigned long long)occurrences);
}

// Forward decl for the file-backed path; defined below.
static U64 sys_mmap64_file(CPU64* cpu, U64 addr, U64 length, U64 prot,
                           U64 flags, U64 fd, U64 offset);

static U64 sys_mmap64(CPU64* cpu, U64 addr, U64 length, U64 prot, U64 flags, U64 fd, U64 offset) {
    if (!(flags & K_MAP_ANONYMOUS)) {
        return sys_mmap64_file(cpu, addr, length, prot, flags, fd, offset);
    }
    if (getenv("BW64_MMAPDUMP") && length >= 0x1000000) {
        klog_fmt("MMAP [pid=%d] BIG anon enter addr=0x%llx len=0x%llx prot=0x%x",
                 (int)(cpu->thread ? cpu->thread->process->id : -1),
                 (unsigned long long)addr, (unsigned long long)length, (unsigned)prot);
    }
    // Exhaustively assigned by the placement switch below; seeded so no
    // compiler has to prove that for itself.
    U64 ret = (U64)-K_EINVAL;
    const bool fixed = (flags & K_MAP_FIXED) != 0;
    const bool fixedNoReplace = (flags & K_MAP_FIXED_NOREPLACE) != 0;
    const U64 alignedAddr = addr & ~0xFFFULL;
    const U64 pageCount = (length + 0xFFFULL) >> 12;
    const U64 mapLen = pageCount << 12;
    const bool nativeIdentity = cpu->memory->nativeIdentityMode();

    boxedvn::GuestMmapRequest request;
    request.address = addr ? alignedAddr : 0;
    request.length = mapLen;
    request.protection = (U32)prot;
    request.fixed = fixed;
    request.fixedNoReplace = fixedNoReplace;
    request.nativeIdentity = nativeIdentity;
    request.anonymous = true;
    if (addr != 0) {
        request.exactRangeAllowed =
            !nativeIdentity ||
            cpu->memory->nativeGuestRangeAllowed(alignedAddr, mapLen);
        // "Free" for an ordinary hint means no ACCESSIBLE mapping. It must NOT
        // mean "nothing mapped": wine reserves huge PROT_NONE arenas and then
        // mmaps committed pages at hint addresses inside its own reservation.
        // Treating those reserved pages as occupied would bounce the mapping
        // to an unrelated address and desync wine's view tree.
        const U32 accessible = K64_PAGE_READ | K64_PAGE_WRITE | K64_PAGE_EXEC;
        const U64 pageStart = alignedAddr >> 12;
        bool rangeFree = true;
        for (U64 i = 0; i < pageCount; i++) {
            if (cpu->memory->getPageFlags(pageStart + i) & accessible) {
                rangeFree = false;
                break;
            }
        }
        request.exactRangeFree = rangeFree;
        // MAP_FIXED_NOREPLACE is defined against total occupancy, reservations
        // included, so it needs the stricter question. A sparse reservation
        // occupies its range as completely as a mapped page does, even though
        // it owns no pages, so it has to be asked about separately.
        request.exactRangeUnmapped =
            !fixedNoReplace ||
            (cpu->memory->rangeCompletelyUnmapped(alignedAddr, mapLen) &&
             !cpu->memory->sparseReservationOverlaps(alignedAddr, mapLen));
    }

    const boxedvn::GuestMmapPlacement placement =
        boxedvn::chooseGuestMmapPlacement(request);

    const U64 ordinal =
        gGuestMmapCounters.calls.fetch_add(1, std::memory_order_relaxed);
    if (fixedNoReplace) {
        gGuestMmapCounters.fixedNoReplaceRequests.fetch_add(
            1, std::memory_order_relaxed);
    }
    const bool lowIneligible =
        addr != 0 && nativeIdentity && !request.exactRangeAllowed;
    if (lowIneligible) {
        gGuestMmapCounters.lowIneligibleHints.fetch_add(
            1, std::memory_order_relaxed);
    }

    switch (placement) {
        case boxedvn::GuestMmapPlacement::MapExact:
            if (fixed && nativeIdentity && !request.exactRangeAllowed) {
                // MAP_FIXED outside the window stays a hard refusal in
                // KMemory64; the caller must retry without MAP_FIXED so the
                // loader can relocate into the guarded host window.
                klog_fmt("sys_mmap64: reject native fixed mapping at 0x%llx "
                         "len=0x%llx; caller must relocate",
                         (unsigned long long)addr,
                         (unsigned long long)length);
            }
            ret = cpu->memory->mmapAnonymousFixed(alignedAddr, length,
                                                  (U32)prot);
            if ((S64)ret >= 0) {
                reportGuestTopAlias(cpu, "COMMIT", alignedAddr, mapLen,
                                    (U32)prot);
            }
            break;
        case boxedvn::GuestMmapPlacement::MapExactNoReplace:
            ret = cpu->memory->mmapAnonymousNoReplace(alignedAddr, mapLen,
                                                      (U32)prot);
            if ((S64)ret < 0) {
                gGuestMmapCounters.rejectedWithoutAllocating.fetch_add(
                    1, std::memory_order_relaxed);
            } else {
                reportGuestTopAlias(cpu, "RESERVE", alignedAddr, mapLen,
                                    (U32)prot);
            }
            break;
        case boxedvn::GuestMmapPlacement::RelocateHighWindow:
            if (lowIneligible) {
                gGuestMmapCounters.highWindowRelocations.fetch_add(
                    1, std::memory_order_relaxed);
            }
            ret = cpu->memory->mmapReserveAndMap(length, (U32)prot);
            if ((S64)ret < 0) {
                gGuestMmapCounters.highWindowFailures.fetch_add(
                    1, std::memory_order_relaxed);
            }
            break;
        case boxedvn::GuestMmapPlacement::ReserveSparse:
            // Guest metadata only: no host mapping, no native backing, and no
            // page object per 4 KiB. One interval, whatever the length.
            ret = cpu->memory->reserveSparseNoReplace(alignedAddr, mapLen);
            if ((S64)ret < 0) {
                gGuestMmapCounters.rejectedWithoutAllocating.fetch_add(
                    1, std::memory_order_relaxed);
            } else {
                gGuestMmapCounters.sparseReservations.fetch_add(
                    1, std::memory_order_relaxed);
            }
            break;
        case boxedvn::GuestMmapPlacement::FailExists:
            gGuestMmapCounters.rejectedWithoutAllocating.fetch_add(
                1, std::memory_order_relaxed);
            ret = (U64)-K_EEXIST;
            break;
        case boxedvn::GuestMmapPlacement::FailNoMemory:
            gGuestMmapCounters.rejectedWithoutAllocating.fetch_add(
                1, std::memory_order_relaxed);
            ret = (U64)-K_ENOMEM;
            break;
    }

    // Diagnostics. The budget is per address space, so a helper process
    // cannot spend the evidence belonging to the one that is failing, and the
    // repeat tracker describes a request that keeps coming back no matter how
    // late in the run it starts. Both are bounded; neither restores a line
    // per syscall.
    boxedvn::GuestMmapSignature signature;
    signature.processId = cpu->thread ? cpu->thread->process->id : 0;
    signature.threadId = cpu->thread ? cpu->thread->id : 0;
    signature.addressSpace = cpu->memory->addressSpaceGeneration();
    signature.address = addr;
    signature.length = length;
    signature.protection = (U32)prot;
    signature.flags = (U32)flags;

    const char* reportKind = nullptr;
    U64 occurrences = 0;
    {
        std::lock_guard<std::mutex> guard(gGuestMmapDiagnosticsMutex);
        if (gGuestMmapDetailBudget.take(signature.processId,
                                        signature.addressSpace)) {
            reportKind = "DETAIL";
            occurrences = 1;
        } else {
            const auto outcome = gGuestMmapRepeats.record(signature);
            occurrences = outcome.occurrences;
            switch (outcome.decision) {
                case boxedvn::GuestMmapRepeatTracker::Decision::First:
                    reportKind = "FIRST";
                    break;
                case boxedvn::GuestMmapRepeatTracker::Decision::Repeat:
                    reportKind = "REPEAT";
                    break;
                case boxedvn::GuestMmapRepeatTracker::Decision::Suppress:
                    reportKind = "LAST";
                    break;
                case boxedvn::GuestMmapRepeatTracker::Decision::Silent:
                    break;
            }
        }
    }
    if (reportKind) {
        const bool sparseOverlap =
            addr != 0 && cpu->memory->sparseReservationOverlaps(alignedAddr,
                                                                mapLen);
        reportGuestMmapRequest(cpu, reportKind, signature, request, placement,
                               sparseOverlap,
                               cpu->memory->sparseReservationCount(), ret,
                               occurrences);
    }
    if (guestMmapSummaryDue(ordinal)) {
        reportGuestMmapSummary("milestone");
    }
    (void)fd;
    if (getenv("BW64_MMAPDUMP") && length >= 0x1000000) {
        klog_fmt("MMAP [pid=%d] BIG anon exit  -> 0x%llx",
                 (int)(cpu->thread ? cpu->thread->process->id : -1),
                 (unsigned long long)ret);
    }
    (void)offset;
    return ret;
}

// ---- SysV shared memory (IPC_PRIVATE only) for the 64-bit guest ----
// Wine's unix-side ntdll uses anonymous shm segments to back memory views; the
// Save/Save As common dialog is the first GUI path that hits this. We service
// shmget/shmat/shmdt/shmctl out of the per-process shm64 table (see kprocess.h
// for why per-process is correct). shmat backs the segment with real anonymous
// pages via the normal mmap allocator, so wine sees a properly-mapped view and
// no longer trips create_view. Non-IPC_PRIVATE keys are rejected (-ENOSYS-free
// fallback isn't what wine wants here; it asks for IPC_PRIVATE).
#define K_IPC_PRIVATE   0
#define K_IPC_RMID      0
#define K_IPC_STAT      2
#define K_IPC_64        0x0100
#define K_SHM_RDONLY    010000

static U64 sys_shmget64(CPU64* cpu, U64 key, U64 size, U64 /*shmflg*/) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_EINVAL;
    // Only anonymous segments are supported (what wine requests). A named key
    // would need a system-wide table; wine's view-backing path never uses one.
    if (key != K_IPC_PRIVATE) return (U64)-K_EINVAL;
    KProcess* process = cpu->thread->process.get();
    S32 id = process->nextShm64Id++;
    KProcess::ShmSeg64 seg;
    seg.size = size;
    process->shm64[id] = seg;
    return (U64)(S64)id;
}

static U64 sys_shmat64(CPU64* cpu, U64 shmid, U64 shmaddr, U64 shmflg) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_EINVAL;
    KProcess* process = cpu->thread->process.get();
    auto it = process->shm64.find((S32)shmid);
    if (it == process->shm64.end()) return (U64)-K_EINVAL;
    KProcess::ShmSeg64& seg = it->second;
    U32 prot = (shmflg & K_SHM_RDONLY) ? K_PROT_READ : (K_PROT_READ | K_PROT_WRITE);
    U64 addr;
    if (shmaddr == 0) {
        addr = cpu->memory->mmapReserveAndMap(seg.size, prot);
    } else {
        addr = cpu->memory->mmapAnonymousFixed(shmaddr & ~0xFFFULL, seg.size, prot);
    }
    if ((S64)addr < 0) return addr;          // propagate -errno
    seg.address = addr;
    return addr;
}

static U64 sys_shmdt64(CPU64* cpu, U64 shmaddr) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_EINVAL;
    KProcess* process = cpu->thread->process.get();
    for (auto it = process->shm64.begin(); it != process->shm64.end(); ++it) {
        KProcess::ShmSeg64& seg = it->second;
        if (seg.address && seg.address == shmaddr) {
            cpu->memory->munmap(seg.address, seg.size);
            seg.address = 0;
            // A segment detached after IPC_RMID is destroyed (Linux semantics).
            if (seg.markedForDelete) process->shm64.erase(it);
            return 0;
        }
    }
    return (U64)-K_EINVAL;
}

static U64 sys_shmctl64(CPU64* cpu, U64 shmid, U64 cmd, U64 buf) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_EINVAL;
    KProcess* process = cpu->thread->process.get();
    cmd &= ~K_IPC_64;
    auto it = process->shm64.find((S32)shmid);
    if (it == process->shm64.end()) return (U64)-K_EINVAL;
    KProcess::ShmSeg64& seg = it->second;
    if (cmd == K_IPC_RMID) {
        // Mark for delete; reap now if nothing is attached (matches Linux: RMID
        // on an unattached segment frees it immediately).
        seg.markedForDelete = true;
        if (seg.address == 0) process->shm64.erase(it);
        return 0;
    }
    if (cmd == K_IPC_STAT) {
        if (!buf) return (U64)-K_EFAULT;
        // Minimal struct shmid64_ds: zero it, then fill shm_segsz. Wine's view
        // backing doesn't inspect the rest; zeroing keys/perms is safe.
        cpu->memory->memsetGuest(buf, 0, 112);
        cpu->memory->writeq(buf + 40, seg.size);   // shm_segsz
        return 0;
    }
    return (U64)-K_EINVAL;
}

// exit(2) — terminate the calling thread. If it's the last thread in the
// process, this ends the process (exitgroup). `group` forces process-wide
// termination regardless of thread count (exit_group(2)).
// BW64_SERIAL_TEARDOWN experiment: a global lock serializing the EXECUTION of
// 64-bit process/thread teardown. On real Linux wineserver observes every client
// disconnect + final request through ONE epoll, fully serial. Here the boot
// helpers run on separate HOST threads and exit truly concurrently, so their
// terminate_thread/close requests + socket EOFs interleave at instruction
// granularity — the suspected trigger for wineserver's release_object refcount
// underflow / heap corruption during the post-boot teardown storm. Holding this
// across the whole cleanup (which closes the wineserver socket -> EOF) forces
// wineserver to see one client's full disconnect before the next begins.
// Recursive so an exitgroup that internally drives sibling teardown can't
// self-deadlock. Env-gated so it's a clean A/B with no cost when off.
static std::recursive_mutex g_serialTeardownMutex;
static bool g_serialTeardownInit = false, g_serialTeardownOn = false;
static U64 sys_exit64(CPU64* cpu, U64 status, bool group) {
    if (!g_serialTeardownInit) {
        g_serialTeardownOn = std::getenv("BW64_SERIAL_TEARDOWN") != nullptr;
        g_serialTeardownInit = true;
    }
    if (cpu->thread && cpu->thread->process) {
        KProcess* p = cpu->thread->process.get();
        klog_fmt("CPU64: %s syscall, status=%lld  pid=%d exe='%s' cmd='%s'",
                 group ? "exit_group" : "exit", (long long)status,
                 (int)p->id, p->exe.c_str(), p->commandLine.c_str());
        // The third of the three lifecycle markers, so a child's scheduling,
        // first syscall and exit can be matched up without hunting.
        klog_fmt("BOXEDWINE_X64_PROC_EXIT pid=%u parent=%u status=%lld "
                 "group=%d exe='%s'",
                 (unsigned)p->id, (unsigned)p->parentId, (long long)status,
                 group ? 1 : 0, p->exe.c_str());
    } else {
        klog_fmt("CPU64: %s syscall, status=%lld",
                 group ? "exit_group" : "exit", (long long)status);
    }
    // Before anything is torn down, while the descendants and the register
    // state are still real. Only for a failing exit: a clean one has nothing
    // to explain.
    if (status != 0) {
        dumpX64ExitDiagnostics(cpu, status);
    }
#if defined(BOXEDWINE_DXMT_NATIVE)
    // A process-wide exit ends whatever it was presenting; hide the DXMT
    // layer if it was this process, so the desktop shows through again.
    if (group && cpu->thread && cpu->thread->process) {
        BVNDXMTDisplayNoteProcessExited(cpu->thread->process->id);
    }
#endif
    // Stop this thread's run loop now, whatever else happens below.
    cpu->yield = true;
    if (cpu->thread && cpu->thread->process) {
        KProcess* process = cpu->thread->process.get();
        std::unique_lock<std::recursive_mutex> serialLk;
        if (g_serialTeardownOn) {
            serialLk = std::unique_lock<std::recursive_mutex>(g_serialTeardownMutex);
        }
        if (group) {
            process->exitgroup(cpu->thread, (U32)status);
        } else {
            // KProcess::exit runs cleanup() (which zeroes clear_child_tid64 and
            // futex-wakes the joiner) and terminates this thread; if it's the
            // last thread it falls through to exitgroup and ends the process.
            process->exit(cpu->thread, (U32)status);
        }
    }
    return 0;
}

// A guest process the emulator can no longer run. Routed through exit_group's
// own completion so nothing downstream has to learn a second ending: the pid
// gets a terminal status, the lifecycle marker is emitted, waiters wake, and
// the session sees the process leave.
//
// When the process is the one the session launched -- its parent is the boot
// process -- the emulator is also asked to stop. Otherwise the surviving helper
// processes keep the platform thread count above zero, boxedmain never returns,
// and the UI goes on presenting a loading state for a program that is gone.
void kfatalProcessExit64(CPU64* cpu, U32 status, const char* reason) {
    if (!cpu || !cpu->thread || !cpu->thread->process) {
        klog_fmt("BOXEDWINE_X64_FATAL_EXIT reason='%s' status=%u "
                 "(no live process to end)",
                 reason ? reason : "unknown", (unsigned)status);
        return;
    }
    KProcess* process = cpu->thread->process.get();
    const U32 parentId = process->parentId;
    klog_fmt("BOXEDWINE_X64_FATAL_EXIT pid=%u parent=%u status=%u reason='%s' "
             "exe='%s'",
             (unsigned)process->id, (unsigned)parentId, (unsigned)status,
             reason ? reason : "unknown", process->exe.c_str());
    // The same two markers a guest exit_group emits, so the exit reads the same
    // way to every consumer of the log.
    klog_fmt("CPU64: exit_group syscall, status=%u  pid=%d exe='%s' cmd='%s'",
             (unsigned)status, (int)process->id, process->exe.c_str(),
             process->commandLine.c_str());
    klog_fmt("BOXEDWINE_X64_PROC_EXIT pid=%u parent=%u status=%u group=1 "
             "exe='%s'",
             (unsigned)process->id, (unsigned)parentId, (unsigned)status,
             process->exe.c_str());
    if (status != 0) {
        dumpX64ExitDiagnostics(cpu, status);
    }
#if defined(BOXEDWINE_DXMT_NATIVE)
    BVNDXMTDisplayNoteProcessExited(process->id);
#endif
    cpu->yield = true;
    // The launched program is the process whose parent is the boot process.
    // Its death ends the session; a helper's does not.
    const bool sessionRoot = parentId <= 1;
    process->exitgroup(cpu->thread, status);
    if (sessionRoot) {
        klog_fmt("BOXEDWINE_X64_FATAL_EXIT session root pid=%u ended; "
                 "stopping the emulator", (unsigned)process->id);
        KSystem::shutingDown = true;
        KNativeSystem::postQuit();
    }
}

// struct utsname is 6 × 65-byte fixed strings on x86-64 Linux (390 bytes).
static U64 sys_uname64(CPU64* cpu, U64 bufAddr) {
    if (!bufAddr) return (U64)-K_EFAULT;
    char buf[6 * 65];
    std::memset(buf, 0, sizeof(buf));
    auto setField = [&](int idx, const char* s) {
        std::strncpy(buf + idx * 65, s, 64);
    };
    setField(0, "Linux");                  // sysname
    setField(1, "boxedwine64");            // nodename
    setField(2, "6.1.0-boxedwine");        // release — pretend modern kernel
    setField(3, "#1 SMP boxedwine64");     // version
    setField(4, "x86_64");                 // machine
    setField(5, "(none)");                 // domainname
    cpu->memory->memcpyToGuest(bufAddr, buf, sizeof(buf));
    return 0;
}

// getrandom — use the same host entropy path as the ELF AT_RANDOM vector.
static U64 sys_getrandom64(CPU64* cpu, U64 bufAddr, U64 buflen, U64 /*flags*/) {
    if (buflen == 0) return 0;
    if (!bufAddr) return (U64)-K_EFAULT;
    if (buflen > 256) buflen = 256;
    U8 tmp[256];
    const KRandom::Source source = KRandom::fill(tmp, (size_t)buflen);
    if (source == KRandom::Source::Invalid) return (U64)-K_EFAULT;
    if (source == KRandom::Source::Fallback) {
        static std::atomic<bool> reportedFallback{false};
        if (!reportedFallback.exchange(true, std::memory_order_relaxed)) {
            klog("sys_getrandom64: host entropy unavailable; using emergency fallback");
        }
    }
    cpu->memory->memcpyToGuest(bufAddr, tmp, buflen);
    return buflen;
}

// prlimit64(pid, resource, new_limit*, old_limit*) — for v1 we always pretend
// "no limit" by writing RLIM64_INFINITY to old_limit if requested.
static U64 sys_prlimit64_64(CPU64* cpu, U64 /*pid*/, U64 /*res*/, U64 newLim, U64 oldLim) {
    if (oldLim) {
        // struct rlimit64 { __u64 rlim_cur; __u64 rlim_max; }
        cpu->memory->writeq(oldLim, ~0ULL);
        cpu->memory->writeq(oldLim + 8, ~0ULL);
    }
    (void)newLim;
    return 0;
}

// clock_gettime(clk, struct timespec*). Returns wall-clock from the host so
// glibc gets monotonically advancing values.
static U64 sys_clock_gettime64(CPU64* cpu, U64 /*clk*/, U64 tsAddr) {
    if (!tsAddr) return (U64)-K_EFAULT;
    U64 us = KSystem::getSystemTimeAsMicroSeconds();
    U64 sec = us / 1000000ULL;
    U64 nsec = (us % 1000000ULL) * 1000ULL;
    cpu->memory->writeq(tsAddr, sec);
    cpu->memory->writeq(tsAddr + 8, nsec);
    return 0;
}

// read/write/open/close — wired to the existing 32-bit KProcess FD table via
// a bounce buffer. The kobject->readNative path is host-pointer, so the
// 64-bit guest address never has to flow through the 32-bit memory layer.
// Reads from fd 0 still return 0 (EOF) so apps that probe stdin don't hang.
static U64 sys_read64(CPU64* cpu, U64 fd, U64 buf, U64 count) {
    if (fd == 0) return 0;
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    KFileDescriptorPtr fdesc = cpu->thread->process->getFileDescriptor((FD)fd);
    if (!fdesc) return (U64)-9; // -EBADF
    if (!fdesc->canRead()) return (U64)-K_EINVAL;
    if (count == 0) return 0;
    // Clamp only to a sane ceiling (was a 1MB cap — too small: wine's PE loader
    // reads whole multi-MB sections in one read() and doesn't loop on a short
    // return, so capping left section tails zero-filled -> the HxD 0x937b30 crash).
    if (count > (256ULL << 20)) count = 256ULL << 20;
    std::vector<U8> tmp((size_t)count);
    // Serialize against sys_mmap64_file (and other reads) on the shared per-zip
    // stream — see g_fileReadMutex. A KFile read that repositions a zip's shared
    // unzFile must not interleave with another thread's read/mmap of the same zip.
    // ONLY lock for real files (KFile): a blocking socket/pipe read must NOT hold
    // this global lock or it stalls every other process's file reads (deadlock).
    U32 got;
    {
        std::shared_ptr<KFile> rkfile = std::dynamic_pointer_cast<KFile>(fdesc->kobject);
        if (rkfile) {
            std::lock_guard<std::mutex> lk(g_fileReadMutex);
            // Loop to satisfy the full request: a single zip-backed readNative
            // (unzReadCurrentFile) may return fewer bytes than asked, and the PE
            // loader that issues a whole-section read() does not loop itself.
            // KFiles are seekable regular files, so looping until EOF is correct.
            U64 total = 0;
            got = 0;
            while (total < count) {
                U32 n = fdesc->kobject->readNative(tmp.data() + total, (U32)(count - total));
                if ((S32)n < 0) { got = n; break; }       // propagate error
                if (n == 0) break;                          // EOF
                total += n;
            }
            if ((S32)got >= 0) got = (U32)total;
        } else {
            // Socket/pipe: a short read is correct (blocking semantics) — single shot.
            got = fdesc->kobject->readNative(tmp.data(), (U32)count);
        }
    }
    // Both outcomes of the read land in the socket ring, not just a success.
    // The device's open question is precisely whether Wine's 16-byte reply
    // header read returned 16, 0 or an error, and a recorder that only fires
    // on success cannot answer it. extra0 carries the requested count and
    // extra1 the signed result, so an EOF and an errno are distinguishable
    // from a short read.
    if ((S32)got < 0) {
        crashRingRecordRead('E', (U32)cpu->thread->process->id, (U32)fd,
                            nullptr, 0, (U32)count, (U32)(S32)got);
        return (U64)(S64)(S32)got; // sign-extend kernel errno
    }
    crashRingRecordRead('R', (U32)cpu->thread->process->id, (U32)fd,
                        tmp.data(), got, (U32)count, (U32)got);
    // The reply that hands this process its PE entry point is the last thing
    // that happens before it should start running Windows code. The device
    // shows it arriving intact and the process exiting anyway, so the handoff
    // after it is what needs watching -- and the socket ring's sixteen-byte
    // prefix stops one field short of `suspend`.
    {
        KProcess* replyProcess = cpu->thread->process.get();
        if (!replyProcess->initProcessDoneReported &&
            replyProcess->lastServerRequestOpcode ==
                K_WINE_REQ_INIT_PROCESS_DONE &&
            got == K_WINE_SERVER_MESSAGE_BYTES) {
            const boxedvn::WineInitProcessDoneReply reply =
                boxedvn::decodeWineInitProcessDoneReply(tmp.data(), got);
            if (boxedvn::wineInitProcessDoneAdmitted(reply)) {
                replyProcess->initProcessDoneReported = true;
                klog_fmt("BOXEDWINE_X64_INIT_DONE_REPLY pid=%u error=%u "
                         "reply_size=%u entry=0x%llx suspend=%d "
                         "request_fd=%u reply_fd=%u",
                         (unsigned)replyProcess->id, (unsigned)reply.error,
                         (unsigned)reply.replySize,
                         (unsigned long long)reply.entry, (int)reply.suspend,
                         (unsigned)replyProcess->lastServerRequestFd,
                         (unsigned)fd);
                // Everything after this point is the loader-to-Windows
                // handoff. Arm the translator's bounded block trace for it;
                // the global budget it normally uses is spent hundreds of
                // blocks earlier and showed nothing of this.
#ifdef BOXEDWINE_FEX64_BACKEND
                BVNFEXBackendArmHandoffTrace((unsigned)replyProcess->id);
#endif
            }
        }
    }
    if (got > 0) {
        cpu->memory->memcpyToGuest(buf, tmp.data(), got);
    }
    if (getenv("BW64_IPCDUMP")) {
        // First bytes help identify wineserver reply headers vs pipe wakeup
        // bytes. BW64_IPCWIDE dumps up to 48 bytes so a select_reply's apc_call
        // body (past the 8-byte reply_header) is visible, not just the header.
        int cap = getenv("BW64_IPCWIDE") ? 48 : 16;
        char hex[160] = {0}; int n = (int)((got < (U32)cap) ? got : (U32)cap);
        for (int i = 0; i < n; i++) snprintf(hex + i*3, 4, "%02x ", tmp[i]);
        klog_fmt("IPC [pid=%d] read(fd=%d,count=%llu) -> %d  [%s]",
                 (int)cpu->thread->process->id, (int)fd,
                 (unsigned long long)count, (int)got, hex);
    }
    return (U64)got;
}

// openat(dirfd, path, flags, mode). Reuse KProcess::openat so relative paths
// resolve against a real directory descriptor as well as AT_FDCWD (-100).
#ifndef K_AT_FDCWD
#define K_AT_FDCWD (-100)
#endif
static U64 sys_openat64(CPU64* cpu, U64 dirfd, U64 pathAddr, U64 flags, U64 mode) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    if (!pathAddr) return (U64)-K_EFAULT;
    char path[1024] = {0};
    cpu->memory->memcpyFromGuest(path, pathAddr, sizeof(path) - 1);
    KProcess* process = cpu->thread->process.get();
    // /proc/self/exe (and /proc/<pid>/exe): the kernel exposes the executable as
    // a magic symlink. wine's loader open()s/realpath()s it to find its install
    // dir; redirect to the real backing file so it resolves to the loader path
    // instead of a bogus "/proc"-derived prefix. (readlink/readlinkat handle the
    // symlink-read form separately.)
    BString self = B("/proc/self/exe");
    BString selfPid = B("/proc/") + BString::valueOf(process->id) + B("/exe");
    if (self == path || selfPid == path) {
        std::memset(path, 0, sizeof(path));
        std::strncpy(path, process->exe.c_str(), sizeof(path) - 1);
    }
    // BW64_DLLPATH: trace the loader's DLL/dir search for the failing first
    // process — logs every path that mentions the wine install dir, system32,
    // or a builtin DLL, so we can see WHICH search dirs ntdll constructs (the
    // bootstrap process never probes x86_64-windows -> "could not load kernel32").
    if (getenv("BW64_DLLPATH") && (strstr(path, "wine") || strstr(path, "system32") ||
                                   strstr(path, "x86_64-windows"))) {
        klog_fmt("DLLPATH pid=%d openat('%s') flags=0x%llx", (int)process->id, path,
                 (unsigned long long)flags);
    }
    U32 rc = process->openat((FD)(S32)dirfd, BString::copy(path),
                             (U32)flags, (U32)mode);
    {
        // O_DIRECTORY is what the loader uses to enumerate a candidate module
        // directory, and it is the operation whose result the packaged listing
        // cannot predict, so say which kind of open this was.
        char detail[64];
        snprintf(detail, sizeof(detail), "flags=0x%llx dir=%d",
                 (unsigned long long)flags, (flags & 0x10000) != 0 ? 1 : 0);
        reportDllSearch(process, (flags & 0x10000) != 0 ? "opendir" : "open",
                        path, (long long)(S64)(S32)rc, detail);
    }
    /* The user-driver chain, reported whether the open succeeded or failed.
     *
     * Every other open diagnostic here is either budgeted per process or fires
     * only on failure, so a device run that loaded winex11 successfully after
     * its module-search budget was spent produced no evidence either way. The
     * question "did the X11 driver load at all" is exactly the one a window
     * that cannot be created needs answered, so it gets its own bounded key.
     *
     * First sighting of each distinct path, capped; nothing per repeat. */
    {
        static const char* const kDriverChain[] = {
            "winex11", "libX11.so", "libXext.so", "fonts.conf",
        };
        bool onDriverChain = false;
        for (const char* needle : kDriverChain) {
            if (strstr(path, needle)) { onDriverChain = true; break; }
        }
        if (onDriverChain) {
            U64 chainKey = 1469598103934665603ULL;
            for (const char* cursor = path; *cursor; ++cursor) {
                chainKey = (chainKey ^ (U64)(U8)*cursor) * 1099511628211ULL;
            }
            static boxedvn::BoundedSyscallReportLimiter driverChainReports;
            static std::mutex driverChainMutex;
            boxedvn::BoundedSyscallReportLimiter::Outcome chain;
            {
                std::lock_guard<std::mutex> guard(driverChainMutex);
                chain = driverChainReports.record((U64)process->id, 0, chainKey,
                                                  (U64)(S64)(S32)rc);
            }
            if (chain.decision !=
                boxedvn::BoundedSyscallReportLimiter::Decision::Silent) {
                klog_fmt("BOXEDWINE_X64_USER_DRIVER pid=%u op=open path='%s' "
                         "result=%d seen=%llu",
                         (unsigned)process->id, path, (int)(S32)rc,
                         (unsigned long long)chain.occurrences);
            }
        }
    }
    if ((S32)rc < 0) {
        if (getenv("BW64_DLLTRACE") && (strstr(path, ".dll") || strstr(path, ".exe") ||
                                        strstr(path, "x86_64-windows") || strstr(path, "kernel32"))) {
            klog_fmt("DLLTRACE pid=%d FAIL open('%s') rc=%d cwd='%s'",
                     (int)process->id, path, (int)(S32)rc, process->currentDirectory.c_str());
        } else if (getenv("BW64_SCDUMP")) {
            BString full = Fs::getFullPath(process->currentDirectory, BString::copy(path));
            klog_fmt("sys_openat64: open('%s') -> %d  [cwd='%s' full='%s' flags=0x%llx]",
                     path, (int)(S32)rc, process->currentDirectory.c_str(), full.c_str(),
                     (unsigned long long)flags);
        } else {
            // A guest that reopens one missing path in a loop wrote 468,768
            // identical lines in a single device run. Report the first
            // sighting of each path/errno in full, a few early repeats and
            // then powers of two, and say once that it has gone quiet. A
            // different path is a different key and is still reported. The
            // return value below is untouched: this bounds the diagnostics,
            // not the syscall.
            U64 pathKey = 1469598103934665603ULL;
            for (const char* cursor = path; *cursor; ++cursor) {
                pathKey = (pathKey ^ (U64)(U8)*cursor) * 1099511628211ULL;
            }
            const boxedvn::BoundedSyscallReportLimiter::Outcome failedOpen =
                cpu->failedOpenReports.record(
                    (U64)process->id,
                    cpu->thread ? (U64)cpu->thread->id : 0,
                    pathKey, (U64)(S64)(S32)rc);
            using FailedOpenDecision =
                boxedvn::BoundedSyscallReportLimiter::Decision;
            if (failedOpen.decision == FailedOpenDecision::Detailed) {
                klog_fmt("sys_openat64: open('%s') -> %d", path, (int)(S32)rc);
            } else if (failedOpen.decision == FailedOpenDecision::Repeat) {
                klog_fmt("sys_openat64: open('%s') -> %d — seen %llu times",
                         path, (int)(S32)rc,
                         (unsigned long long)failedOpen.occurrences);
            } else if (failedOpen.decision == FailedOpenDecision::Suppress) {
                klog_fmt("sys_openat64: open('%s') -> %d — seen %llu times; "
                         "further reports for this path are suppressed",
                         path, (int)(S32)rc,
                         (unsigned long long)failedOpen.occurrences);
            }
        }
        return (U64)(S64)(S32)rc;
    }
    KFileDescriptorPtr result = process->getFileDescriptor((FD)rc);
    if (!result) return (U64)-K_EBADF;
    if (getenv("BW64_DLLTRACE") && (strstr(path, ".dll") || strstr(path, ".exe") ||
                                    strstr(path, "x86_64-windows") || strstr(path, "x86_64-unix"))) {
        klog_fmt("DLLTRACE pid=%d OPEN  '%s' -> fd %d", (int)process->id, path, (int)result->handle);
    }
    // BW64_DIRTRACE: log every O_DIRECTORY open (flags&0x10000) so we can see
    // exactly which directories the failing loader enumerates while hunting for
    // kernel32.dll, and whether the open itself succeeds.
    if (getenv("BW64_DIRTRACE") && ((flags & 0x10000) != 0)) {
        klog_fmt("DIRTRACE pid=%d OPENDIR '%s' -> fd %d (flags=0x%llx)",
                 (int)process->id, path, (int)result->handle, (unsigned long long)flags);
    }
    if (getenv("BW64_SCDUMP")) {
        // Log successful opens of the GUI driver modules so we can see whether
        // wine actually loads its x11 backend (winex11.so/.drv, win32u, the
        // display drivers) vs falling back to the null driver.
        if (strstr(path, "winex11") || strstr(path, "win32u") ||
            strstr(path, ".drv")    || strstr(path, "x11")) {
            klog_fmt("sys_openat64: OPENED '%s' -> fd %d", path, (int)result->handle);
        }
    }
    return (U64)result->handle;
}

static U64 sys_close64(CPU64* cpu, U64 fd) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    // BW64_SOCKDTOR: witness guest close() of a connected stream socket so we can
    // tell a LEGITIMATE close(2) (wine really closed the fd) from a SPURIOUS GC
    // (the object destructs while the fd is conceptually still open). Pairs with
    // the SOCKDTOR dtor witness — match the kobject pointer across both.
    if (std::getenv("BW64_SOCKDTOR")) {
        KFileDescriptorPtr fdesc = cpu->thread->process->getFileDescriptor((FD)fd);
        if (fdesc && fdesc->kobject && fdesc->kobject->type == KTYPE_UNIX_SOCKET) {
            // use_count BEFORE the close is the decisive A-vs-B signal:
            //   ==2 (this fdesc->kobject + the local fdesc copy held here) => this
            //        close drops the LAST process ref -> the object destructs now
            //        -> no other process's fd holds it => MODEL A (shared object).
            //   > 2 => another fd table still references the SAME object, so this
            //        close should NOT destruct it; the kill must be the peer-EOF
            //        cascade from the OTHER end's dtor => MODEL B (peer cascade).
            // (We hold one extra ref via `fdesc` here, so subtract it mentally:
            //  raw==2 means exactly one real holder remains = the fd being closed.)
            long uc = fdesc->kobject.use_count();
            KUnixSocketObject* us = dynamic_cast<KUnixSocketObject*>(fdesc->kobject.get());
            void* peer = nullptr;
            int peerIn = -1, peerOut = -1, peerConn = -1;
            int myIn = -1, myOut = -1, myConn = -1;
            if (us) {
                myIn = (int)us->inClosed; myOut = (int)us->outClosed; myConn = (int)us->connected;
                std::shared_ptr<KUnixSocketObject> con = us->connection.lock();
                if (con) {
                    peer = (void*)con.get();
                    peerIn = (int)con->inClosed; peerOut = (int)con->outClosed; peerConn = (int)con->connected;
                }
            }
            klog_fmt("SOCKCLOSE: pid=%d tid=%d close(fd=%llu) kobj=%p use_count=%ld(raw,-1=fdesc) "
                     "self{in=%d out=%d conn=%d} peer=%p peer{in=%d out=%d conn=%d}",
                     (int)cpu->thread->process->id, (int)cpu->thread->id,
                     (unsigned long long)fd, (void*)fdesc->kobject.get(),
                     uc, myIn, myOut, myConn, peer, peerIn, peerOut, peerConn);
        }
    }
    return (U64)(S64)(S32)cpu->thread->process->close((FD)fd);
}

// getdents64(fd, dirp, count) — read directory entries into the guest buffer
// in Linux struct linux_dirent64 layout:
//   u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char d_name[]; (NUL-term)
// each record padded to 8 bytes. Mirrors KProcess::getdents' is64 path but
// writes through KMemory64 (the 32-bit version's dirp is a U32 address). The
// FsOpenNode file pointer tracks how far we've iterated, so successive calls
// page through the directory and a final call returns 0 (end).
static U64 sys_getdents64_real(CPU64* cpu, U64 fd, U64 dirp, U64 count) {
    // NULL buffer is EFAULT regardless of process state (matches the kernel
    // contract and the existing self-test, which runs with no real process).
    if (!dirp) return (U64)-K_EFAULT;
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    bool dt = getenv("BW64_DIRTRACE") != nullptr;
    int dtpid = (int)cpu->thread->process->id;
    KFileDescriptorPtr fdesc = cpu->thread->process->getFileDescriptor((FD)fd);
    if (!fdesc) { if (dt) klog_fmt("DIRTRACE pid=%d getdents64 fd=%llu -> EBADF (no fdesc)", dtpid, (unsigned long long)fd); return (U64)-9; } // -EBADF
    std::shared_ptr<KFile> kfile = std::dynamic_pointer_cast<KFile>(fdesc->kobject);
    if (!kfile || !kfile->openFile || !kfile->openFile->node) { if (dt) klog_fmt("DIRTRACE pid=%d getdents64 fd=%llu -> ENOTDIR (no kfile/openFile/node)", dtpid, (unsigned long long)fd); return (U64)-K_ENOTDIR; }
    FsOpenNode* openNode = kfile->openFile;
    if (!openNode->node->isDirectory()) { if (dt) klog_fmt("DIRTRACE pid=%d getdents64 path='%s' -> ENOTDIR (not a directory)", dtpid, openNode->node->path.c_str()); return (U64)-K_ENOTDIR; }

    U32 entries = openNode->getDirectoryEntryCount();
    U64 len = 0;
    U64 pos = dirp;
    // The question the packaged archive cannot answer: when the loader
    // enumerates a module directory, does the listing the guest gets back
    // actually contain kernel32.dll? Reported once per enumeration, and only
    // while the bounded budget lasts -- the scan below is over an in-memory
    // directory, so it costs nothing the enumeration was not already doing.
    if (cpu->thread->process->dllSearch.isArmed() &&
        openNode->getFilePointer() == 0) {
        bool hasKernel32 = false;
        for (U32 j = 0; j < entries && !hasKernel32; j++) {
            BString entryName;
            std::shared_ptr<FsNode> entryNode =
                openNode->getDirectoryEntry(j, entryName);
            if (entryNode && strcasecmp(entryName.c_str(), "kernel32.dll") == 0) {
                hasKernel32 = true;
            }
        }
        char detail[64];
        snprintf(detail, sizeof(detail), "entries=%u kernel32=%d", entries,
                 hasKernel32 ? 1 : 0);
        reportDllSearch(cpu->thread->process.get(), "getdents",
                        openNode->node->path.c_str(), (long long)entries, detail);
    }
    // BW64_DIRTRACE: log each getdents64 directory's path, entry count, file
    // pointer (resume index), and whether kernel32.dll is among the names. The
    // residual "could not load kernel32.dll" failure loops openat(O_DIRECTORY)+
    // getdents64 over the wine builtin DLL dir — this proves whether the listing
    // actually surfaces kernel32.dll for the failing process.
    if (getenv("BW64_DIRTRACE")) {
        bool hasK32 = false; int found = 0;
        for (U32 j = 0; j < entries; j++) {
            BString nm; std::shared_ptr<FsNode> e = openNode->getDirectoryEntry(j, nm);
            if (!e) continue;
            found++;
            if (strcasecmp(nm.c_str(), "kernel32.dll") == 0) hasK32 = true;
        }
        klog_fmt("DIRTRACE pid=%d path='%s' entries=%u realFound=%d fp=%u kernel32=%d",
                 (int)cpu->thread->process->id,
                 openNode->node->path.c_str(), entries, found,
                 (U32)openNode->getFilePointer(), hasK32 ? 1 : 0);
    }
    for (U32 i = (U32)openNode->getFilePointer(); i < entries; i++) {
        BString name;
        std::shared_ptr<FsNode> entry = openNode->getDirectoryEntry(i, name);
        if (!entry) continue;
        U32 nameLen = (U32)strlen(name.c_str());
        // d_ino(8) d_off(8) d_reclen(2) d_type(1) name(nameLen) NUL(1)
        U32 recordLen = 20 + nameLen;        // 19 header bytes + name + NUL, but
        recordLen = (recordLen + 7) / 8 * 8; // 8-byte aligned (d_off must align)
        if (len + recordLen > count) {
            if (len == 0) return (U64)-K_EINVAL; // buffer too small for one entry
            break;                                // resume here next call
        }
        cpu->memory->writeq(pos,      (U64)entry->id);       // d_ino
        cpu->memory->writeq(pos + 8,  (U64)(i + 1));         // d_off (next pos)
        cpu->memory->writew(pos + 16, (U16)recordLen);       // d_reclen
        cpu->memory->writeb(pos + 18, (U8)entry->getType(true)); // d_type
        cpu->memory->memcpyToGuest(pos + 19, name.c_str(), nameLen + 1); // name+NUL
        pos += recordLen;
        len += recordLen;
        openNode->seek(i + 1);
    }
    return len;
}

// lseek — wired straight to KObject::seek.
static U64 sys_lseek64(CPU64* cpu, U64 fd, U64 offset, U64 whence) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    KFileDescriptorPtr fdesc = cpu->thread->process->getFileDescriptor((FD)fd);
    if (!fdesc) return (U64)-9;
    S64 off = (S64)offset;
    S64 pos;
    if (whence == 0) { // SEEK_SET
        pos = fdesc->kobject->seek(off);
    } else if (whence == 1) { // SEEK_CUR
        pos = fdesc->kobject->getPos() + off;
        pos = fdesc->kobject->seek(pos);
    } else if (whence == 2) { // SEEK_END
        pos = fdesc->kobject->length() + off;
        pos = fdesc->kobject->seek(pos);
    } else {
        return (U64)-K_EINVAL;
    }
    return (U64)pos;
}

// pread64(fd, buf, count, offset) — read at an absolute offset without moving
// the file position. glibc's ld.so uses it to read a DSO's program headers
// before mmap. Emulated as save-pos/seek/read/restore over the KObject layer.
static U64 sys_pread64(CPU64* cpu, U64 fd, U64 buf, U64 count, U64 offset) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    KFileDescriptorPtr fdesc = cpu->thread->process->getFileDescriptor((FD)fd);
    if (!fdesc) return (U64)-9; // -EBADF
    if (!fdesc->canRead()) return (U64)-K_EINVAL;
    if (count == 0) return 0;
    // Do NOT cap `count`: wine's PE loader preads an ENTIRE section in one call
    // (HxD's .text is ~5.3MB) and does NOT loop on a short return, so a small cap
    // left the section tail zero-filled — a function/thread-proc whose code lives
    // past the cap then executed zeros and crashed (the HxD "jump into 0x937b30"
    // bug: the proc was 4.3MB into a .text formerly capped at 1MB). Clamp only to
    // a sane ceiling to avoid a pathological allocation from a bogus count.
    if (count > (256ULL << 20)) count = 256ULL << 20;
    std::vector<U8> tmp((size_t)count);
    // Loop readNative until we've satisfied `count` or hit EOF: a single zip-backed
    // readNative (unzReadCurrentFile) can legally return FEWER bytes than asked,
    // and our single-shot callers (the PE loader) don't loop themselves.
    auto readFully = [&](void) -> S64 {
        S64 savedPos = fdesc->kobject->getPos();
        fdesc->kobject->seek((S64)offset);
        U64 total = 0;
        while (total < count) {
            U32 got = fdesc->kobject->readNative(tmp.data() + total, (U32)(count - total));
            if ((S32)got < 0) { fdesc->kobject->seek(savedPos); return (S64)(S32)got; }
            if (got == 0) break; // EOF
            total += got;
        }
        fdesc->kobject->seek(savedPos);
        return (S64)total;
    };
    // Same zip-stream hazard as sys_mmap64_file / sys_read64: this seek+read+seek
    // is unsynchronized, and wine's PE loader + glibc ld.so pread DLL/DSO headers
    // from the SHARED per-zip stream. Serialize file reads (KFile only — a
    // blocking socket pread must not hold the global lock). Without this, a
    // header pread races a concurrent mmap of the same zip entry and reads the
    // wrong bytes (the residual "could not load kernel32.dll" path after the
    // mmap/read serialization fixed the segment-map corruption).
    std::shared_ptr<KFile> pkfile = std::dynamic_pointer_cast<KFile>(fdesc->kobject);
    S64 got;
    if (pkfile) {
        std::lock_guard<std::mutex> lk(g_fileReadMutex);
        got = readFully();
    } else {
        got = readFully();
    }
    if (got < 0) return (U64)got;
    if (got > 0) {
        cpu->memory->memcpyToGuest(buf, tmp.data(), (U64)got);
    }
    return (U64)got;
}

// pwrite64(fd, buf, count, offset) — write at an absolute offset without moving
// the file position. wineserver uses it to populate registry/config files in
// the prefix (it pwrite's records into the .reg/server files). Emulated as
// save-pos/seek/write/restore over the KObject layer, mirroring sys_pread64.
static U64 sys_pwrite64(CPU64* cpu, U64 fd, U64 buf, U64 count, U64 offset) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    KFileDescriptorPtr fdesc = cpu->thread->process->getFileDescriptor((FD)fd);
    if (!fdesc) return (U64)-9; // -EBADF
    if (!fdesc->canWrite()) return (U64)-K_EINVAL;
    if (count == 0) return 0;
    // Ceiling only (was a 1MB cap that silently truncated large pwrites).
    if (count > (256ULL << 20)) count = 256ULL << 20;
    std::vector<U8> tmp((size_t)count);
    cpu->memory->memcpyFromGuest(tmp.data(), buf, (U32)count);
    S64 savedPos = fdesc->kobject->getPos();
    fdesc->kobject->seek((S64)offset);
    U32 wrote = fdesc->kobject->writeNative(tmp.data(), (U32)count);
    fdesc->kobject->seek(savedPos);
    return (U64)(S64)(S32)wrote;
}

// writeStatBuf64 — write the x86-64 Linux struct stat (144 bytes) into
// guest memory. Field offsets match the canonical glibc/kernel layout for
// __NR_fstat / __NR_newfstatat result buffers on x86-64.
static void writeStatBuf64(KMemory64* mem, U64 addr, U64 size, U32 mode,
                            U64 ino, U32 uid, U32 gid, U64 mtime) {
    U8 buf[144];
    std::memset(buf, 0, sizeof(buf));
    auto put64 = [&](U32 off, U64 v) { std::memcpy(buf + off, &v, 8); };
    auto put32 = [&](U32 off, U32 v) { std::memcpy(buf + off, &v, 4); };
    put64(0,  1);             // st_dev (fake)
    put64(8,  ino);           // st_ino
    put64(16, 1);             // st_nlink
    put32(24, mode);          // st_mode
    put32(28, uid);           // st_uid
    put32(32, gid);           // st_gid
    put32(36, 0);             // __pad0
    put64(40, 0);             // st_rdev
    put64(48, size);          // st_size
    put64(56, 4096);          // st_blksize
    put64(64, (size + 511) / 512); // st_blocks (512-byte units)
    put64(72, mtime);         // st_atime
    put64(80, 0);
    put64(88, mtime);         // st_mtime
    put64(96, 0);
    put64(104, mtime);        // st_ctime
    put64(112, 0);
    mem->memcpyToGuest(addr, buf, sizeof(buf));
}

// Forward decl — sys_newfstatat64 may delegate to sys_fstat64 for AT_EMPTY_PATH.
static U64 sys_fstat64(CPU64* cpu, U64 fd, U64 statbuf);

// Path-based stat shared by stat/lstat/newfstatat. followSymlink controls
// the lstat vs stat distinction (Fs::getNodeFromLocalPath's third arg).
static U64 sys_stat_path64(CPU64* cpu, U64 pathAddr, U64 statbuf, bool followSymlink) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    if (!pathAddr || !statbuf) return (U64)-K_EFAULT;
    char path[1024] = {0};
    cpu->memory->memcpyFromGuest(path, pathAddr, sizeof(path) - 1);
    BString bpath = BString::copy(path);
    std::shared_ptr<FsNode> node = Fs::getNodeFromLocalPath(
        cpu->thread->process->currentDirectory, bpath, followSymlink);
    if (getenv("BW64_SYSTRACE")) {
        klog_fmt("sys_stat_path64: '%s' (cwd='%s') -> %s", path,
                 cpu->thread->process->currentDirectory.c_str(),
                 node ? "OK" : "ENOENT");
    }
    {
        char detail[48];
        snprintf(detail, sizeof(detail), "follow=%d", followSymlink ? 1 : 0);
        reportDllSearch(cpu->thread->process.get(), "stat", path,
                        node ? 0 : -2, detail);
    }
    {
        // Wine's drive map is a stat of dosdevices/<letter>: per drive. My
        // Computer on the 64-bit desktop lists no drive, so the answer each
        // of those stats gets is recorded, bounded.
        static std::atomic<U32> dosDeviceReports {0};
        const size_t pathLength = strlen(path);
        if (pathLength >= 2 && path[pathLength - 1] == ':' &&
            strstr(path, "/dosdevices/") &&
            dosDeviceReports.fetch_add(1, std::memory_order_relaxed) < 64) {
            klog_fmt("BOXEDWINE_X64_DOSDEVICE_STAT pid=%d path='%s' follow=%d found=%d mode=0%o",
                     cpu->thread->process->id, path, followSymlink ? 1 : 0,
                     node ? 1 : 0, node ? node->getMode() : 0);
        }
    }
    if (!node) return (U64)-2; // -ENOENT
    U64 size  = node->length();
    U32 mode  = node->getMode();
    U64 ino   = node->id;
    U64 mtime = node->lastModified() / 1000; // ms → seconds
    writeStatBuf64(cpu->memory, statbuf, size, mode, ino, 1000, 1000, mtime);
    return 0;
}

// newfstatat(dirfd, path, statbuf, flags). AT_EMPTY_PATH (0x1000) means
// "stat the dirfd itself"; AT_SYMLINK_NOFOLLOW (0x100) makes it lstat.
#ifndef K_AT_SYMLINK_NOFOLLOW
#define K_AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef K_AT_EMPTY_PATH
#define K_AT_EMPTY_PATH 0x1000
#endif
static U64 sys_newfstatat64(CPU64* cpu, U64 dirfd, U64 pathAddr, U64 statbuf, U64 flags) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    if (!statbuf) return (U64)-K_EFAULT;
    // AT_EMPTY_PATH with NULL/"" path means stat the fd.
    if ((flags & K_AT_EMPTY_PATH) && (!pathAddr || cpu->memory->readb(pathAddr) == 0)) {
        return sys_fstat64(cpu, dirfd, statbuf);
    }
    // We only honour AT_FDCWD or absolute paths for now (matches openat).
    char path[1024] = {0};
    cpu->memory->memcpyFromGuest(path, pathAddr, sizeof(path) - 1);
    bool isAbs = (path[0] == '/');
    if (getenv("BW64_SYSTRACE")) {
        klog_fmt("sys_newfstatat64: dirfd=%d path='%s' flags=0x%llx",
                 (int)(S32)dirfd, path, (unsigned long long)flags);
    }
    if (!isAbs && (S32)dirfd != K_AT_FDCWD) {
        return (U64)-2;
    }
    return sys_stat_path64(cpu, pathAddr, statbuf, !(flags & K_AT_SYMLINK_NOFOLLOW));
}

static U64 sys_fstat64(CPU64* cpu, U64 fd, U64 statbuf) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    if (!statbuf) return (U64)-K_EFAULT;
    KFileDescriptorPtr fdesc = cpu->thread->process->getFileDescriptor((FD)fd);
    if (!fdesc) return (U64)-9; // -EBADF
    // Reach for the underlying KFile to ask the FsNode for metadata.
    std::shared_ptr<KFile> kfile = std::dynamic_pointer_cast<KFile>(fdesc->kobject);
    U64 size = 0;
    U32 mode = 0100644; // S_IFREG | 0644
    U64 ino = 0;
    U64 mtime = 0;
    if (kfile && kfile->openFile) {
        size = (U64)kfile->openFile->length();
        if (kfile->openFile->node) {
            mode  = kfile->openFile->node->getMode();
            ino   = kfile->openFile->node->id;
            mtime = (U64)kfile->openFile->node->lastModified();
        }
    } else {
        // Non-file kobject (socket/pipe): claim S_IFCHR so callers don't
        // assume seekable.
        mode = 0020666;
    }
    writeStatBuf64(cpu->memory, statbuf, size, mode, ino, 1000, 1000, mtime);
    return 0;
}

// File-backed mmap. Reads the requested file region into freshly mmap'd
// pages. Not lazy/COW — eager copy is simple and bounded for ld-linux's
// typical 200 KiB lib mapping.
static U64 sys_mmap64_file(CPU64* cpu, U64 addr, U64 length, U64 prot,
                           U64 flags, U64 fd, U64 offset) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    KFileDescriptorPtr fdesc = cpu->thread->process->getFileDescriptor((FD)fd);
    if (!fdesc) return (U64)-9; // -EBADF
    std::shared_ptr<KFile> kfile = std::dynamic_pointer_cast<KFile>(fdesc->kobject);
    if (!kfile || !kfile->openFile) {
        return (U64)-K_ENOSYS;
    }
    static std::atomic<U32> fileMapLogCount{0};
    const U32 logOrdinal =
        fileMapLogCount.fetch_add(1, std::memory_order_relaxed);
    // 32 covered the loader's first modules only; a fault seventeen seconds
    // in, at a library mapped during font enumeration, needs the whole run.
    const bool boundedLog = logOrdinal < 512;
    const U64 requestedAddress = addr;
    bool reserved = false;
    const bool fixed = (flags & K_MAP_FIXED) != 0;
    const bool fixedNoReplace = (flags & K_MAP_FIXED_NOREPLACE) != 0;
    const U32 loadProt = (U32)prot | 0x3u;
    if (length == 0 || addr > UINT64_MAX - length) {
        return (U64)-K_EINVAL;
    }
    // Wine passes low preferred-address hints for DSOs. They are relocatable
    // when MAP_FIXED is absent, but cannot be used as host identity addresses
    // on iOS. Relocate those hints through the same bounded allocator used by
    // anonymous mappings; an explicit MAP_FIXED remains a hard failure in
    // KMemory64 rather than risking host VA destruction. MAP_FIXED_NOREPLACE
    // is neither: it demands this exact address and forbids both relocating
    // and replacing, so an unusable or occupied range is EEXIST.
    if (addr != 0 && cpu->memory->nativeIdentityMode()) {
        const U64 hintAligned = addr & ~0xFFFULL;
        const U64 hintOffset = addr - hintAligned;
        if (length > UINT64_MAX - hintOffset - 0xFFFULL) {
            return (U64)-K_EINVAL;
        }
        const U64 hintMapLen = (length + hintOffset + 0xFFFULL) & ~0xFFFULL;
        const bool hintAllowed =
            cpu->memory->nativeGuestRangeAllowed(hintAligned, hintMapLen);
        if (fixedNoReplace) {
            const bool hintUnmapped =
                cpu->memory->rangeCompletelyUnmapped(hintAligned, hintMapLen) &&
                !cpu->memory->sparseReservationOverlaps(hintAligned, hintMapLen);
            if (!hintUnmapped || !hintAllowed) {
                gGuestMmapCounters.rejectedWithoutAllocating.fetch_add(
                    1, std::memory_order_relaxed);
                if (!hintAllowed) {
                    gGuestMmapCounters.lowIneligibleHints.fetch_add(
                        1, std::memory_order_relaxed);
                }
                // Occupied is EEXIST; empty-but-unhostable is ENOMEM. A
                // placement search answers EEXIST by stepping to the next
                // address, so claiming an occupant that is not there turns a
                // single refusal into a walk across the whole region.
                const U64 error =
                    hintUnmapped ? (U64)-K_ENOMEM : (U64)-K_EEXIST;
                // Bounded on the same tracker the anonymous path uses: a
                // refused hint the guest retries is a loop, and a loop must
                // not be narrated line by line.
                boxedvn::GuestMmapSignature signature;
                signature.processId = cpu->thread->process->id;
                signature.threadId = cpu->thread->id;
                signature.addressSpace =
                    cpu->memory->addressSpaceGeneration();
                signature.address = hintAligned;
                signature.length = hintMapLen;
                signature.protection = (U32)prot;
                signature.flags = (U32)flags;
                boxedvn::GuestMmapRepeatTracker::Outcome outcome;
                {
                    std::lock_guard<std::mutex> guard(
                        gGuestMmapDiagnosticsMutex);
                    outcome = gGuestMmapRepeats.record(signature);
                }
                if (outcome.decision !=
                    boxedvn::GuestMmapRepeatTracker::Decision::Silent) {
                    klog_fmt("BOXEDWINE_X64_MMAP_FILE_REFUSE pid=%llu "
                             "tid=%llu space=%llu rip=0x%llx addr=0x%llx "
                             "len=0x%llx prot=0x%x flags=0x%x allowed=%d "
                             "unmapped=%d errno=%lld repeat=%llu",
                             (unsigned long long)signature.processId,
                             (unsigned long long)signature.threadId,
                             (unsigned long long)signature.addressSpace,
                             (unsigned long long)cpu->rip,
                             (unsigned long long)hintAligned,
                             (unsigned long long)hintMapLen, (unsigned)prot,
                             (unsigned)flags, hintAllowed ? 1 : 0,
                             hintUnmapped ? 1 : 0, (long long)-(S64)error,
                             (unsigned long long)outcome.occurrences);
                }
                return error;
            }
        } else if (!fixed && !hintAllowed) {
            gGuestMmapCounters.lowIneligibleHints.fetch_add(
                1, std::memory_order_relaxed);
            gGuestMmapCounters.highWindowRelocations.fetch_add(
                1, std::memory_order_relaxed);
            addr = cpu->memory->mmapReserveAndMap(length, loadProt);
            if ((S64)addr < 0) {
                gGuestMmapCounters.highWindowFailures.fetch_add(
                    1, std::memory_order_relaxed);
                return addr;
            }
            reserved = true;
            klog_fmt("sys_mmap64_file: relocated native hint to 0x%llx",
                     (unsigned long long)addr);
        }
    }
    if (addr == 0) {
        // ld.so's DSO-span reservation (mmap(NULL, span, PROT_NONE, fd)) comes
        // through here; it MUST draw from the same process-wide cursor as
        // anonymous maps so the FIXED sub-segment maps that follow land on
        // disjoint, uncorrupted pages. Atomically reserve+map (closes the
        // MT TOCTOU race two sibling threads hit via the old scan-then-map).
        addr = cpu->memory->mmapReserveAndMap(length, loadProt);
        if ((S64)addr < 0) {
            return addr;
        }
        reserved = true; // pages already mapped under the lock; don't re-map/zero
    }
    U64 aligned = addr & ~0xFFFULL;
    U64 mapLen = (length + (addr - aligned) + 0xFFF) & ~0xFFFULL;
    bool dump = getenv("BW64_FMMAP") != nullptr;
    if (dump || boundedLog) {
        klog_fmt("BOXEDWINE_X64_FILEMAP ordinal=%u pid=%d requested=0x%llx addr=0x%llx aligned=0x%llx len=0x%llx mapLen=0x%llx "
                  "prot=0x%x flags=0x%x off=0x%llx fixed=%d file='%s'",
                  logOrdinal,
                  (int)(cpu->thread ? cpu->thread->process->id : -1),
                  (unsigned long long)requestedAddress,
                  (unsigned long long)addr, (unsigned long long)aligned,
                 (unsigned long long)length, (unsigned long long)mapLen,
                 (unsigned)prot, (unsigned)flags, (unsigned long long)offset,
                 (int)((flags & K_MAP_FIXED) != 0),
                 kfile->openFile->node ? kfile->openFile->node->path.c_str() : "?");
    }
    // MAP_SHARED file mapping that must be GENUINELY shared across every process
    // that maps the same file: wine's server shared-memory section
    // (/run/user/.../wine/server-1-*/* incl. tmpmap-*) — wineserver maps it
    // WRITABLE and writes object/sequence state; the CLIENTS map the SAME file
    // (e.g. KUSER_SHARED_DATA at 0x7ffe0000) and read those writes directly. A
    // private read-copy would give each process a stale snapshot and desync the
    // section. So that file's pages must alias ONE shared backing buffer.
    //
    // CRUCIAL: this aliasing is ONLY correct for files that hold genuinely
    // process-shared state. It is WRONG for PE image sections. wine maps a DLL's
    // read-only sections (.text/.rdata) with MAP_SHARED too, but then loads the
    // image at a PER-PROCESS base and calls relocate_image, which mprotects those
    // sections writable and patches absolute addresses by THIS process's load
    // delta. If two processes share the backing, the second process's relocation
    // overwrites the first's already-relocated bytes (with a different delta), so
    // the first process then reads doubly/wrongly-relocated pointers — garbage
    // IAT/function pointers, a wild indirect call into low memory, and the
    // downstream wineserver "corrupted double-linked list"/release_object crash.
    // So restrict the shared-backing divert to the wineserver server-section
    // files; every other MAP_SHARED file map (PE images, .nls, etc.) falls
    // through to the private per-process read-copy below, which is correct
    // because each process relocates its own copy.
    bool serverSection = false;
    if (kfile->openFile->node) {
        const char* p = kfile->openFile->node->path.c_str();
        // The wine server runtime dir is .../wine/server-<disp>-<inst>/...
        if (strstr(p, "/wine/server-") || strstr(p, "/server-1-") ||
            strstr(p, "tmpmap")) {
            serverSection = true;
        }
    }
    if (serverSection && (flags & K_MAP_SHARED) && kfile->openFile->node && !reserved) {
        // Seed bytes for any page of this file not yet in the registry.
        std::vector<U8> seedBuf((size_t)mapLen, 0);
        U64 fileBase = offset & ~0xFFFULL;
        S64 savedFp = kfile->openFile->getFilePointer();
        kfile->openFile->seek((S64)fileBase);
        kfile->openFile->readNative(seedBuf.data(), (U32)mapLen);
        kfile->openFile->seek(savedFp);
        U64 r = cpu->memory->mmapSharedFile(aligned, mapLen, (U32)prot,
                                            kfile->openFile->node->path.c_str(),
                                            fileBase, seedBuf.data(), (U64)seedBuf.size());
        if ((S64)r < 0) return r;
        if (dump || boundedLog) {
            klog_fmt("BOXEDWINE_X64_FILEMAP_RETURN ordinal=%u result=0x%llx shared=1",
                     logOrdinal, (unsigned long long)addr);
        }
        return addr;
    }
    // The page that contains `addr` may already hold valid bytes from a PRIOR
    // overlapping mapping of the SAME DSO (glibc maps the first PT_LOAD over the
    // whole span, then MAP_FIXED-remaps later segments whose file offset is not
    // page-aligned — so [aligned, addr) is the tail of the previous segment).
    // mmapAnonymousFixed ZEROES whole pages, so a blind zero here wipes that
    // tail and never refills it (we only read file content into [addr, ...)),
    // corrupting live code/data. Preserve the leading partial-page bytes across
    // the (re)map: snapshot [aligned, addr) before, restore after.
    // For a FIXED map (addr supplied by ld.so) the target pages may already hold
    // valid bytes from a PRIOR overlapping mapping of the SAME DSO (glibc maps
    // the first PT_LOAD over the whole span, then MAP_FIXED-remaps later segments
    // whose file offset is not page-aligned — so [aligned, addr) is the tail of
    // the previous segment). mmapAnonymousFixed ZEROES whole pages, so a blind
    // zero wipes that tail and never refills it (we only read file content into
    // [addr, ...)), corrupting live code/data. Preserve the partial-page head &
    // tail across the (re)map. SKIPPED when `reserved`: mmapReserveAndMap already
    // mapped a fresh, disjoint range under the lock — nothing to preserve.
    if (!reserved) {
        U64 head = addr - aligned;
        std::vector<U8> headSave;
        if (head > 0 && cpu->memory->isPageMapped(aligned >> 12)) {
            headSave.resize((size_t)head);
            cpu->memory->memcpyFromGuest(headSave.data(), aligned, head);
            if (dump) klog_fmt("FMMAP   preserving 0x%llx head bytes at 0x%llx",
                               (unsigned long long)head, (unsigned long long)aligned);
        }
        U64 fileEnd = addr + length;
        U64 mapEnd  = aligned + mapLen;
        U64 tail = (mapEnd > fileEnd) ? (mapEnd - fileEnd) : 0;
        std::vector<U8> tailSave;
        if (tail > 0 && cpu->memory->isPageMapped((fileEnd) >> 12)) {
            tailSave.resize((size_t)tail);
            cpu->memory->memcpyFromGuest(tailSave.data(), fileEnd, tail);
        }
        U64 mapped = cpu->memory->mmapAnonymousFixed(aligned, mapLen, loadProt);
        if ((S64)mapped < 0) {
            return mapped;
        }
        if (!headSave.empty()) cpu->memory->memcpyToGuest(aligned, headSave.data(), head);
        if (!tailSave.empty()) cpu->memory->memcpyToGuest(fileEnd, tailSave.data(), tail);
    }
    // Read [offset, offset+length) and copy into [addr, addr+length).
    // MT-CRITICAL: the seek()+readNative()+seek() sequence is THREE separate calls
    // that must not interleave with another thread's file read. For zip-backed
    // files (wine's DLLs live in wine64.zip) every FsZipOpenNode shares ONE
    // underlying decompression stream (FsZip::zipfile) plus its position state;
    // readNative only locks its own body, so when two wine processes mmap the same
    // DLL concurrently on separate host threads their seek/read interleave and
    // each reads from the OTHER's file position — splattering e.g. winex11.drv's
    // MZ header (file offset 0) over its .text/.data/IAT -> corrupted IAT pointers
    // -> a wild indirect call into low memory (RIP=0x10270) -> "could not load
    // kernel32.dll" -> cascading wineserver heap corruption. A process-global lock
    // across the whole seek+read+restore makes each file-mmap read atomic. DLL
    // loading is not perf-critical, so the serialization cost is acceptable.
    std::vector<U8> buf((size_t)length);
    U32 got;
    {
        std::lock_guard<std::mutex> lk(g_fileReadMutex);
        S64 saved = kfile->openFile->getFilePointer();
        kfile->openFile->seek((S64)offset);
        // Loop: a single zip-backed readNative (unzReadCurrentFile) can return
        // fewer bytes than `length`. A single-shot read here would leave the tail
        // of a large section (e.g. a multi-MB .text) zero-filled — the same class
        // of bug as the capped pread64 that produced the HxD 0x937b30 crash.
        U64 total = 0;
        while (total < length) {
            U32 n = kfile->openFile->readNative(buf.data() + total, (U32)(length - total));
            if (n == 0) break; // EOF / no progress
            total += n;
        }
        got = (U32)total;
        kfile->openFile->seek(saved);
    }
    // BW64_MAPWATCH=0xADDR: log every file-mmap that lands in [ADDR, ADDR+0x20000)
    // with its file offset — to catch a segment being (re)mapped from the WRONG
    // file offset (e.g. offset 0 = the MZ header splattered over .text/.data/IAT).
    {
        static const char* mw = std::getenv("BW64_MAPWATCH");
        if (mw) {
            U64 wa = std::strtoull(mw, nullptr, 0);
            if (addr < wa + 0x20000 && addr + length > wa) {
                U64 f8 = 0; if (got >= 8) std::memcpy(&f8, buf.data(), 8);
                klog_fmt("MAPWATCH pid=%d addr=0x%llx len=0x%llx offset=0x%llx prot=0x%x flags=0x%x got=%u first8=0x%llx file='%s'",
                         (int)cpu->thread->process->id, (unsigned long long)addr,
                         (unsigned long long)length, (unsigned long long)offset, (unsigned)prot,
                         (unsigned)flags, got, (unsigned long long)f8,
                         kfile->openFile->node ? kfile->openFile->node->path.c_str() : "?");
            }
        }
    }
    if (got > 0) {
        cpu->memory->memcpyToGuest(addr, buf.data(), got);
    }
    U64 protectedAddress = cpu->memory->mprotect(aligned, mapLen, (U32)prot);
    if (protectedAddress != aligned) {
        return protectedAddress;
    }
    if (dump || boundedLog) {
        klog_fmt("BOXEDWINE_X64_FILEMAP_RETURN ordinal=%u result=0x%llx bytes=%u shared=0",
                 logOrdinal, (unsigned long long)addr, got);
    }
    (void)flags;
    return addr;
}

// writev — iterate the iovec array, calling write per segment. Each iovec
// entry on x86-64 is { u64 base; u64 len }.
static U64 sys_writev64(CPU64* cpu, U64 fd, U64 iov, U64 iovcnt) {
    U64 total = 0;
    for (U64 i = 0; i < iovcnt; i++) {
        U64 base = cpu->memory->readq(iov + i * 16 + 0);
        U64 len  = cpu->memory->readq(iov + i * 16 + 8);
        if (len == 0) continue;
        S64 wrote = (S64)sys_write64(cpu, fd, base, len);
        if (wrote < 0) {
            return total > 0 ? total : (U64)wrote;
        }
        total += (U64)wrote;
        if ((U64)wrote < len) break; // short write
    }
    return total;
}

// readv(fd, iov, iovcnt) — scatter read. Mirror sys_writev64: walk the
// 64-bit iovec array (struct iovec is {void* base; size_t len} = 16 bytes
// on x86-64) and read into each segment via sys_read64. Stop on the first
// short read (fewer bytes than the segment asked for) — that's EOF / no
// more data right now, same as the kernel. Returns total bytes read, or
// the first segment's error if nothing was read. wine/wineserver read
// server replies through readv, so a missing impl silently breaks the IPC
// reply path (was -ENOSYS).
static U64 sys_readv64(CPU64* cpu, U64 fd, U64 iov, U64 iovcnt) {
    U64 total = 0;
    for (U64 i = 0; i < iovcnt; i++) {
        U64 base = cpu->memory->readq(iov + i * 16 + 0);
        U64 len  = cpu->memory->readq(iov + i * 16 + 8);
        if (len == 0) continue;
        S64 got = (S64)sys_read64(cpu, fd, base, len);
        if (got < 0) {
            return total > 0 ? total : (U64)got;
        }
        total += (U64)got;
        if ((U64)got < len) break; // short read: EOF / drained
    }
    return total;
}

// Read a NUL-terminated guest C-string at addr from 64-bit memory. Bounded so a
// missing terminator can't loop forever; argv/env strings and paths are well
// under this. Reads in small chunks to avoid a huge fixed stack buffer.
static BString readGuestString64(CPU64* cpu, U64 addr) {
    if (!addr) return BString();
    std::string s;
    const U64 MAX = 64 * 1024;
    while (s.size() < MAX) {
        char chunk[256];
        cpu->memory->memcpyFromGuest(chunk, addr, sizeof(chunk));
        U32 n = 0;
        for (; n < sizeof(chunk); n++) {
            if (chunk[n] == 0) { s.append(chunk, n); return BString::copy(s.c_str()); }
        }
        s.append(chunk, sizeof(chunk));
        addr += sizeof(chunk);
    }
    return BString::copy(s.c_str());
}

// Walk a NULL-terminated array of 64-bit guest pointers (argv / envp) and read
// each pointed-to C-string. The 64-bit ABI passes 8-byte pointers (unlike the
// 32-bit execve's 4-byte readd walk).
static void readStringArray64(CPU64* cpu, U64 arrayAddr, std::vector<BString>& out) {
    if (!arrayAddr) return;
    const U32 MAX_ENTRIES = 4096;
    for (U32 i = 0; i < MAX_ENTRIES; i++) {
        U64 p = cpu->memory->readq(arrayAddr);
        if (!p) break;
        arrayAddr += 8;
        out.push_back(readGuestString64(cpu, p));
    }
}

// execve(path, argv, envp) for 64-bit guests. wine64 re-execs itself and spawns
// wineserver64 through this. Marshals the 8-byte-pointer argv/envp arrays out of
// memory64, then drives the shared KProcess::execve, which resets memory and
// re-runs the loader (ElfLoader::loadProgram routes ELF64 -> loadProgram64,
// rebuilding memory64/cpu64 fresh — see KProcess::execve's 64-bit reset). On
// success execve never returns to the caller (the image is replaced); it returns
// -K_CONTINUE which the dispatcher must NOT write into RAX.
static U64 sys_execve64(CPU64* cpu, U64 pathAddr, U64 argvAddr, U64 envpAddr) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    if (!pathAddr) return (U64)-K_EFAULT;
    BString path = readGuestString64(cpu, pathAddr);
    std::vector<BString> args;
    std::vector<BString> envs;
    readStringArray64(cpu, argvAddr, args);
    readStringArray64(cpu, envpAddr, envs);
    // Boot reliability (M18): wine64 auto-spawns a BARE wineserver at boot
    // (argc==1) which races the launcher's later `wineserver64 -p` pin for the
    // server lock. When the bare/transient instance wins (a coin flip), the
    // session's wineserver is non-persistent and shuts down in the first
    // zero-client moment of the slow interpreted boot (the wineboot-chain ->
    // app handoff), cleanly killing explorer and the never-mapped app —
    // ~1-in-2 cold boots died this way. Make every bare wineserver spawn
    // persistent, so whichever instance wins the lock keeps the session alive.
    // Flagged spawns (-p pin, -k, -w) are left untouched (argc > 1).
    if (args.size() == 1 &&
        (path.endsWith("/wineserver64") || path.endsWith("/wineserver"))) {
        args.push_back(B("-p"));
        klog_fmt("sys_execve64: bare wineserver spawn made persistent (-p) for boot reliability");
    }
    klog_fmt("sys_execve64: pid=%d path='%s' argv0='%s' argc=%d envc=%d",
             (int)(cpu->thread && cpu->thread->process ? cpu->thread->process->id : -1),
             path.c_str(),
             args.empty() ? "" : args[0].c_str(), (int)args.size(), (int)envs.size());
    // Full argv dump — invaluable for telling which wine subprocess (wineboot/
    // services.exe/rpcss/plugplay/explorer) is being launched in the re-exec
    // chain; the bare argv0 ('/usr/lib/wine/wine64') is identical for all of them.
    if (getenv("BW64_SYSTRACE") || getenv("BW64_EXECDUMP")) {
        for (size_t i = 1; i < args.size(); i++)
            klog_fmt("sys_execve64:   argv[%d]='%s'", (int)i, args[i].c_str());
    }
    for (auto& e : envs) {
        if (strncmp(e.c_str(), "WINELOADER=", 11) == 0)
            klog_fmt("sys_execve64:   env %s", e.c_str());
        if (getenv("BW64_ENVDUMP"))
            klog_fmt("sys_execve64:   ENV %s", e.c_str());
    }
    return (U64)(S64)(S32)cpu->thread->process->execve(cpu->thread, path, args, envs);
}

// readlink(path, buf, bufsize) — resolve a symlink. /proc/self/exe is the
// big-ticket caller for glibc startup; everything else falls back to the
// FsNode link field if present.
static U64 sys_readlink64(CPU64* cpu, U64 pathAddr, U64 buf, U64 sz) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    if (!pathAddr || !buf || sz == 0) return (U64)-K_EFAULT;
    char path[1024] = {0};
    cpu->memory->memcpyFromGuest(path, pathAddr, sizeof(path) - 1);
    KProcess* proc = cpu->thread->process.get();
    BString pidExe = B("/proc/") + BString::valueOf(proc->id) + B("/exe");
    BString resolved;
    // The kernel's /proc magic symlinks. glibc/wine's realpath() walks
    // /proc/self/exe component-by-component, so we must resolve BOTH the
    // intermediate /proc/self (-> the numeric pid dir) and the terminal
    // .../exe (-> the executable's real path). Without /proc/self resolving,
    // realpath bails and wine derives a bogus "/proc"-based install dir.
    if (std::strcmp(path, "/proc/self") == 0 || std::strcmp(path, "/proc/thread-self") == 0) {
        resolved = BString::valueOf(proc->id);                 // relative target, like Linux
    } else if (std::strcmp(path, "/proc/self/exe") == 0 ||
               std::strcmp(path, "/proc/thread-self/exe") == 0 ||
               pidExe == path) {
        resolved = proc->exe;
    } else {
        std::shared_ptr<FsNode> n = Fs::getNodeFromLocalPath(
            proc->currentDirectory, BString::copy(path), false);
        if (!n || !n->isLink()) return (U64)-22; // -EINVAL on non-symlink
        resolved = n->link;
    }
    U64 toCopy = (U64)resolved.length();
    if (toCopy > sz) toCopy = sz;
    cpu->memory->memcpyToGuest(buf, resolved.c_str(), toCopy);
    return toCopy;
}

// Futex with real per-address waiter bookkeeping.
//
// A futex on a scheduled thread is handled by KThread::futex64, which parks
// and wakes for real; everything from here down is the THREADLESS fallback
// (--x64-run-elf and the selftests), where nothing can block, so WAIT returns
// -EAGAIN even when the word matches. That fallback DOES count would-block
// events per address in cpu->futexWaiters, and WAKE drains up to `val` of
// them and
// returns the count actually drained — matching the kernel's "number
// woken" semantics. The difference from the old stub:
//
//   - A WAKE that follows a WAIT-on-match now returns 1, not 0, which
//     lets glibc's condvar fast path see that its broadcast wasn't a
//     no-op (the old behaviour caused some __pthread_cond_signal paths
//     to loop forever calling FUTEX_WAKE expecting >0).
//   - Waiter counts are addressable per `uaddr`, so WAKE on one futex
//     does not spuriously claim to wake waiters on another.
//
// Storage is freed automatically when count drops to 0 (erase from map).
// Map lives on CPU64 so it's process-scoped — exactly what FUTEX_PRIVATE
// expects. When multi-thread support lands, the map moves to KProcess64
// with no shape change.
//
//   WAIT / WAIT_BITSET (+ _PRIVATE): read 32-bit word at uaddr; if
//       != val return -EAGAIN immediately. If == val, bump waiter count
//       and ALSO return -EAGAIN (would-block placeholder). The bumped
//       count is consumed by the next WAKE on the same address.
//   WAKE / WAKE_BITSET (+ _PRIVATE): drain min(val, count) from the
//       waiter slot for uaddr and return that drain count.
//   REQUEUE / CMP_REQUEUE / WAKE_OP: 0 (no kernel-side migration yet).
//   Anything else: -ENOSYS so glibc takes the user-space fallback.
//
// uaddr==0 → -EFAULT for all ops (matches kernel).
static U64 sys_futex64(CPU64* cpu, U64 uaddr, U32 op, U32 val, U64 timeoutAddr, U32 val3) {
    if (uaddr == 0) return (U64)-K_EFAULT;

    // Real path: when this CPU64 is driven by a scheduled KThread inside a
    // process (the only way multiple threads exist), delegate to the shared
    // futex table so WAIT actually blocks and WAKE actually wakes siblings.
    if (cpu->thread && cpu->thread->process && cpu->thread->process->memory64) {
        S64 r = cpu->thread->futex64(uaddr, op, val, timeoutAddr, val3);
        if (r == K_FUTEX64_PARKED) {
            // Single-threaded cooperative park: don't write RAX, rewind the
            // SYSCALL on yield so it re-runs after we're woken.
            cpu->reExecuteSyscall = true;
            cpu->yield = true;
            return cpu->reg[X64_RAX].u64; // unused; RAX left intact
        }
        return (U64)r;
    }

    // Threadless fallback for --x64-run-elf / selftest: no real thread to
    // park, so keep the would-block bookkeeping stub (records WAIT-on-match
    // so a later WAKE reports a non-zero count, letting glibc's condvar fast
    // path make forward progress single-threaded).
    U32 baseOp = op & ~(X64_FUTEX_PRIVATE_FLAG | X64_FUTEX_CLOCK_REALTIME);
    switch (baseOp) {
        case X64_FUTEX_WAKE:
        case X64_FUTEX_WAKE_BITSET: {
            auto it = cpu->futexWaiters.find(uaddr);
            if (it == cpu->futexWaiters.end()) return 0;
            U32 toDrain = it->second < val ? it->second : val;
            it->second -= toDrain;
            if (it->second == 0) cpu->futexWaiters.erase(it);
            return toDrain;
        }
        case X64_FUTEX_WAIT:
        case X64_FUTEX_WAIT_BITSET: {
            if (!cpu->memory) return (U64)-K_EFAULT;
            U32 cur = cpu->memory->readd(uaddr);
            if (cur != val) return (U64)-K_EAGAIN;
            cpu->futexWaiters[uaddr] += 1;
            return (U64)-K_EAGAIN;
        }
        case X64_FUTEX_REQUEUE:
        case X64_FUTEX_CMP_REQUEUE:
        case X64_FUTEX_WAKE_OP:
            return 0;
        default:
            return (U64)-K_ENOSYS;
    }
}

// rt_sigtimedwait(set, info, timeout, sigsetsize) — block till a signal in
// `set` arrives or `timeout` elapses. With no delivery implemented, the
// only correct non-hanging answer is -EAGAIN (timeout). Validate sigsetsize
// for the standard 8-byte mask first.
static U64 sys_rt_sigtimedwait64(CPU64* /*cpu*/, U64 setPtr, U64 /*infoPtr*/,
                                  U64 /*timeoutPtr*/, U64 sigsetsize) {
    if (sigsetsize != 8) return (U64)-K_EINVAL;
    if (setPtr == 0) return (U64)-K_EFAULT;
    // No signals pending; timeout result.
    return (U64)-K_EAGAIN;
}

// rt_sigsuspend(mask, sigsetsize) — replace mask, sleep till signal, restore.
// With no delivery, returning -EINTR forces glibc to retry the surrounding
// loop (correct for poll/select-on-signal idioms).
static U64 sys_rt_sigsuspend64(CPU64* /*cpu*/, U64 maskPtr, U64 sigsetsize) {
    if (sigsetsize != 8) return (U64)-K_EINVAL;
    if (maskPtr == 0) return (U64)-K_EFAULT;
    return (U64)-K_EINTR;
}

// rt_sigpending(set, sigsetsize) — write currently-pending signal mask.
// Nothing is ever pending in our world; write zeros and succeed.
static U64 sys_rt_sigpending64(CPU64* cpu, U64 setPtr, U64 sigsetsize) {
    if (sigsetsize != 8) return (U64)-K_EINVAL;
    if (setPtr == 0) return (U64)-K_EFAULT;
    if (!cpu->memory) return (U64)-K_EFAULT;
    cpu->memory->writeq(setPtr, 0);
    return 0;
}

// rt_sigaction(2) — storage-only round-trip. We do not yet deliver signals
// to guest handlers (that needs the signal-frame builder of Milestone B),
// but glibc's startup *registers* SIGFPE/SIGSEGV/SIGPIPE handlers very
// early and later queries them; previously this was a bald no-op so the
// second sigaction(SIG, NULL, &old) call always reported SIG_DFL, which
// confuses libpthread's "did the user install a handler?" check.
//
// x86-64 `struct kernel_sigaction` layout (32 bytes):
//   off  0: sa_handler  (8)
//   off  8: sa_flags    (8)
//   off 16: sa_restorer (8)
//   off 24: sa_mask     (8, = full sigset_t on x86-64)
//
// Signal numbers 1..64 are valid; SIGKILL(9) and SIGSTOP(19) can be queried
// but cannot have their handlers changed — we accept the read and silently
// drop the write, matching kernel behaviour.
static U64 sys_rt_sigaction64(CPU64* cpu, U64 sig, U64 actPtr, U64 oldActPtr,
                              U64 sigsetsize) {
    if (sig < 1 || sig > 64) return (U64)-K_EINVAL;
    if (sigsetsize != 8) return (U64)-K_EINVAL; // x86-64 sigset_t is 8 bytes
    if (!cpu->memory) return (U64)-K_EFAULT;

    CPU64::SigAction& slot = cpu->sigActions[sig];

    if (oldActPtr) {
        cpu->memory->writeq(oldActPtr + 0,  slot.installed ? slot.handler  : 0);
        cpu->memory->writeq(oldActPtr + 8,  slot.installed ? slot.flags    : 0);
        cpu->memory->writeq(oldActPtr + 16, slot.installed ? slot.restorer : 0);
        cpu->memory->writeq(oldActPtr + 24, slot.installed ? slot.mask     : 0);
    }

    if (actPtr) {
        if (sig == 9 || sig == 19) {
            // SIGKILL / SIGSTOP: read accepted, write ignored — matches Linux.
            return 0;
        }
        slot.handler  = cpu->memory->readq(actPtr + 0);
        slot.flags    = cpu->memory->readq(actPtr + 8);
        slot.restorer = cpu->memory->readq(actPtr + 16);
        slot.mask     = cpu->memory->readq(actPtr + 24);
        slot.installed = true;
    }
    return 0;
}

// rt_sigprocmask(2) — storage-only round-trip, paired with rt_sigaction
// above. how=SIG_BLOCK(0)/SIG_UNBLOCK(1)/SIG_SETMASK(2). v1 enforces nothing
// at delivery time (no delivery yet) but lets pthread_sigmask round-trip
// without losing state, which libpthread queries during thread init.
#define X64_SIG_BLOCK   0
#define X64_SIG_UNBLOCK 1
#define X64_SIG_SETMASK 2

static U64 sys_rt_sigprocmask64(CPU64* cpu, U64 how, U64 setPtr, U64 oldSetPtr,
                                U64 sigsetsize) {
    if (sigsetsize != 8) return (U64)-K_EINVAL;
    if (!cpu->memory) return (U64)-K_EFAULT;

    if (oldSetPtr) {
        cpu->memory->writeq(oldSetPtr, cpu->sigMask);
    }
    if (setPtr) {
        U64 incoming = cpu->memory->readq(setPtr);
        // SIGKILL(9) and SIGSTOP(19) can never be blocked — strip them
        // from any incoming mask to match kernel behaviour.
        U64 kernelStrip = (1ULL << (9 - 1)) | (1ULL << (19 - 1));
        incoming &= ~kernelStrip;
        switch (how) {
            case X64_SIG_BLOCK:   cpu->sigMask |=  incoming; break;
            case X64_SIG_UNBLOCK: cpu->sigMask &= ~incoming; break;
            case X64_SIG_SETMASK: cpu->sigMask  =  incoming; break;
            default: return (U64)-K_EINVAL;
        }
    }
    return 0;
}

// sigaltstack(2) — storage-only round-trip, completes the sig{action,
// procmask, altstack} trio. Layout of stack_t on x86-64 (24 bytes):
//   off  0: ss_sp    (8)
//   off  8: ss_flags (4)
//   off 12: pad      (4)
//   off 16: ss_size  (8)
//
// Kernel rules we honour here:
//   - If oldss != NULL, write the current state first.
//   - If ss != NULL with SS_DISABLE(2): clear the registration.
//   - If ss != NULL otherwise: ss_flags must be 0 (or SS_AUTODISARM=0x80000000)
//     and ss_size must be >= MINSIGSTKSZ (~2048). Reject otherwise.
//   - Cannot change altstack while currently executing on it (SS_ONSTACK
//     flag set). We don't track that yet, so the check is skipped.
#define X64_SS_ONSTACK     1
#define X64_SS_DISABLE     2
#define X64_SS_AUTODISARM  0x80000000u
#define X64_MINSIGSTKSZ    2048

static U64 sys_sigaltstack64(CPU64* cpu, U64 ssPtr, U64 oldSsPtr) {
    if (!cpu->memory) return (U64)-K_EFAULT;

    if (oldSsPtr) {
        cpu->memory->writeq(oldSsPtr + 0,  cpu->sigAltStack.ssSp);
        cpu->memory->writed(oldSsPtr + 8,  cpu->sigAltStack.ssFlags);
        cpu->memory->writed(oldSsPtr + 12, 0);
        cpu->memory->writeq(oldSsPtr + 16, cpu->sigAltStack.ssSize);
    }

    if (ssPtr) {
        U64 sp    = cpu->memory->readq(ssPtr + 0);
        U32 flags = cpu->memory->readd(ssPtr + 8);
        U64 size  = cpu->memory->readq(ssPtr + 16);

        if (flags & X64_SS_DISABLE) {
            cpu->sigAltStack.ssSp    = 0;
            cpu->sigAltStack.ssFlags = X64_SS_DISABLE;
            cpu->sigAltStack.ssSize  = 0;
        } else {
            U32 allowed = X64_SS_AUTODISARM;
            if (flags & ~allowed) return (U64)-K_EINVAL;
            if (size < X64_MINSIGSTKSZ) return (U64)-K_ENOMEM;
            cpu->sigAltStack.ssSp    = sp;
            cpu->sigAltStack.ssFlags = flags;
            cpu->sigAltStack.ssSize  = size;
        }
    }
    return 0;
}

// sched_getaffinity(2): glibc + libgomp probe this very early to decide thread
// pool sizes. We expose exactly one CPU. The Linux signature is unusual:
//   ssize_t sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask)
// On success returns the number of bytes the kernel actually wrote (clamped
// to cpusetsize, but must be a multiple of sizeof(long)=8). cpusetsize must
// be a multiple of sizeof(long) and >= 8.
//
// We write 8 bytes (one U64 with bit 0 set) and return 8. Userspace then
// counts the bits to get nproc, which is exactly what we want it to see.
static U64 sys_sched_getaffinity64(CPU64* cpu, U64 pid, U64 cpusetsize, U64 maskPtr) {
    (void)pid; // ignore — we model one process
    if (cpusetsize == 0 || (cpusetsize & 7)) return (U64)-K_EINVAL;
    if (!maskPtr) return (U64)-K_EFAULT;
    if (!cpu->memory) return (U64)-K_EFAULT;

    // Write 1 in the low qword (CPU 0 is in our affinity set), zero the rest
    // up to cpusetsize. Userspace inspects only the bytes the kernel wrote
    // (which is our return value), so 8 bytes is enough.
    cpu->memory->writeq(maskPtr, 1);
    for (U64 off = 8; off < cpusetsize && off < 1024; off += 8) {
        cpu->memory->writeq(maskPtr + off, 0);
    }
    return 8;
}

// sched_setaffinity(2): silently accept any mask — we don't actually move
// the thread anywhere, but reject obviously bogus calls.
static U64 sys_sched_setaffinity64(CPU64* cpu, U64 pid, U64 cpusetsize, U64 maskPtr) {
    (void)cpu; (void)pid; (void)maskPtr;
    if (cpusetsize == 0 || (cpusetsize & 7)) return (U64)-K_EINVAL;
    return 0;
}

// statfs(2) / fstatfs(2) — fixed-shape stub. Layout of x86-64 struct statfs
// (120 bytes, all fields long-sized):
//   off  0: f_type        off 56: f_fsid (8)
//   off  8: f_bsize       off 64: f_namelen
//   off 16: f_blocks      off 72: f_frsize
//   off 24: f_bfree       off 80: f_flags
//   off 32: f_bavail      off 88..119: f_spare[4]
//   off 40: f_files
//   off 48: f_ffree
//
// glibc's dynamic loader probes statfs("/proc"), the system's ld.so.cache
// directory, etc. The load-bearing fields are f_type (to know "is this
// tmpfs/proc/etc?"), f_bsize (cache alignment), and f_namelen (path
// canonicalisation). We claim tmpfs (0x01021994) with 4096-byte blocks
// and 255-byte filenames, zero free/total — glibc tolerates the lie.
#define X64_TMPFS_MAGIC 0x01021994

static U64 sys_statfs64_common(CPU64* cpu, U64 bufPtr) {
    if (!bufPtr) return (U64)-K_EFAULT;
    if (!cpu->memory) return (U64)-K_EFAULT;
    cpu->memory->writeq(bufPtr +  0, X64_TMPFS_MAGIC); // f_type
    cpu->memory->writeq(bufPtr +  8, 4096);            // f_bsize
    cpu->memory->writeq(bufPtr + 16, 0);               // f_blocks
    cpu->memory->writeq(bufPtr + 24, 0);               // f_bfree
    cpu->memory->writeq(bufPtr + 32, 0);               // f_bavail
    cpu->memory->writeq(bufPtr + 40, 0);               // f_files
    cpu->memory->writeq(bufPtr + 48, 0);               // f_ffree
    cpu->memory->writeq(bufPtr + 56, 0);               // f_fsid
    cpu->memory->writeq(bufPtr + 64, 255);             // f_namelen
    cpu->memory->writeq(bufPtr + 72, 4096);            // f_frsize
    cpu->memory->writeq(bufPtr + 80, 0);               // f_flags
    cpu->memory->writeq(bufPtr + 88, 0);
    cpu->memory->writeq(bufPtr + 96, 0);
    cpu->memory->writeq(bufPtr + 104, 0);
    cpu->memory->writeq(bufPtr + 112, 0);
    return 0;
}

// ============================================================================
// Signal-frame layout (Linux x86-64 rt_sigframe / ucontext_t).
// ============================================================================
//
// When the kernel delivers a signal it pushes this structure on the user
// stack and sets RIP = handler, RDI = signo, RSI = &siginfo, RDX = &uctx.
// rt_sigreturn(15) reverses it: read the saved cpu state out of the frame
// at the current RSP and restore.
//
// We use the Linux-glibc layout so any handler compiled for x86-64 Linux
// can read it directly. Field offsets and sizes:
//
//   off  +0   rt_sigframe header                       (8 bytes pretty + pad)
//             — actually just the saved restorer addr (8 bytes), kernel
//               relies on the handler to RET into it; we always set RIP
//               from the saved RIP in mcontext on sigreturn so the header
//               is informational only.
//   off  +8   siginfo_t (128 bytes)
//   off +136  ucontext_t
//      +0     uc_flags          (8)
//      +8     uc_link           (8)
//      +16    uc_stack          (24: ss_sp, ss_flags+pad, ss_size)
//      +40    uc_mcontext.gregs[23]  — see gregs index table below
//                                       (23 * 8 = 184 bytes)
//      +224   uc_mcontext.fpregs (8 — pointer; we set 0 → no FPU restore)
//      +232   uc_mcontext.__reserved[8]  (64 bytes — zeroed)
//      +296   uc_sigmask        (8 — single qword, rest of 128-byte sigset
//                                    region is zero-padded)
//      +424   __fpregs_mem      (we don't write this for now)
//
// Total frame size we allocate on the stack: 16-aligned, round up to 1024.
// (Linux's actual frame is ~600 bytes with fpregs_mem; we shrink because
// we never populate fpregs.)
//
// gregs[] index, matching <sys/ucontext.h> REG_* enum:
//   0:R8   1:R9   2:R10  3:R11  4:R12  5:R13  6:R14  7:R15
//   8:RDI  9:RSI 10:RBP 11:RBX 12:RDX 13:RAX 14:RCX 15:RSP
//  16:RIP 17:EFL 18:CSGSFS 19:ERR 20:TRAPNO 21:OLDMASK 22:CR2
#define X64_SIGFRAME_SIZE          1024
#define X64_UCONTEXT_OFF_IN_FRAME  136   // after siginfo
#define X64_MCONTEXT_OFF_IN_UCTX    40
#define X64_GREGS_OFF_IN_UCTX       X64_MCONTEXT_OFF_IN_UCTX
#define X64_SIGMASK_OFF_IN_UCTX    296

// Indices into mcontext.gregs[23].
enum X64Greg {
    X64_GREG_R8 = 0, X64_GREG_R9, X64_GREG_R10, X64_GREG_R11,
    X64_GREG_R12, X64_GREG_R13, X64_GREG_R14, X64_GREG_R15,
    X64_GREG_RDI, X64_GREG_RSI, X64_GREG_RBP, X64_GREG_RBX,
    X64_GREG_RDX, X64_GREG_RAX, X64_GREG_RCX, X64_GREG_RSP,
    X64_GREG_RIP, X64_GREG_EFL, X64_GREG_CSGSFS, X64_GREG_ERR,
    X64_GREG_TRAPNO, X64_GREG_OLDMASK, X64_GREG_CR2,
};

// Build a signal frame on the stack at `framePtr` capturing cpu's full
// gpr/rflags/rip/sigmask state. Caller has already aligned the stack and
// reserved X64_SIGFRAME_SIZE bytes. Returns the address of the ucontext_t
// within the frame (handler receives this as RDX).
static U64 buildSignalFrame(CPU64* cpu, U64 framePtr) {
    // Zero the entire frame first (siginfo, padding, gaps).
    for (U64 i = 0; i < X64_SIGFRAME_SIZE; i += 8) {
        cpu->memory->writeq(framePtr + i, 0);
    }
    U64 uctxPtr   = framePtr + X64_UCONTEXT_OFF_IN_FRAME;
    U64 mctxPtr   = uctxPtr  + X64_MCONTEXT_OFF_IN_UCTX;
    U64 gregsPtr  = mctxPtr; // gregs starts at mcontext
    // uc_stack: copy the registered altstack so the handler can re-arm if needed.
    cpu->memory->writeq(uctxPtr + 16, cpu->sigAltStack.ssSp);
    cpu->memory->writed(uctxPtr + 24, cpu->sigAltStack.ssFlags);
    cpu->memory->writeq(uctxPtr + 32, cpu->sigAltStack.ssSize);
    // gregs[]
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_R8,  cpu->reg[X64_R8].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_R9,  cpu->reg[X64_R9].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_R10, cpu->reg[X64_R10].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_R11, cpu->reg[X64_R11].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_R12, cpu->reg[X64_R12].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_R13, cpu->reg[X64_R13].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_R14, cpu->reg[X64_R14].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_R15, cpu->reg[X64_R15].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RDI, cpu->reg[X64_RDI].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RSI, cpu->reg[X64_RSI].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RBP, cpu->reg[X64_RBP].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RBX, cpu->reg[X64_RBX].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RDX, cpu->reg[X64_RDX].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RAX, cpu->reg[X64_RAX].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RCX, cpu->reg[X64_RCX].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RSP, cpu->reg[X64_RSP].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RIP, cpu->rip);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_EFL, (U64)cpu->rflags);
    // CSGSFS: low 16 = CS, next 16 = GS, next 16 = FS, top 16 = SS.
    //
    // The interrupted CS and SS are real now, and the pair is what
    // rt_sigreturn restores. Linux writes zero for GS and FS on x86-64
    // (arch/x86/kernel/signal_64.c) and this matches it, so a handler that
    // reads the frame sees the same shape it does on the host kernel --
    // Wine's own save_context/restore_context on this architecture take
    // SegCs from here, which is how a WoW64 exception returns to 32-bit
    // code.
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_CSGSFS,
                        (U64)cpu->seg.cs | ((U64)cpu->seg.ss << 48));
    // ERR/TRAPNO/CR2 = 0 (no fault triggered this delivery). A hardware-fault
    // delivery overwrites these afterwards via the fault-info it has.
    // OLDMASK = caller's sigmask before this delivery (we set it to current)
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_OLDMASK, cpu->sigMask);
    // fpregs pointer = 0 (we don't snapshot XMM/x87 here yet — the next
    // increment after delivery wiring lands)
    cpu->memory->writeq(mctxPtr + 184, 0);
    // uc_sigmask
    cpu->memory->writeq(uctxPtr + X64_SIGMASK_OFF_IN_UCTX, cpu->sigMask);
    return uctxPtr;
}

// The code segment a signal handler runs in.
//
// On this architecture a signal handler always enters in the 64-bit code
// segment: the processor takes CS from the descriptor the kernel names, and
// Linux names __USER_CS unconditionally (arch/x86/kernel/signal_64.c). The
// interrupted CS is what the frame carries, and rt_sigreturn puts it back.
//
// Under WoW64 that is not a formality. A thread executing 32-bit code has the
// 32-bit code selector loaded, the translator takes each block's decode width
// from the L bit of the descriptor that selector names, and a 64-bit handler
// entered with it still loaded is decoded as 32-bit code. A device run showed
// exactly that: after the far jump into 32-bit ntdll, a fault in 32-bit code
// was delivered to Wine's 64-bit handler with cs=0x23 still set, the
// handler's first byte `ab` decoded as `stosd`, it stored through a null EDI,
// and the same fault repeated for the rest of the run.
//
// Inert for a thread that never leaves the 64-bit segment: its CS already is
// the value this would set, and nothing is logged or changed.
static std::atomic<U32> gSignalSegmentSwitchReports {0};
static const U32 kSignalSegmentSwitchReports = 32;

static void reportSignalSegmentSwitch(CPU64* cpu, const char* what, U16 fromCS,
                                      U16 toCS) {
    if (gSignalSegmentSwitchReports.fetch_add(1, std::memory_order_relaxed) >=
        kSignalSegmentSwitchReports) {
        return;
    }
    klog_fmt("BOXEDWINE_X64_SIGNAL_CS %s pid=%d tid=%d from_cs=0x%x to_cs=0x%x "
             "rip=0x%llx",
             what,
             (int)(cpu->thread ? cpu->thread->process->id : -1),
             (int)(cpu->thread ? cpu->thread->id : -1),
             (unsigned)fromCS, (unsigned)toCS,
             (unsigned long long)cpu->rip);
}

// Enter the handler in the 64-bit code segment. Call AFTER buildSignalFrame,
// so the frame records the segments that were interrupted.
static void enterSignalHandlerSegments(CPU64* cpu) {
    if (cpu->seg.cs == K_WINE_X64_CODE_SELECTOR) {
        return;
    }
    const U16 fromCS = cpu->seg.cs;
    cpu->seg.cs = K_WINE_X64_CODE_SELECTOR;
    cpu->seg.ss = K_WINE_X64_DATA_SELECTOR;
    reportSignalSegmentSwitch(cpu, "deliver", fromCS, cpu->seg.cs);
}

// The reverse, from what the frame holds. Linux's restore_sigcontext takes CS
// and SS out of the frame here (forcing CPL 3) and ignores the GS and FS
// slots, which is why the FS base is not recomputed: it is owned by the
// selector-write trap and by arch_prctl, and Wine's signal handlers reload it
// themselves on this path. A frame with no CS recorded is left alone rather
// than forced to a selector it never had.
static void restoreSignalFrameSegments(CPU64* cpu, U64 csgsfs) {
    const U16 savedCS = (U16)(csgsfs & 0xffffu);
    const U16 savedSS = (U16)((csgsfs >> 48) & 0xffffu);
    if (savedCS == 0) {
        return;
    }
    const U16 fromCS = cpu->seg.cs;
    cpu->seg.cs = (U16)(savedCS | 3u);
    if (savedSS != 0) {
        cpu->seg.ss = (U16)(savedSS | 3u);
    }
    if (cpu->seg.cs != fromCS) {
        reportSignalSegmentSwitch(cpu, "sigreturn", fromCS, cpu->seg.cs);
    }
}

// Restore cpu state from a signal frame whose ucontext_t lives at the
// current RSP. The kernel's invariant after rt_sigreturn is that RSP
// points one byte *past* the saved-rsp slot (i.e. the frame has been
// popped); we read everything we need, then write cpu->rip and cpu->reg
// values en masse. mcontext lives at RSP + (X64_MCONTEXT_OFF_IN_UCTX).
//
// Returns 0 on success, negative errno otherwise. The kernel actually
// returns the saved RAX as the syscall result so the user code sees the
// pre-signal RAX restored; we mirror that by writing RAX last and
// signalling "skip the normal RAX-as-return-value path" via a special
// sentinel (the caller checks cpu->yield).
static U64 restoreSignalFrame(CPU64* cpu, U64 uctxPtr) {
    U64 mctxPtr   = uctxPtr + X64_MCONTEXT_OFF_IN_UCTX;
    U64 gregsPtr  = mctxPtr;
    cpu->reg[X64_R8].u64  = cpu->memory->readq(gregsPtr + 8 * X64_GREG_R8);
    cpu->reg[X64_R9].u64  = cpu->memory->readq(gregsPtr + 8 * X64_GREG_R9);
    cpu->reg[X64_R10].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_R10);
    cpu->reg[X64_R11].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_R11);
    cpu->reg[X64_R12].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_R12);
    cpu->reg[X64_R13].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_R13);
    cpu->reg[X64_R14].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_R14);
    cpu->reg[X64_R15].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_R15);
    cpu->reg[X64_RDI].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RDI);
    cpu->reg[X64_RSI].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RSI);
    cpu->reg[X64_RBP].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RBP);
    cpu->reg[X64_RBX].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RBX);
    cpu->reg[X64_RDX].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RDX);
    cpu->reg[X64_RCX].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RCX);
    cpu->reg[X64_RSP].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RSP);
    cpu->rip              = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RIP);
    cpu->rflags           = (U32)cpu->memory->readq(gregsPtr + 8 * X64_GREG_EFL);
    cpu->sigMask          = cpu->memory->readq(uctxPtr   + X64_SIGMASK_OFF_IN_UCTX);
    // Resume in the code segment the frame names. For a thread that never
    // left the 64-bit segment this restores what it already had; for a WoW64
    // thread it is how execution returns to 32-bit code, both after a fault
    // taken there and after Wine's own handler rewrites the frame to continue
    // somewhere else in 32-bit code.
    restoreSignalFrameSegments(
        cpu, cpu->memory->readq(gregsPtr + 8 * X64_GREG_CSGSFS));
    // RAX is restored last — the kernel returns RAX as the syscall return
    // value so the user-visible RAX is the *pre-signal* value, not whatever
    // rt_sigreturn would compute.
    U64 savedRax = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RAX);
    return savedRax;
}

// SA_ONSTACK = 0x08000000 in glibc's <bits/sigaction.h>. When set, the
// handler runs on the registered sigaltstack instead of the user stack.
#define X64_SA_ONSTACK   0x08000000

// Synchronously deliver `sig` to `cpu` (the same thread). Models the
// kernel's "queue a signal that's pending immediately" path for the
// single-thread case — i.e. raise(), abort(), pthread_kill(self,...).
//
// Caller has already validated the signal number and confirmed that
// cpu->sigActions[sig].installed && handler is a real function pointer
// (not SIG_DFL/SIG_IGN).
//
// Mechanics:
//   1. Pick the frame stack: altstack if SA_ONSTACK set and altstack is
//      live, otherwise current RSP minus a redzone (128 bytes per SysV).
//   2. Round down to 16-byte alignment, subtract frame size.
//   3. Build the ucontext_t at frame_base via buildSignalFrame.
//   4. Set cpu->reg[X64_RSP] = frame_base, RIP = handler, RDI = sig,
//      RSI = 0 (no siginfo), RDX = &uctx, R10 = 0, R8/R9 = 0.
//   5. Mask the signal in cpu->sigMask for the duration of handler
//      execution (kernel adds sig to mask unless SA_NODEFER set).
//   6. The handler ends with `ret`-to-restorer or `syscall(rt_sigreturn)`,
//      which (a) hits our rt_sigreturn case → restoreSignalFrame → state
//      restored, including sigMask.
//
// Returns true on successful delivery (handler will run next), false if
// the handler slot is not installed / is SIG_DFL/SIG_IGN.
static bool deliverSignalSync(CPU64* cpu, U32 sig) {
    if (sig < 1 || sig > 64) return false;
    CPU64::SigAction& sa = cpu->sigActions[sig];
    if (!sa.installed) return false;
    if (sa.handler == 0 /*SIG_DFL*/ || sa.handler == 1 /*SIG_IGN*/) return false;

    // Pick stack: SA_ONSTACK requires a non-DISABLED altstack.
    U64 baseSp;
    if ((sa.flags & X64_SA_ONSTACK) && cpu->sigAltStack.ssSp != 0 &&
        (cpu->sigAltStack.ssFlags & 2 /*SS_DISABLE*/) == 0) {
        baseSp = cpu->sigAltStack.ssSp + cpu->sigAltStack.ssSize;
    } else {
        baseSp = cpu->reg[X64_RSP].u64 - 128; // red zone
    }
    // 16-align then reserve the frame.
    U64 frameBase = (baseSp - X64_SIGFRAME_SIZE) & ~(U64)15;

    U64 uctxPtr = buildSignalFrame(cpu, frameBase);
    enterSignalHandlerSegments(cpu);

    // x86-64 signal-frame ABI: the kernel pushes the restorer address *below*
    // the ucontext so the handler's terminating `ret` pops it and jumps to
    // the restorer. The restorer then issues `syscall rt_sigreturn` with RSP
    // pointing at the ucontext (== uctxPtr). Without this, the handler's
    // `ret` pops the first qword of ucontext as garbage and crashes.
    U64 retSlot = uctxPtr - 8;
    cpu->memory->writeq(retSlot, sa.restorer);

    // Mask the signal during handler execution (unless SA_NODEFER=0x40000000).
    if ((sa.flags & 0x40000000) == 0) {
        cpu->sigMask |= (1ULL << (sig - 1));
    }
    cpu->sigMask |= sa.mask;

    // Hand off to the handler. RSP points at restorer_addr so `ret` pops it
    // and leaves RSP = uctxPtr, which is what rt_sigreturn expects.
    cpu->reg[X64_RSP].setU64(retSlot);
    cpu->reg[X64_RDI].setU64(sig);      // arg1: signal number
    cpu->reg[X64_RSI].setU64(0);        // arg2: siginfo (we don't synth one)
    cpu->reg[X64_RDX].setU64(uctxPtr);  // arg3: ucontext pointer
    cpu->rip = sa.handler;

    return true;
}

// Deliver one pending, unmasked, installed async signal queued on this thread by
// a cross-thread tkill/tgkill. Called by the scheduler at the start of the
// thread's slice (the thread is parked, so building a frame on its CPU64 is
// safe). At most one per call — the handler runs, and any further pending
// signals are taken on subsequent slices (after rt_sigreturn unmasks). Returns
// true if a signal was delivered.
bool CPU64::deliverPendingSignals() {
    if (!this->thread) return false;
    U64 pending;
    {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->thread->pendingSignalsMutex);
        pending = this->thread->pendingSignals & ~this->sigMask;
    }
    if (!pending) return false;
    for (U32 sig = 1; sig <= 64; sig++) {
        U64 bit = 1ULL << (sig - 1);
        if (!(pending & bit)) continue;
        // Only consume the bit if we can actually deliver it (a handler is
        // installed). If it's SIG_DFL/IGN/uninstalled, leave it pending — for
        // the wineserver SIGUSR1-APC case the handler is always installed by the
        // time wineserver signals the client; an undeliverable signal shouldn't
        // be silently dropped here.
        if (deliverSignalSync(this, sig)) {
            BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->thread->pendingSignalsMutex);
            this->thread->pendingSignals &= ~bit;
            return true;
        }
    }
    return false;
}

// Synchronously deliver a hardware-trap-derived signal (SIGFPE on #DE,
// SIGSEGV on a page fault, ...) at the faulting instruction. Unlike
// deliverSignalSync (used for raise()/tgkill self), this synthesizes a real
// siginfo and fills the mcontext TRAPNO/ERR/CR2 so wine's (and glibc's)
// signal handlers see a genuine fault — wine's setup_raise_exception maps
// SIGFPE + si_code → EXCEPTION_INT_DIVIDE_BY_ZERO etc, and uses the ucontext
// RIP as the exception address. cpu->rip must still point at the faulting
// instruction when this is called (so the captured RIP is the fault site).
bool CPU64::raiseSyncFault(U32 sig, U32 trapNo, S32 siCode, U64 faultAddr) {
    if (sig < 1 || sig > 64) return false;
    SigAction& sa = this->sigActions[sig];
    if (!sa.installed) return false;
    if (sa.handler == 0 /*SIG_DFL*/ || sa.handler == 1 /*SIG_IGN*/) return false;

    U64 baseSp;
    if ((sa.flags & X64_SA_ONSTACK) && this->sigAltStack.ssSp != 0 &&
        (this->sigAltStack.ssFlags & 2 /*SS_DISABLE*/) == 0) {
        baseSp = this->sigAltStack.ssSp + this->sigAltStack.ssSize;
    } else {
        baseSp = this->reg[X64_RSP].u64 - 128; // red zone
    }
    U64 frameBase = (baseSp - X64_SIGFRAME_SIZE) & ~(U64)15;

    U64 uctxPtr = buildSignalFrame(this, frameBase);
    enterSignalHandlerSegments(this);

    // Fill the fault-specific mcontext slots the generic builder leaves zero.
    U64 gregsPtr = uctxPtr + X64_MCONTEXT_OFF_IN_UCTX;
    this->memory->writeq(gregsPtr + 8 * X64_GREG_TRAPNO, (U64)trapNo);
    this->memory->writeq(gregsPtr + 8 * X64_GREG_ERR, 0);
    this->memory->writeq(gregsPtr + 8 * X64_GREG_CR2, faultAddr);

    // Synthesize a siginfo_t at the start of the frame (the siginfo region sits
    // before the ucontext at X64_UCONTEXT_OFF_IN_FRAME). Layout (x86-64):
    //   int si_signo @0; int si_errno @4; int si_code @8; then the union,
    //   where the SIGFPE/SIGSEGV variant places void* si_addr @16.
    U64 siPtr = frameBase;
    this->memory->writed(siPtr + 0, sig);
    this->memory->writed(siPtr + 4, 0);
    this->memory->writed(siPtr + 8, (U32)siCode);
    this->memory->writeq(siPtr + 16, faultAddr);

    U64 retSlot = uctxPtr - 8;
    this->memory->writeq(retSlot, sa.restorer);

    if ((sa.flags & 0x40000000) == 0) {
        this->sigMask |= (1ULL << (sig - 1));
    }
    this->sigMask |= sa.mask;

    this->reg[X64_RSP].setU64(retSlot);
    this->reg[X64_RDI].setU64(sig);        // arg1: signal number
    this->reg[X64_RSI].setU64(siPtr);      // arg2: siginfo*
    this->reg[X64_RDX].setU64(uctxPtr);    // arg3: ucontext*
    this->rip = sa.handler;
    return true;
}

// --- 64-bit socket address bounce -------------------------------------------
// The socket object layer (kbind/kconnect/kaccept/kgetsockname and their
// KSocketObject methods) reads the guest sockaddr through `thread->memory` —
// the 32-bit KMemory. For a 64-bit process that memory is allocated but unused
// (the real address space is memory64), so the sockaddr the guest passed lives
// in memory64, not where the object layer looks. sockaddr_un/_in are pointer-
// free (identical layout 32/64), so rather than duplicate every object method
// we BOUNCE: copy the sockaddr bytes out of memory64 into a small scratch page
// in the 32-bit KMemory, then call the existing 32-bit k* socket helper with
// that scratch address. This reuses all the bind/connect/accept/handshake logic
// unchanged. The scratch page is mmap'd once per process (lazily) and reused.
//
// Returns the 32-bit scratch address holding `len` bytes copied from `src64`,
// or 0 on failure. `outLenAddr` (optional) gets a 4-byte slot holding `len`
// for the helpers that take a pointer-to-socklen_t.
// The scratch must be PER THREAD, not per process: each KProcess has its own
// 32-bit KMemory (so an address mmap'd in one process is unmapped in another —
// the forked wine64 client vs. wineserver), AND sibling THREADS of one process
// share that KMemory but run concurrently. A single per-process scratch is
// clobbered when two threads do socket calls at once (services.exe spawns a
// clone3 worker thread that hammers wineserver alongside the main thread) —
// the result is a malformed request that corrupts wineserver's heap ("unsorted
// double linked list"). So cache (threadId -> addr) and mmap a fresh region the
// first time each thread needs one. KThread::id is unique and stable.
static std::unordered_map<U32, U32> g_socketScratchByThread;
static BOXEDWINE_MUTEX g_scratchMutex;
static U32 bounceSockaddrTo32(CPU64* cpu, U64 src64, U32 len, U32* outLenAddr) {
    KThread* thread = cpu->thread;
    if (!thread || !thread->memory || !thread->process) return 0;
    if (len > 256) len = 256; // sockaddr_un is 110 bytes; cap defensively
    U32 addr;
    {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(g_scratchMutex);
        auto it = g_socketScratchByThread.find(thread->id);
        if (it != g_socketScratchByThread.end()) {
            addr = it->second;
        } else {
            // One page is plenty for a sockaddr + a socklen_t slot.
            addr = thread->memory->mmap(thread, 0, K_PAGE_SIZE,
                K_PROT_READ | K_PROT_WRITE, K_MAP_ANONYMOUS | K_MAP_PRIVATE, -1, 0);
            if (!addr) return 0;
            g_socketScratchByThread[thread->id] = addr;
        }
    }
    if (src64 && len) {
        U8 tmp[256];
        cpu->memory->memcpyFromGuest(tmp, src64, len);
        thread->memory->memcpy(addr, tmp, len);
    }
    if (outLenAddr) {
        U32 lenSlot = addr + 256;
        thread->memory->writed(lenSlot, len);
        *outLenAddr = lenSlot;
    }
    return addr;
}

// Ensure a multi-page scratch region in the 64-bit process's (otherwise unused)
// 32-bit KMemory, big enough for a synthesized 32-bit msghdr + iovec + cmsg +
// a data buffer. Same per-process caching rationale as bounceSockaddrTo32.
// Layout within the region:
//   +0      : 32-bit msghdr (28 bytes: name,namelen,iov,iovlen,control,controllen,flags)
//   +32     : one 32-bit iovec {base,len}
//   +64     : cmsg area (up to a few SCM_RIGHTS fds)
//   +256    : socklen/len scratch
//   +512 .. : data buffer (region size - 512)
#define MSG_SCRATCH_BYTES   (64 * 1024)
#define MSG_SCRATCH_HDR     0
#define MSG_SCRATCH_IOV     32
#define MSG_SCRATCH_CMSG    64
#define MSG_SCRATCH_DATA    512
// Per-THREAD (see bounceSockaddrTo32's note): a per-process msg scratch is
// corrupted when sibling threads sendmsg/recvmsg concurrently.
static std::unordered_map<U32, U32> g_msgScratchByThread;
static U32 msgScratch(KThread* thread) {
    if (!thread || !thread->memory || !thread->process) return 0;
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(g_scratchMutex);
    auto it = g_msgScratchByThread.find(thread->id);
    if (it != g_msgScratchByThread.end()) return it->second;
    U32 addr = thread->memory->mmap(thread, 0, MSG_SCRATCH_BYTES,
        K_PROT_READ | K_PROT_WRITE, K_MAP_ANONYMOUS | K_MAP_PRIVATE, -1, 0);
    if (!addr) return 0;
    g_msgScratchByThread[thread->id] = addr;
    return addr;
}

// Invalidate the cached per-thread 32-bit IPC scratch addresses for a thread.
// MUST be called whenever that thread's 32-bit KMemory is wiped — i.e. on
// execve (execvReset frees the process's 32-bit mappings, including the scratch
// page). Without this the cache hands back a stale address that is no longer
// mapped, and the next sockaddr/msg bounce faults in KMemory::performOnMemory
// ("failed to get ram"). This surfaced once the persistent-session spawn path
// ran a guest through preloader->wine64 execs on a non-main worker: the scratch
// was mmap'd before an execve and reused after it. See KProcess::execve.
void bw64InvalidateScratchForThread(U32 threadId) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(g_scratchMutex);
    g_socketScratchByThread.erase(threadId);
    g_msgScratchByThread.erase(threadId);
}

// 64-bit struct offsets (x86-64):
//   msghdr {name(0); namelen(8); iov(16); iovlen(24); control(32); controllen(40); flags(48)}  =56
//   iovec  {base(0); len(8)}                                                                    =16
//   cmsghdr{len(0,size_t); level(8,int); type(12,int); data(16)}
// 32-bit (what the socket object expects):
//   msghdr {name(0); namelen(4); iov(8); iovlen(12); control(16); controllen(20); flags(24)}
//   iovec  {base(0); len(4)}
//   cmsghdr{len(0); level(4); type(8); data(12)}  -- 16-byte stride in the object code

// sendmsg(fd, msghdr*, flags) for a 64-bit guest. Gathers the scatter buffers
// into one scratch blob, translates any SCM_RIGHTS cmsg, builds a 32-bit msghdr
// in scratch, and calls the 32-bit ksendmsg (reusing all the AF_UNIX queue +
// fd-passing logic).
static U64 sys_sendmsg64(CPU64* cpu, U64 fd, U64 msg64, U64 flags) {
    KThread* thread = cpu->thread;
    if (!thread || !thread->process) return (U64)-K_ENOSYS;
    U32 base = msgScratch(thread);
    if (!base) return (U64)-K_EFAULT;
    KMemory* m32 = thread->memory;

    U64 iov     = cpu->memory->readq(msg64 + 16);
    U64 iovlen  = cpu->memory->readq(msg64 + 24);
    U64 control = cpu->memory->readq(msg64 + 32);
    U64 ctllen  = cpu->memory->readq(msg64 + 40);

    // Gather all iov segments into the scratch data buffer.
    U32 dataAddr = base + MSG_SCRATCH_DATA;
    U32 total = 0;
    U32 dataCap = MSG_SCRATCH_BYTES - MSG_SCRATCH_DATA;
    for (U64 i = 0; i < iovlen; i++) {
        U64 b = cpu->memory->readq(iov + 16 * i);
        U64 l = cpu->memory->readq(iov + 16 * i + 8);
        if (l == 0) continue;
        if (total + l > dataCap) l = dataCap - total;
        if (l == 0) break;
        std::vector<U8> tmp((size_t)l);
        cpu->memory->memcpyFromGuest(tmp.data(), b, (U32)l);
        m32->memcpy(dataAddr + total, tmp.data(), (U32)l);
        total += (U32)l;
    }

    // Build the 32-bit msghdr: a single iovec covering the gathered blob.
    m32->writed(base + MSG_SCRATCH_IOV + 0, dataAddr);
    m32->writed(base + MSG_SCRATCH_IOV + 4, total);
    m32->writed(base + MSG_SCRATCH_HDR + 0, 0);                    // msg_name
    m32->writed(base + MSG_SCRATCH_HDR + 4, 0);                    // msg_namelen
    m32->writed(base + MSG_SCRATCH_HDR + 8, base + MSG_SCRATCH_IOV); // msg_iov
    m32->writed(base + MSG_SCRATCH_HDR + 12, total ? 1 : 0);       // msg_iovlen
    U32 ctl32 = 0, ctllen32 = 0;
    if (control && ctllen >= 16) {
        // Translate SCM_RIGHTS: 64-bit cmsg {len(0),level(8),type(12),fds(16..)}.
        // The object's recv side reads 16-byte cmsg records with the fd at +12,
        // i.e. one fd per 16 bytes. Emit that compact form into the cmsg area.
        U64 clen  = cpu->memory->readq(control + 0);
        U32 level = cpu->memory->readd(control + 8);
        U32 type  = cpu->memory->readd(control + 12);
        if (level == K_SOL_SOCKET && type == K_SCM_RIGHTS) {
            U32 nfds = (U32)((clen - 16) / 4);
            U32 cm = base + MSG_SCRATCH_CMSG;
            for (U32 i = 0; i < nfds; i++) {
                U32 hostFd = cpu->memory->readd(control + 16 + 4 * i);
                m32->writed(cm + 16 * i + 0, 16);            // cmsg_len
                m32->writed(cm + 16 * i + 4, K_SOL_SOCKET);  // cmsg_level
                m32->writed(cm + 16 * i + 8, K_SCM_RIGHTS);  // cmsg_type
                m32->writed(cm + 16 * i + 12, hostFd);       // fd
            }
            ctl32 = cm;
            ctllen32 = nfds * 16;
        }
    }
    m32->writed(base + MSG_SCRATCH_HDR + 16, ctl32);              // msg_control
    m32->writed(base + MSG_SCRATCH_HDR + 20, ctllen32);          // msg_controllen
    m32->writed(base + MSG_SCRATCH_HDR + 24, 0);                 // msg_flags

    U32 rc = ksendmsg(thread, (U32)fd, base + MSG_SCRATCH_HDR, (U32)flags);
    // Record the SEND side of fd-passing in the crashring ('S'). get_handle_fd
    // (wineserver -> client) is the request that precedes the deterministic
    // teardown crash; seeing whether/which fd wineserver actually sent (and
    // whether ksendmsg succeeded) is the missing half — recvmsg's 'M'/'F' only
    // capture the receive side. e0 = #fds sent, e1 = the first guest fd value.
    if (wsReadEnabled() && ctllen32) {
        U32 firstFd = (control && ctllen >= 16) ? cpu->memory->readd(control + 16) : 0;
        crashRingRecordRead('S', (U32)thread->process->id, (U32)fd,
                            nullptr, 0, ctllen32 / 16, firstFd);
    }
    if (getenv("BW64_IPCDUMP")) {
        klog_fmt("IPC [pid=%d] sendmsg fd=%d datalen=%d ctllen32=%d -> rc=%d",
                 (int)thread->process->id, (int)fd, (int)total, (int)ctllen32, (int)(S32)rc);
    }
    return (U64)(S64)(S32)rc;
}

// recvmsg(fd, msghdr*, flags) for a 64-bit guest. Builds a 32-bit msghdr over a
// scratch data buffer + cmsg area, calls krecvmsg, then scatters the received
// bytes back into the 64-bit iov segments and translates any received
// SCM_RIGHTS fds into the 64-bit cmsg.
static U64 sys_recvmsg64(CPU64* cpu, U64 fd, U64 msg64, U64 flags) {
    KThread* thread = cpu->thread;
    if (!thread || !thread->process) return (U64)-K_ENOSYS;
    // BW64_WSCONC: wineserver reads protocol requests via recvmsg (SCM creds/fds),
    // so the concurrency witness must cover this path too — sys_read64's hook
    // alone reported 0 because the request stream never flows through read().
    // We bracket the whole call (krecvmsg blocks) so a second wineserver host
    // thread entering recvmsg while the first is still inside is visible.
    bool wsConcRm = wsConcEnabled() && thread->process->exe.contains("wineserver");
    struct WsConcGuard {
        bool on; CPU64* cpu;
        WsConcGuard(bool on, CPU64* cpu) : on(on), cpu(cpu) {
            if (on) {
                static bool announced = false;
                int now = ++g_wsConcInflight;
                if (!announced) { announced = true;
                    klog_fmt("WSCONC: wineserver recvmsg path armed (first hit) pid=%d tid=%d",
                             (int)cpu->thread->process->id, (int)cpu->thread->id); }
                if (now > 1)
                    klog_fmt("WSCONC: %d host threads concurrently in wineserver recvmsg! pid=%d tid=%d",
                             now, (int)cpu->thread->process->id, (int)cpu->thread->id);
            }
        }
        ~WsConcGuard() { if (on) --g_wsConcInflight; }
    } wsConcGuard(wsConcRm, cpu);
    U32 base = msgScratch(thread);
    if (!base) return (U64)-K_EFAULT;
    KMemory* m32 = thread->memory;

    U64 iov     = cpu->memory->readq(msg64 + 16);
    U64 iovlen  = cpu->memory->readq(msg64 + 24);
    U64 control = cpu->memory->readq(msg64 + 32);
    U64 ctllen  = cpu->memory->readq(msg64 + 40);

    // Total receive capacity across all iov segments, capped to the scratch buf.
    U32 dataAddr = base + MSG_SCRATCH_DATA;
    U32 dataCap = MSG_SCRATCH_BYTES - MSG_SCRATCH_DATA;
    U32 want = 0;
    for (U64 i = 0; i < iovlen; i++) {
        U64 l = cpu->memory->readq(iov + 16 * i + 8);
        want += (U32)l;
    }
    if (want > dataCap) want = dataCap;

    U32 cmArea = base + MSG_SCRATCH_CMSG;
    U32 cmCap = MSG_SCRATCH_DATA - MSG_SCRATCH_CMSG; // room for cmsg records
    m32->writed(base + MSG_SCRATCH_IOV + 0, dataAddr);
    m32->writed(base + MSG_SCRATCH_IOV + 4, want);
    m32->writed(base + MSG_SCRATCH_HDR + 0, 0);
    m32->writed(base + MSG_SCRATCH_HDR + 4, 0);
    m32->writed(base + MSG_SCRATCH_HDR + 8, base + MSG_SCRATCH_IOV);
    m32->writed(base + MSG_SCRATCH_HDR + 12, 1);
    m32->writed(base + MSG_SCRATCH_HDR + 16, (control && ctllen) ? cmArea : 0);
    m32->writed(base + MSG_SCRATCH_HDR + 20, (control && ctllen) ? cmCap : 0);
    m32->writed(base + MSG_SCRATCH_HDR + 24, 0);

    U32 rc = krecvmsg(thread, (U32)fd, base + MSG_SCRATCH_HDR, (U32)flags);
    if (getenv("BW64_IPCDUMP")) {
        klog_fmt("IPC [pid=%d] recvmsg fd=%d want=%d flags=0x%x -> rc=%d ctllen32=%d",
                 (int)thread->process->id, (int)fd, (int)want, (unsigned)flags,
                 (int)(S32)rc, (int)m32->readd(base + MSG_SCRATCH_HDR + 20));
    }
    if (wsReadEnabled() && (S32)rc >= 0) {
        // 'M' record: the wineserver request-reply / fd-pass channel (the select
        // + get_apc_result loop the crash trace runs). hdr = first 16 bytes of
        // the received data; e0 = want, e1 = ctllen32 (bytes of SCM_RIGHTS).
        U8 head[16] = {0};
        U32 n = (rc < 16) ? (U32)rc : 16;
        if (n) m32->memcpy(head, dataAddr, n);
        crashRingRecordRead('M', (U32)thread->process->id, (U32)fd, head, (U32)rc,
                            want, (U32)m32->readd(base + MSG_SCRATCH_HDR + 20));
    }
    if ((S32)rc < 0) return (U64)(S64)(S32)rc;

    // Scatter the received bytes back into the 64-bit iov segments.
    U32 remaining = (U32)rc;
    U32 off = 0;
    for (U64 i = 0; i < iovlen && remaining; i++) {
        U64 b = cpu->memory->readq(iov + 16 * i);
        U64 l = cpu->memory->readq(iov + 16 * i + 8);
        U32 chunk = (U32)((l < remaining) ? l : remaining);
        if (chunk) {
            std::vector<U8> tmp(chunk);
            m32->memcpy(tmp.data(), dataAddr + off, chunk);
            cpu->memory->memcpyToGuest(b, tmp.data(), chunk);
            off += chunk;
            remaining -= chunk;
        }
    }

    // Translate any received SCM_RIGHTS fds (32-bit object wrote 16-byte cmsg
    // records, fd at +12) into the 64-bit cmsg the guest provided.
    U32 ctllen32 = m32->readd(base + MSG_SCRATCH_HDR + 20);
    if (control && ctllen >= 16 && ctllen32 >= 16) {
        U32 nfds = ctllen32 / 16;
        // NEVER write past the guest's control buffer (ctllen): a 64-bit cmsg
        // for nfds is 16 (cmsghdr) + nfds*4 bytes. Cap nfds so it fits — an
        // overflow here scribbles past wine/wineserver's heap-allocated control
        // buffer and corrupts its malloc arena ("unsorted double linked list").
        U32 maxFds = (ctllen >= 16) ? (U32)((ctllen - 16) / 4) : 0;
        if (nfds > maxFds) nfds = maxFds;
        U64 cmsgLen = 16 + (U64)nfds * 4; // 64-bit cmsghdr header is 16 bytes
        cpu->memory->writeq(control + 0, cmsgLen);
        cpu->memory->writed(control + 8, K_SOL_SOCKET);
        cpu->memory->writed(control + 12, K_SCM_RIGHTS);
        for (U32 i = 0; i < nfds; i++) {
            U32 fdv = m32->readd(cmArea + 16 * i + 12);
            cpu->memory->writed(control + 16 + 4 * i, fdv);
            if (wsReadEnabled()) {
                // 'F' witness: which fd was just handed to the guest via
                // SCM_RIGHTS. The same delivered fd value reappearing (e1) flags
                // a double-delivered wineserver object handle.
                crashRingRecordRead('F', (U32)thread->process->id, (U32)fd, nullptr, 0, nfds, fdv);
            }
        }
        cpu->memory->writeq(msg64 + 40, cmsgLen); // msg_controllen
    } else {
        cpu->memory->writeq(msg64 + 40, 0);
    }
    return (U64)(S64)(S32)rc;
}

// Map an x86-64 Linux syscall number to a human-readable name. Used only by
// the unimplemented-syscall log path — when running real glibc binaries, the
// first thing you want to see is "which syscall is missing", not "#291".
// Covers both syscalls we already handle (useful for trace) and the obvious
// gaps that are most likely to surface during early Wine/glibc bring-up.
// Returns "?" for unknown numbers; the caller still logs the raw #N.
static const char* x64SyscallName(U64 nr) {
#ifdef BOXEDWINE_GUEST_X64
    if (nr == BOXEDWINE_X64_HOSTCALL_DXMT_UNIX_CALL) return "boxedwine_dxmt_unix_call";
    if (nr == BOXEDWINE_X64_HOSTCALL_X11_BRIDGE) return "boxedwine_x11_bridge";
#endif
#ifdef BOXEDWINE_OPENGL
    if (nr == GL64_SYSCALL_NR) return "gl64_trap";
#endif
    switch (nr) {
        case 0: return "read";
        case 1: return "write";
        case 2: return "open";
        case 3: return "close";
        case 4: return "stat";
        case 5: return "fstat";
        case 6: return "lstat";
        case 7: return "poll";
        case 8: return "lseek";
        case 9: return "mmap";
        case 10: return "mprotect";
        case 11: return "munmap";
        case 12: return "brk";
        case 13: return "rt_sigaction";
        case 14: return "rt_sigprocmask";
        case 15: return "rt_sigreturn";
        case 16: return "ioctl";
        case 17: return "pread64";
        case 18: return "pwrite64";
        case 19: return "readv";
        case 20: return "writev";
        case 21: return "access";
        case 22: return "pipe";
        case 23: return "select";
        case 24: return "sched_yield";
        case 25: return "mremap";
        case 28: return "madvise";
        case 221: return "fadvise64";
        case 280: return "utimensat";
        case 32: return "dup";
        case 33: return "dup2";
        case 34: return "pause";
        case 35: return "nanosleep";
        case 36: return "getitimer";
        case 38: return "setitimer";
        case 39: return "getpid";
        case 41: return "socket";
        case 42: return "connect";
        case 43: return "accept";
        case 44: return "sendto";
        case 45: return "recvfrom";
        case 46: return "sendmsg";
        case 47: return "recvmsg";
        case 53: return "socketpair";
        case 56: return "clone";
        case 57: return "fork";
        case 58: return "vfork";
        case 59: return "execve";
        case 60: return "exit";
        case 61: return "wait4";
        case 62: return "kill";
        case 63: return "uname";
        case 72: return "fcntl";
        case 79: return "getcwd";
        case 80: return "chdir";
        case 82: return "rename";
        case 86: return "link";
        case 88: return "symlink";
        case 89: return "readlink";
        case 90: return "chmod";
        case 91: return "fchmod";
        case 96: return "gettimeofday";
        case 98: return "getrusage";
        case 99: return "sysinfo";
        case 102: return "getuid";
        case 105: return "setuid";
        case 106: return "setgid";
        case 201: return "time";
        case 264: return "renameat";
        case 316: return "renameat2";
        case 104: return "getgid";
        case 107: return "geteuid";
        case 108: return "getegid";
        case 110: return "getppid";
        case 111: return "getpgrp";
        case 117: return "setresuid";
        case 118: return "getresuid";
        case 119: return "setresgid";
        case 120: return "getresgid";
        case 121: return "getpgid";
        case 124: return "getsid";
        case 127: return "rt_sigpending";
        case 128: return "rt_sigtimedwait";
        case 129: return "rt_sigqueueinfo";
        case 130: return "rt_sigsuspend";
        case 131: return "sigaltstack";
        case 29:  return "shmget";
        case 30:  return "shmat";
        case 31:  return "shmctl";
        case 67:  return "shmdt";
        case 137: return "statfs";
        case 138: return "fstatfs";
        case 157: return "prctl";
        case 158: return "arch_prctl";
        case 186: return "gettid";
        case 202: return "futex";
        case 203: return "sched_setaffinity";
        case 204: return "sched_getaffinity";
        case 217: return "getdents64";
        case 218: return "set_tid_address";
        case 228: return "clock_gettime";
        case 229: return "clock_getres";
        case 230: return "clock_nanosleep";
        case 231: return "exit_group";
        case 232: return "epoll_wait";
        case 233: return "epoll_ctl";
        case 200: return "tkill";
        case 234: return "tgkill";
        case 257: return "openat";
        case 262: return "newfstatat";
        case 270: return "pselect6";
        case 273: return "set_robust_list";
        case 290: return "eventfd2";
        case 291: return "epoll_create1";
        case 293: return "pipe2";
        case 302: return "prlimit64";
        case 318: return "getrandom";
        case 334: return "rseq";
        case 435: return "clone3";
        default:  return "?";
    }
}

/*
 * Forward a guest winemetal unix-call to the host-native DXMT table.  The
 * call is intentionally available only to an identity-mapped FEX process:
 * DXMT's parameter blocks contain host Objective-C handles and pointers, so
 * translating a sparse BoxedWine address here would be both incorrect and a
 * security boundary violation.  Builds without the native DXMT archive keep
 * the syscall reserved but return ENOSYS, preserving the optional-backend
 * property of the core.
 */
static const char* dxmtUnixCallName64(U64 callIndex) {
    switch (callIndex) {
        case 47: return "present";
        case 48: return "present-after-delay";
        case 67: return "next-drawable";
        case 70: return "set-layer-properties";
        case 71: return "get-layer-properties";
        case 72: return "create-view";
        default: return nullptr;
    }
}

// The last DXMT unix calls, for a host fault the FEX handler declines. Only
// the first 32 calls and the named ones are logged, so without this ring a
// fault in native code during an unnamed call names nothing. Written racily
// on purpose: it is a diagnostic, and a torn entry is still a hint.
struct DxmtRecentCall {
    U32 index;
    U32 tid;
    U32 ordinal;
};
static DxmtRecentCall gDxmtRecentCalls[32];
static std::atomic<U32> gDxmtRecentCursor{0};

extern "C" void boxedwineDxmtReportRecentCalls(void) {
    const U32 cursor = gDxmtRecentCursor.load(std::memory_order_relaxed);
    const U32 count = cursor < 32 ? cursor : 32;
    for (U32 i = 0; i < count; ++i) {
        const DxmtRecentCall& r = gDxmtRecentCalls[(cursor - count + i) % 32];
        const char* name = dxmtUnixCallName64(r.index);
        klog_fmt("BOXEDWINE_DXMT_RECENT ordinal=%u tid=%u index=%u name=%s",
                 r.ordinal, r.tid, r.index, name ? name : "-");
    }
}

static U64 boxedwineDxmtUnixCall64(CPU64* cpu, U64 callIndex, U64 args) {
    static std::atomic<U32> callLogCount{0};
    const U32 logOrdinal = callLogCount.fetch_add(1, std::memory_order_relaxed);
    const char* callName = dxmtUnixCallName64(callIndex);
    const bool logCall = logOrdinal < 32 || callName != nullptr;
    const U32 processId = cpu && cpu->thread && cpu->thread->process
        ? cpu->thread->process->id : 0;
    if (logCall) {
        klog_fmt("BOXEDWINE_DXMT_CALL ordinal=%u pid=%u index=%llu name=%s args=0x%llx",
                 logOrdinal, processId, (unsigned long long)callIndex,
                 callName ? callName : "other", (unsigned long long)args);
    }
    if (!cpu || !cpu->memory || !cpu->memory->nativeIdentityMode()) {
        if (logCall) {
            // Name the process, because this is not a DXMT fault: only the
            // one process holding the identity map can reach Metal, and a
            // CreateProcess from inside the desktop is a fork child, which
            // KProcess::clone gives a sparse KMemory64 (fex=0). Such a
            // process cannot present at all; D3D11CreateDevice reports "No
            // default adapter available" and the program exits.
            klog_fmt("BOXEDWINE_DXMT_RETURN ordinal=%u status=%d "
                     "reason=native-memory pid=%u fex=%d",
                     logOrdinal, -K_ENOSYS, processId,
                     cpu && cpu->thread && cpu->thread->process &&
                         cpu->thread->process->useFEX64 ? 1 : 0);
        }
        return (U64)(S64)-K_ENOSYS;
    }
    if (callIndex >= BOXEDWINE_X64_HOSTCALL_DXMT_UNIX_CALL_COUNT) {
        if (logCall) {
            klog_fmt("BOXEDWINE_DXMT_RETURN ordinal=%u status=%d reason=index",
                     logOrdinal, -K_EINVAL);
        }
        return (U64)(S64)-K_EINVAL;
    }
    // The native table dereferences the unix-call parameter block directly,
    // so it needs the HOST address of the guest block. DXMT's PE side builds
    // the block on the calling thread's stack, which Wine places in its
    // top-down arena: canonical 0x7ffffe... addresses that the host serves
    // through a relocated alias, not at their own address. A device run had
    // every DXMT call refused here because that region failed the identity
    // check. Translate with the same rule the translator uses and accept
    // only a block whose host page this address space actually tracks;
    // otherwise a guest could present an arbitrary host pointer to
    // Objective-C/Metal code. The nested guest pointers inside the block are
    // translated by the rewritten unix sources themselves (see
    // scripts/rewrite-dxmt-guest-pointers.py).
    const U64 hostArgs = boxedvn::guestToHostAddress(args);
    const bool guestRangeValid =
        args != 0 && boxedvn::guestRangeHostable(args, 8);
    const bool hostTracked = guestRangeValid &&
        cpu->memory->nativeRangeCoversForPlan(hostArgs, hostArgs + 8);
    if (!hostTracked) {
        if (logCall) {
            klog_fmt("BOXEDWINE_DXMT_RETURN ordinal=%u status=%d reason=args "
                     "guest=0x%llx host=0x%llx hostable=%d",
                     logOrdinal, -K_EFAULT, (unsigned long long)args,
                     (unsigned long long)hostArgs, (int)guestRangeValid);
        }
        return (U64)(S64)-K_EFAULT;
    }
    if (logCall && hostArgs != args) {
        klog_fmt("BOXEDWINE_DXMT_ARGS ordinal=%u guest=0x%llx host=0x%llx",
                 logOrdinal, (unsigned long long)args,
                 (unsigned long long)hostArgs);
    }
    args = hostArgs;

#if defined(BOXEDWINE_DXMT_NATIVE)
    using DxmtUnixEntry = S32 (*)(void*);
    const void* raw = dxmt_winemetal_unix_call_funcs[callIndex];
    if (!raw) {
        if (logCall) {
            klog_fmt("BOXEDWINE_DXMT_RETURN ordinal=%u status=%d reason=entry",
                     logOrdinal,
                     (S32)BOXEDWINE_X64_HOSTCALL_STATUS_NOT_IMPLEMENTED);
        }
        return (U64)(S64)(S32)BOXEDWINE_X64_HOSTCALL_STATUS_NOT_IMPLEMENTED;
    }
    const auto entry = reinterpret_cast<DxmtUnixEntry>(const_cast<void*>(raw));
    if (callIndex == 47 || callIndex == 48) {
        if (callIndex == 47) {
            BVNGuestFrameLimiterWait();
        }
        // A presented drawable means the DXMT layer carries frames; the main
        // loop raises it above SDL's later-created view on its next poll.
        BVNDXMTDisplayNotePresented(cpu->thread->process->id);
    }
    {
        const U32 slot = gDxmtRecentCursor.fetch_add(1, std::memory_order_relaxed) % 32;
        gDxmtRecentCalls[slot].index = static_cast<U32>(callIndex);
        gDxmtRecentCalls[slot].tid = cpu && cpu->thread ? cpu->thread->id : 0;
        gDxmtRecentCalls[slot].ordinal = logOrdinal;
    }
    const S32 status = entry(reinterpret_cast<void*>(static_cast<uintptr_t>(args)));
    if (logCall) {
        klog_fmt("BOXEDWINE_DXMT_RETURN ordinal=%u status=%d",
                 logOrdinal, status);
    }
    return (U64)(S64)status;
#else
    (void)callIndex;
    (void)args;
    if (logCall) {
        klog_fmt("BOXEDWINE_DXMT_RETURN ordinal=%u status=%d reason=archive",
                 logOrdinal, -K_ENOSYS);
    }
    return (U64)(S64)-K_ENOSYS;
#endif
}

void ksyscall64(CPU64* cpu) {
    if (!cpu) return;
    U64 nr   = cpu->reg[X64_RAX].u64;
    U64 a1   = cpu->reg[X64_RDI].u64;
    U64 a2   = cpu->reg[X64_RSI].u64;
    U64 a3   = cpu->reg[X64_RDX].u64;
    U64 a4   = cpu->reg[X64_R10].u64;
    U64 a5   = cpu->reg[X64_R8].u64;
    U64 a6   = cpu->reg[X64_R9].u64;
    U64 ret  = (U64)-K_ENOSYS;

    // Open a tail record before the syscall runs, so one that blocks and never
    // returns is visible as pending rather than as a completed zero. Costs one
    // ring slot; nothing is printed unless the process exits non-zero.
    U64 tailToken = 0;
    if (cpu->thread && cpu->thread->process) {
        KProcess* tailProcess = cpu->thread->process.get();
        const U64 tailArgs[6] = {a1, a2, a3, a4, a5, a6};
        tailToken = tailProcess->syscallTail.begin(
            (U32)tailProcess->id, (U32)cpu->thread->id, nr, cpu->syscallRip,
            tailArgs);
        if (!tailProcess->firstSyscallReported) {
            tailProcess->firstSyscallReported = true;
            klog_fmt("BOXEDWINE_X64_PROC_FIRST_SYSCALL pid=%u parent=%u tid=%u "
                     "nr=%llu (%s) rip=0x%llx exe='%s'",
                     (unsigned)tailProcess->id, (unsigned)tailProcess->parentId,
                     (unsigned)cpu->thread->id, (unsigned long long)nr,
                     x64SyscallName(nr), (unsigned long long)cpu->syscallRip,
                     tailProcess->exe.c_str());
        }
    }

    // Set BW64_SYSTRACE=1 to log every 64-bit syscall — invaluable for wine
    // bring-up (finding where a guest stalls or which syscall is next missing).
    // BW64_TRACEPID=N traces ONLY process N (one cheap compare; doesn't slow the
    // rest of the system, so it can catch a timing-sensitive failure that full
    // SYSTRACE masks). Useful to see exactly which syscall precedes a process's
    // exit_group(1).
    {
        static const char* tracePidEnv = getenv("BW64_TRACEPID");
        static int tracePid = tracePidEnv ? atoi(tracePidEnv) : -1;
        // BW64_TRACE_EXE=<substr> traces every process whose commandLine contains
        // <substr> — pid-independent, so it survives wine's loader pid churn (the
        // wine64 wrapper exits under one pid while the app runs under another).
        // Used to find which Win32 startup syscall precedes a GUI app's silent exit.
        static const char* traceExe = getenv("BW64_TRACE_EXE");
        int myPid = (int)(cpu->thread ? cpu->thread->process->id : -1);
        bool exeMatch = false;
        if (traceExe && cpu->thread && cpu->thread->process &&
            cpu->thread->process->commandLine.contains(traceExe)) {
            exeMatch = true;
        }
        if (getenv("BW64_SYSTRACE") || (tracePid >= 0 && myPid == tracePid) || exeMatch) {
            klog_fmt("SYS64 [pid=%d] #%llu %s (a1=0x%llx a2=0x%llx a3=0x%llx)",
                     myPid, (unsigned long long)nr, x64SyscallName(nr),
                     (unsigned long long)a1, (unsigned long long)a2,
                     (unsigned long long)a3);
        }
    }

    switch (nr) {
        case BOXEDWINE_X64_HOSTCALL_DXMT_UNIX_CALL:
            // RDI = DXMT unix-call index, RSI = identity-mapped args block.
            ret = boxedwineDxmtUnixCall64(cpu, a1, a2);
            break;
        case BOXEDWINE_X64_HOSTCALL_X11_BRIDGE:
            // The x86-64 guest libX11 shim. RDI = operation, RSI = guest
            // argument array, RDX = argument count. See source/x11/x11bridge64.cpp.
            ret = x11Bridge64(cpu, a1, a2, a3);
            break;
#ifdef BOXEDWINE_OPENGL
        case GL64_SYSCALL_NR:
            // Private guest->host OpenGL trap (see source/opengl/gl64bridge*).
            // RDI=fn id, RSI=guest VA of GL64Args. Result goes back in RAX.
            ret = gl64Bridge(cpu, a1, a2);
            break;
#endif
        case X64_SYS_write:
            ret = sys_write64(cpu, a1, a2, a3);
            break;
        case X64_SYS_writev:
            ret = sys_writev64(cpu, a1, a2, a3);
            break;
        case X64_SYS_prctl:
            // prctl(option, ...). We don't model process control state
            // (thread name, dumpable flag, seccomp, etc.). Return success for
            // the common set-style options glibc/busybox issue at startup
            // (PR_SET_NAME=15, PR_SET_DUMPABLE=4, PR_SET_PDEATHSIG=1,
            // PR_SET_VMA=0x53564d41) so callers don't treat it as fatal;
            // PR_CAPBSET_READ(23)/PR_GET_*(even) return 0. A blanket 0 is the
            // pragmatic choice for a single-process interpreter.
            ret = 0;
            break;
        case X64_SYS_arch_prctl:
            ret = sys_arch_prctl64(cpu, a1, a2);
            break;
        case X64_SYS_brk:
            ret = sys_brk64(cpu, a1);
            break;
        case X64_SYS_mmap:
            ret = sys_mmap64(cpu, a1, a2, a3, a4, a5, a6);
            break;
        case X64_SYS_mprotect:
            // Flags-only: record the new protection on the page flags (real impl
            // in KMemory64::mprotect). The interpreter does NOT enforce
            // protection (it reads/writes unconditionally, and RELRO/JIT rely on
            // that lenience), so this is pure bookkeeping — it lets munmap and the
            // gap search reason about wine's PROT_NONE reservations vs committed
            // anon pages. We do NOT free buffers on PROT_NONE: that is the same
            // lazy-re-read hazard that makes decommit-on-munmap unsafe.
            //
            // KMemory64::mprotect returns the addr (mmap-style) on success; the
            // mprotect SYSCALL must return 0. Translate: pass through a negative
            // errno, otherwise report success. (Returning addr made ld.so read it
            // as -errno -> "cannot apply additional memory protection after
            // relocation" -> exit 127.)
            {
                U64 r = cpu->memory->mprotect(a1, a2, a3);
                ret = ((S64)r < 0) ? r : 0;
            }
            break;
        case X64_SYS_munmap:
            // Address-space-only free: KMemory64::munmap trims the ranges
            // reservation record so the region is reusable and the gap search
            // stays fast, WITHOUT freeing page backing buffers. Freeing buffers
            // regresses boot (wine/wineserver lazily re-read munmap'd regions —
            // zero-filled reads trip asserts), so the backing store is kept; the
            // memory is already bounded by lazy commit on the mmap side.
            ret = cpu->memory->munmap(a1, a2);
            break;
        case X64_SYS_shmget:
            ret = sys_shmget64(cpu, a1, a2, a3);
            break;
        case X64_SYS_shmat:
            ret = sys_shmat64(cpu, a1, a2, a3);
            break;
        case X64_SYS_shmctl:
            ret = sys_shmctl64(cpu, a1, a2, a3);
            break;
        case X64_SYS_shmdt:
            ret = sys_shmdt64(cpu, a1);
            break;
        case X64_SYS_set_tid_address:
            // set_tid_address(tidptr): records the address the kernel must
            // zero + futex-wake when this thread exits (CLONE_CHILD_CLEARTID
            // semantics). glibc calls this for every thread; pthread_join
            // relies on the wake. Returns the caller's tid.
            if (cpu->thread) {
                cpu->thread->clear_child_tid64 = a1;
            }
            ret = cpu->thread ? cpu->thread->id : 1;
            break;
        case X64_SYS_rt_sigaction:
            ret = sys_rt_sigaction64(cpu, a1, a2, a3, a4);
            break;
        case X64_SYS_rt_sigprocmask:
            ret = sys_rt_sigprocmask64(cpu, a1, a2, a3, a4);
            break;
        case X64_SYS_sigaltstack:
            ret = sys_sigaltstack64(cpu, a1, a2);
            break;
        case X64_SYS_utimensat:
            // utimensat(dirfd, path, times[2], flags) — set file atime/mtime.
            // fontconfig touches its cache files' timestamps; returning -ENOSYS
            // made it retry (43+ calls seen). We don't model per-file mtime
            // precisely, so report success — the cache write itself already
            // persisted the data. (a1=dirfd, a2=path, a3=times, a4=flags.)
            ret = 0;
            break;
        case X64_SYS_set_robust_list:
        case X64_SYS_madvise:
        case X64_SYS_fadvise64:
            // ld-linux makes these calls before main; safe to no-op.
            // fadvise64(fd, offset, len, advice): a pure readahead hint
            // (fontconfig POSIX_FADV_WILLNEED/RANDOM on its cache mmaps).
            // The interpreter has no page cache to advise, so ignoring it is
            // correct — the data is still read on demand.
            ret = 0;
            break;
        case X64_SYS_ioctl: {
            // ioctl(fd, request, arg). The old blanket no-op (return 0) made
            // wine believe a device-type probe on a regular file SUCCEEDED, so
            // it spun forever re-issuing ioctl(0x82307201) on the PE exe it had
            // just opened. But routing blindly to kobject->ioctl is also wrong:
            // the tty/console ioctls (TCGETS, FIONREAD, VT_*, ...) write their
            // result struct back through thread->memory (the 32-bit KMemory),
            // so for a 64-bit guest they scribble the wrong address and fault.
            //
            // For a 64-bit guest the correct, safe answers here are:
            //  - FIONBIO (0x5421): set/clear non-blocking — route to the kobject
            //    via fcntl semantics; the arg is a single int we read ourselves.
            //  - FIONREAD (0x541B): bytes available — we don't track it; report 0.
            //  - everything else (TCGETS device probes, the 0x82307201 query,
            //    KDSKBMUTE, ...): -ENOTTY, which is what a real kernel returns
            //    for a regular file / non-tty and what wine's fallback expects.
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_ENOSYS; break; }
            U32 cmd = (U32)a2;
            // Device ioctls (M5 sound bridge): give the fd's open node first
            // shot via the 64-bit-aware ioctl64 (default returns ENOTTY, so
            // only devices that opt in — /dev/dsp — change behavior here).
            {
                KFileDescriptorPtr fdesc = cpu->thread->process->getFileDescriptor((FD)(S32)a1);
                if (fdesc) {
                    std::shared_ptr<KFile> kf = std::dynamic_pointer_cast<KFile>(fdesc->kobject);
                    if (kf && kf->openFile) {
                        U32 r = kf->openFile->ioctl64(cmd, a3, cpu->memory);
                        if ((S32)r != -K_ENOTTY) { ret = (U64)(S64)(S32)r; break; }
                    }
                }
            }
            if (cmd == 0x5421) { // FIONBIO: arg = int* (0=blocking, !=0 nonblocking)
                U32 on = a3 ? cpu->memory->readd(a3) : 0;
                cpu->thread->process->fcntrl(cpu->thread, (FD)(S32)a1, K_F_SETFL,
                                             on ? K_O_NONBLOCK : 0);
                ret = 0;
            } else if (cmd == 0x541B) { // FIONREAD: report 0 bytes pending
                if (a3) cpu->memory->writed(a3, 0);
                ret = 0;
            } else {
                ret = (U64)-K_ENOTTY;
            }
            break;
        }
        case X64_SYS_chdir: {
            // chdir(path) — MUST be real: wine chdir's into its WINEPREFIX and
            // then stats "." to validate it. A no-op left cwd at the (missing)
            // default /home/username, so that "." stat failed with ENOENT and
            // wine reported "could not find the prefix". Route to KProcess::chdir.
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_ENOSYS; break; }
            char path[1024] = {0};
            cpu->memory->memcpyFromGuest(path, a1, sizeof(path) - 1);
            ret = (U64)(S64)(S32)cpu->thread->process->chdir(BString::copy(path));
            break;
        }
        case X64_SYS_mremap:
            // mremap(old, oldlen, newlen, flags, newaddr). v1: return old
            // address — glibc malloc only resizes when MREMAP_MAYMOVE is set
            // and tolerates a noop if no growth happens. This is wrong but
            // surfaces obvious failures at the malloc level, not later.
            ret = a1;
            break;
        case X64_SYS_dup:
            if (cpu->thread && cpu->thread->process) {
                U32 newFd = cpu->thread->process->dup((U32)a1);
                ret = (S32)newFd < 0 ? (U64)(S64)(S32)newFd : (U64)newFd;
            } else {
                ret = (U64)-K_ENOSYS;
            }
            break;
        case X64_SYS_dup2:
            if (cpu->thread && cpu->thread->process) {
                U32 newFd = cpu->thread->process->dup2((FD)a1, (FD)a2);
                ret = (S32)newFd < 0 ? (U64)(S64)(S32)newFd : (U64)newFd;
            } else {
                ret = (U64)-K_ENOSYS;
            }
            break;
        case X64_SYS_getcwd: {
            // getcwd(buf, size) — copy current directory string out. Returns
            // a pointer to buf on success, -ERANGE if size is too small.
            if (!a1 || a2 == 0) { ret = (U64)-K_EFAULT; break; }
            // No-process standalone runner: default cwd to "/".
            BString cwd = (cpu->thread && cpu->thread->process)
                              ? cpu->thread->process->currentDirectory : B("/");
            if (!cwd.length()) cwd = B("/");
            U64 need = (U64)cwd.length() + 1;
            if (need > a2) { ret = (U64)-34; /* -ERANGE */ break; }
            cpu->memory->memcpyToGuest(a1, cwd.c_str(), need);
            ret = a1;
            break;
        }
        case X64_SYS_fcntl: {
            // fcntl(fd, cmd, arg). Route to the width-agnostic KProcess::fcntrl,
            // which returns the *real* per-fd state — crucially F_GETFL must
            // report the fd's actual access mode (O_RDONLY/WRONLY/RDWR). The
            // old stub returned 0 (== O_RDONLY) for every get, which made
            // glibc's fdopen(fd, "w") fail with EINVAL when wineserver wrote
            // its registry temp file (O_WRONLY) — that was the "could not save
            // registry branch ... Invalid argument" abort.
            //
            // The lock commands (F_GETLK/F_SETLK/F_SETLKW) take a `struct flock*`
            // in `arg`; fcntrl reads/writes it through thread->memory (the 32-bit
            // KMemory), but for a 64-bit guest the struct lives in cpu->memory
            // (KMemory64). So for those we bounce the flock through the 32-bit
            // scratch and use the 64-bit cmd variant (readFileLock(is64=true)
            // layout: l_type@0 w, l_whence@2 w, l_start@4 q, l_len@12 q,
            // l_pid@20 d — 24 bytes). wineserver F_SETLKs its registry/lock
            // files; without this it page-faulted reading the lock arg from the
            // empty 32-bit memory.
            if (!cpu->thread || !cpu->thread->process || !cpu->thread->memory) {
                ret = (U64)-K_ENOSYS; break;
            }
            U32 cmd = (U32)a2;
            bool isLock = (cmd == K_F_GETLK || cmd == K_F_SETLK || cmd == K_F_SETLKW ||
                           cmd == K_F_GETLK64 || cmd == K_F_SETLK64 || cmd == K_F_SETLKW64);
            if (!isLock) {
                ret = (U64)(S64)(S32)cpu->thread->process->fcntrl(
                    cpu->thread, (FD)(S32)a1, cmd, (U32)a3);
                break;
            }
            // Lock command: marshal the 24-byte 64-bit flock into scratch.
            const U32 FLOCK64_BYTES = 24;
            U32 scratch = bounceSockaddrTo32(cpu, 0, 0, nullptr);
            if (!scratch) { ret = (U64)-K_EFAULT; break; }
            {
                U8 tmp[FLOCK64_BYTES];
                cpu->memory->memcpyFromGuest(tmp, a3, FLOCK64_BYTES);
                cpu->thread->memory->memcpy(scratch, tmp, FLOCK64_BYTES);
            }
            // Use the *64-bit* cmd variant so fcntrl's readFileLock reads the
            // 64-bit layout from the scratch we just populated.
            U32 cmd64 = (cmd == K_F_GETLK)  ? K_F_GETLK64
                      : (cmd == K_F_SETLK)  ? K_F_SETLK64
                      : (cmd == K_F_SETLKW) ? K_F_SETLKW64
                      : cmd;
            ret = (U64)(S64)(S32)cpu->thread->process->fcntrl(
                cpu->thread, (FD)(S32)a1, cmd64, scratch);
            // F_GETLK writes the resulting lock back into the struct.
            if (cmd == K_F_GETLK || cmd == K_F_GETLK64) {
                U8 tmp[FLOCK64_BYTES];
                cpu->thread->memory->memcpy(tmp, scratch, FLOCK64_BYTES);
                cpu->memory->memcpyToGuest(a3, tmp, FLOCK64_BYTES);
            }
            break;
        }
        case X64_SYS_tkill:   // tkill(tid, sig)         — a2 unused (tid in a1)
        case X64_SYS_tgkill: { // tgkill(tgid, tid, sig) — tid in a2
            // Deliver a signal to a specific thread, possibly in ANOTHER process.
            // wineserver does exactly this: send_thread_signal() (server/ptrace.c)
            // does tkill/tgkill(client_pid, client_tid, SIGUSR1) to make a client
            // thread run a queued APC. The old handler only ever signaled SELF
            // (returned ESRCH for any tid != our own), so wineserver's APC
            // delivery ALWAYS failed -> queue_apc() returned 0 -> async_terminate
            // fell through to a SYNCHRONOUS async_set_result that dropped the
            // async-queue's reference EARLY -> free_async_queue then released it
            // AGAIN -> the deterministic teardown double-free (bug #2: the
            // release_object refcount assert / tcache / double-linked-list faces).
            // Now: tgkill carries tid in a2, tkill in a1; route cross-thread
            // through a real per-thread delivery.
            bool isTgkill = (nr == X64_SYS_tgkill);
            U32 targetTid = (U32)(isTgkill ? a2 : a1);
            U32 sig       = (U32)(isTgkill ? a3 : a2);
            U64 ourTid    = cpu->thread ? (U64)cpu->thread->id : 1;
            if (targetTid != (U32)ourTid) {
                KThread* target = KSystem::getThreadById(targetTid);
                if (!target) { ret = (U64)-K_ESRCH; break; }
                if (sig == 0) { ret = 0; break; } // liveness probe — target exists
                // Don't arm a pending signal on a thread that is already tearing
                // down: it will never reach a normal scheduler slice to take it,
                // and a thread parked in futex64's wait loop during exit would
                // otherwise keep trying to deliver it mid-kill — a teardown
                // deadlock. The caller still sees success (the thread is on its
                // way out, which is what a signal would have accomplished).
                if (target->terminating) { ret = 0; break; }
                // Async cross-thread delivery: arm the target's pending-signal;
                // the 64-bit scheduler delivers it (deliverSignalSync) at the
                // target's next slice, before it runs guest code. We can't build
                // a signal frame here because the target may be executing on a
                // different host thread.
                {
                    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(target->pendingSignalsMutex);
                    target->pendingSignals |= (1ULL << (sig - 1));
                }
                ret = 0;
                break;
            }
            if (sig == 0) {
                // sig=0 to our own tid is a liveness probe — we're alive.
                ret = 0;
                break;
            }
            // Set RAX to the syscall's return value *before* building the
            // signal frame so the frame captures RAX=0 (success). Otherwise
            // rt_sigreturn restores RAX to whatever was in it at entry
            // (the syscall number 234), and userspace sees tgkill "fail".
            cpu->reg[X64_RAX].setU64(0);
            if (deliverSignalSync(cpu, sig)) {
                // Handler will run next; ret=0 (kernel reports success
                // *before* handler runs, and the syscall return value is
                // overwritten by RAX-restore at rt_sigreturn anyway).
                ret = 0;
            } else {
                // No handler installed / SIG_DFL / SIG_IGN. For most
                // signals this should terminate the process; for v1 we
                // log and return 0 so abort()-paths can be observed.
                klog_fmt("ksyscall64: tgkill self sig=%u — no handler installed", sig);
                ret = 0;
            }
            break;
        }
        case X64_SYS_rt_sigreturn: {
            // x86-64 ABI: at signal-delivery time the kernel set
            //   RSP = uctxPtr - 8     // points at pushed restorer_addr
            // The handler's terminating `ret` pops that slot, leaving
            //   RSP = uctxPtr         // which is what the restorer sees
            // before issuing `syscall rt_sigreturn`. The ucontext_t
            // therefore lives at the *current* RSP. We read all
            // gprs/rip/rflags/sigmask out of it and overwrite our state.
            // The saved RAX becomes the syscall return value (kernel
            // restores the pre-signal RAX, *not* a sigreturn status).
            if (!cpu->memory) { ret = (U64)-K_EFAULT; break; }
            U64 uctxPtr = cpu->reg[X64_RSP].u64;
            ret = restoreSignalFrame(cpu, uctxPtr);
            // restoreSignalFrame wrote rip/rsp/gprs (all except RAX) directly.
            // The dispatcher writes `ret` (savedRax) into RAX after the break,
            // completing the restore. The cpu->rip we just wrote is final —
            // SYSCALL only advances RIP *before* ksyscall64() is invoked, so
            // our mid-handler write to cpu->rip survives.
            break;
        }
        case X64_SYS_read:
            ret = sys_read64(cpu, a1, a2, a3);
            break;
        case X64_SYS_open:
        case X64_SYS_openat:
            // open(path, flags, mode) — same arg layout once we shift one.
            if (nr == X64_SYS_open) ret = sys_openat64(cpu, ~0ULL, a1, a2, a3);
            else                    ret = sys_openat64(cpu, a1,    a2, a3, a4);
            break;
        case X64_SYS_close:
            ret = sys_close64(cpu, a1);
            break;
        case X64_SYS_fstat:
            ret = sys_fstat64(cpu, a1, a2);
            break;
        case X64_SYS_stat:
            ret = sys_stat_path64(cpu, a1, a2, true);
            break;
        case X64_SYS_lstat:
            ret = sys_stat_path64(cpu, a1, a2, false);
            break;
        case X64_SYS_newfstatat:
            ret = sys_newfstatat64(cpu, a1, a2, a3, a4);
            break;
        case X64_SYS_statfs:
            // a1 = path (ignored — same lie for every fs), a2 = struct statfs*
            ret = sys_statfs64_common(cpu, a2);
            break;
        case X64_SYS_fstatfs:
            // a1 = fd (ignored), a2 = struct statfs*
            ret = sys_statfs64_common(cpu, a2);
            break;
        // Extended attributes: our guest FS has none. get*xattr -> -ENODATA
        // ("attribute does not exist"), the answer Linux gives for a file with
        // no xattrs — this is what wine's ntdll probes for and tolerates. The
        // set*/list*/remove* variants likewise report "unsupported/empty".
        // Without these, wine64 startup tripped the unimplemented-syscall path
        // (#191 getxattr) during prefix/service bring-up.
        case 191: // getxattr
        case 192: // lgetxattr
        case 193: // fgetxattr
            ret = (U64)-K_ENODATA;
            break;
        case 194: // listxattr
        case 195: // llistxattr
        case 196: // flistxattr
            ret = 0; // empty attribute list
            break;
        case 188: // setxattr
        case 189: // lsetxattr
        case 190: // fsetxattr
        case 197: // removexattr
        case 198: // lremovexattr
        case 199: // fremovexattr
            ret = (U64)-K_ENOTSUP;
            break;
        case X64_SYS_lseek:
            ret = sys_lseek64(cpu, a1, a2, a3);
            break;
        case X64_SYS_pread64:
            ret = sys_pread64(cpu, a1, a2, a3, a4);
            break;
        case X64_SYS_readlink:
            ret = sys_readlink64(cpu, a1, a2, a3);
            break;
        case X64_SYS_readlinkat:
            // readlinkat(dirfd, path, buf, bufsiz). Modern glibc/wine ntdll use
            // THIS, not readlink(89), to resolve /proc/self/exe (the basis for
            // wine's loader/module dir derivation). dirfd is AT_FDCWD or the
            // path is absolute (/proc/self/exe is), so we can ignore dirfd and
            // reuse the readlink resolver on (path=a2, buf=a3, sz=a4).
            ret = sys_readlink64(cpu, a2, a3, a4);
            break;
        case X64_SYS_access:
        case X64_SYS_faccessat:
        case X64_SYS_faccessat2: {
            // access(path, mode) / faccessat(dirfd, path, mode[, flags]) /
            // faccessat2(dirfd, path, mode, flags). We don't model EUID perms
            // yet, so existence is the only check (mode/flags ignored). For the
            // *at forms the path is arg2 (dirfd is AT_FDCWD or the path is
            // absolute — wine probes ntdll.so by absolute path). wine uses
            // faccessat/faccessat2 to confirm its module dir before loading
            // ntdll, so without these it reports "cannot get path to ntdll.so".
            // Standalone runner (no KProcess) -> ENOENT, matching ld.so probes.
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-2; break; }
            U64 pathArg = (nr == X64_SYS_access) ? a1 : a2;
            char path[1024] = {0};
            cpu->memory->memcpyFromGuest(path, pathArg, sizeof(path) - 1);
            std::shared_ptr<FsNode> n = Fs::getNodeFromLocalPath(
                cpu->thread->process->currentDirectory, BString::copy(path), true);
            ret = n ? 0 : (U64)-2;
            reportDllSearch(cpu->thread->process.get(), "access", path,
                            (long long)(S64)ret, "");
            break;
        }
        case X64_SYS_mkdir:
        case X64_SYS_mkdirat: {
            // mkdir(path, mode) / mkdirat(dirfd, path, mode). wineboot creates
            // its prefix tree (WINEPREFIX, drive_c, windows, ...) through these.
            // path is arg1 (mkdir) or arg2 (mkdirat); dirfd is AT_FDCWD or the
            // path is absolute. Routes to the existing KProcess mkdir, which
            // writes into the native (writable) root overlay.
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_ENOSYS; break; }
            U64 pathArg = (nr == X64_SYS_mkdir) ? a1 : a2;
            char path[1024] = {0};
            cpu->memory->memcpyFromGuest(path, pathArg, sizeof(path) - 1);
            BString full = Fs::getFullPath(cpu->thread->process->currentDirectory,
                                           BString::copy(path));
            U32 mode = (U32)((nr == X64_SYS_mkdir) ? a2 : a3);
            ret = (U64)(S64)(S32)cpu->thread->process->mkdir(full, mode);
            if (getenv("BW64_SYSTRACE")) {
                klog_fmt("sys_mkdir64: '%s' full='%s' -> %d", path, full.c_str(),
                         (int)(S32)ret);
            }
            break;
        }
        case X64_SYS_symlink:
        case X64_SYS_symlinkat: {
            // symlink(target, linkpath) / symlinkat(target, dirfd, linkpath).
            // wineboot builds dosdevices/c: -> ../drive_c and z: -> / symlinks
            // when initializing the prefix. target is arg1; linkpath is arg2
            // (symlink) or arg3 (symlinkat, dirfd=arg2 is AT_FDCWD/absolute).
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_ENOSYS; break; }
            char target[1024] = {0};
            char linkpath[1024] = {0};
            cpu->memory->memcpyFromGuest(target, a1, sizeof(target) - 1);
            U64 linkArg = (nr == X64_SYS_symlink) ? a2 : a3;
            cpu->memory->memcpyFromGuest(linkpath, linkArg, sizeof(linkpath) - 1);
            BString fullLink = Fs::getFullPath(cpu->thread->process->currentDirectory,
                                               BString::copy(linkpath));
            ret = (U64)(S64)(S32)cpu->thread->process->symlink(
                BString::copy(target), fullLink);
            if (getenv("BW64_SCDUMP")) {
                klog_fmt("sys_symlink64: target='%s' link='%s' (cwd='%s') -> %d",
                         target, fullLink.c_str(),
                         cpu->thread->process->currentDirectory.c_str(), (int)(S32)ret);
            }
            break;
        }
        case X64_SYS_link:
        case X64_SYS_linkat: {
            // link(oldpath, newpath) / linkat(olddirfd, oldpath, newdirfd,
            // newpath, flags). fontconfig builds its on-disk cache atomically by
            // writing a temp file, link()-ing it to the final cache name, then
            // unlink()-ing the temp — so a missing link() leaves the cache
            // perpetually un-built and fontconfig re-scans every font dir on
            // every process launch (the boot-storm font-scan loop). Route to
            // KProcess::link, which hard-links (or copies) in the writable
            // overlay. linkat's dirfds are AT_FDCWD/absolute in this path.
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_ENOSYS; break; }
            U64 oldArg = (nr == X64_SYS_link) ? a1 : a2;
            U64 newArg = (nr == X64_SYS_link) ? a2 : a4;
            char oldp[1024] = {0};
            char newp[1024] = {0};
            cpu->memory->memcpyFromGuest(oldp, oldArg, sizeof(oldp) - 1);
            cpu->memory->memcpyFromGuest(newp, newArg, sizeof(newp) - 1);
            BString fullOld = Fs::getFullPath(cpu->thread->process->currentDirectory,
                                              BString::copy(oldp));
            BString fullNew = Fs::getFullPath(cpu->thread->process->currentDirectory,
                                              BString::copy(newp));
            ret = (U64)(S64)(S32)cpu->thread->process->link(fullOld, fullNew);
            if (getenv("BW64_SCDUMP")) {
                klog_fmt("sys_link64: old='%s' new='%s' -> %d",
                         fullOld.c_str(), fullNew.c_str(), (int)(S32)ret);
            }
            break;
        }
        case X64_SYS_rename:
        case X64_SYS_renameat:
        case X64_SYS_renameat2: {
            // rename(from, to) / renameat(olddirfd, from, newdirfd, to) /
            // renameat2(olddirfd, from, newdirfd, to, flags). wineserver saves
            // each registry branch by writing a temp file and rename()-ing it
            // into place (system.reg/user.reg/userdef.reg) — without this the
            // prefix never persists ("could not save registry branch ... :
            // Invalid argument"). Reuse KProcess::rename/renameat, which route
            // through the FsNode layer to the native rename in the writable
            // overlay. renameat2 with a plain (flags==0) rename is identical to
            // renameat; the NOREPLACE/EXCHANGE/WHITEOUT flag bits aren't
            // supported, so reject them with EINVAL (glibc/wine fall back to a
            // plain rename when renameat2 fails).
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_ENOSYS; break; }
            char from[1024] = {0};
            char to[1024] = {0};
            if (nr == X64_SYS_rename) {
                cpu->memory->memcpyFromGuest(from, a1, sizeof(from) - 1);
                cpu->memory->memcpyFromGuest(to,   a2, sizeof(to) - 1);
                ret = (U64)(S64)(S32)cpu->thread->process->rename(
                    BString::copy(from), BString::copy(to));
            } else {
                if (nr == X64_SYS_renameat2 && (a5 & ~0ULL) != 0) {
                    // any flag set -> unsupported in this path
                    ret = (U64)-K_EINVAL; break;
                }
                FD olddirfd = (FD)(S32)a1;
                FD newdirfd = (FD)(S32)a3;
                cpu->memory->memcpyFromGuest(from, a2, sizeof(from) - 1);
                cpu->memory->memcpyFromGuest(to,   a4, sizeof(to) - 1);
                ret = (U64)(S64)(S32)cpu->thread->process->renameat(
                    olddirfd, BString::copy(from), newdirfd, BString::copy(to));
            }
            if (getenv("BW64_SYSTRACE")) {
                klog_fmt("sys_rename64: '%s' -> '%s' ret=%d", from, to, (int)(S32)ret);
            }
            break;
        }
        case X64_SYS_socketpair: {
            // socketpair(domain, type, protocol, sv[2]). wineserver's first IPC
            // call — it creates the AF_UNIX request/reply channel. Reuse the
            // socket-object wiring (ksocketpairFds, no guest-memory access) and
            // write the two FDs into memory64 (pointer-free 4-byte ints, same
            // layout 32/64). `type` carries SOCK_CLOEXEC/NONBLOCK flag bits.
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_ENOSYS; break; }
            U32 type = (U32)a2 & 0xFF;
            U32 sockFlags = (U32)a2 & ~0xFFu; // SOCK_CLOEXEC(0x80000)/NONBLOCK(0x800)
            U32 passFlags = 0;
            if (sockFlags & 0x80000) passFlags |= K_O_CLOEXEC;
            if (sockFlags & 0x800)   passFlags |= K_O_NONBLOCK;
            FD fd1 = 0, fd2 = 0;
            S32 rc = ksocketpairFds(cpu->thread, (U32)a1, type, (U32)a3, passFlags, fd1, fd2);
            if (rc) { ret = (U64)(S64)rc; break; }
            cpu->memory->writed(a4, (U32)fd1);
            cpu->memory->writed(a4 + 4, (U32)fd2);
            ret = 0;
            break;
        }
        case X64_SYS_shutdown:
            // shutdown(sockfd, how). No guest memory; reuse the 32-bit handler.
            // wineserver's sock_check_pollhup shuts down one end of its pair to
            // probe POLLHUP behaviour during init.
            if (!cpu->thread) { ret = (U64)-K_ENOSYS; break; }
            ret = (U64)(S64)(S32)kshutdown(cpu->thread, (U32)a1, (U32)a2);
            break;
        case X64_SYS_socket: {
            // socket(domain, type, protocol). wineserver creates its listening
            // AF_UNIX socket. ksocket takes no guest memory — call directly. The
            // type's high bits are SOCK_CLOEXEC(0x80000)/SOCK_NONBLOCK(0x800);
            // strip them for ksocket and apply via fcntl after.
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_ENOSYS; break; }
            U32 sockType = (U32)a2 & 0xFF;
            FD sfd = (FD)(S32)ksocket((U32)a1, sockType, (U32)a3);
            if ((S32)sfd >= 0) {
                if ((U32)a2 & 0x80000) cpu->thread->process->fcntrl(cpu->thread, sfd, K_F_SETFD, FD_CLOEXEC);
                if ((U32)a2 & 0x800)   cpu->thread->process->fcntrl(cpu->thread, sfd, K_F_SETFL, K_O_NONBLOCK);
            }
            ret = (U64)(S64)(S32)sfd;
            break;
        }
        case X64_SYS_bind: {
            // bind(sockfd, addr, addrlen) — wineserver binds its listening
            // AF_UNIX socket (wineserver.sock). Bounce the sockaddr into 32-bit
            // scratch and reuse kbind + the object layer.
            if (!cpu->thread) { ret = (U64)-K_ENOSYS; break; }
            U32 s32 = bounceSockaddrTo32(cpu, a2, (U32)a3, nullptr);
            if (!s32) { ret = (U64)-K_EFAULT; break; }
            ret = (U64)(S64)(S32)kbind(cpu->thread, (U32)a1, s32, (U32)a3);
            break;
        }
        case X64_SYS_connect: {
            // connect(sockfd, addr, addrlen) — wine64 connects to wineserver's
            // socket. Same bounce.
            if (!cpu->thread) { ret = (U64)-K_ENOSYS; break; }
            U32 s32 = bounceSockaddrTo32(cpu, a2, (U32)a3, nullptr);
            if (!s32) { ret = (U64)-K_EFAULT; break; }
            ret = (U64)(S64)(S32)kconnect(cpu->thread, (U32)a1, s32, (U32)a3);
            /* Whether the guest ever reaches the X server, reported without an
             * environment variable nobody sets on a device. A window that
             * cannot be created and an X connection that never happened are
             * the same failure seen from two ends; this is the other end.
             * Bounded to the first few, which is where the answer is. */
            {
                const U16 connectFamily = cpu->memory->readw(a2);
                if (connectFamily == 1 /*AF_UNIX*/) {
                    char unixPath[128] = {0};
                    cpu->memory->memcpyFromGuest(unixPath, a2 + 2,
                                                 sizeof(unixPath) - 1);
                    if (strstr(unixPath, "X11-unix") || strstr(unixPath, "/X")) {
                        static std::atomic<unsigned> xConnectReports {0};
                        if (xConnectReports.fetch_add(
                                1, std::memory_order_relaxed) < 8) {
                            klog_fmt("BOXEDWINE_X64_X11_CONNECT pid=%d "
                                     "path='%s' result=%d",
                                     (int)(cpu->thread ? cpu->thread->process->id : -1),
                                     unixPath, (int)(S32)ret);
                        }
                    }
                }
            }
            if (getenv("BW64_SCDUMP")) {
                // sockaddr_un: family@0 (2), sun_path@2. sockaddr_in: family@0,
                // port@2 (BE u16), addr@4 (BE u32). Decode both so we can tell a
                // unix X11 connect (/tmp/.X11-unix/X0) from a TCP X connect
                // (port 6000+display) from plain DNS.
                U16 fam = cpu->memory->readw(a2);
                char detail[128] = {0};
                if (fam == 1 /*AF_UNIX*/) {
                    cpu->memory->memcpyFromGuest(detail, a2 + 2, sizeof(detail) - 1);
                } else if (fam == 2 /*AF_INET*/) {
                    U16 portBE = cpu->memory->readw(a2 + 2);
                    U32 ipBE   = cpu->memory->readd(a2 + 4);
                    U16 port = (U16)((portBE >> 8) | (portBE << 8));
                    snprintf(detail, sizeof(detail), "%u.%u.%u.%u:%u",
                             (ipBE) & 0xff, (ipBE >> 8) & 0xff,
                             (ipBE >> 16) & 0xff, (ipBE >> 24) & 0xff, port);
                } else {
                    snprintf(detail, sizeof(detail), "(family %d)", (int)fam);
                }
                klog_fmt("sys_connect64: fd=%d family=%d %s -> %d",
                         (int)a1, (int)fam, detail, (int)(S32)ret);
            }
            break;
        }
        case X64_SYS_listen:
            // listen(sockfd, backlog) — no guest memory.
            if (!cpu->thread) { ret = (U64)-K_ENOSYS; break; }
            ret = (U64)(S64)(S32)klisten(cpu->thread, (U32)a1, (U32)a2);
            break;
        case X64_SYS_accept: {
            // accept(sockfd, addr, addrlen) — wineserver accepts a wine64 client.
            // addr/addrlen are optional out-params; pass a scratch slot when set
            // and copy the result back to memory64.
            if (!cpu->thread) { ret = (U64)-K_ENOSYS; break; }
            U32 lenSlot = 0;
            U32 s32 = a2 ? bounceSockaddrTo32(cpu, 0, 256, &lenSlot) : 0;
            ret = (U64)(S64)(S32)kaccept(cpu->thread, (U32)a1, s32, lenSlot, 0);
            // (peer sockaddr writeback omitted — AF_UNIX accept returns an empty
            // name; wine doesn't rely on it for the server connection.)
            break;
        }
        case X64_SYS_getsockname:
        case X64_SYS_getpeername: {
            // get{sock,peer}name(fd, addr, addrlen*). Bounce; copy the resulting
            // sockaddr + length back into memory64.
            if (!cpu->thread || !a2 || !a3) { ret = (U64)-K_EFAULT; break; }
            U32 inLen = cpu->memory->readd(a3);
            U32 lenSlot = 0;
            U32 s32 = bounceSockaddrTo32(cpu, 0, inLen, &lenSlot);
            if (!s32) { ret = (U64)-K_EFAULT; break; }
            U32 rc = (nr == X64_SYS_getsockname)
                ? kgetsockname(cpu->thread, (U32)a1, s32, lenSlot)
                : kgetpeername(cpu->thread, (U32)a1, s32, lenSlot);
            if ((S32)rc >= 0) {
                U32 outLen = cpu->thread->memory->readd(lenSlot);
                if (outLen > 256) outLen = 256;
                U8 tmp[256];
                cpu->thread->memory->memcpy(tmp, s32, outLen);
                cpu->memory->memcpyToGuest(a2, tmp, outLen);
                cpu->memory->writed(a3, outLen);
            }
            ret = (U64)(S64)(S32)rc;
            break;
        }
        case X64_SYS_fchdir:
            // fchdir(fd) — chdir to a directory held open by descriptor. No
            // guest memory. wineserver fchdir's into the prefix's server dir;
            // without it, it mapped ENOSYS to a file error and asserted.
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_ENOSYS; break; }
            ret = (U64)(S64)(S32)cpu->thread->process->fchdir((FD)a1);
            break;
        case X64_SYS_ftruncate:
            // ftruncate(fd, length) — wineserver sizes its registry/mapping
            // files. No guest memory.
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_ENOSYS; break; }
            ret = (U64)(S64)(S32)cpu->thread->process->ftruncate64((FD)a1, a2);
            break;
        case X64_SYS_umask:
            // umask(mask) — returns the previous mask. wine sets it during init.
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_ENOSYS; break; }
            ret = (U64)cpu->thread->process->umask((U32)a1);
            break;
        case X64_SYS_setsockopt: {
            // setsockopt(fd, level, name, optval, optlen). wineserver sets
            // socket options (SO_PASSCRED etc.) on its connection. optval is a
            // small pointer-free buffer — bounce it into 32-bit scratch.
            if (!cpu->thread) { ret = (U64)-K_ENOSYS; break; }
            U32 optlen = (U32)a5;
            U32 v32 = (a4 && optlen) ? bounceSockaddrTo32(cpu, a4, optlen, nullptr) : 0;
            ret = (U64)(S64)(S32)ksetsockopt(cpu->thread, (U32)a1, (U32)a2, (U32)a3, v32, optlen);
            break;
        }
        case X64_SYS_getsockopt: {
            // getsockopt(fd, level, name, optval, optlen*). optlen is a pointer
            // to socklen_t (in/out). Bounce optval + the length slot, run the
            // helper, copy the result + new length back to memory64.
            if (!cpu->thread || !a5) { ret = (U64)-K_EFAULT; break; }
            U32 inLen = cpu->memory->readd(a5);
            U32 lenSlot = 0;
            U32 v32 = bounceSockaddrTo32(cpu, a4, inLen, &lenSlot);
            if (!v32) { ret = (U64)-K_EFAULT; break; }
            U32 rc = kgetsockopt(cpu->thread, (U32)a1, (U32)a2, (U32)a3, v32, lenSlot);
            if ((S32)rc >= 0) {
                U32 outLen = cpu->thread->memory->readd(lenSlot);
                if (outLen > 256) outLen = 256;
                U8 tmp[256];
                cpu->thread->memory->memcpy(tmp, v32, outLen);
                cpu->memory->memcpyToGuest(a4, tmp, outLen);
                cpu->memory->writed(a5, outLen);
            }
            ret = (U64)(S64)(S32)rc;
            break;
        }
        case X64_SYS_sendmsg:
            // sendmsg(fd, msghdr*, flags) — the wineserver request channel.
            ret = sys_sendmsg64(cpu, a1, a2, a3);
            break;
        case X64_SYS_recvmsg:
            // recvmsg(fd, msghdr*, flags) — the wineserver reply channel
            // (carries SCM_RIGHTS fds for shared mappings).
            ret = sys_recvmsg64(cpu, a1, a2, a3);
            break;
        case X64_SYS_sendto:
        case X64_SYS_recvfrom: {
            // sendto(fd, buf, len, flags, dest_addr, addrlen) /
            // recvfrom(fd, buf, len, flags, src_addr, addrlen). wine's unix ntdll
            // uses these on the (connected AF_UNIX) wineserver socket, so the
            // addr argument is unused — pass NULL addr to k*. Bounce the data
            // buffer through the 32-bit msg scratch (k* read/write thread->memory)
            // and copy results back to the 64-bit guest buffer via cpu->memory.
            if (!cpu->thread || !cpu->thread->process || !cpu->thread->memory) {
                ret = (U64)-K_ENOSYS; break;
            }
            U32 scratch = msgScratch(cpu->thread);
            if (!scratch) { ret = (U64)-K_EFAULT; break; }
            U32 dataAddr = scratch + MSG_SCRATCH_DATA;
            U32 cap = MSG_SCRATCH_BYTES - MSG_SCRATCH_DATA;
            U64 len = a3; if (len > cap) len = cap;
            if (nr == X64_SYS_sendto) {
                U8 tmp[4096];
                U64 off = 0;
                // copy guest buffer -> 32-bit scratch in chunks
                while (off < len) {
                    U32 chunk = (U32)((len - off > sizeof(tmp)) ? sizeof(tmp) : (len - off));
                    cpu->memory->memcpyFromGuest(tmp, a2 + off, chunk);
                    cpu->thread->memory->memcpy(dataAddr + (U32)off, tmp, chunk);
                    off += chunk;
                }
                ret = (U64)(S64)(S32)ksendto(cpu->thread, (U32)a1, dataAddr, (U32)len,
                                             (U32)a4, 0, 0);
            } else {
                S32 rc = (S32)krecvfrom(cpu->thread, (U32)a1, dataAddr, (U32)len,
                                        (U32)a4, 0, 0);
                if (rc > 0) {
                    U8 tmp[4096];
                    U64 off = 0;
                    while (off < (U64)rc) {
                        U32 chunk = (U32)(((U64)rc - off > sizeof(tmp)) ? sizeof(tmp) : ((U64)rc - off));
                        cpu->thread->memory->memcpy(tmp, dataAddr + (U32)off, chunk);
                        cpu->memory->memcpyToGuest(a2 + off, tmp, chunk);
                        off += chunk;
                    }
                }
                ret = (U64)(S64)rc;
            }
            break;
        }
        case X64_SYS_sendmmsg:
        case X64_SYS_recvmmsg: {
            // sendmmsg(fd, mmsghdr* msgvec, vlen, flags[, timeout]) /
            // recvmmsg(...). wine's unix ntdll uses sendmmsg on the wineserver
            // socket. struct mmsghdr = { struct msghdr msg_hdr; unsigned msg_len }:
            // the x86-64 msghdr is 56 bytes, msg_len at +56, stride 64 (8-byte
            // aligned). Loop the existing single-message 64-bit path over each
            // entry, writing back the per-message byte count, and return the
            // number of messages processed (Linux semantics). Stop early on the
            // first error after >=1 success (like the kernel).
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_ENOSYS; break; }
            U64 fd = a1, vec = a2; U32 vlen = (U32)a3; U64 flags = a4;
            const U64 MMSG_STRIDE = 64;   // sizeof(struct mmsghdr) on x86-64
            const U64 MSGLEN_OFF  = 56;   // offsetof(mmsghdr, msg_len)
            U32 done = 0;
            S64 lastErr = 0;
            for (U32 i = 0; i < vlen; i++) {
                U64 hdr = vec + i * MMSG_STRIDE;
                S64 rc = (nr == X64_SYS_sendmmsg)
                       ? (S64)(S32)sys_sendmsg64(cpu, fd, hdr, flags)
                       : (S64)(S32)sys_recvmsg64(cpu, fd, hdr, flags);
                if (rc < 0) { lastErr = rc; break; }
                cpu->memory->writed(hdr + MSGLEN_OFF, (U32)rc);
                done++;
            }
            ret = done ? (U64)done : (U64)lastErr;
            break;
        }
        case X64_SYS_setsid:
            // setsid() — new session; we don't model sessions, return the pid
            // (matches the 32-bit stub). wineserver daemonizes with this.
            ret = (cpu->thread && cpu->thread->process) ? (U64)cpu->thread->process->id : 1;
            break;
        case X64_SYS_unlink:
        case X64_SYS_unlinkat: {
            // unlink(path) / unlinkat(dirfd, path, flags). wine removes stale
            // lock/socket files in its prefix. path is arg1 (unlink) or arg2
            // (unlinkat, dirfd AT_FDCWD/absolute). Route to KProcess::unlinkFile.
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_ENOSYS; break; }
            U64 pathArg = (nr == X64_SYS_unlink) ? a1 : a2;
            char path[1024] = {0};
            cpu->memory->memcpyFromGuest(path, pathArg, sizeof(path) - 1);
            ret = (U64)(S64)(S32)cpu->thread->process->unlinkFile(
                BString(Fs::getFullPath(cpu->thread->process->currentDirectory,
                                        BString::copy(path))));
            break;
        }
        case X64_SYS_pipe:
        case X64_SYS_pipe2: {
            // pipe(fds) / pipe2(fds, flags). Boxedwine models pipes as a
            // connected AF_UNIX stream pair (same as the 32-bit path). wineserver
            // uses a pipe for its signal/wakeup fd. Write the two FDs to memory64.
            // EFAULT before the process check: the bare --x64-selftest runner
            // has no KProcess, and the pipe(NULL) self-test expects -EFAULT.
            if (a1 == 0) { ret = (U64)-K_EFAULT; break; }  // pipefd must be valid
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_ENOSYS; break; }
            U32 pipeFlags = (nr == X64_SYS_pipe2) ? (U32)a2 : 0;
            U32 passFlags = 0;
            if (pipeFlags & 0x80000) passFlags |= K_O_CLOEXEC;  // O_CLOEXEC
            if (pipeFlags & 0x800)   passFlags |= K_O_NONBLOCK; // O_NONBLOCK
            FD fd1 = 0, fd2 = 0;
            S32 rc = ksocketpairFds(cpu->thread, K_AF_UNIX, K_SOCK_STREAM, 0, passFlags, fd1, fd2);
            if (rc) { ret = (U64)(S64)rc; break; }
            cpu->memory->writed(a1, (U32)fd1);
            cpu->memory->writed(a1 + 4, (U32)fd2);
            ret = 0;
            break;
        }
        case X64_SYS_uname:
            ret = sys_uname64(cpu, a1);
            break;
        case X64_SYS_getrandom:
            ret = sys_getrandom64(cpu, a1, a2, a3);
            break;
        case X64_SYS_prlimit64:
            ret = sys_prlimit64_64(cpu, a1, a2, a3, a4);
            break;
        case X64_SYS_getrlimit:
            // getrlimit(resource, struct rlimit* {unsigned long cur,max}).
            // wine's unix ntdll queries RLIMIT_STACK/NOFILE/AS during thread
            // setup. Pretend "no limit" (RLIM_INFINITY) like prlimit64 does.
            if (a2) {
                cpu->memory->writeq(a2, ~0ULL);
                cpu->memory->writeq(a2 + 8, ~0ULL);
            }
            ret = 0;
            break;
        case X64_SYS_setrlimit:
            ret = 0; // accept and ignore — we don't enforce limits
            break;
        case X64_SYS_sched_getscheduler:
            ret = 0; // SCHED_OTHER
            break;
        case X64_SYS_sched_setscheduler:
            ret = 0; // accept; we have one scheduling class
            break;
        case X64_SYS_sched_getparam:
            // sched_param { int sched_priority } — report priority 0.
            if (a2) cpu->memory->writed(a2, 0);
            ret = 0;
            break;
        case X64_SYS_mlockall:
        case X64_SYS_munlockall:
        case X64_SYS_mlock:
        case X64_SYS_munlock:
            // We never page guest memory out, so locking is a no-op success.
            ret = 0;
            break;
        case X64_SYS_clock_gettime:
            ret = sys_clock_gettime64(cpu, a1, a2);
            break;
        case X64_SYS_getuid:
        case X64_SYS_geteuid:
        case X64_SYS_getgid:
        case X64_SYS_getegid:
            ret = 1000; // pretend uid/gid 1000
            break;
        case X64_SYS_setuid:
        case X64_SYS_setgid:
            // We model exactly one user (uid/gid 1000). Tools like coreutils
            // ls/id call setuid/setgid to drop privileges at startup; accept
            // any request to set our own id and reject others with EPERM, the
            // same way a real kernel would for an unprivileged process.
            ret = (a1 == 1000) ? 0 : (U64)-1; // -EPERM
            break;
        case 118: // getresuid(ruid*, euid*, suid*)
        case 120: // getresgid(rgid*, egid*, sgid*)
            // wine's ntdll queries these during process init. We model one
            // user, so real==effective==saved==1000. Each arg is a uid_t* (4
            // bytes); a NULL pointer would be EFAULT but wine always passes
            // valid stack slots.
            if (!a1 || !a2 || !a3) { ret = (U64)-K_EFAULT; break; }
            cpu->memory->writed(a1, 1000);
            cpu->memory->writed(a2, 1000);
            cpu->memory->writed(a3, 1000);
            ret = 0;
            break;
        case 117: // setresuid(ruid, euid, suid)
        case 119: // setresgid(rgid, egid, sgid)
            // Accept setting our own id (1000 or -1 = "unchanged"); reject any
            // attempt to switch to a different user with EPERM, mirroring an
            // unprivileged process. -1 (0xffffffff) means leave that id alone.
            {
                auto ok = [](U64 v){ return v == 1000 || v == 0xffffffffULL; };
                ret = (ok(a1) && ok(a2) && ok(a3)) ? 0 : (U64)-1; // -EPERM
            }
            break;
        case X64_SYS_time: {
            // time(tloc): seconds since epoch; writes to *tloc when non-null.
            U64 sec = KSystem::getSystemTimeAsMicroSeconds() / 1000000ULL;
            if (a1) cpu->memory->writeq(a1, sec);
            ret = sec;
            break;
        }
        case X64_SYS_getpid:
        case X64_SYS_gettid:
            ret = cpu->thread ? cpu->thread->id : 1;
            break;
        case X64_SYS_futex:
            ret = sys_futex64(cpu, a1, (U32)a2, (U32)a3, a4, (U32)a6);
            break;
        case X64_SYS_poll:
        case X64_SYS_ppoll: {
            // poll(fds, nfds, timeout_ms) / ppoll(fds, nfds, timespec*, sigmask).
            // wineserver's sock_init runs sock_check_pollhup(): it closes one end
            // of a socketpair and polls the other, requiring POLLHUP — the old
            // stub (return 0) failed that check and aborted server startup. Bounce
            // the pollfd array (8 bytes each: {int fd; short events; short revents}
            // — layout-identical 32/64) into the process's 32-bit scratch KMemory,
            // call the width-agnostic kpoll (which drives internal_poll's real
            // readiness/blocking + POLLHUP detection), then copy the array back so
            // the updated revents reach the 64-bit guest.
            if (!cpu->thread || !cpu->thread->process || !cpu->thread->memory) {
                ret = (U64)-K_ENOSYS; break;
            }
            U32 nfds = (U32)a2;
            // kpoll's timeout convention (see internal_poll): 0 == return
            // immediately (non-blocking); a value > 0xF0000000 == wait forever;
            // anything in between is a millisecond deadline. The userspace poll
            // ABI maps onto this directly — poll(timeout=-1) is 0xFFFFFFFF
            // (>0xF0000000 == infinite) and poll(timeout=0) is non-blocking — so
            // the 32-bit path passes the raw arg straight through. We do the same.
            U32 timeoutMs;
            if (nr == X64_SYS_ppoll) {
                // ppoll: a3 is a timespec* (NULL == infinite wait).
                if (a3 == 0) {
                    timeoutMs = 0xFFFFFFFFu; // infinite
                } else {
                    U64 sec  = cpu->memory->readq(a3);
                    U64 nsec = cpu->memory->readq(a3 + 8);
                    U64 ms = sec * 1000 + nsec / 1000000;
                    // A real zero timespec is a non-blocking poll (kpoll: 0).
                    timeoutMs = (U32)ms;
                }
            } else {
                // poll: a3 is a signed int ms; pass through (-1 -> 0xFFFFFFFF).
                timeoutMs = (U32)a3;
            }
            if (nfds == 0) {
                // No fds: kpoll with our scratch still honors the timeout sleep.
                U32 scratch0 = bounceSockaddrTo32(cpu, 0, 0, nullptr);
                ret = (U64)(S64)(S32)kpoll(cpu->thread, scratch0, 0, timeoutMs);
                break;
            }
            U32 bytes = nfds * 8;
            // Reuse the multi-page socket scratch (>= one page); cap nfds so the
            // array fits comfortably within a single page.
            if (bytes > K_PAGE_SIZE) { ret = (U64)-K_EINVAL; break; }
            U32 scratch = bounceSockaddrTo32(cpu, 0, 0, nullptr);
            if (!scratch) { ret = (U64)-K_EFAULT; break; }
            // Copy the guest pollfd array into the 32-bit scratch.
            {
                U8 tmp[K_PAGE_SIZE];
                cpu->memory->memcpyFromGuest(tmp, a1, bytes);
                cpu->thread->memory->memcpy(scratch, tmp, bytes);
            }
            S32 rc = (S32)kpoll(cpu->thread, scratch, nfds, timeoutMs);
            // Copy the (revents-updated) array back to the 64-bit guest.
            {
                U8 tmp[K_PAGE_SIZE];
                cpu->thread->memory->memcpy(tmp, scratch, bytes);
                cpu->memory->memcpyToGuest(a1, tmp, bytes);
            }
            ret = (U64)(S64)rc;
            break;
        }
        case X64_SYS_select:
        case X64_SYS_pselect6: {
            // select/pselect6(nfds, readfds*, writefds*, exceptfds*, timeout*, ...).
            // We have no native select, but kpoll() is the real readiness/blocking
            // engine. Translate the fd_set bitmaps into a pollfd array, run kpoll,
            // then translate the revents back into the fd_sets. Without this, wine
            // (winex11/msg-loop) spins on pselect6=ENOSYS and never reaches GL.
            if (!cpu->thread || !cpu->thread->process || !cpu->thread->memory) {
                ret = (U64)-K_ENOSYS; break;
            }
            U32 nfds = (U32)a1;          // highest fd + 1
            U64 rfdsAddr = a2, wfdsAddr = a3, efdsAddr = a4, tsAddr = a5;
            // The shared 32-bit poll scratch is one page and each pollfd is
            // eight bytes. Do not overflow it while translating fd_sets.
            constexpr U32 maxPollFds = K_PAGE_SIZE / 8;
            if (nfds > maxPollFds) { ret = (U64)-K_EINVAL; break; }

            // pselect6 uses timespec {sec,nsec}; select uses timeval {sec,usec}.
            // A NULL timeout means infinite wait for either syscall.
            U32 timeoutMs;
            if (tsAddr == 0) {
                timeoutMs = 0xFFFFFFFFu;
            } else {
                const S64 sec = (S64)cpu->memory->readq(tsAddr);
                const S64 subsec = (S64)cpu->memory->readq(tsAddr + 8);
                const S64 subsecLimit = nr == X64_SYS_select
                    ? 1000000LL : 1000000000LL;
                if (sec < 0 || subsec < 0 || subsec >= subsecLimit) {
                    ret = (U64)-K_EINVAL;
                    break;
                }
                const U64 divisor = nr == X64_SYS_select ? 1000ULL : 1000000ULL;
                const U64 millis = (U64)sec * 1000ULL +
                                   ((U64)subsec + divisor - 1) / divisor;
                timeoutMs = millis >= 0xF0000000ULL
                    ? 0xEFFFFFFFU : (U32)millis;
            }

            // Read the three fd_set bitmaps (each (nfds+7)/8 bytes, capped to the
            // 128-byte/1024-fd standard). 0 addr == that set not requested.
            U32 setBytes = (nfds + 7) / 8;
            if (setBytes > 128) setBytes = 128;
            U8 rset[128] = {0}, wset[128] = {0}, eset[128] = {0};
            if (rfdsAddr) cpu->memory->memcpyFromGuest(rset, rfdsAddr, setBytes);
            if (wfdsAddr) cpu->memory->memcpyFromGuest(wset, wfdsAddr, setBytes);
            if (efdsAddr) cpu->memory->memcpyFromGuest(eset, efdsAddr, setBytes);

            auto isSet = [](const U8* s, U32 fd) { return (s[fd >> 3] >> (fd & 7)) & 1; };

            // Build a pollfd array (8 bytes each: {int fd; short events; short revents})
            // — one entry per fd that appears in any set. POLLIN=1, POLLOUT=4,
            // POLLPRI=2 (the select events) per Linux poll.h.
            struct PFD { S32 fd; U16 ev; U16 rev; };
            PFD pfds[maxPollFds];
            U32 npoll = 0;
            for (U32 fd = 0; fd < nfds && npoll < maxPollFds; fd++) {
                U16 ev = 0;
                if (rfdsAddr && isSet(rset, fd)) ev |= 0x0001; // POLLIN
                if (wfdsAddr && isSet(wset, fd)) ev |= 0x0004; // POLLOUT
                if (efdsAddr && isSet(eset, fd)) ev |= 0x0002; // POLLPRI
                if (ev) { pfds[npoll].fd = (S32)fd; pfds[npoll].ev = ev; pfds[npoll].rev = 0; npoll++; }
            }

            // Run kpoll via the 32-bit scratch (same path the poll/ppoll case uses).
            S32 rc;
            U32 scratch = bounceSockaddrTo32(cpu, 0, 0, nullptr);
            if (!scratch) { ret = (U64)-K_EFAULT; break; }
            if (npoll == 0) {
                rc = (S32)kpoll(cpu->thread, scratch, 0, timeoutMs);
            } else {
                U8 tmp[K_PAGE_SIZE] = {0};
                for (U32 i = 0; i < npoll; i++) {
                    U8* e = tmp + i * 8;
                    *(U32*)e = (U32)pfds[i].fd;
                    *(U16*)(e + 4) = pfds[i].ev;
                    *(U16*)(e + 6) = 0;
                }
                cpu->thread->memory->memcpy(scratch, tmp, npoll * 8);
                rc = (S32)kpoll(cpu->thread, scratch, npoll, timeoutMs);
                cpu->thread->memory->memcpy(tmp, scratch, npoll * 8);
                for (U32 i = 0; i < npoll; i++) pfds[i].rev = *(U16*)(tmp + i * 8 + 6);
            }
            if (rc < 0) { ret = (U64)(S64)rc; break; }

            // Translate revents back into freshly-zeroed fd_sets and count ready fds
            // (select returns the total number of ready bits across all sets).
            U8 outR[128] = {0}, outW[128] = {0}, outE[128] = {0};
            U32 readyCount = 0;
            for (U32 i = 0; i < npoll; i++) {
                U32 fd = (U32)pfds[i].fd; U16 rev = pfds[i].rev;
                if (rfdsAddr && (rev & (0x0001 | 0x0010 /*POLLHUP*/ ))) { outR[fd>>3] |= (1<<(fd&7)); readyCount++; }
                if (wfdsAddr && (rev & 0x0004)) { outW[fd>>3] |= (1<<(fd&7)); readyCount++; }
                if (efdsAddr && (rev & (0x0002 | 0x0008 /*POLLERR*/))) { outE[fd>>3] |= (1<<(fd&7)); readyCount++; }
            }
            if (rfdsAddr) cpu->memory->memcpyToGuest(rfdsAddr, outR, setBytes);
            if (wfdsAddr) cpu->memory->memcpyToGuest(wfdsAddr, outW, setBytes);
            if (efdsAddr) cpu->memory->memcpyToGuest(efdsAddr, outE, setBytes);
            ret = (U64)readyCount;
            break;
        }
        case X64_SYS_sched_yield:
            // Relinquish the host CPU so a sibling guest thread/process that must
            // run to signal an awaited event isn't starved while this thread
            // spin-then-yields (RtlpWaitForCriticalSection, a server_select retry,
            // or wine's NtDelayExecution short-sleep loop).
            //
            // In the MULTI-THREADED build each guest thread runs on its own host
            // thread, so std::this_thread::yield() is the whole job. We must NOT
            // set cpu->yield here: the MT run loop (normalPlatformMultiThreaded.cpp)
            // treats `yield==true && !terminating` as "this thread is done" and
            // BREAKS out — deleting the thread. That made any guest sched_yield
            // (notably wine's Sleep() short-delay spin) silently destroy the
            // calling thread mid-run -> the process went idle/hung. cpu->yield is
            // only meaningful for the single-threaded cooperative scheduler.
#ifndef BOXEDWINE_MULTI_THREADED
            cpu->yield = true;
#endif
            std::this_thread::yield();
            ret = 0;
            break;
        case X64_SYS_sched_getaffinity:
            ret = sys_sched_getaffinity64(cpu, a1, a2, a3);
            break;
        case X64_SYS_sched_setaffinity:
            ret = sys_sched_setaffinity64(cpu, a1, a2, a3);
            break;
        case X64_SYS_kill: {
            // kill(pid, sig). For a real target pid>0 route through the shared
            // KSystem::kill so it consults the live process table — wineserver
            // probes client liveness with kill(pid,0) and force-reaps dead
            // clients with kill(pid,SIGKILL) during the multi-process boot
            // teardown. The old "single-process world" stub answered kill(pid,0)
            // as ALWAYS-alive (sig==0 short-circuited to 0 for ANY pid) and
            // kill(otherpid,sig) as ALWAYS -ESRCH, so wineserver could neither
            // detect a dead client nor kill it -> its reap state machine wedged
            // and double-freed a process object (the "unsorted double linked
            // list"/release_object heap corruption during boot-helper exit).
            // KSystem::kill returns -ESRCH for a nonexistent pid and 0 for a
            // live one (sig==0), matching POSIX and the 32-bit syscall_kill.
            U32 sig = (U32)a2;
            S64 spid = (S64)a1;
            U64 ourPid = cpu->thread && cpu->thread->process ?
                         (U64)cpu->thread->process->id : 1;
            if (spid > 0 && (U64)spid != ourPid) {
                ret = (U64)(S64)(S32)KSystem::kill((S32)spid, sig);
                break;
            }
            // pid<=0 (process group / broadcast) or our own pid: deliver to self
            // synchronously, as before. sig==0 is a permission/liveness probe.
            if (sig == 0) { ret = 0; break; }
            if (deliverSignalSync(cpu, sig)) {
                ret = 0;
            } else {
                klog_fmt("ksyscall64: kill self sig=%u — no handler installed", sig);
                ret = 0;
            }
            break;
        }
        case X64_SYS_rt_sigtimedwait:
            ret = sys_rt_sigtimedwait64(cpu, a1, a2, a3, a4);
            break;
        case X64_SYS_rt_sigsuspend:
            ret = sys_rt_sigsuspend64(cpu, a1, a2);
            break;
        case X64_SYS_rt_sigpending:
            ret = sys_rt_sigpending64(cpu, a1, a2);
            break;
        case X64_SYS_rt_sigqueueinfo:
            // rt_sigqueueinfo(tgid, sig, info) — like kill but with siginfo.
            // We don't deliver, so report success the same way kill does.
            ret = 0;
            break;
        case X64_SYS_pause:
            // pause() blocks till any signal; we never deliver, so the
            // glibc-compatible answer is -EINTR (caller's loop retries).
            ret = (U64)-K_EINTR;
            break;
        case X64_SYS_wait4: {
            // wait4(pid, status*, options, rusage*) — reap a forked child (e.g.
            // wineboot waiting on the wineserver it fork+exec'd). Reuse the
            // arch-neutral reaper and write the status into memory64. rusage
            // (a4) is ignored.
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_ECHILD; break; }
            int status = 0;
            U32 rc = KSystem::reapChild(cpu->thread, (S32)a1, (U32)a3, a2 ? &status : nullptr);
            if ((S32)rc > 0 && a2) {
                cpu->memory->writed(a2, (U32)status);
            }
            ret = (U64)(S64)(S32)rc;
            break;
        }
        case X64_SYS_clone:
            // x86-64 clone arg order: (flags=rdi, stack=rsi, parent_tid=rdx,
            // child_tid=r10, tls=r8). Create a real thread sharing this
            // process + memory64. Falls back to -ENOSYS for non-thread clones.
            if (cpu->thread && cpu->thread->process && cpu->thread->process->memory64) {
                ret = cpu->thread->process->clone64(cpu->thread, a1, a2, a3, a5, a4);
            } else {
                klog_fmt("ksyscall64: clone(flags=0x%llx) with no process/memory64",
                         (unsigned long long)a1);
                ret = (U64)-K_ENOSYS;
            }
            break;
        case X64_SYS_execve:
            // Replaces the process image. On success returns -K_CONTINUE (the
            // new program's RAX is already set by loadProgram64); the dispatcher
            // skips the RAX write for -K_CONTINUE so we don't clobber it.
            ret = sys_execve64(cpu, a1, a2, a3);
            break;
        case X64_SYS_getitimer:
        case X64_SYS_setitimer:
            // Interval timers are rarely used by modern glibc (timer_create
            // is preferred); explicit ENOSYS keeps callers honest.
            ret = (U64)-K_ENOSYS;
            break;
        case X64_SYS_clone3:
            // glibc 2.34+ prefers clone3. Implement it directly (its ENOSYS
            // fallback to clone is fragile in our interpreter).
            if (cpu->thread && cpu->thread->process && cpu->thread->process->memory64) {
                ret = cpu->thread->process->clone364(cpu->thread, a1, a2);
            } else {
                ret = (U64)-K_ENOSYS;
            }
            break;
        case X64_SYS_eventfd2:
            if (!cpu->thread || !cpu->thread->process) {
                ret = (U64)-K_ENOSYS;
            } else {
                ret = (U64)(S64)(S32)syscall_eventfd2(
                    cpu->thread, (U32)a1, (U32)a2);
            }
            break;
        case X64_SYS_epoll_create:
        case X64_SYS_epoll_create1:
            // epoll_create(size) / epoll_create1(flags) — allocate an epoll set.
            // No guest memory. wineserver's main loop is epoll-driven. The
            // process epoll machinery is reused as-is.
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_ENOSYS; break; }
            ret = (U64)(S64)(S32)cpu->thread->process->epollcreate(
                (nr == X64_SYS_epoll_create) ? (U32)a1 : 0,
                (nr == X64_SYS_epoll_create1) ? (U32)a1 : 0);
            break;
        case X64_SYS_epoll_ctl: {
            // epoll_ctl(epfd, op, fd, event*). The epoll_event (12 bytes, packed
            // on x86-64: u32 events; u64 data) is layout-identical 32/64 but the
            // object reads it via thread->memory — bounce the 12 bytes through
            // 32-bit scratch. EPOLL_CTL_DEL passes a NULL event. No process
            // (bare selftest) → -EBADF, matching the kernel + the self-test.
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_EBADF; break; }
            U32 ev32 = 0;
            if (a4) ev32 = bounceSockaddrTo32(cpu, a4, 12, nullptr);
            ret = (U64)(S64)(S32)cpu->thread->process->epollctl((FD)a1, (U32)a2, (FD)a3, ev32);
            if (getenv("BW64_IPCDUMP")) {
                U32 events = a4 ? cpu->thread->memory->readd(ev32 + 0) : 0;
                klog_fmt("IPC epoll_ctl(epfd=%d, op=%d, fd=%d, events=0x%x) -> %d",
                         (int)a1, (int)a2, (int)a3, events, (int)(S32)ret);
            }
            break;
        }
        case X64_SYS_epoll_wait: {
            // epoll_wait(epfd, events, maxevents, timeout). Bounce: let the
            // object write the result events into 32-bit scratch, then copy them
            // back to memory64. Cap maxevents so the scratch page holds them.
            // No process (bare selftest) → -EBADF.
            if (!cpu->thread || !cpu->thread->process) { ret = (U64)-K_EBADF; break; }
            U32 maxevents = (U32)a3;
            if (maxevents > 64) maxevents = 64;       // 64*12=768 < page
            U32 ev32 = bounceSockaddrTo32(cpu, 0, maxevents * 12, nullptr);
            if (!ev32) { ret = (U64)-K_EFAULT; break; }
            U32 rc = cpu->thread->process->epollwait(cpu->thread, (FD)a1, ev32, maxevents, (U32)a4);
            if ((S32)rc > 0) {
                U32 bytes = rc * 12;
                U8 tmp[64 * 12];
                cpu->thread->memory->memcpy(tmp, ev32, bytes);
                cpu->memory->memcpyToGuest(a2, tmp, bytes);
            }
            if (getenv("BW64_IPCDUMP") && (S32)rc > 0) {
                // Only log non-timeout waits — a returned event means a fd in the
                // set (wineserver socket or wait pipe) became readable.
                char fds[256] = {0}; int off = 0;
                for (U32 i = 0; i < rc && off < 230; i++) {
                    U32 ev   = cpu->thread->memory->readd(ev32 + i*12 + 0);
                    U64 data = cpu->thread->memory->readq(ev32 + i*12 + 4);
                    off += snprintf(fds + off, sizeof(fds) - off,
                                    "{ev=0x%x,data=0x%llx} ", ev,
                                    (unsigned long long)data);
                }
                klog_fmt("IPC epoll_wait(epfd=%d,to=%d) -> %d  %s",
                         (int)a1, (int)(S32)a4, (int)rc, fds);
            }
            // BW64_EPSPIN: identify a perpetually-ready fd (the Mode 2 spinner).
            // For each returned event, the epoll `data` is the guest fd number;
            // log that fd's kobject type so we can see WHAT keeps firing EPOLLIN.
            if (getenv("BW64_EPSPIN") && (S32)rc > 0) {
                for (U32 i = 0; i < rc; i++) {
                    U32 ev   = cpu->thread->memory->readd(ev32 + i*12 + 0);
                    U64 data = cpu->thread->memory->readq(ev32 + i*12 + 4);
                    KFileDescriptorPtr rfd = cpu->thread->process->getFileDescriptor((FD)data);
                    const char* kind = "none";
                    if (rfd && rfd->kobject) {
                        switch (rfd->kobject->type) {
                            case KTYPE_FILE: kind="file"; break;
                            case KTYPE_UNIX_SOCKET: {
                                kind="unixsock";
                                break;
                            }
                            case KTYPE_NATIVE_SOCKET: kind="natsock"; break;
                            case KTYPE_EPOLL: kind="epoll"; break;
                            case KTYPE_EVENT: kind="event"; break;
                            case KTYPE_TIMER: kind="timer"; break;
                            case KTYPE_SIGNAL: kind="signal"; break;
                            default: kind="other"; break;
                        }
                    }
                    klog_fmt("EPSPIN pid=%d epfd=%d to=%d readyfd=%llu ev=0x%x kind=%s",
                             (int)cpu->thread->process->id, (int)a1, (int)(S32)a4,
                             (unsigned long long)data, ev, kind);
                }
            }
            ret = (U64)(S64)(S32)rc;
            break;
        }
        case X64_SYS_getdents64:
            ret = sys_getdents64_real(cpu, a1, a2, a3);
            break;
        case X64_SYS_pwrite64:
            // pwrite64(fd, buf, count, offset) — wineserver populates registry/
            // config files in the prefix with it.
            if (a2 == 0) { ret = (U64)-K_EFAULT; break; }
            ret = sys_pwrite64(cpu, a1, a2, a3, a4);
            break;
        case X64_SYS_readv:
            // Scatter-gather read — walk the iovec, read into each segment.
            if (a2 == 0) { ret = (U64)-K_EFAULT; break; }
            ret = sys_readv64(cpu, a1, a2, a3);
            break;
        case X64_SYS_chmod:
        case X64_SYS_fchmod:
            // No-op success: rootfs is effectively read-only for our purpose
            // and glibc's installer paths frequently call chmod on temp
            // files. Returning 0 avoids spurious install-time failures
            // without actually touching anything.
            ret = 0;
            break;
        case X64_SYS_gettimeofday: {
            // struct timeval { U64 sec; U64 usec; }
            if (a1) {
                U64 us = KSystem::getSystemTimeAsMicroSeconds();
                cpu->memory->writeq(a1,     us / 1000000ULL);
                cpu->memory->writeq(a1 + 8, us % 1000000ULL);
            }
            if (a2) {
                // struct timezone — minutes_west, dsttime. Zero both.
                cpu->memory->writed(a2,     0);
                cpu->memory->writed(a2 + 4, 0);
            }
            ret = 0;
            break;
        }
        case X64_SYS_getrusage:
            // struct rusage is large; for v1 just zero-fill 144 bytes.
            // glibc only inspects ru_utime and ru_stime on the startup path.
            if (a2) cpu->memory->memsetGuest(a2, 0, 144);
            ret = 0;
            break;
        case X64_SYS_sysinfo:
            // struct sysinfo — zero-fill the standard 64-byte layout.
            // ld-linux doesn't actually read these; some binaries call it
            // anyway as part of /proc startup probes.
            if (a1) cpu->memory->memsetGuest(a1, 0, 112);
            ret = 0;
            break;
        case X64_SYS_getppid:
            ret = cpu->thread && cpu->thread->process ? cpu->thread->process->parentId : 1;
            break;
        case X64_SYS_getpgrp:
        case X64_SYS_getpgid:
        case X64_SYS_getsid:
            ret = cpu->thread ? cpu->thread->id : 1;
            break;
        case X64_SYS_clock_getres:
            // 1 ns resolution claim. Some libc time helpers use this to size
            // a struct timespec field.
            if (a2) {
                cpu->memory->writeq(a2,     0);
                cpu->memory->writeq(a2 + 8, 1);
            }
            ret = 0;
            break;
        case X64_SYS_nanosleep: {
            if (!cpu->thread || !a1) { ret = (U64)-K_EFAULT; break; }
            const S64 sec = (S64)cpu->memory->readq(a1);
            const S64 nsec = (S64)cpu->memory->readq(a1 + 8);
            if (sec < 0 || nsec < 0 || nsec >= 1000000000LL ||
                (U64)sec > (UINT64_MAX - (U64)nsec) / 1000000000ULL) {
                ret = (U64)-K_EINVAL;
                break;
            }
            ret = (U64)(S64)(S32)cpu->thread->nanoSleep(
                (U64)sec * 1000000000ULL + (U64)nsec);
            break;
        }
        case X64_SYS_clock_nanosleep: {
            if (!cpu->thread || !a3) { ret = (U64)-K_EFAULT; break; }
            // Linux uses TIMER_ABSTIME=1. The shared KThread implementation
            // currently supports relative sleeps only, so reject other flags
            // instead of silently returning without sleeping.
            if (a2 != 0) { ret = (U64)-K_ENOTSUP; break; }
            const S64 sec = (S64)cpu->memory->readq(a3);
            const S64 nsec = (S64)cpu->memory->readq(a3 + 8);
            if (sec < 0 || nsec < 0 || nsec >= 1000000000LL ||
                (U64)sec > (UINT64_MAX - (U64)nsec) / 1000000000ULL) {
                ret = (U64)-K_EINVAL;
                break;
            }
            ret = (U64)(S64)(S32)cpu->thread->clockNanoSleep(
                (U32)a1, 0, (U64)sec * 1000000000ULL + (U64)nsec,
                (U32)a4);
            break;
        }
        case X64_SYS_rseq:
            // restartable-sequences registration. glibc 2.35+ calls this on
            // every thread start. Pretend it's not supported so glibc falls
            // back to plain mutexes.
            ret = (U64)-K_ENOSYS;
            break;
        case X64_SYS_exit:
            ret = sys_exit64(cpu, a1, false);
            break;
        case X64_SYS_exit_group:
            ret = sys_exit64(cpu, a1, true);
            break;
        case 74:  // fsync(fd)
        case 75:  // fdatasync(fd)
        case 26:  // msync(addr,len,flags)
            // Our VFS writes are effectively synchronous (no host page cache to
            // flush for the guest's purposes), so a flush is a successful no-op.
            // Previously these fell to the default -ENOSYS, which some callers
            // (e.g. fontconfig cache write, sqlite, wine's registry save) RETRY
            // — appearing as a repeating "unimplemented syscall #74" near a hang.
            ret = 0;
            break;
        default: {
            // A guest that retries an unsupported syscall does so at machine
            // speed: one such loop wrote 408 MB across 3,215,735 identical
            // lines and buried the evidence that would have explained it.
            // Report the first sighting of each process/thread/number/RIP in
            // full, a few early repeats and then powers of two, and go quiet
            // after saying so. A different number or a different RIP is a
            // different key and still gets its own first-fault report. The
            // return value below is unchanged: this bounds the diagnostics,
            // not the semantics.
            const boxedvn::BoundedSyscallReportLimiter::Outcome unsupported =
                cpu->unsupportedSyscallReports.record(
                    cpu->thread && cpu->thread->process
                        ? (U64)cpu->thread->process->id : 0,
                    cpu->thread ? (U64)cpu->thread->id : 0,
                    (U64)nr, (U64)cpu->syscallRip);
            using UnsupportedDecision =
                boxedvn::BoundedSyscallReportLimiter::Decision;
            if (unsupported.decision == UnsupportedDecision::Detailed) {
                klog_fmt("ksyscall64: unimplemented syscall #%llu (%s) at RIP=0x%llx — RDI=0x%llx RSI=0x%llx RDX=0x%llx R10=0x%llx R8=0x%llx R9=0x%llx",
                         (unsigned long long)nr,
                         x64SyscallName(nr),
                         (unsigned long long)cpu->rip,
                         (unsigned long long)a1,
                         (unsigned long long)a2,
                         (unsigned long long)a3,
                         (unsigned long long)a4,
                         (unsigned long long)a5,
                         (unsigned long long)a6);
            } else if (unsupported.decision == UnsupportedDecision::Repeat) {
                klog_fmt("ksyscall64: unimplemented syscall #%llu (%s) at RIP=0x%llx — seen %llu times",
                         (unsigned long long)nr,
                         x64SyscallName(nr),
                         (unsigned long long)cpu->rip,
                         (unsigned long long)unsupported.occurrences);
            } else if (unsupported.decision == UnsupportedDecision::Suppress) {
                klog_fmt("ksyscall64: unimplemented syscall #%llu (%s) at RIP=0x%llx — seen %llu times; further reports for this syscall are suppressed",
                         (unsigned long long)nr,
                         x64SyscallName(nr),
                         (unsigned long long)cpu->rip,
                         (unsigned long long)unsupported.occurrences);
            }
            // Dump the bytes around the syscall RIP and the full register set
            // so a real Linux syscall can be told apart from a wine NT thunk
            // landing here. syscallRip points at the 0F 05 instruction. Bound
            // it with the same decision: this is the most verbose part of the
            // report and must not run millions of times either.
            if (unsupported.decision == UnsupportedDecision::Detailed &&
                getenv("BW64_SCDUMP")) {
                U64 r = cpu->syscallRip;
                U8 b[16];
                for (int i = 0; i < 16; i++) b[i] = cpu->memory->readb(r - 6 + i);
                klog_fmt("  SCDUMP bytes@RIP-6: %02x %02x %02x %02x %02x %02x [%02x %02x] %02x %02x %02x %02x %02x %02x %02x %02x",
                         b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
                klog_fmt("  SCDUMP RAX=%llx RBX=%llx RCX=%llx RBP=%llx RSP=%llx R12=%llx fsbase=%llx gsbase=%llx",
                         (unsigned long long)cpu->reg[X64_RAX].u64,
                         (unsigned long long)cpu->reg[X64_RBX].u64,
                         (unsigned long long)cpu->reg[X64_RCX].u64,
                         (unsigned long long)cpu->reg[X64_RBP].u64,
                         (unsigned long long)cpu->reg[X64_RSP].u64,
                         (unsigned long long)cpu->reg[X64_R12].u64,
                         (unsigned long long)cpu->fsbase,
                         (unsigned long long)cpu->gsbase);
            }
            ret = (U64)-K_ENOSYS;
            break;
        }
    }

    // execve (and any future blocking/restart paths) return -K_CONTINUE to mean
    // "the new image / restart already set the registers — do NOT overwrite RAX
    // with this sentinel." Mirrors the 32-bit dispatcher (syscall.cpp).
    if (ret == (U64)(S64)-K_CONTINUE || ret == (U64)(S64)-K_WAIT) {
        // The register state belongs to a new image, or the syscall parked and
        // will run again. Either way it did not produce a result.
        if (tailToken && cpu->thread && cpu->thread->process) {
            cpu->thread->process->syscallTail.restart(tailToken);
        }
        return;
    }
    if (tailToken && cpu->thread && cpu->thread->process) {
        cpu->thread->process->syscallTail.complete(tailToken, (S64)ret);
    }
    cpu->reg[X64_RAX].setU64(ret);
}

#endif // BOXEDWINE_GUEST_X64
