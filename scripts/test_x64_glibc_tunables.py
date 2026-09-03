"""BoxedVN - contract tests for the x86-64 guest launch environment.

Four device runs ended the same way. The launched process runs on FEX, which
advertises AVX/AVX2/FMA through CPUID, so glibc's dynamic linker resolved that
process's IFUNC string and memory routines to the AVX2 variants. Every process
forked from it cannot run on FEX -- a second identity mapping of the guest
address space is impossible -- so it runs on the x86-64 interpreter, which
implements SSE2 only. Wine double-forks before every exec, and the grandchild
died inside libc before it ever reached execve:

    CPU64: unimpl opcode at RIP=0x7a40196d64 bytes=c5 f9 6e c6 89 f8 25

which is `vmovd xmm0, esi`, the prologue of an AVX2 string routine. Wine could
therefore start no child at all: no explorer.exe, no services, no start.exe.

The fix is GLIBC_TUNABLES in the launch environment, which ld.so reads in every
process of the session, while CPUID keeps advertising AVX to the program itself.
These tests hold the three properties that fix depends on: the tunables name
every feature that would select a VEX-encoded routine (spelled the way the
packaged glibc accepts), the launch composes them in the one place every launch
path goes through, and CPUID is still advertising AVX.

They also cover the second half of that launch environment -- the DXVK d3d9
override, which used to decline whenever the caller had set WINEDLLOVERRIDES,
which is always -- and the interpreter's illegal-instruction ending, which used
to park the thread with no exit status at all.

Source-level contracts on purpose: building the emulator needs the iOS
toolchain, and the C++ behaviour of the composition helpers is checked in
ios/tests/test_guest_wine64_layout.cpp where a compiler exists.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
LAYOUT_HEADER = REPO / "include" / "guest_wine64_layout.h"
STARTUP_ARGS = REPO / "source" / "sdl" / "startupArgs.cpp"
CPU64 = REPO / "source" / "emulation" / "cpu" / "cpu64.cpp"
KSIGNAL = REPO / "include" / "ksignal.h"
FEX_BACKEND = REPO / "ios" / "runtime" / "src" / "BVNFEXBackend.mm"
RUNTIME_BUILDER = REPO / "scripts" / "build-wine64-runtime-ci.sh"
LAYOUT_TESTS = REPO / "ios" / "tests" / "test_guest_wine64_layout.cpp"

# Every CPU feature whose presence would make glibc pick a VEX-encoded IFUNC
# variant, plus the two that decide which lazy-binding trampoline ld.so uses.
# These spellings are the ones glibc 2.39 accepts: its sysdeps/x86/cpu-tunables.c
# matches AVX, AVX2, FMA and FMA4 (lengths 3 and 4), XSAVE and XSAVEC (5 and 6)
# and AVX_Fast_Unaligned_Load (23). The packaged rootfs is assembled on an
# Ubuntu 24.04 runner, which is glibc 2.39.
REQUIRED_HWCAPS = (
    "AVX",
    "AVX2",
    "AVX_Fast_Unaligned_Load",
    "FMA",
    "FMA4",
    "XSAVE",
    "XSAVEC",
)

# The overrides the app itself passes, and therefore the case that runs on a
# device. See ios/app/Sources/AppModel.swift.
APP_WINEDLLOVERRIDES = "d3d11,dxgi,d3d10core,winemetal=n,b"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def macro_value(source: str, name: str, _seen: frozenset = frozenset()) -> str:
    """What a #define expands to: adjacent string literals concatenated, and
    any macro named in the body expanded the way the preprocessor would."""
    if name in _seen:
        raise AssertionError(f"{name} expands to itself")
    match = re.search(
        r"^#define[ \t]+" + re.escape(name) + r"[ \t]((?:[^\n\\]*\\\r?\n)*[^\n]*)$",
        source, re.MULTILINE)
    if match is None:
        raise AssertionError(f"{name} is not defined")
    body = re.sub(r"\\\r?\n", " ", match.group(1))
    value = ""
    for literal, identifier in re.findall(
            r'"((?:[^"\\]|\\.)*)"|([A-Za-z_][A-Za-z0-9_]*)', body):
        if identifier:
            value += macro_value(source, identifier, _seen | {name})
        else:
            value += literal
    return value


class GlibcHwcapsTunable(unittest.TestCase):
    def setUp(self) -> None:
        self.header = read(LAYOUT_HEADER)

    def test_every_vex_selecting_feature_is_disabled(self) -> None:
        hwcaps = macro_value(self.header, "K_X64_GUEST_GLIBC_HWCAPS")
        entries = hwcaps.split(",")
        for feature in REQUIRED_HWCAPS:
            self.assertIn(f"-{feature}", entries,
                          f"glibc would still be free to pick a {feature} routine")
        for entry in entries:
            self.assertTrue(entry.startswith("-"),
                            f"{entry!r} would ENABLE a feature, not disable it")

    def test_no_feature_name_the_packaged_glibc_does_not_know(self) -> None:
        # An unknown name is ignored rather than rejected, so a wrong spelling
        # reads as protection that is not actually there. F16C is the near
        # miss: real CPUID bit, no glibc tunable.
        hwcaps = macro_value(self.header, "K_X64_GUEST_GLIBC_HWCAPS")
        for entry in hwcaps.split(","):
            self.assertIn(entry.lstrip("-"), REQUIRED_HWCAPS,
                          f"{entry!r} is not a glibc 2.39 hwcaps name")

    def test_the_tunable_is_spelled_the_way_ld_so_reads_it(self) -> None:
        self.assertEqual(
            macro_value(self.header, "K_X64_GUEST_GLIBC_HWCAPS_TUNABLE"),
            "glibc.cpu.hwcaps=" +
            macro_value(self.header, "K_X64_GUEST_GLIBC_HWCAPS"))
        self.assertEqual(
            macro_value(self.header, "K_X64_GUEST_GLIBC_TUNABLES_NAME"),
            "GLIBC_TUNABLES")

    def test_the_rootfs_this_was_spelled_for_is_the_one_that_is_packaged(self) -> None:
        # The tunable names differ between glibc 2.32 and 2.33. If the runtime
        # ever stops being assembled on Ubuntu 24.04 (glibc 2.39), the spelling
        # above has to be revisited rather than silently kept.
        self.assertIn("source_image=ubuntu-24.04-apt", read(RUNTIME_BUILDER))


class LaunchEnvironmentComposition(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(STARTUP_ARGS)

    def test_the_tunables_are_added_where_every_x64_launch_passes(self) -> None:
        # One place, inside the FEX64 block that composes the 64-bit launch
        # environment, so the desktop, both cube probes and "Run program..."
        # all get it without each knowing about it.
        self.assertIn("static void addGuestGlibcTunables(", self.source)
        block = self.source.split("if (requestedFEX64) {", 1)
        self.assertEqual(len(block), 2, "the FEX64 launch block moved")
        self.assertIn("addGuestGlibcTunables(envValues);", block[1])

    def test_a_callers_tunables_are_preserved(self) -> None:
        # The composition itself is a pure helper in the header, tested in C++;
        # what matters here is that startupArgs calls it rather than pushing a
        # second GLIBC_TUNABLES beside the caller's.
        self.assertIn("boxedvn::guestGlibcTunablesValue(", self.source)
        self.assertNotIn('envValues.push_back(B("GLIBC_TUNABLES=', self.source)

    def test_the_launch_says_what_the_guest_will_carry(self) -> None:
        self.assertIn("BOXEDWINE_X64_GLIBC_TUNABLES value=%s", self.source)

    def test_the_dxvk_d3d9_override_is_merged_not_declined(self) -> None:
        # The app always sets WINEDLLOVERRIDES, so the old decline branch was
        # the one that ran and the projected DXVK image was never loaded.
        self.assertNotIn("status=override-kept: the launch already set",
                         self.source)
        self.assertIn("mergeWow64DxvkD3d9Override(envValues);", self.source)
        self.assertIn("boxedvn::wineDllOverridesWithDxvkD3d9(", self.source)
        self.assertIn("status=%s", self.source)

    def test_the_merged_override_keeps_the_apps_modules_and_adds_d3d9(self) -> None:
        header = read(LAYOUT_HEADER)
        entry = macro_value(header, "K_X64_WOW64_D3D9_NATIVE_OVERRIDE")
        separator = macro_value(header, "K_WINE_DLL_OVERRIDES_SEPARATOR")
        self.assertEqual(entry, "d3d9=n")
        self.assertEqual(separator, ";", "Wine separates override entries with ';'")
        self.assertEqual(APP_WINEDLLOVERRIDES + separator + entry,
                         "d3d11,dxgi,d3d10core,winemetal=n,b;d3d9=n")


class CpuidStillAdvertisesAvx(unittest.TestCase):
    def test_the_guest_program_still_sees_avx(self) -> None:
        # The tunables are what keeps glibc off the VEX routines. Reverting the
        # CPUID change would be the other way to fix the helpers, and it would
        # break the 64-bit program, which needs the feature bits.
        source = read(FEX_BACKEND)
        self.assertIn("BOXEDWINE_FEX64_CPUID", source)
        self.assertIn("avx=%u avx2=%u fma=%u", source)


class IllegalInstructionEnding(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(CPU64)

    def test_the_opcode_is_still_named(self) -> None:
        self.assertIn("CPU64: unimpl opcode at RIP=", self.source)

    def test_the_guest_gets_a_sigill_it_can_handle(self) -> None:
        self.assertIn("raiseSyncFault(K_SIGILL,", self.source)
        self.assertIn("K_ILL_ILLOPN", self.source)
        self.assertIn("K_ILL_ILLOPN\t2", read(KSIGNAL))

    def test_an_unhandled_one_ends_the_process_with_a_status(self) -> None:
        # Without this the thread simply stopped: the pid stayed alive with no
        # status, no lifecycle marker, and a parent in waitpid never woke.
        tail = self.source.split("CPU64: unimpl opcode at RIP=", 1)[1]
        self.assertIn("thread->process->signaled = K_SIGILL;", tail)
        self.assertIn('kfatalProcessExit64(this, 128 + K_SIGILL,', tail)
        self.assertIn('"cpu64-illegal-instruction"', tail)

    def test_the_fault_is_reported_at_the_faulting_instruction(self) -> None:
        tail = self.source.split("CPU64: unimpl opcode at RIP=", 1)[1]
        self.assertIn("rip = ipStart;", tail)


class TheBehaviourIsCoveredWhereACompilerExists(unittest.TestCase):
    def test_the_cpp_tests_exercise_the_composition_helpers(self) -> None:
        source = read(LAYOUT_TESTS)
        for helper in ("guestGlibcTunablesValue",
                       "glibcTunablesSetHwcaps",
                       "wineDllOverridesWithDxvkD3d9",
                       "wineDllOverridesNameModule"):
            self.assertIn(helper, source,
                          f"{helper} has no compiled test")


if __name__ == "__main__":
    unittest.main()
