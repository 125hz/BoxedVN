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

#include "cpu64.h"
#include "wine_nt_syscall_memory.h"
#include "cmpxchg16b.h"
#include "sse42_string_compare.h"
#include "kmemory64.h"
#include "syscall64.h"
#include "ksignal.h"   // K_SIGFPE

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

// Sharded locks for CPU64 atomic read-modify-write memory ops (XCHG with
// memory, CMPXCHG, XADD, LOCK-prefixed ALU). In the multi-threaded build each
// guest thread runs on its own host thread over a shared KMemory64, so glibc's
// pthread mutexes/condvars — which rely on these instructions being atomic —
// would race without this.
//
// We keep the existing (verified) loadRM/runAlu/storeRM RMW logic intact and
// only serialize it under a lock keyed on the operand's guest address. A single
// global mutex would serialize EVERY locked op across all threads even when
// they touch unrelated words (glibc hammers distinct futex/lock words from
// different threads), so instead we hash the operand's page address into a
// bank of locks: two threads working different pages take different locks and
// run in parallel. Two ops on the same word always collide on the same lock,
// which is exactly the atomicity we need. Recursive so a guest exception/signal
// path that re-enters can't self-deadlock; cheap and uncontended in the
// single-threaded build.
//
// CPU64_ATOMIC_LOCKS must be a power of two so the mask is a single AND.
#define CPU64_ATOMIC_LOCKS 256
static std::recursive_mutex g_cpu64AtomicLocks[CPU64_ATOMIC_LOCKS];

// Pick the lock bank for a guest effective address. Shift by the page shift so
// same-page accesses (the common contended case is a single lock word) share a
// bank while distinct pages spread across banks; the low page bits carry no
// useful entropy for sharding since a lock word sits at one offset.
static inline std::recursive_mutex& cpu64AtomicLockFor(U64 effAddr) {
    return g_cpu64AtomicLocks[(effAddr >> K64_PAGE_SHIFT) & (CPU64_ATOMIC_LOCKS - 1)];
}

// FPU status-word C0/C2/C3 bit positions (see Intel SDM Vol 1 §8.1.3).
// We can't reuse the macros in fpu.cpp because they're file-local there.
static inline void cpu64_fpu_set_c012_3(FPU& f, int c3, int c2, int c0) {
    f.sw &= ~(0x4000 | 0x0400 | 0x0100);
    if (c3) f.sw |= 0x4000;
    if (c2) f.sw |= 0x0400;
    if (c0) f.sw |= 0x0100;
}

CPU64::CPU64(KMemory64* memory) : memory(memory) {
    reg[X64_RSP].setU64(0);
    fpu.FINIT();
}

CPU64::~CPU64() = default;

void CPU64::cloneRegistersFrom(const CPU64* from) {
    for (int i = 0; i < X64_REG_COUNT; i++) {
        reg[i] = from->reg[i];
    }
    rip = from->rip;
    rflags = from->rflags;
    fsbase = from->fsbase;
    gsbase = from->gsbase;
    // Selectors are architectural thread state and clone the way the rest of
    // the register file does. A thread is always cloned from 64-bit code --
    // Wine's WoW64 layer issues its system calls after the far transfer back
    // -- so this carries the 64-bit pair in practice; carrying whatever the
    // parent actually had is what the processor does.
    seg = from->seg;
    // The TLS descriptors clone with the selectors that index them: Linux
    // copies the parent's descriptor array into the child (CLONE_SETTLS then
    // replaces one entry), and a child that inherited FS without inheriting
    // the descriptor FS names would address its TEB through a zero base.
    for (unsigned i = 0; i < K_GUEST_SEGMENT_TABLE_ENTRIES; i++) {
        threadAreaDescriptors[i] = from->threadAreaDescriptors[i];
    }
    for (int i = 0; i < 16; i++) {
        xmm[i] = from->xmm[i];
    }
    fpu = from->fpu;
    // Signal state is process-wide in glibc's model; copy the parent's so the
    // new thread observes the same handler/mask registrations (delivery across
    // threads is a later milestone).
    for (int i = 0; i < 65; i++) {
        sigActions[i] = from->sigActions[i];
    }
    sigMask = from->sigMask;
    sigAltStack = from->sigAltStack;
}

// Hot enough to appear as its own frame in native profiles (i.e. the compiler
// chose not to inline it into step()'s huge body) — force it: the fast path is
// a compare + two array indexes and call overhead dominates it.
__attribute__((always_inline)) inline U8 CPU64::fetchByte(U64 addr) {
    if (!memory) return 0;
    // Fast path: same code page as the last fetch AND the host-pointer cache
    // generation this entry was filled at is still current -> read straight
    // from the cached backing buffer, no lock, no map lookup. Code locality
    // makes this hit on essentially every byte within an instruction and
    // across a loop body, eliminating the per-byte pagesMutex + unordered_map
    // cost that dominated interpreter throughput.
    U64 pageNum = addr >> K64_PAGE_SHIFT;
    if (pageNum == fetchCachePage && fetchCacheGen == k64PageCacheGeneration()) {
        U8* data = fetchCacheData;
        // Belt and braces: a live entry can no longer hold NULL (only this
        // thread fills it, and only with a non-null buffer), but the whole
        // crash class this guards against was a NULL base indexed by an
        // in-page offset, so never index without looking.
        if (data) {
            return data[addr & K64_PAGE_MASK];
        }
    }
    return fetchByteMiss(addr, pageNum);
}

// Hard-capped witness for a fetch whose page has no backing buffer. Every such
// fetch now raises a guest SIGSEGV, so the interesting ones are the first few:
// eight lines, then one line saying the rest are suppressed, and nothing after
// that. The previous "one per 4096 forever" budget is what let a run print
// hundreds of thousands of these while the interpreter executed zeros.
#define CPU64_FETCH_NO_BACKING_REPORT_LIMIT 8u
static void cpu64ReportFetchNoBacking(int pid, U64 addr, U64 pageNum, bool mapped) {
    static std::atomic<U64> reports {0};
    const U64 n = reports.fetch_add(1, std::memory_order_relaxed);
    if (n < CPU64_FETCH_NO_BACKING_REPORT_LIMIT) {
        klog_fmt("BOXEDWINE_X64_FETCH_NO_BACKING pid=%d addr=0x%llx page=0x%llx "
                 "mapped=%d count=%llu",
                 pid, (unsigned long long)addr, (unsigned long long)pageNum,
                 mapped ? 1 : 0, (unsigned long long)(n + 1));
    } else if (n == CPU64_FETCH_NO_BACKING_REPORT_LIMIT) {
        klog_fmt("BOXEDWINE_X64_FETCH_NO_BACKING pid=%d further reports suppressed "
                 "after %u lines", pid, CPU64_FETCH_NO_BACKING_REPORT_LIMIT);
    }
}

U8 CPU64::fetchByteMiss(U64 addr, U64 pageNum) {
    // Read the generation BEFORE resolving the page: an invalidation that
    // lands while we are looking the buffer up has to lose, or we would stamp
    // the current generation onto a pointer it was meant to retire.
    const U32 gen = k64PageCacheGeneration();
    U8* data = memory->getCommittedPagePtr(pageNum);
    if (data) {
        fetchCachePage = pageNum;
        fetchCacheData = data;
        fetchCacheGen = gen;
        return data[addr & K64_PAGE_MASK];
    }
    // No backing buffer. Leave the cache empty so we never memoize a hole, and
    // fault. There is no instruction here to execute: a page the address space
    // does not know at all is an absent translation (SEGV_MAPERR), and a page
    // that IS mapped but has nothing committed behind it is a reservation the
    // guest never wrote (SEGV_ACCERR) -- hardware faults on the fetch in both
    // cases. Returning zero for the mapped case is what let the interpreter
    // decode `add [rax],al` out of a reservation and repeat it hundreds of
    // millions of times instead of reporting the wild jump that got there.
    //
    // Data reads of an uncommitted page still read zero, which is correct for
    // reserved-but-untouched memory; that path is KMemory64::readb and is not
    // touched here. This is the instruction-fetch path only.
    invalidateFetchCache();
    const bool mapped = memory->getPageFlags(pageNum) != 0;
    cpu64ReportFetchNoBacking((int)(thread && thread->process ? (int)thread->process->id : -1),
                              addr, pageNum, mapped);
    if (!fetchFault) {
        fetchFault = true;
        fetchFaultAddr = addr;
        fetchFaultMapped = mapped;
    }
    return 0;
}

U32 CPU64::fetchDword(U64 addr) {
    return memory ? memory->readd(addr) : 0;
}

U64 CPU64::fetchQword(U64 addr) {
    return memory ? memory->readq(addr) : 0;
}

void CPU64::push64(U64 value) {
    reg[X64_RSP].u64 -= 8;
    if (memory) memory->writeq(reg[X64_RSP].u64, value);
}

U64 CPU64::pop64() {
    U64 v = memory ? memory->readq(reg[X64_RSP].u64) : 0;
    reg[X64_RSP].u64 += 8;
    return v;
}

// The base comes from the descriptor the selector indexes -- the same table the
// legacy `int 0x80` gate wrote it into, and the same rule the translator
// backend's fault handler applies when it serves one of these instructions.
// A selector that names no installed descriptor leaves the base at zero, which
// is what every flat descriptor this port publishes would give.
void CPU64::loadSegmentSelector(bool fs, U16 selector) {
    const U64 base = boxedvn::kernelLegacySegmentBase64(this, selector);
    if (fs) {
        seg.fs = selector;
        fsbase = base;
    } else {
        seg.gs = selector;
        gsbase = base;
    }
    static std::atomic<U32> reports {0};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 32) {
        klog_fmt("BOXEDWINE_X64_SEGMENT_WRITE pid=%d tid=%d seg=%s selector=0x%x "
                 "base=0x%llx rip=0x%llx",
                 (thread && thread->process) ? (int)thread->process->id : -1,
                 thread ? (int)thread->id : -1, fs ? "fs" : "gs",
                 (unsigned)selector, (unsigned long long)base,
                 (unsigned long long)rip);
    }
}

// AH/CH/DH/BH addressing: only valid when there is no REX prefix at all.
// With any REX (even REX without W/R/X/B set, i.e. 0x40 itself), indices
// 4..7 of the 8-bit ModR/M reg field name SPL/BPL/SIL/DIL — the low bytes
// of RSP/RBP/RSI/RDI — instead of the high bytes of A/C/D/B.
U8 CPU64::readReg8(U8 index, bool rexPresent) {
    if (!rexPresent && index >= 4 && index <= 7) {
        return reg[index - 4].h8;
    }
    return reg[index].u8;
}

void CPU64::writeReg8(U8 index, U8 value, bool rexPresent) {
    if (!rexPresent && index >= 4 && index <= 7) {
        reg[index - 4].setH8(value);
        return;
    }
    reg[index].setU8(value);
}

// Same story as fetchByte: ~14% of interpreter time as a standalone frame,
// mostly call overhead + spilling `out` — inline it into step().
__attribute__((always_inline)) inline U32 CPU64::consumePrefixes(Prefixes& out) {
    U32 off = 0;
    // Loop until we hit a non-prefix byte. REX, if present, MUST be the last
    // prefix immediately before the opcode (Intel SDM Vol.2 §2.2.1) — we
    // still permit the loop to encounter it and stop on the next byte.
    while (true) {
        U8 b = fetchByte(rip + off);
        switch (b) {
            case 0x66: out.osize16 = true; off++; continue;
            case 0x67: out.asize32 = true; off++; continue;
            case 0x64: out.seg = 0x64; off++; continue; // FS
            case 0x65: out.seg = 0x65; off++; continue; // GS
            case 0x26: case 0x2E: case 0x36: case 0x3E:
                // ES/CS/SS/DS segment overrides — ignored in long mode
                // (effective base is 0). Branch hints repurposed from 2E/3E
                // are likewise non-architectural.
                off++; continue;
            case 0xF0:
                // LOCK — record it so the RMW handler serializes the op across
                // host threads (the multi-threaded build runs guest threads on
                // real host threads sharing one KMemory64).
                out.lock = true; off++; continue;
            case 0xF2: out.rep = 0xF2; off++; continue;
            case 0xF3: out.rep = 0xF3; off++; continue;
            default:
                if ((b & 0xF0) == 0x40) {
                    out.rex = b;
                    off++;
                }
                return off;
        }
    }
}

__attribute__((always_inline)) inline CPU64::ModRM CPU64::decodeModRM(U64 modrmAddr, const Prefixes& p, U32 trailingImmBytes) {
    ModRM m;
    U8 modrm = fetchByte(modrmAddr);
    U8 mod = (modrm >> 6) & 0x3;
    U8 regF = (modrm >> 3) & 0x7;
    U8 rm  =  modrm        & 0x7;

    m.regField = (U8)(regF | ((p.rex & 0x04) ? 0x08 : 0));   // REX.R extends
    m.length = 1;

    if (mod == 0x3) {
        m.isReg = true;
        m.rmIndex = (U8)(rm | ((p.rex & 0x01) ? 0x08 : 0));  // REX.B extends
        return m;
    }

    // RIP-relative: mod=00, rm=101 (no SIB).
    if (mod == 0x0 && rm == 0x5) {
        S32 disp32 = (S32)fetchDword(modrmAddr + 1);
        m.length = 5;
        // Effective address uses RIP of the *next* instruction = modrmAddr +
        // (length of modrm+disp) + trailingImmBytes.
        m.effAddr = modrmAddr + m.length + trailingImmBytes + (U64)(S64)disp32;
        m.isRipRel = true;
        return m;
    }

    // SIB byte follows when rm == 100 (in any mod != 11).
    U64 base = 0;
    bool haveBase = true;
    if (rm == 0x4) {
        U8 sib = fetchByte(modrmAddr + 1);
        m.length = 2;
        U8 scale = (sib >> 6) & 0x3;
        U8 idxF  = (sib >> 3) & 0x7;
        U8 baseF =  sib        & 0x7;

        U8 idxIndex = (U8)(idxF | ((p.rex & 0x02) ? 0x08 : 0));   // REX.X
        U8 baseIndex = (U8)(baseF | ((p.rex & 0x01) ? 0x08 : 0)); // REX.B

        // Index field 100 with REX.X=0 → "no index". With REX.X=1 it's R12.
        U64 idxVal = 0;
        if (!(idxF == 0x4 && !(p.rex & 0x02))) {
            idxVal = reg[idxIndex].u64 << scale;
        }

        // Base field 101 with mod=00 → disp32 only, no base. Else base reg.
        if (baseF == 0x5 && mod == 0x0) {
            haveBase = false;
            base = 0;
        } else {
            base = reg[baseIndex].u64;
        }

        m.effAddr = base + idxVal;
    } else {
        U8 baseIndex = (U8)(rm | ((p.rex & 0x01) ? 0x08 : 0));
        m.effAddr = reg[baseIndex].u64;
    }

    // Displacement.
    if (mod == 0x1) {
        S8 disp8 = (S8)fetchByte(modrmAddr + m.length);
        m.effAddr += (U64)(S64)disp8;
        m.length += 1;
    } else if (mod == 0x2 || (mod == 0x0 && rm == 0x4 && !haveBase)) {
        S32 disp32 = (S32)fetchDword(modrmAddr + m.length);
        m.effAddr += (U64)(S64)disp32;
        m.length += 4;
    }

    if (p.asize32) {
        m.effAddr &= 0xFFFFFFFFULL;
    }

    // FS/GS segment overrides add the per-thread segment base. In long mode
    // CS/DS/ES/SS bases are always 0 and any prefix for them is a no-op,
    // but FS and GS keep their MSR-controlled bases — that's how glibc and
    // ld-linux access TLS (mov rax, fs:[0x28] stack canary, etc.).
    if (p.seg == 0x64) m.effAddr += fsbase;
    else if (p.seg == 0x65) m.effAddr += gsbase;

    return m;
}

__attribute__((always_inline)) inline U64 CPU64::loadRM(const ModRM& m, U32 size, bool rexPresent) {
    if (m.isReg) {
        switch (size) {
            case 1: return readReg8(m.rmIndex, rexPresent);
            case 2: return reg[m.rmIndex].u16;
            case 4: return reg[m.rmIndex].u32;
            case 8: return reg[m.rmIndex].u64;
        }
        return 0;
    }
    switch (size) {
        case 1: return memory->readb(m.effAddr);
        case 2: return memory->readw(m.effAddr);
        case 4: return memory->readd(m.effAddr);
        case 8: return memory->readq(m.effAddr);
    }
    return 0;
}

__attribute__((always_inline)) inline void CPU64::storeRM(const ModRM& m, U32 size, U64 value, bool rexPresent) {
    if (m.isReg) {
        switch (size) {
            case 1: writeReg8(m.rmIndex, (U8)value, rexPresent); return;
            case 2: reg[m.rmIndex].setU16((U16)value); return;
            case 4: reg[m.rmIndex].setU32((U32)value); return; // zero-extends
            case 8: reg[m.rmIndex].setU64(value); return;
        }
        return;
    }
    switch (size) {
        case 1: memory->writeb(m.effAddr, (U8)value); return;
        case 2: memory->writew(m.effAddr, (U16)value); return;
        case 4: memory->writed(m.effAddr, (U32)value); return;
        case 8: memory->writeq(m.effAddr, value); return;
    }
}

// -----------------------------------------------------------------------
// Flag computation helpers.
//
// Strategy: eager compute. v1 prioritises correctness and inspectability
// over the 32-bit path's lazy-flags machinery. Once the interpreter is
// running real programs we can revisit and port lazy flags.
//
// Flag definitions per Intel SDM Vol.1 §3.4.3.1:
//   CF — carry/borrow out of MSB (unsigned overflow)
//   PF — even parity of low byte of result
//   AF — carry/borrow out of bit 3 (nibble), for BCD ops
//   ZF — result is zero
//   SF — MSB of result
//   OF — signed overflow (carry-in to MSB != carry-out)
//
// Width is the byte width of the operands (1/2/4/8). Flag formulas are the
// same as 32-bit Boxedwine's eager paths, generalised to U64.
// -----------------------------------------------------------------------

static inline U64 maskFor(U32 width) {
    if (width >= 8) return 0xFFFFFFFFFFFFFFFFULL;
    return (1ULL << (width * 8)) - 1ULL;
}
static inline U64 signBitFor(U32 width) {
    return 1ULL << (width * 8 - 1);
}
static inline bool parityEven(U8 b) {
    b ^= b >> 4; b ^= b >> 2; b ^= b >> 1;
    return (b & 1) == 0;
}

// Sets ZF/SF/PF based on the masked result. Caller fills CF/OF/AF.
static void setSZP(U32& f, U64 result, U32 width) {
    U64 r = result & maskFor(width);
    f &= ~(X64_ZF | X64_SF | X64_PF);
    if (r == 0) f |= X64_ZF;
    if (r & signBitFor(width)) f |= X64_SF;
    if (parityEven((U8)r)) f |= X64_PF;
}

static void flagsAdd(U32& f, U64 a, U64 b, U64 r, U32 width) {
    U64 mask = maskFor(width);
    a &= mask; b &= mask; U64 rm = r & mask;
    f &= ~(X64_CF | X64_OF | X64_AF);
    // CF: did the unsigned sum overflow the width?
    if (rm < a) f |= X64_CF;
    // OF: signs of operands match and differ from result sign.
    U64 sb = signBitFor(width);
    if (((~(a ^ b)) & (a ^ rm)) & sb) f |= X64_OF;
    if (((a ^ b ^ rm) & 0x10) != 0) f |= X64_AF;
    setSZP(f, rm, width);
}

static void flagsSub(U32& f, U64 a, U64 b, U64 r, U32 width) {
    U64 mask = maskFor(width);
    a &= mask; b &= mask; U64 rm = r & mask;
    f &= ~(X64_CF | X64_OF | X64_AF);
    if (a < b) f |= X64_CF;
    U64 sb = signBitFor(width);
    if (((a ^ b) & (a ^ rm)) & sb) f |= X64_OF;
    if (((a ^ b ^ rm) & 0x10) != 0) f |= X64_AF;
    setSZP(f, rm, width);
}

// Logical ops always clear CF and OF, leave AF undefined (we clear it).
static void flagsLogic(U32& f, U64 r, U32 width) {
    f &= ~(X64_CF | X64_OF | X64_AF);
    setSZP(f, r, width);
}

__attribute__((always_inline)) inline void CPU64::runAlu(U8 aluOp, U32 size, bool destIsRM, U64 lhs, U64 rhs,
                   const ModRM& m, bool rexPresentLocal) {
    // aluOp: 0=ADD 1=OR 2=ADC 3=SBB 4=AND 5=SUB 6=XOR 7=CMP
    U64 result = 0;
    bool storeBack = (aluOp != 7); // CMP discards result
    switch (aluOp) {
        case 0: result = lhs + rhs;
                flagsAdd(rflags, lhs, rhs, result, size); break;
        case 1: result = lhs | rhs;
                flagsLogic(rflags, result, size); break;
        case 2: { U64 cIn = (rflags & X64_CF) ? 1 : 0;
                result = lhs + rhs + cIn;
                flagsAdd(rflags, lhs, rhs + cIn, result, size); break; }
        case 3: { U64 cIn = (rflags & X64_CF) ? 1 : 0;
                result = lhs - rhs - cIn;
                flagsSub(rflags, lhs, rhs + cIn, result, size); break; }
        case 4: result = lhs & rhs;
                flagsLogic(rflags, result, size); break;
        case 5: result = lhs - rhs;
                flagsSub(rflags, lhs, rhs, result, size); break;
        case 6: result = lhs ^ rhs;
                flagsLogic(rflags, result, size); break;
        case 7: result = lhs - rhs;
                flagsSub(rflags, lhs, rhs, result, size); break;
    }
    if (!storeBack) return;
    if (destIsRM) {
        storeRM(m, size, result, rexPresentLocal);
    } else {
        switch (size) {
            case 1: writeReg8(m.regField, (U8)result, rexPresentLocal); break;
            case 2: reg[m.regField].setU16((U16)result); break;
            case 4: reg[m.regField].setU32((U32)result); break;
            case 8: reg[m.regField].setU64(result); break;
        }
    }
}

bool CPU64::evalCC(U8 cc) const {
    switch (cc & 0xF) {
        case 0x0: return (rflags & X64_OF) != 0;
        case 0x1: return (rflags & X64_OF) == 0;
        case 0x2: return (rflags & X64_CF) != 0;
        case 0x3: return (rflags & X64_CF) == 0;
        case 0x4: return (rflags & X64_ZF) != 0;
        case 0x5: return (rflags & X64_ZF) == 0;
        case 0x6: return (rflags & (X64_CF | X64_ZF)) != 0;
        case 0x7: return (rflags & (X64_CF | X64_ZF)) == 0;
        case 0x8: return (rflags & X64_SF) != 0;
        case 0x9: return (rflags & X64_SF) == 0;
        case 0xA: return (rflags & X64_PF) != 0;
        case 0xB: return (rflags & X64_PF) == 0;
        case 0xC: return ((rflags & X64_SF) != 0) != ((rflags & X64_OF) != 0);
        case 0xD: return ((rflags & X64_SF) != 0) == ((rflags & X64_OF) != 0);
        case 0xE: return ((rflags & X64_ZF) != 0) ||
                         (((rflags & X64_SF) != 0) != ((rflags & X64_OF) != 0));
        case 0xF: return ((rflags & X64_ZF) == 0) &&
                         (((rflags & X64_SF) != 0) == ((rflags & X64_OF) != 0));
    }
    return false;
}

// Compute SHL/SHR/SAR/ROL/ROR/RCL/RCR result + flags. count is already masked.
// Returns the new value; flag effects are applied to rflags. For rotates the
// SZP flags are not touched (per Intel SDM); only CF (and OF for count==1).
__attribute__((always_inline)) inline static U64 doShift(U32& rflags, U8 sub, U64 v, U8 count, U32 width) {
    if (count == 0) return v;
    U64 mask = maskFor(width);
    v &= mask;
    U64 result = v;
    U64 sb = signBitFor(width);
    U32 wbits = width * 8;
    bool cf = (rflags & X64_CF) != 0;
    bool isRotate = (sub <= 3);
    switch (sub) {
        case 0: { // ROL
            U8 c = count % wbits;
            if (c == 0) {
                result = v;
                // CF still set from low bit of result per Intel
                cf = (v & 1) != 0;
            } else {
                result = ((v << c) | (v >> (wbits - c))) & mask;
                cf = (result & 1) != 0;
            }
            break;
        }
        case 1: { // ROR
            U8 c = count % wbits;
            if (c == 0) {
                result = v;
                cf = (v & sb) != 0;
            } else {
                result = ((v >> c) | (v << (wbits - c))) & mask;
                cf = (result & sb) != 0;
            }
            break;
        }
        case 2: { // RCL — rotate through carry, (wbits+1)-bit rotation
            U8 c = count % (wbits + 1);
            for (U8 i = 0; i < c; i++) {
                bool newCf = (result & sb) != 0;
                result = ((result << 1) | (cf ? 1 : 0)) & mask;
                cf = newCf;
            }
            break;
        }
        case 3: { // RCR
            U8 c = count % (wbits + 1);
            for (U8 i = 0; i < c; i++) {
                bool newCf = (result & 1) != 0;
                result = ((result >> 1) | (cf ? sb : 0)) & mask;
                cf = newCf;
            }
            break;
        }
        case 4: // SHL/SAL
        case 6: // alias
            cf = (v >> (wbits - count)) & 1;
            result = (v << count) & mask;
            break;
        case 5: // SHR
            cf = (v >> (count - 1)) & 1;
            result = v >> count;
            break;
        case 7: // SAR — arithmetic, replicate sign bit
            {
                S64 sv = (width == 8) ? (S64)v
                       : (width == 4) ? (S64)(S32)v
                       : (width == 2) ? (S64)(S16)v
                                      : (S64)(S8)v;
                cf = (v >> (count - 1)) & 1;
                result = (U64)(sv >> count) & mask;
            }
            break;
        default:
            return v;
    }
    rflags &= ~(X64_CF | X64_OF);
    if (cf) rflags |= X64_CF;
    if (count == 1) {
        bool of = false;
        if (sub == 4 || sub == 6) of = ((result & sb) != 0) != cf;
        else if (sub == 5) of = (v & sb) != 0;
        else if (sub == 0 || sub == 2) of = ((result & sb) != 0) != cf;   // ROL/RCL: MSB(result) XOR CF
        else if (sub == 1 || sub == 3) of = ((result & sb) != 0) != (((result << 1) & sb) != 0); // ROR/RCR: MSB XOR (MSB-1)
        if (of) rflags |= X64_OF;
    }
    if (!isRotate) setSZP(rflags, result, width);
    return result;
}

// Minimal x86-64 decode-and-execute. Grows opcode by opcode. Anything not
// handled logs the leading bytes and yields so we surface gaps quickly
// instead of looping.
// BW64_OPPROF: opcode-frequency profiler (env-gated, zero cost when off). Counts
// each executed (op, op2) pair so we can see which opcodes dominate a real
// Wine service and GUI-process boot, and order the step() dispatch by measured
// frequency.
// op2 is the second byte for the 0F two-byte map, else 0. Dumped at process exit.
static bool g_opProf = false;
static bool g_opProfInit = false;
static U64 g_opProfCount[256][256] = {};   // [op][op2]
static U64 g_opProf1[256] = {};            // single-byte totals

void cpu64DumpOpProfile() {
    if (!g_opProf) return;
    // Top single-byte opcodes.
    struct E { U32 op, op2; U64 n; };
    std::vector<E> v;
    for (U32 a = 0; a < 256; a++) {
        if (g_opProf1[a]) v.push_back({a, 0, g_opProf1[a]});
        for (U32 b = 0; b < 256; b++)
            if (g_opProfCount[a][b]) v.push_back({a, b, g_opProfCount[a][b]});
    }
    std::sort(v.begin(), v.end(), [](const E& x, const E& y){ return x.n > y.n; });
    klog("BW64_OPPROF: top opcodes by execution count (op[/op2] = count):");
    U64 total = 0; for (auto& e : v) total += e.n;
    for (size_t i = 0; i < v.size() && i < 40; i++) {
        if (v[i].op == 0x0F)
            klog_fmt("  0F %02x = %llu (%.1f%%)", v[i].op2,
                     (unsigned long long)v[i].n, total ? 100.0*v[i].n/total : 0.0);
        else
            klog_fmt("  %02x    = %llu (%.1f%%)", v[i].op,
                     (unsigned long long)v[i].n, total ? 100.0*v[i].n/total : 0.0);
    }
    klog_fmt("BW64_OPPROF: %llu instructions profiled", (unsigned long long)total);
}

// ---------------------------------------------------------------------------
// BW64_REFWATCH: refcount double-release canary for bug #2 (wineserver teardown
// heap corruption). The assert that aborts wineserver is
//     server/object.c:443  assert( obj->refcount );   // release_object()
// i.e. release_object() is called on a struct object whose refcount is ALREADY
// 0 — a double-release / use-after-free. (refcount is the first member, a 4-byte
// unsigned int at offset 0; ops* is at +8. Verified from the stripped wineserver64
// disassembly: release_object @ file-vaddr 0x30170 begins
//   53               push %rbx                ; function entry (we trap HERE)
//   8b 07            mov  (%rdi),%eax        ; eax = obj->refcount
//   85 c0            test %eax,%eax
//   74 xx            je   <assert_fail path>  ; refcount==0 -> abort.)
//
// WSBT (stack scan at the abort WRITE) couldn't name the culprit because the
// binary is stripped + frame-pointer-omitted. This catches the bug ONE
// instruction EARLIER — at release_object's entry, while %rdi still holds the
// dying object and [%rsp] still holds the CALLER's return address — which is the
// exact wineserver call site that released a dead object (the fix lead).
//
// Self-validating: rather than trust a load base, we confirm the 4 prologue
// bytes (8b 07 85 c0) at the probed address; default address is the fixed PIE
// base (0x400000000) + 0x30170, overridable via BW64_REFWATCH=0xADDR. Gated;
// one rip compare per instruction only when the env is set.
static bool g_refWatchInit = false;
static U64  g_refObjAddr = 0;          // absolute guest vaddr of release_object
static bool g_refValidated = false;    // prologue bytes confirmed at g_refObjAddr
// Small ring of recent release_object calls so that, at the fatal double-release,
// we can also show the object's PRIOR (legitimate, final) release + its caller.
struct RefRelRec { U64 obj; U64 caller; U32 refBefore; U32 pid; };
static RefRelRec g_refRing[64];
static U32       g_refRingNext = 0;
static std::mutex g_refRingMutex;

static void refWatchInit() {
    g_refWatchInit = true;
    const char* e = std::getenv("BW64_REFWATCH");
    if (!e) return;
    if (e[0] == '0' && (e[1] == 'x' || e[1] == 'X'))
        g_refObjAddr = std::strtoull(e, nullptr, 0);
    else
        g_refObjAddr = 0x400000000ULL + 0x30170ULL; // PIE base + release_object
}

U32 CPU64::step() {
    U64 ipStart = rip;
    fetchFault = false;
    fetchFaultMapped = false;

    // BW64_REFWATCH probe: catch wineserver's double-release at release_object
    // entry, one instruction before the assert. See the big comment above.
    if (!g_refWatchInit) refWatchInit();
    if (g_refObjAddr && rip == g_refObjAddr) {
        // Confirm we're really at release_object (prologue 8b 07 85 c0). Done
        // once; if the bytes don't match, the base guess is wrong — disarm so we
        // don't spam on a coincidental rip match.
        if (!g_refValidated) {
            // release_object entry: 53 (push %rbx) 8b 07 (mov (%rdi),%eax)
            // 85 c0 (test %eax,%eax). We trap the very first byte (entry), where
            // %rdi still holds the object and [%rsp] is the caller's return addr.
            U8 b0 = fetchByte(rip), b1 = fetchByte(rip + 1),
               b2 = fetchByte(rip + 2), b3 = fetchByte(rip + 3);
            if (b0 == 0x53 && b1 == 0x8b && b2 == 0x07 && b3 == 0x85) {
                g_refValidated = true;
                klog_fmt("REFWATCH: armed — release_object confirmed at 0x%llx (pid=%u %s)",
                         (unsigned long long)rip,
                         (unsigned)(thread && thread->process ? thread->process->id : 0),
                         (thread && thread->process) ? thread->process->name.c_str() : "?");
            } else {
                klog_fmt("REFWATCH: prologue mismatch at 0x%llx (got %02x %02x %02x %02x) — disarming; set BW64_REFWATCH=0x<release_object addr>",
                         (unsigned long long)rip, b0, b1, b2, b3);
                g_refObjAddr = 0;
            }
        }
        if (g_refObjAddr) {
            U64 obj    = reg[X64_RDI].u64;
            U32 refNow = memory ? memory->readd(obj) : 0;
            U64 caller = (memory && reg[X64_RSP].u64) ? memory->readq(reg[X64_RSP].u64) : 0;
            U32 pid    = (thread && thread->process) ? thread->process->id : 0;
            {
                std::lock_guard<std::mutex> lk(g_refRingMutex);
                if (refNow == 0) {
                    // THE BUG: releasing an object whose refcount is already 0.
                    U64 ops = memory ? memory->readq(obj + 8) : 0;
                    klog_fmt("REFWATCH: *** DOUBLE-RELEASE *** pid=%u obj=0x%llx refcount=0 "
                             "ops=0x%llx caller=0x%llx (caller-PIE=0x%llx)",
                             (unsigned)pid, (unsigned long long)obj,
                             (unsigned long long)ops, (unsigned long long)caller,
                             (unsigned long long)(caller >= 0x400000000ULL ? caller - 0x400000000ULL : caller));
                    // First 32 bytes of the dead object for ops/struct identification.
                    char hx[128] = {0};
                    for (int i = 0; i < 32; i++)
                        snprintf(hx + i * 3, 4, "%02x ", memory ? memory->readb(obj + i) : 0);
                    klog_fmt("REFWATCH:   obj[0..32]= %s", hx);
                    // Replay any earlier release of THIS object from the ring — the
                    // legitimate final release + its caller, to pair against this
                    // buggy one. (Newest-first.)
                    for (U32 k = 0; k < 64; k++) {
                        U32 idx = (g_refRingNext + 64 - 1 - k) % 64;
                        if (g_refRing[idx].obj == obj) {
                            klog_fmt("REFWATCH:   prior release of obj=0x%llx: caller=0x%llx (PIE=0x%llx) refBefore=%u pid=%u",
                                     (unsigned long long)obj,
                                     (unsigned long long)g_refRing[idx].caller,
                                     (unsigned long long)(g_refRing[idx].caller >= 0x400000000ULL ? g_refRing[idx].caller - 0x400000000ULL : g_refRing[idx].caller),
                                     g_refRing[idx].refBefore, g_refRing[idx].pid);
                        }
                    }
                    // Dump the wineserver write ring filtered to this object's
                    // body (refcount + list linkage) — shows every write to the
                    // dead object's next/prev/refcount + the issuing RIP, so we
                    // can tell a stray/duplicate LINK write (emulator-induced)
                    // from a genuine wine double-link. (BW64_MEMRING must be set.)
                    kmemory64DumpMemRing(obj);
                }
                // Record every release (including this fatal one) into the ring.
                g_refRing[g_refRingNext % 64] = { obj, caller, refNow, pid };
                g_refRingNext++;
            }
        }
    }

    Prefixes p;
    U32 opOff = consumePrefixes(p);
    bool rexW = (p.rex & 0x08) != 0;
    bool rexPresent = (p.rex != 0);

    // Default operand size in long mode is 32; REX.W → 64; 66h → 16.
    U32 opSize = rexW ? 8u : (p.osize16 ? 2u : 4u);

    U8 op = fetchByte(rip + opOff);

    // The prefix/opcode bytes came from a page with no backing buffer, either
    // absent from this address space or present only as a reservation.
    // Hardware faults on the fetch; the sparse reader would hand back zeros and
    // the decoder would happily execute them, so deliver the guest SIGSEGV
    // here, before the instruction has any effect. No zeros are ever executed.
    if (fetchFault) {
        const U64 faultAddr = fetchFaultAddr;
        const bool faultMapped = fetchFaultMapped;
        fetchFault = false;
        fetchFaultMapped = false;
        // Hard-capped: a handler that returns to the faulting RIP re-faults, so
        // this repeats for as long as the guest keeps trying. The first few
        // lines name the RIP that jumped into nothing, which is the whole
        // diagnostic value; after that the report is silent rather than
        // printing forever at a fixed interval.
        {
            static std::atomic<U64> faults {0};
            const U64 n = faults.fetch_add(1, std::memory_order_relaxed);
            if (n < CPU64_FETCH_NO_BACKING_REPORT_LIMIT) {
                klog_fmt("BOXEDWINE_X64_FETCH_FAULT pid=%u rip=0x%llx addr=0x%llx "
                         "mapped=%d count=%llu — instruction fetch from a page "
                         "with no backing",
                         (unsigned)(thread && thread->process ? thread->process->id : 0),
                         (unsigned long long)ipStart, (unsigned long long)faultAddr,
                         faultMapped ? 1 : 0, (unsigned long long)(n + 1));
            } else if (n == CPU64_FETCH_NO_BACKING_REPORT_LIMIT) {
                klog_fmt("BOXEDWINE_X64_FETCH_FAULT pid=%u further reports "
                         "suppressed after %u lines",
                         (unsigned)(thread && thread->process ? thread->process->id : 0),
                         CPU64_FETCH_NO_BACKING_REPORT_LIMIT);
            }
        }
        // A reservation the guest may not execute is an access error; a page
        // with no translation at all is a mapping error.
        const S32 siCode = faultMapped ? /*SEGV_ACCERR*/2 : /*SEGV_MAPERR*/1;
        if (this->raiseSyncFault(K_SIGSEGV, /*trapNo #PF*/14, siCode, faultAddr)) {
            return 0; // rip now at the handler
        }
        if (thread && thread->process) thread->process->signalProcess(K_SIGSEGV);
        yield = true;
        return 0;
    }

    if (!g_opProfInit) {
        g_opProfInit = true;
        g_opProf = std::getenv("BW64_OPPROF") != nullptr;
        if (g_opProf) std::atexit(cpu64DumpOpProfile);
    }
    if (g_opProf) {
        if (op == 0x0F) g_opProfCount[0x0F][fetchByte(rip + opOff + 1)]++;
        else g_opProf1[op]++;
    }

    // ---- Dispatch jump table ----
    // Generated by tools/gen_dispatch64.py from the if-cascade below: every
    // opcode value jumps to the FIRST block whose condition could possibly
    // match it (conditions with non-op terms still evaluate at the target and
    // fall through to later blocks exactly as before — only the provably
    // non-matching compares above the target are skipped). A native profile
    // had 34% of interpreter time in step(); the linear compare chain was a
    // large slice of that. Ops no block can match go straight to `unhandled`.
    // REGENERATE (do not hand-edit) after adding/reordering opcode blocks:
    //   python3 tools/gen_dispatch64.py
    // That generator is not in this tree. Until it is, an added block is
    // entered by hand under the same rule: its opcodes are removed from the
    // `unhandled` list and given a case naming the FIRST block that could
    // match them, and the block itself is placed so that the ops which used
    // to fall through to its successor still reach that successor.
    switch (op) {
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        goto dsp_0;
        case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
        goto dsp_1;
        case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
        goto dsp_2;
        case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
        goto dsp_3;
        case 0x89: goto dsp_4;
        case 0x8b: goto dsp_5;
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x08: case 0x09:
        case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x14: case 0x15: case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d:
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x28: case 0x29:
        case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x34: case 0x35: case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d:
        goto dsp_6;
        case 0x80: case 0x81: case 0x83: goto dsp_7;
        case 0x84: case 0x85: goto dsp_8;
        case 0x88: goto dsp_9;
        case 0x8a: goto dsp_10;
        case 0x8c: case 0x8e: goto dsp_11;
        case 0xc6: goto dsp_12;
        case 0xc7: goto dsp_13;
        case 0x8d: goto dsp_14;
        case 0x90: goto dsp_15;
        case 0x9b: goto dsp_16;
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: goto dsp_17;
        case 0x86: case 0x87: goto dsp_18;
        case 0x99: goto dsp_19;
        case 0x98: goto dsp_20;
        case 0xd7: goto dsp_21;
        case 0x68: case 0x6a: goto dsp_22;
        case 0x0f: goto dsp_23;
        case 0xc3: goto dsp_26;
        case 0xcf: goto dsp_27;
        case 0xa0: case 0xa1: case 0xa2: case 0xa3: goto dsp_28;
        case 0xa8: case 0xa9: goto dsp_29;
        case 0xf6: case 0xf7: goto dsp_30;
        case 0xfe: goto dsp_31;
        case 0xff: goto dsp_32;
        case 0x8f: goto dsp_33;
        case 0xe8: goto dsp_34;
        case 0xe9: goto dsp_35;
        case 0xeb: goto dsp_36;
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
        goto dsp_37;
        case 0x63: goto dsp_39;
        case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: goto dsp_40;
        case 0x69: case 0x6b: goto dsp_41;
        case 0xa4: case 0xa5: case 0xaa: case 0xab: goto dsp_42;
        case 0xa6: case 0xa7: case 0xae: case 0xaf: goto dsp_43;
        case 0xac: case 0xad: goto dsp_43b;
        case 0xc9: goto dsp_44;
        case 0x9c: goto dsp_45;
        case 0x9d: goto dsp_46;
        case 0x9e: goto dsp_47;
        case 0x9f: goto dsp_48;
        case 0xcc: case 0xcd: goto dsp_49;
        case 0xfc: goto dsp_50;
        case 0xfd: goto dsp_51;
        case 0xf5: goto dsp_52;
        case 0xf8: goto dsp_53;
        case 0xf9: goto dsp_54;
        case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf:
        goto dsp_58;
        case 0x06: case 0x07: case 0x0e: case 0x16: case 0x17: case 0x1e: case 0x1f: case 0x26:
        case 0x27: case 0x2e: case 0x2f: case 0x36: case 0x37: case 0x3e: case 0x3f: case 0x40:
        case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48:
        case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: case 0x60:
        case 0x61: case 0x62: case 0x64: case 0x65: case 0x66: case 0x67: case 0x6c: case 0x6d:
        case 0x6e: case 0x6f: case 0x82: case 0x9a: case 0xc2: case 0xc4:
        case 0xc5: case 0xc8: case 0xca: case 0xcb: case 0xce: case 0xd4: case 0xd5:
        case 0xd6: case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe4: case 0xe5: case 0xe6:
        case 0xe7: case 0xea: case 0xec: case 0xed: case 0xee: case 0xef: case 0xf0: case 0xf1:
        case 0xf2: case 0xf3: case 0xf4: case 0xfa: case 0xfb: goto unhandled;
    }

    // ---- Single-byte opcodes ----

    // PUSH r64 (50+rd). Always 64-bit in long mode (operand size override
    // ignored on near push of GPR).
dsp_0:
    if (op >= 0x50 && op <= 0x57) {
        U8 r = (U8)((op - 0x50) | ((p.rex & 0x01) ? 0x08 : 0));
        push64(reg[r].u64);
        rip += opOff + 1;
        return opOff + 1;
    }
    // POP r64 (58+rd).
dsp_1:
    if (op >= 0x58 && op <= 0x5F) {
        U8 r = (U8)((op - 0x58) | ((p.rex & 0x01) ? 0x08 : 0));
        reg[r].setU64(pop64());
        rip += opOff + 1;
        return opOff + 1;
    }

    // MOV r8, imm8 (B0+rb). REX.B extends the destination; absence of REX
    // means AH/BH/CH/DH for indices 4-7, REX present means SPL/BPL/SIL/DIL.
dsp_2:
    if (op >= 0xB0 && op <= 0xB7) {
        U8 r = (U8)((op - 0xB0) | ((p.rex & 0x01) ? 0x08 : 0));
        U8 imm = fetchByte(rip + opOff + 1);
        writeReg8(r, imm, rexPresent);
        rip += opOff + 2;
        return opOff + 2;
    }

    // MOV r64, imm64 (B8+rd with REX.W). Without REX.W this would be the
    // 32-bit imm form (also legal; handled in the !rexW branch below).
dsp_3:
    if (op >= 0xB8 && op <= 0xBF) {
        U8 r = (U8)((op - 0xB8) | ((p.rex & 0x01) ? 0x08 : 0));
        if (rexW) {
            U64 imm = fetchQword(rip + opOff + 1);
            reg[r].setU64(imm);
            rip += opOff + 1 + 8;
            return opOff + 1 + 8;
        } else if (p.osize16) {
            U16 imm = (U16)(fetchByte(rip + opOff + 1) |
                            ((U16)fetchByte(rip + opOff + 2) << 8));
            reg[r].setU16(imm);
            rip += opOff + 1 + 2;
            return opOff + 1 + 2;
        } else {
            U32 imm = fetchDword(rip + opOff + 1);
            reg[r].setU32(imm);   // zero-extends to 64
            rip += opOff + 1 + 4;
            return opOff + 1 + 4;
        }
    }

    // MOV r/m, r  (89 /r). Operand size from prefix/REX.W.
dsp_4:
    if (op == 0x89) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U64 src;
        switch (opSize) {
            case 1: src = readReg8(m.regField, rexPresent); break;
            case 2: src = reg[m.regField].u16; break;
            case 4: src = reg[m.regField].u32; break;
            default: src = reg[m.regField].u64; break;
        }
        storeRM(m, opSize, src, rexPresent);
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // MOV r, r/m  (8B /r). Reverse direction; same width rules.
dsp_5:
    if (op == 0x8B) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U64 val = loadRM(m, opSize, rexPresent);
        switch (opSize) {
            case 1: writeReg8(m.regField, (U8)val, rexPresent); break;
            case 2: reg[m.regField].setU16((U16)val); break;
            case 4: reg[m.regField].setU32((U32)val); break;
            case 8: reg[m.regField].setU64(val); break;
        }
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // ---- Hot-opcode fast dispatch (Milestone H) ----
    // These blocks were profiled (BW64_OPPROF) as the most-executed opcodes in a
    // real wine64 boot — the ALU r/r-and-r/m group, the 80/81/83 imm-ALU group,
    // and 84/85 TEST together are ~30% of all instructions, on top of the 89/8B
    // MOVs above (~27%). They were physically below ~30 other single-byte checks;
    // hoisting them here (they are disjoint from every check above — verified: no
    // earlier branch matches 00-3D / 80 / 81 / 83 / 84 / 85) cuts the average
    // dispatch scan from ~30 to ~5 for >55% of executions. Bodies are unchanged.

    // ALU r/r and r/m forms. 00/01/02/03 + 04/05 imm-acc, repeated per 8 bytes
    // for ADD/OR/ADC/SBB/AND/SUB/XOR/CMP. /6 and /7 rows are legacy long-mode-
    // invalid opcodes (filtered), and 0F is the two-byte escape.
dsp_6:
    if (op <= 0x3D && ((op & 0x06) != 0x06)) {
        U8 aluOp = (op >> 3) & 0x7;
        U8 form  = op & 0x7;
        if (form <= 3) {
            U32 size = (form & 1) ? opSize : 1;
            bool destIsRM = (form < 2);  // forms 0,1: dest is r/m; forms 2,3: dest is reg
            ModRM m = decodeModRM(rip + opOff + 1, p, 0);
            // `lock add`/`lock or`/`lock and`/... on a memory dest must be an
            // atomic RMW across host threads (glibc futex/lock words use these).
            std::unique_lock<std::recursive_mutex> atomicLock(cpu64AtomicLockFor(m.effAddr), std::defer_lock);
            if (p.lock && destIsRM && !m.isReg) atomicLock.lock();
            U64 a, b;
            if (destIsRM) {
                a = loadRM(m, size, rexPresent);
                b = (size == 1) ? readReg8(m.regField, rexPresent)
                  : (size == 2) ? (U64)reg[m.regField].u16
                  : (size == 4) ? (U64)reg[m.regField].u32
                                : reg[m.regField].u64;
            } else {
                a = (size == 1) ? readReg8(m.regField, rexPresent)
                  : (size == 2) ? (U64)reg[m.regField].u16
                  : (size == 4) ? (U64)reg[m.regField].u32
                                : reg[m.regField].u64;
                b = loadRM(m, size, rexPresent);
            }
            runAlu(aluOp, size, destIsRM, a, b, m, rexPresent);
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
        if (form == 4 || form == 5) {
            U32 size = (form == 4) ? 1 : opSize;
            U64 imm = 0;
            U32 immLen = 0;
            if (size == 1) {
                imm = fetchByte(rip + opOff + 1); immLen = 1;
            } else if (size == 2) {
                imm = (U16)(fetchByte(rip + opOff + 1) |
                            ((U16)fetchByte(rip + opOff + 2) << 8)); immLen = 2;
            } else if (size == 4) {
                imm = fetchDword(rip + opOff + 1); immLen = 4;
            } else {
                S32 i32 = (S32)fetchDword(rip + opOff + 1);
                imm = (U64)(S64)i32; immLen = 4;
            }
            U64 a = (size == 1) ? reg[X64_RAX].u8
                  : (size == 2) ? reg[X64_RAX].u16
                  : (size == 4) ? reg[X64_RAX].u32 : reg[X64_RAX].u64;
            ModRM fake;
            fake.isReg = true; fake.rmIndex = X64_RAX; fake.regField = 0;
            runAlu(aluOp, size, true, a, imm, fake, rexPresent);
            U32 used = opOff + 1 + immLen;
            rip += used;
            return used;
        }
    }

    // 80/81/83 immediate-group ALU. /digit selects the alu op.
dsp_7:
    if (op == 0x80 || op == 0x81 || op == 0x83) {
        U32 size = (op == 0x80) ? 1 : opSize;
        ModRM m = decodeModRM(rip + opOff + 1, p,
            (op == 0x81) ? (size == 2 ? 2 : 4) : 1);
        U8 aluOp = m.regField & 0x7;
        U64 imm = 0; U32 immLen = 0;
        U64 immAddr = rip + opOff + 1 + m.length;
        if (op == 0x80 || op == 0x83) {
            S8 i8 = (S8)fetchByte(immAddr);
            imm = (U64)(S64)i8; immLen = 1;
            if (size == 2) imm &= 0xFFFF;
            else if (size == 4) imm &= 0xFFFFFFFFULL;
        } else { // 0x81
            if (size == 2) {
                imm = (U16)(fetchByte(immAddr) | ((U16)fetchByte(immAddr + 1) << 8));
                immLen = 2;
            } else if (size == 4) {
                imm = fetchDword(immAddr); immLen = 4;
            } else {
                S32 i32 = (S32)fetchDword(immAddr);
                imm = (U64)(S64)i32; immLen = 4;
            }
        }
        std::unique_lock<std::recursive_mutex> atomicLock(cpu64AtomicLockFor(m.effAddr), std::defer_lock);
        if (p.lock && !m.isReg) atomicLock.lock();
        U64 a = loadRM(m, size, rexPresent);
        runAlu(aluOp, size, true, a, imm, m, rexPresent);
        U32 used = opOff + 1 + m.length + immLen;
        rip += used;
        return used;
    }

    // TEST r/m, r (84/85). Computes AND, sets flags, discards result.
dsp_8:
    if (op == 0x84 || op == 0x85) {
        U32 size = (op == 0x84) ? 1 : opSize;
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U64 a = loadRM(m, size, rexPresent);
        U64 b = (size == 1) ? readReg8(m.regField, rexPresent)
              : (size == 2) ? reg[m.regField].u16
              : (size == 4) ? reg[m.regField].u32 : reg[m.regField].u64;
        flagsLogic(rflags, a & b, size);
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // MOV r/m8, r8 (88 /r) and MOV r8, r/m8 (8A /r).
dsp_9:
    if (op == 0x88) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U8 src = readReg8(m.regField, rexPresent);
        storeRM(m, 1, src, rexPresent);
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }
dsp_10:
    if (op == 0x8A) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U8 val = (U8)loadRM(m, 1, rexPresent);
        writeReg8(m.regField, val, rexPresent);
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // MOV r/m16, Sreg (8C /r) and MOV Sreg, r/m16 (8E /r). In 64-bit long mode
    // the segment registers are vestigial: CS/SS/DS/ES carry the flat user-mode
    // selectors and FS/GS bases are set via arch_prctl (the selector value is
    // 0). The ModRM `reg` field selects the segment: 0=ES 1=CS 2=SS 3=DS 4=FS
    // 5=GS. wine's PE loader reads %cs (8C /1) to confirm it's running 64-bit,
    // so we must return the canonical Linux user selectors. Stores are 16-bit
    // (a register destination is zero-extended to the operand size per the SDM,
    // but the common encoding here targets memory or a 16-bit slot).
dsp_11:
    if (op == 0x8C || op == 0x8E) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        // Linux x86-64 user-mode selectors. FS/GS selectors are 0 (base via MSR).
        static const U16 kSegSel[6] = { 0x2b, 0x33, 0x2b, 0x2b, 0x00, 0x00 };
        U8 segIdx = m.regField & 0x7;
        if (op == 0x8C) {
            // FS and GS answer with whatever selector was actually loaded into
            // them (zero until something loads one, which is what a 64-bit
            // thread that only ever used arch_prctl reads); the other four are
            // the canonical Linux user-mode selectors.
            U16 sel = (segIdx == 4) ? seg.fs
                    : (segIdx == 5) ? seg.gs
                    : (segIdx < 6) ? kSegSel[segIdx] : 0;
            // Destination width: for a memory operand it's 16-bit; for a register
            // operand the selector is zero-extended to the operand size.
            if (m.isReg) {
                storeRM(m, opSize == 8 ? 8 : (opSize == 4 ? 4 : 2), (U64)sel, rexPresent);
            } else {
                storeRM(m, 2, (U64)sel, rexPresent);
            }
        } else {
            // MOV Sreg, r/m16 — loading a segment selector. In long mode this is
            // effectively a no-op for CS/SS/DS/ES (the descriptor cache is flat).
            //
            // FS and GS are the exception, and the reason this is not a discard
            // any more: a selector loaded into either of them takes its base
            // from the descriptor it indexes. Wine's WoW64 layer allocates that
            // descriptor through the legacy `int 0x80` gate and then writes FS
            // with this instruction before far-jumping into 32-bit code that
            // addresses its TEB through FS, so discarding the write left the
            // 32-bit thread reading its TEB at whatever arch_prctl had set.
            const U16 selector = (U16)loadRM(m, 2, rexPresent);
            if (segIdx == 4 || segIdx == 5) {
                loadSegmentSelector(segIdx == 4, selector);
            }
        }
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // MOV r/m8, imm8 (C6 /0). Discovered via real musl-static hello: the
    // x86_64-linux-musl startup sets a static byte flag (often
    // __environ_locked or a TLS-init guard) with this exact encoding.
dsp_12:
    if (op == 0xC6) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 1);
        if ((m.regField & 0x7) == 0) {
            U8 imm = fetchByte(rip + opOff + 1 + m.length);
            storeRM(m, 1, imm, rexPresent);
            U32 used = opOff + 1 + m.length + 1;
            rip += used;
            return used;
        }
    }

    // MOV r/m, imm  (C7 /0). imm is imm32 sign-extended for 64-bit, or
    // imm32 zero-extended for 32-bit, or imm16 for 16-bit.
dsp_13:
    if (op == 0xC7) {
        ModRM m = decodeModRM(rip + opOff + 1, p, opSize == 2 ? 2 : 4);
        // /0 is the only defined sub-op for C7; any other reg field falls
        // through to the unhandled diagnostic at the bottom.
        if ((m.regField & 0x7) == 0) {
            if (opSize == 2) {
                U16 imm = (U16)(fetchByte(rip + opOff + 1 + m.length) |
                                ((U16)fetchByte(rip + opOff + 1 + m.length + 1) << 8));
                storeRM(m, 2, imm, rexPresent);
                U32 used = opOff + 1 + m.length + 2;
                rip += used;
                return used;
            }
            S32 imm32 = (S32)fetchDword(rip + opOff + 1 + m.length);
            U64 val = (opSize == 8) ? (U64)(S64)imm32 : (U64)(U32)imm32;
            storeRM(m, opSize, val, rexPresent);
            U32 used = opOff + 1 + m.length + 4;
            rip += used;
            return used;
        }
    }

    // LEA r, m (8D /r). Effective address only — no memory access. opSize
    // controls how much of the computed address is written.
dsp_14:
    if (op == 0x8D) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        // LEA with reg src is undefined; falls through to unhandled.
        if (!m.isReg) {
            U64 addr = m.effAddr;
            switch (opSize) {
                case 2: reg[m.regField].setU16((U16)addr); break;
                case 4: reg[m.regField].setU32((U32)addr); break;
                case 8: reg[m.regField].setU64(addr); break;
            }
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
    }

    // NOP (90). 0x90 without REX prefix is also XCHG RAX,RAX which is a NOP.
    // With REX.B=1 it becomes XCHG R8,RAX — we fall through to the XCHG block
    // below in that case. opcode 0x90 with REX.B=0 is the plain NOP.
dsp_15:
    if (op == 0x90 && !(p.rex & 0x01)) {
        rip += opOff + 1;
        return opOff + 1;
    }

    // FWAIT / WAIT (9B). On hardware, waits for pending x87 exceptions to
    // be serviced. Our soft FPU has no asynchronous exceptions, so this
    // is a NOP. Discovered around x87 epilogue sequences emitted by
    // gcc/clang when a function returns long double or stores via FSTP m80.
dsp_16:
    if (op == 0x9B) {
        rip += opOff + 1;
        return opOff + 1;
    }

    // XCHG r, RAX (90+rd). XCHG R8-R15 with RAX when REX.B=1. The plain 0x90
    // case is handled above as NOP. opSize from prefix/REX.W.
dsp_17:
    if (op >= 0x90 && op <= 0x97) {
        U8 ri = (U8)((op - 0x90) | ((p.rex & 0x01) ? 0x08 : 0));
        if (ri == X64_RAX) {
            rip += opOff + 1;
            return opOff + 1;
        }
        U64 a, b;
        if (opSize == 2) { a = reg[X64_RAX].u16; b = reg[ri].u16; reg[X64_RAX].setU16((U16)b); reg[ri].setU16((U16)a); }
        else if (opSize == 4) { a = reg[X64_RAX].u32; b = reg[ri].u32; reg[X64_RAX].setU64((U32)b); reg[ri].setU64((U32)a); }
        else { a = reg[X64_RAX].u64; b = reg[ri].u64; reg[X64_RAX].setU64(b); reg[ri].setU64(a); }
        rip += opOff + 1;
        return opOff + 1;
    }

    // XCHG r/m, r (86 r/m8, 87 r/m). Atomic when used with LOCK; we're
    // single-threaded so plain swap is correct.
dsp_18:
    if (op == 0x86 || op == 0x87) {
        U32 size = (op == 0x86) ? 1 : opSize;
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        // XCHG with a memory operand is implicitly atomic on x86 (LOCK is
        // assumed); serialize it across host threads so glibc's spinlock
        // acquire (xchg [lock], eax) is correct under real threading.
        std::unique_lock<std::recursive_mutex> atomicLock(cpu64AtomicLockFor(m.effAddr), std::defer_lock);
        if (!m.isReg) atomicLock.lock();
        U64 a = loadRM(m, size, rexPresent);
        U64 b = (size == 1) ? readReg8(m.regField, rexPresent)
              : (size == 2) ? (U64)reg[m.regField].u16
              : (size == 4) ? (U64)reg[m.regField].u32
                            : reg[m.regField].u64;
        storeRM(m, size, b, rexPresent);
        if (size == 1) writeReg8(m.regField, (U8)a, rexPresent);
        else if (size == 2) reg[m.regField].setU16((U16)a);
        else if (size == 4) reg[m.regField].setU64((U32)a);
        else reg[m.regField].setU64(a);
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // CDQ/CQO (99). Sign-extend EAX/RAX into EDX/RDX. 16-bit form (with 66
    // prefix) is CWD: sign-extend AX into DX.
dsp_19:
    if (op == 0x99) {
        if (opSize == 2) {
            reg[X64_RDX].setU16((reg[X64_RAX].u16 & 0x8000) ? 0xFFFFu : 0u);
        } else if (opSize == 4) {
            reg[X64_RDX].setU64((reg[X64_RAX].u32 & 0x80000000u) ? 0xFFFFFFFFu : 0u);
        } else {
            reg[X64_RDX].setU64((reg[X64_RAX].u64 & 0x8000000000000000ULL) ? ~(U64)0 : 0ULL);
        }
        rip += opOff + 1;
        return opOff + 1;
    }

    // CBW/CWDE/CDQE (98). Sign-extend AL→AX (16), AX→EAX (32), EAX→RAX (64).
dsp_20:
    if (op == 0x98) {
        if (opSize == 2) reg[X64_RAX].setU16((U16)(S16)(S8)reg[X64_RAX].u8);
        else if (opSize == 4) reg[X64_RAX].setU64((U64)(U32)(S32)(S16)reg[X64_RAX].u16);
        else reg[X64_RAX].setU64((U64)(S64)(S32)reg[X64_RAX].u32);
        rip += opOff + 1;
        return opOff + 1;
    }

    // XLAT/XLATB (D7): AL = [RBX + AL] (zero-extended AL as a byte index). In
    // long mode the base is the full 64-bit RBX; no segment base is applied
    // (CS/DS/ES/SS are flat). An address-size override would use EBX, but
    // FreeType's table-driven code uses the default 64-bit form.
dsp_21:
    if (op == 0xD7) {
        U64 base = p.asize32 ? (U64)reg[X64_RBX].u32 : reg[X64_RBX].u64;
        U64 addr = base + (U64)reg[X64_RAX].u8;
        reg[X64_RAX].setU8(memory->readb(addr));
        rip += opOff + 1;
        return opOff + 1;
    }

    // PUSH imm8 (6A) / PUSH imm32 sign-ext (68). Always 64-bit push in long mode.
dsp_22:
    if (op == 0x6A || op == 0x68) {
        S64 imm = (op == 0x6A)
            ? (S64)(S8)fetchByte(rip + opOff + 1)
            : (S64)(S32)fetchDword(rip + opOff + 1);
        U32 immLen = (op == 0x6A) ? 1 : 4;
        U64 sp = reg[X64_RSP].u64 - 8;
        memory->writeq(sp, (U64)imm);
        reg[X64_RSP].setU64(sp);
        U32 used = opOff + 1 + immLen;
        rip += used;
        return used;
    }

    // SYSCALL (0F 05).
dsp_23:
    if (op == 0x0F && fetchByte(rip + opOff + 1) == 0x05) {
        // Per AMD64 ABI, SYSCALL clobbers RCX with the return RIP and R11
        // with RFLAGS. Userspace relies on this even when the syscall
        // succeeds — set both before transferring to the kernel layer.
        U64 syscallStartRip = rip;
        U64 nextRip = rip + opOff + 2;
        // Wine's PE-side NtXxx thunks choose between a raw `syscall` and an
        // indirect `call [0x7ffe1000]` to __wine_syscall_dispatcher, based on
        // the SystemCall flag at KUSER_SHARED_DATA+0x308. Wine sets that flag
        // on a host where it has wired the raw syscall to its own dispatcher;
        // BoxedWine has not, so the flag reads 0, every thunk falls into the
        // raw `syscall`, and RAX holds a *Windows NT* ordinal rather than a
        // Linux number. Dispatching that through the Linux table answers the
        // wrong question -- NT ordinal 227 is not clock_settime -- and the
        // -ENOSYS that comes back is retried forever.
        //
        // Rather than synthesize a dispatcher frame (its prolog pops the
        // return address and reads gs:[0x328], a fragile ABI to fake), flip
        // Wine's own decision: set the SystemCall flag so every later thunk
        // takes its native indirect path on its own, and steer this in-flight
        // thunk onto that path by resuming at the jne target.
        //
        // Recognition is by instruction shape, in wine_nt_syscall_stub.h, and
        // is shared with the FEX adapter so the two backends cannot disagree.
        // This used to gate on ntdll's old 0x170000000 image base; the Wine
        // in the x86-64 runtime maps these thunks near 0x6fffffca0000 and the
        // check silently stopped matching. A byte-and-control-flow match
        // cannot be fooled by address alone, which is what makes it safe to
        // apply to every SYSCALL rather than only to a chosen range.
        boxedvn::WineNtSyscallStub ntStub;
        if (boxedvn::matchWineNtSyscallStubInGuest(
                memory, syscallStartRip, reg[X64_RAX].u64, ntStub)) {
            boxedvn::setWineNtSystemCallFlag(memory);
            // The indirect path is a call, not a SYSCALL: Wine's dispatcher
            // still expects the NT call's first argument in RCX, which the
            // thunk's own `mov r10, rcx` preserved.
            reg[X64_RCX].setU64(reg[X64_R10].u64);
            // RAX keeps the NT ordinal; the dispatcher reads it from there.
            rip = ntStub.indirectPath;
            boxedvn::reportWineNtSyscallRedirect(
                ntStub, thread && thread->process ? (U32)thread->process->id : 0,
                thread ? (U32)thread->id : 0, "interp");
            return opOff + 2;
        }
        reg[X64_RCX].setU64(nextRip);
        reg[X64_R11].setU64((U64)rflags);
        rip = nextRip;
        syscallRip = syscallStartRip;
        ksyscall64(this);
        // Single-threaded cooperative park: a futex WAIT that had to block set
        // reExecuteSyscall and parked this thread. Rewind RIP to the SYSCALL so
        // it re-runs when the thread is rescheduled (RAX was not written). yield
        // has already been requested by the park so the run loop returns now.
        if (reExecuteSyscall) {
            reExecuteSyscall = false;
            rip = syscallStartRip;
        }
        return opOff + 2;
    }

    // ENDBR64 (F3 0F 1E FA) / ENDBR32 (F3 0F 1E FB) — CET indirect-branch
    // landing pads. We don't model shadow stacks / IBT, so they're NOPs.
    // glibc 2.36 emits ENDBR64 at the top of nearly every function, so this
    // is hit constantly the moment real glibc code runs. Also accept the
    // RDSSPQ/INCSSP-adjacent NOP form F3 0F 1E /r generically (modrm reg/reg)
    // as a NOP since none of the CET state is observable here.
    if (op == 0x0F && fetchByte(rip + opOff + 1) == 0x1E && p.rep == 0xF3) {
        U8 modrm = fetchByte(rip + opOff + 2);
        // ENDBR64=FA, ENDBR32=FB, plus the general reg/reg NOP-equivalent form.
        if (modrm == 0xFA || modrm == 0xFB || (modrm & 0xC0) == 0xC0) {
            U32 used = opOff + 3;
            rip += used;
            return used;
        }
    }

    // Multi-byte NOP: 0F 1F /0 (any ModR/M form, any length).
    if (op == 0x0F && fetchByte(rip + opOff + 1) == 0x1F) {
        ModRM m = decodeModRM(rip + opOff + 2, p, 0);
        if ((m.regField & 0x7) == 0) {
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
    }

    // PUSH FS (0F A0) / POP FS (0F A1) / PUSH GS (0F A8) / POP GS (0F A9).
    // The only other instructions that can write FS or GS -- LFS and LGS are
    // not encodable in long mode -- and the pop form is what a context restore
    // uses to put a WoW64 thread's 32-bit selector back. In long mode the stack
    // slot is a qword unless 66h asks for a word; the selector itself is always
    // the low 16 bits, and a load takes its base from the descriptor it names.
    if (op == 0x0F) {
        U8 segOp = fetchByte(rip + opOff + 1);
        if (segOp == 0xA0 || segOp == 0xA1 || segOp == 0xA8 || segOp == 0xA9) {
            bool isFs = (segOp == 0xA0 || segOp == 0xA1);
            bool isPop = (segOp == 0xA1 || segOp == 0xA9);
            U32 slot = p.osize16 ? 2u : 8u;
            if (isPop) {
                U16 selector;
                if (slot == 2) {
                    selector = memory ? memory->readw(reg[X64_RSP].u64) : 0;
                    reg[X64_RSP].setU64(reg[X64_RSP].u64 + 2);
                } else {
                    selector = (U16)pop64();
                }
                loadSegmentSelector(isFs, selector);
            } else {
                U16 selector = isFs ? seg.fs : seg.gs;
                if (slot == 2) {
                    reg[X64_RSP].setU64(reg[X64_RSP].u64 - 2);
                    if (memory) memory->writew(reg[X64_RSP].u64, selector);
                } else {
                    push64((U64)selector);
                }
            }
            U32 used = opOff + 2;
            rip += used;
            return used;
        }
    }

    // RET (C3) — near return, no immediate.
dsp_26:
    if (op == 0xC3) {
        rip = pop64();
        return opOff + 1;
    }

    // IRETQ (REX.W CF == 48 CF), and the non-promoted IRET (CF) / IRETD.
    // Wine's PE-side ntdll uses iretq to return from its user-mode exception
    // dispatcher (NtContinue / the KiUserExceptionDispatcher tail). In long
    // mode the interrupt stack frame is, from the top of stack downward:
    //   RIP, CS, RFLAGS, RSP, SS  (five entries).
    // Each entry is pushed as a qword for IRETQ (REX.W); for a 32-bit IRETD
    // (no REX.W) the entries are dwords. We don't model segmentation (CS/SS
    // are a flat-model approximation — see the CSGSFS handling in syscall64),
    // so we pop and discard CS/SS. RFLAGS is masked exactly like POPFQ (9D):
    // only the user-visible arithmetic/direction/IF bits are writable; the
    // reserved bits keep their fixed values. This is effectively a privileged
    // return-from-frame, structurally the same as restoreSignalFrame.
dsp_27:
    if (op == 0xCF) {
        const U32 writable = 0x00254FD5u; // CF PF AF ZF SF TF IF DF OF (matches POPFQ)
        if (rexW) {
            U64 newRip   = pop64();
            (void)         pop64();        // CS (ignored — flat model)
            U64 newFlags = pop64();
            U64 newRsp   = pop64();
            (void)         pop64();        // SS (ignored — flat model)
            rip = newRip;
            rflags = (rflags & ~writable) | ((U32)newFlags & writable);
            reg[X64_RSP].setU64(newRsp);
        } else {
            // 32-bit IRETD: dword entries off the (still 64-bit) stack.
            U64 sp = reg[X64_RSP].u64;
            U32 newEip   = memory->readd(sp);      sp += 4;
            (void)         memory->readd(sp);      sp += 4; // CS
            U32 newFlags = memory->readd(sp);      sp += 4;
            U32 newEsp   = memory->readd(sp);      sp += 4;
            (void)         memory->readd(sp);      sp += 4; // SS
            rip = (U64)newEip;
            rflags = (rflags & ~writable) | (newFlags & writable);
            reg[X64_RSP].setU64((U64)newEsp);
        }
        return opOff + 1;
    }

    // ---- ALU r/r-and-r/m (00-3D), 80/81/83 imm-ALU, and 84/85 TEST were
    // hoisted to the hot-opcode fast dispatch right after the 89/8B MOVs
    // (Milestone H) — they were profiled as ~30% of executions and no longer
    // pay the ~30-deep single-byte scan to reach here. See that block above.

    // TEST AL/AX/EAX/RAX, imm  (A8 ib / A9 iz).
    // A0/A1/A2/A3 — MOV AL/AX/EAX/RAX ↔ moffs (64-bit absolute address).
    // In long mode the displacement is always 8 bytes regardless of address-size
    // prefix. REX.W on A1/A3 makes the operand 64-bit; otherwise normal opSize
    // rules apply. AL/A0/A2 are byte-size; AX/EAX/RAX A1/A3 follow opSize.
    //
    // gcc emits these for `mov rax, [absolute_addr]` from PIC C code with
    // -fno-pic compiled against fixed-address globals, and for any static
    // ET_EXEC that addresses a known absolute symbol. Surfaced by
    // tools/buildMultiSegmentElf64.py via the unimpl-tracer.
dsp_28:
    if (op == 0xA0 || op == 0xA1 || op == 0xA2 || op == 0xA3) {
        U32 size = (op == 0xA0 || op == 0xA2) ? 1 : opSize;
        // Fetch the 8-byte absolute address that follows the opcode.
        U64 addr = (U64)fetchByte(rip + opOff + 1)
                 | ((U64)fetchByte(rip + opOff + 2) << 8)
                 | ((U64)fetchByte(rip + opOff + 3) << 16)
                 | ((U64)fetchByte(rip + opOff + 4) << 24)
                 | ((U64)fetchByte(rip + opOff + 5) << 32)
                 | ((U64)fetchByte(rip + opOff + 6) << 40)
                 | ((U64)fetchByte(rip + opOff + 7) << 48)
                 | ((U64)fetchByte(rip + opOff + 8) << 56);
        if (op == 0xA0 || op == 0xA1) {
            // load from [moffs] into AL/AX/EAX/RAX
            switch (size) {
                case 1: reg[X64_RAX].setU8(memory->readb(addr)); break;
                case 2: reg[X64_RAX].setU16(memory->readw(addr)); break;
                case 4: reg[X64_RAX].setU32(memory->readd(addr)); break;
                case 8: reg[X64_RAX].setU64(memory->readq(addr)); break;
            }
        } else {
            // store AL/AX/EAX/RAX into [moffs]
            switch (size) {
                case 1: memory->writeb(addr, reg[X64_RAX].u8); break;
                case 2: memory->writew(addr, reg[X64_RAX].u16); break;
                case 4: memory->writed(addr, reg[X64_RAX].u32); break;
                case 8: memory->writeq(addr, reg[X64_RAX].u64); break;
            }
        }
        U32 used = opOff + 1 + 8;
        rip += used;
        return used;
    }

dsp_29:
    if (op == 0xA8 || op == 0xA9) {
        U32 size = (op == 0xA8) ? 1 : opSize;
        U64 a = (size == 1) ? reg[X64_RAX].u8
              : (size == 2) ? reg[X64_RAX].u16
              : (size == 4) ? reg[X64_RAX].u32 : reg[X64_RAX].u64;
        U64 imm; U32 immLen;
        if (size == 1) { imm = fetchByte(rip + opOff + 1); immLen = 1; }
        else if (size == 2) {
            imm = (U16)(fetchByte(rip + opOff + 1) |
                        ((U16)fetchByte(rip + opOff + 2) << 8));
            immLen = 2;
        } else if (size == 4) {
            imm = fetchDword(rip + opOff + 1); immLen = 4;
        } else {
            S32 i32 = (S32)fetchDword(rip + opOff + 1);
            imm = (U64)(S64)i32; immLen = 4;
        }
        flagsLogic(rflags, a & imm, size);
        U32 used = opOff + 1 + immLen;
        rip += used;
        return used;
    }

    // F6/F7 — group 3: /0 TEST imm, /2 NOT, /3 NEG, /4 MUL, /5 IMUL, /6 DIV, /7 IDIV.
    // v1: only /0 TEST imm wired (needed by ld-linux); rest unimpl.
dsp_30:
    if (op == 0xF6 || op == 0xF7) {
        U32 size = (op == 0xF6) ? 1 : opSize;
        // Decode with NO trailing immediate: only the /0 TEST subform carries
        // an immediate, and the sub-opcode lives in the ModRM reg field which
        // we can't know until we decode. trailingImmBytes ONLY shifts the
        // RIP-relative effective address (disp is relative to end-of-insn), so
        // a blanket nonzero value mis-addresses the no-immediate subforms (DIV/
        // IDIV/NOT/NEG/MUL/IMUL) by the immediate width — a real bug that made
        // a RIP-relative `divq [rip+disp]` read 4 bytes past its operand. We
        // fix up the TEST operand's RIP-relative address below, once we know it.
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U8 sub = m.regField & 0x7;
        if (sub == 0) {
            U64 imm; U32 immLen;
            U64 immAddr = rip + opOff + 1 + m.length;
            if (size == 1) { imm = fetchByte(immAddr); immLen = 1; }
            else if (size == 2) {
                imm = (U16)(fetchByte(immAddr) | ((U16)fetchByte(immAddr + 1) << 8));
                immLen = 2;
            } else if (size == 4) {
                imm = fetchDword(immAddr); immLen = 4;
            } else {
                S32 i32 = (S32)fetchDword(immAddr);
                imm = (U64)(S64)i32; immLen = 4;
            }
            // RIP-relative TEST: the disp is relative to the address after the
            // whole instruction, which includes this immediate. decodeModRM
            // computed effAddr with trailing=0, so add the immediate length.
            if (m.isRipRel) m.effAddr += immLen;
            U64 a = loadRM(m, size, rexPresent);
            flagsLogic(rflags, a & imm, size);
            U32 used = opOff + 1 + m.length + immLen;
            rip += used;
            return used;
        }
        if (sub == 2) { // NOT — no flags affected
            U64 a = loadRM(m, size, rexPresent);
            storeRM(m, size, ~a, rexPresent);
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
        if (sub == 3) { // NEG — same as 0 - r/m, flags set per SUB.
            U64 a = loadRM(m, size, rexPresent);
            U64 r = (U64)0 - a;
            flagsSub(rflags, 0, a, r, size);
            // NEG sets CF = (operand != 0). flagsSub already does this when a != 0.
            storeRM(m, size, r, rexPresent);
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
        if (sub == 4 || sub == 5) {
            // MUL (/4 unsigned) and IMUL (/5 signed) one-operand form.
            //   byte:  AX        = AL  * r/m8
            //   word:  DX:AX     = AX  * r/m16
            //   dword: EDX:EAX   = EAX * r/m32   (RDX/RAX upper bits zeroed)
            //   qword: RDX:RAX   = RAX * r/m64
            // CF=OF set when the high half is non-zero (MUL) or doesn't match
            // sign-extension of the low half (IMUL). SF/ZF/AF/PF undefined.
            U64 a = loadRM(m, size, rexPresent);
            bool isSigned = (sub == 5);
            U64 lo = 0, hi = 0;
            bool overflow = false;
            if (size == 1) {
                if (isSigned) {
                    S16 prod = (S16)(S8)reg[X64_RAX].u8 * (S16)(S8)a;
                    reg[X64_RAX].setU16((U16)prod);
                    overflow = ((S8)(prod & 0xFF) != prod);
                } else {
                    U16 prod = (U16)reg[X64_RAX].u8 * (U16)(U8)a;
                    reg[X64_RAX].setU16(prod);
                    overflow = (prod >> 8) != 0;
                }
            } else if (size == 2) {
                if (isSigned) {
                    S32 prod = (S32)(S16)reg[X64_RAX].u16 * (S32)(S16)(U16)a;
                    lo = (U16)prod;
                    hi = (U16)(prod >> 16);
                    overflow = ((S16)lo != prod);
                } else {
                    U32 prod = (U32)reg[X64_RAX].u16 * (U32)(U16)a;
                    lo = (U16)prod;
                    hi = (U16)(prod >> 16);
                    overflow = hi != 0;
                }
                reg[X64_RAX].setU16((U16)lo);
                reg[X64_RDX].setU16((U16)hi);
            } else if (size == 4) {
                if (isSigned) {
                    S64 prod = (S64)(S32)reg[X64_RAX].u32 * (S64)(S32)(U32)a;
                    lo = (U32)prod;
                    hi = (U32)((U64)prod >> 32);
                    overflow = ((S32)lo != prod);
                } else {
                    U64 prod = (U64)reg[X64_RAX].u32 * (U64)(U32)a;
                    lo = (U32)prod;
                    hi = (U32)(prod >> 32);
                    overflow = hi != 0;
                }
                // 32-bit dest writes zero-extend the full 64-bit reg.
                reg[X64_RAX].setU64(lo);
                reg[X64_RDX].setU64(hi);
            } else {
                __uint128_t prod;
                if (isSigned) {
                    __int128 p = (__int128)(S64)reg[X64_RAX].u64 * (__int128)(S64)a;
                    prod = (__uint128_t)p;
                    lo = (U64)prod;
                    hi = (U64)(prod >> 64);
                    overflow = ((S64)lo != p);
                } else {
                    prod = (__uint128_t)reg[X64_RAX].u64 * (__uint128_t)a;
                    lo = (U64)prod;
                    hi = (U64)(prod >> 64);
                    overflow = hi != 0;
                }
                reg[X64_RAX].setU64(lo);
                reg[X64_RDX].setU64(hi);
            }
            rflags &= ~(X64_CF | X64_OF);
            if (overflow) rflags |= (X64_CF | X64_OF);
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
        if (sub == 6 || sub == 7) {
            // DIV (/6 unsigned) and IDIV (/7 signed) one-operand form.
            //   byte:  AL  = AX        / r/m8 ;  AH = remainder
            //   word:  AX  = DX:AX     / r/m16; DX = remainder
            //   dword: EAX = EDX:EAX   / r/m32; EDX = remainder
            //   qword: RAX = RDX:RAX   / r/m64; RDX = remainder
            // Divide by zero or quotient overflow → #DE. We currently just
            // skip the write and continue; full #DE delivery is a TODO.
            U64 a = loadRM(m, size, rexPresent);
            if (a == 0) {
                // #DE: divide error. Deliver SIGFPE/FPE_INTDIV at this RIP so
                // the guest's exception handling runs exactly as on real
                // hardware (wine maps it to EXCEPTION_INT_DIVIDE_BY_ZERO and
                // its vectored/SEH handlers decide what to do). rip still
                // points at the DIV, which is what the handler's ucontext and
                // si_addr must capture.
                if (getenv("BW64_DIVZERO")) {
                    U64 ea = m.isReg ? 0 : m.effAddr;
                    // For the libX11 _XrmInternalStringToQuark rehash divide,
                    // the divisor global sits at ea; the table mask is ea+0x10
                    // and the table pointer at ea-0x3240. Dump them to see
                    // whether the table is live while the divisor reads 0.
                    U64 mask = (!m.isReg) ? memory->readq(ea + 0x10) : 0;
                    U64 tptr = (!m.isReg) ? memory->readq(ea - 0x3240) : 0;
                    klog_fmt("CPU64: #DE divide-by-zero RIP=0x%llx size=%u sub=%u "
                             "RAX=0x%llx RDX=0x%llx RCX=0x%llx RBX=0x%llx "
                             "ea=0x%llx [ea]=0x%llx mask@+0x10=0x%llx tptr@-0x3240=0x%llx",
                             (unsigned long long)rip, size, sub,
                             (unsigned long long)reg[X64_RAX].u64,
                             (unsigned long long)reg[X64_RDX].u64,
                             (unsigned long long)reg[X64_RCX].u64,
                             (unsigned long long)reg[X64_RBX].u64,
                             (unsigned long long)ea,
                             (unsigned long long)(m.isReg ? 0 : memory->readq(ea)),
                             (unsigned long long)mask, (unsigned long long)tptr);
                }
                if (this->raiseSyncFault(K_SIGFPE, /*trapNo #DE*/0,
                                         K_FPE_INTDIV, rip)) {
                    return 0; // rip now at the handler
                }
                // No SIGFPE handler installed — terminate like the kernel's
                // default action for an uncaught #DE instead of spinning on a
                // re-executed faulting DIV.
                klog_fmt("CPU64: unhandled #DE at RIP=0x%llx (no SIGFPE handler) "
                         "— terminating thread", (unsigned long long)rip);
                if (thread) thread->process->signalProcess(K_SIGFPE);
                yield = true;
                return 0;
            }
            bool isSigned = (sub == 7);
            if (size == 1) {
                U16 num = reg[X64_RAX].u16;
                if (isSigned) {
                    S16 sn = (S16)num;
                    S8 d = (S8)a;
                    S16 q = sn / d;
                    S16 rem = sn % d;
                    reg[X64_RAX].setU8((U8)(S8)q);
                    reg[X64_RAX].setH8((U8)(S8)rem);
                } else {
                    U16 q = num / (U8)a;
                    U16 rem = num % (U8)a;
                    reg[X64_RAX].setU8((U8)q);
                    reg[X64_RAX].setH8((U8)rem);
                }
            } else if (size == 2) {
                U32 num = ((U32)reg[X64_RDX].u16 << 16) | reg[X64_RAX].u16;
                if (isSigned) {
                    S32 sn = (S32)num;
                    S16 d = (S16)a;
                    reg[X64_RAX].setU16((U16)(S16)(sn / d));
                    reg[X64_RDX].setU16((U16)(S16)(sn % d));
                } else {
                    reg[X64_RAX].setU16((U16)(num / (U16)a));
                    reg[X64_RDX].setU16((U16)(num % (U16)a));
                }
            } else if (size == 4) {
                U64 num = ((U64)reg[X64_RDX].u32 << 32) | reg[X64_RAX].u32;
                if (isSigned) {
                    S64 sn = (S64)num;
                    S32 d = (S32)a;
                    reg[X64_RAX].setU64((U64)(U32)(sn / d));
                    reg[X64_RDX].setU64((U64)(U32)(sn % d));
                } else {
                    reg[X64_RAX].setU64((U64)(U32)(num / (U32)a));
                    reg[X64_RDX].setU64((U64)(U32)(num % (U32)a));
                }
            } else {
                // 128/64 -> 64 quotient. Use __int128.
                __uint128_t num = ((__uint128_t)reg[X64_RDX].u64 << 64) | reg[X64_RAX].u64;
                if (isSigned) {
                    __int128 sn = (__int128)num;
                    S64 d = (S64)a;
                    reg[X64_RAX].setU64((U64)(sn / d));
                    reg[X64_RDX].setU64((U64)(sn % d));
                } else {
                    reg[X64_RAX].setU64((U64)(num / (__uint128_t)a));
                    reg[X64_RDX].setU64((U64)(num % (__uint128_t)a));
                }
            }
            // DIV/IDIV leave flags undefined per Intel SDM.
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
        goto unhandled;
    }

    // FE — group 4: INC r/m8 (/0) and DEC r/m8 (/1). Byte-only sibling of the
    // FF group; all other /digit values are invalid encodings. wineserver's
    // startup hits this (INC byte ptr [rip+disp32] on a refcount/flag byte).
dsp_31:
    if (op == 0xFE) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 1);
        U8 sub = m.regField & 0x7;
        if (sub == 0 || sub == 1) {
            U64 a = loadRM(m, 1, rexPresent);
            U64 r = (sub == 0) ? a + 1 : a - 1;
            // INC/DEC preserve CF; other flags per ADD/SUB of 1.
            U32 savedCF = rflags & X64_CF;
            if (sub == 0) flagsAdd(rflags, a, 1, r, 1);
            else          flagsSub(rflags, a, 1, r, 1);
            rflags = (rflags & ~X64_CF) | savedCF;
            storeRM(m, 1, r, rexPresent);
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
        goto unhandled;
    }

    // INC/DEC r/m via FF /0 and /1. (Single-byte 40-4F encodings are REX
    // in long mode and are already consumed by the prefix loop.)
dsp_32:
    if (op == 0xFF) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U8 sub = m.regField & 0x7;
        if (sub == 0 || sub == 1) {
            U64 a = loadRM(m, opSize, rexPresent);
            U64 r = (sub == 0) ? a + 1 : a - 1;
            // INC/DEC don't affect CF; preserve it. Other flags per ADD/SUB.
            U32 savedCF = rflags & X64_CF;
            if (sub == 0) flagsAdd(rflags, a, 1, r, opSize);
            else          flagsSub(rflags, a, 1, r, opSize);
            rflags = (rflags & ~X64_CF) | savedCF;
            storeRM(m, opSize, r, rexPresent);
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
        if (sub == 2) { // CALL r/m (always 64-bit operand in long mode)
            U64 target = loadRM(m, 8, rexPresent);
            U32 used = opOff + 1 + m.length;
            U64 nextRip = rip + used;
            push64(nextRip);
            rip = target;
            return used;
        }
        if (sub == 4) { // JMP r/m (always 64-bit)
            U64 target = loadRM(m, 8, rexPresent);
            rip = target;
            return opOff + 1 + m.length; // bytes consumed regardless
        }
        if (sub == 6) { // PUSH r/m
            U64 v = loadRM(m, 8, rexPresent);
            push64(v);
            U32 used = opOff + 1 + m.length;
            rip += used;
            return used;
        }
        goto unhandled;
    }

    // 8F /0 — POP r/m (operand size always 64 in long mode for POP).
dsp_33:
    if (op == 0x8F) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        if ((m.regField & 0x7) != 0) goto unhandled;
        U64 v = pop64();
        storeRM(m, 8, v, rexPresent);
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // ---- Control flow ----

    // CALL rel32 (E8 cd). Pushes RIP-of-next-instruction; jumps to
    // RIP-of-next + sign_ext(disp32).
dsp_34:
    if (op == 0xE8) {
        S32 disp = (S32)fetchDword(rip + opOff + 1);
        U32 used = opOff + 1 + 4;
        U64 nextRip = rip + used;
        push64(nextRip);
        rip = nextRip + (U64)(S64)disp;
        return used;
    }

    // JMP rel32 (E9 cd).
dsp_35:
    if (op == 0xE9) {
        S32 disp = (S32)fetchDword(rip + opOff + 1);
        U32 used = opOff + 1 + 4;
        rip = rip + used + (U64)(S64)disp;
        return used;
    }

    // JMP rel8 (EB cb).
dsp_36:
    if (op == 0xEB) {
        S8 disp = (S8)fetchByte(rip + opOff + 1);
        U32 used = opOff + 1 + 1;
        rip = rip + used + (U64)(S64)disp;
        return used;
    }

    // Jcc rel8 (70-7F). Condition encoded in low 4 bits.
dsp_37:
    if (op >= 0x70 && op <= 0x7F) {
        S8 disp = (S8)fetchByte(rip + opOff + 1);
        U32 used = opOff + 1 + 1;
        rip = evalCC(op & 0xF) ? (rip + used + (U64)(S64)disp) : (rip + used);
        return used;
    }

    // Jcc rel32 (0F 80-8F). Same condition encoding.
    if (op == 0x0F) {
        U8 op2 = fetchByte(rip + opOff + 1);
        if (op2 >= 0x80 && op2 <= 0x8F) {
            S32 disp = (S32)fetchDword(rip + opOff + 2);
            U32 used = opOff + 2 + 4;
            rip = evalCC(op2 & 0xF) ? (rip + used + (U64)(S64)disp) : (rip + used);
            return used;
        }

        // 0F 40-4F — CMOVcc r, r/m. Same condition encoding as Jcc.
        if (op2 >= 0x40 && op2 <= 0x4F) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 src = loadRM(m, opSize, rexPresent);
            if (evalCC(op2 & 0xF)) {
                switch (opSize) {
                    case 2: reg[m.regField].setU16((U16)src); break;
                    case 4: reg[m.regField].setU32((U32)src); break;
                    case 8: reg[m.regField].setU64(src); break;
                }
            } else if (opSize == 4) {
                // Important x86-64 quirk: even when CMOVcc is NOT taken, the
                // 32-bit operand-size form still zero-extends the destination
                // (because the destination is the 32-bit name of the reg,
                // and *any* write to a 32-bit name zero-extends). So we
                // must write back the existing low 32 bits.
                reg[m.regField].setU32(reg[m.regField].u32);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F 90-9F — SETcc r/m8.
        if (op2 >= 0x90 && op2 <= 0x9F) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U8 val = evalCC(op2 & 0xF) ? 1 : 0;
            storeRM(m, 1, val, rexPresent);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F AF /r — IMUL r, r/m (two-operand). Signed multiply.
        if (op2 == 0xAF) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 a = (opSize == 2) ? reg[m.regField].u16
                  : (opSize == 4) ? reg[m.regField].u32 : reg[m.regField].u64;
            U64 b = loadRM(m, opSize, rexPresent);
            // Sign-extend operands to do a proper signed multiply.
            S64 sa, sb;
            if (opSize == 2) { sa = (S64)(S16)a; sb = (S64)(S16)b; }
            else if (opSize == 4) { sa = (S64)(S32)a; sb = (S64)(S32)b; }
            else { sa = (S64)a; sb = (S64)b; }
            S64 r = sa * sb;
            // CF/OF set if signed result doesn't fit in opSize.
            bool overflow = false;
            if (opSize == 2) overflow = (r != (S64)(S16)r);
            else if (opSize == 4) overflow = (r != (S64)(S32)r);
            else { __int128 r128 = (__int128)sa * (__int128)sb; overflow = (r128 != (__int128)r); }
            rflags &= ~(X64_CF | X64_OF);
            if (overflow) rflags |= X64_CF | X64_OF;
            switch (opSize) {
                case 2: reg[m.regField].setU16((U16)r); break;
                case 4: reg[m.regField].setU32((U32)r); break;
                case 8: reg[m.regField].setU64((U64)r); break;
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F BC /r — BSF (bit scan forward). ZF=1 if src==0, else dest=index.
        // 0F BD /r — BSR (bit scan reverse).
        if (op2 == 0xBC || op2 == 0xBD) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 src = loadRM(m, opSize, rexPresent) & maskFor(opSize);
            if (src == 0) {
                rflags |= X64_ZF;
                // Destination value is architecturally undefined; leave as-is.
            } else {
                rflags &= ~X64_ZF;
                U32 idx = 0;
                if (op2 == 0xBC) { while (((src >> idx) & 1) == 0) idx++; }
                else { idx = opSize * 8 - 1; while (((src >> idx) & 1) == 0) idx--; }
                switch (opSize) {
                    case 2: reg[m.regField].setU16((U16)idx); break;
                    case 4: reg[m.regField].setU32(idx); break;
                    case 8: reg[m.regField].setU64(idx); break;
                }
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F B6 /r — MOVZX r, r/m8.  0F B7 /r — MOVZX r, r/m16.
        if (op2 == 0xB6 || op2 == 0xB7) {
            U32 srcSize = (op2 == 0xB6) ? 1 : 2;
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 v = loadRM(m, srcSize, rexPresent);
            switch (opSize) {
                case 2: reg[m.regField].setU16((U16)v); break;
                case 4: reg[m.regField].setU32((U32)v); break;
                case 8: reg[m.regField].setU64(v); break;
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F BE /r — MOVSX r, r/m8.  0F BF /r — MOVSX r, r/m16.
        if (op2 == 0xBE || op2 == 0xBF) {
            U32 srcSize = (op2 == 0xBE) ? 1 : 2;
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 raw = loadRM(m, srcSize, rexPresent);
            S64 v = (srcSize == 1) ? (S64)(S8)raw : (S64)(S16)raw;
            switch (opSize) {
                case 2: reg[m.regField].setU16((U16)v); break;
                case 4: reg[m.regField].setU32((U32)v); break;
                case 8: reg[m.regField].setU64((U64)v); break;
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F C8+rd — BSWAP r32 / r64. Reverses byte order. 16-bit form is
        // architecturally undefined; we treat it as zero-extend of low byte
        // swap which matches what most CPUs do (and glibc never emits the 16
        // form).
        if (op2 >= 0xC8 && op2 <= 0xCF) {
            U8 ri = (U8)((op2 - 0xC8) | ((p.rex & 0x01) ? 0x08 : 0));
            if (opSize == 8) {
                U64 v = reg[ri].u64;
                v = __builtin_bswap64(v);
                reg[ri].setU64(v);
            } else {
                U32 v = reg[ri].u32;
                v = __builtin_bswap32(v);
                reg[ri].setU64(v);
            }
            rip += opOff + 2;
            return opOff + 2;
        }

        // 0F B0 /r — CMPXCHG r/m8, r8.  0F B1 /r — CMPXCHG r/m, r.
        // If AL/AX/EAX/RAX == r/m, store r into r/m and set ZF=1. Otherwise
        // load r/m into AL/AX/EAX/RAX and clear ZF. Flags follow the implicit
        // SUB of acc vs r/m.
        if (op2 == 0xB0 || op2 == 0xB1) {
            U32 size = (op2 == 0xB0) ? 1 : opSize;
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            // CMPXCHG is glibc's mutex fast path; make the read-compare-write
            // atomic across host threads when the operand is in memory.
            std::unique_lock<std::recursive_mutex> atomicLock(cpu64AtomicLockFor(m.effAddr), std::defer_lock);
            if (!m.isReg) atomicLock.lock();
            U64 dest = loadRM(m, size, rexPresent);
            U64 acc = (size == 1) ? reg[X64_RAX].u8
                    : (size == 2) ? (U64)reg[X64_RAX].u16
                    : (size == 4) ? (U64)reg[X64_RAX].u32
                                  : reg[X64_RAX].u64;
            U64 cmpRes = acc - dest;
            flagsSub(rflags, acc, dest, cmpRes, size);
            if (acc == dest) {
                U64 src = (size == 1) ? readReg8(m.regField, rexPresent)
                        : (size == 2) ? (U64)reg[m.regField].u16
                        : (size == 4) ? (U64)reg[m.regField].u32
                                      : reg[m.regField].u64;
                storeRM(m, size, src, rexPresent);
            } else {
                if (size == 1) reg[X64_RAX].setU8((U8)dest);
                else if (size == 2) reg[X64_RAX].setU16((U16)dest);
                else if (size == 4) reg[X64_RAX].setU64((U32)dest);
                else reg[X64_RAX].setU64(dest);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F A3 /r — BT  r/m, r       (test bit, no modify)
        // 0F AB /r — BTS r/m, r       (set bit, return old in CF)
        // 0F B3 /r — BTR r/m, r       (reset bit)
        // 0F BB /r — BTC r/m, r       (complement bit)
        if (op2 == 0xA3 || op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 v = loadRM(m, opSize, rexPresent);
            U64 idx = (opSize == 8) ? reg[m.regField].u64 :
                      (opSize == 4) ? (U64)reg[m.regField].u32 :
                                      (U64)reg[m.regField].u16;
            U64 bit = idx & (opSize * 8 - 1);
            U64 mask = 1ULL << bit;
            bool old = (v & mask) != 0;
            rflags &= ~X64_CF;
            if (old) rflags |= X64_CF;
            if (op2 != 0xA3) {
                U64 nv = v;
                if (op2 == 0xAB) nv |= mask;
                else if (op2 == 0xB3) nv &= ~mask;
                else nv ^= mask; // BTC
                storeRM(m, opSize, nv, rexPresent);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F BA /r — group with imm8: /4 BT  /5 BTS  /6 BTR  /7 BTC
        if (op2 == 0xBA) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            U8 sub = m.regField & 0x7;
            if (sub >= 4 && sub <= 7) {
                U64 v = loadRM(m, opSize, rexPresent);
                U8 imm = fetchByte(rip + opOff + 2 + m.length);
                U64 bit = imm & (opSize * 8 - 1);
                U64 mask = 1ULL << bit;
                bool old = (v & mask) != 0;
                rflags &= ~X64_CF;
                if (old) rflags |= X64_CF;
                if (sub != 4) {
                    U64 nv = v;
                    if (sub == 5) nv |= mask;
                    else if (sub == 6) nv &= ~mask;
                    else nv ^= mask;
                    storeRM(m, opSize, nv, rexPresent);
                }
                U32 used = opOff + 2 + m.length + 1;
                rip += used;
                return used;
            }
        }

        // 0F A4 /r ib — SHLD r/m, r, imm8 (double-precision shift left)
        // 0F A5 /r    — SHLD r/m, r, CL
        // 0F AC /r ib — SHRD r/m, r, imm8
        // 0F AD /r    — SHRD r/m, r, CL
        if (op2 == 0xA4 || op2 == 0xA5 || op2 == 0xAC || op2 == 0xAD) {
            bool isLeft = (op2 == 0xA4 || op2 == 0xA5);
            bool hasImm = (op2 == 0xA4 || op2 == 0xAC);
            ModRM m = decodeModRM(rip + opOff + 2, p, hasImm ? 1 : 0);
            U64 dest = loadRM(m, opSize, rexPresent);
            U64 src = (opSize == 8) ? reg[m.regField].u64 :
                      (opSize == 4) ? (U64)reg[m.regField].u32 :
                                      (U64)reg[m.regField].u16;
            U8 count;
            U32 immLen = 0;
            if (hasImm) {
                count = fetchByte(rip + opOff + 2 + m.length);
                immLen = 1;
            } else {
                count = reg[X64_RCX].u8;
            }
            count &= (opSize == 8) ? 0x3F : 0x1F;
            U64 mask = maskFor(opSize);
            dest &= mask; src &= mask;
            if (count != 0) {
                U64 result;
                bool cf;
                if (isLeft) {
                    cf = (dest >> (opSize * 8 - count)) & 1;
                    result = ((dest << count) | (src >> (opSize * 8 - count))) & mask;
                } else {
                    cf = (dest >> (count - 1)) & 1;
                    result = ((dest >> count) | (src << (opSize * 8 - count))) & mask;
                }
                rflags &= ~(X64_CF | X64_OF);
                if (cf) rflags |= X64_CF;
                setSZP(rflags, result, opSize);
                storeRM(m, opSize, result, rexPresent);
            }
            U32 used = opOff + 2 + m.length + immLen;
            rip += used;
            return used;
        }

        // F3 0F B8 — POPCNT r, r/m  (REP prefix selects POPCNT vs BSF)
        // F3 0F BC — TZCNT (vs 0F BC BSF)
        // F3 0F BD — LZCNT (vs 0F BD BSR)
        if ((op2 == 0xB8 && p.rep == 0xF3) ||
            (op2 == 0xBC && p.rep == 0xF3) ||
            (op2 == 0xBD && p.rep == 0xF3)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 src = loadRM(m, opSize, rexPresent) & maskFor(opSize);
            U64 result;
            if (op2 == 0xB8) { // POPCNT
                result = __builtin_popcountll(src);
            } else if (op2 == 0xBC) { // TZCNT
                result = src ? __builtin_ctzll(src) : opSize * 8;
            } else { // LZCNT
                U32 bits = opSize * 8;
                result = src ? (__builtin_clzll(src) - (64 - bits)) : bits;
            }
            if (opSize == 8) reg[m.regField].setU64(result);
            else if (opSize == 4) reg[m.regField].setU64((U32)result);
            else reg[m.regField].setU16((U16)result);
            rflags &= ~(X64_ZF | X64_CF | X64_OF | X64_SF | X64_PF | X64_AF);
            if (op2 == 0xB8) { if (result == 0) rflags |= X64_ZF; }
            else if (op2 == 0xBC) { if (src == 0) rflags |= X64_CF; if ((result & maskFor(opSize)) == 0) rflags |= X64_ZF; }
            else { if (src == 0) rflags |= X64_CF; if ((result & maskFor(opSize)) == 0) rflags |= X64_ZF; }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F BC — BSF r, r/m  (bit scan forward); 0F BD — BSR (reverse).
        if (op2 == 0xBC || op2 == 0xBD) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 src = loadRM(m, opSize, rexPresent) & maskFor(opSize);
            rflags &= ~X64_ZF;
            if (src == 0) {
                rflags |= X64_ZF;
                // dest undefined; leave unchanged
            } else {
                U64 idx = (op2 == 0xBC) ? __builtin_ctzll(src)
                                        : (63 - __builtin_clzll(src));
                if (opSize == 8) reg[m.regField].setU64(idx);
                else if (opSize == 4) reg[m.regField].setU64((U32)idx);
                else reg[m.regField].setU16((U16)idx);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F 31 — RDTSC. Returns guest "cycles" in EDX:EAX. We just shove the
        // host microsecond counter in; precision doesn't matter for early boot.
        if (op2 == 0x31) {
            U64 tsc = KSystem::getSystemTimeAsMicroSeconds();
            reg[X64_RAX].setU64((U32)(tsc & 0xFFFFFFFF));
            reg[X64_RDX].setU64((U32)(tsc >> 32));
            rip += opOff + 2;
            return opOff + 2;
        }

        // 0F C0 /r — XADD r/m8, r8.  0F C1 /r — XADD r/m, r.
        // tmp = r/m + r;  r = r/m;  r/m = tmp. Flags follow the ADD.
        if (op2 == 0xC0 || op2 == 0xC1) {
            U32 size = (op2 == 0xC0) ? 1 : opSize;
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            // XADD (LOCK XADD) backs glibc's atomic counters; serialize the
            // memory RMW across host threads.
            std::unique_lock<std::recursive_mutex> atomicLock(cpu64AtomicLockFor(m.effAddr), std::defer_lock);
            if (!m.isReg) atomicLock.lock();
            U64 d = loadRM(m, size, rexPresent);
            U64 s = (size == 1) ? readReg8(m.regField, rexPresent)
                  : (size == 2) ? (U64)reg[m.regField].u16
                  : (size == 4) ? (U64)reg[m.regField].u32
                                : reg[m.regField].u64;
            U64 sum = d + s;
            flagsAdd(rflags, d, s, sum, size);
            // src reg <- old r/m
            if (size == 1) writeReg8(m.regField, (U8)d, rexPresent);
            else if (size == 2) reg[m.regField].setU16((U16)d);
            else if (size == 4) reg[m.regField].setU64((U32)d);
            else reg[m.regField].setU64(d);
            storeRM(m, size, sum, rexPresent);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // 0F C7 /1 with REX.W - CMPXCHG16B m128.
        //
        // Wine's ntdll reaches this five bytes into a hot path
        // (f0 49 0f c7 08 = lock cmpxchg16b [r8]) and the device log stopped
        // there with "unimpl opcode". The 128-bit memory value is compared
        // against RDX:RAX; on equality RCX:RBX is stored and ZF is set, and on
        // inequality memory is left alone and loaded into RDX:RAX with ZF
        // clear. The caller here follows it with `sete r10b`, so ZF is the
        // architectural result that matters; the other status flags are
        // documented as undefined for this instruction, so no flagsSub.
        //
        // /1 requires a memory operand: the register form is invalid and must
        // not be silently executed as something else. Without REX.W the same
        // encoding is CMPXCHG8B, which nothing has needed yet.
        if (op2 == 0xC7 && opSize == 8) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if ((m.regField & 7) == 1) {
                if (!boxedvn::isCmpxchg16bEncoding(m.regField, opSize == 8,
                                                   m.isReg)) {
                    // /1 has no register form. Report it as unimplemented
                    // rather than inventing a meaning; returning a zero length
                    // here would spin the interpreter.
                    goto unhandled;
                }
                // The whole compare-read-write is one operation as far as
                // other threads are concerned. Take the lock for LOCK and
                // without it alike: the guest is reading and writing 16 bytes
                // through two 8-byte accesses either way, and a torn result is
                // never the right answer.
                std::unique_lock<std::recursive_mutex> atomicLock(
                    cpu64AtomicLockFor(m.effAddr), std::defer_lock);
                atomicLock.lock();
                const U64 memLow = memory->readq(m.effAddr);
                const U64 memHigh = memory->readq(m.effAddr + 8);
                const boxedvn::Cmpxchg16bResult swap =
                    boxedvn::evaluateCmpxchg16b(memLow, memHigh,
                                                reg[X64_RAX].u64,
                                                reg[X64_RDX].u64,
                                                reg[X64_RBX].u64,
                                                reg[X64_RCX].u64);
                if (swap.succeeded) {
                    memory->writeq(m.effAddr, swap.writeLow);
                    memory->writeq(m.effAddr + 8, swap.writeHigh);
                }
                // On failure memory is untouched and the caller is handed what
                // it actually found, which is how a compare-and-swap loop
                // makes progress.
                reg[X64_RAX].setU64(swap.raxAfter);
                reg[X64_RDX].setU64(swap.rdxAfter);
                if (swap.zeroFlag) rflags |= X64_ZF;
                else rflags &= ~X64_ZF;
                U32 used = opOff + 2 + m.length;
                rip += used;
                return used;
            }
        }
    }

    // 63 /r — MOVSXD r64, r/m32 (with REX.W). Without REX.W it acts like
    // MOV r32, r/m32 (Intel: deprecated form). We support both.
dsp_39:
    if (op == 0x63) {
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U64 raw = loadRM(m, 4, rexPresent);
        if (rexW) {
            S64 v = (S64)(S32)raw;
            reg[m.regField].setU64((U64)v);
        } else {
            reg[m.regField].setU32((U32)raw);
        }
        U32 used = opOff + 1 + m.length;
        rip += used;
        return used;
    }

    // Shift group — D0/D1/D2/D3/C0/C1 with /digit selecting the op:
    //   /4 SHL  /5 SHR  /6 alias SHL  /7 SAR
    //   /0 ROL  /1 ROR  /2 RCL  /3 RCR
    // D0/C0 = byte form; D1/C1 = opSize form. D0/D1 shift by 1; D2/D3 shift
    // by CL; C0/C1 shift by imm8.
dsp_40:
    if (op == 0xD0 || op == 0xD1 || op == 0xD2 || op == 0xD3 ||
        op == 0xC0 || op == 0xC1) {
        U32 size = (op == 0xD0 || op == 0xD2 || op == 0xC0) ? 1 : opSize;
        bool hasImm = (op == 0xC0 || op == 0xC1);
        ModRM m = decodeModRM(rip + opOff + 1, p, hasImm ? 1 : 0);
        U8 sub = m.regField & 0x7;
        if (sub <= 7) {
            U8 count;
            U32 immLen = 0;
            if (op == 0xD0 || op == 0xD1) {
                count = 1;
            } else if (op == 0xD2 || op == 0xD3) {
                count = reg[X64_RCX].u8;
            } else {
                count = fetchByte(rip + opOff + 1 + m.length);
                immLen = 1;
            }
            // Mask count: 5 bits for 8/16/32, 6 bits for 64.
            count &= (size == 8) ? 0x3F : 0x1F;
            U64 v = loadRM(m, size, rexPresent);
            U64 r = doShift(rflags, sub, v, count, size);
            if (count != 0) storeRM(m, size, r, rexPresent);
            else if (size == 4 && m.isReg) {
                // Zero-extend the 32-bit destination even on count==0.
                reg[m.rmIndex].setU32((U32)v);
            }
            U32 used = opOff + 1 + m.length + immLen;
            rip += used;
            return used;
        }
    }

    // IMUL r, r/m, imm (69 iz / 6B ib). Three-operand signed multiply.
dsp_41:
    if (op == 0x69 || op == 0x6B) {
        ModRM m = decodeModRM(rip + opOff + 1, p,
            (op == 0x69) ? (opSize == 2 ? 2 : 4) : 1);
        U64 a = loadRM(m, opSize, rexPresent);
        S64 imm;
        U32 immLen;
        U64 immAddr = rip + opOff + 1 + m.length;
        if (op == 0x6B) {
            imm = (S64)(S8)fetchByte(immAddr); immLen = 1;
        } else if (opSize == 2) {
            imm = (S64)(S16)(fetchByte(immAddr) |
                             ((U16)fetchByte(immAddr + 1) << 8));
            immLen = 2;
        } else {
            imm = (S64)(S32)fetchDword(immAddr); immLen = 4;
        }
        S64 sa = (opSize == 2) ? (S64)(S16)a
               : (opSize == 4) ? (S64)(S32)a : (S64)a;
        S64 r = sa * imm;
        bool overflow = false;
        if (opSize == 2) overflow = (r != (S64)(S16)r);
        else if (opSize == 4) overflow = (r != (S64)(S32)r);
        else { __int128 r128 = (__int128)sa * (__int128)imm; overflow = (r128 != (__int128)r); }
        rflags &= ~(X64_CF | X64_OF);
        if (overflow) rflags |= X64_CF | X64_OF;
        switch (opSize) {
            case 2: reg[m.regField].setU16((U16)r); break;
            case 4: reg[m.regField].setU32((U32)r); break;
            case 8: reg[m.regField].setU64((U64)r); break;
        }
        U32 used = opOff + 1 + m.length + immLen;
        rip += used;
        return used;
    }

    // String ops — MOVS and STOS. Address size in long mode defaults to 64
    // (RSI/RDI/RCX); with 0x67 prefix it's ESI/EDI/ECX. Direction flag DF
    // controls increment vs decrement. With REP (F3), repeat until RCX==0.
    //
    // MOVSB/MOVSW/MOVSD/MOVSQ — A4 (byte), A5 (opSize).
    // STOSB/STOSW/STOSD/STOSQ — AA (byte), AB (opSize). Source is RAX.
dsp_42:
    if (op == 0xA4 || op == 0xA5 || op == 0xAA || op == 0xAB) {
        U32 size = (op == 0xA4 || op == 0xAA) ? 1 : opSize;
        bool isStos = (op == 0xAA || op == 0xAB);
        S64 step = (rflags & X64_DF) ? -(S64)size : (S64)size;
        U64 count = (p.rep != 0) ? reg[X64_RCX].u64 : 1;
        if (p.asize32) count &= 0xFFFFFFFFULL;
        while (count--) {
            U64 src = reg[X64_RSI].u64;
            U64 dst = reg[X64_RDI].u64;
            if (p.asize32) { src &= 0xFFFFFFFFULL; dst &= 0xFFFFFFFFULL; }
            U64 val;
            if (isStos) {
                val = (size == 1) ? reg[X64_RAX].u8
                    : (size == 2) ? reg[X64_RAX].u16
                    : (size == 4) ? reg[X64_RAX].u32 : reg[X64_RAX].u64;
            } else {
                switch (size) {
                    case 1: val = memory->readb(src); break;
                    case 2: val = memory->readw(src); break;
                    case 4: val = memory->readd(src); break;
                    default: val = memory->readq(src); break;
                }
            }
            switch (size) {
                case 1: memory->writeb(dst, (U8)val); break;
                case 2: memory->writew(dst, (U16)val); break;
                case 4: memory->writed(dst, (U32)val); break;
                case 8: memory->writeq(dst, val); break;
            }
            if (!isStos) reg[X64_RSI].setU64(reg[X64_RSI].u64 + (U64)step);
            reg[X64_RDI].setU64(reg[X64_RDI].u64 + (U64)step);
        }
        if (p.rep != 0) reg[X64_RCX].setU64(0);
        rip += opOff + 1;
        return opOff + 1;
    }

    // CMPS / SCAS — string compare. CMPS compares [RSI] vs [RDI]; SCAS
    // compares AL/AX/EAX/RAX vs [RDI]. Both update flags as a CMP and step
    // RSI/RDI by ±size. REPE (F3) repeats while ZF=1; REPNE (F2) while ZF=0;
    // both also break when RCX reaches 0.
    //   CMPSB/CMPSW/CMPSD/CMPSQ — A6 / A7
    //   SCASB/SCASW/SCASD/SCASQ — AE / AF
dsp_43:
    if (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) {
        U32 size = (op == 0xA6 || op == 0xAE) ? 1 : opSize;
        bool isScas = (op == 0xAE || op == 0xAF);
        S64 step = (rflags & X64_DF) ? -(S64)size : (S64)size;
        U64 count = (p.rep != 0) ? reg[X64_RCX].u64 : 1;
        if (p.asize32) count &= 0xFFFFFFFFULL;
        bool stopOnZF = (p.rep == 0xF3); // REPE
        bool stopOnNZ = (p.rep == 0xF2); // REPNE
        U64 lhs = 0, rhs = 0;
        while (count > 0) {
            count--;
            if (isScas) {
                lhs = (size == 1) ? reg[X64_RAX].u8
                    : (size == 2) ? reg[X64_RAX].u16
                    : (size == 4) ? reg[X64_RAX].u32 : reg[X64_RAX].u64;
            } else {
                U64 src = reg[X64_RSI].u64;
                if (p.asize32) src &= 0xFFFFFFFFULL;
                switch (size) {
                    case 1: lhs = memory->readb(src); break;
                    case 2: lhs = memory->readw(src); break;
                    case 4: lhs = memory->readd(src); break;
                    default: lhs = memory->readq(src); break;
                }
            }
            U64 dst = reg[X64_RDI].u64;
            if (p.asize32) dst &= 0xFFFFFFFFULL;
            switch (size) {
                case 1: rhs = memory->readb(dst); break;
                case 2: rhs = memory->readw(dst); break;
                case 4: rhs = memory->readd(dst); break;
                default: rhs = memory->readq(dst); break;
            }
            U64 diff = lhs - rhs;
            flagsSub(rflags, lhs, rhs, diff, size);
            if (!isScas) reg[X64_RSI].setU64(reg[X64_RSI].u64 + (U64)step);
            reg[X64_RDI].setU64(reg[X64_RDI].u64 + (U64)step);
            if (p.rep != 0) {
                bool zfNow = (rflags & X64_ZF) != 0;
                if (stopOnZF && !zfNow) break;
                if (stopOnNZ && zfNow) break;
            }
        }
        if (p.rep != 0) reg[X64_RCX].setU64(count);
        rip += opOff + 1;
        return opOff + 1;
    }

    // LODS — load string. AL/AX/EAX/RAX = [RSI], then RSI steps by ±size.
    //   LODSB — AC (byte)
    //   LODSW/LODSD/LODSQ — AD (opSize)
    //
    // The one member of the string family the interpreter did not decode. A
    // device run stopped a helper at "unimpl opcode ... bytes=ac eb df 5b 5e",
    // which is `lodsb; jmp short -0x21` -- the read half of a hand-written
    // byte loop, the same shape as the STOS/SCAS loops already here.
    //
    // REP is architecturally permitted and useless: every iteration but the
    // last is overwritten, so the visible result is the final element and
    // RCX=0. It is implemented the way MOVS/STOS above implement it rather
    // than special-cased, so a guest that emits it behaves the same as on
    // hardware. Flags are untouched: LODS is a move, not an arithmetic op.
dsp_43b:
    if (op == 0xAC || op == 0xAD) {
        U32 size = (op == 0xAC) ? 1 : opSize;
        S64 step = (rflags & X64_DF) ? -(S64)size : (S64)size;
        U64 count = (p.rep != 0) ? reg[X64_RCX].u64 : 1;
        if (p.asize32) count &= 0xFFFFFFFFULL;
        while (count--) {
            U64 src = reg[X64_RSI].u64;
            if (p.asize32) src &= 0xFFFFFFFFULL;
            switch (size) {
                // Each width writes the register the way the rest of the
                // interpreter does: a byte or word load leaves the upper bits
                // of RAX alone, a dword load zero-extends to 64.
                case 1: reg[X64_RAX].setU8(memory->readb(src)); break;
                case 2: reg[X64_RAX].setU16(memory->readw(src)); break;
                case 4: reg[X64_RAX].setU32(memory->readd(src)); break;
                default: reg[X64_RAX].setU64(memory->readq(src)); break;
            }
            reg[X64_RSI].setU64(reg[X64_RSI].u64 + (U64)step);
        }
        if (p.rep != 0) reg[X64_RCX].setU64(0);
        rip += opOff + 1;
        return opOff + 1;
    }

    // LEAVE (C9). Equivalent to: RSP = RBP; RBP = pop64().
dsp_44:
    if (op == 0xC9) {
        reg[X64_RSP].setU64(reg[X64_RBP].u64);
        reg[X64_RBP].setU64(pop64());
        rip += opOff + 1;
        return opOff + 1;
    }

    // PUSHFQ (9C) / POPFQ (9D). PUSHFQ pushes the low 32 bits of rflags
    // extended to 64; POPFQ pops 64 bits but only the low 32 carry the
    // user-visible flags. We mirror that: writeable mask covers the
    // arithmetic and direction flags + IF.
dsp_45:
    if (op == 0x9C) {
        push64((U64)rflags);
        rip += opOff + 1;
        return opOff + 1;
    }
dsp_46:
    if (op == 0x9D) {
        U64 v = pop64();
        const U32 writable = 0x00254FD5u; // CF PF AF ZF SF TF IF DF OF + others
        rflags = (rflags & ~writable) | ((U32)v & writable);
        rip += opOff + 1;
        return opOff + 1;
    }

    // SAHF (9E). Load the low 8 bits of rflags from AH. Only SF/ZF/AF/PF/CF
    // are user-visible in those bits per AMD64; bit 1 reads as 1.
dsp_47:
    if (op == 0x9E) {
        U8 ah = (U8)((reg[X64_RAX].u64 >> 8) & 0xFF);
        const U32 mask = X64_SF | X64_ZF | X64_AF | X64_PF | X64_CF;
        rflags = (rflags & ~mask) | ((U32)ah & mask);
        rip += opOff + 1;
        return opOff + 1;
    }

    // LAHF (9F). Store the low 8 bits of rflags (SF/ZF/0/AF/0/PF/1/CF) into
    // AH. Bit 1 is always 1, bits 3/5 are always 0.
dsp_48:
    if (op == 0x9F) {
        U8 v = (U8)(rflags & (X64_SF | X64_ZF | X64_AF | X64_PF | X64_CF));
        v |= 0x02; // reserved bit 1 reads as 1
        U64 rax = reg[X64_RAX].u64;
        rax = (rax & ~0xFF00ULL) | ((U64)v << 8);
        reg[X64_RAX].setU64(rax);
        rip += opOff + 1;
        return opOff + 1;
    }

    // Software interrupts: INT3 (CC) and INT imm8 (CD ib).
    //
    // Three different things share this encoding, and only one of them is an
    // error:
    //
    //   - `int $0x80` is the legacy i386 syscall gate. A 64-bit process is not
    //     supposed to need it, but Wine's unix ntdll allocates a WoW64 thread's
    //     32-bit FS/GS selectors through it (there is no x86-64 syscall number
    //     for set_thread_area), and that runs in every process that starts a
    //     32-bit program. The interpreter serves it through the same kernel
    //     helper the translator backend's fault handler uses, so the two
    //     backends cannot answer the same instruction differently.
    //   - INT3, and the two-byte `int $3`, are breakpoints. Wine's debugger
    //     support plants them and expects EXCEPTION_BREAKPOINT, which is a
    //     SIGTRAP with TRAP_BRKPT here. The processor reports the address AFTER
    //     the trapping instruction, so rip is advanced before the frame is
    //     built -- Wine's own handler subtracts the one byte back.
    //   - Every other vector is not a user gate: the processor raises a general
    //     protection fault and the kernel turns it into SIGSEGV/SI_KERNEL.
    //
    // None of the three is an unimplemented opcode, which is what all of them
    // used to be (or, for INT3, a silent yield that left the process alive with
    // no status at all).
dsp_49:
    if (op == 0xCC || op == 0xCD) {
        const U8 vector = (op == 0xCC) ? 3 : fetchByte(rip + opOff + 1);
        const U32 used = opOff + 1 + (op == 0xCD ? 1u : 0u);
        if (op == 0xCD && vector == 0x80) {
            // The helper reads the number and arguments out of the register
            // file, performs the call, and writes the i386 answer into rax. It
            // deliberately leaves rip alone: the gate resumes after its two
            // bytes, which is what `used` already counts.
            boxedvn::kernelLegacySyscall64(this);
            rip += used;
            return used;
        }
        // Both endings report the address AFTER the instruction, which is what
        // the processor pushes for a software interrupt; a guest cannot
        // usefully re-execute one, and Wine's breakpoint handler subtracts the
        // byte back itself.
        rip += used;
        if (vector == 3) {
            if (this->raiseSyncFault(K_SIGTRAP, /*trapNo #BP*/3, K_TRAP_BRKPT,
                                     rip)) {
                return 0; // rip now at the handler
            }
            klog_fmt("CPU64: INT3 at RIP=0x%llx with no SIGTRAP handler pid=%d",
                     (unsigned long long)ipStart,
                     (thread && thread->process) ? (int)thread->process->id : -1);
            if (thread && thread->process) {
                thread->process->signaled = K_SIGTRAP;
            }
            kfatalProcessExit64(this, 128 + K_SIGTRAP, "cpu64-breakpoint-trap");
            yield = true;
            return 0;
        }
        if (this->raiseSyncFault(K_SIGSEGV, /*trapNo #GP*/13, K_SI_KERNEL, 0)) {
            return 0; // rip now at the handler
        }
        klog_fmt("CPU64: INT 0x%02x at RIP=0x%llx with no SIGSEGV handler pid=%d",
                 (unsigned)vector, (unsigned long long)ipStart,
                 (thread && thread->process) ? (int)thread->process->id : -1);
        if (thread && thread->process) {
            thread->process->signaled = K_SIGSEGV;
        }
        kfatalProcessExit64(this, 128 + K_SIGSEGV, "cpu64-software-interrupt");
        yield = true;
        return 0;
    }

    // CLD (FC) / STD (FD). Direction flag for string ops.
dsp_50:
    if (op == 0xFC) { rflags &= ~X64_DF; rip += opOff + 1; return opOff + 1; }
dsp_51:
    if (op == 0xFD) { rflags |=  X64_DF; rip += opOff + 1; return opOff + 1; }

    // CMC (F5) / CLC (F8) / STC (F9). Carry-flag toggle/clear/set.
dsp_52:
    if (op == 0xF5) { rflags ^= X64_CF; rip += opOff + 1; return opOff + 1; }
dsp_53:
    if (op == 0xF8) { rflags &= ~X64_CF; rip += opOff + 1; return opOff + 1; }
dsp_54:
    if (op == 0xF9) { rflags |=  X64_CF; rip += opOff + 1; return opOff + 1; }

    // CPUID (0F A2). Return a conservative feature set: SSE2 only, no SSE3+,
    // no AVX. glibc IFUNC dispatchers consult ECX feature bits and select the
    // simpler implementations when AVX/SSSE3 are clear, which keeps us off
    // unimplemented vector opcodes during memcpy/strcmp.
    if (op == 0x0F && fetchByte(rip + opOff + 1) == 0xA2) {
        U32 leaf = reg[X64_RAX].u32;
        U32 sub  = reg[X64_RCX].u32;
        U32 a = 0, b = 0, c = 0, d = 0;
        switch (leaf) {
            case 0x0:
                a = 0x1; // max leaf
                // "Genu" "ineI" "ntel"
                b = 0x756E6547; d = 0x49656E69; c = 0x6C65746E;
                break;
            case 0x1: {
                a = (6 << 8) | (15 << 4) | 3; // family 6 stepping 3
                b = 0;
                // ECX feature bits: SSE3=0, SSSE3=0, SSE4.1=0, SSE4.2=0,
                // POPCNT=0 (bit23), XSAVE=0, OSXSAVE=0, AVX=0.
                c = 0;
                // EDX bits: FPU=0 PDE=1 TSC=4 MSR=5 PAE=6 MCE=7 CX8=8
                // APIC=9 SEP=11 MTRR=12 PGE=13 MCA=14 CMOV=15 PAT=16
                // PSE36=17 CLFSH=19 MMX=23 FXSR=24 SSE=25 SSE2=26.
                d = (1u<<0)|(1u<<4)|(1u<<5)|(1u<<8)|(1u<<15)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<26);
                break;
            }
            case 0x80000000:
                a = 0x80000001;
                break;
            case 0x80000001:
                // EDX bit 29 = LM (long mode), bit 11 = SYSCALL, bit 20 = NX.
                d = (1u<<29)|(1u<<11)|(1u<<20);
                break;
            default:
                break;
        }
        (void)sub;
        reg[X64_RAX].setU64(a);
        reg[X64_RBX].setU64(b);
        reg[X64_RCX].setU64(c);
        reg[X64_RDX].setU64(d);
        rip += opOff + 2;
        return opOff + 2;
    }

    // ---- SSE2 (minimum-viable subset for ld.so + glibc) ----
    //
    // Encodings: 0F xx with optional 0x66 (packed-integer) or 0xF3/0xF2
    // (scalar/string) prefix. We handle the moves and PXOR — that covers
    // SSE register zeroing and 16-byte stack-aligned loads/stores used by
    // ld-linux during relocation.
    if (op == 0x0F) {
        U8 op2 = fetchByte(rip + opOff + 1);
        bool osize66 = p.osize16; // for SSE this means "use packed-integer form"
        // MOVAPS/MOVAPD xmm, xmm/m128       — 0F 28 (load) / 0F 29 (store)
        // MOVUPS/MOVUPD                     — 0F 10/11
        // MOVDQA xmm, xmm/m128              — 66 0F 6F (load) / 66 0F 7F (store)
        // MOVDQU                            — F3 0F 6F / F3 0F 7F
        // All do a 16-byte aligned-or-unaligned memcpy. We treat them
        // identically — no #GP on misalignment.
        //
        // Carve out F2/F3 prefixed variants of 0x10/0x11 — those are the
        // scalar SSE2 MOVSD/MOVSS forms, handled in the scalar-FP block
        // further down, not as 16-byte moves.
        bool isScalarPrefixed10or11 =
            (op2 == 0x10 || op2 == 0x11) && (p.rep == 0xF2 || p.rep == 0xF3);
        bool isMove128 =
            ((op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29) && !isScalarPrefixed10or11) ||
            ((op2 == 0x6F || op2 == 0x7F) && (osize66 || p.rep == 0xF3));
        if (isMove128) {
            bool isStore = (op2 == 0x11 || op2 == 0x29 || op2 == 0x7F);
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (isStore) {
                // dst = r/m, src = xmm[regField]
                if (m.isReg) {
                    xmm[m.rmIndex] = xmm[m.regField];
                } else {
                    memory->writeq(m.effAddr,     xmm[m.regField].lo);
                    memory->writeq(m.effAddr + 8, xmm[m.regField].hi);
                }
            } else {
                // dst = xmm[regField], src = r/m
                if (m.isReg) {
                    xmm[m.regField] = xmm[m.rmIndex];
                } else {
                    xmm[m.regField].lo = memory->readq(m.effAddr);
                    xmm[m.regField].hi = memory->readq(m.effAddr + 8);
                }
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PXOR xmm, xmm/m128 — 66 0F EF /r. Used everywhere for register
        // zeroing (faster than MOV 0).
        // XORPS xmm, xmm/m128 — 0F 57 /r (no prefix). Float-typed bitwise
        // XOR; bit pattern identical to PXOR. Discovered in musl printf
        // init — used to zero XMM0 before a varargs scalar-FP write.
        // XORPD xmm, xmm/m128 — 66 0F 57 /r. Same bit op, double-typed.
        // Packed bitwise-logical float ops — all operate on the full 128 bits;
        // PS vs PD differ only in the 0x66 prefix, irrelevant to a bit op:
        //   0F 54 ANDPS/ANDPD    dst &=  src
        //   0F 55 ANDNPS/ANDNPD  dst = ~dst & src
        //   0F 56 ORPS/ORPD      dst |=  src
        //   0F 57 XORPS/XORPD    dst ^=  src   ;  66 0F EF PXOR (same op)
        // glibc __printf_fp uses ANDPD to mask the sign bit (fabs) for %f.
        if (op2 == 0x54 || op2 == 0x55 || op2 == 0x56 || op2 == 0x57 ||
            (op2 == 0xEF && osize66)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            switch (op2) {
                case 0x54: xmm[m.regField].lo = dLo & srcLo;  xmm[m.regField].hi = dHi & srcHi;  break;
                case 0x55: xmm[m.regField].lo = ~dLo & srcLo; xmm[m.regField].hi = ~dHi & srcHi; break;
                case 0x56: xmm[m.regField].lo = dLo | srcLo;  xmm[m.regField].hi = dHi | srcHi;  break;
                default:   xmm[m.regField].lo = dLo ^ srcLo;  xmm[m.regField].hi = dHi ^ srcHi;  break;
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // MOVD/MOVQ — scalar move between GPR and XMM.
        //   66 0F 6E /r  MOVD xmm, r/m32     (or MOVQ xmm, r/m64 with REX.W)
        //   66 0F 7E /r  MOVD r/m32, xmm     (or MOVQ with REX.W)
        //   F3 0F 7E /r  MOVQ xmm, xmm/m64   (load low qword, zero high)
        if ((op2 == 0x6E || op2 == 0x7E) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            bool isStore = (op2 == 0x7E);
            bool wide = rexW;
            U32 width = wide ? 8 : 4;
            if (isStore) {
                U64 v = wide ? xmm[m.regField].lo : (xmm[m.regField].lo & 0xFFFFFFFFULL);
                storeRM(m, width, v, rexPresent);
            } else {
                U64 v = loadRM(m, width, rexPresent);
                if (!wide) v &= 0xFFFFFFFFULL;
                xmm[m.regField].lo = v;
                xmm[m.regField].hi = 0;
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        if (op2 == 0x7E && p.rep == 0xF3) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 lo;
            if (m.isReg) {
                lo = xmm[m.rmIndex].lo;
            } else {
                lo = memory->readq(m.effAddr);
            }
            xmm[m.regField].lo = lo;
            xmm[m.regField].hi = 0;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PCMPEQB xmm, xmm/m128 — 66 0F 74 /r. Compare 16 bytes; each byte
        // of dst becomes 0xFF if equal, 0x00 if not. glibc's strlen / strchr
        // / memchr loop on this.
        if ((op2 == 0x74 || op2 == 0x75 || op2 == 0x76) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dstLo = xmm[m.regField].lo;
            U64 dstHi = xmm[m.regField].hi;
            // op2 == 0x74 → bytes, 0x75 → words, 0x76 → dwords.
            U32 elemBits = (op2 == 0x74) ? 8 : (op2 == 0x75) ? 16 : 32;
            U32 elemCount = 128 / elemBits;
            U64 mask = (elemBits == 64) ? ~0ULL : ((1ULL << elemBits) - 1);
            U64 outLo = 0, outHi = 0;
            for (U32 i = 0; i < elemCount; i++) {
                U64 a, b;
                U64 bitOff;
                if (i * elemBits < 64) {
                    bitOff = i * elemBits;
                    a = (dstLo >> bitOff) & mask;
                    b = (srcLo >> bitOff) & mask;
                } else {
                    bitOff = i * elemBits - 64;
                    a = (dstHi >> bitOff) & mask;
                    b = (srcHi >> bitOff) & mask;
                }
                U64 r = (a == b) ? mask : 0;
                if (i * elemBits < 64) outLo |= r << (i * elemBits);
                else                   outHi |= r << (i * elemBits - 64);
            }
            xmm[m.regField].lo = outLo;
            xmm[m.regField].hi = outHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PMOVMSKB r32, xmm — 66 0F D7 /r. Extract the high bit of each of
        // the 16 bytes into the low 16 bits of r32. Paired with PCMPEQB to
        // turn the 16-byte compare into a 16-bit "any equal?" mask.
        if (op2 == 0xD7 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 lo = xmm[m.rmIndex].lo;
            U64 hi = xmm[m.rmIndex].hi;
            U32 mask = 0;
            for (U32 i = 0; i < 8; i++) {
                if (lo & (1ULL << (i * 8 + 7))) mask |= (1u << i);
                if (hi & (1ULL << (i * 8 + 7))) mask |= (1u << (i + 8));
            }
            reg[m.regField].setU32(mask);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // POR / PAND / PANDN — 66 0F EB / DB / DF /r. Same shape as PXOR.
        if ((op2 == 0xEB || op2 == 0xDB || op2 == 0xDF) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            if (op2 == 0xEB) {
                xmm[m.regField].lo |= srcLo;
                xmm[m.regField].hi |= srcHi;
            } else if (op2 == 0xDB) {
                xmm[m.regField].lo &= srcLo;
                xmm[m.regField].hi &= srcHi;
            } else { // PANDN: dst = (~dst) & src
                xmm[m.regField].lo = (~xmm[m.regField].lo) & srcLo;
                xmm[m.regField].hi = (~xmm[m.regField].hi) & srcHi;
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PSHUFLW xmm, xmm/m128, imm8 — F2 0F 70 /r ib. Shuffles the four
        // low words (lo qword) per imm8; copies hi qword through unchanged.
        // PSHUFHW xmm, xmm/m128, imm8 — F3 0F 70 /r ib. Same but for hi.
        // Discovered in musl strchr/strlen path — the byte-search uses
        // PSHUFLW to broadcast a search byte across the low half of an XMM
        // register before a 16-byte PCMPEQB.
        if (op2 == 0x70 && (p.rep == 0xF2 || p.rep == 0xF3)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            if (p.rep == 0xF2) {
                // PSHUFLW: shuffle low 4 words from srcLo by imm; hi passes through.
                U16 w[4] = {
                    (U16)(srcLo & 0xFFFF),
                    (U16)((srcLo >> 16) & 0xFFFF),
                    (U16)((srcLo >> 32) & 0xFFFF),
                    (U16)((srcLo >> 48) & 0xFFFF)
                };
                U16 out[4];
                for (int i = 0; i < 4; i++) out[i] = w[(imm >> (i * 2)) & 0x3];
                xmm[m.regField].lo = (U64)out[0] | ((U64)out[1] << 16)
                                   | ((U64)out[2] << 32) | ((U64)out[3] << 48);
                xmm[m.regField].hi = srcHi;
            } else {
                // PSHUFHW: shuffle high 4 words from srcHi by imm; lo passes through.
                U16 w[4] = {
                    (U16)(srcHi & 0xFFFF),
                    (U16)((srcHi >> 16) & 0xFFFF),
                    (U16)((srcHi >> 32) & 0xFFFF),
                    (U16)((srcHi >> 48) & 0xFFFF)
                };
                U16 out[4];
                for (int i = 0; i < 4; i++) out[i] = w[(imm >> (i * 2)) & 0x3];
                xmm[m.regField].lo = srcLo;
                xmm[m.regField].hi = (U64)out[0] | ((U64)out[1] << 16)
                                   | ((U64)out[2] << 32) | ((U64)out[3] << 48);
            }
            U32 used = opOff + 2 + m.length + 1;
            rip += used;
            return used;
        }

        // PSHUFD xmm, xmm/m128, imm8 — 66 0F 70 /r ib.
        // imm8 picks four 2-bit indices selecting which source dword goes to
        // each destination dword position. Common forms: imm=0 (broadcast
        // low dword), imm=0xFF (broadcast high dword).
        if (op2 == 0x70 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            U32 dwords[4] = {
                (U32)srcLo, (U32)(srcLo >> 32),
                (U32)srcHi, (U32)(srcHi >> 32)
            };
            U32 out[4];
            for (int i = 0; i < 4; i++) out[i] = dwords[(imm >> (i * 2)) & 0x3];
            xmm[m.regField].lo = ((U64)out[1] << 32) | out[0];
            xmm[m.regField].hi = ((U64)out[3] << 32) | out[2];
            U32 used = opOff + 2 + m.length + 1;
            rip += used;
            return used;
        }
        // Per-lane shift with imm8 — 66 0F 71/72/73 /sub ib.
        //   71 → word lanes, 72 → dword lanes, 73 → qword lanes.
        //   sub == 6 → shift left logical  (PSLLW/PSLLD/PSLLQ)
        //   sub == 2 → shift right logical (PSRLW/PSRLD/PSRLQ)
        //   sub == 4 → shift right arith   (PSRAW/PSRAD; not defined for qwords)
        //   sub == 7 with 0x73 → PSLLDQ (byte-granular), handled below
        //   sub == 3 with 0x73 → PSRLDQ
        if ((op2 == 0x71 || op2 == 0x72 || (op2 == 0x73)) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            U8 sub = m.regField & 0x7;
            // Defer byte-granular forms (73 /7 and 73 /3) to the PSLLDQ block.
            if (op2 == 0x73 && (sub == 7 || sub == 3)) {
                // fall through to the existing PSLLDQ handler below
            } else if (sub == 6 || sub == 2 || sub == 4) {
                U8 imm = fetchByte(rip + opOff + 2 + m.length);
                U32 elemBits = (op2 == 0x71) ? 16 : (op2 == 0x72) ? 32 : 64;
                U32 elemCount = 128 / elemBits;
                U64 lo = xmm[m.rmIndex].lo;
                U64 hi = xmm[m.rmIndex].hi;
                U64 oLo = 0, oHi = 0;
                U64 mask = (elemBits == 64) ? ~0ULL : ((1ULL << elemBits) - 1);
                bool saturate = (imm >= elemBits);
                for (U32 i = 0; i < elemCount; i++) {
                    U32 bitPos = i * elemBits;
                    U64 v;
                    if (bitPos < 64) v = (lo >> bitPos) & mask;
                    else             v = (hi >> (bitPos - 64)) & mask;
                    U64 r;
                    if (saturate) {
                        if (sub == 4) {
                            U64 signMask = 1ULL << (elemBits - 1);
                            r = (v & signMask) ? mask : 0;
                        } else {
                            r = 0;
                        }
                    } else if (sub == 6) {
                        r = (v << imm) & mask;
                    } else if (sub == 2) {
                        r = v >> imm;
                    } else { // SRA
                        U64 signMask = 1ULL << (elemBits - 1);
                        if (v & signMask) {
                            U64 fillBits = (~0ULL) << (elemBits - imm);
                            r = ((v >> imm) | fillBits) & mask;
                        } else {
                            r = (v >> imm) & mask;
                        }
                    }
                    if (bitPos < 64) oLo |= r << bitPos;
                    else             oHi |= r << (bitPos - 64);
                }
                xmm[m.rmIndex].lo = oLo;
                xmm[m.rmIndex].hi = oHi;
                U32 used = opOff + 2 + m.length + 1;
                rip += used;
                return used;
            }
        }
        // PSLLDQ/PSRLDQ xmm, imm8 — 66 0F 73 /7 ib (left) or /3 ib (right).
        // Byte-granularity logical shift of the entire 16-byte register.
        // Used by glibc memcpy/memset to mask the tail when a copy isn't
        // 16-byte aligned.
        if (op2 == 0x73 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 1);
            U8 sub = m.regField & 0x7;
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            if (sub == 7 || sub == 3) {
                U64 lo = xmm[m.rmIndex].lo;
                U64 hi = xmm[m.rmIndex].hi;
                U32 shift = imm > 16 ? 16 : imm;
                if (shift == 0) {
                    // no-op
                } else if (sub == 7) {
                    // shift left by `shift` bytes
                    if (shift >= 8) {
                        hi = lo << ((shift - 8) * 8);
                        lo = 0;
                    } else {
                        U32 bits = shift * 8;
                        hi = (hi << bits) | (lo >> (64 - bits));
                        lo = lo << bits;
                    }
                } else { // PSRLDQ
                    if (shift >= 8) {
                        lo = hi >> ((shift - 8) * 8);
                        hi = 0;
                    } else {
                        U32 bits = shift * 8;
                        lo = (lo >> bits) | (hi << (64 - bits));
                        hi = hi >> bits;
                    }
                }
                xmm[m.rmIndex].lo = lo;
                xmm[m.rmIndex].hi = hi;
                U32 used = opOff + 2 + m.length + 1;
                rip += used;
                return used;
            }
        }
        // PSUBB/PSUBW/PSUBD/PSUBQ — 66 0F F8/F9/FA/FB /r. Same shape as PADD.
        if ((op2 == 0xF8 || op2 == 0xF9 || op2 == 0xFA || op2 == 0xFB) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            if (op2 == 0xF8) { // PSUBB
                for (int i = 0; i < 8; i++) {
                    U8 a = (dLo >> (i*8)) & 0xFF; U8 b = (srcLo >> (i*8)) & 0xFF;
                    oLo |= (U64)((U8)(a - b)) << (i*8);
                    U8 c = (dHi >> (i*8)) & 0xFF; U8 d = (srcHi >> (i*8)) & 0xFF;
                    oHi |= (U64)((U8)(c - d)) << (i*8);
                }
            } else if (op2 == 0xF9) { // PSUBW
                for (int i = 0; i < 4; i++) {
                    U16 a = (dLo >> (i*16)) & 0xFFFF; U16 b = (srcLo >> (i*16)) & 0xFFFF;
                    oLo |= (U64)((U16)(a - b)) << (i*16);
                    U16 c = (dHi >> (i*16)) & 0xFFFF; U16 d = (srcHi >> (i*16)) & 0xFFFF;
                    oHi |= (U64)((U16)(c - d)) << (i*16);
                }
            } else if (op2 == 0xFA) { // PSUBD
                for (int i = 0; i < 2; i++) {
                    U32 a = (dLo >> (i*32)) & 0xFFFFFFFFu; U32 b = (srcLo >> (i*32)) & 0xFFFFFFFFu;
                    oLo |= (U64)((U32)(a - b)) << (i*32);
                    U32 c = (dHi >> (i*32)) & 0xFFFFFFFFu; U32 d = (srcHi >> (i*32)) & 0xFFFFFFFFu;
                    oHi |= (U64)((U32)(c - d)) << (i*32);
                }
            } else { // PSUBQ
                oLo = dLo - srcLo;
                oHi = dHi - srcHi;
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PUNPCKLBW/W/D/Q xmm, xmm/m128 — 66 0F 60/61/62/6C /r.
        // Interleave low-half lanes of dst and src.
        //   60 = bytes:  out[0]=d[0] out[1]=s[0] out[2]=d[1] out[3]=s[1] ...
        //   61 = words, 62 = dwords, 6C = qwords.
        // PUNPCKH (high half) at 0F 68/69/6A/6D, same interleave on the
        // upper 8 bytes of each input.
        if ((op2 == 0x60 || op2 == 0x61 || op2 == 0x62 || op2 == 0x6C ||
             op2 == 0x68 || op2 == 0x69 || op2 == 0x6A || op2 == 0x6D) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            // Opcode map (PUNPCK): LOW forms take the low halves of each
            // operand, HIGH forms the high halves.
            //   LOW : 60=LBW 61=LWD 62=LDQ 6C=LQDQ
            //   HIGH: 68=HBW 69=HWD 6A=HDQ 6D=HQDQ
            // NOTE 0x6C (PUNPCKLQDQ) is numerically >= 0x68 but is a LOW
            // form — classifying by ">= 0x68" wrongly buckets it as HIGH and
            // reads the (often-zero) high halves, which silently corrupted
            // glibc's malloc bin self-pointers. Classify explicitly.
            bool isHigh = (op2 == 0x68 || op2 == 0x69 || op2 == 0x6A || op2 == 0x6D);
            U64 dSrc = isHigh ? dHi : dLo;
            U64 sSrc = isHigh ? srcHi : srcLo;
            U64 oLo = 0, oHi = 0;
            // Map opcode -> element-width index: 0=byte 1=word 2=dword 4=qword.
            U8 sub;
            switch (op2) {
                case 0x60: case 0x68: sub = 0; break; // byte
                case 0x61: case 0x69: sub = 1; break; // word
                case 0x62: case 0x6A: sub = 2; break; // dword
                default:              sub = 4; break; // 6C/6D qword
            }
            if (sub == 0) { // byte interleave: 16 bytes out, 8 from each input
                for (int i = 0; i < 8; i++) {
                    U64 db = (dSrc >> (i*8)) & 0xFF;
                    U64 sb = (sSrc >> (i*8)) & 0xFF;
                    U32 pos = i * 16;
                    if (pos < 64) oLo |= db << pos;
                    else          oHi |= db << (pos - 64);
                    pos += 8;
                    if (pos < 64) oLo |= sb << pos;
                    else          oHi |= sb << (pos - 64);
                }
            } else if (sub == 1) { // word interleave
                for (int i = 0; i < 4; i++) {
                    U64 dw = (dSrc >> (i*16)) & 0xFFFF;
                    U64 sw = (sSrc >> (i*16)) & 0xFFFF;
                    U32 pos = i * 32;
                    if (pos < 64) oLo |= dw << pos;
                    else          oHi |= dw << (pos - 64);
                    pos += 16;
                    if (pos < 64) oLo |= sw << pos;
                    else          oHi |= sw << (pos - 64);
                }
            } else if (sub == 2) { // dword interleave
                oLo = (U32)dSrc | ((U64)(U32)sSrc << 32);
                oHi = (U32)(dSrc >> 32) | ((U64)(U32)(sSrc >> 32) << 32);
            } else { // qword interleave (sub==4)
                oLo = dSrc;
                oHi = sSrc;
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PCMPGTB/W/D xmm, xmm/m128 — 66 0F 64/65/66 /r. Signed greater-than:
        // each lane of dst becomes all-1s if dst > src, else all-0s. Paired
        // with PCMPEQB in glibc's classified-character scans.
        if ((op2 == 0x64 || op2 == 0x65 || op2 == 0x66) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            if (op2 == 0x64) { // PCMPGTB (signed bytes)
                for (int i = 0; i < 8; i++) {
                    S8 a = (S8)((dLo >> (i*8)) & 0xFF);
                    S8 b = (S8)((srcLo >> (i*8)) & 0xFF);
                    if (a > b) oLo |= 0xFFULL << (i*8);
                    S8 c = (S8)((dHi >> (i*8)) & 0xFF);
                    S8 d = (S8)((srcHi >> (i*8)) & 0xFF);
                    if (c > d) oHi |= 0xFFULL << (i*8);
                }
            } else if (op2 == 0x65) { // PCMPGTW
                for (int i = 0; i < 4; i++) {
                    S16 a = (S16)((dLo >> (i*16)) & 0xFFFF);
                    S16 b = (S16)((srcLo >> (i*16)) & 0xFFFF);
                    if (a > b) oLo |= 0xFFFFULL << (i*16);
                    S16 c = (S16)((dHi >> (i*16)) & 0xFFFF);
                    S16 d = (S16)((srcHi >> (i*16)) & 0xFFFF);
                    if (c > d) oHi |= 0xFFFFULL << (i*16);
                }
            } else { // PCMPGTD
                for (int i = 0; i < 2; i++) {
                    S32 a = (S32)((dLo >> (i*32)) & 0xFFFFFFFFu);
                    S32 b = (S32)((srcLo >> (i*32)) & 0xFFFFFFFFu);
                    if (a > b) oLo |= 0xFFFFFFFFULL << (i*32);
                    S32 c = (S32)((dHi >> (i*32)) & 0xFFFFFFFFu);
                    S32 d = (S32)((srcHi >> (i*32)) & 0xFFFFFFFFu);
                    if (c > d) oHi |= 0xFFFFFFFFULL << (i*32);
                }
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PADDB/PADDW/PADDD/PADDQ — 66 0F FC/FD/FE/D4 /r. Used by SSE
        // crypto-style mixing in glibc's hash routines.
        if ((op2 == 0xFC || op2 == 0xFD || op2 == 0xFE || op2 == 0xD4) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            if (op2 == 0xFC) { // PADDB
                for (int i = 0; i < 8; i++) {
                    U8 a = (dLo >> (i*8)) & 0xFF; U8 b = (srcLo >> (i*8)) & 0xFF;
                    oLo |= (U64)((U8)(a + b)) << (i*8);
                    U8 c = (dHi >> (i*8)) & 0xFF; U8 d = (srcHi >> (i*8)) & 0xFF;
                    oHi |= (U64)((U8)(c + d)) << (i*8);
                }
            } else if (op2 == 0xFD) { // PADDW
                for (int i = 0; i < 4; i++) {
                    U16 a = (dLo >> (i*16)) & 0xFFFF; U16 b = (srcLo >> (i*16)) & 0xFFFF;
                    oLo |= (U64)((U16)(a + b)) << (i*16);
                    U16 c = (dHi >> (i*16)) & 0xFFFF; U16 d = (srcHi >> (i*16)) & 0xFFFF;
                    oHi |= (U64)((U16)(c + d)) << (i*16);
                }
            } else if (op2 == 0xFE) { // PADDD
                for (int i = 0; i < 2; i++) {
                    U32 a = (dLo >> (i*32)) & 0xFFFFFFFFu; U32 b = (srcLo >> (i*32)) & 0xFFFFFFFFu;
                    oLo |= (U64)((U32)(a + b)) << (i*32);
                    U32 c = (dHi >> (i*32)) & 0xFFFFFFFFu; U32 d = (srcHi >> (i*32)) & 0xFFFFFFFFu;
                    oHi |= (U64)((U32)(c + d)) << (i*32);
                }
            } else { // PADDQ
                oLo = dLo + srcLo;
                oHi = dHi + srcHi;
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // MOVQ xmm/m64, xmm — 66 0F D6 /r. Store low qword.
        if (op2 == 0xD6 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (m.isReg) {
                xmm[m.rmIndex].lo = xmm[m.regField].lo;
                xmm[m.rmIndex].hi = 0;
            } else {
                memory->writeq(m.effAddr, xmm[m.regField].lo);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PMINUB/PMAXUB — 66 0F DA/DE /r. Unsigned byte min/max across 16
        // lanes. Used by glibc's strlen/memchr fast paths.
        if ((op2 == 0xDA || op2 == 0xDE) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            for (int i = 0; i < 8; i++) {
                U8 a = (dLo  >> (i*8)) & 0xFF; U8 b = (srcLo >> (i*8)) & 0xFF;
                U8 c = (dHi  >> (i*8)) & 0xFF; U8 d = (srcHi >> (i*8)) & 0xFF;
                U8 lo8 = (op2 == 0xDA) ? (a < b ? a : b) : (a > b ? a : b);
                U8 hi8 = (op2 == 0xDA) ? (c < d ? c : d) : (c > d ? c : d);
                oLo |= (U64)lo8 << (i*8);
                oHi |= (U64)hi8 << (i*8);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PMINSW/PMAXSW — 66 0F EA/EE /r. Signed word min/max across 8 lanes.
        if ((op2 == 0xEA || op2 == 0xEE) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            for (int i = 0; i < 4; i++) {
                S16 a = (S16)((dLo  >> (i*16)) & 0xFFFF);
                S16 b = (S16)((srcLo >> (i*16)) & 0xFFFF);
                S16 c = (S16)((dHi  >> (i*16)) & 0xFFFF);
                S16 d = (S16)((srcHi >> (i*16)) & 0xFFFF);
                S16 lo16 = (op2 == 0xEA) ? (a < b ? a : b) : (a > b ? a : b);
                S16 hi16 = (op2 == 0xEA) ? (c < d ? c : d) : (c > d ? c : d);
                oLo |= ((U64)(U16)lo16) << (i*16);
                oHi |= ((U64)(U16)hi16) << (i*16);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PAVGB — 66 0F E0 /r. Unsigned byte average with rounding:
        // dst[i] = (dst[i] + src[i] + 1) >> 1.
        if (op2 == 0xE0 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            for (int i = 0; i < 8; i++) {
                U16 a = (dLo  >> (i*8)) & 0xFF; U16 b = (srcLo >> (i*8)) & 0xFF;
                U16 c = (dHi  >> (i*8)) & 0xFF; U16 d = (srcHi >> (i*8)) & 0xFF;
                oLo |= (U64)((U8)((a + b + 1) >> 1)) << (i*8);
                oHi |= (U64)((U8)((c + d + 1) >> 1)) << (i*8);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PSADBW — 66 0F F6 /r. Sum-of-absolute-differences over each 8-byte
        // half. Used by glibc memcmp/strncmp. Output: low qword = sum for low
        // half, high qword = sum for high half.
        if (op2 == 0xF6 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 sumLo = 0, sumHi = 0;
            for (int i = 0; i < 8; i++) {
                int a = (dLo  >> (i*8)) & 0xFF;
                int b = (srcLo >> (i*8)) & 0xFF;
                int c = (dHi  >> (i*8)) & 0xFF;
                int d = (srcHi >> (i*8)) & 0xFF;
                sumLo += (U64)(a > b ? a - b : b - a);
                sumHi += (U64)(c > d ? c - d : d - c);
            }
            xmm[m.regField].lo = sumLo;
            xmm[m.regField].hi = sumHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PMULLW — 66 0F D5 /r. Per-word signed multiply, store low 16 of
        // each 32-bit result. PMULHW — 66 0F E5 /r. Same but store high 16.
        if ((op2 == 0xD5 || op2 == 0xE5) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            bool high = (op2 == 0xE5);
            for (int i = 0; i < 4; i++) {
                S16 a = (S16)((dLo  >> (i*16)) & 0xFFFF);
                S16 b = (S16)((srcLo >> (i*16)) & 0xFFFF);
                S16 c = (S16)((dHi  >> (i*16)) & 0xFFFF);
                S16 d = (S16)((srcHi >> (i*16)) & 0xFFFF);
                S32 p1 = (S32)a * (S32)b;
                S32 p2 = (S32)c * (S32)d;
                U16 outLo = high ? (U16)((p1 >> 16) & 0xFFFF) : (U16)(p1 & 0xFFFF);
                U16 outHi = high ? (U16)((p2 >> 16) & 0xFFFF) : (U16)(p2 & 0xFFFF);
                oLo |= ((U64)outLo) << (i*16);
                oHi |= ((U64)outHi) << (i*16);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PMULUDQ — 66 0F F4 /r. Multiply unsigned dwords in lanes 0 and 2,
        // producing two 64-bit results.
        if (op2 == 0xF4 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 a = dLo & 0xFFFFFFFFULL;
            U64 b = srcLo & 0xFFFFFFFFULL;
            U64 c = dHi & 0xFFFFFFFFULL;
            U64 d = srcHi & 0xFFFFFFFFULL;
            xmm[m.regField].lo = a * b;
            xmm[m.regField].hi = c * d;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // PACKSSWB / PACKUSWB / PACKSSDW — 66 0F 63 / 67 / 6B /r.
        // Saturate two source halves down to a smaller element size and
        // interleave dst-first, src-second.
        if ((op2 == 0x63 || op2 == 0x67 || op2 == 0x6B) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            if (op2 == 0x6B) {
                // PACKSSDW: 4 signed dwords → 4 signed words per half.
                // dst halves produce 4 words → 8 words total. Lanes 0..3 from
                // dst (lo+hi dwords of dLo/dHi), lanes 4..7 from src.
                auto satS16 = [](S32 v) -> U16 {
                    if (v > 32767) v = 32767;
                    if (v < -32768) v = -32768;
                    return (U16)(v & 0xFFFF);
                };
                U16 w[8];
                w[0] = satS16((S32)(dLo & 0xFFFFFFFFu));
                w[1] = satS16((S32)((dLo >> 32) & 0xFFFFFFFFu));
                w[2] = satS16((S32)(dHi & 0xFFFFFFFFu));
                w[3] = satS16((S32)((dHi >> 32) & 0xFFFFFFFFu));
                w[4] = satS16((S32)(srcLo & 0xFFFFFFFFu));
                w[5] = satS16((S32)((srcLo >> 32) & 0xFFFFFFFFu));
                w[6] = satS16((S32)(srcHi & 0xFFFFFFFFu));
                w[7] = satS16((S32)((srcHi >> 32) & 0xFFFFFFFFu));
                for (int i = 0; i < 4; i++) oLo |= ((U64)w[i]) << (i*16);
                for (int i = 0; i < 4; i++) oHi |= ((U64)w[i+4]) << (i*16);
            } else {
                // PACKSSWB / PACKUSWB: 8 signed words from each input →
                // 8 bytes per side. Total 16 bytes interleaved dst-then-src.
                bool sign = (op2 == 0x63);
                auto satS8 = [](S16 v) -> U8 {
                    if (v > 127) v = 127;
                    if (v < -128) v = -128;
                    return (U8)(v & 0xFF);
                };
                auto satU8 = [](S16 v) -> U8 {
                    if (v > 255) v = 255;
                    if (v < 0) v = 0;
                    return (U8)(v & 0xFF);
                };
                U8 b[16];
                for (int i = 0; i < 4; i++) {
                    S16 a = (S16)((dLo  >> (i*16)) & 0xFFFF);
                    S16 c = (S16)((dHi  >> (i*16)) & 0xFFFF);
                    b[i]   = sign ? satS8(a) : satU8(a);
                    b[i+4] = sign ? satS8(c) : satU8(c);
                }
                for (int i = 0; i < 4; i++) {
                    S16 a = (S16)((srcLo >> (i*16)) & 0xFFFF);
                    S16 c = (S16)((srcHi >> (i*16)) & 0xFFFF);
                    b[i+8]  = sign ? satS8(a) : satU8(a);
                    b[i+12] = sign ? satS8(c) : satU8(c);
                }
                for (int i = 0; i < 8; i++) oLo |= ((U64)b[i]) << (i*8);
                for (int i = 0; i < 8; i++) oHi |= ((U64)b[i+8]) << (i*8);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // MOVMSKPS — 0F 50 /r. Extract sign bits of the 4 single-precision
        // floats in xmm[rmIndex] into bits 0..3 of the general-purpose
        // r32/r64 destination, zero the rest. The reg-only form is the
        // only legal encoding. We don't model FP types — just take bit 31
        // of each 32-bit lane. MOVMSKPD — 66 0F 50 /r. Same idea but 2
        // double-precision lanes (bits 63 of each 64-bit half).
        // SHUFPS — 0F C6 /r ib (no 66). Per-imm8 4-dword shuffle: bits
        // [1:0]/[3:2] pick lanes from dst, [5:4]/[7:6] pick from src.
        // Output lanes 0,1 ← dst, 2,3 ← src.
        // SHUFPD — 66 0F C6 /r ib. 2-qword shuffle: bit 0 picks dst lane,
        // bit 1 picks src lane.
        if (op2 == 0xC6) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            if (osize66) {
                // SHUFPD
                U64 dHalves[2] = { dLo, dHi };
                U64 sHalves[2] = { srcLo, srcHi };
                oLo = dHalves[(imm >> 0) & 1];
                oHi = sHalves[(imm >> 1) & 1];
            } else {
                // SHUFPS — break into 4 dwords.
                U32 d[4] = { (U32)dLo, (U32)(dLo >> 32), (U32)dHi, (U32)(dHi >> 32) };
                U32 s[4] = { (U32)srcLo, (U32)(srcLo >> 32), (U32)srcHi, (U32)(srcHi >> 32) };
                U32 o[4];
                o[0] = d[(imm >> 0) & 3];
                o[1] = d[(imm >> 2) & 3];
                o[2] = s[(imm >> 4) & 3];
                o[3] = s[(imm >> 6) & 3];
                oLo = (U64)o[0] | ((U64)o[1] << 32);
                oHi = (U64)o[2] | ((U64)o[3] << 32);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length + 1;
            rip += used;
            return used;
        }
        // PEXTRW — 66 0F C5 /r ib. Extract one word from xmm[rmIndex] (imm8
        // & 7 selects the lane) into r32/r64.
        if (op2 == 0xC5 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            if (m.isReg) {
                int lane = imm & 7;
                U64 src = (lane < 4) ? xmm[m.rmIndex].lo : xmm[m.rmIndex].hi;
                U64 w = (src >> ((lane & 3) * 16)) & 0xFFFF;
                reg[m.regField].setU64(w);
            }
            U32 used = opOff + 2 + m.length + 1;
            rip += used;
            return used;
        }
        // PINSRW — 66 0F C4 /r ib. Insert low 16 of r32 (or m16) into xmm[reg]
        // at lane (imm8 & 7).
        if (op2 == 0xC4 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            U16 v;
            if (m.isReg) v = (U16)reg[m.rmIndex].u32;
            else         v = memory->readw(m.effAddr);
            int lane = imm & 7;
            U64* dst = (lane < 4) ? &xmm[m.regField].lo : &xmm[m.regField].hi;
            int sub = lane & 3;
            U64 mask = ~((U64)0xFFFF << (sub * 16));
            *dst = (*dst & mask) | ((U64)v << (sub * 16));
            U32 used = opOff + 2 + m.length + 1;
            rip += used;
            return used;
        }
        // MOVNTDQ — 66 0F E7 /r. Non-temporal store of xmm to m128. We
        // ignore the cache hint and treat as a plain 16-byte store.
        if (op2 == 0xE7 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (!m.isReg) {
                memory->writeq(m.effAddr,     xmm[m.regField].lo);
                memory->writeq(m.effAddr + 8, xmm[m.regField].hi);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // MOVLHPS — 0F 16 /r (reg form). xmm[reg].hi := xmm[rm].lo.
        // MOVHLPS — 0F 12 /r (reg form). xmm[reg].lo := xmm[rm].hi.
        // Memory forms (MOVLPS/MOVHPS) are also encoded with 0F 12/16 but
        // with a non-reg ModR/M — keep those simple too.
        if ((op2 == 0x12 || op2 == 0x16) && !osize66 && p.rep == 0) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (m.isReg) {
                if (op2 == 0x16) {
                    xmm[m.regField].hi = xmm[m.rmIndex].lo; // MOVLHPS
                } else {
                    xmm[m.regField].lo = xmm[m.rmIndex].hi; // MOVHLPS
                }
            } else {
                // Memory form: MOVLPS loads/stores low qword, MOVHPS high.
                if (op2 == 0x12) {
                    xmm[m.regField].lo = memory->readq(m.effAddr);
                } else {
                    xmm[m.regField].hi = memory->readq(m.effAddr);
                }
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // 0F 13 /r — MOVLPS m64, xmm. 0F 17 /r — MOVHPS m64, xmm.
        if ((op2 == 0x13 || op2 == 0x17) && !osize66 && p.rep == 0) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (!m.isReg) {
                U64 v = (op2 == 0x13) ? xmm[m.regField].lo : xmm[m.regField].hi;
                memory->writeq(m.effAddr, v);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        if (op2 == 0x50) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (m.isReg) {
                U64 lo = xmm[m.rmIndex].lo;
                U64 hi = xmm[m.rmIndex].hi;
                U32 out = 0;
                if (osize66) {
                    // MOVMSKPD — 2 doubles, take bit 63 of each.
                    if (lo & (1ULL << 63)) out |= 1;
                    if (hi & (1ULL << 63)) out |= 2;
                } else {
                    // MOVMSKPS — 4 floats, take bit 31 of each 32-bit lane.
                    if ((lo >> 31) & 1) out |= 1;
                    if ((lo >> 63) & 1) out |= 2;
                    if ((hi >> 31) & 1) out |= 4;
                    if ((hi >> 63) & 1) out |= 8;
                }
                reg[m.regField].setU64((U64)out);
                U32 used = opOff + 2 + m.length;
                rip += used;
                return used;
            }
        }
        // PMULHUW — 66 0F E4 /r. Unsigned word multiply, store high 16 of
        // each product. Glibc's hash mixing uses this.
        if (op2 == 0xE4 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            for (int i = 0; i < 4; i++) {
                U16 a = (dLo  >> (i*16)) & 0xFFFF;
                U16 b = (srcLo >> (i*16)) & 0xFFFF;
                U16 c = (dHi  >> (i*16)) & 0xFFFF;
                U16 d = (srcHi >> (i*16)) & 0xFFFF;
                U32 p1 = (U32)a * (U32)b;
                U32 p2 = (U32)c * (U32)d;
                oLo |= ((U64)((p1 >> 16) & 0xFFFF)) << (i*16);
                oHi |= ((U64)((p2 >> 16) & 0xFFFF)) << (i*16);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // Variable per-lane shifts — 66 0F D1/D2/D3 (right logical W/D/Q),
        // E1/E2 (right arithmetic W/D), F1/F2/F3 (left W/D/Q). Shift count
        // comes from the LOW QWORD of the source xmm; if > element width,
        // the result is zero (logical) or all-sign (arithmetic).
        bool isVarShift = osize66 &&
            (op2 == 0xD1 || op2 == 0xD2 || op2 == 0xD3 ||
             op2 == 0xE1 || op2 == 0xE2 ||
             op2 == 0xF1 || op2 == 0xF2 || op2 == 0xF3);
        if (isVarShift) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo;
            if (m.isReg) srcLo = xmm[m.rmIndex].lo;
            else         srcLo = memory->readq(m.effAddr);
            U64 dLo = xmm[m.regField].lo;
            U64 dHi = xmm[m.regField].hi;
            U64 oLo = 0, oHi = 0;
            // Element size and direction by opcode.
            int  esize  = (op2 == 0xD1 || op2 == 0xE1 || op2 == 0xF1) ? 16 :
                          (op2 == 0xD2 || op2 == 0xE2 || op2 == 0xF2) ? 32 : 64;
            bool isLeft = (op2 >= 0xF1);
            bool isArith = (op2 == 0xE1 || op2 == 0xE2);
            U64 cnt = srcLo;
            // Saturate count: too-large shifts zero everything for logical,
            // or replicate sign for arithmetic.
            if (cnt >= (U64)esize) {
                if (!isArith) {
                    xmm[m.regField].lo = 0;
                    xmm[m.regField].hi = 0;
                } else {
                    // Per lane, fill with the sign bit replicated.
                    auto fillSign = [&](U64 v) -> U64 {
                        U64 out = 0;
                        for (int i = 0; i < 128 / esize / 2; i++) {
                            U64 lane = (v >> (i*esize)) & ((esize == 64) ? ~0ULL : ((1ULL << esize) - 1));
                            U64 signMask = (lane >> (esize - 1)) & 1 ? ((esize == 64) ? ~0ULL : ((1ULL << esize) - 1)) : 0;
                            out |= signMask << (i*esize);
                        }
                        return out;
                    };
                    xmm[m.regField].lo = fillSign(dLo);
                    xmm[m.regField].hi = fillSign(dHi);
                }
                U32 used = opOff + 2 + m.length;
                rip += used;
                return used;
            }
            int n = 64 / esize;
            U64 mask = (esize == 64) ? ~0ULL : ((1ULL << esize) - 1);
            for (int i = 0; i < n; i++) {
                U64 a = (dLo >> (i*esize)) & mask;
                U64 b = (dHi >> (i*esize)) & mask;
                U64 ra, rb;
                if (isLeft) {
                    ra = (a << cnt) & mask;
                    rb = (b << cnt) & mask;
                } else if (isArith) {
                    S64 sa = (esize == 16) ? (S64)(S16)a :
                             (esize == 32) ? (S64)(S32)a : (S64)a;
                    S64 sb = (esize == 16) ? (S64)(S16)b :
                             (esize == 32) ? (S64)(S32)b : (S64)b;
                    ra = (U64)(sa >> cnt) & mask;
                    rb = (U64)(sb >> cnt) & mask;
                } else {
                    ra = (a >> cnt) & mask;
                    rb = (b >> cnt) & mask;
                }
                oLo |= ra << (i*esize);
                oHi |= rb << (i*esize);
            }
            xmm[m.regField].lo = oLo;
            xmm[m.regField].hi = oHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // 0F 01 — group containing XGETBV / RDTSCP / SWAPGS / etc.
        // The third byte distinguishes the form. We implement the two
        // that user-space glibc actually issues:
        //   0F 01 D0   XGETBV   — read XSAVE feature mask into EDX:EAX
        //   0F 01 F9   RDTSCP   — read TSC and processor ID
        if (op2 == 0x01) {
            U8 op3 = fetchByte(rip + opOff + 2);
            if (op3 == 0xD0) {
                // XGETBV: glibc calls it via __get_cpu_features to decide
                // which XMM state is enabled. We advertise legacy XSAVE
                // state only: bit0=x87, bit1=SSE → ECX selector 0 returns
                // EDX:EAX = 0:0x3.
                if (reg[X64_RCX].u32 == 0) {
                    reg[X64_RAX].setU64(0x3);
                    reg[X64_RDX].setU64(0);
                } else {
                    // Unknown XCR — return zero (legacy behaviour).
                    reg[X64_RAX].setU64(0);
                    reg[X64_RDX].setU64(0);
                }
                U32 used = opOff + 3;
                rip += used;
                return used;
            }
            if (op3 == 0xF9) {
                // RDTSCP: same as RDTSC plus aux ID in ECX. Reuse the
                // TSC counter our RDTSC path uses (instructionCount-based
                // monotonic synthetic clock); ECX = 0 (CPU 0).
                U64 tsc = (U64)instructionCount;
                reg[X64_RAX].setU64(tsc & 0xFFFFFFFFULL);
                reg[X64_RDX].setU64((tsc >> 32) & 0xFFFFFFFFULL);
                reg[X64_RCX].setU64(0);
                U32 used = opOff + 3;
                rip += used;
                return used;
            }
            // Fall through to default panic for unimplemented 0F 01 .. forms.
        }

        // PREFETCH* — 0F 18 /reg. Treated as a no-op (hint only).
        if (op2 == 0x18) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        // NOP (multi-byte) — 0F 1F /reg with optional operand. Used by glibc
        // for code alignment; ModR/M absorbs disp/SIB bytes.
        if (op2 == 0x1F) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // ---- SSSE3 three-byte opcodes ----
        //
        // 66 0F 38 00 /r  — PSHUFB  xmm1, xmm2/m128
        //   For each byte i of the destination: if shuffle[i] high bit set,
        //   write 0; else write src[shuffle[i] & 0x0F]. Used by glibc's
        //   SSSE3-optimised strlen/strcmp/memcpy.
        //
        // 66 0F 3A 0F /r ib — PALIGNR xmm1, xmm2/m128, imm8
        //   Concatenate dest:src into a 32-byte temp, then right-shift it
        //   by imm8 bytes, take the low 16 bytes. Used by some glibc
        //   memcpy variants and Wine's CRT.
        if ((op2 == 0x38 || op2 == 0x3A) && osize66) {
            U8 op3 = fetchByte(rip + opOff + 2);
            if (op2 == 0x38 && op3 == 0x00) {
                // PSHUFB
                ModRM m = decodeModRM(rip + opOff + 3, p, 0);
                U64 ctrlLo, ctrlHi;
                if (m.isReg) {
                    ctrlLo = xmm[m.rmIndex].lo;
                    ctrlHi = xmm[m.rmIndex].hi;
                } else {
                    ctrlLo = memory->readq(m.effAddr);
                    ctrlHi = memory->readq(m.effAddr + 8);
                }
                U8 src[16];
                for (int i = 0; i < 8; i++) src[i]     = (U8)(xmm[m.regField].lo >> (i*8));
                for (int i = 0; i < 8; i++) src[i+8]   = (U8)(xmm[m.regField].hi >> (i*8));
                U8 ctrl[16];
                for (int i = 0; i < 8; i++) ctrl[i]    = (U8)(ctrlLo >> (i*8));
                for (int i = 0; i < 8; i++) ctrl[i+8]  = (U8)(ctrlHi >> (i*8));
                U8 dst[16];
                for (int i = 0; i < 16; i++) {
                    dst[i] = (ctrl[i] & 0x80) ? 0 : src[ctrl[i] & 0x0F];
                }
                U64 nLo = 0, nHi = 0;
                for (int i = 0; i < 8; i++) nLo |= ((U64)dst[i])   << (i*8);
                for (int i = 0; i < 8; i++) nHi |= ((U64)dst[i+8]) << (i*8);
                xmm[m.regField].lo = nLo;
                xmm[m.regField].hi = nHi;
                U32 used = opOff + 3 + m.length;
                rip += used;
                return used;
            }
            if (op2 == 0x38 && op3 == 0x29) {
                // PCMPEQQ xmm1, xmm2/m128 — SSE4.1
                // Per qword: if equal, all-ones (0xFFFFFFFFFFFFFFFF); else zero.
                // Used by glibc's SSE4.1 strstr / __strcasestr_sse42.
                ModRM m = decodeModRM(rip + opOff + 3, p, 0);
                U64 sLo, sHi;
                if (m.isReg) {
                    sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi;
                } else {
                    sLo = memory->readq(m.effAddr);
                    sHi = memory->readq(m.effAddr + 8);
                }
                xmm[m.regField].lo = (xmm[m.regField].lo == sLo) ? 0xFFFFFFFFFFFFFFFFULL : 0;
                xmm[m.regField].hi = (xmm[m.regField].hi == sHi) ? 0xFFFFFFFFFFFFFFFFULL : 0;
                U32 used = opOff + 3 + m.length;
                rip += used;
                return used;
            }
            if (op2 == 0x38 && op3 == 0x17) {
                // PTEST xmm1, xmm2/m128 — SSE4.1
                // ZF = ((dst & src) == 0) ? 1 : 0
                // CF = ((~dst & src) == 0) ? 1 : 0    (AND NOT test)
                // OF/SF/AF/PF cleared. Used everywhere by glibc's SSE4.2
                // string ops for fast "all zero?" branches.
                ModRM m = decodeModRM(rip + opOff + 3, p, 0);
                U64 sLo, sHi;
                if (m.isReg) {
                    sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi;
                } else {
                    sLo = memory->readq(m.effAddr);
                    sHi = memory->readq(m.effAddr + 8);
                }
                U64 dLo = xmm[m.regField].lo;
                U64 dHi = xmm[m.regField].hi;
                bool zf = ((dLo & sLo) == 0) && ((dHi & sHi) == 0);
                bool cf = ((~dLo & sLo) == 0) && ((~dHi & sHi) == 0);
                rflags &= ~(X64_ZF | X64_CF | X64_OF | X64_SF | X64_AF | X64_PF);
                if (zf) rflags |= X64_ZF;
                if (cf) rflags |= X64_CF;
                U32 used = opOff + 3 + m.length;
                rip += used;
                return used;
            }
            if (op2 == 0x3A && op3 == 0x0F) {
                // PALIGNR
                ModRM m = decodeModRM(rip + opOff + 3, p, 1);
                U64 srcLo, srcHi;
                if (m.isReg) {
                    srcLo = xmm[m.rmIndex].lo;
                    srcHi = xmm[m.rmIndex].hi;
                } else {
                    srcLo = memory->readq(m.effAddr);
                    srcHi = memory->readq(m.effAddr + 8);
                }
                U8 imm = fetchByte(rip + opOff + 3 + m.length);
                // Concatenate dest:src — dest is high 16, src is low 16.
                U8 cat[32];
                for (int i = 0; i < 8; i++) cat[i]     = (U8)(srcLo >> (i*8));
                for (int i = 0; i < 8; i++) cat[i+8]   = (U8)(srcHi >> (i*8));
                for (int i = 0; i < 8; i++) cat[i+16]  = (U8)(xmm[m.regField].lo >> (i*8));
                for (int i = 0; i < 8; i++) cat[i+24]  = (U8)(xmm[m.regField].hi >> (i*8));
                U8 dst[16] = {0};
                if (imm < 32) {
                    for (int i = 0; i < 16; i++) {
                        int idx = i + imm;
                        dst[i] = (idx < 32) ? cat[idx] : 0;
                    }
                }
                U64 nLo = 0, nHi = 0;
                for (int i = 0; i < 8; i++) nLo |= ((U64)dst[i])   << (i*8);
                for (int i = 0; i < 8; i++) nHi |= ((U64)dst[i+8]) << (i*8);
                xmm[m.regField].lo = nLo;
                xmm[m.regField].hi = nHi;
                U32 used = opOff + 3 + m.length + 1;
                rip += used;
                return used;
            }
            // ---- SSE4.2 packed string compares: 66 0F 3A 60..63 /r ib ----
            //
            // PCMPESTRM (60), PCMPESTRI (61), PCMPISTRM (62), PCMPISTRI (63).
            // glibc's SSE4.2 strcmp/strcspn/strspn/strstr are selected by
            // IFUNC because the translated guest reports SSE4.2, and a forked
            // child runs here until it execs. With LD_LIBRARY_PATH in the
            // environment wineboot's child compared a string before its exec
            // and stopped at "unimpl opcode ... 66 0f 3a 63 04 16 1a", so
            // services and explorer were never started. The semantics live in
            // include/sse42_string_compare.h, where they are unit tested.
            if (op2 == 0x3A && (op3 >= 0x60 && op3 <= 0x63)) {
                ModRM m = decodeModRM(rip + opOff + 3, p, 1);
                U8 first[16];
                U8 second[16];
                for (int i = 0; i < 8; i++) first[i]    = (U8)(xmm[m.regField].lo >> (i*8));
                for (int i = 0; i < 8; i++) first[i+8]  = (U8)(xmm[m.regField].hi >> (i*8));
                if (m.isReg) {
                    for (int i = 0; i < 8; i++) second[i]   = (U8)(xmm[m.rmIndex].lo >> (i*8));
                    for (int i = 0; i < 8; i++) second[i+8] = (U8)(xmm[m.rmIndex].hi >> (i*8));
                } else {
                    // Implicit-length forms may legitimately run past a page
                    // that holds only the terminator, but the whole 16 bytes
                    // are still one aligned operand, so read them as such.
                    const U64 lo = memory->readq(m.effAddr);
                    const U64 hi = memory->readq(m.effAddr + 8);
                    for (int i = 0; i < 8; i++) second[i]   = (U8)(lo >> (i*8));
                    for (int i = 0; i < 8; i++) second[i+8] = (U8)(hi >> (i*8));
                }
                const U8 imm = fetchByte(rip + opOff + 3 + m.length);
                const bool explicitLength = (op3 & 0x02) == 0;
                const bool maskForm = (op3 & 0x01) == 0;
                U32 firstLength;
                U32 secondLength;
                if (explicitLength) {
                    firstLength = boxedvn::sse42StringExplicitLength(reg[X64_RAX].u64, rexW, imm);
                    secondLength = boxedvn::sse42StringExplicitLength(reg[X64_RDX].u64, rexW, imm);
                } else {
                    firstLength = boxedvn::sse42StringImplicitLength(first, imm);
                    secondLength = boxedvn::sse42StringImplicitLength(second, imm);
                }
                const boxedvn::Sse42StringResult result =
                    boxedvn::sse42StringCompare(first, firstLength, second, secondLength, imm);
                if (maskForm) {
                    U64 nLo = 0, nHi = 0;
                    for (int i = 0; i < 8; i++) nLo |= ((U64)result.xmm0[i])   << (i*8);
                    for (int i = 0; i < 8; i++) nHi |= ((U64)result.xmm0[i+8]) << (i*8);
                    xmm[0].lo = nLo;
                    xmm[0].hi = nHi;
                } else {
                    // ECX is written as a 32-bit result: the upper half of
                    // RCX is zeroed in both the 32- and 64-bit forms.
                    reg[X64_RCX].setU64((U64)result.index);
                }
                rflags &= ~(X64_ZF | X64_CF | X64_OF | X64_SF | X64_AF | X64_PF);
                if (result.cf) rflags |= X64_CF;
                if (result.zf) rflags |= X64_ZF;
                if (result.sf) rflags |= X64_SF;
                if (result.of) rflags |= X64_OF;
                U32 used = opOff + 3 + m.length + 1;
                rip += used;
                return used;
            }
            // ---- SSSE3 packed integer ops sharing 66 0F 38 /r ----
            //
            // PABSB/W/D (1C/1D/1E): packed absolute value of signed bytes/
            // words/dwords. PHADDW/D (01/02): horizontal add of adjacent
            // element pairs across dest then src. PSIGNB/W/D (08/09/0A):
            // apply the sign of each src element to the corresponding dest
            // element (negate if src<0, zero if src==0, keep if src>0).
            // All are reg/reg or reg/mem, no immediate. clang emits these
            // for vectorised abs()/reductions and libc++ ranges code.
            if (op2 == 0x38 && (op3 == 0x1C || op3 == 0x1D || op3 == 0x1E ||
                                op3 == 0x01 || op3 == 0x02 ||
                                op3 == 0x08 || op3 == 0x09 || op3 == 0x0A)) {
                ModRM m = decodeModRM(rip + opOff + 3, p, 0);
                U64 sLo, sHi;
                if (m.isReg) {
                    sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi;
                } else {
                    sLo = memory->readq(m.effAddr);
                    sHi = memory->readq(m.effAddr + 8);
                }
                U64 dLo = xmm[m.regField].lo;
                U64 dHi = xmm[m.regField].hi;

                // Unpack dest and src into element arrays at the right width.
                auto unpackB = [](U64 lo, U64 hi, S8 out[16]) {
                    for (int i = 0; i < 8; i++) out[i]   = (S8)(U8)(lo >> (i*8));
                    for (int i = 0; i < 8; i++) out[i+8] = (S8)(U8)(hi >> (i*8));
                };
                auto unpackW = [](U64 lo, U64 hi, S16 out[8]) {
                    for (int i = 0; i < 4; i++) out[i]   = (S16)(U16)(lo >> (i*16));
                    for (int i = 0; i < 4; i++) out[i+4] = (S16)(U16)(hi >> (i*16));
                };
                auto unpackD = [](U64 lo, U64 hi, S32 out[4]) {
                    out[0] = (S32)(U32)lo; out[1] = (S32)(U32)(lo >> 32);
                    out[2] = (S32)(U32)hi; out[3] = (S32)(U32)(hi >> 32);
                };
                auto packB = [](S8 in[16], U64& lo, U64& hi) {
                    lo = hi = 0;
                    for (int i = 0; i < 8; i++) lo |= ((U64)(U8)in[i])   << (i*8);
                    for (int i = 0; i < 8; i++) hi |= ((U64)(U8)in[i+8]) << (i*8);
                };
                auto packW = [](S16 in[8], U64& lo, U64& hi) {
                    lo = hi = 0;
                    for (int i = 0; i < 4; i++) lo |= ((U64)(U16)in[i])   << (i*16);
                    for (int i = 0; i < 4; i++) hi |= ((U64)(U16)in[i+4]) << (i*16);
                };
                auto packD = [](S32 in[4], U64& lo, U64& hi) {
                    lo = ((U64)(U32)in[0]) | (((U64)(U32)in[1]) << 32);
                    hi = ((U64)(U32)in[2]) | (((U64)(U32)in[3]) << 32);
                };

                U64 nLo = 0, nHi = 0;
                if (op3 == 0x1C) {            // PABSB
                    S8 d[16]; unpackB(dLo, dHi, d);
                    S8 r[16];
                    for (int i = 0; i < 16; i++) r[i] = (S8)(d[i] < 0 ? -d[i] : d[i]);
                    packB(r, nLo, nHi);
                } else if (op3 == 0x1D) {     // PABSW
                    S16 d[8]; unpackW(dLo, dHi, d);
                    S16 r[8];
                    for (int i = 0; i < 8; i++) r[i] = (S16)(d[i] < 0 ? -d[i] : d[i]);
                    packW(r, nLo, nHi);
                } else if (op3 == 0x1E) {     // PABSD
                    S32 d[4]; unpackD(dLo, dHi, d);
                    S32 r[4];
                    for (int i = 0; i < 4; i++) r[i] = (d[i] < 0 ? -d[i] : d[i]);
                    packD(r, nLo, nHi);
                } else if (op3 == 0x01) {     // PHADDW
                    S16 d[8], s[8]; unpackW(dLo, dHi, d); unpackW(sLo, sHi, s);
                    S16 r[8];
                    r[0]=(S16)(d[0]+d[1]); r[1]=(S16)(d[2]+d[3]);
                    r[2]=(S16)(d[4]+d[5]); r[3]=(S16)(d[6]+d[7]);
                    r[4]=(S16)(s[0]+s[1]); r[5]=(S16)(s[2]+s[3]);
                    r[6]=(S16)(s[4]+s[5]); r[7]=(S16)(s[6]+s[7]);
                    packW(r, nLo, nHi);
                } else if (op3 == 0x02) {     // PHADDD
                    S32 d[4], s[4]; unpackD(dLo, dHi, d); unpackD(sLo, sHi, s);
                    S32 r[4];
                    r[0]=d[0]+d[1]; r[1]=d[2]+d[3];
                    r[2]=s[0]+s[1]; r[3]=s[2]+s[3];
                    packD(r, nLo, nHi);
                } else if (op3 == 0x08) {     // PSIGNB
                    S8 d[16], s[16]; unpackB(dLo, dHi, d); unpackB(sLo, sHi, s);
                    S8 r[16];
                    for (int i = 0; i < 16; i++)
                        r[i] = (S8)(s[i] < 0 ? -d[i] : (s[i] == 0 ? 0 : d[i]));
                    packB(r, nLo, nHi);
                } else if (op3 == 0x09) {     // PSIGNW
                    S16 d[8], s[8]; unpackW(dLo, dHi, d); unpackW(sLo, sHi, s);
                    S16 r[8];
                    for (int i = 0; i < 8; i++)
                        r[i] = (S16)(s[i] < 0 ? -d[i] : (s[i] == 0 ? 0 : d[i]));
                    packW(r, nLo, nHi);
                } else {                       // PSIGND (0x0A)
                    S32 d[4], s[4]; unpackD(dLo, dHi, d); unpackD(sLo, sHi, s);
                    S32 r[4];
                    for (int i = 0; i < 4; i++)
                        r[i] = (s[i] < 0 ? -d[i] : (s[i] == 0 ? 0 : d[i]));
                    packD(r, nLo, nHi);
                }
                xmm[m.regField].lo = nLo;
                xmm[m.regField].hi = nHi;
                U32 used = opOff + 3 + m.length;
                rip += used;
                return used;
            }
            // Other 0F 38 / 0F 3A forms — fall through to default panic.
        }

        // ---- SSE2 scalar double-precision FP ----
        //
        // Scalar SSE2 ops touch only the low 64 bits of the XMM register
        // (one double); the high 64 bits are left untouched for reg/reg
        // forms and zeroed for memory loads via MOVSD (per the Intel SDM
        // wording, but glibc's hot paths only care about the low qword,
        // so we leave .hi alone for moves between registers too).
        //
        // We use type-punning via memcpy on a U64<->double pair to stay
        // strict-aliasing-clean.
        auto u64ToDouble = [](U64 bits) -> double {
            double d; std::memcpy(&d, &bits, sizeof(d)); return d;
        };
        auto doubleToU64 = [](double d) -> U64 {
            U64 bits; std::memcpy(&bits, &d, sizeof(bits)); return bits;
        };

        // MOVSD xmm, xmm/m64   F2 0F 10 /r   (load low qword; zero high if mem)
        // MOVSD xmm/m64, xmm   F2 0F 11 /r   (store low qword)
        if ((op2 == 0x10 || op2 == 0x11) && p.rep == 0xF2) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            bool isStore = (op2 == 0x11);
            if (isStore) {
                U64 v = xmm[m.regField].lo;
                if (m.isReg) xmm[m.rmIndex].lo = v;
                else         memory->writeq(m.effAddr, v);
            } else {
                U64 v;
                if (m.isReg) {
                    v = xmm[m.rmIndex].lo;
                    xmm[m.regField].lo = v; // .hi unchanged for reg/reg
                } else {
                    v = memory->readq(m.effAddr);
                    xmm[m.regField].lo = v;
                    xmm[m.regField].hi = 0; // memory form zeros high
                }
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // Scalar FP arithmetic — all share the F2 0F prefix and a ModR/M
        // operand that is either xmm or m64.
        //   ADDSD  F2 0F 58
        //   MULSD  F2 0F 59
        //   SUBSD  F2 0F 5C
        //   DIVSD  F2 0F 5E
        //   SQRTSD F2 0F 51
        //   MINSD  F2 0F 5D
        //   MAXSD  F2 0F 5F
        if (p.rep == 0xF2 &&
            (op2 == 0x58 || op2 == 0x59 || op2 == 0x5C || op2 == 0x5E ||
             op2 == 0x51 || op2 == 0x5D || op2 == 0x5F)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcBits = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
            double a = u64ToDouble(xmm[m.regField].lo);
            double b = u64ToDouble(srcBits);
            double r;
            switch (op2) {
                case 0x58: r = a + b; break;
                case 0x59: r = a * b; break;
                case 0x5C: r = a - b; break;
                case 0x5E: r = a / b; break;
                case 0x51: r = std::sqrt(b); break; // SQRTSD reads src, ignores dst
                case 0x5D: r = (a < b) ? a : b; break;
                case 0x5F: r = (a > b) ? a : b; break;
                default:   r = a; break;
            }
            xmm[m.regField].lo = doubleToU64(r);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // Packed double-precision FP arithmetic — 66 0F prefix selects the
        // packed-double form. Operates on two doubles in parallel (low qword
        // and high qword of the XMM regs). Discovered via musl libm
        // (sqrt/sin/cos use packed double pairs internally for table-based
        // polynomial evaluation).
        //   ADDPD  66 0F 58
        //   MULPD  66 0F 59
        //   SUBPD  66 0F 5C
        //   DIVPD  66 0F 5E
        //   SQRTPD 66 0F 51
        //   MINPD  66 0F 5D
        //   MAXPD  66 0F 5F
        if (osize66 &&
            (op2 == 0x58 || op2 == 0x59 || op2 == 0x5C || op2 == 0x5E ||
             op2 == 0x51 || op2 == 0x5D || op2 == 0x5F)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            double aLo = u64ToDouble(xmm[m.regField].lo);
            double aHi = u64ToDouble(xmm[m.regField].hi);
            double bLo = u64ToDouble(srcLo);
            double bHi = u64ToDouble(srcHi);
            double rLo, rHi;
            switch (op2) {
                case 0x58: rLo = aLo + bLo; rHi = aHi + bHi; break;
                case 0x59: rLo = aLo * bLo; rHi = aHi * bHi; break;
                case 0x5C: rLo = aLo - bLo; rHi = aHi - bHi; break;
                case 0x5E: rLo = aLo / bLo; rHi = aHi / bHi; break;
                case 0x51: rLo = std::sqrt(bLo); rHi = std::sqrt(bHi); break;
                case 0x5D: rLo = (aLo < bLo) ? aLo : bLo;
                           rHi = (aHi < bHi) ? aHi : bHi; break;
                case 0x5F: rLo = (aLo > bLo) ? aLo : bLo;
                           rHi = (aHi > bHi) ? aHi : bHi; break;
                default:   rLo = aLo; rHi = aHi; break;
            }
            xmm[m.regField].lo = doubleToU64(rLo);
            xmm[m.regField].hi = doubleToU64(rHi);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // Packed single-precision FP arithmetic — no prefix selects PS form.
        //   ADDPS  0F 58, MULPS 0F 59, SUBPS 0F 5C, DIVPS 0F 5E,
        //   SQRTPS 0F 51, MINPS 0F 5D, MAXPS 0F 5F
        // Operates on 4 floats packed in the XMM register.
        if (!osize66 && p.rep == 0 &&
            (op2 == 0x58 || op2 == 0x59 || op2 == 0x5C || op2 == 0x5E ||
             op2 == 0x51 || op2 == 0x5D || op2 == 0x5F)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) {
                srcLo = xmm[m.rmIndex].lo;
                srcHi = xmm[m.rmIndex].hi;
            } else {
                srcLo = memory->readq(m.effAddr);
                srcHi = memory->readq(m.effAddr + 8);
            }
            auto u32f = [](U32 b){ float f; std::memcpy(&f,&b,4); return f; };
            auto fu32 = [](float f){ U32 b; std::memcpy(&b,&f,4); return b; };
            float a[4], b[4], r[4];
            a[0] = u32f((U32)(xmm[m.regField].lo & 0xFFFFFFFFULL));
            a[1] = u32f((U32)(xmm[m.regField].lo >> 32));
            a[2] = u32f((U32)(xmm[m.regField].hi & 0xFFFFFFFFULL));
            a[3] = u32f((U32)(xmm[m.regField].hi >> 32));
            b[0] = u32f((U32)(srcLo & 0xFFFFFFFFULL));
            b[1] = u32f((U32)(srcLo >> 32));
            b[2] = u32f((U32)(srcHi & 0xFFFFFFFFULL));
            b[3] = u32f((U32)(srcHi >> 32));
            for (int i = 0; i < 4; i++) {
                switch (op2) {
                    case 0x58: r[i] = a[i] + b[i]; break;
                    case 0x59: r[i] = a[i] * b[i]; break;
                    case 0x5C: r[i] = a[i] - b[i]; break;
                    case 0x5E: r[i] = a[i] / b[i]; break;
                    case 0x51: r[i] = std::sqrt(b[i]); break;
                    case 0x5D: r[i] = (a[i] < b[i]) ? a[i] : b[i]; break;
                    case 0x5F: r[i] = (a[i] > b[i]) ? a[i] : b[i]; break;
                    default:   r[i] = a[i]; break;
                }
            }
            xmm[m.regField].lo = (U64)fu32(r[0]) | ((U64)fu32(r[1]) << 32);
            xmm[m.regField].hi = (U64)fu32(r[2]) | ((U64)fu32(r[3]) << 32);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // ---- SSE3 horizontal FP: HADDPS/HSUBPS (F2 0F 7C/7D),
        //      HADDPD/HSUBPD (66 0F 7C/7D) ----
        //
        // HADD pairs adjacent lanes: for HADDPS the result is
        //   { d0+d1, d2+d3, s0+s1, s2+s3 }  (singles)
        // HADDPD:
        //   { d0+d1, s0+s1 }                (doubles)
        // HSUB is the same with subtraction (lane0-lane1, etc). clang emits
        // these for std::accumulate / reduction loops over float arrays.
        if ((op2 == 0x7C || op2 == 0x7D) &&
            (p.rep == 0xF2 || osize66)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcLo, srcHi;
            if (m.isReg) { srcLo = xmm[m.rmIndex].lo; srcHi = xmm[m.rmIndex].hi; }
            else { srcLo = memory->readq(m.effAddr); srcHi = memory->readq(m.effAddr + 8); }
            bool isSub = (op2 == 0x7D);
            if (osize66) {
                // packed double
                double d0 = u64ToDouble(xmm[m.regField].lo);
                double d1 = u64ToDouble(xmm[m.regField].hi);
                double s0 = u64ToDouble(srcLo);
                double s1 = u64ToDouble(srcHi);
                double r0 = isSub ? (d0 - d1) : (d0 + d1);
                double r1 = isSub ? (s0 - s1) : (s0 + s1);
                xmm[m.regField].lo = doubleToU64(r0);
                xmm[m.regField].hi = doubleToU64(r1);
            } else {
                // packed single (F2 prefix)
                auto u32f = [](U32 b){ float f; std::memcpy(&f,&b,4); return f; };
                auto fu32 = [](float f){ U32 b; std::memcpy(&b,&f,4); return b; };
                float d[4], s[4];
                d[0]=u32f((U32)xmm[m.regField].lo); d[1]=u32f((U32)(xmm[m.regField].lo>>32));
                d[2]=u32f((U32)xmm[m.regField].hi); d[3]=u32f((U32)(xmm[m.regField].hi>>32));
                s[0]=u32f((U32)srcLo); s[1]=u32f((U32)(srcLo>>32));
                s[2]=u32f((U32)srcHi); s[3]=u32f((U32)(srcHi>>32));
                float r[4];
                r[0]=isSub?(d[0]-d[1]):(d[0]+d[1]);
                r[1]=isSub?(d[2]-d[3]):(d[2]+d[3]);
                r[2]=isSub?(s[0]-s[1]):(s[0]+s[1]);
                r[3]=isSub?(s[2]-s[3]):(s[2]+s[3]);
                xmm[m.regField].lo = (U64)fu32(r[0]) | ((U64)fu32(r[1]) << 32);
                xmm[m.regField].hi = (U64)fu32(r[2]) | ((U64)fu32(r[3]) << 32);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // ---- SSE3 MOVSHDUP (F3 0F 16) / MOVSLDUP (F3 0F 12) ----
        //   MOVSHDUP {a0,a1,a2,a3} -> {a1,a1,a3,a3} (duplicate odd lanes)
        //   MOVSLDUP {a0,a1,a2,a3} -> {a0,a0,a2,a2} (duplicate even lanes)
        // Used in complex-arithmetic and broadcast patterns. Note 0F 12 with
        // F3 is MOVSLDUP, distinct from MOVLPS (no prefix) / MOVDDUP (F2).
        if (p.rep == 0xF3 && (op2 == 0x16 || op2 == 0x12)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 sLo, sHi;
            if (m.isReg) { sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi; }
            else { sLo = memory->readq(m.effAddr); sHi = memory->readq(m.effAddr + 8); }
            U32 e0 = (U32)sLo, e1 = (U32)(sLo >> 32);
            U32 e2 = (U32)sHi, e3 = (U32)(sHi >> 32);
            U32 r0, r1, r2, r3;
            if (op2 == 0x16) { r0 = e1; r1 = e1; r2 = e3; r3 = e3; }   // SHDUP
            else             { r0 = e0; r1 = e0; r2 = e2; r3 = e2; }   // SLDUP
            xmm[m.regField].lo = (U64)r0 | ((U64)r1 << 32);
            xmm[m.regField].hi = (U64)r2 | ((U64)r3 << 32);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // ---- SSE4.1 packed dword min/max/mul: 66 0F 38 38/39/3D/3C/40 ----
        //   PMINSD 39, PMAXSD 3D, PMINSB 38, PMAXSB 3C, PMULLD 40
        // (the byte forms 38/3C round out the signed min/max family). All
        // are 66 0F 38 /r with a standard ModRM. clang emits PMULLD for
        // int-vector multiply and PMIN/PMAXSD for std::min/max over int[].
        if (osize66 && op2 == 0x38) {
            U8 op3b = fetchByte(rip + opOff + 2);
            if (op3b == 0x39 || op3b == 0x3D || op3b == 0x40 ||
                op3b == 0x38 || op3b == 0x3C) {
                ModRM m = decodeModRM(rip + opOff + 3, p, 0);
                U64 sLo, sHi;
                if (m.isReg) { sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi; }
                else { sLo = memory->readq(m.effAddr); sHi = memory->readq(m.effAddr + 8); }
                U64 dLo = xmm[m.regField].lo, dHi = xmm[m.regField].hi;
                U64 nLo, nHi;
                if (op3b == 0x38 || op3b == 0x3C) {
                    // signed byte min(38)/max(3C)
                    auto doByte = [&](U64 dq, U64 sq) -> U64 {
                        U64 out = 0;
                        for (int i = 0; i < 8; i++) {
                            S8 dv = (S8)(U8)(dq >> (i*8));
                            S8 sv = (S8)(U8)(sq >> (i*8));
                            S8 rv = (op3b == 0x38) ? (dv < sv ? dv : sv)
                                                   : (dv > sv ? dv : sv);
                            out |= ((U64)(U8)rv) << (i*8);
                        }
                        return out;
                    };
                    nLo = doByte(dLo, sLo); nHi = doByte(dHi, sHi);
                } else {
                    // dword min(39)/max(3D)/mullo(40)
                    auto doDword = [&](U64 dq, U64 sq) -> U64 {
                        S32 d0 = (S32)(U32)dq, d1 = (S32)(U32)(dq >> 32);
                        S32 s0 = (S32)(U32)sq, s1 = (S32)(U32)(sq >> 32);
                        S32 r0, r1;
                        if (op3b == 0x39) { r0 = d0 < s0 ? d0 : s0; r1 = d1 < s1 ? d1 : s1; }
                        else if (op3b == 0x3D) { r0 = d0 > s0 ? d0 : s0; r1 = d1 > s1 ? d1 : s1; }
                        else { r0 = (S32)((U32)d0 * (U32)s0); r1 = (S32)((U32)d1 * (U32)s1); }
                        return ((U64)(U32)r0) | (((U64)(U32)r1) << 32);
                    };
                    nLo = doDword(dLo, sLo); nHi = doDword(dHi, sHi);
                }
                xmm[m.regField].lo = nLo;
                xmm[m.regField].hi = nHi;
                U32 used = opOff + 3 + m.length;
                rip += used;
                return used;
            }
        }

        // CVTSI2SD xmm, r/m32   F2 0F 2A /r       (REX.W → r/m64)
        // Convert int to double; result in low 64 of dst, high unchanged.
        if (op2 == 0x2A && p.rep == 0xF2) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 srcSize = rexW ? 8 : 4;
            U64 srcRaw = loadRM(m, srcSize, rexPresent);
            double d = rexW
                ? (double)(S64)srcRaw
                : (double)(S32)(U32)srcRaw;
            xmm[m.regField].lo = doubleToU64(d);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CVTSD2SS xmm, xmm/m64   F2 0F 5A /r — narrow double (src low qword)
        // to single, writing the result into dst's low 32 bits (high 32 of the
        // low qword preserved, per Intel). winex11/win32u hits this on DPI/
        // scaling math.
        if (op2 == 0x5A && p.rep == 0xF2) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcBits = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
            float r = (float)u64ToDouble(srcBits);
            // float -> raw bits (u32ToFloat/floatToU32 helpers are declared
            // further down in the F3 block, so bit-cast inline here).
            U32 rBits; std::memcpy(&rBits, &r, sizeof(rBits));
            U64 keep = xmm[m.regField].lo & 0xFFFFFFFF00000000ULL;
            xmm[m.regField].lo = keep | (U64)rBits;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CVTSD2SI r32, xmm/m64   F2 0F 2D /r     (REX.W → r64)
        // CVTTSD2SI r32, xmm/m64  F2 0F 2C /r     (REX.W → r64) — truncate
        // Convert double to signed int. Compilers emit 2C (truncate) far
        // more often than 2D (current rounding) because C's int cast is
        // defined as truncate. Bodies are identical here — our 2D path
        // already truncates via C cast — so the only difference is which
        // opcode bytes we accept.
        if ((op2 == 0x2D || op2 == 0x2C) && p.rep == 0xF2) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcBits = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
            double d = u64ToDouble(srcBits);
            if (rexW) reg[m.regField].setU64((U64)(S64)d);
            else      reg[m.regField].setU32((U32)(S32)d);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CMPSD xmm, xmm/m64, imm8   F2 0F C2 /r ib — compare two doubles per
        // the imm8 predicate; write an all-ones (true) or all-zero (false)
        // 64-bit mask into dst's low qword (high qword preserved). The 8 core
        // predicates (imm8 & 7): 0 EQ, 1 LT, 2 LE, 3 UNORD, 4 NEQ, 5 NLT,
        // 6 NLE, 7 ORD. winex11/win32u emits CMPSD ...,6 (NLE) for clamp math.
        if (op2 == 0xC2 && p.rep == 0xF2) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcBits = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            double a = u64ToDouble(xmm[m.regField].lo);
            double b = u64ToDouble(srcBits);
            bool unordered = std::isnan(a) || std::isnan(b);
            bool res;
            switch (imm & 7) {
                case 0: res = (a == b); break;                  // EQ
                case 1: res = (a <  b); break;                  // LT
                case 2: res = (a <= b); break;                  // LE
                case 3: res = unordered; break;                 // UNORD
                case 4: res = unordered || (a != b); break;     // NEQ
                case 5: res = unordered || !(a <  b); break;    // NLT
                case 6: res = unordered || !(a <= b); break;    // NLE
                default: res = !unordered; break;               // ORD
            }
            xmm[m.regField].lo = res ? 0xFFFFFFFFFFFFFFFFULL : 0ULL;
            U32 used = opOff + 2 + m.length + 1; // +imm8
            rip += used;
            return used;
        }

        // UCOMISD xmm, xmm/m64   66 0F 2E /r
        // COMISD  xmm, xmm/m64   66 0F 2F /r
        // Compare two doubles, set EFLAGS ZF/PF/CF per result. UCOMISD and
        // COMISD only differ in whether QNaN raises #IA — we treat them
        // identically (no FP exceptions modelled).
        if ((op2 == 0x2E || op2 == 0x2F) && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 srcBits = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
            double a = u64ToDouble(xmm[m.regField].lo);
            double b = u64ToDouble(srcBits);
            // Per Intel SDM: unordered → ZF=PF=CF=1, greater → all 0,
            // less → CF=1, equal → ZF=1. Also clears OF/SF/AF.
            U64 newFlags = rflags & ~(X64_ZF | X64_PF | X64_CF |
                                      X64_OF | X64_SF | X64_AF);
            if (std::isnan(a) || std::isnan(b)) {
                newFlags |= X64_ZF | X64_PF | X64_CF;
            } else if (a > b) {
                // all three remain 0
            } else if (a < b) {
                newFlags |= X64_CF;
            } else {
                newFlags |= X64_ZF;
            }
            rflags = newFlags;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // ---- SSE scalar single-precision FP ----
        //
        // Mirror image of the F2 0F double-precision block. F3 prefix +
        // 0F xx selects the scalar single (float) form. Touches only the
        // low 32 bits (one float) of the XMM register; bits 32..127 are
        // left untouched on reg/reg moves and zeroed on memory loads.
        auto u32ToFloat = [](U32 bits) -> float {
            float f; std::memcpy(&f, &bits, sizeof(f)); return f;
        };
        auto floatToU32 = [](float f) -> U32 {
            U32 bits; std::memcpy(&bits, &f, sizeof(bits)); return bits;
        };

        // MOVSS xmm, xmm/m32   F3 0F 10 /r
        // MOVSS xmm/m32, xmm   F3 0F 11 /r
        if ((op2 == 0x10 || op2 == 0x11) && p.rep == 0xF3) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            bool isStore = (op2 == 0x11);
            if (isStore) {
                U32 v = (U32)(xmm[m.regField].lo & 0xFFFFFFFFULL);
                if (m.isReg) {
                    // reg/reg: replace low 32 bits, leave rest.
                    U64 keep = xmm[m.rmIndex].lo & 0xFFFFFFFF00000000ULL;
                    xmm[m.rmIndex].lo = keep | v;
                } else {
                    memory->writed(m.effAddr, v);
                }
            } else {
                U32 v;
                if (m.isReg) {
                    v = (U32)(xmm[m.rmIndex].lo & 0xFFFFFFFFULL);
                    U64 keep = xmm[m.regField].lo & 0xFFFFFFFF00000000ULL;
                    xmm[m.regField].lo = keep | v;
                    // .hi unchanged for reg/reg
                } else {
                    v = memory->readd(m.effAddr);
                    xmm[m.regField].lo = (U64)v; // zero-extend; high32 of lo cleared
                    xmm[m.regField].hi = 0;      // memory form zeros high qword too
                }
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // Scalar single arithmetic:
        //   ADDSS  F3 0F 58
        //   MULSS  F3 0F 59
        //   SUBSS  F3 0F 5C
        //   DIVSS  F3 0F 5E
        //   SQRTSS F3 0F 51
        //   MINSS  F3 0F 5D
        //   MAXSS  F3 0F 5F
        if (p.rep == 0xF3 &&
            (op2 == 0x58 || op2 == 0x59 || op2 == 0x5C || op2 == 0x5E ||
             op2 == 0x51 || op2 == 0x5D || op2 == 0x5F)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 srcBits = m.isReg
                ? (U32)(xmm[m.rmIndex].lo & 0xFFFFFFFFULL)
                : memory->readd(m.effAddr);
            float a = u32ToFloat((U32)(xmm[m.regField].lo & 0xFFFFFFFFULL));
            float b = u32ToFloat(srcBits);
            float r;
            switch (op2) {
                case 0x58: r = a + b; break;
                case 0x59: r = a * b; break;
                case 0x5C: r = a - b; break;
                case 0x5E: r = a / b; break;
                case 0x51: r = std::sqrt(b); break; // SQRT uses src as input
                case 0x5D: r = (a < b) ? a : b; break;
                case 0x5F: r = (a > b) ? a : b; break;
                default:   r = a; break;
            }
            U64 keep = xmm[m.regField].lo & 0xFFFFFFFF00000000ULL;
            xmm[m.regField].lo = keep | (U64)floatToU32(r);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CVTSI2SS xmm, r/m32 (or r/m64 with REX.W)  F3 0F 2A /r
        if (op2 == 0x2A && p.rep == 0xF3) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            float f;
            if (rexW) {
                S64 v = m.isReg ? (S64)reg[m.rmIndex].u64 : (S64)memory->readq(m.effAddr);
                f = (float)v;
            } else {
                S32 v = m.isReg ? (S32)reg[m.rmIndex].u32 : (S32)memory->readd(m.effAddr);
                f = (float)v;
            }
            U64 keep = xmm[m.regField].lo & 0xFFFFFFFF00000000ULL;
            xmm[m.regField].lo = keep | (U64)floatToU32(f);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CVTSS2SD xmm, xmm/m32   F3 0F 5A /r — widen single (src low 32 bits)
        // to double, writing the full low qword of dst. winex11/win32u hits
        // this converting single-precision metrics to double for layout math.
        if (op2 == 0x5A && p.rep == 0xF3) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 srcBits = m.isReg
                ? (U32)(xmm[m.rmIndex].lo & 0xFFFFFFFFULL)
                : memory->readd(m.effAddr);
            double d = (double)u32ToFloat(srcBits);
            xmm[m.regField].lo = doubleToU64(d);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CVTSS2SI  r32/r64, xmm/m32  F3 0F 2D /r
        // CVTTSS2SI r32/r64, xmm/m32  F3 0F 2C /r  — truncating variant
        // Same rationale as F2 0x2C: compilers prefer the truncating form
        // for C int casts; our 2D path already uses C cast (truncate) so
        // the body is shared.
        if ((op2 == 0x2D || op2 == 0x2C) && p.rep == 0xF3) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 srcBits = m.isReg
                ? (U32)(xmm[m.rmIndex].lo & 0xFFFFFFFFULL)
                : memory->readd(m.effAddr);
            float f = u32ToFloat(srcBits);
            if (rexW) reg[m.regField].setU64((U64)(S64)f);
            else      reg[m.regField].setU32((U32)(S32)f);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CMPSS xmm, xmm/m32, imm8   F3 0F C2 /r ib — scalar single sibling of
        // CMPSD; writes an all-ones/all-zero 32-bit mask into dst's low 32 bits
        // (rest of the low qword preserved). Same imm8 predicates as CMPSD.
        if (op2 == 0xC2 && p.rep == 0xF3) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 srcBits = m.isReg
                ? (U32)(xmm[m.rmIndex].lo & 0xFFFFFFFFULL)
                : memory->readd(m.effAddr);
            U8 imm = fetchByte(rip + opOff + 2 + m.length);
            float a = u32ToFloat((U32)(xmm[m.regField].lo & 0xFFFFFFFFULL));
            float b = u32ToFloat(srcBits);
            bool unordered = std::isnan(a) || std::isnan(b);
            bool res;
            switch (imm & 7) {
                case 0: res = (a == b); break;
                case 1: res = (a <  b); break;
                case 2: res = (a <= b); break;
                case 3: res = unordered; break;
                case 4: res = unordered || (a != b); break;
                case 5: res = unordered || !(a <  b); break;
                case 6: res = unordered || !(a <= b); break;
                default: res = !unordered; break;
            }
            U64 keep = xmm[m.regField].lo & 0xFFFFFFFF00000000ULL;
            xmm[m.regField].lo = keep | (res ? 0xFFFFFFFFULL : 0ULL);
            U32 used = opOff + 2 + m.length + 1; // +imm8
            rip += used;
            return used;
        }

        // UNPCKLPD xmm, xmm/m128  66 0F 14 /r — interleave low qwords.
        //   dst.hi = src.lo;  dst.lo unchanged.
        // Used by glibc/Wine to assemble a {lo, hi} pair of doubles from
        // two separate xmm.lo halves.
        if (op2 == 0x14 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 sLo = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
            xmm[m.regField].hi = sLo;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // UNPCKHPD xmm, xmm/m128  66 0F 15 /r — interleave high qwords.
        //   dst.lo = dst.hi;  dst.hi = src.hi.
        // Used by musl libm to pull the high double out of a packed pair
        // for follow-up scalar operations.
        if (op2 == 0x15 && osize66) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 sHi;
            if (m.isReg) sHi = xmm[m.rmIndex].hi;
            else         sHi = memory->readq(m.effAddr + 8);
            xmm[m.regField].lo = xmm[m.regField].hi;
            xmm[m.regField].hi = sHi;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // MOVLPD/MOVLPS xmm, m64  66 0F 12 /r  /  0F 12 /r — load m64 to
        //   xmm.lo, leave xmm.hi unchanged.
        // MOVHPD/MOVHPS xmm, m64  66 0F 16 /r  /  0F 16 /r — load m64 to
        //   xmm.hi, leave xmm.lo unchanged.
        // MOVDDUP        xmm, xmm/m64  F2 0F 12 /r — load m64, broadcast to
        //   BOTH halves of xmm. ld.so + libm use this heavily for scalar
        //   doubles fed into packed math.
        // Register-form 66 0F 12 /r and 0F 12 /r is MOVHLPS/MOVLPS-reg
        //   (dst.lo = src.hi for 0F 12 reg; dst.lo = src.lo for 66 0F 12 reg).
        if ((op2 == 0x12 || op2 == 0x16) && p.rep == 0) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 val;
            if (m.isReg) {
                // 0F 12 reg-form is MOVHLPS: dst.lo = src.hi
                // 66 0F 12 reg-form is MOVLPD (treated as MOVAPD low): dst.lo = src.lo
                // 0F 16 reg-form is MOVLHPS: dst.hi = src.lo
                // 66 0F 16 reg-form is MOVHPD (treated as MOVAPD high): dst.hi = src.hi
                if (op2 == 0x12) val = osize66 ? xmm[m.rmIndex].lo : xmm[m.rmIndex].hi;
                else             val = osize66 ? xmm[m.rmIndex].hi : xmm[m.rmIndex].lo;
            } else {
                val = memory->readq(m.effAddr);
            }
            if (op2 == 0x12) xmm[m.regField].lo = val;
            else             xmm[m.regField].hi = val;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
        if (op2 == 0x12 && p.rep == 0xF2) {
            // MOVDDUP: load m64 (or src.lo if reg), duplicate to both halves
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 val = m.isReg ? xmm[m.rmIndex].lo : memory->readq(m.effAddr);
            xmm[m.regField].lo = val;
            xmm[m.regField].hi = val;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // MOVLPD m64, xmm  66 0F 13 /r  /  0F 13 /r — store xmm.lo to m64.
        // MOVHPD m64, xmm  66 0F 17 /r  /  0F 17 /r — store xmm.hi to m64.
        // (No register form for 0F 13 / 17.)
        if ((op2 == 0x13 || op2 == 0x17) && p.rep == 0) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (!m.isReg) {
                U64 val = (op2 == 0x13) ? xmm[m.regField].lo : xmm[m.regField].hi;
                memory->writeq(m.effAddr, val);
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // UNPCKLPS xmm, xmm/m128  0F 14 /r — interleave 32-bit floats from
        // low half: dst = {dst[0], src[0], dst[1], src[1]} (each 32-bit).
        // UNPCKHPS xmm, xmm/m128  0F 15 /r — from high half.
        if ((op2 == 0x14 || op2 == 0x15) && !osize66 && p.rep == 0) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 sLo, sHi;
            if (m.isReg) { sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi; }
            else         { sLo = memory->readq(m.effAddr); sHi = memory->readq(m.effAddr + 8); }
            U64 dLo = xmm[m.regField].lo, dHi = xmm[m.regField].hi;
            U32 d[4] = {(U32)dLo,(U32)(dLo>>32),(U32)dHi,(U32)(dHi>>32)};
            U32 s[4] = {(U32)sLo,(U32)(sLo>>32),(U32)sHi,(U32)(sHi>>32)};
            U32 r[4];
            if (op2 == 0x14) { r[0]=d[0]; r[1]=s[0]; r[2]=d[1]; r[3]=s[1]; }
            else             { r[0]=d[2]; r[1]=s[2]; r[2]=d[3]; r[3]=s[3]; }
            xmm[m.regField].lo = (U64)r[0] | ((U64)r[1] << 32);
            xmm[m.regField].hi = (U64)r[2] | ((U64)r[3] << 32);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // ---- Packed FP↔int converts ----
        //
        // All five variants share opcode bytes 0F 5B (FP conversions) or
        // 0F E6 (the F2/F3-specific double-precision variants), and the
        // prefix selects which form. Source is xmm/m128, dest is xmm.

        // CVTDQ2PS   0F 5B /r        — 4× S32 → 4× float
        // CVTPS2DQ   66 0F 5B /r     — 4× float → 4× S32 (round to nearest)
        // CVTTPS2DQ  F3 0F 5B /r     — 4× float → 4× S32 (truncate toward zero)
        if (op2 == 0x5B) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 sLo, sHi;
            if (m.isReg) { sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi; }
            else         { sLo = memory->readq(m.effAddr); sHi = memory->readq(m.effAddr + 8); }
            U32 dwords[4] = { (U32)sLo, (U32)(sLo >> 32), (U32)sHi, (U32)(sHi >> 32) };
            U32 nDw[4];
            if (!osize66 && p.rep == 0) {
                // CVTDQ2PS: dwords are signed ints, write float bits.
                for (int i = 0; i < 4; i++) {
                    float f = (float)(S32)dwords[i];
                    U32 bits; std::memcpy(&bits, &f, 4); nDw[i] = bits;
                }
            } else if (osize66) {
                // CVTPS2DQ: dwords are float bits, write rounded ints.
                for (int i = 0; i < 4; i++) {
                    float f; std::memcpy(&f, &dwords[i], 4);
                    nDw[i] = (U32)(S32)std::lrintf(f);
                }
            } else if (p.rep == 0xF3) {
                // CVTTPS2DQ: float → int truncating.
                for (int i = 0; i < 4; i++) {
                    float f; std::memcpy(&f, &dwords[i], 4);
                    nDw[i] = (U32)(S32)f;
                }
            } else {
                goto unhandled; // F2 0F 5B is not defined
            }
            xmm[m.regField].lo = ((U64)nDw[0]) | (((U64)nDw[1]) << 32);
            xmm[m.regField].hi = ((U64)nDw[2]) | (((U64)nDw[3]) << 32);
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // CVTDQ2PD  F3 0F E6 /r — low 2× S32 → 2× f64
        // CVTPD2DQ  F2 0F E6 /r — 2× f64 → low 2× S32 (rounded), hi qword = 0
        // CVTTPD2DQ 66 0F E6 /r — 2× f64 → low 2× S32 (truncate), hi qword = 0
        // fontconfig/freetype emit the 66-prefixed truncating form heavily.
        if (op2 == 0xE6 && (p.rep == 0xF3 || p.rep == 0xF2 || osize66)) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U64 sLo, sHi;
            if (m.isReg) { sLo = xmm[m.rmIndex].lo; sHi = xmm[m.rmIndex].hi; }
            else         { sLo = memory->readq(m.effAddr); sHi = memory->readq(m.effAddr + 8); }
            if (p.rep == 0xF3) {
                // CVTDQ2PD: take low 2 dwords of src as S32, write 2 f64s.
                S32 i0 = (S32)(U32)sLo;
                S32 i1 = (S32)(U32)(sLo >> 32);
                double d0 = (double)i0, d1 = (double)i1;
                U64 b0, b1;
                std::memcpy(&b0, &d0, 8); std::memcpy(&b1, &d1, 8);
                xmm[m.regField].lo = b0;
                xmm[m.regField].hi = b1;
            } else {
                // CVTPD2DQ (F2, round) / CVTTPD2DQ (66, truncate):
                // 2 f64s → 2 S32s in low qword, high qword zeroed.
                double d0, d1;
                std::memcpy(&d0, &sLo, 8); std::memcpy(&d1, &sHi, 8);
                U32 i0, i1;
                if (osize66) { // truncate toward zero
                    i0 = (U32)(S32)d0;
                    i1 = (U32)(S32)d1;
                } else {       // round to nearest (current rounding mode)
                    i0 = (U32)(S32)std::lrint(d0);
                    i1 = (U32)(S32)std::lrint(d1);
                }
                xmm[m.regField].lo = ((U64)i0) | (((U64)i1) << 32);
                xmm[m.regField].hi = 0;
            }
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // MOVNTI m32/m64, r32/r64  0F C3 /r  — non-temporal store of GPR.
        // glibc's memset/memcpy hot paths use this to bypass cache. We
        // treat it as a plain mov to memory; the cache-bypass hint is a
        // perf nicety the interpreter doesn't model.
        if (op2 == 0xC3) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            if (!m.isReg) {
                U32 width = rexW ? 8 : 4;
                U64 v = rexW ? reg[m.regField].u64 : (U64)reg[m.regField].u32;
                if (width == 8) memory->writeq(m.effAddr, v);
                else            memory->writed(m.effAddr, (U32)v);
            }
            // reg/reg form of 0F C3 is illegal — ignore.
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }

        // UCOMISS xmm, xmm/m32   0F 2E /r   (no 66, no F2/F3 prefix)
        // COMISS  xmm, xmm/m32   0F 2F /r
        if ((op2 == 0x2E || op2 == 0x2F) && !osize66 && p.rep == 0) {
            ModRM m = decodeModRM(rip + opOff + 2, p, 0);
            U32 srcBits = m.isReg
                ? (U32)(xmm[m.rmIndex].lo & 0xFFFFFFFFULL)
                : memory->readd(m.effAddr);
            float a = u32ToFloat((U32)(xmm[m.regField].lo & 0xFFFFFFFFULL));
            float b = u32ToFloat(srcBits);
            U64 newFlags = rflags & ~(X64_ZF | X64_PF | X64_CF |
                                      X64_OF | X64_SF | X64_AF);
            if (std::isnan(a) || std::isnan(b)) {
                newFlags |= X64_ZF | X64_PF | X64_CF;
            } else if (a > b) {
                // all three remain 0
            } else if (a < b) {
                newFlags |= X64_CF;
            } else {
                newFlags |= X64_ZF;
            }
            rflags = newFlags;
            U32 used = opOff + 2 + m.length;
            rip += used;
            return used;
        }
    }

    // 0F AE group: FXSAVE(/0) FXRSTOR(/1) LDMXCSR(/2) STMXCSR(/3) and the
    // mod==11 register forms MFENCE/LFENCE/SFENCE (/5,/6,/7).
    //
    // FXSAVE/FXRSTOR MUST round-trip the XMM registers: glibc's lazy PLT
    // resolver _dl_runtime_resolve_fxsave does FXSAVE before calling
    // _dl_fixup (which clobbers XMM0-7 internally) and FXRSTOR after, so the
    // caller's float/double arguments survive symbol resolution. When these
    // were no-ops, the FIRST call to any float-taking function through the
    // PLT (e.g. printf("%.5f", x)) saw XMM0 clobbered -> the argument read as
    // 0.0 -> "0.00000"; later calls bound the GOT and skipped the resolver,
    // so they were correct. That "first %f prints 0" was the real bug (NOT
    // x87 80-bit precision). We model the legacy 512-byte FXSAVE area only
    // for the XMM block (offset 160, 16 regs x 16 bytes); MXCSR/x87 state is
    // not modeled here but the resolver doesn't depend on it.
    if (op == 0x0F && fetchByte(rip + opOff + 1) == 0xAE) {
        U8 aeModrm = fetchByte(rip + opOff + 2);
        U8 aeReg = (aeModrm >> 3) & 7;
        U8 aeMod = (aeModrm >> 6) & 3;
        ModRM m = decodeModRM(rip + opOff + 2, p, 0);
        U32 used = opOff + 2 + m.length;
        if (aeMod != 3 && aeReg == 0) {
            // FXSAVE m512byte — save XMM0..15 into the legacy area.
            for (int i = 0; i < 16; i++) {
                memory->writeq(m.effAddr + 160 + i * 16,     xmm[i].lo);
                memory->writeq(m.effAddr + 160 + i * 16 + 8, xmm[i].hi);
            }
        } else if (aeMod != 3 && aeReg == 1) {
            // FXRSTOR m512byte — restore XMM0..15 from the legacy area.
            for (int i = 0; i < 16; i++) {
                xmm[i].lo = memory->readq(m.effAddr + 160 + i * 16);
                xmm[i].hi = memory->readq(m.effAddr + 160 + i * 16 + 8);
            }
        }
        // LDMXCSR(/2), STMXCSR(/3) and the fences remain no-ops: we don't
        // model MXCSR exception/rounding bits, and memory ordering is moot
        // for a single-stepped interpreter.
        rip += used;
        return used;
    }

    // x87 FPU minimal subset. Reuses the shared FPU class (common/fpu.h) for
    // register-side state — we drive memory I/O through CPU64::memory ourselves
    // because the FPU::FLD_F64_EA family takes a 32-bit CPU* that we can't
    // satisfy. ModR/M "reg" field is the sub-opcode (/0../7) for memory forms;
    // for register forms (mod==11) the low 3 bits identify ST(i).
dsp_58:
    if (op == 0xD8 || op == 0xD9 || op == 0xDA || op == 0xDB ||
        op == 0xDC || op == 0xDD || op == 0xDE || op == 0xDF) {
        U8 modrmByte = fetchByte(rip + opOff + 1);
        ModRM m = decodeModRM(rip + opOff + 1, p, 0);
        U32 used = opOff + 1 + m.length;
        U8 sub = m.regField & 7; // x87 sub-opcode, ignore REX.R
        if (!m.isReg) {
            // memory form
            if (op == 0xD9 && sub == 0) {
                // FLD m32fp
                U32 bits = memory->readd(m.effAddr);
                fpu.PREP_PUSH();
                fpu.FLD_F32(bits, fpu.STV(0));
            } else if (op == 0xD9 && sub == 3) {
                // FSTP m32fp — convert TOS to f32, store, pop
                double d;
                if (fpu.isRegCached[fpu.top]) {
                    d = fpu.regCache[fpu.top].d;
                } else {
                    d = (double)fpu.getF64(fpu.top);
                }
                union { float f; U32 i; } u; u.f = (float)d;
                memory->writed(m.effAddr, u.i);
                fpu.FPOP();
            } else if (op == 0xD9 && sub == 5) {
                // D9 /5 = FLDCW m16 — load FPU control word. Our soft FPU
                // honors the rounding bits inside FROUND; we store the
                // raw value via cwSet for the few callers that read it
                // back via FNSTCW.
                U16 cw = (U16)memory->readw(m.effAddr);
                fpu.SetCW(cw);
            } else if (op == 0xD9 && sub == 7) {
                // D9 /7 = FNSTCW m16 — store FPU control word. Always
                // succeeds; no exception checks because we don't model
                // FPU exception bits.
                memory->writew(m.effAddr, (U16)fpu.CW());
            } else if (op == 0xDD && sub == 7) {
                // DD /7 = FNSTSW m16 — store status word to memory.
                memory->writew(m.effAddr, (U16)fpu.SW());
            } else if (op == 0xDD && sub == 0) {
                // FLD m64fp
                U64 bits = memory->readq(m.effAddr);
                fpu.PREP_PUSH();
                fpu.FLD_F64(bits, fpu.STV(0));
            } else if (op == 0xDD && sub == 3) {
                // FSTP m64fp — store TOS bits, pop
                U64 bits;
                if (fpu.isRegCached[fpu.top]) {
                    bits = fpu.regCache[fpu.top].l;
                } else {
                    union { double d; U64 u; } u; u.d = fpu.getF64(fpu.top);
                    bits = u.u;
                }
                memory->writeq(m.effAddr, bits);
                fpu.FPOP();
            } else if (op == 0xDC && sub == 0) {
                // FADD m64fp — load operand into scratch slot 8, add to ST(0)
                U64 bits = memory->readq(m.effAddr);
                fpu.FLD_F64(bits, 8);
                fpu.FADD(fpu.STV(0), 8);
            } else if (op == 0xDC && sub == 1) {
                // FMUL m64fp
                U64 bits = memory->readq(m.effAddr);
                fpu.FLD_F64(bits, 8);
                fpu.FMUL(fpu.STV(0), 8);
            } else if (op == 0xDC && sub == 4) {
                // FSUB m64fp — ST(0) = ST(0) - m64
                U64 bits = memory->readq(m.effAddr);
                fpu.FLD_F64(bits, 8);
                fpu.FSUB(fpu.STV(0), 8);
            } else if (op == 0xDC && sub == 5) {
                // FSUBR m64fp — ST(0) = m64 - ST(0)
                U64 bits = memory->readq(m.effAddr);
                fpu.FLD_F64(bits, 8);
                fpu.FSUBR(fpu.STV(0), 8);
            } else if (op == 0xDC && sub == 6) {
                // FDIV m64fp — ST(0) = ST(0) / m64
                U64 bits = memory->readq(m.effAddr);
                fpu.FLD_F64(bits, 8);
                fpu.FDIV(fpu.STV(0), 8);
            } else if (op == 0xDC && sub == 7) {
                // FDIVR m64fp — ST(0) = m64 / ST(0)
                U64 bits = memory->readq(m.effAddr);
                fpu.FLD_F64(bits, 8);
                fpu.FDIVR(fpu.STV(0), 8);
            } else if (op == 0xDA && sub <= 7) {
                // DA /N with m32int operand:
                //   /0 FIADD, /1 FIMUL, /2 FICOM, /3 FICOMP,
                //   /4 FISUB, /5 FISUBR, /6 FIDIV, /7 FIDIVR.
                // Convert int32 to f64, then run the ST(0)-op variant via the
                // shared FPU helpers (which expect both operands in the FPU
                // stack), using scratch slot 8.
                S32 v = (S32)memory->readd(m.effAddr);
                fpu.FLD_I32(v, 8);
                if (sub == 0) {
                    fpu.FADD(fpu.STV(0), 8);
                } else if (sub == 1) {
                    fpu.FMUL(fpu.STV(0), 8);
                } else if (sub == 2 || sub == 3) {
                    // FICOM / FICOMP — set C3/C2/C0 from comparison
                    double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d
                                                         : fpu.getF64(fpu.top);
                    double b = (double)v;
                    if (std::isnan(a) || std::isnan(b)) {
                        cpu64_fpu_set_c012_3(fpu, 1, 1, 1);
                    } else if (a == b) {
                        cpu64_fpu_set_c012_3(fpu, 1, 0, 0);
                    } else if (a < b) {
                        cpu64_fpu_set_c012_3(fpu, 0, 0, 1);
                    } else {
                        cpu64_fpu_set_c012_3(fpu, 0, 0, 0);
                    }
                    if (sub == 3) fpu.FPOP();
                } else if (sub == 4) {
                    fpu.FSUB(fpu.STV(0), 8);
                } else if (sub == 5) {
                    fpu.FSUBR(fpu.STV(0), 8);
                } else if (sub == 6) {
                    fpu.FDIV(fpu.STV(0), 8);
                } else /* sub == 7 */ {
                    fpu.FDIVR(fpu.STV(0), 8);
                }
            } else if (op == 0xDE && sub <= 7) {
                // DE /N with m16int operand: FIADD/FIMUL/FICOM/FICOMP/FISUB/FISUBR/FIDIV/FIDIVR
                // Same control flow as DA but the operand is signed 16-bit.
                S16 v = (S16)memory->readw(m.effAddr);
                fpu.FLD_I16(v, 8);
                if (sub == 0) {
                    fpu.FADD(fpu.STV(0), 8);
                } else if (sub == 1) {
                    fpu.FMUL(fpu.STV(0), 8);
                } else if (sub == 2 || sub == 3) {
                    double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d
                                                         : fpu.getF64(fpu.top);
                    double b = (double)v;
                    if (std::isnan(a) || std::isnan(b)) {
                        cpu64_fpu_set_c012_3(fpu, 1, 1, 1);
                    } else if (a == b) {
                        cpu64_fpu_set_c012_3(fpu, 1, 0, 0);
                    } else if (a < b) {
                        cpu64_fpu_set_c012_3(fpu, 0, 0, 1);
                    } else {
                        cpu64_fpu_set_c012_3(fpu, 0, 0, 0);
                    }
                    if (sub == 3) fpu.FPOP();
                } else if (sub == 4) {
                    fpu.FSUB(fpu.STV(0), 8);
                } else if (sub == 5) {
                    fpu.FSUBR(fpu.STV(0), 8);
                } else if (sub == 6) {
                    fpu.FDIV(fpu.STV(0), 8);
                } else /* sub == 7 */ {
                    fpu.FDIVR(fpu.STV(0), 8);
                }
            } else if (op == 0xDB && sub == 0) {
                // FILD m32int — load signed 32-bit, push as f64
                S32 v = (S32)memory->readd(m.effAddr);
                fpu.PREP_PUSH();
                fpu.FLD_I32(v, fpu.STV(0));
            } else if (op == 0xDB && (sub == 2 || sub == 3)) {
                // FIST m32int (sub=2) / FISTP m32int (sub=3) — round per CW
                S32 v;
                if (fpu.isRegCached[fpu.top]) {
                    v = (S32)fpu.FROUND(fpu.regCache[fpu.top].d);
                } else {
                    union { U64 u; double d; } u; u.d = fpu.getF64(fpu.top);
                    v = (S32)fpu.FROUND(u.d);
                }
                memory->writed(m.effAddr, (U32)v);
                if (sub == 3) fpu.FPOP();
            } else if (op == 0xDB && sub == 7) {
                // FSTP m80fp — store TOS as 80-bit extended precision, pop.
                // glibc's __printf_fp promotes double->long double here even
                // when the format spec is %f and spills the result via FSTP
                // m80 to generate high-precision digits. We MUST preserve all
                // 64 mantissa bits: route through FPU::ST80, which reads the
                // SoftFloat extFloat80_t register (FPU::getReg promotes a
                // cached double on the fly if useF64 ever left one there).
                // The old hand-rolled f64->f80 synthesis truncated to 52
                // mantissa bits and collapsed %.5f of pi to 0.00000.
                // 80-bit layout: 10 bytes LE, bytes 0..7 = explicit-bit
                // mantissa (signif), bytes 8..9 = sign(15)|biasedExp(14..0).
                U64 low; U64 high;
                fpu.ST80(fpu.top, &low, &high);
                memory->writeq(m.effAddr, low);
                memory->writew(m.effAddr + 8, (U16)high);
                fpu.FPOP();
            } else if (op == 0xDB && sub == 5) {
                // FLD m80fp — load 80-bit extended precision, push.
                // Inverse of FSTP m80; keep the full 80-bit value in the
                // SoftFloat register via FPU::LD80 instead of round-tripping
                // through double (which discarded the low mantissa bits).
                U64 low = memory->readq(m.effAddr);
                U16 high = (U16)memory->readw(m.effAddr + 8);
                fpu.PREP_PUSH();
                fpu.LD80(fpu.STV(0), low, high);
            } else if (op == 0xDF && sub == 0) {
                // FILD m16int — load signed 16-bit, push as f64
                S16 v = (S16)memory->readw(m.effAddr);
                fpu.PREP_PUSH();
                fpu.FLD_I16(v, fpu.STV(0));
            } else if (op == 0xDF && (sub == 2 || sub == 3)) {
                // FIST m16int (sub=2) / FISTP m16int (sub=3)
                S16 v;
                if (fpu.isRegCached[fpu.top]) {
                    v = (S16)fpu.FROUND(fpu.regCache[fpu.top].d);
                } else {
                    union { U64 u; double d; } u; u.d = fpu.getF64(fpu.top);
                    v = (S16)fpu.FROUND(u.d);
                }
                memory->writew(m.effAddr, (U16)v);
                if (sub == 3) fpu.FPOP();
            } else if (op == 0xDF && sub == 5) {
                // FILD m64int — load signed 64-bit, push as f64
                S64 v = (S64)memory->readq(m.effAddr);
                fpu.PREP_PUSH();
                fpu.FLD_I64(v, fpu.STV(0));
            } else if (op == 0xDF && sub == 7) {
                // FISTP m64int — round per CW, store as i64, pop
                S64 v;
                if (fpu.isRegCached[fpu.top]) {
                    v = (S64)fpu.FROUND(fpu.regCache[fpu.top].d);
                } else {
                    union { U64 u; double d; } u; u.d = fpu.getF64(fpu.top);
                    v = (S64)fpu.FROUND(u.d);
                }
                memory->writeq(m.effAddr, (U64)v);
                fpu.FPOP();
            } else if ((op == 0xD8 || op == 0xDC) && (sub == 2 || sub == 3)) {
                // FCOM/FCOMP m32fp or m64fp — compare ST(0) to memory operand,
                // set C0/C2/C3. We inline because the FPU::FCOM helper takes
                // CPU*. Then pop if sub==3.
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d
                                                     : fpu.getF64(fpu.top);
                double b;
                if (op == 0xD8) {
                    union { U32 i; float f; } u; u.i = memory->readd(m.effAddr);
                    b = (double)u.f;
                } else {
                    union { U64 i; double d; } u; u.i = memory->readq(m.effAddr);
                    b = u.d;
                }
                // FCOM result: C3=1/C2=1/C0=1 for unordered (NaN), C3=1/C2=0/C0=0
                // for equal, C3=0/C2=0/C0=1 for ST(0)<src, C3=0/C2=0/C0=0 for >.
                if (std::isnan(a) || std::isnan(b)) {
                    cpu64_fpu_set_c012_3(fpu, 1, 1, 1);
                } else if (a == b) {
                    cpu64_fpu_set_c012_3(fpu, 1, 0, 0);
                } else if (a < b) {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 1);
                } else {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 0);
                }
                if (sub == 3) fpu.FPOP();
            } else {
                goto unhandled;
            }
            rip += used;
            return used;
        } else {
            // register form (mod==11)
            U8 i = modrmByte & 7; // ST(i) selector
            if (op == 0xD9 && (modrmByte & 0xF8) == 0xC0) {
                // D9 C0+i = FLD ST(i)
                U32 src = fpu.STV(i);
                fpu.PREP_PUSH();
                U32 dst = fpu.STV(0);
                // duplicate src into dst. F64 fast path stores bits + cache flag.
                fpu.regCache[dst] = fpu.regCache[src];
                fpu.regs[dst] = fpu.regs[src];
                fpu.isRegCached[dst] = fpu.isRegCached[src];
                fpu.tags[dst] = TAG_Valid;
            } else if (op == 0xD9 && (modrmByte & 0xF8) == 0xC8) {
                // D9 C8+i = FXCH ST(0),ST(i)
                fpu.FXCH(fpu.STV(0), fpu.STV(i));
            } else if (op == 0xD9 && modrmByte == 0xE0) {
                // D9 E0 = FCHS — negate ST(0)
                fpu.FCHS();
            } else if (op == 0xD9 && modrmByte == 0xE1) {
                // D9 E1 = FABS — absolute value of ST(0)
                fpu.FABS();
            } else if (op == 0xD9 && modrmByte == 0xE8) {
                // D9 E8 = FLD1 — push +1.0
                fpu.PREP_PUSH();
                fpu.FLD1();
            } else if (op == 0xD9 && modrmByte == 0xEE) {
                // D9 EE = FLDZ — push +0.0
                fpu.PREP_PUSH();
                fpu.FLDZ();
            } else if (op == 0xDF && modrmByte == 0xE0) {
                // DF E0 = FNSTSW AX — write status word into AX, preserving
                // the upper 48 bits of RAX (per AMD64 manual).
                reg[X64_RAX].setU16((U16)fpu.SW());
            } else if (op == 0xDD && (modrmByte & 0xF8) == 0xC0) {
                // DD C0+i = FFREE ST(i) — mark ST(i) as empty (TAG_Empty).
                fpu.tags[fpu.STV(i)] = TAG_Empty;
                fpu.isRegCached[fpu.STV(i)] = false;
            } else if (op == 0xDD && (modrmByte & 0xF8) == 0xD0) {
                // DD D0+i = FST ST(i) — copy ST(0) into ST(i), no pop.
                U32 src = fpu.STV(0);
                U32 dst = fpu.STV(i);
                fpu.regCache[dst] = fpu.regCache[src];
                fpu.regs[dst] = fpu.regs[src];
                fpu.isRegCached[dst] = fpu.isRegCached[src];
                fpu.tags[dst] = TAG_Valid;
            } else if (op == 0xDD && (modrmByte & 0xF8) == 0xD8) {
                // DD D8+i = FSTP ST(i) — copy ST(0) into ST(i), then pop.
                U32 src = fpu.STV(0);
                U32 dst = fpu.STV(i);
                fpu.regCache[dst] = fpu.regCache[src];
                fpu.regs[dst] = fpu.regs[src];
                fpu.isRegCached[dst] = fpu.isRegCached[src];
                fpu.tags[dst] = TAG_Valid;
                fpu.FPOP();
            } else if (op == 0xDD && ((modrmByte & 0xF8) == 0xE0 || (modrmByte & 0xF8) == 0xE8)) {
                // DD E0+i = FUCOM ST(i)  (sub-form E0)
                // DD E8+i = FUCOMP ST(i) (sub-form E8, pops afterwards)
                // Set C3/C2/C0 per the standard FCOM result; treat QNaN as
                // unordered (we already do the same for FCOM).
                U32 si = fpu.STV(i);
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d : fpu.getF64(fpu.top);
                double b = fpu.isRegCached[si]      ? fpu.regCache[si].d      : fpu.getF64(si);
                if (std::isnan(a) || std::isnan(b)) {
                    cpu64_fpu_set_c012_3(fpu, 1, 1, 1);
                } else if (a == b) {
                    cpu64_fpu_set_c012_3(fpu, 1, 0, 0);
                } else if (a < b) {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 1);
                } else {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 0);
                }
                if ((modrmByte & 0xF8) == 0xE8) fpu.FPOP();
            } else if (op == 0xD8 && (modrmByte & 0xF8) == 0xD0) {
                // D8 D0+i = FCOM ST(0), ST(i) — set C0/C2/C3
                U32 si = fpu.STV(i);
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d : fpu.getF64(fpu.top);
                double b = fpu.isRegCached[si]      ? fpu.regCache[si].d      : fpu.getF64(si);
                if (std::isnan(a) || std::isnan(b)) {
                    cpu64_fpu_set_c012_3(fpu, 1, 1, 1);
                } else if (a == b) {
                    cpu64_fpu_set_c012_3(fpu, 1, 0, 0);
                } else if (a < b) {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 1);
                } else {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 0);
                }
            } else if (op == 0xD8 && (modrmByte & 0xF8) == 0xD8) {
                // D8 D8+i = FCOMP ST(0), ST(i) — like FCOM, then pop
                U32 si = fpu.STV(i);
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d : fpu.getF64(fpu.top);
                double b = fpu.isRegCached[si]      ? fpu.regCache[si].d      : fpu.getF64(si);
                if (std::isnan(a) || std::isnan(b)) {
                    cpu64_fpu_set_c012_3(fpu, 1, 1, 1);
                } else if (a == b) {
                    cpu64_fpu_set_c012_3(fpu, 1, 0, 0);
                } else if (a < b) {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 1);
                } else {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 0);
                }
                fpu.FPOP();
            } else if (op == 0xDE && (modrmByte & 0xC0) == 0xC0 && modrmByte != 0xD9) {
                // DE C0+i / C8+i / E0+i / E8+i / F0+i / F8+i — operate on
                // ST(i),ST(0), then pop. (DE D9 = FCOMPP is below.) The
                // shared FPU helpers expect (destSlot, srcSlot), so we
                // pass STV(i), STV(0).
                U32 si = fpu.STV(i);
                U32 s0 = fpu.STV(0);
                U8 grp = modrmByte & 0xF8;
                if (grp == 0xC0) {
                    fpu.FADD(si, s0);
                } else if (grp == 0xC8) {
                    fpu.FMUL(si, s0);
                } else if (grp == 0xE0) {
                    // FSUBRP: ST(i) = ST(0) - ST(i), pop
                    fpu.FSUBR(si, s0);
                } else if (grp == 0xE8) {
                    // FSUBP: ST(i) = ST(i) - ST(0), pop
                    fpu.FSUB(si, s0);
                } else if (grp == 0xF0) {
                    // FDIVRP: ST(i) = ST(0) / ST(i), pop
                    fpu.FDIVR(si, s0);
                } else if (grp == 0xF8) {
                    // FDIVP: ST(i) = ST(i) / ST(0), pop
                    fpu.FDIV(si, s0);
                } else {
                    goto unhandled;
                }
                fpu.FPOP();
            } else if (op == 0xDE && modrmByte == 0xD9) {
                // DE D9 = FCOMPP — compare ST(0) to ST(1), pop twice
                U32 s1 = fpu.STV(1);
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d : fpu.getF64(fpu.top);
                double b = fpu.isRegCached[s1]      ? fpu.regCache[s1].d      : fpu.getF64(s1);
                if (std::isnan(a) || std::isnan(b)) {
                    cpu64_fpu_set_c012_3(fpu, 1, 1, 1);
                } else if (a == b) {
                    cpu64_fpu_set_c012_3(fpu, 1, 0, 0);
                } else if (a < b) {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 1);
                } else {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 0);
                }
                fpu.FPOP();
                fpu.FPOP();
            } else if ((op == 0xDB || op == 0xDA) && ((modrmByte & 0xF8) == 0xF0 || (modrmByte & 0xF8) == 0xE8)) {
                // DB F0+i = FCOMI ST(0), ST(i) — set ZF/PF/CF in rflags.
                // DA F0+i = FUCOMI — same impl (we don't distinguish QNaN signaling).
                // DB E8+i = FUCOMI ST(0), ST(i). DA E8+i is reserved/invalid but
                // we accept it for symmetry; we don't distinguish signaling vs
                // quiet NaN here.
                // SDM table: unordered -> ZF=PF=CF=1; equal -> ZF=1, PF=CF=0;
                // ST(0)<ST(i) -> CF=1; ST(0)>ST(i) -> all zero. Other flags
                // (OF/SF/AF) are cleared per spec.
                U32 si = fpu.STV(i);
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d : fpu.getF64(fpu.top);
                double b = fpu.isRegCached[si]      ? fpu.regCache[si].d      : fpu.getF64(si);
                rflags &= ~(X64_CF | X64_PF | X64_ZF | X64_SF | X64_OF | X64_AF);
                if (std::isnan(a) || std::isnan(b)) {
                    rflags |= (X64_ZF | X64_PF | X64_CF);
                } else if (a == b) {
                    rflags |= X64_ZF;
                } else if (a < b) {
                    rflags |= X64_CF;
                }
                // a > b: no flags set
            } else if (op == 0xDF && (modrmByte & 0xF8) == 0xF0) {
                // DF F0+i = FCOMIP ST(0), ST(i) — same as FCOMI, then pop.
                U32 si = fpu.STV(i);
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d : fpu.getF64(fpu.top);
                double b = fpu.isRegCached[si]      ? fpu.regCache[si].d      : fpu.getF64(si);
                rflags &= ~(X64_CF | X64_PF | X64_ZF | X64_SF | X64_OF | X64_AF);
                if (std::isnan(a) || std::isnan(b)) {
                    rflags |= (X64_ZF | X64_PF | X64_CF);
                } else if (a == b) {
                    rflags |= X64_ZF;
                } else if (a < b) {
                    rflags |= X64_CF;
                }
                fpu.FPOP();
            } else if (op == 0xDA && modrmByte == 0xE9) {
                // DA E9 = FUCOMPP — compare ST(0) to ST(1), pop twice (same as FCOMPP for our purposes)
                U32 s1 = fpu.STV(1);
                double a = fpu.isRegCached[fpu.top] ? fpu.regCache[fpu.top].d : fpu.getF64(fpu.top);
                double b = fpu.isRegCached[s1]      ? fpu.regCache[s1].d      : fpu.getF64(s1);
                if (std::isnan(a) || std::isnan(b)) {
                    cpu64_fpu_set_c012_3(fpu, 1, 1, 1);
                } else if (a == b) {
                    cpu64_fpu_set_c012_3(fpu, 1, 0, 0);
                } else if (a < b) {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 1);
                } else {
                    cpu64_fpu_set_c012_3(fpu, 0, 0, 0);
                }
                fpu.FPOP();
                fpu.FPOP();
            } else if (op == 0xDB && modrmByte == 0xE2) {
                // DB E2 = FNCLEX — clear the x87 exception flags. We don't model
                // FPU exception bits, so this just clears the relevant status
                // bits and is otherwise a no-op. winex11/win32u issues it before
                // FLDCW when setting up its rounding mode.
                fpu.SetSW(fpu.SW() & ~0x80ff); // clear ES + the 6 exception bits + SF
            } else if (op == 0xDB && modrmByte == 0xE3) {
                // DB E3 = FNINIT — reinitialize the x87 FPU to its default state.
                fpu.FINIT();
            } else if (op == 0xDB && (modrmByte == 0xE0 || modrmByte == 0xE1)) {
                // DB E0 = FNENI, DB E1 = FNDISI — 8087 enable/disable interrupt;
                // no-ops on all CPUs since the 80287. Accept and ignore.
            } else if (op == 0xD9 && modrmByte == 0xD0) {
                // D9 D0 = FNOP — explicit x87 no-op.
            } else {
                goto unhandled;
            }
            rip += used;
            return used;
        }
    }

unhandled:
    // Unimplemented. Print enough leading bytes to identify the opcode
    // in the Intel SDM tables and bail out so we don't silently loop.
    //
    // The process is named here rather than only under BW64_UNIMPLDUMP: a
    // session runs a dozen helpers on this interpreter at once, and a report
    // that says only RIP leaves the reader inferring which one hit it from
    // whichever pid happened to log a line just above. That inference was
    // wrong once already.
    klog_fmt("CPU64: unimpl opcode at RIP=0x%llx bytes=%02x %02x %02x %02x %02x %02x %02x (rex=0x%02x osz=%d asz=%d seg=%02x rep=%02x) pid=%d exe='%s' cmd='%s'",
             (unsigned long long)ipStart,
             fetchByte(ipStart),
             fetchByte(ipStart + 1),
             fetchByte(ipStart + 2),
             fetchByte(ipStart + 3),
             fetchByte(ipStart + 4),
             fetchByte(ipStart + 5),
             fetchByte(ipStart + 6),
             p.rex, (int)p.osize16, (int)p.asize32, p.seg, p.rep,
             (thread && thread->process) ? (int)thread->process->id : -1,
             (thread && thread->process) ? thread->process->exe.c_str() : "",
             (thread && thread->process) ? thread->process->commandLine.c_str() : "");
    // BW64_UNIMPLDUMP: when a thread decodes a bogus opcode it has almost always
    // jumped to a garbage address (corruption / bad control transfer). Dump the
    // pid + GPRs + the top of the stack so we can see WHO called into here and
    // whether RIP/RSP are sane. Env-gated; off by default.
    if (std::getenv("BW64_UNIMPLDUMP")) {
        int pid = (thread && thread->process) ? (int)thread->process->id : -1;
        U64 rsp = reg[X64_RSP].u64;
        klog_fmt("  UNIMPLDUMP pid=%d rip=0x%llx rsp=0x%llx rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx rsi=0x%llx rdi=0x%llx rbp=0x%llx",
                 pid, (unsigned long long)ipStart, (unsigned long long)rsp,
                 (unsigned long long)reg[X64_RAX].u64, (unsigned long long)reg[X64_RBX].u64,
                 (unsigned long long)reg[X64_RCX].u64, (unsigned long long)reg[X64_RDX].u64,
                 (unsigned long long)reg[X64_RSI].u64, (unsigned long long)reg[X64_RDI].u64,
                 (unsigned long long)reg[X64_RBP].u64);
        if (thread && thread->process) {
            klog_fmt("  UNIMPLDUMP exe='%s'", thread->process->exe.c_str());
        }
        // Top 8 stack qwords — the most recent one is usually the return address
        // of whoever CALLed into the bogus RIP.
        for (int i = 0; i < 8; i++) {
            U64 v = 0;
            if (memory) v = memory->readq(rsp + (U64)i * 8);
            klog_fmt("  UNIMPLDUMP   [rsp+0x%02x] = 0x%llx", i * 8, (unsigned long long)v);
        }
        // 32 bytes at RIP, and at the suspicious pointer registers, so we can see
        // whether RIP lands in real module code or a pattern-filled buffer.
        auto dumpMem = [&](const char* tag, U64 a) {
            if (!memory) return;
            U8 b[32];
            for (int i = 0; i < 32; i++) b[i] = memory->readb(a + (U64)i);
            klog_fmt("  UNIMPLDUMP %s @0x%llx: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x",
                     tag, (unsigned long long)a,
                     b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7], b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15],
                     b[16],b[17],b[18],b[19],b[20],b[21],b[22],b[23], b[24],b[25],b[26],b[27],b[28],b[29],b[30],b[31]);
        };
        dumpMem("RIP ", ipStart);
        dumpMem("RIP-", ipStart - 16);
        dumpMem("RBX ", reg[X64_RBX].u64);
        dumpMem("RBP ", reg[X64_RBP].u64);
    }
    // An opcode this interpreter does not implement is, to the guest, an
    // illegal instruction, and the guest has to be told so.
    //
    // What used to happen instead was nothing: `yield = true` breaks the
    // dispatch loop in normalPlatformMultiThreaded.cpp, the platform thread
    // returns, and the process stays alive with no exit status, no lifecycle
    // marker and nobody to wake a parent blocked in waitpid. A device log
    // therefore ended with the opcode line and no BOXEDWINE_X64_PROC_EXIT for
    // that pid at all, which reads as "the helper vanished" rather than "the
    // helper executed an instruction we do not have".
    //
    // rip is put back at the faulting instruction first: several of the ten
    // paths that reach this label decode part of an instruction before giving
    // up, and the ucontext RIP a handler reads has to be the fault site.
    rip = ipStart;
    if (this->raiseSyncFault(K_SIGILL, /*trapNo #UD*/6, K_ILL_ILLOPN, ipStart)) {
        return 0; // rip now at the handler
    }
    // No handler: the kernel's default action for SIGILL is to terminate the
    // process, and a waiter has to see that it died of a signal rather than
    // exiting. The low 7 bits of the wait status are the terminating signal
    // (KSystem's waitpid builds it from `signaled`), which is what makes
    // WIFSIGNALED true and WTERMSIG report SIGILL in the parent.
    if (thread && thread->process) {
        thread->process->signaled = K_SIGILL;
    }
    // 128 + SIGILL, the shell's convention for a signal death, so a status
    // printed anywhere still names the signal. kfatalProcessExit64 emits the
    // same two markers a guest exit_group does, so this ending reads the same
    // way to every consumer of the log.
    kfatalProcessExit64(this, 128 + K_SIGILL, "cpu64-illegal-instruction");
    yield = true;
    return 0;
}

void CPU64::run() {
    // Optional instruction tracer for bring-up debugging. Gated on env so it
    // never costs anything in normal runs. BOXEDWINE64_TRACE_FROM/TO bound the
    // instructionCount window; each traced step prints RIP + the leading
    // opcode bytes + the GPRs most relevant to pointer-corruption hunts.
    static const char* tf = std::getenv("BOXEDWINE64_TRACE_FROM");
    static const char* tt = std::getenv("BOXEDWINE64_TRACE_TO");
    U64 traceFrom = tf ? std::strtoull(tf, nullptr, 0) : (U64)-1;
    U64 traceTo   = tt ? std::strtoull(tt, nullptr, 0) : 0;
    bool tracing = (tf != nullptr);

    // BW64_WILDJUMP: arm a per-thread RIP-history ring that auto-dumps the
    // moment execution lands in low memory (a wild computed jump — e.g. the
    // freetype-load bug RIP=0x10xxx). The dump shows the last 32 (rip, opcode,
    // key GPRs) tuples so the *source* instruction that produced the bad target
    // (an indirect call/jmp/ret through a clobbered pointer) is visible. Cheap:
    // a 32-entry ring updated per step; the threshold check is one compare.
    static const bool wildJump = std::getenv("BW64_WILDJUMP") != nullptr;
    const U64 WILD_LO = 0x100000ull;   // anything below 1MB is never real code
    struct RipHist { U64 rip, rax, rbx, rcx, rdx, rsi, rdi, rsp, rbp; U8 b0,b1,b2,b3; };
    RipHist ring[32] = {};
    int ringPos = 0;
    bool wildFired = false;

    // backendHandoff, unlike yield, is not a request to stop this guest: it
    // asks this loop to return to the scheduler so a process that took the
    // translated role at its own execve is dispatched through FEX instead of
    // being interpreted for the rest of its life. See KProcess::execve.
    while (!yield && !backendHandoff) {
        if (tracing && instructionCount >= traceFrom && instructionCount <= traceTo) {
            U64 r = rip;
            klog_fmt("TRACE #%llu RIP=0x%llx %02x %02x %02x %02x  "
                     "rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rsp=%llx rbp=%llx "
                     "xmm0=%llx:%llx xmm1=%llx:%llx",
                     (unsigned long long)instructionCount, (unsigned long long)r,
                     fetchByte(r), fetchByte(r+1), fetchByte(r+2), fetchByte(r+3),
                     (unsigned long long)reg[X64_RAX].u64, (unsigned long long)reg[X64_RBX].u64,
                     (unsigned long long)reg[X64_RCX].u64, (unsigned long long)reg[X64_RDX].u64,
                     (unsigned long long)reg[X64_RSI].u64, (unsigned long long)reg[X64_RDI].u64,
                     (unsigned long long)reg[X64_RSP].u64, (unsigned long long)reg[X64_RBP].u64,
                     (unsigned long long)xmm[0].hi, (unsigned long long)xmm[0].lo,
                     (unsigned long long)xmm[1].hi, (unsigned long long)xmm[1].lo);
        }
        if (wildJump) {
            RipHist& h = ring[ringPos];
            h.rip = rip;
            h.rax = reg[X64_RAX].u64; h.rbx = reg[X64_RBX].u64;
            h.rcx = reg[X64_RCX].u64; h.rdx = reg[X64_RDX].u64;
            h.rsi = reg[X64_RSI].u64; h.rdi = reg[X64_RDI].u64;
            h.rsp = reg[X64_RSP].u64; h.rbp = reg[X64_RBP].u64;
            h.b0 = fetchByte(rip);   h.b1 = fetchByte(rip+1);
            h.b2 = fetchByte(rip+2); h.b3 = fetchByte(rip+3);
            ringPos = (ringPos + 1) & 31;
            if (!wildFired && rip != 0 && rip < WILD_LO) {
                wildFired = true;
                klog_fmt("BW64_WILDJUMP: RIP=0x%llx entered low memory @ insn #%llu "
                         "— dumping last 32 instructions (oldest first):",
                         (unsigned long long)rip, (unsigned long long)instructionCount);
                for (int i = 0; i < 32; i++) {
                    RipHist& e = ring[(ringPos + i) & 31];
                    if (e.rip == 0 && e.b0 == 0 && e.rsp == 0) continue;
                    klog_fmt("  [%2d] RIP=0x%llx %02x %02x %02x %02x  "
                             "rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rsp=%llx rbp=%llx",
                             i, (unsigned long long)e.rip, e.b0, e.b1, e.b2, e.b3,
                             (unsigned long long)e.rax, (unsigned long long)e.rbx,
                             (unsigned long long)e.rcx, (unsigned long long)e.rdx,
                             (unsigned long long)e.rsi, (unsigned long long)e.rdi,
                             (unsigned long long)e.rsp, (unsigned long long)e.rbp);
                }
            }
        }
#ifdef BOXEDWINE_BLOCK_EXEC
        {
            U32 bn = tryBlockStep();
            if (bn) {
                instructionCount += bn;
                continue;
            }
        }
#endif
        U32 n = step();
        if (n == 0) break;
        instructionCount++;
        // A thread killed from outside (persistent-session app switch kills the
        // previous app) is only MARKED terminating — nothing interrupts a
        // CPU-bound guest loop that never blocks in a syscall (some engines spin
        // by design), so without this check run() never returns and the killed
        // process can never finish dying. One flag test every 64k instructions
        // is noise next to step()'s cost.
        //
        // SAME cadence, second job (M16, D3D Present fix): deliver any pending
        // cross-thread signal. In the MULTI-THREADED build each guest thread runs
        // its OWN cpu64->run() loop (normalPlatformMultiThreaded.cpp), which —
        // unlike the cooperative scheduler and the futex-wait path — never
        // delivered pendingSignals. A thread that BUSY-POLLS (non-blocking
        // recvmsg/read in a spin, never entering a futex wait) therefore never
        // received the wineserver's SIGUSR1 "APC nudge" carrying an async-I/O
        // completion. That was the D3D Present() hang: wined3d's render thread
        // spun on the wineserver socket for Present's completion whose APC nudge
        // sat undelivered in pendingSignals forever. Delivering here runs the
        // client's APC handler so Present can return. Cheap: only fires when
        // pendingSignals is set, gated to once per 64k instructions.
        if ((instructionCount & 0xFFFF) == 0) {
            if (thread && thread->terminating) {
                break;
            }
            if (thread && thread->pendingSignals) {
                deliverPendingSignals();
            }
        }
    }
}


#ifdef BOXEDWINE_BLOCK_EXEC
// ---- Phase 2 prototype implementation (see cpu64.h). Semantics of every
// record kind are copied from the interpreter bodies below — same helpers
// (loadRM/storeRM/runAlu/doShift/evalCC), same flag behavior.

bool CPU64::decodeModRMRecipe(U64 modrmAddr, const Prefixes& p, U32 trailingImmBytes, BRecipe& out) {
    U8 modrm = fetchByte(modrmAddr);
    U8 mod = (modrm >> 6) & 0x3;
    U8 regF = (modrm >> 3) & 0x7;
    U8 rm  =  modrm        & 0x7;
    out.regField = (U8)(regF | ((p.rex & 0x04) ? 0x08 : 0));
    out.length = 1;
    out.seg = p.seg;
    out.asize32 = p.asize32;
    if (mod == 0x3) {
        out.isReg = true;
        out.rmIndex = (U8)(rm | ((p.rex & 0x01) ? 0x08 : 0));
        return true;
    }
    if (mod == 0x0 && rm == 0x5) {
        S32 disp32 = (S32)fetchDword(modrmAddr + 1);
        out.length = 5;
        out.ripRel = true;
        out.ripTarget = modrmAddr + out.length + trailingImmBytes + (U64)(S64)disp32;
        return true;
    }
    bool haveBase = true;
    if (rm == 0x4) {
        U8 sib = fetchByte(modrmAddr + 1);
        out.length = 2;
        U8 scale = (sib >> 6) & 0x3;
        U8 idxF  = (sib >> 3) & 0x7;
        U8 baseF =  sib        & 0x7;
        if (!(idxF == 0x4 && !(p.rex & 0x02))) {
            out.idxIdx = (U8)(idxF | ((p.rex & 0x02) ? 0x08 : 0));
            out.scale = scale;
        }
        if (baseF == 0x5 && mod == 0x0) {
            haveBase = false;
        } else {
            out.baseIdx = (U8)(baseF | ((p.rex & 0x01) ? 0x08 : 0));
        }
    } else {
        out.baseIdx = (U8)(rm | ((p.rex & 0x01) ? 0x08 : 0));
    }
    if (mod == 0x1) {
        out.disp = (S32)(S8)fetchByte(modrmAddr + out.length);
        out.length += 1;
    } else if (mod == 0x2 || (mod == 0x0 && rm == 0x4 && !haveBase)) {
        out.disp = (S32)fetchDword(modrmAddr + out.length);
        out.length += 4;
    }
    return true;
}

__attribute__((always_inline)) inline CPU64::ModRM CPU64::resolveRecipe(const BRecipe& r) {
    ModRM m;
    m.regField = r.regField;
    m.length = r.length;
    if (r.isReg) {
        m.isReg = true;
        m.rmIndex = r.rmIndex;
        return m;
    }
    if (r.ripRel) {
        m.isRipRel = true;
        m.effAddr = r.ripTarget;
        return m;
    }
    U64 ea = (U64)(S64)r.disp;
    if (r.baseIdx != 0xFF) ea += reg[r.baseIdx].u64;
    if (r.idxIdx != 0xFF) ea += reg[r.idxIdx].u64 << r.scale;
    if (r.asize32) ea &= 0xFFFFFFFFULL;
    if (r.seg == 0x64) ea += fsbase;
    else if (r.seg == 0x65) ea += gsbase;
    m.effAddr = ea;
    return m;
}

bool CPU64::buildBlock(U64 startRip, BBlock& b) {
    b.startRip = 0;
    b.n = 0;
    U64 r = startRip;
    while (b.n < BBLOCK_MAX_RECS) {
        Prefixes p;
        U64 savedRip = rip;
        rip = r;                      // consumePrefixes/fetch read via rip-relative helpers? they take absolute addrs; safe
        U32 opOff;
        {
            // consumePrefixes reads at this->rip
            opOff = consumePrefixes(p);
        }
        rip = savedRip;
        if (p.lock || p.rep || p.osize16) break;   // unsupported in v1
        U8 op = fetchByte(r + opOff);
        bool rexW = (p.rex & 0x08) != 0;
        U8 size = rexW ? 8 : 4;
        BRec& rec = b.recs[b.n];
        rec = BRec();
        rec.size = size;
        rec.rexPresent = (p.rex != 0) ? 1 : 0;

        if (op == 0x89 || op == 0x8B) {
            if (!decodeModRMRecipe(r + opOff + 1, p, 0, rec.mr)) break;
            rec.kind = (op == 0x89) ? BK_MOV_RM_R : BK_MOV_R_RM;
            rec.len = (U8)(opOff + 1 + rec.mr.length);
        } else if (op <= 0x3D && ((op & 0x06) != 0x06) && (op & 0x7) <= 3) {
            U8 form = op & 0x7;
            if ((form & 1) == 0) break;            // 8-bit forms unsupported in v1
            if (!decodeModRMRecipe(r + opOff + 1, p, 0, rec.mr)) break;
            rec.kind = (form < 2) ? BK_ALU_RM_R : BK_ALU_R_RM;
            rec.sub = (op >> 3) & 0x7;
            rec.len = (U8)(opOff + 1 + rec.mr.length);
        } else if (op == 0x81 || op == 0x83) {
            U32 trailing = (op == 0x81) ? 4u : 1u;
            if (!decodeModRMRecipe(r + opOff + 1, p, trailing, rec.mr)) break;
            rec.kind = BK_ALU_RM_IMM;
            rec.sub = rec.mr.regField & 0x7;
            U64 immAddr = r + opOff + 1 + rec.mr.length;
            U32 immLen;
            if (op == 0x83) {
                S8 i8 = (S8)fetchByte(immAddr);
                rec.imm = (U64)(S64)i8;
                if (size == 4) rec.imm &= 0xFFFFFFFFULL;
                immLen = 1;
            } else if (size == 4) {
                rec.imm = fetchDword(immAddr);
                immLen = 4;
            } else {
                S32 i32 = (S32)fetchDword(immAddr);
                rec.imm = (U64)(S64)i32;
                immLen = 4;
            }
            rec.len = (U8)(opOff + 1 + rec.mr.length + immLen);
        } else if (op == 0xC1) {
            if (!decodeModRMRecipe(r + opOff + 1, p, 1, rec.mr)) break;
            rec.kind = BK_SHIFT_IMM;
            rec.sub = rec.mr.regField & 0x7;
            U8 count = fetchByte(r + opOff + 1 + rec.mr.length);
            rec.imm = count & ((size == 8) ? 0x3F : 0x1F);
            rec.len = (U8)(opOff + 1 + rec.mr.length + 1);
        } else if (op == 0x0F && fetchByte(r + opOff + 1) == 0xAF) {
            if (!decodeModRMRecipe(r + opOff + 2, p, 0, rec.mr)) break;
            rec.kind = BK_IMUL_R_RM;
            rec.len = (U8)(opOff + 2 + rec.mr.length);
        } else if (op == 0x85) {
            if (!decodeModRMRecipe(r + opOff + 1, p, 0, rec.mr)) break;
            rec.kind = BK_TEST_RM_R;
            rec.len = (U8)(opOff + 1 + rec.mr.length);
        } else if (op == 0x8D) {
            if (!decodeModRMRecipe(r + opOff + 1, p, 0, rec.mr)) break;
            if (rec.mr.isReg) break;               // LEA reg-form undefined
            rec.kind = BK_LEA;
            rec.len = (U8)(opOff + 1 + rec.mr.length);
        } else if (op == 0xC7) {
            if (!decodeModRMRecipe(r + opOff + 1, p, 4, rec.mr)) break;
            if ((rec.mr.regField & 0x7) != 0) break;
            rec.kind = BK_MOV_RM_IMM;
            S32 imm32 = (S32)fetchDword(r + opOff + 1 + rec.mr.length);
            rec.imm = (size == 8) ? (U64)(S64)imm32 : (U64)(U32)imm32;
            rec.len = (U8)(opOff + 1 + rec.mr.length + 4);
        } else if (op >= 0xB8 && op <= 0xBF) {
            rec.kind = BK_MOV_R_IMM;
            rec.sub = (U8)((op - 0xB8) | ((p.rex & 0x01) ? 0x08 : 0));
            if (rexW) {
                rec.imm = fetchQword(r + opOff + 1);
                rec.len = (U8)(opOff + 1 + 8);
            } else {
                rec.imm = fetchDword(r + opOff + 1);
                rec.len = (U8)(opOff + 1 + 4);
            }
        } else if (op >= 0x50 && op <= 0x57) {
            rec.kind = BK_PUSH;
            rec.sub = (U8)((op - 0x50) | ((p.rex & 0x01) ? 0x08 : 0));
            rec.len = (U8)(opOff + 1);
        } else if (op >= 0x58 && op <= 0x5F) {
            rec.kind = BK_POP;
            rec.sub = (U8)((op - 0x58) | ((p.rex & 0x01) ? 0x08 : 0));
            rec.len = (U8)(opOff + 1);
        } else if (op == 0x0F && fetchByte(r + opOff + 1) >= 0x80 && fetchByte(r + opOff + 1) <= 0x8F) {
            rec.kind = BK_JCC8;                    // same executor as rel8
            rec.sub = fetchByte(r + opOff + 1) & 0xF;
            rec.jccDelta = (S32)fetchDword(r + opOff + 2);
            rec.len = (U8)(opOff + 2 + 4);
            b.n++;
            r += rec.len;
            break;                                  // terminator
        } else if (op >= 0x70 && op <= 0x7F) {
            rec.kind = BK_JCC8;
            rec.sub = op & 0xF;
            rec.jccDelta = (S32)(S8)fetchByte(r + opOff + 1);
            rec.len = (U8)(opOff + 2);
            b.n++;
            r += rec.len;
            break;                                  // terminator
        } else {
            break;                                  // unsupported → end block here
        }
        b.n++;
        r += rec.len;
    }
    if (b.n < 2) {
        return false;                               // not worth a block
    }
    // Register + snapshot page generations (register FIRST so no write can
    // slip between the gen read and future executions unnoticed).
    b.page0 = startRip >> 12;
    b.page1 = (r - 1) >> 12;
    KMemory64::blockPageRegister(b.page0);
    if (b.page1 != b.page0) KMemory64::blockPageRegister(b.page1);
    b.gen0 = KMemory64::blockPageGenOf(b.page0);
    b.gen1 = KMemory64::blockPageGenOf(b.page1);
    b.startRip = startRip;
    blockStatsBuilt++;
    return true;
}

U32 CPU64::execBlock(const BBlock& b) {
    U32 executed = 0;
    for (U16 i = 0; i < b.n; i++) {
        const BRec& rec = b.recs[i];
        bool rexPresent = rec.rexPresent != 0;
        U32 size = rec.size;
        switch (rec.kind) {
        case BK_MOV_RM_R: {
            ModRM m = resolveRecipe(rec.mr);
            U64 src = (size == 4) ? (U64)reg[m.regField].u32 : reg[m.regField].u64;
            storeRM(m, size, src, rexPresent);
            rip += rec.len;
            break;
        }
        case BK_MOV_R_RM: {
            ModRM m = resolveRecipe(rec.mr);
            U64 val = loadRM(m, size, rexPresent);
            if (size == 4) reg[m.regField].setU32((U32)val);
            else reg[m.regField].setU64(val);
            rip += rec.len;
            break;
        }
        case BK_ALU_RM_R: {
            ModRM m = resolveRecipe(rec.mr);
            U64 a = loadRM(m, size, rexPresent);
            U64 bv = (size == 4) ? (U64)reg[m.regField].u32 : reg[m.regField].u64;
            runAlu(rec.sub, size, true, a, bv, m, rexPresent);
            rip += rec.len;
            break;
        }
        case BK_ALU_R_RM: {
            ModRM m = resolveRecipe(rec.mr);
            U64 a = (size == 4) ? (U64)reg[m.regField].u32 : reg[m.regField].u64;
            U64 bv = loadRM(m, size, rexPresent);
            runAlu(rec.sub, size, false, a, bv, m, rexPresent);
            rip += rec.len;
            break;
        }
        case BK_ALU_RM_IMM: {
            ModRM m = resolveRecipe(rec.mr);
            U64 a = loadRM(m, size, rexPresent);
            runAlu(rec.sub, size, true, a, rec.imm, m, rexPresent);
            rip += rec.len;
            break;
        }
        case BK_SHIFT_IMM: {
            ModRM m = resolveRecipe(rec.mr);
            U8 count = (U8)rec.imm;
            U64 v = loadRM(m, size, rexPresent);
            U64 res = doShift(rflags, rec.sub, v, count, size);
            if (count != 0) storeRM(m, size, res, rexPresent);
            else if (size == 4 && m.isReg) reg[m.rmIndex].setU32((U32)v);
            rip += rec.len;
            break;
        }
        case BK_IMUL_R_RM: {
            ModRM m = resolveRecipe(rec.mr);
            U64 a = (size == 4) ? (U64)reg[m.regField].u32 : reg[m.regField].u64;
            U64 bv = loadRM(m, size, rexPresent);
            S64 sa, sb;
            if (size == 4) { sa = (S64)(S32)a; sb = (S64)(S32)bv; }
            else { sa = (S64)a; sb = (S64)bv; }
            S64 res = sa * sb;
            bool overflow;
            if (size == 4) overflow = (res != (S64)(S32)res);
            else { __int128 r128 = (__int128)sa * (__int128)sb; overflow = (r128 != (__int128)res); }
            rflags &= ~(X64_CF | X64_OF);
            if (overflow) rflags |= X64_CF | X64_OF;
            if (size == 4) reg[m.regField].setU32((U32)res);
            else reg[m.regField].setU64(res);
            rip += rec.len;
            break;
        }
        case BK_TEST_RM_R: {
            ModRM m = resolveRecipe(rec.mr);
            U64 a = loadRM(m, size, rexPresent);
            U64 bv = (size == 4) ? (U64)reg[m.regField].u32 : reg[m.regField].u64;
            flagsLogic(rflags, a & bv, size);
            rip += rec.len;
            break;
        }
        case BK_LEA: {
            ModRM m = resolveRecipe(rec.mr);
            if (size == 4) reg[m.regField].setU32((U32)m.effAddr);
            else reg[m.regField].setU64(m.effAddr);
            rip += rec.len;
            break;
        }
        case BK_MOV_RM_IMM: {
            ModRM m = resolveRecipe(rec.mr);
            storeRM(m, size, rec.imm, rexPresent);
            rip += rec.len;
            break;
        }
        case BK_MOV_R_IMM: {
            if (size == 4) reg[rec.sub].setU32((U32)rec.imm);
            else reg[rec.sub].setU64(rec.imm);
            rip += rec.len;
            break;
        }
        case BK_PUSH: {
            push64(reg[rec.sub].u64);
            rip += rec.len;
            break;
        }
        case BK_POP: {
            reg[rec.sub].setU64(pop64());
            rip += rec.len;
            break;
        }
        case BK_JCC8: {
            rip += rec.len;
            if (evalCC(rec.sub)) rip += (U64)(S64)rec.jccDelta;
            executed++;
            return executed;
        }
        }
        executed++;
    }
    return executed;
}

U32 CPU64::tryBlockStep() {
    U64 r = rip;
    U32 neg = (U32)((r ^ (r >> 10)) & 1023);
    if (blockNegCache[neg] == r) return 0;
    if (!blockTable) {
        blockTable = std::make_unique<BBlock[]>(BBLOCK_SLOTS);
        if (std::getenv("BW64_BLOCKSTATS")) {
            static CPU64* statsCpu = this;
            std::atexit([] {
                klog_fmt("BLOCKSTATS: built=%llu execs=%llu insnsInBlocks=%llu totalInsns=%llu coverage=%.1f%%",
                         (unsigned long long)statsCpu->blockStatsBuilt,
                         (unsigned long long)statsCpu->blockStatsExec,
                         (unsigned long long)statsCpu->blockStatsInsn,
                         (unsigned long long)statsCpu->instructionCount,
                         statsCpu->instructionCount ? 100.0 * statsCpu->blockStatsInsn / statsCpu->instructionCount : 0.0);
            });
        }
    }
    BBlock& b = blockTable[(U32)((r ^ (r >> 9)) & (BBLOCK_SLOTS - 1))];
    if (b.startRip == r) {
        if (KMemory64::blockPageGenOf(b.page0) == b.gen0 &&
            KMemory64::blockPageGenOf(b.page1) == b.gen1) {
            U32 n = execBlock(b);
            blockStatsExec++;
            blockStatsInsn += n;
            return n;
        }
        b.startRip = 0; // stale — rebuild below
    }
    if (!buildBlock(r, b)) {
        blockNegCache[neg] = r;
        return 0;
    }
    U32 n = execBlock(b);
    blockStatsExec++;
    blockStatsInsn += n;
    return n;
}
#endif // BOXEDWINE_BLOCK_EXEC

U64 CPU64::runBounded(U64 maxInsn) {
    static const char* tf = std::getenv("BOXEDWINE64_TRACE_FROM");
    static const char* tt = std::getenv("BOXEDWINE64_TRACE_TO");
    U64 traceFrom = tf ? std::strtoull(tf, nullptr, 0) : (U64)-1;
    U64 traceTo   = tt ? std::strtoull(tt, nullptr, 0) : 0;
    bool tracing = (tf != nullptr);

    U64 ran = 0;
    while (!yield && !backendHandoff && ran < maxInsn) {
        if (tracing && instructionCount >= traceFrom && instructionCount <= traceTo) {
            U64 r = rip;
            klog_fmt("TRACE #%llu RIP=0x%llx %02x %02x %02x %02x  "
                     "rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rsp=%llx rbp=%llx "
                     "xmm0=%llx:%llx xmm1=%llx:%llx",
                     (unsigned long long)instructionCount, (unsigned long long)r,
                     fetchByte(r), fetchByte(r+1), fetchByte(r+2), fetchByte(r+3),
                     (unsigned long long)reg[X64_RAX].u64, (unsigned long long)reg[X64_RBX].u64,
                     (unsigned long long)reg[X64_RCX].u64, (unsigned long long)reg[X64_RDX].u64,
                     (unsigned long long)reg[X64_RSI].u64, (unsigned long long)reg[X64_RDI].u64,
                     (unsigned long long)reg[X64_RSP].u64, (unsigned long long)reg[X64_RBP].u64,
                     (unsigned long long)xmm[0].hi, (unsigned long long)xmm[0].lo,
                     (unsigned long long)xmm[1].hi, (unsigned long long)xmm[1].lo);
        }
#ifdef BOXEDWINE_BLOCK_EXEC
        {
            U32 bn = tryBlockStep();
            if (bn) {
                instructionCount += bn;
                ran += bn;
                continue;
            }
        }
#endif
        U32 n = step();
        if (n == 0) break;
        instructionCount++;
        ran++;
    }
    return ran;
}

#endif // BOXEDWINE_GUEST_X64
