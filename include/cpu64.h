/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef __CPU64_H__
#define __CPU64_H__

#ifdef BOXEDWINE_GUEST_X64

#include "reg64.h"
#include <memory>
#include "../source/emulation/cpu/common/fpu.h"
#include <unordered_map>

class KThread;
class KMemory64;

// Sentinel returned by KThread::futex64 when a FUTEX_WAIT had to block and the
// thread was parked on its condition (single-threaded cooperative mode). The
// futex syscall translates this into cpu->reExecuteSyscall so the SYSCALL
// instruction re-runs after the thread is woken. Chosen well outside any
// plausible real return value or -errno. In BOXEDWINE_MULTI_THREADED builds
// futex64 blocks inline and never returns this.
#define K_FUTEX64_PARKED ((S64)0x4000000000000000LL)

// x86-64 register indices. These match the encoding in the ModR/M and REX
// bytes (REX.R/REX.X/REX.B extend the 3-bit field to 4 bits).
enum X64Reg : U8 {
    X64_RAX = 0, X64_RCX = 1, X64_RDX = 2, X64_RBX = 3,
    X64_RSP = 4, X64_RBP = 5, X64_RSI = 6, X64_RDI = 7,
    X64_R8  = 8, X64_R9  = 9, X64_R10 = 10, X64_R11 = 11,
    X64_R12 = 12, X64_R13 = 13, X64_R14 = 14, X64_R15 = 15,
    X64_REG_COUNT = 16
};

// Flag bits — same numeric layout as the 32-bit CPU. RFLAGS upper 32 bits
// are reserved (no documented user-visible bits live there on AMD64).
#define X64_CF  0x00000001
#define X64_PF  0x00000004
#define X64_AF  0x00000010
#define X64_ZF  0x00000040
#define X64_SF  0x00000080
#define X64_DF  0x00000400
#define X64_OF  0x00000800
#define X64_IF  0x00000200

class CPU64 {
public:
    explicit CPU64(KMemory64* memory);
    ~CPU64();

    Reg64 reg[X64_REG_COUNT] = {};
    U64   rip = 0;
    U32   rflags = 0x202; // IF | reserved bit 1
    U64   fsbase = 0;     // x86-64: set via arch_prctl(ARCH_SET_FS)
    U64   gsbase = 0;

    // XMM register state. 16 × 128-bit slots, accessed as two U64 halves.
    // SSE2 is the minimum baseline for x86-64; glibc memcpy/memset, the
    // PIC __x86_get_pc_thunk_bx variants, and TLS init touch these even
    // before main() runs.
    struct Xmm { U64 lo = 0; U64 hi = 0; };
    Xmm   xmm[16] = {};

    // x87 FPU state — shared with the 32-bit CPU's FPU implementation. We
    // only use the register-side methods (FLD/FADD/...) and do all memory
    // I/O ourselves through CPU64's KMemory64 helpers; the EA-form helpers
    // on FPU take a 32-bit CPU* that we can't satisfy.
    FPU fpu;

    // Signal handler table. Mirrors the x86-64 `struct kernel_sigaction`
    // round-tripped through rt_sigaction(2): handler, flags, restorer,
    // sa_mask (64-bit). Indexed by signal number 1..64; slot 0 unused.
    // v1 stores only — no delivery yet. Lets glibc's startup sigaction()
    // calls observe their previous registrations instead of losing them.
    struct SigAction {
        U64 handler = 0;   // SIG_DFL=0, SIG_IGN=1, else func ptr
        U64 flags = 0;
        U64 restorer = 0;
        U64 mask = 0;
        bool installed = false;
    };
    SigAction sigActions[65] = {};

    // Signal blocking mask, round-tripped through rt_sigprocmask(2). Bit N
    // (1..64) corresponds to signal N+1. v1 stores only — no enforcement
    // at delivery time, since delivery itself isn't built yet.
    U64 sigMask = 0;

    // Alternate signal stack, round-tripped through sigaltstack(2). When
    // disabled, ssSp/ssSize are 0 and ssFlags has SS_DISABLE(2) set. The
    // signal-frame builder (future B work) will consult this when an
    // installed SigAction has SA_ONSTACK set.
    struct SigAltStack {
        U64 ssSp = 0;
        U32 ssFlags = 2; // SS_DISABLE — disabled by default
        U64 ssSize = 0;
    };
    SigAltStack sigAltStack;

    // Futex bookkeeping. Maps the guest U64 address of a futex word to the
    // number of waiters parked on it. In a single-threaded world we never
    // *block* — but the count lets WAKE return a non-zero answer when a
    // previous WAIT-blocked entry would have parked, which is what glibc's
    // condvar implementation compares against to decide whether to spin.
    //
    // We keep the map per-CPU64 (i.e. per-process here, since CPU64 is
    // one-per-process for v1). When real KThread64 lands, the right home
    // is on KProcess64, but the storage shape stays identical.
    std::unordered_map<U64, U32> futexWaiters;

    KThread*   thread = nullptr;
    KMemory64* memory = nullptr;

    // Instruction-fetch page cache. fetchByte() is the single hottest call in
    // the interpreter: every instruction byte went through memory->readb(),
    // which takes pagesMutex and does an unordered_map lookup PER BYTE. On a
    // tight guest loop (e.g. wineserver's O(n) UTF-16 case-fold name compare —
    // ~15 instrs, ~45 byte fetches per iteration, millions of iterations) that
    // lock+hash dominates and starves the whole emulator. Code has near-total
    // page locality, so caching the current code page's backing pointer turns
    // the common case into a bounds-compare + array index with no lock. The
    // cached K64Page buffer stays valid because munmap is address-space-only
    // (never frees buffers) and K64Page payloads never move on rehash; the only
    // events that replace the page map are fork (a fresh CPU64 with its own
    // cache) and exit. Invalidated explicitly via invalidateFetchCache() if the
    // backing store is ever decommitted under us. -1 page = empty.
    U64 fetchCachePage = (U64)-1;
    U8* fetchCacheData = nullptr;
    void invalidateFetchCache() { fetchCachePage = (U64)-1; fetchCacheData = nullptr; }

    // Standalone-runner heap pointer. The full kernel tracks the program
    // break on KProcess::brkEnd64; the --x64-run-elf path has no KProcess,
    // so sys_brk64 falls back to this when thread/process is null. Set by
    // the runner to just past the loaded image so glibc's malloc can grow
    // a real heap via brk (without it, brk returns 0, glibc's MORECORE
    // mismatches its own sbrk base, and the heap metadata corrupts).
    // 0 means "not a standalone run" — sys_brk64 keeps its old behaviour.
    U64 runnerBrk = 0;

    // Unified bump allocator for mmap(NULL, ...) placements. BOTH anonymous
    // and file-backed mmaps draw from this ONE pointer so they can never hand
    // out overlapping addresses. Previously sys_mmap64 and sys_mmap64_file
    // each had their own static counter seeded at 0x700000000, so an
    // anonymous map (e.g. ld.so's malloc arena) and a file-backed map (e.g. a
    // DSO span reservation) could be assigned the SAME base — and
    // mmapAnonymousFixed zero-fills on (re)map, silently clobbering the other.
    // That collision corrupted the second versioned DSO's version records,
    // surfacing in the guest ld.so as "unsupported version 0 of Verneed
    // record". One shared pointer per process makes mmap placements disjoint.
    // 0 means "uninitialised" → first use seeds it to MMAP64_BASE.
    U64 mmapNext = 0;

    bool yield = false;
    // Set by a syscall (futex WAIT) that parked this thread on a condition in
    // single-threaded cooperative mode. The SYSCALL handler rewinds RIP back
    // to the SYSCALL instruction and yields, so when the scheduler reschedules
    // this thread (after a FUTEX_WAKE) the SYSCALL re-executes and the futex
    // sees its wake flag. Mirrors the 32-bit -K_WAIT re-execution model where
    // the syscall dispatcher leaves EIP unadvanced. Unused (always false) in
    // BOXEDWINE_MULTI_THREADED builds, where futex WAIT blocks the host thread
    // inline and the syscall completes normally.
    bool reExecuteSyscall = false;
    // RIP of the SYSCALL instruction currently being serviced. Captured before
    // RIP is advanced so reExecuteSyscall can rewind to it.
    U64  syscallRip = 0;
    U64  instructionCount = 0;

    void run();
    // Run up to maxInsn instructions or until yield / decode failure.
    // Returns the number actually executed. Used by the self-test harness
    // to avoid hanging the host on a buggy test program.
    U64 runBounded(U64 maxInsn);

    // Copy the architectural register/FPU/signal state from another CPU64 into
    // this one — used by clone64 to seed a new thread from its parent. Does
    // NOT copy `memory`/`thread` (set by the caller), `runnerBrk` (standalone
    // only), `futexWaiters`, or scheduling flags (yield/reExecuteSyscall).
    void cloneRegistersFrom(const CPU64* from);

    // Synchronously deliver a hardware-fault signal (e.g. SIGFPE on #DE,
    // SIGSEGV on a page fault) to this thread, the way the kernel injects a
    // trap-derived signal at the faulting instruction. Builds a signal frame
    // capturing the *current* rip (so the handler's ucontext points at the
    // faulting instruction), synthesizes a minimal siginfo (si_signo/si_code/
    // si_addr), and sets gregs TRAPNO/ERR/CR2. Returns true if a handler ran
    // (rip now points at it); false if no handler is installed (SIG_DFL/IGN) —
    // the caller then falls back to its previous behaviour. Defined alongside
    // the signal-frame helpers in syscall64.cpp. `siCode` is the si_code (e.g.
    // FPE_INTDIV=1), `trapNo` the x86 vector (0 = #DE), `faultAddr` -> si_addr.
    bool raiseSyncFault(U32 sig, U32 trapNo, S32 siCode, U64 faultAddr);

    // Deliver any pending, unmasked async signals queued on this thread by
    // another thread (cross-thread tkill/tgkill — e.g. wineserver's SIGUSR1
    // APC nudge). Called by the scheduler at the start of this thread's slice,
    // before it runs guest code, so the frame is built while the thread is
    // parked. Returns true if a signal was delivered (rip now in a handler).
    bool deliverPendingSignals();

    void push64(U64 value);
    U64  pop64();

private:
    U32 step();

    U8  fetchByte(U64 addr);
    U32 fetchDword(U64 addr);
    U64 fetchQword(U64 addr);

    // Decoded prefixes — populated by consumePrefixes() and consumed by the
    // opcode handler. Reset at the top of each step().
    struct Prefixes {
        U8   rex = 0;     // 0 if no REX, else 0x40..0x4F
        bool osize16 = false;   // 66h: operand size override
        bool asize32 = false;   // 67h: address size override (truncate to 32)
        U8   seg = 0;     // 0 none, else 0x64 (FS) or 0x65 (GS); other segs ignored in long mode
        U8   rep = 0;     // 0 none, 0xF2 REPNE, 0xF3 REP/REPE
        bool lock = false; // F0h LOCK prefix — RMW must be atomic across threads
    };

    // Effective operand of a ModR/M byte (excluding the "reg" field — that's
    // returned separately as regField). length is the count of bytes after
    // the opcode that the ModR/M+SIB+disp consume.
    struct ModRM {
        bool isReg = false;   // mod==11 → register direct
        U8   rmIndex = 0;     // 0..15 — REX.B extended
        U64  effAddr = 0;     // valid when !isReg (already finalized for RIP-rel)
        U8   length = 0;      // bytes consumed (modrm + optional sib + disp)
        U8   regField = 0;    // top-3-bit "reg" field, REX.R extended (always returned)
        bool isRipRel = false;// for diagnostic/future use
    };

#ifdef BOXEDWINE_BLOCK_EXEC
    // ---- Phase 2 prototype: decoded-block executor (experiment builds only).
    // A BBlock is a run of pre-decoded instruction records starting at a RIP,
    // executed by a dense switch that skips fetch/prefix/ModRM parsing and the
    // giant dispatch. Validity = Phase-1 page generations (pages register via
    // KMemory64::blockPageRegister; any write to them bumps the gen and the
    // block rebuilds). Registers are NEVER cached — ModRM recipes re-resolve
    // register values on every execution.
    struct BRecipe {
        U8  length = 0;        // modrm+sib+disp bytes
        U8  regField = 0;
        bool isReg = false;
        bool ripRel = false;
        U8  rmIndex = 0;
        U8  baseIdx = 0xFF, idxIdx = 0xFF, scale = 0;
        U8  seg = 0;
        bool asize32 = false;
        S32 disp = 0;
        U64 ripTarget = 0;
    };
    enum BKind : U8 {
        BK_MOV_RM_R, BK_MOV_R_RM, BK_ALU_RM_R, BK_ALU_R_RM,
        BK_ALU_RM_IMM, BK_SHIFT_IMM, BK_IMUL_R_RM, BK_JCC8,
        BK_TEST_RM_R, BK_LEA, BK_MOV_RM_IMM, BK_MOV_R_IMM, BK_PUSH, BK_POP
    };
    struct BRec {
        U8  kind = 0;
        U8  size = 4;          // operand size (4/8 only in v1)
        U8  sub = 0;           // aluOp / shift sub-op / jcc condition
        U8  rexPresent = 0;
        U8  len = 0;           // full instruction byte length
        BRecipe mr;
        U64 imm = 0;           // finalized immediate (sign/mask applied at build)
        S32 jccDelta = 0;
    };
    static constexpr U32 BBLOCK_MAX_RECS = 24;
    struct BBlock {
        U64 startRip = 0;      // 0 = empty slot
        U64 page0 = 0, page1 = 0;
        U32 gen0 = 0, gen1 = 0;
        U16 n = 0;
        BRec recs[BBLOCK_MAX_RECS];
    };
    static constexpr U32 BBLOCK_SLOTS = 512;
    std::unique_ptr<BBlock[]> blockTable;      // lazily allocated
    U64 blockNegCache[1024] = {};              // rips known not to block
    U64 blockStatsExec = 0, blockStatsInsn = 0, blockStatsBuilt = 0;

    bool decodeModRMRecipe(U64 modrmAddr, const Prefixes& p, U32 trailingImmBytes, BRecipe& out);
    ModRM resolveRecipe(const BRecipe& r);
    bool buildBlock(U64 startRip, BBlock& b);
    U32  execBlock(const BBlock& b);           // returns instructions executed
    U32  tryBlockStep();                       // probe/build/exec; 0 = interpret
#endif

    // Read prefixes starting at rip; advance an internal cursor. Returns
    // the byte index of the primary opcode, relative to rip.
    U32 consumePrefixes(Prefixes& out);

    // Decode the ModR/M byte at rip+offset using the prefixes. baseAfterImm
    // is what RIP would be at the end of the *full* instruction — needed for
    // RIP-relative effective address. Pass 0 if no further immediate; helpers
    // for common widths sit on top.
    ModRM decodeModRM(U64 modrmAddr, const Prefixes& p, U32 trailingImmBytes);

    // Read/write the operand using the effective ModR/M, in a given size
    // (1/2/4/8 bytes). 32-bit writes zero-extend per x86-64. 8-bit access
    // honours the REX-presence rule (with-REX uses SPL/BPL/SIL/DIL for
    // indices 4..7; without REX uses AH/CH/DH/BH).
    U64  loadRM(const ModRM& m, U32 size, bool rexPresent);
    void storeRM(const ModRM& m, U32 size, U64 value, bool rexPresent);

    // Whole-register 8-bit access (handles the AH/CH/DH/BH vs SPL/BPL/SIL/
    // DIL split). Used by both ModR/M memory ops (for reg field) and direct
    // register opcodes.
    U8   readReg8(U8 index, bool rexPresent);
    void writeReg8(U8 index, U8 value, bool rexPresent);

    // Execute one of the 8 ALU operations (0=ADD..7=CMP), update flags,
    // and store the result back to the chosen destination (r/m or reg-field
    // depending on destIsRM). CMP discards the result.
    void runAlu(U8 aluOp, U32 size, bool destIsRM, U64 lhs, U64 rhs,
                const ModRM& m, bool rexPresentLocal);

    // Evaluate a 4-bit condition code (Jcc/CMOVcc/SETcc share the same
    // encoding: 0=O 1=NO 2=B 3=AE 4=E 5=NE 6=BE 7=A 8=S 9=NS A=P B=NP
    // C=L D=GE E=LE F=G).
    bool evalCC(U8 cc) const;
};

#endif // BOXEDWINE_GUEST_X64
#endif
