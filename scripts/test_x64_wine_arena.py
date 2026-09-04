"""BoxedVN - contract tests for the address space Wine actually reserves.

Five device logs at one revision all end the same way, and all of them were
read as though the guest were asking for memory outside the lanes we had just
widened:

    0078:err:virtual:allocate_virtual_memory out of memory for allocation,
                                             base (nil) size 48d30000
    0024:err:virtual:map_fixed_area out of memory for
                                             0x6fffffc50000-0x6ffffffeb000

with not one BOXEDWINE_X64_LANE_REFUSED line anywhere, which reads as "we
refuse nothing". Four separate things in that reading are wrong -- the first
three about where the arena comes from and what the addresses in those errors
are, the fourth about the fix they suggest -- and this file is where each of
them is nailed down.

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

4. Why a bigger arena would not have helped, and what does.

   The arena is 1.7 GiB and the low lane hosts eight, so the obvious next move
   was to hand Wine more of it. That move is worth at most 368 MiB and cannot
   be made from this side at all.

   The failing allocation came from a 32-bit image under WoW64, and every
   allocation the 32-bit half makes carries a ceiling the IMAGE declares.
   ntdll's virtual_set_large_address_space() sets user_space_wow_limit to
   limit_2g - 1, or limit_4g - 1 for an image marked
   IMAGE_FILE_LARGE_ADDRESS_AWARE; dlls/wow64/syscall.c reads the resulting
   HighestUserAddress into default_zero_bits, wow64_NtAllocateVirtualMemory
   substitutes it whenever the caller passed no zero_bits, and
   get_zero_bits_limit() turns it back into limit_high. 0x48d30000 is 1.14 GiB
   and the probe was an ordinary PE, so it was being allocated inside two.

   Inside that ceiling Wine's two placement paths cannot be combined. map_view
   tries map_reserved_area, which only places views INSIDE a reserved range,
   then map_free_area, which probes MAP_FIXED_NOREPLACE and therefore only
   places views OUTSIDE every reserved range -- a reservation is a real
   PROT_NONE mapping and answers EEXIST. So the largest single allocation an
   ordinary 32-bit process can obtain is the longer of the free run in
   [0x10000, 0x68000000) and the one in [0x68000000, 0x7f000000): 1.62 GiB and
   368 MiB. Merging them is all an arena change is worth, and mmap_init's
   constants are reachable only through a preloader -- whose own table
   (loader/preloader.c) reserves the same addresses -- or through a patched
   ntdll.

   The image's own ceiling is worth more and costs a linker flag.
   [0x80000000, 0x100000000) is two contiguous gigabytes that no reservation
   covers, that this address space places nothing in, and that the low lane
   already hosts. The probe is linked --large-address-aware and the bit is
   checked in the file rather than in the link line.

   And when the next log still shows a program running out, it will say why:
   Wine reaches its failure without issuing a syscall, so the census in
   kmemory64.cpp reports each band's occupancy and longest free run at the
   events that come closest to it.

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
PROBE_BUILD = REPO / "scripts" / "build-guest-graphics-probe.sh"

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

# The ceilings ntdll applies to a 32-bit image under WoW64, from
# virtual_set_large_address_space(). These are Windows' figures, not ours.
WOW64_LIMIT_DEFAULT = 0x80000000
WOW64_LIMIT_LARGE_ADDRESS_AWARE = 0x100000000

# The lowest address Wine will place a view at (address_space_start).
LOW_BAND_START = 0x10000

# The size the device log shows being refused: 1.14 GiB, base (nil).
REFUSED_ALLOCATION = 0x48D30000

CENSUS = "BOXEDWINE_X64_ARENA_CENSUS"
CENSUS_BANDS = ("dos-low", "teb", "top-down", "wow-2g", "wow-4g")
CENSUS_FIELDS = (
    "gen=%llu",
    "reason=%s",
    "want=0x%llx",
    "band=%s",
    "range=[0x%llx,0x%llx)",
    "len=0x%llx",
    "mapped=0x%llx",
    "accessible=0x%llx",
    "free=0x%llx",
    "largest_free=0x%llx",
    "largest_free_at=0x%llx",
    "fits=%d",
    "native=%d",
    "highest=0x%llx",
    "pages=%llu",
)

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


class TheCeilingIsTheImagesAndNotTheArenas(unittest.TestCase):
    """The reason a larger arena would not have fixed the failing run."""

    def setUp(self) -> None:
        self.header = read(ALIAS_HEADER)

    def test_both_wow64_ceilings_are_named_with_wines_own_derivation(self) -> None:
        self.assertIn("kWow64LimitDefault = 0x%xULL" % WOW64_LIMIT_DEFAULT,
                      self.header)
        self.assertIn("kWow64LimitLargeAddressAware = 0x%xULL"
                      % WOW64_LIMIT_LARGE_ADDRESS_AWARE, self.header)
        # Where the two figures come from, so neither can be "adjusted".
        self.assertIn("virtual_set_large_address_space()", self.header)
        self.assertIn("IMAGE_FILE_LARGE_ADDRESS_AWARE", self.header)
        self.assertIn("default_zero_bits", self.header)
        self.assertIn("get_zero_bits_limit()", self.header)

    def test_both_ceilings_lie_inside_the_low_lane(self) -> None:
        # Recomputed: raising an image's ceiling is only safe because every
        # address it then reaches for is one this address space can host.
        for limit in (WOW64_LIMIT_DEFAULT, WOW64_LIMIT_LARGE_ADDRESS_AWARE):
            self.assertTrue(hostable(LOW_BAND_START, limit - LOW_BAND_START))
        self.assertLessEqual(WOW64_LIMIT_LARGE_ADDRESS_AWARE, LOW_LIMIT)

    def test_the_refused_allocation_does_not_fit_the_reserved_low_range(self) -> None:
        # The arithmetic that settles it. Under the 2 GiB ceiling a 32-bit
        # process has exactly two runs to be placed in, and they cannot be
        # combined: map_reserved_area only places INSIDE a reservation and
        # map_free_area only OUTSIDE one.
        reserved = WINE_ARENA[0][1] - WINE_ARENA[0][0]
        gap = WINE_ARENA[1][0] - WINE_ARENA[0][1]
        self.assertLess(gap, REFUSED_ALLOCATION)
        # The request is more than half the reserved range, so ONE view placed
        # anywhere near the middle of it makes the request unsatisfiable for
        # the life of the process. That is why it fails with the band far from
        # full.
        self.assertGreater(2 * REFUSED_ALLOCATION, reserved)
        # Merging the two -- reserving to 0x7f000000 -- is the whole of what an
        # arena change could be worth, and it stays under the same ceiling.
        merged = reserved + gap
        self.assertGreater(merged, reserved)
        self.assertLess(merged, WOW64_LIMIT_DEFAULT)
        # Raising the image's ceiling is worth more: the entire band above
        # 2 GiB, which no reservation covers at all.
        above_2g = WOW64_LIMIT_LARGE_ADDRESS_AWARE - WOW64_LIMIT_DEFAULT
        self.assertGreater(above_2g, REFUSED_ALLOCATION)
        for base, end in WINE_ARENA:
            self.assertFalse(base < WOW64_LIMIT_LARGE_ADDRESS_AWARE
                             and end > WOW64_LIMIT_DEFAULT)

    def test_the_arena_cannot_be_enlarged_from_this_side(self) -> None:
        # Both doors, named, so the next reader does not reopen either without
        # knowing what is behind it.
        self.assertIn("loader/preloader.c", self.header)
        self.assertIn("patched ntdll or a patched preloader", self.header)

    def test_a_64_bit_image_is_not_in_this_band_at_all(self) -> None:
        self.assertIn("A 64-bit image is not in this band at all", self.header)

    def test_the_top_down_arena_is_above_every_32_bit_ceiling(self) -> None:
        # It is the 64-bit half's, it is where Wine relocates its builtins to,
        # and nothing here may be read as counting it against a 32-bit budget.
        self.assertGreaterEqual(WINE_ARENA[2][0],
                                WOW64_LIMIT_LARGE_ADDRESS_AWARE)
        self.assertIn("the top-down arena is above every 32-bit ceiling",
                      self.header)


class TheCensusBandsAreDeclared(unittest.TestCase):
    def setUp(self) -> None:
        self.header = read(ALIAS_HEADER)

    def test_the_bands_are_the_three_reservations_plus_the_two_ceilings(self) -> None:
        table = self.header[self.header.index("kGuestCensusBands[5]"):]
        table = table[:table.index("};")]
        names = re.findall(r'"([a-z0-9-]+)"\}', table)
        self.assertEqual(tuple(names), CENSUS_BANDS)

    def test_the_ceiling_bands_are_not_described_as_reservations(self) -> None:
        self.assertIn("the last two are not reservations at all", self.header)


class TheCensusSaysWhatIsLeft(unittest.TestCase):
    """A second out-of-memory has to arrive with the occupancy attached."""

    def setUp(self) -> None:
        self.memory = read(KMEMORY_SOURCE)
        self.header = read(KMEMORY_HEADER)
        self.source = read(SYSCALLS)

    def test_the_census_names_every_field_a_reader_needs(self) -> None:
        self.assertIn(CENSUS, self.memory)
        for field in CENSUS_FIELDS:
            self.assertIn(field, self.memory)

    def test_the_census_reports_the_longest_free_run(self) -> None:
        # The number Wine's allocator actually decides on. Totals alone would
        # not have distinguished "the band is full" from "the band is in two
        # pieces", which is the difference this failure turns on.
        self.assertIn("largest_free=0x%llx", self.memory)
        self.assertIn("out.largestFree = run << K64_PAGE_SHIFT;", self.memory)

    def test_the_census_separates_held_from_used(self) -> None:
        # Wine's reservations are mapped and inaccessible; a census that could
        # not tell those from committed pages would report a full band from
        # the first reserve_area call.
        self.assertIn("K64_PAGE_READ | K64_PAGE_WRITE | K64_PAGE_EXEC",
                      self.memory)
        self.assertIn("mapped at all, and mapped with some access",
                      self.memory)

    def test_the_census_says_which_ceiling_is_in_force(self) -> None:
        # Not readable from the guest, but decidable from the evidence: a run
        # whose highest mapped byte below 4 GiB never passes 0x80000000 is a
        # run under the 2 GiB ceiling.
        self.assertIn("if (pageEnd > highestLow) highestLow = pageEnd;",
                      self.memory)
        self.assertIn("never passes 0x80000000", self.memory)

    def test_an_unseen_page_counts_as_occupied(self) -> None:
        # A free run this census cannot vouch for must not be reported as one.
        self.assertIn("as occupied rather than report a free run that may not "
                      "exist", self.memory)

    def test_the_budget_is_per_address_space(self) -> None:
        self.assertIn("mutable std::atomic<U32> arenaCensusReports {0};",
                      self.header)
        self.assertIn("constexpr U32 K64_ARENA_CENSUS_LIMIT = 12;", self.memory)

    def test_refusals_reach_slots_nothing_else_can(self) -> None:
        # An unreserved budget is spent by the loader and the milestones long
        # before the first thing goes wrong, and a refusal is the one event
        # that can arrive after everything else has stopped happening.
        self.assertIn("constexpr U32 K64_ARENA_CENSUS_RESERVED = 4;",
                      self.memory)
        self.assertIn("const U32 limit = force ? K64_ARENA_CENSUS_LIMIT",
                      self.memory)
        self.assertIn("censusForced = true;", self.source)

    def test_a_declined_census_does_not_spend_a_slot(self) -> None:
        # A plain fetch_add would burn the reserved slots on the calls it then
        # declined to print, which is the opposite of reserving them.
        self.assertIn("arenaCensusReports.compare_exchange_weak(",
                      self.memory)
        self.assertNotIn("arenaCensusReports.fetch_add(", self.memory)

    def test_the_census_covers_interpreted_address_spaces_too(self) -> None:
        # Same three reservations, same page map, and the top-down arena is one
        # of the bands -- which is what checks the stack having moved out of it.
        self.assertIn("This runs for interpreted address spaces too.",
                      self.memory)
        self.assertIn("native=%d", self.memory)

    def test_the_triggers_are_the_ones_closest_to_the_failure(self) -> None:
        self.assertIn("constexpr U64 kGuestArenaCensusLargeRequest = "
                      "16ULL << 20;", self.source)
        self.assertIn("constexpr U64 kGuestArenaCensusRefusedRequest = "
                      "1ULL << 20;", self.source)
        self.assertIn("constexpr U64 kGuestArenaCensusFirstMilestone = 128;",
                      self.source)
        for reason in ('"large-request"', '"refused"', '"milestone"'):
            self.assertIn(reason, self.source)
        self.assertIn("cpu->memory->reportGuestArenaCensus(censusReason, "
                      "mapLen, censusForced);", self.source)

    def test_the_census_says_why_it_cannot_be_exact(self) -> None:
        # Wine gives up without issuing a syscall, so there is no call to hang
        # the report on and the census can only approach the moment.
        self.assertIn("issues no mmap", self.header)
        self.assertIn("can only be approached, never captured", self.source)


class TheProbeCanReachTheAddressSpaceWeHost(unittest.TestCase):
    def setUp(self) -> None:
        self.script = read(PROBE_BUILD)

    def test_the_probe_is_linked_large_address_aware(self) -> None:
        self.assertIn("-Wl,--large-address-aware", self.script)

    def test_the_flag_is_checked_in_the_file_and_not_the_link_line(self) -> None:
        # objdump renders the field differently across binutils versions; the
        # byte does not. e_lfanew at 0x3c, PE signature, then the 20-byte COFF
        # header whose last two bytes are Characteristics.
        self.assertIn('read_le_at 60 4 "${OUTPUT}"', self.script)
        self.assertIn('!= "50450000"', self.script)
        self.assertIn("read_le_at $(( PE_HEADER_OFFSET + 22 )) 2", self.script)
        self.assertIn("CHARACTERISTICS & 0x20", self.script)

    def test_the_reason_for_the_flag_is_recorded_where_it_is_applied(self) -> None:
        self.assertIn("user_space_wow_limit", self.script)
        self.assertIn("0x48d30000", self.script)


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
