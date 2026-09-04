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

These tests hold the reconciliation in place: the page map stays the authority,
the repair never grants more than the page map grants, the retry is bounded,
and the witness that will identify the producing ledger keeps printing every
fact needed to tell "no host page" from "host page, wrong protection".

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
        self.assertIn("for (U64 sub = 0; sub < hostPage; sub += K64_PAGE_SIZE)",
                      self.body)
        # `pages` is keyed by CANONICAL guest page, so the host page has to be
        # translated back before its subpages can be named.
        self.assertIn("k64HostToGuestAddress(hostStart)", self.body)

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
        # Otherwise the next mprotect over this page refuses for want of
        # coverage and the next munmap leaves the host page behind.
        self.assertIn("nativeForgetRange(hostStart, hostPage)", self.body)
        self.assertIn("nativeRanges.emplace(hostStart,", self.body)
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


if __name__ == "__main__":
    unittest.main()
