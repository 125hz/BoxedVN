#!/usr/bin/env python3
# BoxedVN - patch the pinned ARM64EC emulator DLL in place.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/patch-xtajit64.py <input.dll> <output.dll>
#
# The emulator's allocator ships as a prebuilt binary whose source revision was
# force-pushed out of existence, so it cannot be rebuilt.  This applies in-place
# instruction patches to it.  Nothing is resized, relinked or relocated: every
# existing RVA keeps its address, which is what lets the ARM64EC recovery
# machinery in the Wine unix side keep working against the same offsets.
#
# The defect
# ----------
# Three deallocation paths push a freed block onto a page's free list without
# checking that the pointer is a block of that page at all:
#
#   block_deallocate            0x149b3c  str x8,  [x2]
#   span_deallocate_block       0x14a810  str x9,  [x2]
#   page_put_thread_free_block  0x14b234  str x10, [x1]
#
# Each derives the page by masking the freed pointer, so a page base masks to
# itself and the store lands on that page's own {size_class, block_size}.
# Blocks begin at page + PAGE_HEADER_SIZE (128), so a page base is never a legal
# block and none of these stores is ever correct for one.
#
# The device proved the third one.  page_put_thread_free_block computes
#
#   0x14b218  madd w12, block_size, thread_free_index, #0x80
#   0x14b22c  add  x10, page, x12
#   0x14b234  str  x10, [x1]
#
# and with block_size 32, index 1916 and x1 equal to the page base that is
# exactly *(0x7005010000) = 0x700501f000, the observed corruption, byte for
# byte.  It is the cross-thread path, which is the one taken here because x18
# does not match the owning heap during early startup.
#
# Downstream, page_initialize_blocks multiplies by the corrupted block_size and
# hands out block addresses far outside the page, threading free lists through
# unrelated memory -- which is what overwrites the live emulator context and
# produces every bogus virtual dispatch recovered so far.
#
# The patch
# ---------
# At each of the three entry points, one instruction becomes a branch into
# padding inside .text where the displaced instruction is re-issued alongside a
# check that the freed pointer is not the derived page base.  If it is, the free
# is dropped.  Dropping cannot discard a valid deallocation, because a page base
# is not a block address in any size class.  It leaks at most one allocation
# from a caller that is already wrong, in exchange for not corrupting the
# descriptor.
#
# This is a stopgap for the prebuilt.  The real repair belongs in the allocator
# source, once there is a build of it to repair.

import hashlib
import struct
import sys

# The exact prebuilt these patches were derived against.
EXPECTED_INPUT_SHA256 = "cd10135284910048152d9ada3312053f11270d0e606e8f62a3e7b62d2e8ef591"

CAVE_BASE = 0x2041A4   # zero padding inside .text VirtualSize
CAVE_END = 0x204200    # last word this patch may use

RET = 0xD65F03C0


def enc_b(pc, target):
    return 0x14000000 | (((target - pc) >> 2) & 0x03FFFFFF)


def enc_bcond(pc, target, cond):
    return 0x54000000 | ((((target - pc) >> 2) & 0x7FFFF) << 5) | cond


def enc_tbnz_w(pc, target, bit, rt):
    return 0x37000000 | ((bit & 0x1F) << 19) | ((((target - pc) >> 2) & 0x3FFF) << 5) | rt


EQ, NE = 0, 1

C1, C2, C3 = 0x2041A4, 0x2041C0, 0x2041E0

# name, site RVA, original word at the site, [(rva, word, text), ...]
PATCHES = [
    (
        "block_deallocate", 0x149B28, 0x37280188,
        [
            (0x149B28, enc_b(0x149B28, C1), "b stub"),
            (C1 + 0, 0xEB01005F, "cmp x2, x1"),
            (C1 + 4, enc_bcond(C1 + 4, C1 + 20, EQ), "b.eq drop"),
            (C1 + 8, enc_tbnz_w(C1 + 8, C1 + 16, 5, 8), "tbnz w8,#5,slow"),
            (C1 + 12, enc_b(C1 + 12, 0x149B2C), "b resume"),
            (C1 + 16, enc_b(C1 + 16, 0x149B58), "b slow"),
            (C1 + 20, RET, "ret"),
        ],
    ),
    (
        "span_deallocate_block", 0x14A7FC, 0x54000381,
        [
            (0x14A7FC, enc_b(0x14A7FC, C2), "b stub"),
            (C2 + 0, 0xEB01005F, "cmp x2, x1"),
            (C2 + 4, enc_bcond(C2 + 4, C2 + 24, EQ), "b.eq drop"),
            (C2 + 8, 0xEB12013F, "cmp x9, x18"),
            (C2 + 12, enc_bcond(C2 + 12, C2 + 20, NE), "b.ne cross"),
            (C2 + 16, enc_b(C2 + 16, 0x14A800), "b resume"),
            (C2 + 20, enc_b(C2 + 20, 0x14A86C), "b cross"),
            (C2 + 24, 0xF84107FE, "ldr x30,[sp],#0x10"),
            (C2 + 28, RET, "ret"),
        ],
    ),
    (
        "page_put_thread_free_block", 0x14B1F4, 0xD50339BF,
        [
            (0x14B1F4, enc_b(0x14B1F4, C3), "b stub"),
            (C3 + 0, 0xEB00003F, "cmp x1, x0"),
            (C3 + 4, enc_bcond(C3 + 4, C3 + 16, EQ), "b.eq drop"),
            (C3 + 8, 0xD50339BF, "dmb ishld"),
            (C3 + 12, enc_b(C3 + 12, 0x14B1F8), "b resume"),
            (C3 + 16, 0xF94013FE, "ldr x30,[sp,#0x20]"),
            (C3 + 20, 0xA9415BF5, "ldp x21,x22,[sp,#0x10]"),
            (C3 + 24, 0xA8C353F3, "ldp x19,x20,[sp],#0x30"),
            (C3 + 28, RET, "ret"),
        ],
    ),
]


def section_table(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 0x14)[0]
    base = pe + 0x18 + optsz
    out = []
    for i in range(nsec):
        o = base + i * 40
        name = data[o:o + 8].rstrip(b"\0").decode("latin1")
        vsz, va, rsz, rptr = struct.unpack_from("<IIII", data, o + 8)
        out.append((name, va, vsz, rptr, rsz))
    return out


def rva_to_off(sections, rva):
    for name, va, vsz, rptr, rsz in sections:
        if va <= rva < va + max(vsz, rsz):
            return rptr + (rva - va)
    raise SystemExit("RVA %#x is not inside any section" % rva)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: patch-xtajit64.py <input.dll> <output.dll>")
    src, dst = sys.argv[1], sys.argv[2]

    data = bytearray(open(src, "rb").read())
    got = hashlib.sha256(data).hexdigest()
    if got != EXPECTED_INPUT_SHA256:
        raise SystemExit(
            "refusing to patch: unexpected input\n"
            "  expected sha256 %s\n"
            "  got            %s\n"
            "The prebuilt changed; re-derive the patches before updating the hash."
            % (EXPECTED_INPUT_SHA256, got))

    sections = section_table(data)

    # Every site must still hold the instruction the stub displaces, and every
    # cave word this patch writes must still be padding.
    for name, site, original, words in PATCHES:
        off = rva_to_off(sections, site)
        now = struct.unpack_from("<I", data, off)[0]
        if now != original:
            raise SystemExit(
                "refusing to patch: %s site %#x holds %08x, expected %08x"
                % (name, site, now, original))
    for name, site, original, words in PATCHES:
        for rva, word, text in words:
            if rva == site:
                continue
            if not (CAVE_BASE <= rva < CAVE_END):
                raise SystemExit("refusing to patch: %#x is outside the cave" % rva)
            off = rva_to_off(sections, rva)
            if struct.unpack_from("<I", data, off)[0] != 0:
                raise SystemExit(
                    "refusing to patch: cave word %#x is not padding" % rva)

    print("badfree-v2: refuse a free whose pointer is the page descriptor address")
    for name, site, original, words in PATCHES:
        print("  %s" % name)
        for rva, word, text in words:
            off = rva_to_off(sections, rva)
            before = struct.unpack_from("<I", data, off)[0]
            struct.pack_into("<I", data, off, word)
            print("    %#010x  %08x -> %08x   %s" % (rva, before, word, text))

    open(dst, "wb").write(bytes(data))
    print("  input  sha256 %s" % got)
    print("  output sha256 %s" % hashlib.sha256(data).hexdigest())


if __name__ == "__main__":
    main()
