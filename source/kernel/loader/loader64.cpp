/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"
#include <atomic>

#include "loader64.h"
#include "elfloaderpolicy64.h"
#include "fex64loaderhandoff.h"
#include "kelf64.h"
#include "krandom.h"
#include <algorithm>
#include <array>
#ifdef BOXEDWINE_GUEST_X64
#include "kmemory64.h"
#include "cpu64.h"
// The ranges ntdll reserves for itself when no preloader runs. The guest
// layout below has to keep out of them; named here rather than repeated.
#include "guest_low_alias.h"
#endif
// ELF p_type values reused from loader.cpp. Kept local here to avoid a
// cross-file include just for two constants.
#ifndef PT_LOAD
#define PT_LOAD 1
#endif
#ifndef PT_INTERP
#define PT_INTERP 3
#endif
#ifndef PT_PHDR
#define PT_PHDR 6
#endif
// GNU extensions — not in the SysV phdr set but every modern toolchain
// emits them. PT_GNU_RELRO marks the region the linker should mprotect
// to read-only after relocations complete (typically .got + .got.plt).
#define PT_GNU_RELRO 0x6474E552
// PT_GNU_STACK: a 0-sized phdr whose p_flags advertise stack permissions.
// p_flags & PF_X (=1) means "stack must be executable". Modern toolchains
// emit RW (no X); the binary opts-in only if it actually needs trampolines.
#define PT_GNU_STACK 0x6474E551
#define PF_X 0x1

// ET_EXEC=2, ET_DYN=3. PIE executables (and shared libs) are ET_DYN.
#define ET_EXEC 2
#define ET_DYN  3

// Maximum sensible PT_INTERP path. Real-world values are well under 256;
// the cap protects against a malformed/hostile binary.
#define INTERP_PATH_MAX 1024

Elf64ParseResult ElfLoader64::parseBuffer(const U8* data, U64 length) {
    Elf64ParseResult result;
    if (!data || length < sizeof(struct k_Elf64_Ehdr)) {
        klog_fmt("ElfLoader64::parseBuffer: buffer too short (length=%llu)",
                 (unsigned long long)length);
        return result;
    }

    struct k_Elf64_Ehdr ehdr;
    memcpy(&ehdr, data, sizeof(ehdr));

    if (ehdr.e_ident[0] != 0x7F ||
        ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L' ||
        ehdr.e_ident[3] != 'F' ||
        ehdr.e_ident[4] != k_ELFCLASS64) {
        klog("ElfLoader64::parseBuffer: not an ELF64 file");
        return result;
    }
    if (ehdr.e_machine != k_EM_X86_64) {
        klog_fmt("ElfLoader64::parseBuffer: unsupported e_machine 0x%x (only x86-64 is supported in v1)", ehdr.e_machine);
        return result;
    }
    if (ehdr.e_phentsize != sizeof(struct k_Elf64_Phdr)) {
        klog_fmt("ElfLoader64::parseBuffer: e_phentsize %u != sizeof(Elf64_Phdr) %u",
                 (U32)ehdr.e_phentsize, (U32)sizeof(struct k_Elf64_Phdr));
        return result;
    }
    if (ehdr.e_phnum == 0) {
        klog("ElfLoader64::parseBuffer: e_phnum == 0, nothing to load");
        return result;
    }
    // Bounds check: phdr table must fit inside the buffer.
    const U64 phdrBytes = (U64)ehdr.e_phnum * ehdr.e_phentsize;
    if (ehdr.e_phoff > length || phdrBytes > length - ehdr.e_phoff) {
        klog_fmt("ElfLoader64::parseBuffer: phdr table extends past buffer "
                 "(offset=%llu size=%llu length=%llu)",
                 (unsigned long long)ehdr.e_phoff,
                 (unsigned long long)phdrBytes,
                 (unsigned long long)length);
        return result;
    }

    result.entry = ehdr.e_entry;
    result.phoff = ehdr.e_phoff;
    result.phentsize = ehdr.e_phentsize;
    result.phnum = ehdr.e_phnum;
    result.isPie = (ehdr.e_type == ET_DYN);

    U64 lo = (U64)-1;
    U64 hi = 0;
    for (U16 i = 0; i < ehdr.e_phnum; i++) {
        struct k_Elf64_Phdr phdr;
        U64 phdrOff = ehdr.e_phoff + (U64)i * ehdr.e_phentsize;
        memcpy(&phdr, data + phdrOff, sizeof(phdr));

        if (phdr.p_type == PT_LOAD) {
            if (phdr.p_filesz > phdr.p_memsz ||
                phdr.p_offset > length ||
                phdr.p_filesz > length - phdr.p_offset ||
                phdr.p_vaddr > UINT64_MAX - phdr.p_memsz) {
                klog("ElfLoader64::parseBuffer: invalid PT_LOAD bounds");
                return result;
            }
            if (phdr.p_memsz == 0) continue;
            Elf64LoadSegment seg;
            seg.vaddr  = phdr.p_vaddr;
            seg.memsz  = phdr.p_memsz;
            seg.filesz = phdr.p_filesz;
            seg.offset = phdr.p_offset;
            seg.flags  = phdr.p_flags;
            seg.align  = phdr.p_align;
            result.segments.push_back(seg);
            if (phdr.p_vaddr < lo) lo = phdr.p_vaddr;
            const U64 segmentEnd = phdr.p_vaddr + phdr.p_memsz;
            if (segmentEnd > hi) hi = segmentEnd;
        } else if (phdr.p_type == PT_PHDR) {
            result.phdrVaddr = phdr.p_vaddr;
            result.phdrPresent = true;
        } else if (phdr.p_type == PT_INTERP) {
            if (phdr.p_filesz == 0 || phdr.p_filesz > INTERP_PATH_MAX) {
                klog_fmt("ElfLoader64::parseBuffer: PT_INTERP filesz %llu out of range",
                         (unsigned long long)phdr.p_filesz);
                return result;
            }
            if (phdr.p_offset > length ||
                phdr.p_filesz > length - phdr.p_offset) {
                klog("ElfLoader64::parseBuffer: PT_INTERP extends past buffer");
                return result;
            }
            char interp[INTERP_PATH_MAX + 1] = { 0 };
            memcpy(interp, data + phdr.p_offset, (size_t)phdr.p_filesz);
            interp[phdr.p_filesz] = 0;
            result.interpreter = BString::copy(interp);
        } else if (phdr.p_type == k_PT_DYNAMIC) {
            result.dynamic.present = true;
            result.dynamic.vaddr = phdr.p_vaddr;
            result.dynamic.memsz = phdr.p_memsz;
        } else if (phdr.p_type == k_PT_TLS) {
            result.tls.present = true;
            result.tls.vaddr  = phdr.p_vaddr;
            result.tls.filesz = phdr.p_filesz;
            result.tls.memsz  = phdr.p_memsz;
            result.tls.align  = phdr.p_align;
        } else if (phdr.p_type == PT_GNU_RELRO) {
            result.relro.present = true;
            result.relro.vaddr   = phdr.p_vaddr;
            result.relro.memsz   = phdr.p_memsz;
        } else if (phdr.p_type == PT_GNU_STACK) {
            result.gnuStackPresent = true;
            result.gnuStackExec = (phdr.p_flags & PF_X) != 0;
        }
    }

    if (result.segments.empty()) {
        klog("ElfLoader64::parseBuffer: no PT_LOAD segments");
        return result;
    }
    result.baseAddrLow = lo;
    result.baseAddrHigh = hi;
    if (!result.phdrPresent) {
        // PT_PHDR is optional. When absent, derive the in-memory address from
        // the PT_LOAD segment that contains the file's program-header table.
        // AT_PHDR is a virtual address, not baseAddrLow + e_phoff: the latter
        // is wrong whenever the first load segment has a non-zero p_offset or
        // a vaddr that does not equal its file offset.
        for (const Elf64LoadSegment& seg : result.segments) {
            if (ehdr.e_phoff >= seg.offset &&
                ehdr.e_phoff - seg.offset <= seg.filesz &&
                phdrBytes <= seg.filesz - (ehdr.e_phoff - seg.offset)) {
                result.phdrVaddr = seg.vaddr + (ehdr.e_phoff - seg.offset);
                result.phdrPresent = true;
                break;
            }
        }
    }
    result.ok = true;
    return result;
}

Elf64ParseResult ElfLoader64::parse(FsOpenNode* openNode) {
    Elf64ParseResult result;
    if (!openNode) return result;

    // Slurp the entire file. ELF binaries we care about are bounded (the
    // main exe + ld-linux are each well under 10 MB even uncompressed).
    S64 len = openNode->length();
    if (len <= 0) {
        klog("ElfLoader64::parse: file empty or length unknown");
        return result;
    }
    std::vector<U8> buffer((size_t)len);
    openNode->seek(0);
    U32 readTotal = 0;
    while (readTotal < (U32)len) {
        U32 chunk = openNode->readNative(buffer.data() + readTotal, (U32)len - readTotal);
        if (chunk == 0) break;
        readTotal += chunk;
    }
    if (readTotal != (U32)len) {
        klog_fmt("ElfLoader64::parse: short slurp (got %u of %lld)",
                 readTotal, (long long)len);
        return result;
    }
    return parseBuffer(buffer.data(), (U64)len);
}

#ifdef BOXEDWINE_GUEST_X64

// PIE relocation base for ET_DYN binaries when the loader hasn't picked
// one. Sits well below the x86-64 user-space cap (0x7FFFFFFFFFFF) and far
// above anything ELF32 ever touches, so the address is recognisable in
// logs.
#define X64_PIE_BASE 0x400000000ULL

#ifndef K_PROT_READ
#define K_PROT_READ  1
#define K_PROT_WRITE 2
#define K_PROT_EXEC  4
#endif

// p_flags bits: PF_X=1, PF_W=2, PF_R=4 (note: opposite of K_PROT_* ordering).
static U32 phdrFlagsToProt(U32 pFlags) {
    U32 prot = 0;
    if (pFlags & 0x4) prot |= K_PROT_READ;
    if (pFlags & 0x2) prot |= K_PROT_WRITE;
    if (pFlags & 0x1) prot |= K_PROT_EXEC;
    return prot;
}

enum class FEX64InterpreterNormalization {
    NotNeeded,
    Applied,
    Failed,
};

// The pinned loader's `_start` saves the return from `_dl_start` in R12 and
// eventually transfers to it with `jmp r12`. The native-identity FEX path has
// been observed returning AT_BASE there, which asks the decoder to execute the
// loader's ELF header. BoxedWine has already validated AT_ENTRY while mapping
// the main image, so replace only the exact pinned handoff tail with a
// relative branch to a nearby immediate-entry trampoline. The trampoline is
// placed in zero padding at the end of the loader's executable host page; no
// ELF segment contents or guest stack words are repurposed.
static FEX64InterpreterNormalization normalizeFEX64InterpreterHandoff(
        KMemory64* mem, const Elf64ParseResult& image, U64 base,
        U64 programEntry) {
    if (!mem || !mem->nativeIdentityMode()) {
        return FEX64InterpreterNormalization::NotNeeded;
    }

    static constexpr std::array<U8, 56> kHandoffTail = {
        0x4c, 0x89, 0xe0, 0x4c, 0x89, 0xec, 0x48, 0x8b,
        0x14, 0x24, 0x48, 0x89, 0xd6, 0x48, 0x83, 0xe4,
        0xf0, 0x48, 0x8b, 0x3d, 0x70, 0x8a, 0x01, 0x00,
        0x49, 0x8d, 0x4c, 0xd5, 0x10, 0x49, 0x8d, 0x55,
        0x08, 0x31, 0xed, 0xe8, 0xe0, 0x61, 0xfe, 0xff,
        0x48, 0x8d, 0x15, 0xd9, 0x5d, 0xfe, 0xff, 0x4c,
        0x89, 0xec, 0x41, 0xff, 0xe4, 0x0f, 0x1f, 0x00,
    };
    static constexpr U64 kJumpBytes = 6;

    U64 matchAddress = 0;
    U64 trampolineAddress = 0;
    U32 matchProtection = 0;
    U32 matchCount = 0;
    for (const Elf64LoadSegment& segment : image.segments) {
        if (!(segment.flags & PF_X) || segment.filesz < kHandoffTail.size() ||
            segment.filesz > SIZE_MAX ||
            segment.vaddr > UINT64_MAX - segment.memsz) {
            continue;
        }
        std::vector<U8> bytes((size_t)segment.filesz);
        mem->memcpyFromGuest(bytes.data(), segment.vaddr + base,
                             segment.filesz);
        auto cursor = bytes.cbegin();
        while (cursor != bytes.cend()) {
            const auto found = std::search(cursor, bytes.cend(),
                                           kHandoffTail.cbegin(),
                                           kHandoffTail.cend());
            if (found == bytes.cend()) break;
            const U64 offset = (U64)std::distance(bytes.cbegin(), found);
            matchAddress = segment.vaddr + base + offset;
            matchProtection = phdrFlagsToProt(segment.flags);
            matchCount++;

            const U64 segmentEnd = segment.vaddr + segment.memsz;
            if (segmentEnd <= UINT64_MAX - K64_PAGE_MASK) {
                const U64 mappedEnd =
                    (segmentEnd + K64_PAGE_MASK) & ~K64_PAGE_MASK;
                const U64 alignedTrampoline =
                    (segmentEnd + 15) & ~static_cast<U64>(15);
                if (alignedTrampoline <= mappedEnd &&
                    mappedEnd - alignedTrampoline >=
                        boxedvn::Fex64LoaderHandoffPatch::kTrampolineSize) {
                    trampolineAddress = base + alignedTrampoline;
                }
            }
            cursor = found + 1;
        }
    }

    if (matchCount == 0) {
        klog("loadProgram64: translated-loader entry handoff replacement not required");
        return FEX64InterpreterNormalization::NotNeeded;
    }
    if (matchCount != 1 || trampolineAddress == 0 || programEntry == 0) {
        klog_fmt("loadProgram64: refusing ambiguous translated-loader entry handoff replacement (matches=%u trampoline=0x%llx entry=0x%llx)",
                 matchCount, (unsigned long long)trampolineAddress,
                 (unsigned long long)programEntry);
        return FEX64InterpreterNormalization::Failed;
    }

    const U64 jumpAddress = matchAddress + kHandoffTail.size() - kJumpBytes;
    const auto patch = boxedvn::planFex64LoaderHandoffPatch(
        jumpAddress, trampolineAddress, programEntry);
    if (!patch.has_value()) {
        klog("loadProgram64: translated-loader entry handoff trampoline is unreachable");
        return FEX64InterpreterNormalization::Failed;
    }

    std::array<U8, boxedvn::Fex64LoaderHandoffPatch::kTrampolineSize>
        priorTrampoline = {};
    mem->memcpyFromGuest(priorTrampoline.data(), trampolineAddress,
                         priorTrampoline.size());
    if (std::any_of(priorTrampoline.cbegin(), priorTrampoline.cend(),
                    [](U8 byte) { return byte != 0; })) {
        klog_fmt("loadProgram64: translated-loader entry handoff trampoline 0x%llx is not padding",
                 (unsigned long long)trampolineAddress);
        return FEX64InterpreterNormalization::Failed;
    }

    const U64 jumpPage = jumpAddress & ~K64_PAGE_MASK;
    const U64 trampolinePage = trampolineAddress & ~K64_PAGE_MASK;
    const bool jumpWritable = mem->mprotect(
        jumpPage, K64_PAGE_SIZE, K_PROT_READ | K_PROT_WRITE) == jumpPage;
    const bool trampolineWritable = jumpWritable &&
        (trampolinePage == jumpPage ||
         mem->mprotect(trampolinePage, K64_PAGE_SIZE,
                       K_PROT_READ | K_PROT_WRITE) == trampolinePage);
    if (!trampolineWritable) {
        if (jumpWritable) {
            mem->mprotect(jumpPage, K64_PAGE_SIZE, matchProtection);
        }
        klog("loadProgram64: translated-loader entry handoff pages could not be made writable");
        return FEX64InterpreterNormalization::Failed;
    }

    mem->memcpyToGuest(trampolineAddress, patch->trampoline.data(),
                       patch->trampoline.size());
    mem->memcpyToGuest(jumpAddress, patch->branchToTrampoline.data(),
                       patch->branchToTrampoline.size());

    std::array<U8, 6> verifyJump = {};
    std::array<U8, boxedvn::Fex64LoaderHandoffPatch::kTrampolineSize>
        verifyTrampoline = {};
    mem->memcpyFromGuest(verifyJump.data(), jumpAddress, verifyJump.size());
    mem->memcpyFromGuest(verifyTrampoline.data(), trampolineAddress,
                         verifyTrampoline.size());
    bool restored = true;
    if (trampolinePage != jumpPage) {
        restored = mem->mprotect(trampolinePage, K64_PAGE_SIZE,
                                 matchProtection) == trampolinePage;
    }
    restored = (mem->mprotect(jumpPage, K64_PAGE_SIZE,
                              matchProtection) == jumpPage) && restored;
    if (!restored || verifyJump != patch->branchToTrampoline ||
        verifyTrampoline != patch->trampoline) {
        klog_fmt("loadProgram64: translated-loader entry handoff replacement failed at 0x%llx (write=%d restore=%d)",
                 (unsigned long long)jumpAddress,
                 verifyJump == patch->branchToTrampoline &&
                         verifyTrampoline == patch->trampoline ? 1 : 0,
                 restored ? 1 : 0);
        return FEX64InterpreterNormalization::Failed;
    }

    klog_fmt("loadProgram64: installed translated-loader entry handoff 0x%llx -> 0x%llx via trampoline 0x%llx",
             (unsigned long long)jumpAddress,
             (unsigned long long)programEntry,
             (unsigned long long)trampolineAddress);
    return FEX64InterpreterNormalization::Applied;
}

// The pinned glibc loader contains an SSE2 memset implementation that does not
// retire under the iOS FEX path after the final libc mapping succeeds. Device
// samples remain inside this routine even after normalising its pointer update,
// while the bundled correctness process proves that the same translator and
// memory path complete REP STOSB correctly. Replace only the exact pinned
// routine entry, before any guest code is dispatched, with the ABI-equivalent
// scalar string operation. R9 is caller-saved and preserves memset's required
// return value while RDI and RCX are consumed.
static FEX64InterpreterNormalization normalizeFEX64InterpreterMemset(
        KMemory64* mem, const Elf64ParseResult& image, U64 base) {
    if (!mem || !mem->nativeIdentityMode()) {
        return FEX64InterpreterNormalization::NotNeeded;
    }

    static constexpr std::array<U8, 24> kPattern = {
        0xf3, 0x0f, 0x1e, 0xfa,       // endbr64
        0x66, 0x0f, 0x6e, 0xc6,       // movd xmm0, esi
        0x48, 0x89, 0xf8,             // mov rax, rdi
        0x66, 0x0f, 0x60, 0xc0,       // punpcklbw xmm0, xmm0
        0x66, 0x0f, 0x61, 0xc0,       // punpcklwd xmm0, xmm0
        0x66, 0x0f, 0x70, 0xc0, 0x00, // pshufd xmm0, xmm0, 0
    };
    static constexpr std::array<U8, 19> kReplacement = {
        0xf3, 0x0f, 0x1e, 0xfa,       // endbr64
        0x49, 0x89, 0xf9,             // mov r9, rdi
        0x48, 0x89, 0xd1,             // mov rcx, rdx
        0x40, 0x88, 0xf0,             // mov al, sil
        0xf3, 0xaa,                   // rep stosb
        0x4c, 0x89, 0xc8,             // mov rax, r9
        0xc3,                         // ret
    };

    U64 matchAddress = 0;
    U32 matchProtection = 0;
    U32 matchCount = 0;
    for (const Elf64LoadSegment& segment : image.segments) {
        if (!(segment.flags & PF_X) || segment.filesz < kPattern.size() ||
            segment.filesz > SIZE_MAX) {
            continue;
        }
        std::vector<U8> bytes((size_t)segment.filesz);
        mem->memcpyFromGuest(bytes.data(), segment.vaddr + base,
                             segment.filesz);
        auto cursor = bytes.cbegin();
        while (cursor != bytes.cend()) {
            auto found = std::search(cursor, bytes.cend(),
                                     kPattern.cbegin(), kPattern.cend());
            if (found == bytes.cend()) break;
            const U64 offset = (U64)std::distance(bytes.cbegin(), found);
            matchAddress = segment.vaddr + base + offset;
            matchProtection = phdrFlagsToProt(segment.flags);
            matchCount++;
            cursor = found + 1;
        }
    }

    if (matchCount == 0) {
        klog("loadProgram64: translated-loader memset replacement not required");
        return FEX64InterpreterNormalization::NotNeeded;
    }
    if (matchCount != 1) {
        klog_fmt("loadProgram64: refusing ambiguous translated-loader memset replacement (matches=%u)",
                 matchCount);
        return FEX64InterpreterNormalization::Failed;
    }

    const U64 pageAddress = matchAddress & ~K64_PAGE_MASK;
    if (mem->mprotect(pageAddress, K64_PAGE_SIZE,
                      K_PROT_READ | K_PROT_WRITE) != pageAddress) {
        klog_fmt("loadProgram64: translated-loader memset replacement could not make 0x%llx writable",
                 (unsigned long long)matchAddress);
        return FEX64InterpreterNormalization::Failed;
    }

    mem->memcpyToGuest(matchAddress, kReplacement.data(),
                       kReplacement.size());
    std::array<U8, kReplacement.size()> verify = {};
    mem->memcpyFromGuest(verify.data(), matchAddress,
                         verify.size());
    const bool restored = mem->mprotect(pageAddress, K64_PAGE_SIZE,
                                        matchProtection) == pageAddress;
    if (!restored || verify != kReplacement) {
        klog_fmt("loadProgram64: translated-loader memset replacement failed at 0x%llx (write=%d restore=%d)",
                 (unsigned long long)matchAddress,
                 verify == kReplacement ? 1 : 0, restored ? 1 : 0);
        return FEX64InterpreterNormalization::Failed;
    }

    klog_fmt("loadProgram64: installed translated-loader REP STOS memset at 0x%llx",
             (unsigned long long)matchAddress);
    return FEX64InterpreterNormalization::Applied;
}

template <typename CopySegment>
static bool mapLoadSpan(KMemory64* mem, const Elf64ParseResult& r,
                        U64 reloc, const char* tag,
                        CopySegment copySegment) {
    U64 spanStart = UINT64_MAX;
    U64 spanEnd = 0;
    for (const Elf64LoadSegment& seg : r.segments) {
        if (seg.vaddr > UINT64_MAX - reloc) {
            klog_fmt("loadProgram64[%s]: relocated segment address overflow", tag);
            return false;
        }
        const U64 vaddr = seg.vaddr + reloc;
        if (seg.memsz > UINT64_MAX - vaddr ||
            vaddr + seg.memsz > UINT64_MAX - K64_PAGE_MASK) {
            klog_fmt("loadProgram64[%s]: relocated segment length overflow", tag);
            return false;
        }
        const U64 alignedAddr = vaddr & ~K64_PAGE_MASK;
        const U64 alignedEnd = (vaddr + seg.memsz + K64_PAGE_MASK) & ~K64_PAGE_MASK;
        if (alignedAddr < spanStart) spanStart = alignedAddr;
        if (alignedEnd > spanEnd) spanEnd = alignedEnd;
    }
    if (spanStart == UINT64_MAX || spanEnd <= spanStart) {
        klog_fmt("loadProgram64[%s]: empty PT_LOAD span", tag);
        return false;
    }

    // Darwin's host pages are larger than the 4 KiB x86 guest pages. Mapping
    // each PT_LOAD independently makes two adjacent guest segments request
    // partially-overlapping host pages, which the guarded identity mapper must
    // reject. Reserve the complete image once, while writable, then populate
    // every segment and apply final page permissions after all copies.
    const U64 mapped = mem->mmapAnonymousFixed(
        spanStart, spanEnd - spanStart, K_PROT_READ | K_PROT_WRITE);
    if (mapped != spanStart) {
        klog_fmt("loadProgram64[%s]: mmap failed for span [0x%llx,0x%llx) (got 0x%llx)",
                 tag, (unsigned long long)spanStart,
                 (unsigned long long)spanEnd, (unsigned long long)mapped);
        return false;
    }

    for (const Elf64LoadSegment& seg : r.segments) {
        const U64 vaddr = seg.vaddr + reloc;
        if (!copySegment(seg, vaddr)) {
            return false;
        }
    }
    for (const Elf64LoadSegment& seg : r.segments) {
        if (seg.memsz > seg.filesz) {
            mem->memsetGuest(seg.vaddr + reloc + seg.filesz, 0,
                             seg.memsz - seg.filesz);
        }
    }

    for (U64 page = spanStart; page < spanEnd; page += K64_PAGE_SIZE) {
        U32 pageProt = 0;
        for (const Elf64LoadSegment& seg : r.segments) {
            const U64 segStart = (seg.vaddr + reloc) & ~K64_PAGE_MASK;
            const U64 segEnd = (seg.vaddr + reloc + seg.memsz + K64_PAGE_MASK) &
                               ~K64_PAGE_MASK;
            if (page >= segStart && page < segEnd) {
                pageProt |= phdrFlagsToProt(seg.flags);
            }
        }
        if (mem->mprotect(page, K64_PAGE_SIZE, pageProt) != page) {
            klog_fmt("loadProgram64[%s]: final mprotect failed at 0x%llx",
                     tag, (unsigned long long)page);
            return false;
        }
    }

    for (const Elf64LoadSegment& seg : r.segments) {
        const U64 vaddr = seg.vaddr + reloc;
        const U64 alignedAddr = vaddr & ~K64_PAGE_MASK;
        const U64 mapLen = ((vaddr - alignedAddr) + seg.memsz +
                            K64_PAGE_MASK) & ~K64_PAGE_MASK;
        klog_fmt("loadProgram64[%s]:   mapped seg vaddr=0x%llx len=0x%llx prot=0x%x filesz=0x%llx",
                 tag, (unsigned long long)alignedAddr,
                 (unsigned long long)mapLen, phdrFlagsToProt(seg.flags),
                 (unsigned long long)seg.filesz);
    }
    return true;
}

// Public companion to the file-backed mapSegments — same shape, buffer
// source. Self-test entry point.
bool ElfLoader64::mapSegmentsFromBuffer(KMemory64* mem,
                                        const Elf64ParseResult& r,
                                        const U8* buffer,
                                        U64 bufferLength,
                                        U64 reloc,
                                        const char* tag) {
    return mapLoadSpan(mem, r, reloc, tag,
        [&](const Elf64LoadSegment& seg, U64 vaddr) {
            if (seg.filesz == 0) return true;
            if (seg.offset > bufferLength ||
                seg.filesz > bufferLength - seg.offset) {
                klog_fmt("mapSegmentsFromBuffer[%s]: segment extends past buffer (offset=%llu filesz=%llu bufLen=%llu)",
                         tag, (unsigned long long)seg.offset,
                         (unsigned long long)seg.filesz,
                         (unsigned long long)bufferLength);
                return false;
            }
            mem->memcpyToGuest(vaddr, buffer + seg.offset, seg.filesz);
            return true;
        });
}

// Map every PT_LOAD segment of one parsed ELF into guest memory at the
// given relocation base. Returns true on success. Used for both the main
// executable and (recursively) PT_INTERP.
static bool mapSegments(KMemory64* mem, FsOpenNode* openNode,
                        const Elf64ParseResult& r, U64 reloc,
                        const char* tag, int pid, const char* modPath) {
    (void)pid;
    (void)modPath;
    return mapLoadSpan(mem, r, reloc, tag,
        [&](const Elf64LoadSegment& seg, U64 vaddr) {
            if (seg.filesz == 0) return true;
            if (seg.filesz > UINT32_MAX) {
                klog_fmt("loadProgram64[%s]: segment is too large to read", tag);
                return false;
            }
            std::vector<U8> buf((size_t)seg.filesz);
            openNode->seek((U64)seg.offset);
            U32 read = openNode->readNative(buf.data(), (U32)seg.filesz);
            if (read != seg.filesz) {
                klog_fmt("loadProgram64[%s]: short read on segment (got %u of %llu)",
                         tag, read, (unsigned long long)seg.filesz);
                return false;
            }
            mem->memcpyToGuest(vaddr, buf.data(), seg.filesz);
            return true;
        });
}

// Open a guest-rootfs path. The interpreter path in PT_INTERP is an absolute
// guest path like "/lib64/ld-linux-x86-64.so.2". Resolve via the Fs layer
// against the empty current directory (which uses the root).
static FsOpenNode* openGuestPath(BString path) {
    std::shared_ptr<FsNode> node = Fs::getNodeFromLocalPath(B(""), path, true);
    if (!node) return nullptr;
    return node->open(K_O_RDONLY);
}

// Eager application of R_X86_64_RELATIVE relocations from a parsed dynamic
// section. RELATIVE entries do not name a symbol — they just say "store
// (load_base + addend) at this offset". They make up the vast majority of
// relocations in PIE binaries and shared libraries, and resolving them in
// the host loader is what lets a PIE exe run without first jumping through
// the dynamic linker.
//
// Cross-module / symbol-resolving relocations (GLOB_DAT, JUMP_SLOT, 64,
// COPY, IRELATIVE) are left for Milestone A3 once DT_NEEDED recursion is
// in place — they require the cross-library symbol table that A3 builds.
//
// Returns the number of RELATIVE relocations applied (mainly for logging).
U64 ElfLoader64::applyRelativeRelocations(KMemory64* mem,
                                          const Elf64DynamicInfo& dyn,
                                          U64 reloc,
                                          const char* tag) {
    if (!dyn.present || dyn.memsz == 0) return 0;

    // Read the entire PT_DYNAMIC array out of guest memory (it's small —
    // typically a few dozen entries).
    U64 dynAddr = dyn.vaddr + reloc;
    U64 nEntries = dyn.memsz / sizeof(k_Elf64_Dyn);
    std::vector<k_Elf64_Dyn> dynArr(nEntries);
    mem->memcpyFromGuest(dynArr.data(), dynAddr, dyn.memsz);

    // First pass: pluck the tags we care about. We only use the RELA tables
    // here; JMPREL (PLT relocations) are also RELA-shaped on x86-64 but
    // their entries are GLOB_DAT/JUMP_SLOT which we leave for A3.
    U64 relaAddr = 0, relaSz = 0, relaEnt = sizeof(k_Elf64_Rela);
    for (U64 i = 0; i < nEntries; i++) {
        const k_Elf64_Dyn& d = dynArr[i];
        if (d.d_tag == k_DT_NULL) break;
        switch (d.d_tag) {
            case k_DT_RELA:    relaAddr = d.d_un.d_ptr; break;
            case k_DT_RELASZ:  relaSz   = d.d_un.d_val; break;
            case k_DT_RELAENT: relaEnt  = d.d_un.d_val; break;
        }
    }

    if (relaAddr == 0 || relaSz == 0) {
        klog_fmt("loadProgram64[%s]: no RELA table — skipping relocation pass", tag);
        return 0;
    }

    U64 nRela = relaSz / relaEnt;
    klog_fmt("loadProgram64[%s]: RELA table at 0x%llx, %llu entries (entsize=%llu)",
             tag,
             (unsigned long long)(relaAddr + reloc),
             (unsigned long long)nRela,
             (unsigned long long)relaEnt);

    U64 relativeCount = 0;
    U64 skippedCount = 0;
    for (U64 i = 0; i < nRela; i++) {
        k_Elf64_Rela rela;
        mem->memcpyFromGuest(&rela, relaAddr + reloc + i * relaEnt, sizeof(rela));
        U32 type = k_ELF64_R_TYPE(rela.r_info);
        U64 dst = rela.r_offset + reloc;
        switch (type) {
            case k_R_X86_64_RELATIVE:
                mem->writeq(dst, (U64)((S64)reloc + rela.r_addend));
                relativeCount++;
                break;
            case k_R_X86_64_NONE:
                break;
            default:
                // Defer to ld-linux for everything else (symbol-bound types).
                skippedCount++;
                break;
        }
    }
    klog_fmt("loadProgram64[%s]: applied %llu RELATIVE relocs (deferred %llu symbol-bound)",
             tag,
             (unsigned long long)relativeCount,
             (unsigned long long)skippedCount);
    return relativeCount;
}

// Resolve a NUL-terminated string out of guest memory at strtab+offset. Caps
// at maxLen so a malformed entry can't run off the page and DoS the loader.
static std::string readGuestCString(KMemory64* mem, U64 strtab, U32 offset, U32 maxLen = 1024) {
    std::string out;
    out.reserve(64);
    for (U32 i = 0; i < maxLen; i++) {
        U8 b = mem->readb(strtab + offset + i);
        if (b == 0) break;
        out.push_back((char)b);
    }
    return out;
}

// Apply the symbol-bound R_X86_64_* relocations against the dynamic section
// at dyn.vaddr+reloc. This is the Milestone A3 relocation engine — the
// remaining piece (DT_NEEDED file loading + symbol-table merging) is a
// separate concern handled by the caller; this function takes the merged
// symbol table as input so it can be unit-tested in isolation against
// synthetic data.
//
// Handled types:
//   R_X86_64_GLOB_DAT  *dst = sym + addend
//   R_X86_64_JUMP_SLOT *dst = sym + addend         (PLT slot — eager binding)
//   R_X86_64_64        *dst = sym + addend         (64-bit absolute)
//   R_X86_64_RELATIVE  ignored (handled by applyRelativeRelocations)
//   R_X86_64_NONE      ignored
//   any other type     skipped, counted as unresolved-deferred
//
// Walks BOTH DT_RELA and DT_JMPREL — x86-64 uses RELA for both. The PLT
// table (DT_JMPREL) is conventionally pure JUMP_SLOT, but we don't assume
// that — the same opcode switch handles both tables.
U64 ElfLoader64::applySymbolRelocations(KMemory64* mem,
                                        const Elf64DynamicInfo& dyn,
                                        U64 reloc,
                                        const std::unordered_map<std::string, U64>& symbols,
                                        const char* tag,
                                        U64* outResolved,
                                        U64* outUnresolved) {
    U64 resolved = 0;
    U64 unresolved = 0;
    auto finish = [&]() {
        if (outResolved) *outResolved = resolved;
        if (outUnresolved) *outUnresolved = unresolved;
        return resolved + unresolved;
    };
    if (!dyn.present || dyn.memsz == 0) return finish();

    U64 dynAddr = dyn.vaddr + reloc;
    U64 nEntries = dyn.memsz / sizeof(k_Elf64_Dyn);
    std::vector<k_Elf64_Dyn> dynArr(nEntries);
    mem->memcpyFromGuest(dynArr.data(), dynAddr, dyn.memsz);

    U64 relaAddr = 0, relaSz = 0, relaEnt = sizeof(k_Elf64_Rela);
    U64 pltRelAddr = 0, pltRelSz = 0;
    U64 symtab = 0, strtab = 0;
    U64 syment = sizeof(k_Elf64_Sym);
    for (U64 i = 0; i < nEntries; i++) {
        const k_Elf64_Dyn& d = dynArr[i];
        if (d.d_tag == k_DT_NULL) break;
        switch (d.d_tag) {
            case k_DT_RELA:     relaAddr   = d.d_un.d_ptr; break;
            case k_DT_RELASZ:   relaSz     = d.d_un.d_val; break;
            case k_DT_RELAENT:  relaEnt    = d.d_un.d_val; break;
            case k_DT_JMPREL:   pltRelAddr = d.d_un.d_ptr; break;
            case k_DT_PLTRELSZ: pltRelSz   = d.d_un.d_val; break;
            case k_DT_SYMTAB:   symtab     = d.d_un.d_ptr; break;
            case k_DT_STRTAB:   strtab     = d.d_un.d_ptr; break;
            case k_DT_SYMENT:   syment     = d.d_un.d_val; break;
        }
    }

    if (symtab == 0 || strtab == 0) {
        klog_fmt("applySymbolRelocations[%s]: no SYMTAB/STRTAB — nothing to resolve",
                 tag);
        return finish();
    }

    auto walkTable = [&](U64 tableAddr, U64 tableSz, U64 entSize, const char* which) {
        if (tableAddr == 0 || tableSz == 0) return;
        U64 nRela = tableSz / entSize;
        for (U64 i = 0; i < nRela; i++) {
            k_Elf64_Rela rela;
            mem->memcpyFromGuest(&rela, tableAddr + reloc + i * entSize, sizeof(rela));
            U32 type = k_ELF64_R_TYPE(rela.r_info);
            U32 symIdx = k_ELF64_R_SYM(rela.r_info);
            U64 dst = rela.r_offset + reloc;
            if (type == k_R_X86_64_NONE || type == k_R_X86_64_RELATIVE) continue;
            if (type != k_R_X86_64_GLOB_DAT &&
                type != k_R_X86_64_JUMP_SLOT &&
                type != k_R_X86_64_64 &&
                type != k_R_X86_64_COPY) {
                // IRELATIVE/TLS — out of scope for the eager pass.
                unresolved++;
                continue;
            }
            k_Elf64_Sym sym;
            mem->memcpyFromGuest(&sym, symtab + reloc + symIdx * syment, sizeof(sym));
            std::string name = readGuestCString(mem, strtab + reloc, sym.st_name);
            auto it = symbols.find(name);
            if (it == symbols.end()) {
                unresolved++;
                klog_fmt("applySymbolRelocations[%s/%s]: unresolved '%s' (type=%u)",
                         tag, which, name.c_str(), (unsigned)type);
                continue;
            }
            if (type == k_R_X86_64_COPY) {
                // Copy sym.st_size bytes from the source DSO's symbol into the
                // exe's reserved BSS slot at dst. Addend is unused for COPY
                // per ABI. Size comes from the local sym entry — the exe
                // declares its placeholder at the right size, and that
                // matches the source's allocated size.
                U64 size = sym.st_size;
                if (size == 0 || size > (1ULL << 20)) {
                    // Sanity-cap at 1 MiB; a COPY larger than that points to a
                    // malformed binary or a misparse.
                    klog_fmt("applySymbolRelocations[%s/%s]: COPY '%s' rejected (size=%llu)",
                             tag, which, name.c_str(), (unsigned long long)size);
                    unresolved++;
                    continue;
                }
                std::vector<U8> buf((size_t)size);
                mem->memcpyFromGuest(buf.data(), it->second, size);
                mem->memcpyToGuest(dst, buf.data(), size);
                resolved++;
                continue;
            }
            U64 value = it->second + (U64)rela.r_addend;
            mem->writeq(dst, value);
            resolved++;
        }
    };

    walkTable(relaAddr, relaSz, relaEnt, "RELA");
    walkTable(pltRelAddr, pltRelSz, sizeof(k_Elf64_Rela), "JMPREL");

    klog_fmt("applySymbolRelocations[%s]: resolved=%llu unresolved=%llu",
             tag,
             (unsigned long long)resolved,
             (unsigned long long)unresolved);
    return finish();
}

// Extract DT_NEEDED library names from the dynamic array. Two-pass walk:
// first find DT_STRTAB so we know where to dereference d_val into a name,
// then collect all DT_NEEDED entries in original order (ordering is
// load-significant for symbol-resolution scope under ld.so).
std::vector<std::string> ElfLoader64::extractNeededLibraries(KMemory64* mem,
                                                             const Elf64DynamicInfo& dyn,
                                                             U64 reloc) {
    std::vector<std::string> out;
    if (!dyn.present || dyn.memsz == 0) return out;

    U64 dynAddr = dyn.vaddr + reloc;
    U64 nEntries = dyn.memsz / sizeof(k_Elf64_Dyn);
    std::vector<k_Elf64_Dyn> dynArr(nEntries);
    mem->memcpyFromGuest(dynArr.data(), dynAddr, dyn.memsz);

    U64 strtab = 0;
    for (U64 i = 0; i < nEntries; i++) {
        const k_Elf64_Dyn& d = dynArr[i];
        if (d.d_tag == k_DT_NULL) break;
        if (d.d_tag == k_DT_STRTAB) { strtab = d.d_un.d_ptr; break; }
    }
    if (strtab == 0) {
        klog("extractNeededLibraries: no DT_STRTAB — skipping");
        return out;
    }

    U64 strtabAddr = strtab + reloc;
    for (U64 i = 0; i < nEntries; i++) {
        const k_Elf64_Dyn& d = dynArr[i];
        if (d.d_tag == k_DT_NULL) break;
        if (d.d_tag != k_DT_NEEDED) continue;
        std::string name = readGuestCString(mem, strtabAddr, (U32)d.d_un.d_val);
        if (!name.empty()) out.push_back(std::move(name));
    }
    klog_fmt("extractNeededLibraries: %llu DT_NEEDED entries",
             (unsigned long long)out.size());
    return out;
}

// Walk DT_SYMTAB/DT_STRTAB and collect every defined global/weak symbol.
// See header for the (strtab - symtab) length-bounding rationale.
std::unordered_map<std::string, U64>
ElfLoader64::extractGlobalSymbols(KMemory64* mem,
                                  const Elf64DynamicInfo& dyn,
                                  U64 reloc) {
    std::unordered_map<std::string, U64> out;
    if (!dyn.present || dyn.memsz == 0) return out;

    U64 dynAddr = dyn.vaddr + reloc;
    U64 nEntries = dyn.memsz / sizeof(k_Elf64_Dyn);
    std::vector<k_Elf64_Dyn> dynArr(nEntries);
    mem->memcpyFromGuest(dynArr.data(), dynAddr, dyn.memsz);

    U64 symtab = 0, strtab = 0;
    U64 syment = sizeof(k_Elf64_Sym);
    for (U64 i = 0; i < nEntries; i++) {
        const k_Elf64_Dyn& d = dynArr[i];
        if (d.d_tag == k_DT_NULL) break;
        switch (d.d_tag) {
            case k_DT_SYMTAB: symtab = d.d_un.d_ptr; break;
            case k_DT_STRTAB: strtab = d.d_un.d_ptr; break;
            case k_DT_SYMENT: syment = d.d_un.d_val; break;
        }
    }
    if (symtab == 0 || strtab == 0 || strtab <= symtab) {
        klog_fmt("extractGlobalSymbols: missing SYMTAB/STRTAB or non-adjacent layout (symtab=0x%llx strtab=0x%llx)",
                 (unsigned long long)symtab, (unsigned long long)strtab);
        return out;
    }

    U64 symBytes = strtab - symtab;
    U64 nSyms = symBytes / syment;
    U64 strBase = strtab + reloc;
    for (U64 i = 0; i < nSyms; i++) {
        k_Elf64_Sym sym;
        mem->memcpyFromGuest(&sym, symtab + reloc + i * syment, sizeof(sym));
        // st_shndx==0 (SHN_UNDEF) means this is a reference, not a
        // definition — skip.
        if (sym.st_shndx == 0) continue;
        U8 bind = (U8)(sym.st_info >> 4);
        // 0=LOCAL, 1=GLOBAL, 2=WEAK. Only GLOBAL/WEAK are exported.
        if (bind != 1 && bind != 2) continue;
        if (sym.st_name == 0) continue;
        std::string name = readGuestCString(mem, strBase, sym.st_name);
        if (name.empty()) continue;
        // Don't let a later weak override a strong; for now first-wins
        // (matches ld.so default scope rules within a single DSO).
        auto ins = out.emplace(std::move(name), sym.st_value + reloc);
        (void)ins;
    }
    klog_fmt("extractGlobalSymbols: %llu exported symbols (scanned %llu slots)",
             (unsigned long long)out.size(),
             (unsigned long long)nSyms);
    return out;
}

// Single-level DT_NEEDED orchestrator. See header for the contract.
U64 ElfLoader64::linkSharedObjects(KMemory64* mem,
                                   const Elf64ParseResult& mainParsed,
                                   U64 mainReloc,
                                   const std::vector<PreloadedLibrary>& preloaded,
                                   U64 firstLibBase,
                                   std::vector<LinkedLibrary>* outLibs) {
    // Start the symbol union with whatever the main exe defines (rarely
    // important — most exes import everything — but executables can
    // export symbols via -rdynamic, and we shouldn't lose them).
    std::unordered_map<std::string, U64> globals =
        extractGlobalSymbols(mem, mainParsed.dynamic, mainReloc);

    // Step through each lib: parse → map → RELATIVE → harvest globals.
    std::vector<LinkedLibrary> linked;
    U64 nextBase = firstLibBase;
    const U64 LIB_STEP = 16ULL * 1024 * 1024; // 16 MiB per lib slot

    for (const PreloadedLibrary& lib : preloaded) {
        Elf64ParseResult r = parseBuffer(lib.buffer, lib.length);
        if (!r.ok) {
            klog_fmt("linkSharedObjects: parse failed for '%s'", lib.name.c_str());
            continue;
        }
        U64 reloc = lib.desiredReloc ? lib.desiredReloc : nextBase;
        if (!mapSegmentsFromBuffer(mem, r, lib.buffer, lib.length, reloc, lib.name.c_str())) {
            klog_fmt("linkSharedObjects: map failed for '%s'", lib.name.c_str());
            continue;
        }
        applyRelativeRelocations(mem, r.dynamic, reloc, lib.name.c_str());
        auto libGlobals = extractGlobalSymbols(mem, r.dynamic, reloc);
        for (auto& [name, addr] : libGlobals) {
            // First definition wins (matches ld.so load-order scope).
            globals.emplace(name, addr);
        }
        linked.push_back({lib.name, reloc, r});
        if (!lib.desiredReloc) nextBase += LIB_STEP;
    }

    // Resolve symbol-bound relocs on the main exe AND every lib against
    // the merged table. Main first matches ld.so's main-exe-first
    // symbol-search scope.
    U64 totalResolved = 0, totalUnresolved = 0;
    U64 resolved = 0, unresolved = 0;
    applySymbolRelocations(mem, mainParsed.dynamic, mainReloc, globals,
                           "exe", &resolved, &unresolved);
    totalResolved   += resolved;
    totalUnresolved += unresolved;
    for (const LinkedLibrary& lib : linked) {
        resolved = unresolved = 0;
        applySymbolRelocations(mem, lib.parsed.dynamic, lib.reloc, globals,
                               lib.name.c_str(), &resolved, &unresolved);
        totalResolved   += resolved;
        totalUnresolved += unresolved;
    }
    klog_fmt("linkSharedObjects: %zu libs linked, %llu resolved, %llu unresolved",
             linked.size(),
             (unsigned long long)totalResolved,
             (unsigned long long)totalUnresolved);

    U64 linkedCount = linked.size();
    if (outLibs) *outLibs = std::move(linked);
    return linkedCount;
}

// Transitive DT_NEEDED orchestrator. BFS over the dependency graph rooted
// at the main exe's DT_NEEDED list. Each visited library:
//   1. is fetched via the caller's callback
//   2. is parsed + mapped + RELATIVE-relocated
//   3. has its own DT_NEEDED entries enqueued (deduped by name)
// After the BFS finishes, the merged symbol table is built and the
// symbol-bound relocations are applied on the main exe AND every loaded
// library, matching ld.so's load-order scope.
U64 ElfLoader64::linkSharedObjectsRecursive(KMemory64* mem,
                                            const Elf64ParseResult& mainParsed,
                                            U64 mainReloc,
                                            const LibFetcher& fetcher,
                                            U64 firstLibBase,
                                            std::vector<LinkedLibrary>* outLibs) {
    // BFS state.
    std::vector<std::string> queue =
        extractNeededLibraries(mem, mainParsed.dynamic, mainReloc);
    std::unordered_map<std::string, U64> visited; // name -> reloc base
    std::vector<LinkedLibrary> linked;
    // We keep the raw bytes alive until linking finishes (we may need to
    // re-walk them, and the parser's substring views point into them).
    std::vector<std::vector<U8>> retainedBlobs;

    U64 nextBase = firstLibBase;
    const U64 LIB_STEP = 16ULL * 1024 * 1024;

    for (size_t qi = 0; qi < queue.size(); qi++) {
        // COPY the name out of the queue — pushing into `queue` later in
        // this iteration can resize the vector and invalidate any reference
        // we hold into it. A dangling string here looks like a successful
        // load with a garbage `name` field in the linked vector.
        std::string name = queue[qi];
        if (visited.count(name)) continue;

        FetchedLibrary fetched = fetcher(name);
        if (fetched.bytes.empty()) {
            klog_fmt("linkSharedObjectsRecursive: fetcher returned empty for '%s'",
                     name.c_str());
            // Still mark visited so we don't keep re-asking.
            visited.emplace(name, 0);
            continue;
        }

        retainedBlobs.push_back(std::move(fetched.bytes));
        const std::vector<U8>& blob = retainedBlobs.back();

        Elf64ParseResult r = parseBuffer(blob.data(), blob.size());
        if (!r.ok) {
            klog_fmt("linkSharedObjectsRecursive: parse failed for '%s'",
                     name.c_str());
            visited.emplace(name, 0);
            continue;
        }

        U64 reloc = nextBase;
        if (!mapSegmentsFromBuffer(mem, r, blob.data(), blob.size(), reloc,
                                   name.c_str())) {
            klog_fmt("linkSharedObjectsRecursive: map failed for '%s'",
                     name.c_str());
            visited.emplace(name, 0);
            continue;
        }
        applyRelativeRelocations(mem, r.dynamic, reloc, name.c_str());
        // Enqueue this library's own DT_NEEDED entries (BFS).
        auto deps = extractNeededLibraries(mem, r.dynamic, reloc);
        for (const std::string& dep : deps) {
            if (!visited.count(dep)) queue.push_back(dep);
        }

        visited.emplace(name, reloc);
        linked.push_back({name, reloc, r});
        nextBase += LIB_STEP;
    }

    // Build the merged symbol table from the main exe + every linked lib.
    std::unordered_map<std::string, U64> globals =
        extractGlobalSymbols(mem, mainParsed.dynamic, mainReloc);
    for (const LinkedLibrary& lib : linked) {
        auto libGlobals = extractGlobalSymbols(mem, lib.parsed.dynamic, lib.reloc);
        for (auto& [n, addr] : libGlobals) globals.emplace(n, addr);
    }

    // Apply symbol-bound relocations on main + every lib.
    U64 totalResolved = 0, totalUnresolved = 0;
    U64 resolved = 0, unresolved = 0;
    applySymbolRelocations(mem, mainParsed.dynamic, mainReloc, globals,
                           "exe", &resolved, &unresolved);
    totalResolved += resolved; totalUnresolved += unresolved;
    for (const LinkedLibrary& lib : linked) {
        resolved = unresolved = 0;
        applySymbolRelocations(mem, lib.parsed.dynamic, lib.reloc, globals,
                               lib.name.c_str(), &resolved, &unresolved);
        totalResolved += resolved; totalUnresolved += unresolved;
    }
    klog_fmt("linkSharedObjectsRecursive: %zu libs linked (transitive), "
             "%llu resolved, %llu unresolved",
             linked.size(),
             (unsigned long long)totalResolved,
             (unsigned long long)totalUnresolved);

    U64 count = linked.size();
    if (outLibs) *outLibs = std::move(linked);
    return count;
}

// glibc x86-64 TLS layout (variant II, the only one glibc uses on amd64):
//
//   [ static TLS image (memsz bytes) ][ TCB (≥ tcbhead_t) ]
//                                     ^
//                                     fs base = TCB address
//
// The TLS image sits at negative offsets from FS. The first qword of the
// TCB is the self-pointer ($fs:0 must equal $fs), which glibc reads in its
// hot paths to find the current thread-control block. We size the TCB at
// 0x100 bytes — larger than the actual tcbhead_t (~0x80) so plenty of
// padding for things we haven't audited.
//
// blockBase must be pre-mapped by the caller (caller chooses the address
// so it can either pin a known free region or allocate from a heap).
U64 ElfLoader64::setupStaticTls(KMemory64* mem,
                                const Elf64TlsInfo& tls,
                                U64 imageBase,
                                U64 blockBase) {
    if (!tls.present) return 0;

    // Round image size up to alignment so the TCB lands aligned too.
    U64 align = tls.align ? tls.align : 8;
    U64 imageSize = (tls.memsz + align - 1) & ~(align - 1);

    // Copy the file portion of the image (filesz bytes) into the front of
    // the block, then zero-fill the BSS portion (memsz - filesz).
    if (tls.filesz) {
        std::vector<U8> buf(tls.filesz);
        mem->memcpyFromGuest(buf.data(), imageBase, tls.filesz);
        mem->memcpyToGuest(blockBase, buf.data(), tls.filesz);
    }
    if (tls.memsz > tls.filesz) {
        mem->memsetGuest(blockBase + tls.filesz, 0,
                         tls.memsz - tls.filesz);
    }

    // TCB sits immediately after the (aligned) image. First qword =
    // self-pointer.
    U64 tcb = blockBase + imageSize;
    mem->writeq(tcb, tcb);

    klog_fmt("setupStaticTls: image=[0x%llx..+%llu] tcb=0x%llx (fs=0x%llx)",
             (unsigned long long)blockBase,
             (unsigned long long)imageSize,
             (unsigned long long)tcb,
             (unsigned long long)tcb);
    return tcb;
}

bool ElfLoader64::loadProgram(KThread* thread, FsOpenNode* openNode, U64* rip) {
    klog("loadProgram64: begin");
    Elf64ParseResult r = parse(openNode);
    if (!r.ok) {
        return false;
    }
    klog_fmt("loadProgram64: entry=0x%llx phoff=0x%llx phnum=%u segments=%u interp=%s pie=%d",
             (unsigned long long)r.entry,
             (unsigned long long)r.phoff,
             (U32)r.phnum,
             (U32)r.segments.size(),
             r.interpreter.length() ? r.interpreter.c_str() : "(none)",
             r.isPie ? 1 : 0);
    klog_fmt("loadProgram64: load range [0x%llx, 0x%llx) span=%llu bytes",
             (unsigned long long)r.baseAddrLow,
             (unsigned long long)r.baseAddrHigh,
             (unsigned long long)(r.baseAddrHigh - r.baseAddrLow));
    if (r.dynamic.present) {
        klog_fmt("loadProgram64: PT_DYNAMIC vaddr=0x%llx memsz=%llu",
                 (unsigned long long)r.dynamic.vaddr,
                 (unsigned long long)r.dynamic.memsz);
    }
    if (r.tls.present) {
        klog_fmt("loadProgram64: PT_TLS vaddr=0x%llx filesz=%llu memsz=%llu align=%llu",
                 (unsigned long long)r.tls.vaddr,
                 (unsigned long long)r.tls.filesz,
                 (unsigned long long)r.tls.memsz,
                 (unsigned long long)r.tls.align);
    }

    if (!thread || !thread->process) {
        klog("loadProgram64: null thread/process");
        return false;
    }
    KProcess* process = thread->process.get();
    if (!process->memory64) {
        process->memory64 = new KMemory64(process,
#ifdef BOXEDWINE_FEX64_BACKEND
                                          process->useFEX64
#else
                                          false
#endif
        );
    }
    KMemory64* mem = process->memory64;

    // Native-identity FEX code dereferences guest pointers as host pointers.
    // The historical low PIE base is inside iOS's reserved malloc-xzone band,
    // so relocate the image into the proven native guest window. This is also
    // deliberately applied to ET_EXEC: a non-PIE image with absolute low
    // addresses is not safe to map on this host and will be rejected by the
    // guarded KMemory64 mapping path if its relocations cannot be honored.
    U64 reloc = mem->nativeIdentityMode()
        ? K64_NATIVE_GUEST_IMAGE_BASE
        : (r.isPie ? X64_PIE_BASE : 0);
    if (mem->nativeIdentityMode()) {
        klog_fmt("loadProgram64: native guest image base=0x%llx (elf=%s)",
                 (unsigned long long)reloc, r.isPie ? "PIE" : "ET_EXEC");
    }

    if (!mapSegments(mem, openNode, r, reloc, "exe", (int)process->id,
                     process->name.length() ? process->name.c_str() : "exe")) {
        return false;
    }

    // Apply R_X86_64_RELATIVE relocations against the exe's dynamic section
    // (if any). Symbol-bound relocations are still left to ld-linux until
    // Milestone A3 wires up cross-library symbol resolution.
    applyRelativeRelocations(mem, r.dynamic, reloc, "exe");

    // PT_INTERP owns the main image's complete relocation lifecycle. Our
    // narrow pass above intentionally leaves symbol-bound relocations and the
    // dynamic linker's own pointer rebasing unfinished, so applying RELRO here
    // would make ld-linux fault on its first write to DT_PLTGOT. A guest
    // dynamic linker enforces RELRO with mprotect after it has finished.
    //
    // Keep the existing kernel-side finalization only for images without an
    // interpreter, where this loader owns the relocation-to-entry boundary.
    if (r.relro.present && r.relro.memsz &&
        kernelOwnsElf64Relro(r.interpreter.length() != 0)) {
        U64 relroAddr = (r.relro.vaddr + reloc) & ~K64_PAGE_MASK;
        U64 relroEnd  = (r.relro.vaddr + reloc + r.relro.memsz + K64_PAGE_MASK) & ~K64_PAGE_MASK;
        mem->mprotect(relroAddr, relroEnd - relroAddr, 0x1 /* PROT_READ */);
        klog_fmt("loadProgram64: PT_GNU_RELRO 0x%llx..0x%llx -> RO",
                 (unsigned long long)relroAddr,
                 (unsigned long long)relroEnd);
    } else if (r.relro.present && r.relro.memsz) {
        U64 relroAddr = (r.relro.vaddr + reloc) & ~K64_PAGE_MASK;
        U64 relroEnd  = (r.relro.vaddr + reloc + r.relro.memsz + K64_PAGE_MASK) & ~K64_PAGE_MASK;
        klog_fmt("loadProgram64: deferring PT_GNU_RELRO 0x%llx..0x%llx to PT_INTERP dynamic linker",
                 (unsigned long long)relroAddr,
                 (unsigned long long)relroEnd);
    }

    *rip = r.entry + reloc;
    klog_fmt("loadProgram64: exe RIP=0x%llx (pages mapped: %llu)",
             (unsigned long long)*rip,
             (unsigned long long)mem->mappedPageCount());

    // Populate process bookkeeping. ld-linux reads phdr via AT_PHDR; brk
    // syscall reads brkEnd64.
    process->entry64 = r.entry + reloc;
    if (!r.phdrPresent || r.phdrVaddr > UINT64_MAX - reloc) {
        klog("loadProgram64: program-header table is not present in the loaded image");
        return false;
    }
    process->phdr64 = r.phdrVaddr + reloc;
    process->phnum64 = r.phnum;
    process->phentsize64 = r.phentsize;
    const U64 brkAlignment = mem->nativeIdentityMode()
        ? K64_NATIVE_GUEST_LAYOUT_ALIGN
        : K64_PAGE_SIZE;
    process->brkEnd64 = (r.baseAddrHigh + reloc + brkAlignment - 1) &
                        ~(brkAlignment - 1);
    klog_fmt("loadProgram64: initial brk=0x%llx mmap base=0x%llx",
             (unsigned long long)process->brkEnd64,
             (unsigned long long)(mem->nativeIdentityMode()
                 ? K64_NATIVE_GUEST_MMAP_BASE
                 : 0));
    process->is64Bit = true;

    // ---- PT_INTERP: recursively load the dynamic linker. ----
    //
    // For a dynamically-linked binary the kernel transfers control to the
    // interpreter (typically /lib64/ld-linux-x86-64.so.2), not the binary's
    // own entry. AT_BASE in the auxv tells the interpreter its own load
    // base; AT_ENTRY tells it where the real program entry is so it can
    // jump there after resolving relocations.
    U64 interpBase = 0;
    bool haveInterp = false;
    if (r.interpreter.length()) {
        FsOpenNode* interpNode = openGuestPath(r.interpreter);
        if (!interpNode) {
            klog_fmt("loadProgram64: PT_INTERP '%s' not found in guest rootfs",
                     r.interpreter.c_str());
            // Fail loudly — a dynamic binary without its loader will crash
            // on the first PLT call.
            return false;
        }
        Elf64ParseResult interpR = parse(interpNode);
        if (!interpR.ok) {
            klog_fmt("loadProgram64: PT_INTERP '%s' parse failed", r.interpreter.c_str());
            interpNode->close();
            delete interpNode;
            return false;
        }
        // Pick a base well away from the exe and stack. ld-linux is ET_DYN
        // and small (~200 KiB), so any free high address is fine.
        interpBase = mem->nativeIdentityMode()
            ? K64_NATIVE_GUEST_INTERP_BASE
            : (0x7FFFF7FCE000ULL & ~K64_PAGE_MASK);
        if (!mapSegments(mem, interpNode, interpR, interpBase, "interp",
                         (int)process->id, r.interpreter.c_str())) {
            interpNode->close();
            delete interpNode;
            return false;
        }
        // ld-linux is itself a PIE shared object — it needs its own
        // R_X86_64_RELATIVE entries fixed up before it can run. (Without
        // this, _dl_start crashes on the first indirect call through its
        // own GOT.)
        applyRelativeRelocations(mem, interpR.dynamic, interpBase, "interp");
        if (normalizeFEX64InterpreterMemset(mem, interpR, interpBase) ==
            FEX64InterpreterNormalization::Failed) {
            interpNode->close();
            delete interpNode;
            return false;
        }
        if (normalizeFEX64InterpreterHandoff(
                mem, interpR, interpBase, process->entry64) ==
            FEX64InterpreterNormalization::Failed) {
            interpNode->close();
            delete interpNode;
            return false;
        }
        // Control transfers to the interpreter, not the exe.
        *rip = interpR.entry + interpBase;
        klog_fmt("loadProgram64: interp '%s' mapped at base 0x%llx, RIP=0x%llx",
                 r.interpreter.c_str(),
                 (unsigned long long)interpBase,
                 (unsigned long long)*rip);
        interpNode->close();
        delete interpNode;
        haveInterp = true;
    }

    // -------------------------------------------------------------------
    // Build the initial user-mode stack.
    //
    // System V x86-64 init stack layout, top-down:
    //   <strings>           argv + envp string pool
    //   <padding to 16B>
    //   auxv [terminated by AT_NULL=0]
    //   envp [terminated by NULL]
    //   argv [terminated by NULL]
    //   argc (8 bytes)      ← RSP points here on entry
    //
    // The ABI requires (RSP & 0xF) == 0 at the *call* to _start, which
    // means RSP must be 16-byte aligned when _start begins. _start expects
    // argc at [RSP], argv at [RSP+8], etc.
    // -------------------------------------------------------------------
    // A normal x86-64 Linux process puts its stack near the top of the
    // canonical 47-bit user range.  FEX's iOS backend directly dereferences
    // guest addresses, however, and an iOS process only has the lower host VA
    // window available for arbitrary fixed mappings.  The proven iOS FEX
    // layout keeps its host-only allocator furniture below the proven native
    // guest window, so place the BoxedWine guest stack inside that window.
    //
    // The sparse/interpreter stack used to sit at 0x7FFFFFFFE000, one page
    // below the canonical top, which is where Linux puts it. Ours differs
    // from Linux in a way that matters: the whole 8 MiB is mapped at load
    // time, where Linux maps a few pages and grows on demand. Eight mapped
    // MiB there land inside the range ntdll reserves for its top-down arena,
    // [0x7ffffe000000, 0x7fffffff0000), and reserve_area answers a refusal by
    // halving rather than failing -- so every interpreted Wine process
    // (wineserver, wineboot, services.exe, explorer) silently ran with 24 MiB
    // of the 32 MiB it asked for. A device log shows it exactly: one
    // MAP_FIXED_NOREPLACE for 0x7ffffe000000+0x1ff0000 refused with EEXIST,
    // then 0xff0000 and 0x800000 granted, then sixteen further probes down to
    // 0x10000 all refused, per process.
    //
    // The stack goes below the arena instead. Nothing else of ours is up
    // there: the interpreter base and the TLS block sit near 0x7FFFF7800000,
    // 88 MiB lower, and the guest keeps a conventional high stack address.
    const U64 SPARSE_STACK_TOP = 0x7FFFFDFFE000ULL;
    const U64 STACK_TOP = mem->nativeIdentityMode()
        ? K64_NATIVE_GUEST_STACK_TOP
        : SPARSE_STACK_TOP;
    const U64 STACK_SIZE  = 8ULL * 1024 * 1024;
    // The reason the constant is what it is, checked where it is written: the
    // stack top, which is the highest address the stack ever occupies, is at
    // or below the base of the arena, so no page of it is inside.
    static_assert(SPARSE_STACK_TOP <= boxedvn::kWineArenaTopDownBase,
                  "the sparse guest stack must lie entirely below the "
                  "top-down arena ntdll reserves for itself");
    const U64 STACK_BASE  = STACK_TOP - STACK_SIZE;
    U64 mapped = mem->mmapAnonymousFixed(STACK_BASE, STACK_SIZE,
                                         K_PROT_READ | K_PROT_WRITE);
    if (mapped != STACK_BASE) {
        klog_fmt("loadProgram64: stack mmap failed (got 0x%llx)", (unsigned long long)mapped);
        return false;
    }

    // Use the real argv/envp that KProcess::startProcess stashed for us. When
    // they're empty (the bare --x64-run-elf test harness calls loadProgram
    // directly, not through startProcess) fall back to a single-element argv
    // so _start/_dl_start still validate.
    std::vector<BString> argv = process->startupArgs64;
    if (argv.empty()) {
        const char* exeName = process->name.length() ? process->name.c_str() : "boxedwine64.exe";
        argv.push_back(BString::copy(exeName));
    }
    std::vector<BString> envp = process->startupEnv64;
    /* One line naming the display this process will look for.
     *
     * Wine's X11 user driver is loaded by win32u and initialises by opening
     * DISPLAY. If the variable does not survive into the x86-64 process the
     * driver cannot connect, the process ends up with no desktop window, and
     * every CreateWindowEx returns ERROR_INVALID_WINDOW_HANDLE -- which is
     * what a device run showed, with nothing in the log to say whether the
     * variable was there at all. Bounded: the first few processes only, which
     * is where the answer is. */
    {
        static std::atomic<unsigned> displayReports {0};
        if (displayReports.fetch_add(1, std::memory_order_relaxed) < 8) {
            const char* displayValue = nullptr;
            for (const BString& entry : envp) {
                if (entry.startsWith("DISPLAY=")) {
                    displayValue = entry.c_str() + 8;
                    break;
                }
            }
            klog_fmt("BOXEDWINE_X64_GUEST_DISPLAY pid=%u display='%s' env=%u",
                     (unsigned)process->id,
                     displayValue ? displayValue : "(unset)",
                     (unsigned)envp.size());
        }
    }
    if (envp.empty()) {
        envp.push_back(BString::copy("PATH=/bin:/usr/bin"));
    }

    // First write all the strings near the top of stack, recording each
    // string's guest address.
    U64 sp = STACK_TOP - 16;
    std::vector<U64> argvPtrs;
    std::vector<U64> envpPtrs;
    for (auto it = envp.rbegin(); it != envp.rend(); ++it) {
        U64 len = it->length() + 1;
        sp -= len;
        mem->memcpyToGuest(sp, it->c_str(), len);
        envpPtrs.insert(envpPtrs.begin(), sp);
    }
    for (auto it = argv.rbegin(); it != argv.rend(); ++it) {
        U64 len = it->length() + 1;
        sp -= len;
        mem->memcpyToGuest(sp, it->c_str(), len);
        argvPtrs.insert(argvPtrs.begin(), sp);
    }

    static constexpr char platformName[] = "x86_64";
    sp -= sizeof(platformName);
    const U64 platformAddr = sp;
    mem->memcpyToGuest(platformAddr, platformName, sizeof(platformName));

    // Linux supplies 16 bytes of process entropy through AT_RANDOM. glibc uses
    // them for stack and pointer guards before main(), so advertising a
    // zero-filled region violates the ELF process-start contract.
    sp -= 16;
    const U64 randomAddr = sp;
    std::array<U8, 16> startupRandom{};
    const KRandom::Source randomSource =
        KRandom::fill(startupRandom.data(), startupRandom.size());
    if (randomSource == KRandom::Source::Invalid) {
        klog("loadProgram64: could not construct AT_RANDOM data");
        return false;
    }
    mem->memcpyToGuest(randomAddr, startupRandom.data(), startupRandom.size());
    klog_fmt("loadProgram64: startup auxv random=%s platform=%s",
             randomSource == KRandom::Source::System ? "system" : "fallback",
             platformName);

    // Align sp down to 16B. The vector below pushes pairs of qwords, so
    // we need (sp - totalSize) to land on a 16-byte boundary.
    auto pushQ = [&](U64 v) {
        sp -= 8;
        mem->writeq(sp, v);
    };

    // Compute auxv. Minimal viable set for glibc / ld-linux:
    enum {
        AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5,
        AT_PAGESZ = 6, AT_BASE = 7, AT_ENTRY = 9, AT_UID = 11,
        AT_EUID = 12, AT_GID = 13, AT_EGID = 14, AT_RANDOM = 25,
        AT_HWCAP = 16, AT_CLKTCK = 17, AT_PLATFORM = 15, AT_SECURE = 23,
        AT_EXECFN = 31,
    };
    // AT_EXECFN: guest address of the executable's pathname (argv[0]'s string).
    // glibc exposes it and wine's loader derives its bindir/loader/module paths
    // from it (via __progname / get_argv0). Without it wine falls back to a
    // bogus "/proc"-derived path and fails to exec the wine loader.
    U64 execfnAddr = argvPtrs.empty() ? 0 : argvPtrs[0];
    struct Aux { U64 k, v; };
    std::vector<Aux> aux = {
        { AT_PHDR,    process->phdr64 },
        { AT_PHENT,   process->phentsize64 },
        { AT_PHNUM,   process->phnum64 },
        { AT_PAGESZ,  K64_PAGE_SIZE },
        { AT_BASE,    haveInterp ? interpBase : 0 },
        { AT_ENTRY,   process->entry64 },
        { AT_UID,     process->userId },
        { AT_EUID,    process->effectiveUserId },
        { AT_GID,     process->groupId },
        { AT_EGID,    process->effectiveGroupId },
        { AT_PLATFORM, platformAddr },
        { AT_RANDOM,  randomAddr },
        { AT_HWCAP,   0 },
        { AT_CLKTCK,  100 },
        { AT_SECURE,  0 },
        { AT_EXECFN,  execfnAddr },
        { AT_NULL,    0 },
    };

    // Total qwords that go onto the stack:
    //   argc(1) + argvPtrs + NULL + envpPtrs + NULL + 2*aux
    U64 totalQ = 1 + argvPtrs.size() + 1 + envpPtrs.size() + 1 + 2 * aux.size();
    // After pushing totalQ*8 bytes from sp, the new sp must be 16-aligned.
    U64 afterSize = totalQ * 8;
    U64 targetSp = (sp - afterSize) & ~0xFULL;
    sp = targetSp + afterSize;

    // Push from top of frame down to argc — meaning we walk in reverse.
    for (auto it = aux.rbegin(); it != aux.rend(); ++it) {
        pushQ(it->v);
        pushQ(it->k);
    }
    pushQ(0); // envp terminator
    for (auto it = envpPtrs.rbegin(); it != envpPtrs.rend(); ++it) pushQ(*it);
    pushQ(0); // argv terminator
    for (auto it = argvPtrs.rbegin(); it != argvPtrs.rend(); ++it) pushQ(*it);
    pushQ((U64)argv.size()); // argc

    // sp now points at argc; this is what RSP should be at _start.
    klog_fmt("loadProgram64: stack built, RSP=0x%llx argc=%u envc=%u auxc=%u",
             (unsigned long long)sp, (U32)argv.size(), (U32)envp.size(), (U32)aux.size());

    // -------------------------------------------------------------------
    // Install the initial-thread TLS block.
    //
    // setupStaticTls already exists (and is unit-tested) but until now the
    // loader only *parsed* PT_TLS — the block was never created and the
    // CPU's FS base stayed 0. That meant any ld-linux that touches its
    // FS-relative errno slot (which it does almost immediately after rseq
    // init) read random memory at offset zero.
    //
    // Place the TLS block in the gap between stack-top-minus-8MB and the
    // interpreter — a 1 MiB block below the native stack is well clear of
    // both. Skip this step when no PT_TLS segment is present (static
    // hello-world ELFs don't have one).
    // -------------------------------------------------------------------
    U64 fsBaseToSet = 0;
    if (r.tls.present && r.tls.memsz) {
        const U64 TLS_BLOCK_BASE = mem->nativeIdentityMode()
            ? K64_NATIVE_GUEST_TLS_BASE
            : 0x7FFFF7800000ULL;
        const U64 TLS_BLOCK_SIZE = 0x100000ULL; // 1 MiB — far more than enough
        U64 tlsMapped = mem->mmapAnonymousFixed(TLS_BLOCK_BASE, TLS_BLOCK_SIZE,
                                                K_PROT_READ | K_PROT_WRITE);
        if (tlsMapped != TLS_BLOCK_BASE) {
            klog_fmt("loadProgram64: TLS block mmap failed (got 0x%llx)",
                     (unsigned long long)tlsMapped);
            return false;
        }
        // PT_TLS image lives at r.tls.vaddr in the exe — adjust for the PIE
        // relocation offset, since the segment was loaded at vaddr+reloc.
        U64 imageBase = r.tls.vaddr + reloc;
        fsBaseToSet = setupStaticTls(mem, r.tls, imageBase, TLS_BLOCK_BASE);
        klog_fmt("loadProgram64: PT_TLS installed at 0x%llx, fsBase=0x%llx",
                 (unsigned long long)TLS_BLOCK_BASE,
                 (unsigned long long)fsBaseToSet);
    }

    // Create the CPU64 and seed RIP/RSP. The thread's eip field stays at 0
    // because ELF64 processes don't use the 32-bit CPU — the schedule path
    // (when we add it) must branch on KProcess::is64Bit to pick which
    // run-loop to invoke.
    if (!process->cpu64) {
        process->cpu64 = new CPU64(mem);
    }
    process->cpu64->thread = thread;
    // The main thread's per-thread CPU64 is the same instance as the
    // process-wide one. clone64 allocates a distinct CPU64 (sharing memory64)
    // for each additional thread, and the scheduler drives thread->cpu64.
    thread->cpu64 = process->cpu64;
    // When an interpreter is present, *rip points at the interpreter entry,
    // not the executable entry — that's the correct first instruction.
    process->cpu64->rip = *rip;
    process->cpu64->reg[X64_RSP].setU64(sp);
    // System V calls _start with RDX = pointer to a function to register
    // with atexit, or 0 if none. Linux passes 0.
    process->cpu64->reg[X64_RDX].setU64(0);
    if (fsBaseToSet) {
        process->cpu64->fsbase = fsBaseToSet;
    }

    return true;
}
#endif
