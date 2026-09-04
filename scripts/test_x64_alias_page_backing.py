"""BoxedVN - contract tests for guest-lane host page backing.

A 32-bit Windows program ran for about a hundred seconds, created its window,
and then died in a word-copy loop:

    BOXEDWINE_FEX64_UNALIGNED  ... address=0x7800d88000 handled=0 advance=0
    BOXEDWINE_FEX64_GUEST_FAULT ... host_signal=10 si_code=1 fault=0x7800d88000
                                    guest_rip=0x7a6fb0b0 cs=0x23 decode=32
    CPU64: exit_group syscall, status=2147483650

0x7800d88000 is the low-alias image of guest 0xd88000, which is what rax held,
and the host instruction (0x79400066 = `ldrh w6, [x3]`) is a plain 16-bit load
with no atomicity to emulate, so FEX's unaligned handler correctly declined it.
The fault became a guest SIGBUS and Wine turned that into
STATUS_DATATYPE_MISALIGNMENT (0x80000002), the process exit status.

The address is page aligned, so this was never an alignment fault. si_code=1 is
BUS_ADRALN, but that means nothing here: Darwin's arm64 sendsig() assigns
BUS_ADRALN to EVERY SIGBUS unconditionally, and only SIGSEGV carries a
KERN_-derived si_code. What the SIGNAL says instead is that an EXC_BAD_ACCESS
whose code was not KERN_INVALID_ADDRESS reached the thread.

The underlying fact is that a native-identity address space keeps two ledgers
for the same bytes at two different granularities -- the 4 KiB guest page map,
and the host VM plus nativeRanges at the iOS host page size of 16 KiB -- and
FEX consults only the second one, because the translator dereferences the alias
directly. When they disagree the guest is killed for reading a page it mapped.

The device run that followed showed the repair working exactly as designed and
the program dying anyway:

    BOXEDWINE_X64_ALIAS_BACKING  ... guest=0xd88000 mapped=1 guest_prot=0x0
    BOXEDWINE_FEX64_GUEST_FAULT  ... guest_signal=7 trap=17 guest_rip=0x7a6fb0b0

mapped=1 with guest_prot=0x0 is a page the guest reserved and has not committed
-- which is what NtAllocateVirtualMemory's MEM_RESERVE leaves behind, and what
mmapAnonymousFixed records for PROT_NONE: K64_PAGE_MAPPED with neither
K64_PAGE_READ nor K64_PAGE_WRITE. The repair refused it, correctly, because the
page map does not entitle the guest to read it. (The host= and host_present=
fields on that line are the report's untouched defaults: the refusal returns
before either is probed, so they are not evidence that the host page is absent.)

What was wrong was the signal that refusal turned into. On Linux that access is
SIGSEGV with SEGV_ACCERR and trap 14, which Wine turns into
STATUS_ACCESS_VIOLATION and commits the page from its own handler. Passing
Darwin's SIGBUS through delivered guest signal 7 with trap 17, which Wine reads
as STATUS_DATATYPE_MISALIGNMENT: the page-fault handler never ran.

The run after THAT one delivered the page fault correctly and the program died
anyway, at guest 0xbe0000 where the previous run had died at 0xd88000:

    BOXEDWINE_X64_ALIAS_BACKING ... guest=0xbe0000 mapped=1 guest_prot=0x0
                                    decision=guest-protected
    BOXEDWINE_FEX64_GUEST_FAULT ... guest_signal=11 trap=14 guest_si_code=2
    BOXEDWINE_SESSION_PROGRAM_EXIT status=0xc0000005

So Wine saw STATUS_ACCESS_VIOLATION for a page it believed it had committed and
passed it to the program: the refusal itself was wrong. Both addresses are
16 KiB aligned and neither is 64 KiB aligned, which is the host page size and
nothing else in the system; the faulting loop had read the preceding bytes
without faulting, so the rights ended exactly at a host page boundary. The
producing side was mprotect: it asked nativeRangeCovers about the whole
HOST-ROUNDED interval and returned -EINVAL for the ENTIRE request when any host
page in it -- including an edge host page the request only partly covered, or
one the lazy fault repair had not yet materialised -- was untracked. It returned
before writing a single guest page's flags and logged nothing, so the pages Wine
had just committed kept the flags of the PROT_NONE reservation they were being
committed out of. Two more paths applied one protection across a whole interval
the same way: nativeMapAnonymous chose between the requested protection and
read/write from plan.exactHostCover, a property of the WHOLE interval, and both
mprotect and mmapSharedFile then assigned that value to every tracked range the
interval merely overlapped.

The rule the tests below now hold is one sentence in two directions: a guest
page's rights change only in an operation that names it, and a host page carries
exactly the union of what its four guest pages hold, recomputed per host page
whenever any of them changes. An untracked host page is skipped rather than
failing the call. A guest page also remembers which operation last wrote its
flags and what it wrote, and the fault witness prints that for the faulting page
and for its most recently written neighbour in the same host page -- which is
what separates "the whole host page was never committed" from "this 4 KiB page
lost rights its neighbours kept".

That mprotect fix is in, and the run after it refused guest 0xd88000 with the
provenance printed:

    BOXEDWINE_X64_ALIAS_BACKING ... guest=0xd88000 mapped=1 guest_prot=0x0
        host_present=0 tracked=0 last_op=mmap-anon last_flags=0x20 last_seq=6226
        nb_op=mmap-anon nb_flags=0x20 nb_seq=6226 decision=guest-protected

This time the refusal is right, and three independent facts say so. The page's
rights were last written by the anonymous PROT_NONE mmap that created it, with
the same stamp as its neighbour, so no mprotect ever named it. The syscall tail
contains no mmap, mprotect or madvise anywhere inside it; the nearest memory
calls are mmap(0xed0000, 0x910000, PROT_NONE, MAP_FIXED) and
mprotect(0xed0000, 0x910000, PROT_READ|PROT_WRITE), which we answered with
0xed0000 and 0 -- a region 0x148000 bytes ABOVE the fault, and both of them
completed before the fault rather than in response to it. And Wine's own views
tree, which is a ledger we do not write, reached the same verdict: its handler
committed nothing and the process terminated with 0xc0000005 in a 32-bit
word-copy loop (rax=0xd88000, rcx=0xff14c8, edx on the stack), which is what
Wine does for an address no view of its owns.

So the guest read memory it never committed, on a wild or overrun pointer, and
the defect is upstream of this file. What the witness still could not say is
which of those two it was, and the answer decides where to look next. Two
things were missing, and both are now recorded. A page remembers the write
BEFORE the last one that changed its flags: Wine restores a freed view by
re-mmapping it PROT_NONE over the reserved arena, so a never-used reservation
and a use-after-free of a committed block are the same last_op with the same
last_flags, and only the prior write separates them. And a refused fault names
the nearest readable page either side of itself, bounded, so a cursor that
walked off the end of a real buffer -- which ends exactly where the readable
pages below it stop -- is distinguishable from a pointer that never addressed
anything at all.

These tests hold both halves in place: the page map stays the authority, the
repair never grants more than the page map grants, the retry is bounded, the
witness that will identify the producing ledger keeps printing every fact
needed to tell "no host page" from "host page, wrong protection", and the
signal the guest is finally handed is classified from the page map rather than
from a host signal number that cannot carry the distinction.

Source-level contracts on purpose: building the emulator needs the iOS
toolchain, and there is no host on which the ARM64 signal path can be run.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
KMEMORY_HEADER = REPO / "include" / "kmemory64.h"
KMEMORY_SOURCE = REPO / "source" / "kernel" / "kmemory64.cpp"
ADAPTER = REPO / "ios" / "runtime" / "src" / "BVNFEXCPU64Adapter.mm"
ALIAS_HEADER = REPO / "include" / "guest_low_alias.h"
KSIGNAL_HEADER = REPO / "include" / "ksignal.h"
FEX_BUILD = REPO / "scripts" / "build-fex64-fex.sh"

# The witness line. Every one of these fields has to survive, because the point
# of the line is to tell the two ledgers apart on the next device run.
WITNESS = "BOXEDWINE_X64_ALIAS_BACKING"
WITNESS_FIELDS = (
    "pid=%d",
    "tid=%d",
    "fault=0x%llx",
    "guest=0x%llx",
    "mapped=%d",
    "guest_prot=0x%x",
    "host=[0x%llx,0x%llx)",
    "host_present=%d",
    "host_prot=0x%x",
    "tracked=%d",
    "materialised=%d",
    "reprotected=%d",
    "decision=%s",
)

# Every verdict nativeRepairHostFault can reach. A repair is only two of them;
# the rest are refusals that leave the fault as the guest's.
REPAIR_DECISIONS = ("materialised", "reprotected")
REFUSAL_DECISIONS = (
    "not-native",
    "outside-lane",
    "guest-unmapped",
    "guest-protected",
    "host-page-size",
    "backing-elsewhere",
    "reserve-failed",
    "map-failed",
    "mprotect-failed",
)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    """The text of a function, from its signature to the closing brace in
    column zero. Good enough for the file-scope definitions here."""
    start = source.index(signature)
    end = source.index("\n}\n", start)
    return source[start:end]


class TheRepairIsDrivenByThePageMap(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(KMEMORY_SOURCE)
        self.body = function_body(
            self.source, "bool KMemory64::nativeRepairHostFault(")

    def test_a_page_the_guest_never_mapped_is_still_a_guest_fault(self) -> None:
        # This is the property that keeps a real access violation an access
        # violation. Without it every wild pointer inside a guest lane would be
        # answered with a fresh zero page and the program would run on garbage.
        self.assertIn("K64_PAGE_MAPPED", self.body)
        self.assertIn('report.decision = "guest-unmapped"', self.body)
        self.assertIn('report.decision = "guest-protected"', self.body)

    def test_a_repair_never_grants_more_than_the_page_map_grants(self) -> None:
        # The host page is raised to the union its four guest subpages are
        # entitled to -- k64GuestProtFromPageFlags of each -- and to nothing
        # else. A blanket PROT_READ|PROT_WRITE would silently hand the guest
        # write access to a page it protected.
        self.assertIn("k64GuestProtFromPageFlags", self.body)
        self.assertIn("k64NativeProt(unionProt)", self.body)
        self.assertNotIn("mprotect((void*)(uintptr_t)hostStart, (size_t)hostPage,\n"
                         "                   PROT_READ | PROT_WRITE)", self.body)

    def test_the_union_is_taken_over_the_whole_host_page(self) -> None:
        # One 16 KiB host page carries four 4 KiB guest pages, so a repair that
        # looked only at the faulting guest page would revoke its neighbours.
        # The union now has exactly ONE definition, shared with every producing
        # path, so the repair reaches it by calling that.
        self.assertIn("nativeHostPageProtLocked(hostStart, hostPage,", self.body)
        union = function_body(
            self.source, "U32 KMemory64::nativeHostPageProtLocked(")
        self.assertIn(
            "for (U64 sub = 0; sub < hostPageSize; sub += K64_PAGE_SIZE)",
            union)
        # `pages` is keyed by CANONICAL guest page, so the host page has to be
        # translated back before its subpages can be named.
        self.assertIn("k64HostToGuestAddress(hostPageStart)", union)

    def test_the_union_is_computed_before_the_refusal(self) -> None:
        # A refused fault has to say what the REST of the host page holds. A
        # guest page with no rights beside three that have them is a
        # granularity defect; one whose whole host page has none is a
        # reservation the guest has genuinely not committed. The witness could
        # not tell those apart while the union was computed after the return.
        union = self.body.index("unionProt = nativeHostPageProtLocked(")
        refusal = self.body.index('report.decision = "guest-protected"')
        self.assertLess(union, refusal)
        self.assertIn("report.hostPageProt = unionProt;", self.body)

    def test_only_a_guest_image_address_is_repaired(self) -> None:
        # The round trip is the whole membership test: it holds for the low
        # alias, for the high identity lane where it is the identity, and for
        # the top-arena alias, and fails for the emulator's own heap and the
        # translated code arena.
        self.assertIn("k64GuestToHostAddress(guestAddress) != hostFaultAddress",
                      self.body)
        self.assertIn("nativeGuestRangeAllowed(guestPageAddress, K64_PAGE_SIZE)",
                      self.body)
        self.assertIn('report.decision = "outside-lane"', self.body)

    def test_a_missing_host_page_is_claimed_the_way_mapping_claims_one(self) -> None:
        # Reserve first, then MAP_FIXED over the reservation. A bare MAP_FIXED
        # at an untracked address could replace the executable arena or another
        # runtime region.
        reserve = self.body.index("k64NativeReserveHostRange")
        mapped = self.body.index("::mmap((void*)(uintptr_t)hostStart")
        self.assertLess(reserve, mapped,
                        "the host page must be reserved before it is mapped")
        self.assertIn("k64NativeHostRangeFree(hostStart, hostPage)", self.body)
        self.assertIn("k64NativeReleaseReservedHostRange(hostStart, hostPage)",
                      self.body)

    def test_a_present_host_page_is_never_remapped_over(self) -> None:
        # Re-mapping a page that already holds guest bytes would zero them. The
        # materialise branch runs only when the host has no region there.
        self.assertIn("if (!report.hostPresent) {", self.body)
        self.assertNotIn("::memset", self.body)
        # And a page whose bytes live somewhere else -- a shared-file page a
        # sparse process demoted back to its canonical buffer -- is never given
        # a fresh zero page either: that would answer the read with the wrong
        # contents, which is worse than the fault.
        self.assertIn("backedAtAlias", self.body)
        self.assertIn('report.decision = "backing-elsewhere"', self.body)

    def test_the_repair_keeps_the_native_range_ledger_honest(self) -> None:
        # Otherwise the next munmap leaves the host page behind. Exactly this
        # host page is recorded -- nativeTrackRangeLocked forgets and re-adds
        # the named interval and nothing else.
        self.assertIn("nativeTrackRangeLocked(hostStart, hostPage, unionProt)",
                      self.body)
        self.assertIn("k64DTlbInvalidateAll()", self.body)

    def test_every_exit_names_a_decision(self) -> None:
        for decision in REPAIR_DECISIONS + REFUSAL_DECISIONS:
            self.assertIn(f'"{decision}"', self.body,
                          f"{decision} is not reachable any more")

    def test_the_other_ledger_is_actually_read(self) -> None:
        # host_prot / host_present come from the host VM itself, not from
        # nativeRanges. That is what distinguishes "no host page here" from
        # "host page here, but PROT_NONE" -- two different bugs with the same
        # guest symptom.
        self.assertIn("k64NativeHostProtOf(hostStart, report.hostPresent)",
                      self.body)
        self.assertIn("vm_region_64(", read(KMEMORY_SOURCE))


class TheHeaderStatesTheContract(unittest.TestCase):
    def setUp(self) -> None:
        self.header = read(KMEMORY_HEADER)

    def test_the_report_carries_both_ledgers(self) -> None:
        struct = self.header.split("struct K64NativeFaultRepair {", 1)[1]
        struct = struct.split("};", 1)[0]
        for field in ("guestAddress", "hostPageStart", "hostPageLength",
                      "inGuestLane", "pageMapped", "guestProt", "hostPageProt",
                      "hostPresent", "hostProtBefore", "tracked",
                      "materialised", "reprotected", "decision"):
            self.assertIn(field, struct, f"the witness lost {field}")

    def test_the_entry_point_takes_a_required_protection(self) -> None:
        # The caller says what the access needed. Passing it, rather than
        # assuming read/write, is what keeps a store to a read-only guest page a
        # guest fault.
        self.assertIn("bool nativeRepairHostFault(U64 hostFaultAddress, U32 requiredProt,",
                      self.header)

    def test_the_guest_page_size_is_still_a_quarter_of_the_ios_host_page(self) -> None:
        # The whole failure mode is this ratio. If the guest page ever became
        # 16 KiB the union logic above would be dead weight rather than load
        # bearing, and that should be a deliberate change.
        match = re.search(r"^#define K64_PAGE_SIZE\s+(\d+)", self.header, re.M)
        self.assertIsNotNone(match)
        self.assertEqual(int(match.group(1)), 4096)


class TheSignalPathRetriesSafely(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(ADAPTER)

    def test_the_repair_is_attempted_before_the_fault_becomes_a_guest_signal(self) -> None:
        handler = self.source.split(
            "BVNFEXCPU64AdapterHandleHostFault(", 1)[1]
        repair = handler.index("repairGuestLaneHostFault(adapter, signal,")
        raise_fault = handler.index("BVNFEXCPU64AdapterSyncFromFEX(adapter, frame)")
        self.assertLess(repair, raise_fault,
                        "the guest signal is built before the repair is tried")

    def test_a_fex_generated_exception_is_never_repaired(self) -> None:
        # Those faults are FEX's own trapping stubs; their si_addr describes the
        # stub, not a guest access.
        self.assertIn("if (!generatedException &&\n"
                      "        repairGuestLaneHostFault(adapter, signal, faultAddress, hostPC)) {",
                      self.source)

    def test_both_host_signals_are_considered(self) -> None:
        # Darwin reports a forbidden access as SIGBUS and an absent region as
        # SIGSEGV, so keying on one of them would miss half the cases.
        body = self.source.split("static bool repairGuestLaneHostFault(", 1)[1]
        self.assertIn("if (signal != SIGBUS && signal != SIGSEGV) return false;",
                      body)

    def test_only_read_access_is_required_for_a_repair(self) -> None:
        # The translator cannot tell the handler load from store. Requiring read
        # alone, and restoring exactly the page map's protection, makes a store
        # to a read-only page fault again rather than succeed.
        body = self.source.split("static bool repairGuestLaneHostFault(", 1)[1]
        self.assertIn("nativeRepairHostFault(\n            faultAddress, K_PROT_READ, report)",
                      body)

    def test_the_retry_cannot_loop_forever(self) -> None:
        # A repaired instruction is retried at the same host PC. The guard makes
        # an identical (address, PC) fault immediately after a repair fall
        # through to the guest, so the loop is at most one extra iteration.
        body = self.source.split("static bool repairGuestLaneHostFault(", 1)[1]
        self.assertIn("gAliasBackingRepairGuard.armed &&", body)
        self.assertIn("gAliasBackingRepairGuard.address == faultAddress &&", body)
        self.assertIn("gAliasBackingRepairGuard.hostPC == hostPC", body)
        self.assertIn('report.decision = "repeat"', body)
        self.assertIn("gAliasBackingRepairGuard.armed = repaired;", body)
        self.assertIn("static thread_local AliasBackingRepairGuard", self.source)

    def test_a_successful_repair_retries_the_instruction_in_place(self) -> None:
        # No RIP advance, no register spill: the host page now backs the address
        # the instruction already computed.
        handler = self.source.split(
            "repairGuestLaneHostFault(adapter, signal, faultAddress, hostPC)) {", 1)[1]
        self.assertTrue(handler.lstrip().startswith("return true;"),
                        "a repaired fault must return straight to the retry")


class TheWitnessNamesTheCase(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(ADAPTER)

    def test_the_line_exists_and_is_bounded(self) -> None:
        self.assertIn(WITNESS, self.source)
        body = self.source.split("static bool repairGuestLaneHostFault(", 1)[1]
        self.assertIn("gAliasBackingReports.fetch_add(1, std::memory_order_relaxed) < 64",
                      body)

    def test_the_line_prints_every_fact_that_discriminates(self) -> None:
        line = self.source.split(WITNESS, 1)[1].split(");", 1)[0]
        for field in WITNESS_FIELDS:
            self.assertIn(field, line, f"the witness stopped printing {field}")

    def test_it_is_only_printed_for_a_fault_it_can_speak_about(self) -> None:
        # A host fault outside every guest lane -- the emulator's own heap, the
        # code arena -- is not this failure mode and must not add a line.
        body = self.source.split("static bool repairGuestLaneHostFault(", 1)[1]
        self.assertIn("if ((report.inGuestLane || repeat) &&", body)


class TheTranslatorStillDereferencesDirectly(unittest.TestCase):
    """The reason a repair is needed at all, and the reason it covers the
    64-bit lane too."""

    def test_the_translation_is_applied_to_every_guest_access(self) -> None:
        # One `orr` plus one `bic`, unconditionally, for 32-bit and 64-bit
        # guests alike. There is no page-table consultation anywhere in it, so
        # the host VM is the only thing standing between the guest and its
        # bytes -- in the identity lane exactly as much as in the alias window.
        patch = read(REPO / "scripts" / "fex64-patches" /
                     "fex-boxedwine-low-address-alias.patch")
        self.assertIn("AliasGuestAddress", patch)
        self.assertNotIn("Is64BitMode", patch,
                         "the alias translation must not be 32-bit only")

    def test_the_identity_lane_is_the_identity_under_the_translation(self) -> None:
        # host = (guest | base) & ~clear. Every high-lane address already
        # carries the base's bits and touches none of the clear mask, so the
        # 64-bit lane is dereferenced at its own address and is backed by the
        # very same nativeRanges/mprotect bookkeeping.
        header = read(ALIAS_HEADER)
        base = int(re.search(r"kGuestLowAliasBase = (0x[0-9A-Fa-f]+)ULL",
                             header).group(1), 16)
        limit = int(re.search(r"kGuestLowLimit = (0x[0-9A-Fa-f]+)ULL",
                              header).group(1), 16)
        clear = int(re.search(r"kGuestTopClearMask = (0x[0-9A-Fa-f]+)ULL",
                              header).group(1), 16)
        high_base = base + limit
        high_end = high_base + limit
        for address in (high_base, high_base + 0x1000, high_end - 0x1000):
            self.assertEqual((address | base) & ~clear, address)

    def test_the_unapplied_window_bias_patch_is_not_in_the_build(self) -> None:
        # scripts/fex64-patches/fex32-guest-window-bias.patch is an earlier,
        # incompatible take on the same translation: a per-context ADD bias
        # applied only when Is64BitMode() is false. It does not touch page
        # backing, and adding it beside the alias patch would give the JIT two
        # different host addresses for one guest address.
        build = read(FEX_BUILD)
        self.assertNotIn("fex32-guest-window-bias.patch", build)


class TheGuestSignalIsClassifiedFromThePageMap(unittest.TestCase):
    """The second half of the failure: the refusal above is right, and the
    signal it was turned into was wrong."""

    def setUp(self) -> None:
        self.source = read(ADAPTER)
        self.body = function_body(
            self.source, "static GuestMemoryFaultClass classifyGuestMemoryFault(")

    def test_the_page_map_is_what_is_consulted(self) -> None:
        # Not the host signal number, and not si_code: on Darwin neither can
        # tell a protection failure from an alignment fault.
        self.assertIn("isPageMapped(pageNumber)", self.body)
        self.assertIn("getPageFlags(pageNumber) & K64_PAGE_READ", self.body)

    def test_an_unmapped_guest_address_is_a_mapping_error(self) -> None:
        self.assertIn("result.code = mapped ? K_SEGV_ACCERR : K_SEGV_MAPERR;",
                      self.body)

    def test_a_mapped_page_that_forbids_the_access_is_an_access_error(self) -> None:
        # A Wine MEM_RESERVE view is exactly this: K64_PAGE_MAPPED with no
        # K64_PAGE_READ. Wine commits it on demand from its own handler, but
        # only ever reaches that handler for STATUS_ACCESS_VIOLATION.
        self.assertIn("if (!mapped || !readable) {", self.body)
        self.assertIn("result.signal = K_SIGSEGV;", self.body)
        self.assertIn("K_SEGV_ACCERR", self.body)

    def test_both_page_fault_verdicts_carry_trap_14(self) -> None:
        # Trap 14 is #PF. Wine's exception dispatcher reads the trap number, so
        # a SIGSEGV that still said 17 would be no better than the SIGBUS was.
        self.assertEqual(self.body.count("result.trapNumber = 14; // #PF"), 2,
                         "both SIGSEGV verdicts must name #PF")

    def test_only_an_entitled_access_stays_a_bus_error(self) -> None:
        # An alignment fault is the one memory fault left on a page the map
        # says the guest may read, so that is the only path that keeps SIGBUS.
        bus = self.body.index("result.signal = K_SIGBUS;")
        readable_gate = self.body.index("if (!mapped || !readable) {")
        self.assertLess(readable_gate, bus,
                        "SIGBUS must only be reachable past the page-map gate")
        self.assertIn("result.trapNumber = 17;", self.body)
        self.assertIn("result.code = K_BUS_ADRALN;", self.body)

    def test_the_guest_sees_the_guest_address_not_the_host_alias(self) -> None:
        # The translator dereferenced the alias, so si_addr is a host address.
        # Handing 0x7800d88000 to a 32-bit guest names no address it has.
        self.assertIn(
            "const uint64_t guestAddress = k64HostToGuestAddress(faultAddress);",
            self.body)
        self.assertIn("result.address = guestAddress;", self.body)

    def test_only_a_guest_lane_address_is_reclassified(self) -> None:
        # Same membership test the repair applies: the round trip through the
        # alias, then the window guard. A fault on the emulator's own heap or
        # on the code arena keeps the host verdict AND the host address.
        self.assertIn("k64GuestToHostAddress(guestAddress) != faultAddress",
                      self.body)
        self.assertIn("nativeGuestRangeAllowed(guestPageAddress, K64_PAGE_SIZE)",
                      self.body)
        self.assertIn("result.fromPageMap = true;", self.body)

    def test_only_memory_faults_are_reclassified(self) -> None:
        # SIGILL and SIGFPE describe the instruction, not an address; the page
        # map has nothing to say about them.
        self.assertIn(
            "if (signal != SIGBUS && signal != SIGSEGV) return result;",
            self.body)


class TheClassificationReachesTheGuest(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(ADAPTER)
        # The DEFINITION, not the no-op stub of the same name at the top of the
        # file: splitting on the bare name would put the whole file in scope
        # and make every ordering assertion below meaningless.
        self.handler = self.source.split(
            "BVNFEXCPU64Adapter* adapter, const void* signalConfigPointer, "
            "int signal,", 1)[1]

    def test_the_guest_signal_comes_from_the_classification(self) -> None:
        for field in ("uint32_t guestSignal = faultClass.signal;",
                      "uint32_t guestTrapNumber = faultClass.trapNumber;",
                      "uint32_t guestSignalCode = faultClass.code;",
                      "uint64_t guestFaultAddress = faultClass.address;"):
            self.assertIn(field, self.handler)

    def test_the_classified_values_are_what_is_raised(self) -> None:
        # raiseSyncFault is the architectural operation; it is what builds the
        # Linux siginfo frame the guest handler reads.
        self.assertIn("raiseSyncFault(\n            guestSignal, guestTrapNumber,\n"
                      "            static_cast<int>(guestSignalCode), guestFaultAddress)",
                      self.handler)

    def test_fex_still_gets_first_refusal_on_an_unaligned_access(self) -> None:
        # FEX emulates the atomic in place or backpatches the access and says
        # how far the host PC moves. Nothing may come between that and the
        # fault, or an emulable unaligned atomic would be turned into a guest
        # signal instead of being completed.
        unaligned = self.handler.index("HandleUnalignedAccess(")
        classify = self.handler.index("classifyGuestMemoryFault(")
        self.assertLess(unaligned, classify,
                        "the classifier must not run before FEX's own handler")
        # And it is still entered on exactly the host description Darwin gives
        # an unaligned atomic, with no page-map precondition of its own.
        self.assertIn(
            "if (signal == SIGBUS && siginfo->si_code == BUS_ADRALN && inCodeBuffer) {",
            self.source)

    def test_the_repair_is_still_tried_before_the_classification(self) -> None:
        # A page the map DOES entitle the guest to is repaired and retried; it
        # must never be classified into a guest signal at all.
        repair = self.handler.index("repairGuestLaneHostFault(adapter, signal,")
        classify = self.handler.index("classifyGuestMemoryFault(")
        self.assertLess(repair, classify)

    def test_a_fex_generated_exception_is_never_reclassified(self) -> None:
        # Its si_addr describes FEX's own trapping stub, not a guest access.
        self.assertIn("if (!generatedException) {\n"
                      "        faultClass = classifyGuestMemoryFault(adapter, signal,\n"
                      "                                              siginfo->si_code, faultAddress);\n"
                      "    }", self.handler)
        # And the generated branch still overrides all four from FEX's own
        # architectural description.
        generated = self.handler.split("if (generatedException) {", 1)[1]
        for field in ("guestSignal = hostSignalGuestNumber(faultData.Signal);",
                      "guestTrapNumber = faultData.TrapNo;",
                      "guestSignalCode = faultData.si_code;",
                      "guestFaultAddress = frame->State.rip;"):
            self.assertIn(field, generated)

    def test_the_witness_says_what_the_guest_was_handed(self) -> None:
        # host_signal / si_code alone cannot be read back into a verdict, which
        # is the whole point; the line has to carry both sides.
        start = self.source.index(
            '"BOXEDWINE_FEX64_GUEST_FAULT pid=%d tid=%d host_signal=%d "')
        line = self.source[start:].split(");", 1)[0]
        for field in ("host_signal=%d", "si_code=%d", "fault=0x%llx",
                      "guest_signal=%u", "trap=%u", "guest_si_code=%u",
                      "guest_fault=0x%llx", "pagemap=%d"):
            self.assertIn(field, line,
                          f"the fault witness stopped printing {field}")


class TheSignalCodesAreLinuxs(unittest.TestCase):
    def setUp(self) -> None:
        self.header = read(KSIGNAL_HEADER)

    def test_the_codes_carry_the_values_a_linux_guest_expects(self) -> None:
        # asm-generic/siginfo.h. Wine reads si_code out of the frame CPU64
        # builds, so a wrong value here is a wrong exception record.
        for name, value in (("K_SEGV_MAPERR", 1), ("K_SEGV_ACCERR", 2),
                            ("K_BUS_ADRALN", 1), ("K_BUS_ADRERR", 2),
                            ("K_BUS_OBJERR", 3)):
            match = re.search(rf"^#define {name}\s+(\d+)", self.header, re.M)
            self.assertIsNotNone(match, f"{name} is not defined")
            self.assertEqual(int(match.group(1)), value)

    def test_the_guest_signal_numbers_are_linuxs(self) -> None:
        # Darwin numbers SIGBUS 10 and SIGSEGV 11; Linux numbers them 7 and 11.
        for name, value in (("K_SIGSEGV", 11), ("K_SIGBUS", 7)):
            match = re.search(rf"^#define {name}\s+(\d+)", self.header, re.M)
            self.assertIsNotNone(match)
            self.assertEqual(int(match.group(1)), value)


class AReservationIsAMappedPageWithNoRights(unittest.TestCase):
    """Why the page carried no protection at all, from the producing side."""

    def setUp(self) -> None:
        self.source = read(KMEMORY_SOURCE)

    def test_prot_none_records_a_mapped_page_with_no_access_bits(self) -> None:
        # Wine reserves address space before committing it, and a reserved view
        # reaches this address space as PROT_NONE. Both producers start from
        # K64_PAGE_MAPPED and add READ/WRITE only for the requested protection,
        # so prot 0 leaves mapped=1 with guest_prot=0x0 -- exactly what the
        # device witness printed.
        for producer in ("U32 flags = K64_PAGE_MAPPED;",
                         "U32 newFlags = K64_PAGE_MAPPED;"):
            self.assertIn(producer, self.source)
        self.assertIn("if (prot & 0x1) flags |= K64_PAGE_READ;", self.source)
        self.assertIn("if (prot & 0x1) newFlags |= K64_PAGE_READ;", self.source)

    def test_the_protection_bits_are_read_and_write_only(self) -> None:
        # K64_PAGE_EXEC is guest metadata, so nothing else can zero the
        # protection of a page the page map still calls mapped.
        body = function_body(self.source, "static U32 k64GuestProtFromPageFlags(")
        self.assertIn("if (flags & K64_PAGE_READ) prot |= 0x1;", body)
        self.assertIn("if (flags & K64_PAGE_WRITE) prot |= 0x2;", body)
        self.assertIn("K64_PAGE_EXEC intentionally remains guest metadata only",
                      body)

    def test_the_repair_refuses_a_reservation_rather_than_committing_it(self) -> None:
        # Committing it here would be BoxedWine deciding what Wine's own
        # page-fault handler is for. The refusal is right; delivering it as a
        # page fault is what makes the refusal usable.
        body = function_body(
            self.source, "bool KMemory64::nativeRepairHostFault(")
        # The entitlement is decided under the same lock that read the page,
        # and the refusal is that decision rather than a second reading of it.
        self.assertIn("entitled = report.guestProt != 0 &&", body)
        self.assertIn("(report.guestProt & requiredProt) == requiredProt &&", body)
        self.assertIn("(unionProt & requiredProt) == requiredProt;", body)
        self.assertIn("if (!entitled) {", body)
        self.assertIn('report.decision = "guest-protected"', body)


class AGuestPageChangesOnlyInACallThatNamesIt(unittest.TestCase):
    """The producing rule. A 4 KiB guest page's rights may be written only by
    an operation whose own guest range covers it -- never as a side effect of
    the 16 KiB host page it happens to share."""

    def setUp(self) -> None:
        self.source = read(KMEMORY_SOURCE)
        self.header = read(KMEMORY_HEADER)

    def test_there_is_exactly_one_writer_of_a_page_s_flags(self) -> None:
        # Routing every write through K64Page::writeFlags is what makes the
        # rule checkable at all, and what lets the page remember its writer.
        self.assertIn("void writeFlags(U32 value, U8 writer, U16 stamp) {",
                      self.header)
        stray = re.findall(r"(?:page|copy|it->second)->flags\s*(?:=|\|=|&=)\s",
                           self.source)
        self.assertEqual(stray, [],
                         "a guest page's flags are written outside writeFlags")

    def test_each_producer_writes_only_the_pages_of_its_own_request(self) -> None:
        # Every flag-writing loop is bounded by the REQUEST's guest page range,
        # so no host-page rounding can reach a page the caller did not name.
        for signature in ("U64 KMemory64::mmapAnonymousFixed(",
                          "U64 KMemory64::mmapSharedFile(",
                          "U64 KMemory64::mprotect(",
                          "U64 KMemory64::munmap("):
            body = function_body(self.source, signature)
            self.assertIn("for (U64 i = 0; i < pageCount; i++)", body,
                          f"{signature} lost its request-bounded write loop")
            self.assertIn("writeFlags(", body,
                          f"{signature} no longer records who wrote the flags")

    def test_every_producer_names_itself(self) -> None:
        for writer, signature in (
                ("K64_WRITER_MMAP_ANON", "U64 KMemory64::mmapAnonymousFixed("),
                ("K64_WRITER_MMAP_FILE", "U64 KMemory64::mmapSharedFile("),
                ("K64_WRITER_MPROTECT", "U64 KMemory64::mprotect("),
                ("K64_WRITER_MUNMAP", "U64 KMemory64::munmap(")):
            self.assertIn(writer, function_body(self.source, signature))


class AHostPageCarriesTheUnionOfItsGuestPages(unittest.TestCase):
    """The consuming rule, in both directions. The host protection of a 16 KiB
    page is the union of what its four guest pages hold -- recomputed per host
    page, never a single value spread across an interval."""

    def setUp(self) -> None:
        self.source = read(KMEMORY_SOURCE)

    def test_no_producer_computes_a_host_protection_of_its_own(self) -> None:
        # The old mprotect took the union of the FIRST and LAST host page's
        # neighbours and applied that one value to every host page between
        # them, and mmapSharedFile applied `prot | 0x3` the same way. Both are
        # gone: nativeReconcileHostProt is the only caller of ::mprotect on a
        # guest-lane host page outside the fault repair.
        for signature in ("U64 KMemory64::mprotect(",
                          "U64 KMemory64::mmapSharedFile(",
                          "U64 KMemory64::munmap(",
                          "U64 KMemory64::mmapAnonymousFixed("):
            body = function_body(self.source, signature)
            self.assertNotIn("::mprotect((void*)", body,
                             f"{signature} still sets a host protection itself")
            self.assertIn("nativeReconcileHostProt(", body)

    def test_the_reconciliation_walks_one_host_page_at_a_time(self) -> None:
        body = function_body(
            self.source, "void KMemory64::nativeReconcileHostProt(")
        self.assertIn("for (U64 host = hostStart; host < hostEnd; host += hostPage)",
                      body)
        self.assertIn("nativeHostPageProtLocked(host, hostPage,", body)

    def test_an_untracked_host_page_is_skipped_and_never_fails_the_call(self) -> None:
        # This is the defect that cleared a committed page's rights: mprotect
        # asked nativeRangeCovers about the whole HOST-rounded interval and
        # returned -EINVAL for the entire request when an edge host page was
        # untracked, so the guest pages it named kept the flags of the
        # reservation they were being committed out of.
        mprotect = function_body(self.source, "U64 KMemory64::mprotect(")
        self.assertNotIn("nativeRangeCovers(hostStart, hostEnd)", mprotect)
        reconcile = function_body(
            self.source, "void KMemory64::nativeReconcileHostProt(")
        self.assertIn("!nativeRangeCovers(host, host + hostPage)", reconcile)
        self.assertIn("continue;", reconcile)

    def test_the_page_map_is_written_before_the_host_is_reconciled(self) -> None:
        # The reconciliation reads the map, so it has to run after the write or
        # it would restore the protection the request just changed.
        mprotect = function_body(self.source, "U64 KMemory64::mprotect(")
        write = mprotect.index("K64_WRITER_MPROTECT")
        reconcile = mprotect.index("nativeReconcileHostProt(reconcileStart")
        self.assertLess(write, reconcile)

    def test_a_guard_page_that_owns_its_host_page_is_representable(self) -> None:
        # Wine uses PROT_NONE guard pages deliberately. The union is 0 when all
        # four guest subpages grant nothing, and k64NativeProt(0) is PROT_NONE,
        # so such a host page genuinely faults. A guard page that SHARES a host
        # page with an accessible neighbour cannot be enforced at 16 KiB
        # granularity, and the union is the side to err on: revoking the
        # neighbour would kill a page the guest is entitled to.
        reconcile = function_body(
            self.source, "void KMemory64::nativeReconcileHostProt(")
        self.assertIn("k64NativeProt(runProt)", reconcile)
        native_prot = function_body(self.source, "static int k64NativeProt(")
        self.assertIn("int result = 0;", native_prot)
        union = function_body(
            self.source, "U32 KMemory64::nativeHostPageProtLocked(")
        self.assertIn("U32 unionProt = 0;", union)
        # Nothing may raise a page that grants nothing, except a pinned buffer
        # whose host pointer a blocked futex waiter still holds.
        self.assertIn("if (it->second->flags & K64_PAGE_PINNED) unionProt |= 0x3u;",
                      union)

    def test_an_executable_page_stays_host_readable(self) -> None:
        # K64_PAGE_EXEC is guest metadata, but the translator reads the bytes
        # of a page the guest may execute. Without the read term, an
        # execute-only guest page whose host page nothing else claims would
        # become PROT_NONE now that the union can lower a protection, and the
        # decoder would fault on its own instruction fetch.
        union = function_body(
            self.source, "U32 KMemory64::nativeHostPageProtLocked(")
        self.assertIn("if (it->second->flags & K64_PAGE_EXEC) unionProt |= 0x1u;",
                      union)

    def test_the_ledger_is_only_rewritten_for_the_named_interval(self) -> None:
        # `range.prot = ...` over every entry that merely OVERLAPPED the
        # interval rewrote the recorded protection of host pages the request
        # never named.
        self.assertNotIn("range.prot = ", self.source)
        track = function_body(
            self.source, "void KMemory64::nativeTrackRangeLocked(")
        self.assertIn("nativeForgetRange(start, length);", track)
        self.assertIn("nativeRanges.emplace(mergedStart,", track)

    def test_the_mapping_path_leaves_the_protection_to_the_reconciliation(self) -> None:
        # nativeMapAnonymous could only choose from plan.exactHostCover, a
        # property of the WHOLE interval: a request whose length was not a
        # multiple of the host page left every host page in it read/write, so a
        # PROT_NONE reservation was PROT_NONE nowhere.
        body = function_body(self.source, "bool KMemory64::nativeMapAnonymous(")
        self.assertNotIn("const int finalProt", body)
        self.assertNotIn("plan.exactHostCover ?", body)
        self.assertIn("nativeTrackRangeLocked(hostStart, hostLength, 0x3u)",
                      body)
        anon = function_body(self.source, "U64 KMemory64::mmapAnonymousFixed(")
        self.assertIn("nativeReconcileHostProt(", anon)


class ThePageRemembersWhoWroteItsRights(unittest.TestCase):
    """The witness the next device log needs: which operation last wrote the
    faulting guest page's flags, and what it wrote."""

    def setUp(self) -> None:
        self.header = read(KMEMORY_HEADER)
        self.source = read(KMEMORY_SOURCE)
        self.adapter = read(ADAPTER)

    def test_the_record_is_four_bytes_and_lives_on_the_page(self) -> None:
        struct = self.header.split("struct K64Page {", 1)[1].split("\n};", 1)[0]
        for field in ("U8  lastWriter", "U8  lastFlags", "U16 lastWriteStamp"):
            self.assertIn(field, struct)

    def test_every_operation_that_can_write_flags_has_a_name(self) -> None:
        for writer in ("K64_WRITER_NONE", "K64_WRITER_MMAP_ANON",
                       "K64_WRITER_MMAP_FILE", "K64_WRITER_MPROTECT",
                       "K64_WRITER_MUNMAP", "K64_WRITER_COMMIT",
                       "K64_WRITER_PIN", "K64_WRITER_CLONE"):
            self.assertIn(writer, self.header)
        names = function_body(self.source, "const char* k64PageWriterName(")
        for name in ("mmap-anon", "mmap-file", "mprotect", "munmap", "commit",
                     "pin", "clone", "none"):
            self.assertIn(f'"{name}"', names)

    def test_the_report_carries_the_faulting_page_and_a_neighbour(self) -> None:
        struct = self.header.split("struct K64NativeFaultRepair {", 1)[1]
        struct = struct.split("};", 1)[0]
        for field in ("lastWriter", "lastWriteFlags", "lastWriteStamp",
                      "neighbourWriter", "neighbourWriteFlags",
                      "neighbourWriteStamp"):
            self.assertIn(field, struct)

    def test_the_refusal_path_fills_them_in(self) -> None:
        # The case they exist for is the refusal, so they must be recorded
        # before nativeRepairHostFault returns false.
        body = function_body(
            self.source, "bool KMemory64::nativeRepairHostFault(")
        record = body.index("report.lastWriter = k64PageWriterName(")
        refusal = body.index('report.decision = "guest-protected"')
        self.assertLess(record, refusal)

    def test_the_witness_prints_them_and_is_still_bounded(self) -> None:
        line = self.adapter.split(WITNESS, 1)[1].split(");", 1)[0]
        for field in ("last_op=%s", "last_flags=0x%x", "last_seq=%u",
                      "nb_op=%s", "nb_flags=0x%x", "nb_seq=%u"):
            self.assertIn(field, line)
        body = self.adapter.split(
            "static bool repairGuestLaneHostFault(", 1)[1]
        self.assertIn("gAliasBackingReports.fetch_add(1, std::memory_order_relaxed) < 64",
                      body)

    def test_the_page_also_remembers_the_write_before_the_last_change(self) -> None:
        # Without this, lastWriter answers the wrong question. A page reserved
        # PROT_NONE and never used, and a page the guest committed and then
        # freed back into the reservation, both read
        # last_op=mmap-anon last_flags=0x20: Wine restores a freed view by
        # re-mmapping it PROT_NONE over the reserved arena, which is the same
        # producer writing the same value. Only the write before the last
        # change separates a wild pointer from a use-after-free.
        struct = self.header.split("struct K64Page {", 1)[1].split("\n};", 1)[0]
        for field in ("U8  priorWriter", "U8  priorFlags",
                      "U16 priorWriteStamp"):
            self.assertIn(field, struct)
        # And it only shifts on a write that CHANGES the value, so re-reserving
        # an already reserved page cannot erase the commit that came before.
        write = self.header.split(
            "void writeFlags(U32 value, U8 writer, U16 stamp) {", 1)[1]
        write = write.split("\n    }", 1)[0]
        self.assertIn("if (value != flags) {", write)
        self.assertIn("priorWriter = lastWriter;", write)
        self.assertIn("priorFlags = lastFlags;", write)
        self.assertIn("priorWriteStamp = lastWriteStamp;", write)

    def test_the_prior_write_reaches_the_witness(self) -> None:
        struct = self.header.split("struct K64NativeFaultRepair {", 1)[1]
        struct = struct.split("};", 1)[0]
        for field in ("priorWriter", "priorWriteFlags", "priorWriteStamp"):
            self.assertIn(field, struct)
        body = function_body(
            self.source, "bool KMemory64::nativeRepairHostFault(")
        record = body.index("report.priorWriter = k64PageWriterName(")
        refusal = body.index('report.decision = "guest-protected"')
        self.assertLess(record, refusal, "the refusal must carry the prior write")
        line = self.adapter.split(WITNESS, 1)[1].split(");", 1)[0]
        for field in ("prior_op=%s", "prior_flags=0x%x", "prior_seq=%u"):
            self.assertIn(field, line)


class ARefusedFaultNamesTheMemoryAroundIt(unittest.TestCase):
    """The remaining question a refusal cannot answer on its own.

    The device run at revision 957383ad refused guest 0xd88000 correctly: the
    page carried K64_PAGE_MAPPED and nothing else, its last writer was the
    anonymous PROT_NONE mmap that created it, and Wine -- whose own views tree
    is an independent ledger -- agreed, raising STATUS_ACCESS_VIOLATION and
    terminating with 0xc0000005 rather than committing anything. The syscall
    tail names no mmap, mprotect or madvise inside that page at all; the
    nearest are mmap(0xed0000, 0x910000, PROT_NONE) and
    mprotect(0xed0000, 0x910000, PROT_READ|PROT_WRITE), 0x148000 bytes above
    it, which we answered with the address asked for and 0.

    So the guest read memory it never committed and the refusal was right. What
    the witness could not say is HOW the guest got there, and the two shapes
    have different culprits: a cursor that walked off the end of a real buffer
    (the last readable page below the fault ends exactly at that buffer's end),
    versus a pointer that never addressed anything (nothing readable either
    way). The bounded scan below is what will tell them apart, and it runs only
    on a refusal -- the run repaired 3752 faults successfully before this one,
    and none of them may pay for it.
    """

    def setUp(self) -> None:
        self.header = read(KMEMORY_HEADER)
        self.source = read(KMEMORY_SOURCE)
        self.adapter = read(ADAPTER)
        self.body = function_body(
            self.source, "void KMemory64::nativeCommittedNeighbourhoodLocked(")

    def test_the_scan_is_bounded_in_both_directions(self) -> None:
        match = re.search(r"^#define K64_FAULT_NEIGHBOURHOOD_PAGES\s+(\d+)",
                          self.header, re.M)
        self.assertIsNotNone(match)
        self.assertLessEqual(int(match.group(1)), 4096,
                             "the scan runs in a signal handler")
        self.assertEqual(
            self.body.count("step <= K64_FAULT_NEIGHBOURHOOD_PAGES"), 2,
            "both directions must be bounded")
        # And it never walks below page zero.
        self.assertIn("if (step > guestPageNumber) break;", self.body)

    def test_only_a_readable_page_counts_as_committed(self) -> None:
        # A page that is mapped with no rights is part of the same reservation
        # the fault is in; counting it would report the hole as zero pages wide
        # and hide the very distinction the scan exists to draw.
        self.assertEqual(self.body.count("K64_PAGE_MAPPED"), 2)
        self.assertEqual(self.body.count("K64_PAGE_READ"), 2)

    def test_the_boundary_below_is_the_first_unreadable_address(self) -> None:
        # committedBelow is the END of the object the guest overran, not the
        # base of its last page: a loop that walked off a buffer stops exactly
        # there, and the buffer's size follows from its own base.
        self.assertIn(
            "report.committedBelow = (guestPageNumber - step + 1) << K64_PAGE_SHIFT;",
            self.body)
        self.assertIn(
            "report.committedAbove = (guestPageNumber + step) << K64_PAGE_SHIFT;",
            self.body)

    def test_only_a_refusal_pays_for_the_scan(self) -> None:
        # Every repaired fault would otherwise cost two thousand map lookups.
        repair = function_body(
            self.source, "bool KMemory64::nativeRepairHostFault(")
        self.assertEqual(
            repair.count("nativeCommittedNeighbourhoodLocked(guestPageNumber, report)"),
            2, "exactly the two page-map refusals scan")
        unmapped = repair.index('report.decision = "guest-unmapped"')
        scan = repair.index(
            "nativeCommittedNeighbourhoodLocked(guestPageNumber, report)")
        self.assertLess(scan, unmapped)
        # The successful path must not reach it at all: the scan sits behind
        # the entitlement decision.
        self.assertIn("if (!entitled) {\n            nativeCommittedNeighbourhoodLocked(",
                      repair)

    def test_the_scan_runs_under_the_lock_that_owns_the_page_map(self) -> None:
        # It reads `pages` directly, and nativeRepairHostFault calls it from
        # inside its own pagesMutex section rather than taking the lock twice.
        declaration = self.header.split(
            "void nativeCommittedNeighbourhoodLocked(", 1)[0]
        self.assertIn("Caller holds pagesMutex.", declaration[-600:])
        self.assertNotIn("BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX", self.body)

    def test_the_witness_prints_both_boundaries(self) -> None:
        struct = self.header.split("struct K64NativeFaultRepair {", 1)[1]
        struct = struct.split("};", 1)[0]
        for field in ("committedBelow", "committedAbove"):
            self.assertIn(field, struct)
        line = self.adapter.split(WITNESS, 1)[1].split(");", 1)[0]
        for field in ("commit_below=0x%llx", "commit_above=0x%llx"):
            self.assertIn(field, line)


class TheHostReprotectWitnessIsBounded(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(KMEMORY_SOURCE)

    def test_the_volume_of_the_new_host_witness_is_bounded_too(self) -> None:
        reconcile = function_body(
            self.source, "void KMemory64::nativeReconcileHostProt(")
        self.assertIn("BOXEDWINE_X64_HOST_REPROTECT", reconcile)
        self.assertIn("reprotectFailures.fetch_add(std::memory_order_relaxed"
                      .replace("std::memory_order_relaxed",
                               "1, std::memory_order_relaxed"),
                      reconcile)
        self.assertIn("16)", reconcile)


if __name__ == "__main__":
    unittest.main()
