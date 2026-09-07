#!/usr/bin/env python3
"""Compile Wine's actual mapping-flag selection and unified-loader decision."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
virtual = (root / "dlls/ntdll/unix/virtual.c").read_text()
start = virtual.index("static NTSTATUS map_file_into_view(")
start = virtual.index("    if (vprot & VPROT_WRITE)", start)
end = virtual.index("    map_size =", start)
policy = virtual[start:end]
loader = (root / "dlls/ntdll/unix/loader.c").read_text()
start = loader.index("char *get_alternate_wineloader( WORD machine )")
end = loader.index("static void preloader_exec", start)
source = r"""
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
typedef unsigned short WORD;
typedef int BOOL;
static int is_win64 = 1;
static WORD current_machine = 0x8664;
static const char *build_dir, *alt_build_dir, *dll_dir = "/wine";
static WORD get_alt_machine(WORD machine) { return machine == 0x8664 ? 0x14c : 0x8664; }
static const char *get_so_dir(WORD machine) { return machine == 0x14c ? "/i386-unix" : "/x86_64-unix"; }
""" + loader[start:end] + r"""
#define VPROT_READ 1
#define VPROT_WRITE 2
#define VPROT_WRITECOPY 4
static int mapping(unsigned vprot, unsigned expected_flags, int expected_prot) {
    int prot = PROT_READ | PROT_WRITE;
    unsigned flags = MAP_FIXED;
""" + policy + r"""
    if (flags != expected_flags || prot != expected_prot) {
        fprintf(stderr, "vprot=%u flags=%u prot=%d expected flags=%u prot=%d\n",
                vprot, flags, prot, expected_flags, expected_prot);
        return 1;
    }
    return 0;
}
int main(void) {
    int failed = mapping(VPROT_READ, MAP_FIXED | MAP_SHARED, PROT_READ);
    failed |= mapping(VPROT_READ | VPROT_WRITE, MAP_FIXED | MAP_SHARED, PROT_READ | PROT_WRITE);
    failed |= mapping(VPROT_READ | VPROT_WRITECOPY, MAP_FIXED | MAP_PRIVATE, PROT_READ | PROT_WRITE);
    setenv("WINEARCH", "wow64", 1);
    if (get_alternate_wineloader(0x14c) || get_alternate_wineloader(0x8664)) failed = 1;
    setenv("WINEARCH", "win64", 1);
    char *alternate = get_alternate_wineloader(0x14c);
    if (!alternate || !strstr(alternate, "/i386-unix/wine")) failed = 1;
    free(alternate);
    return failed;
}
"""
with tempfile.TemporaryDirectory(prefix="boxedvn-wine-sections-") as temporary:
    c = Path(temporary) / "policy.c"
    binary = Path(temporary) / "policy"
    c.write_text(source)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + ["-std=c11", "-Wall", "-Wextra", str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Wine read-only/shared, write-copy/private and unified WoW64 loader checks passed")
