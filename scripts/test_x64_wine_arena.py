"""BoxedVN - contract tests for the address space Wine actually reserves.

Five device logs at one revision all end the same way, and all of them were
read as though the guest were asking for memory outside the lanes we had just
widened:

    0078:err:virtual:allocate_virtual_memory out of memory for allocation,
                                             base (nil) size 48d30000
    0024:err:virtual:map_fixed_area out of memory for
                                             0x6fffffc50000-0x6ffffffeb000

with not one BOXEDWINE_X64_LANE_REFUSED line anywhere, which reads as "we
refuse nothing". Three separate things in that reading are wrong, and this
file is where each of them is nailed down.

1. Where the arena comes from.

   No preloader runs. The packaged wine64 layer carries no wine64-preloader,
   scripts/validate-wine64-runtime.sh never checked for one, and our own
   wine-preloader is a guest link created only when that path is ABSENT,
   pointing at a name the archive does not contain. Every log shows the
   consequence directly: an execve of wine-preloader with argc N followed
   immediately by an execve of the loader with argc N-1 and no
   BOXEDWINE_X64_EXEC line in between -- Wine's loader/main.c
   preloader_exec() falling back to execv(argv[1], argv + 1).

   So wine_main_preload_info is null and ntdll takes mmap_init's no-preloader
   branch (dlls/ntdll/unix/virtual.c, Wine 9):

       if (preload_info) return;
       reserve_area( (void *)0x000000010000, (void *)0x000068000000 );
       reserve_area( (void *)0x00007f000000, (void *)0x00007fff0000 );
       reserve_area( (void *)0x7ffffe000000, (void *)0x7fffffff0000 );

   Those three ranges are the entire arena, and the log confirms the guest
   asks for exactly them, in that order, at the top of every process:

       addr=0x10000       len=0x67ff0000 flags=0x104022 -> ret=0x10000
       addr=0x7f000000    len=0xff0000   flags=0x104022 -> ret=0x7f000000
       addr=0x7ffffe000000 len=0x1ff0000 flags=0x104022 -> ret=0x7ffffe000000

   Nothing else contributes. WINEPRELOADRESERVE is parsed by the preloader
   that never runs, and ntdll uses it only to EXCLUDE a range from its
   searches (find_reserved_free_area_outside_preloader); it never calls
   mmap_add_reserved_area on it. An environment variable therefore cannot
   hand Wine an arena inside our identity lane, and the launch does not try.

2. What 0x6ffff... actually is.

   It is not a top-arena address with a nibble changed. 0x6fffffc50000 is
   ntdll.dll's link-time ImageBase: the interpreted processes in the same
   logs map that exact address FILE-backed, naming
   .../x86_64-windows/ntdll.dll section by section at file offsets 0,
   0x1000, 0x6d000, and every failing range is exactly one module long, packed
   downwards from just under 0x700000000000. The twin 0x7fffffc50000 occurs
   zero times in any of the five logs.

   What is true, and is the reason the refusal is correct rather than a
   missing lane, is that the two-instruction translation cannot tell those two
   addresses apart: a builtin base has SOME clear-mask bits set and not
   others, so (g | low) & ~mask maps 0x6fffffc50000 and 0x7fffffc50000 to the
   same host address, 0x7fffc50000. Admitting the band would alias two guest
   pages onto one host page. The test recomputes that here rather than
   trusting the header.

3. Why the log showed no refusals.

   Because the refusals never reached the witness. The mmap placement policy
   decides an out-of-lane MAP_FIXED_NOREPLACE for itself and returns -ENOMEM
   without ever calling the mapping path that reports lane refusals, so a run
   that rejected 778 of 1025 guest mmaps printed no LANE_REFUSED line at all.

And one thing our own layout got wrong: the sparse/interpreter guest stack
was placed at 0x7FFFFFFFE000 with all 8 MiB mapped at load time, which lands
inside the top-down arena ntdll reserves. reserve_area answers a refusal by
halving rather than failing, so every interpreted Wine process silently ran
with 24 MiB of the 32 MiB it asked for -- visible in the logs as one refused
0x1ff0000 probe, two grants totalling 0x17f0000, and sixteen further refusals
down to 0x10000, per process.

Source-level contracts on purpose: building the emulator needs the iOS
toolchain, and none of this can be run on this host.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ALIAS_HEADER = REPO / "include" / "guest_low_alias.h"
LOADER = REPO / "source" / "kernel" / "loader" / "loader64.cpp"
SYSCALLS = REPO / "source" / "kernel" / "syscall64.cpp"
KMEMORY_HEADER = REPO / "include" / "kmemory64.h"
KMEMORY_SOURCE = REPO / "source" / "kernel" / "kmemory64.cpp"
STARTUP = REPO / "source" / "sdl" / "startupArgs.cpp"

# ntdll's own reservations, from mmap_init's no-preloader branch. These are
# Wine's constants, not ours; they are repeated here so a change to the header
# has to be a deliberate one.
WINE_ARENA = (
    (0x000000010000, 0x000068000000),
    (0x00007F000000, 0x00007FFF0000),
    (0x7FFFFE000000, 0x7FFFFFFF0000),
)

# The lane arithmetic, recomputed independently of the header.
LOW_ALIAS_BASE = 0x7800000000
LOW_LIMIT = 0x200000000
HIGH_BASE = LOW_ALIAS_BASE + LOW_LIMIT
HIGH_END = 0x7F80000000
TOP_BASE = 0x7FFF80000000
TOP_END = 0x7FFFFFFF0000
TOP_CLEAR_MASK = 0x7F8000000000

# ntdll.dll's link-time ImageBase, and the "one nibble changed" twin the log
# was suspected of having truncated.
NTDLL_IMAGE_BASE = 0x6FFFFFC50000
NTDLL_IMAGE_TWIN = NTDLL_IMAGE_BASE | TOP_CLEAR_MASK

WITNESS = "BOXEDWINE_X64_WINE_ARENA"
WITNESS_FIELDS = (
    "pid=%d",
    "tid=%d",
    "space=%llu",
    "range=%s",
    "reserved=[0x%llx,0x%llx)",
    "guest=[0x%llx,0x%llx)",
    "len=0x%llx",
    "prot=0x%x",
    "native=%d",
    "hostable=%d",
    "host=[0x%llx,0x%llx)",
    "placement=%s",
    "ret=0x%llx",
    "granted=%d",
)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def host_address(guest: int) -> int:
    """The translation the ARM64 backend emits, spelled as it emits it."""
    return (guest | LOW_ALIAS_BASE) & ~TOP_CLEAR_MASK


def hostable(address: int, length: int) -> bool:
    end = address + length
    if length == 0:
        return False
    if end <= LOW_LIMIT:
        return True
    if address >= HIGH_BASE and end <= HIGH_END:
        return True
    return address >= TOP_BASE and end <= TOP_END


class TheArenaIsTheThreeRangesNtdllReserves(unittest.TestCase):
    def setUp(self) -> None:
        self.header = read(ALIAS_HEADER)

    def test_the_header_states_wines_own_reservation_call(self) -> None:
        # The literals have to be Wine's, and the branch they come from has to
        # be named, or the next reader has no way to check them.
        self.assertIn("if (preload_info) return;", self.header)
        for base, end in WINE_ARENA:
            self.assertIn("reserve_area( (void *)0x%012x, (void *)0x%012x );"
                          % (base, end), self.header)

    def test_the_ranges_are_declared_and_are_wines(self) -> None:
        table = self.header[self.header.index("kWineArenaRanges[3]"):]
        table = table[:table.index("};")]
        found = [(int(a, 16), int(b, 16)) for a, b in
                 re.findall(r"\{0x([0-9a-fA-F]+)ULL,\s*0x([0-9a-fA-F]+)ULL,",
                            table)]
        self.assertEqual(found, list(WINE_ARENA))

    def test_every_reserved_range_is_one_this_address_space_can_host(self) -> None:
        # Recomputed, not read: this is the property that decides whether Wine
        # gets its arena whole or silently gets a fraction of it.
        for base, end in WINE_ARENA:
            self.assertTrue(hostable(base, end - base),
                            "0x%x-0x%x is not hostable" % (base, end))
        self.assertEqual(
            self.header.count("static_assert(guestRangeHostable(kWineArenaRanges"),
            3)

    def test_a_shrunk_arena_is_not_a_refusal_and_the_header_says_so(self) -> None:
        # reserve_area halves rather than failing, so a range we hand over in
        # part produces no error anywhere -- the guest just has less arena.
        self.assertIn("halves recursively rather than failing", self.header)

    def test_wine_preload_reserve_cannot_supply_an_arena(self) -> None:
        # The one alternative that would have needed no code: rejected in the
        # header, with the reason, so it is not tried again.
        self.assertIn("find_reserved_free_area_outside_preloader", self.header)
        self.assertIn("never adds a reserved area", self.header)

    def test_the_top_down_range_is_named_for_the_loader(self) -> None:
        self.assertIn("kWineArenaTopDownBase = 0x7ffffe000000ULL", self.header)
        self.assertIn("kWineArenaTopDownEnd = 0x7fffffff0000ULL", self.header)

    def test_a_subrange_of_a_reserved_range_is_recognised(self) -> None:
        # Wine halves, so the halves have to be recognised as arena too.
        self.assertIn("int guestWineArenaRangeIndex(", self.header)


class TheBuiltinImageBandIsNotTheArena(unittest.TestCase):
    def setUp(self) -> None:
        self.header = read(ALIAS_HEADER)

    def test_the_failing_addresses_are_module_image_bases(self) -> None:
        self.assertIn("kWineBuiltinNtdllImageBase = 0x6FFFFFC50000ULL",
                      self.header)
        self.assertIn("ntdll.dll's link-time ImageBase", self.header)

    def test_the_band_is_outside_every_lane(self) -> None:
        self.assertFalse(hostable(0x6FFF00000000,
                                  0x700000000000 - 0x6FFF00000000))
        self.assertFalse(hostable(NTDLL_IMAGE_BASE, 0x39B000))
        self.assertIn("!guestRangeHostable(kWineBuiltinImageBandBase",
                      self.header)

    def test_the_image_base_and_its_twin_collide_under_the_translation(self) -> None:
        # The decidable question: is 0x6fffffc50000 a truncated 0x7fffffc50000?
        # No -- but the two would be indistinguishable to the translator, which
        # is why the band cannot simply be admitted as a fourth lane.
        self.assertEqual(NTDLL_IMAGE_TWIN, 0x7FFFFFC50000)
        self.assertEqual(host_address(NTDLL_IMAGE_BASE),
                         host_address(NTDLL_IMAGE_TWIN))
        self.assertEqual(host_address(NTDLL_IMAGE_BASE), 0x7FFFC50000)
        self.assertIn("guestToHostAddress(kWineBuiltinNtdllImageBase) ==",
                      self.header)

    def test_the_twin_would_be_an_arena_address(self) -> None:
        self.assertTrue(TOP_BASE <= NTDLL_IMAGE_TWIN < TOP_END)

    def test_the_header_says_a_fourth_lane_needs_a_new_translation(self) -> None:
        self.assertIn("without touching NZCV", self.header)


class TheSparseStackKeepsOutOfTheArena(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(LOADER)

    def test_the_stack_top_is_named_and_below_the_arena(self) -> None:
        match = re.search(r"const U64 SPARSE_STACK_TOP = 0x([0-9A-Fa-f]+)ULL;",
                          self.source)
        self.assertIsNotNone(match)
        top = int(match.group(1), 16)
        self.assertLessEqual(top, WINE_ARENA[2][0])

    def test_the_whole_stack_clears_the_arena(self) -> None:
        match = re.search(r"const U64 SPARSE_STACK_TOP = 0x([0-9A-Fa-f]+)ULL;",
                          self.source)
        top = int(match.group(1), 16)
        size = 8 * 1024 * 1024
        self.assertNotIn("0x7FFFFFFFE000ULL", self.source)
        # Grows down, so the top is the highest address it ever occupies.
        self.assertTrue(top - size < top <= WINE_ARENA[2][0])

    def test_the_constraint_is_checked_where_the_constant_is_written(self) -> None:
        self.assertIn("static_assert(SPARSE_STACK_TOP <= "
                      "boxedvn::kWineArenaTopDownBase,", self.source)

    def test_the_other_sparse_furniture_stays_below_the_stack(self) -> None:
        # The interpreter base and the TLS block; neither may be moved into the
        # gap this change opened.
        for literal in ("0x7FFFF7FCE000ULL", "0x7FFFF7800000ULL"):
            self.assertIn(literal, self.source)
            self.assertLess(int(literal[2:-3], 16), WINE_ARENA[2][0])


class TheArenaIsWitnessed(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(SYSCALLS)

    def test_the_witness_names_every_field_a_reader_needs(self) -> None:
        self.assertIn(WITNESS, self.source)
        for field in WITNESS_FIELDS:
            self.assertIn(field, self.source)

    def test_the_witness_fires_from_the_anonymous_mmap_path(self) -> None:
        self.assertIn("reportGuestWineArena(cpu, alignedAddr, mapLen, "
                      "(U32)prot, nativeIdentity,", self.source)

    def test_the_witness_is_bounded(self) -> None:
        self.assertIn("constexpr U64 kGuestWineArenaReports = 48;",
                      self.source)
        self.assertIn("gGuestWineArenaReports.fetch_add(1, "
                      "std::memory_order_relaxed) >=", self.source)

    def test_the_witness_only_describes_arena_ranges(self) -> None:
        self.assertIn("boxedvn::guestWineArenaRangeIndex(guestAddress, length)",
                      self.source)

    def test_the_witness_says_where_the_bytes_live(self) -> None:
        # Which of guest and host address a log is showing was previously left
        # to be inferred, and this is the range that has two of them.
        self.assertIn("boxedvn::guestToHostAddress(guestAddress)", self.source)


class EveryRefusalReachesTheLaneWitness(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(SYSCALLS)
        self.header = read(KMEMORY_HEADER)
        self.memory = read(KMEMORY_SOURCE)

    def test_the_placement_policys_own_refusals_are_reported(self) -> None:
        self.assertIn("boxedvn::guestMmapPlacementIsFailure(placement)",
                      self.source)
        self.assertIn('cpu->memory->reportGuestLaneRefusal(\n'
                      '            "mmap-placement", alignedAddr, mapLen,',
                      self.source)

    def test_only_out_of_lane_refusals_are_reported(self) -> None:
        # An EEXIST for an address inside a lane is an occupancy answer, not a
        # lane answer, and reporting it would drown the ones that matter.
        self.assertIn("if (nativeIdentity && addr != 0 && "
                      "!request.exactRangeAllowed &&", self.source)

    def test_the_report_says_whether_the_guest_was_told_it_owned_the_range(self) -> None:
        self.assertIn("cpu->memory->sparseReservationOverlaps(alignedAddr, "
                      "mapLen) ? 1", self.source)

    def test_the_entry_point_exists_and_forwards_to_the_bounded_witness(self) -> None:
        self.assertIn("void reportGuestLaneRefusal(const char* op, U64 addr, "
                      "U64 len,", self.header)
        self.assertIn("k64ReportGuestLaneRefusal(this, op, addr, len, "
                      "sparseReserved);", self.memory)


class ThePreloaderOutcomeIsStated(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(STARTUP)

    def test_the_alias_line_says_whether_the_target_exists(self) -> None:
        # "status=linked" described the link node, not the binary, and read for
        # months as though a preloader were available.
        self.assertIn("target_present=%d", self.source)
        self.assertIn("status=present", self.source)

    def test_the_consequence_is_named(self) -> None:
        self.assertIn("preloader_exec()", self.source)
        self.assertIn("mmap_init reserves its own arena", self.source)


if __name__ == "__main__":
    unittest.main()
