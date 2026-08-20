/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifndef __kelf64_H__
#define __kelf64_H__

#include "platformBoxedwine.h"
#include "kelf.h" // for k_EI_NIDENT and PACKED

// ELF64 struct definitions per the System V ABI x86-64 supplement.
// Pointer-bearing fields (Addr/Off) widen to 64 bits; field order and
// alignment differ from ELF32 in non-obvious ways — DO NOT just s/U32/U64/
// on the ELF32 structs.

#define k_Elf64_Addr   U64
#define k_Elf64_Half   U16
#define k_Elf64_Off    U64
#define k_Elf64_Sword  S32
#define k_Elf64_Word   U32
#define k_Elf64_Sxword S64
#define k_Elf64_Xword  U64

PACKED(
struct k_Elf64_Ehdr {
    unsigned char  e_ident[k_EI_NIDENT];
    k_Elf64_Half   e_type;
    k_Elf64_Half   e_machine;
    k_Elf64_Word   e_version;
    k_Elf64_Addr   e_entry;
    k_Elf64_Off    e_phoff;
    k_Elf64_Off    e_shoff;
    k_Elf64_Word   e_flags;
    k_Elf64_Half   e_ehsize;
    k_Elf64_Half   e_phentsize;
    k_Elf64_Half   e_phnum;
    k_Elf64_Half   e_shentsize;
    k_Elf64_Half   e_shnum;
    k_Elf64_Half   e_shstrndx;
}
);

PACKED(
struct k_Elf64_Shdr {
    k_Elf64_Word   sh_name;
    k_Elf64_Word   sh_type;
    k_Elf64_Xword  sh_flags;
    k_Elf64_Addr   sh_addr;
    k_Elf64_Off    sh_offset;
    k_Elf64_Xword  sh_size;
    k_Elf64_Word   sh_link;
    k_Elf64_Word   sh_info;
    k_Elf64_Xword  sh_addralign;
    k_Elf64_Xword  sh_entsize;
}
);

// Field order in Phdr is different from ELF32: p_flags moves before p_offset.
PACKED(
struct k_Elf64_Phdr {
    k_Elf64_Word   p_type;
    k_Elf64_Word   p_flags;
    k_Elf64_Off    p_offset;
    k_Elf64_Addr   p_vaddr;
    k_Elf64_Addr   p_paddr;
    k_Elf64_Xword  p_filesz;
    k_Elf64_Xword  p_memsz;
    k_Elf64_Xword  p_align;
}
);

// e_machine value for x86-64 (a.k.a. AMD64). i386 ELFs use EM_386 = 0x03.
#define k_EM_X86_64 0x3E
#define k_EM_386    0x03

// ELFCLASS values in e_ident[4].
#define k_ELFCLASS32 1
#define k_ELFCLASS64 2

// p_type values used by the dynamic linker path.
#define k_PT_NULL    0
#define k_PT_LOAD    1
#define k_PT_DYNAMIC 2
#define k_PT_INTERP  3
#define k_PT_NOTE    4
#define k_PT_PHDR    6
#define k_PT_TLS     7
#define k_PT_GNU_EH_FRAME 0x6474e550
#define k_PT_GNU_STACK    0x6474e551
#define k_PT_GNU_RELRO    0x6474e552

// PT_DYNAMIC entry. Walked as an array terminated by d_tag == DT_NULL.
PACKED(
struct k_Elf64_Dyn {
    k_Elf64_Sxword d_tag;
    union {
        k_Elf64_Xword d_val;
        k_Elf64_Addr  d_ptr;
    } d_un;
}
);

// RELA: relocations with explicit addend (the form x86-64 uses; REL is unused).
PACKED(
struct k_Elf64_Rela {
    k_Elf64_Addr   r_offset;
    k_Elf64_Xword  r_info;
    k_Elf64_Sxword r_addend;
}
);

// Symbol table entry.
PACKED(
struct k_Elf64_Sym {
    k_Elf64_Word   st_name;
    unsigned char  st_info;
    unsigned char  st_other;
    k_Elf64_Half   st_shndx;
    k_Elf64_Addr   st_value;
    k_Elf64_Xword  st_size;
}
);

// r_info packs symbol index in high 32 bits and relocation type in low 32.
#define k_ELF64_R_SYM(i)  ((U32)((i) >> 32))
#define k_ELF64_R_TYPE(i) ((U32)((i) & 0xFFFFFFFFULL))

// DT_* tags we consume during dynamic linking.
#define k_DT_NULL     0
#define k_DT_NEEDED   1
#define k_DT_PLTRELSZ 2
#define k_DT_PLTGOT   3
#define k_DT_HASH     4
#define k_DT_STRTAB   5
#define k_DT_SYMTAB   6
#define k_DT_RELA     7
#define k_DT_RELASZ   8
#define k_DT_RELAENT  9
#define k_DT_STRSZ    10
#define k_DT_SYMENT   11
#define k_DT_INIT     12
#define k_DT_FINI     13
#define k_DT_SONAME   14
#define k_DT_RPATH    15
#define k_DT_PLTREL   20
#define k_DT_JMPREL   23
#define k_DT_INIT_ARRAY    25
#define k_DT_FINI_ARRAY    26
#define k_DT_INIT_ARRAYSZ  27
#define k_DT_FINI_ARRAYSZ  28
#define k_DT_RUNPATH       29
#define k_DT_FLAGS         30

// R_X86_64_* relocation types per the AMD64 ABI.
#define k_R_X86_64_NONE      0
#define k_R_X86_64_64        1
#define k_R_X86_64_PC32      2
#define k_R_X86_64_GOT32     3
#define k_R_X86_64_PLT32     4
#define k_R_X86_64_COPY      5
#define k_R_X86_64_GLOB_DAT  6
#define k_R_X86_64_JUMP_SLOT 7
#define k_R_X86_64_RELATIVE  8
#define k_R_X86_64_GOTPCREL  9
#define k_R_X86_64_32        10
#define k_R_X86_64_32S       11
#define k_R_X86_64_16        12
#define k_R_X86_64_PC16      13
#define k_R_X86_64_8         14
#define k_R_X86_64_PC8       15
#define k_R_X86_64_DTPMOD64  16
#define k_R_X86_64_DTPOFF64  17
#define k_R_X86_64_TPOFF64   18
#define k_R_X86_64_TLSGD     19
#define k_R_X86_64_TLSLD     20
#define k_R_X86_64_DTPOFF32  21
#define k_R_X86_64_GOTTPOFF  22
#define k_R_X86_64_TPOFF32   23
#define k_R_X86_64_IRELATIVE 37

#endif
