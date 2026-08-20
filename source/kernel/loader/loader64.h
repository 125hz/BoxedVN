/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef __LOADER64_H__
#define __LOADER64_H__

#include "kelf64.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>

class FsOpenNode;
class KThread;

// Parsed view of one PT_LOAD segment. All addresses are 64-bit guest
// virtual; offset/sizes are file offsets. Held only long enough for the
// mapper to act on.
struct Elf64LoadSegment {
    U64 vaddr;
    U64 memsz;
    U64 filesz;
    U64 offset;
    U32 flags; // p_flags: PF_R/PF_W/PF_X (1/2/4)
    U64 align;
};

// Captured PT_DYNAMIC location for later relocation processing. vaddr/memsz
// are the unrelocated values straight out of the phdr; the loader adds the
// load slide when reading the dynamic array from guest memory.
struct Elf64DynamicInfo {
    bool present = false;
    U64 vaddr = 0;
    U64 memsz = 0;
};

// Captured PT_TLS template info (used to size and copy per-thread TLS blocks).
struct Elf64TlsInfo {
    bool present = false;
    U64 vaddr = 0;
    U64 filesz = 0;
    U64 memsz = 0;
    U64 align = 0;
};

// Captured PT_GNU_RELRO region — the loader marks these pages read-only
// AFTER relocations have been applied, matching the contract ld.so
// honors at runtime. Without enforcement a guest write to .got.plt
// silently succeeds; with it the page flags reflect read-only protection.
//
// Note: actual fault delivery on a write to a non-writable page is a
// separate gap — KMemory64::memcpy* / writeb still bypass flag checks
// today. RELRO here is recorded correctly so when fault enforcement
// lands, RELRO will work automatically without re-touching the loader.
struct Elf64RelroInfo {
    bool present = false;
    U64 vaddr = 0;
    U64 memsz = 0;
};

struct Elf64ParseResult {
    bool ok = false;
    U64 entry = 0;            // e_entry
    U64 phoff = 0;            // e_phoff (file offset of phdr table)
    U16 phentsize = 0;
    U16 phnum = 0;
    U64 baseAddrLow = 0;      // lowest p_vaddr across PT_LOAD
    U64 baseAddrHigh = 0;     // highest p_vaddr + p_memsz across PT_LOAD
    bool isPie = false;       // e_type == ET_DYN, needs relocation
    std::vector<Elf64LoadSegment> segments;
    BString interpreter;      // PT_INTERP contents (empty if none)
    Elf64DynamicInfo dynamic; // PT_DYNAMIC vaddr/memsz (empty if none)
    Elf64TlsInfo tls;         // PT_TLS template info (empty if none)
    Elf64RelroInfo relro;     // PT_GNU_RELRO region (empty if none)
    // PT_GNU_STACK p_flags: bit 0x1 = exec-stack requested. Most modern
    // binaries omit PT_GNU_STACK or emit it with p_flags=RW (no exec) —
    // recording the value lets us refuse to map an executable stack later
    // if we ever care to enforce NX on the stack region. v1 maps the
    // stack RWX unconditionally; the field is informational.
    bool gnuStackPresent = false;
    bool gnuStackExec = false;
};

class ElfLoader64 {
public:
    // Pure-parse: reads the Ehdr + Phdrs and returns a structured view.
    // Does NOT touch guest memory or thread state. Safe to call without
    // any 64-bit CPU/memory subsystem present.
    static Elf64ParseResult parse(FsOpenNode* openNode);

    // Same as parse(FsOpenNode*) but operates on a contiguous in-memory
    // buffer. Used by self-tests with synthesized ELF blobs (no FS
    // round-trip), and internally by parse() after slurping the file.
    static Elf64ParseResult parseBuffer(const U8* data, U64 length);

#ifdef BOXEDWINE_GUEST_X64
    // Maps PT_LOAD segments into the thread's guest memory, sets RIP,
    // populates KProcess phdr/phentsize/phnum/loaderBaseAddress/brkEnd/entry.
    // Requires the thread's memory to be a 64-bit-capable KMemory.
    // Returns false if the binary cannot be loaded.
    //
    // NOT YET WIRED — depends on KMemory64 (Phase 1.5) and CPU64 (Phase 2).
    static bool loadProgram(KThread* thread, FsOpenNode* openNode, U64* rip);

    // Apply R_X86_64_RELATIVE relocations against the dynamic section at
    // dyn.vaddr+reloc. Public so self-tests can exercise the relocation
    // pass without an FS round-trip. Returns the number of RELATIVE
    // relocations applied; symbol-bound types (GLOB_DAT/JUMP_SLOT/64/COPY)
    // are skipped — those are Milestone A3's job.
    static U64 applyRelativeRelocations(class KMemory64* mem,
                                        const Elf64DynamicInfo& dyn,
                                        U64 reloc,
                                        const char* tag);

    // Apply symbol-bound relocations (R_X86_64_GLOB_DAT, JUMP_SLOT, 64) against
    // the dynamic section at dyn.vaddr+reloc. Symbol names are resolved against
    // a flat caller-supplied table — for the full DT_NEEDED recursion path this
    // would be the merged symbol table across all loaded DSOs; for self-tests
    // the caller injects a small synthetic table.
    //
    // Result counts:
    //   resolved   = number of relocations whose symbol was found and written
    //   unresolved = symbol not in the table (caller decides whether to fail
    //                or defer to ld-linux)
    // Returns resolved + unresolved; RELATIVE/NONE entries are silently
    // ignored (they're handled by applyRelativeRelocations).
    //
    // Walks BOTH the DT_RELA table AND the DT_JMPREL (PLT) table — on x86-64
    // both are RELA-shaped and both can contain symbol-bound entries.
    //
    // Public so self-tests can exercise resolution without an FS round-trip.
    static U64 applySymbolRelocations(class KMemory64* mem,
                                      const Elf64DynamicInfo& dyn,
                                      U64 reloc,
                                      const std::unordered_map<std::string, U64>& symbols,
                                      const char* tag,
                                      U64* outResolved = nullptr,
                                      U64* outUnresolved = nullptr);

    // Extract DT_NEEDED entries from PT_DYNAMIC and resolve each one to a
    // shared-object name via DT_STRTAB. Returns library names in their
    // original PT_DYNAMIC order (which matters for symbol-resolution
    // ordering once DT_NEEDED recursion is wired). Returns an empty vector
    // if there's no PT_DYNAMIC, no DT_STRTAB, or no DT_NEEDED entries.
    //
    // Pure parser — does NOT open files, does NOT recurse. The file-loading
    // and recursive parse pass (real Milestone A3) consumes this list and
    // is gated on the rootfs containing libc.so.6.
    //
    // Public so self-tests can exercise the parse path against synthetic
    // dynamic-array bytes without an FS round-trip.
    static std::vector<std::string> extractNeededLibraries(class KMemory64* mem,
                                                          const Elf64DynamicInfo& dyn,
                                                          U64 reloc);

    // Walk DT_SYMTAB / DT_STRTAB and collect every defined global symbol
    // (st_shndx != 0 and binding STB_GLOBAL or STB_WEAK), keyed by name
    // and valued at (st_value + reloc).
    //
    // The symbol table's length is bounded by `(strtab - symtab) / syment`
    // because in every observed layout (gcc, ld, llvm-link, hand-built)
    // STRTAB sits immediately after SYMTAB. If a future binary violates
    // that we'll need DT_HASH / DT_GNU_HASH chain-following, but every
    // ELF we'll see in v1 honors the convention.
    //
    // Public so self-tests can exercise symbol extraction without an FS
    // round-trip. Used by the DT_NEEDED orchestrator to build the merged
    // symbol table that applySymbolRelocations consumes.
    static std::unordered_map<std::string, U64>
        extractGlobalSymbols(class KMemory64* mem,
                             const Elf64DynamicInfo& dyn,
                             U64 reloc);

    // One pre-resolved dependency for linkSharedObjects.
    struct PreloadedLibrary {
        std::string name;
        const U8* buffer;
        U64 length;
        U64 desiredReloc; // 0 = auto-assign sequentially.
    };

    // Result of linking one library through the pipeline.
    struct LinkedLibrary {
        std::string name;
        U64 reloc;
        Elf64ParseResult parsed;
    };

    // Eager DT_NEEDED orchestrator (single-level — does NOT transitively
    // walk DT_NEEDED inside each preloaded lib; the caller pre-flattens).
    //
    // For each lib in `preloaded`:
    //   1. Parse + mapSegmentsFromBuffer at the assigned reloc
    //   2. applyRelativeRelocations on its PT_DYNAMIC
    //   3. extractGlobalSymbols → merge into the symbol union
    // Then:
    //   4. applySymbolRelocations on the main exe AND every lib, against
    //      the merged symbol table.
    //
    // Main exe must already be mapped and RELATIVE-relocated by the caller.
    // `firstLibBase` is the address at which to place the first lib; each
    // subsequent lib gets bumped by a 16 MiB step (way more than any DSO
    // we'll see) unless its desiredReloc is non-zero.
    //
    // Returns the number of libs successfully linked; populates outLibs
    // with their parse results + assigned relocs (for the caller to
    // re-discover entry, TLS, etc).
    static U64 linkSharedObjects(class KMemory64* mem,
                                 const Elf64ParseResult& mainParsed,
                                 U64 mainReloc,
                                 const std::vector<PreloadedLibrary>& preloaded,
                                 U64 firstLibBase,
                                 std::vector<LinkedLibrary>* outLibs = nullptr);

    // One blob returned by the LibFetcher callback. `bytes` may be empty
    // to signal "not found" (the recursive linker treats that as a soft
    // failure: the symbol becomes unresolved, but linking continues).
    struct FetchedLibrary {
        std::vector<U8> bytes;
    };
    using LibFetcher = std::function<FetchedLibrary(const std::string& name)>;

    // Transitive DT_NEEDED orchestrator. Starts from `mainParsed`'s
    // DT_NEEDED entries, walks each requested library via `fetcher`, then
    // recurses into each library's own DT_NEEDED entries BFS-style. Avoids
    // loading the same library twice (keyed by name).
    //
    // Layout: each loaded library is placed at firstLibBase + N * 16 MiB,
    // matching linkSharedObjects's step. Caller MUST ensure the address
    // region [firstLibBase, firstLibBase + 16MiB * MAX_LIBS) is free.
    //
    // After all libraries are mapped and RELATIVE-relocated, the merged
    // symbol table is built and applySymbolRelocations is invoked on the
    // main exe and every loaded library.
    //
    // Public so self-tests can drive it with an in-memory fetcher; the
    // real production caller (loadProgram64) wires it to openGuestPath.
    //
    // Returns the number of libraries successfully loaded (transitively).
    static U64 linkSharedObjectsRecursive(class KMemory64* mem,
                                          const Elf64ParseResult& mainParsed,
                                          U64 mainReloc,
                                          const LibFetcher& fetcher,
                                          U64 firstLibBase,
                                          std::vector<LinkedLibrary>* outLibs = nullptr);

    // Map every PT_LOAD segment from a contiguous in-memory ELF buffer
    // into guest memory at the given relocation base. Mirrors the file-
    // backed loader path but takes its segment bytes from a buffer.
    //
    // Used by self-tests that synthesize ELF blobs in memory; the file-
    // backed loader path uses an internal helper with the same shape.
    //
    // Returns true on success.
    static bool mapSegmentsFromBuffer(class KMemory64* mem,
                                      const Elf64ParseResult& r,
                                      const U8* buffer,
                                      U64 bufferLength,
                                      U64 reloc,
                                      const char* tag);

    // Allocate and populate a static TLS block from the PT_TLS template.
    // Layout (glibc x86-64): [tls image at negative offsets] [TCB] ← FS.
    // TCB head's first qword is the self-pointer ($fs:0 = $fs).
    //
    //   imageBase = guest address from which the TLS template bytes
    //               (filesz bytes) should be read. For the main exe this
    //               is r.tls.vaddr + reloc.
    //   blockBase = guest address where the new TLS block is mapped. Must
    //               be page-aligned and pre-mapped by the caller (or
    //               picked from a known free region).
    //   Returns the FS base (= TCB address) on success, or 0 on failure.
    //
    // Public so self-tests can exercise template-copy without an FS
    // round-trip. Does NOT touch CPU64::fsbase — caller is responsible
    // for that (typically via arch_prctl(ARCH_SET_FS)).
    static U64 setupStaticTls(class KMemory64* mem,
                              const Elf64TlsInfo& tls,
                              U64 imageBase,
                              U64 blockBase);
#endif
};

#endif
