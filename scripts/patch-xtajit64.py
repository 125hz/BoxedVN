#!/usr/bin/env python3
# BoxedVN - patch the pinned ARM64EC emulator DLL in place.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/patch-xtajit64.py <input.dll> <output.dll>
#
# The emulator's allocator ships as a prebuilt binary whose source revision was
# force-pushed out of existence, so it cannot be rebuilt.  This applies one
# in-place instruction patch to it.  Nothing is resized, relinked or relocated:
# every existing RVA keeps its address, which is what lets the ARM64EC recovery
# machinery in the Wine unix side keep working against the same offsets.
#
# The defect
# ----------
# block_deallocate() at RVA 0x149b04 derives the owning page from the freed
# pointer and pushes the block onto that page's local free list:
#
#     and  x8, x0, #0xffffffffff000000   ; span   = block & ~0xFFFFFF (16 MiB)
#     ldr  x8, [x8, #0x50]               ; span->page_address_mask
#     and  x1, x8, x0                    ; page   = block & mask
#     ...
#     str  x2, [x1, #0x20]               ; page->local_free = block
#     str  x8, [x2]                      ; block->next      = old local_free
#
# There is no bounds check.  A page base masks to itself, so freeing one writes
# the old free-list head over the page's own {size_class, block_size}.  Blocks
# start at page + PAGE_HEADER_SIZE (128), so a page base is never a legal block
# and this store is never correct.
#
# On device that produced, for a class-2 page:
#
#     *(0x7005010000) = 0x700501f000     ; the previous local_free, block 1916
#
# turning size_class into 0x0501f000 and block_size into 0x70.  Downstream,
# page_initialize_blocks() multiplies by the corrupted block_size and threads
# free lists straight through unrelated pages, which is what destroys the live
# ContextImpl.
#
# The patch
# ---------
# Route a free whose pointer equals the derived page base away from the fast
# path.  One instruction at 0x149b28 becomes a branch into padding inside
# .text; the displaced generic_free test is re-issued there.
#
#     0x149b28: b     cave
#     cave+0:   cmp   x2, x1            ; freed pointer == derived page base?
#     cave+4:   b.eq  L_drop
#     cave+8:   tbnz  w8, #5, L_slow    ; displaced generic_free test
#     cave+12:  b     0x149b2c          ; resume fast path
#     L_slow:   b     0x149b58          ; original slow path
#     L_drop:   ret                     ; refuse an impossible free
#
# Dropping the free cannot discard a valid deallocation, because a page base is
# not a block address in any size class.  It leaks at most one allocation from a
# caller that is already wrong, in exchange for not corrupting the descriptor.
#
# This is a stopgap for the prebuilt.  The real repair belongs in the allocator
# source, once there is a build of it to repair.

import hashlib
import struct
import sys

# The exact prebuilt this patch was derived against.
EXPECTED_INPUT_SHA256 = "cd10135284910048152d9ada3312053f11270d0e606e8f62a3e7b62d2e8ef591"

PATCH_SITE = 0x149B28  # tbnz w8, #5, #0x149b58   inside block_deallocate
CAVE = 0x2041A4        # zero padding inside .text VirtualSize
CAVE_WORDS = 6

RESUME = 0x149B2C      # fast path, immediately after the displaced test
SLOW = 0x149B58        # original slow/generic free path

ORIGINAL_AT_SITE = 0x37280188  # tbnz w8, #5, #0x149b58

CMP_X2_X1 = 0xEB01005F
RET = 0xD65F03C0


def enc_b(pc, target):
    return 0x14000000 | (((target - pc) >> 2) & 0x03FFFFFF)


def enc_beq(pc, target):
    return 0x54000000 | ((((target - pc) >> 2) & 0x7FFFF) << 5)


def enc_tbnz_w(pc, target, bit, rt):
    return 0x37000000 | ((bit & 0x1F) << 19) | ((((target - pc) >> 2) & 0x3FFF) << 5) | rt


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
            "The prebuilt changed; re-derive the patch before updating the hash."
            % (EXPECTED_INPUT_SHA256, got))

    sections = section_table(data)

    site_off = rva_to_off(sections, PATCH_SITE)
    site_now = struct.unpack_from("<I", data, site_off)[0]
    if site_now != ORIGINAL_AT_SITE:
        raise SystemExit(
            "refusing to patch: %#x holds %08x, expected %08x"
            % (PATCH_SITE, site_now, ORIGINAL_AT_SITE))

    cave_off = rva_to_off(sections, CAVE)
    for i in range(CAVE_WORDS):
        w = struct.unpack_from("<I", data, cave_off + i * 4)[0]
        if w != 0:
            raise SystemExit(
                "refusing to patch: cave %#x+%#x holds %08x, expected padding"
                % (CAVE, i * 4, w))

    l_slow = CAVE + 16
    l_drop = CAVE + 20
    writes = [
        (PATCH_SITE, enc_b(PATCH_SITE, CAVE), "b cave"),
        (CAVE + 0, CMP_X2_X1, "cmp x2, x1"),
        (CAVE + 4, enc_beq(CAVE + 4, l_drop), "b.eq L_drop"),
        (CAVE + 8, enc_tbnz_w(CAVE + 8, l_slow, 5, 8), "tbnz w8, #5, L_slow"),
        (CAVE + 12, enc_b(CAVE + 12, RESUME), "b resume"),
        (l_slow, enc_b(l_slow, SLOW), "b slow"),
        (l_drop, RET, "ret"),
    ]

    print("badfree-v1: refuse block_deallocate() of a page base")
    for rva, word, text in writes:
        off = rva_to_off(sections, rva)
        before = struct.unpack_from("<I", data, off)[0]
        struct.pack_into("<I", data, off, word)
        print("  %#010x  %08x -> %08x   %s" % (rva, before, word, text))

    open(dst, "wb").write(bytes(data))
    print("  input  sha256 %s" % got)
    print("  output sha256 %s" % hashlib.sha256(data).hexdigest())


if __name__ == "__main__":
    main()
