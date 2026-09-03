"""BoxedVN - contract tests for the legacy i386 `int 0x80` gate in 64-bit code.

A device run launched a 32-bit program from the 64-bit desktop. Wine started it
as a child process, which therefore runs on the x86-64 interpreter rather than
on the translator, and it died at once:

    CPU64: unimpl opcode at RIP=0x7002546ca bytes=cd 80 8b 04 24 4c 89
    BOXEDWINE_X64_FATAL_EXIT pid=45 status=132 reason='cpu64-illegal-instruction'

`cd 80` is `int $0x80`. Wine's 64-bit unix ntdll allocates a WoW64 thread's
32-bit FS/GS selectors with set_thread_area, which has no x86-64 syscall number
at all, so it reaches the i386 gate -- in every process that starts a 32-bit
program. The translator backend already served that instruction from the general
protection fault it raises; the interpreter had no decoding for opcode 0xCD.

These tests pin the shape of the fix rather than its behaviour: the gate's
semantics live in ONE kernel-side helper that both backends call, the
interpreter decodes the three things that share this encoding (the gate, the
breakpoint, and every other vector) instead of dying on them, and the selector
writes that follow set_thread_area resolve their base from the descriptor the
gate installed.

Source-level contracts on purpose: building the emulator needs the iOS
toolchain, and there is no host compiler here.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GATE_HEADER = REPO / "include" / "legacysyscall64.h"
GATE_SOURCE = REPO / "source" / "kernel" / "legacysyscall64.cpp"
CPU64_HEADER = REPO / "include" / "cpu64.h"
CPU64 = REPO / "source" / "emulation" / "cpu" / "cpu64.cpp"
KSIGNAL = REPO / "include" / "ksignal.h"
SEGMENT_TABLE = REPO / "include" / "guest_segment_table.h"
FEX_ADAPTER = REPO / "ios" / "runtime" / "src" / "BVNFEXCPU64Adapter.mm"
SOURCE_LIST = REPO / "cmake" / "BoxedwineSources.cmake"

# The helper both backends call, and the one that resolves a selector's base.
HELPER = "kernelLegacySyscall64"
SEGMENT_HELPER = "kernelLegacySegmentBase64"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def defined_value(source: str, name: str) -> str:
    match = re.search(
        r"^#define[ \t]+" + re.escape(name) + r"[ \t]+([^\s/]+)", source,
        re.MULTILINE)
    if match is None:
        raise AssertionError(f"{name} is not defined")
    return match.group(1)


class TheGateHasOneKernelSideImplementation(unittest.TestCase):
    def setUp(self) -> None:
        self.header = read(GATE_HEADER)
        self.source = read(GATE_SOURCE)

    def test_the_helper_is_declared_for_both_backends(self) -> None:
        # A header in include/ rather than a static in either backend: the FEX
        # adapter and the interpreter must be able to call the same function.
        self.assertIn("namespace boxedvn", self.header)
        self.assertIn(f"LegacySyscallResult {HELPER}(CPU64* cpu);", self.header)
        self.assertIn(f"U64 {SEGMENT_HELPER}(CPU64* cpu, U16 selector);",
                      self.header)

    def test_the_helper_is_defined_in_the_kernel(self) -> None:
        self.assertIn(f"LegacySyscallResult {HELPER}(CPU64* cpu) {{",
                      self.source)
        self.assertIn(f"U64 {SEGMENT_HELPER}(CPU64* cpu, U16 selector) {{",
                      self.source)

    def test_the_i386_numbers_are_the_i386_ones(self) -> None:
        # 243 is set_thread_area on i386 and io_setup on x86-64. Reading the
        # gate's number out of the x86-64 table is the mistake the gate exists
        # to avoid, so the three numbers are pinned here.
        self.assertEqual(defined_value(self.header,
                                       "K_LEGACY_I386_SYS_set_thread_area"),
                         "243")
        self.assertEqual(defined_value(self.header,
                                       "K_LEGACY_I386_SYS_get_thread_area"),
                         "244")
        self.assertEqual(defined_value(self.header,
                                       "K_LEGACY_I386_SYS_modify_ldt"), "123")

    def test_the_tls_slots_are_the_ones_linux_hands_to_user_space(self) -> None:
        # GDT_ENTRY_TLS_MIN..MAX. A slot outside this range is EINVAL, and the
        # table it indexes is the one the translator reads.
        self.assertEqual(defined_value(self.header, "K_GUEST_TLS_ENTRY_MIN"),
                         "12")
        self.assertEqual(defined_value(self.header, "K_GUEST_TLS_ENTRY_MAX"),
                         "14")
        self.assertEqual(
            defined_value(self.header, "K_LEGACY_SYSCALL_INSTRUCTION_LENGTH"),
            "2", "`int $0x80` is two bytes and the gate resumes after them")
        self.assertIn("#define K_GUEST_SEGMENT_TABLE_ENTRIES 32",
                      read(SEGMENT_TABLE))

    def test_the_three_served_numbers_are_dispatched(self) -> None:
        for number in ("K_LEGACY_I386_SYS_set_thread_area",
                       "K_LEGACY_I386_SYS_get_thread_area",
                       "K_LEGACY_I386_SYS_modify_ldt"):
            self.assertIn(f"case {number}:", self.source,
                          f"{number} reaches no handler")

    def test_an_unserved_number_is_enosys_and_is_reported_once(self) -> None:
        # A guest that retries an unserved number does so in a loop; one line
        # per number is what keeps that from filling a device's disk while
        # still naming every number that was refused.
        self.assertIn("BOXEDWINE_X64_LEGACY_SYSCALL", self.source)
        self.assertIn("status=enosys", self.source)
        self.assertIn("nr=%u status=enosys", self.source)
        self.assertIn("out.result = -K_ENOSYS;", self.source)
        self.assertIn("reportUnservedLegacySyscall(cpu, out.number);",
                      self.source)
        report = self.source.split("void reportUnservedLegacySyscall", 1)[1]
        self.assertIn("fetch_or(bit", report,
                      "the report has to be keyed on the number, once each")

    def test_the_answer_goes_back_in_eax_sign_extended(self) -> None:
        # The compat entry stores the i386 result in rax sign-extended, which is
        # what makes a negative errno compare as negative in the caller.
        self.assertIn("cpu->reg[X64_RAX].u64 = (U64)(S64)out.result;",
                      self.source)

    def test_the_helper_does_not_advance_rip(self) -> None:
        # Each backend advances its own program counter: the interpreter counts
        # instruction bytes for its dispatch, the adapter resumes the
        # translator. A helper that also advanced it would double-count.
        self.assertNotIn("cpu->rip +=", self.source)

    def test_the_new_translation_unit_is_compiled(self) -> None:
        # The core is globbed, so a new file under source/ is built without a
        # list edit. If that ever stops being true this test says so.
        self.assertIn('"${BOXEDWINE_ROOT}/source/*.cpp"', read(SOURCE_LIST))


class TheInterpreterDecodesTheSoftwareInterrupts(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(CPU64)

    def test_the_opcode_reaches_a_block_instead_of_unhandled(self) -> None:
        # The dispatch jump table sends an opcode to the first block that could
        # match it; 0xcd used to be in the list that goes straight to
        # `unhandled`, which is where the device run died.
        self.assertIn("case 0xcc: case 0xcd: goto dsp_49;", self.source)
        table = self.source.split("---- Dispatch jump table ----", 1)[1]
        table = table.split("---- Single-byte opcodes ----", 1)[0]
        # The arm that goes straight to `unhandled` is the last one: everything
        # after the final `goto dsp_` and before `goto unhandled;`.
        unhandled = table.rpartition("goto dsp_")[2].split("goto unhandled;",
                                                           1)[0]
        self.assertIn("case 0x06:", unhandled, "the unhandled arm moved")
        self.assertNotIn("case 0xcd:", unhandled,
                         "0xcd is still routed to the illegal-instruction path")
        self.assertIn("goto unhandled;", self.source,
                      "the illegal-instruction path itself must stay")

    def test_the_gate_is_served_through_the_shared_helper(self) -> None:
        block = self.source.split("dsp_49:", 1)[1]
        self.assertIn("vector == 0x80", block)
        self.assertIn(f"boxedvn::{HELPER}(this);", block,
                      "the interpreter must not have its own gate semantics")

    def test_a_breakpoint_is_a_sigtrap_the_guest_can_handle(self) -> None:
        block = self.source.split("dsp_49:", 1)[1]
        self.assertIn("raiseSyncFault(K_SIGTRAP, /*trapNo #BP*/3, K_TRAP_BRKPT",
                      block)
        # Unhandled, it ends the process the way every other fatal fault does:
        # with a status a waiting parent can read.
        self.assertIn("thread->process->signaled = K_SIGTRAP;", block)
        self.assertIn('kfatalProcessExit64(this, 128 + K_SIGTRAP,', block)
        self.assertIn('"cpu64-breakpoint-trap"', block)

    def test_int3_no_longer_yields_silently(self) -> None:
        # The old ending left the process alive with no status at all, which
        # reads in a log as the helper vanishing.
        self.assertNotIn("no SIGTRAP delivery yet", self.source)

    def test_any_other_vector_is_a_sigsegv(self) -> None:
        block = self.source.split("dsp_49:", 1)[1]
        self.assertIn("raiseSyncFault(K_SIGSEGV, /*trapNo #GP*/13, K_SI_KERNEL",
                      block)
        self.assertIn('kfatalProcessExit64(this, 128 + K_SIGSEGV,', block)
        self.assertIn('"cpu64-software-interrupt"', block)

    def test_the_si_codes_are_the_kernels(self) -> None:
        ksignal = read(KSIGNAL)
        self.assertEqual(defined_value(ksignal, "K_TRAP_BRKPT"), "1")
        self.assertEqual(defined_value(ksignal, "K_SI_KERNEL"), "0x80")


class TheSelectorWritesResolveTheirBase(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(CPU64)
        self.header = read(CPU64_HEADER)

    def test_the_descriptors_live_on_the_thread(self) -> None:
        self.assertIn("boxedvn::LegacyThreadAreaDescriptor", self.header)
        self.assertIn("threadAreaDescriptors[K_GUEST_SEGMENT_TABLE_ENTRIES]",
                      self.header)

    def test_a_cloned_thread_inherits_them(self) -> None:
        # A child that inherited FS without the descriptor FS names would
        # address its TEB through a zero base.
        clone = self.source.split("void CPU64::cloneRegistersFrom", 1)[1]
        clone = clone.split("\n}", 1)[0]
        self.assertIn("threadAreaDescriptors[i] = from->threadAreaDescriptors[i];",
                      clone)

    def test_mov_to_fs_or_gs_takes_the_descriptor_base(self) -> None:
        # This is the instruction Wine executes immediately after
        # set_thread_area; it used to read the operand and discard it.
        self.assertIn("void CPU64::loadSegmentSelector(bool fs, U16 selector) {",
                      self.source)
        self.assertIn(f"boxedvn::{SEGMENT_HELPER}(this, selector);", self.source)
        block = self.source.split("dsp_11:", 1)[1].split("dsp_12:", 1)[0]
        self.assertIn("loadSegmentSelector(segIdx == 4, selector);", block)
        self.assertNotIn("(void)loadRM(m, 2, rexPresent);", block,
                         "the selector write is discarded again")

    def test_pop_fs_and_pop_gs_are_decoded(self) -> None:
        # The only other instructions that can write FS or GS; LFS and LGS are
        # not encodable in long mode.
        self.assertIn("segOp == 0xA0 || segOp == 0xA1 || segOp == 0xA8 || segOp == 0xA9",
                      self.source)
        self.assertIn("loadSegmentSelector(isFs, selector);", self.source)


class BothBackendsAnswerTheSameInstruction(unittest.TestCase):
    def test_the_adapter_serves_the_gate_on_the_same_terms(self) -> None:
        # The adapter is edited by whoever owns that file; this holds the
        # property that matters either way. Once it calls the shared helper the
        # first branch passes; until then its own implementation has to agree
        # with the helper about which slots set_thread_area may use.
        adapter = read(FEX_ADAPTER)
        self.assertIn("emulateLegacySyscall", adapter,
                      "nothing serves the gate on the translator side")
        if HELPER in adapter:
            return
        header = read(GATE_HEADER)
        low = defined_value(header, "K_GUEST_TLS_ENTRY_MIN")
        high = defined_value(header, "K_GUEST_TLS_ENTRY_MAX")
        self.assertRegex(adapter, r"number == 243",
                         "the adapter no longer serves set_thread_area")
        self.assertRegex(
            adapter, rf"entry < {low} \|\| entry > {high}",
            "the adapter's own slot range no longer matches the helper's")
        self.assertIn("K_GUEST_SEGMENT_TABLE_ENTRIES", adapter)

    def test_the_adapter_serves_the_selector_writes_too(self) -> None:
        adapter = read(FEX_ADAPTER)
        self.assertIn("emulateSegmentSelectorWrite", adapter)


if __name__ == "__main__":
    unittest.main()
