"""BoxedVN - contract tests for Wine's debug output on the 64-bit lane.

The 64-bit lane produced no Wine debug output at all. Not a trace, not a warn,
not even an `err:` or `fixme:` -- and those two are on by default, without any
WINEDEBUG. The environment arrived (every exec logged it), and a write to
guest descriptor 2 from a PE module reached the log (the translation layer's
own two lines prove that), so neither end of the chain was broken.

The middle was. Wine initialises its debug channels lazily, on the first
TRACE/WARN/ERR/FIXME any module writes, and the first thing that
initialisation does is ask whether its own stderr is /dev/null. Read it in the
module we ship, x86_64-unix/ntdll.so, at init_options -- reached from
__wine_dbg_get_channel_flags:

    getenv("WINEDEBUG")            -> kept in a register, not used yet
    fstat(2, &st1)
    if (S_ISCHR(st1.st_mode) &&
        stat("/dev/null", &st2) == 0 && S_ISCHR(st2.st_mode) &&
        st1.st_rdev == st2.st_rdev) {
            default_flags = 0;     /* err and fixme included */
            return;                /* WINEDEBUG is never parsed */
    }

This lane lost that comparison for a reason that has nothing to do with Wine:
the 64-bit stat writer answered st_rdev 0 for every file. Descriptor 2 is
/dev/tty0, a character device; /dev/null is a character device; both reported
device 0; so every 64-bit process muted itself before the environment was ever
read. The 32-bit lane never lost it -- its stat writer passes the node's rdev
-- and the Wine messages its captures do contain ("wine client error:") are
fprintf(stderr) in ntdll's server code, not debug channels, so that lane was
never evidence either way.

Source-level contracts on purpose: building the emulator needs the iOS
toolchain, and no compiler runs on the host these were written on. What is
checked here is that the two stats now answer differently, that the answer
comes from the node rather than a constant, and that a log will say which way
the comparison went.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SYSCALL64 = REPO / "source" / "kernel" / "syscall64.cpp"
STARTUP_ARGS = REPO / "source" / "sdl" / "startupArgs.cpp"
KSTAT = REPO / "include" / "kstat.h"
FS_HEADER = REPO / "source" / "io" / "fs.h"
BVN_RUNTIME_HEADER = REPO / "ios" / "runtime" / "include" / "BVNRuntime.h"
BVN_PATHS = REPO / "ios" / "runtime" / "src" / "BVNPaths.mm"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def function_body(source: str, signature_start: str) -> str:
    """The text from a function's definition to its closing brace. Enough to
    assert on what a function does without a parser. A forward declaration is
    skipped: its signature is followed by a `;`, not a `{`."""
    index = -1
    for candidate in re.finditer(re.escape(signature_start), source):
        rest = source[candidate.start():]
        brace = rest.find("{")
        semicolon = rest.find(";")
        if brace != -1 and (semicolon == -1 or brace < semicolon):
            index = candidate.start()
            break
    if index == -1:
        raise AssertionError(f"no definition of {signature_start!r}")
    depth = 0
    seen_brace = False
    for position in range(index, len(source)):
        character = source[position]
        if character == "{":
            depth += 1
            seen_brace = True
        elif character == "}":
            depth -= 1
            if seen_brace and depth == 0:
                return source[index:position + 1]
    raise AssertionError(f"unterminated function at {signature_start!r}")


def virtual_file_registration(source: str, path: str) -> str:
    match = re.search(
        r"Fs::addVirtualFile\(B\(\"" + re.escape(path) + r"\"\)[^;]*;",
        source)
    if match is None:
        raise AssertionError(f"{path} is not registered")
    return match.group(0)


def registered_device(source: str, path: str) -> tuple[int, int]:
    """The (major, minor) a virtual file is registered with."""
    registration = virtual_file_registration(source, path)
    match = re.search(r"k_mdev\((\d+),\s*(\d+)\)", registration)
    if match is None:
        raise AssertionError(f"{path} is registered without a device number")
    return int(match.group(1)), int(match.group(2))


def mdev(major: int, minor: int) -> int:
    """The same arithmetic as the k_mdev macro, read from the header so a
    change to the encoding fails here instead of silently passing."""
    definition = re.search(
        r"^#define k_mdev\(x,y\)[ \t]+\(\(x <<[ \t]*(\d+)\)[ \t]*\|[ \t]*y\)$",
        read(FS_HEADER), re.MULTILINE)
    if definition is None:
        raise AssertionError("k_mdev is not defined the way this test reads it")
    return (major << int(definition.group(1))) | minor


class TheTwoStatsWineCompares(unittest.TestCase):
    """The rule Wine applies, applied to the numbers this emulator registers."""

    def setUp(self) -> None:
        self.startup = read(STARTUP_ARGS)
        self.kstat = read(KSTAT)

    def character_device_bit(self) -> int:
        match = re.search(r"^#define K__S_IFCHR[ \t]+(0x[0-9A-Fa-f]+)$",
                          self.kstat, re.MULTILINE)
        assert match is not None, "K__S_IFCHR is not defined"
        return int(match.group(1), 16)

    def test_stderr_and_dev_null_are_both_character_devices(self) -> None:
        # Not a detail: if either stopped being one, Wine's check would pass
        # for a different reason and this whole file would be testing nothing.
        for path in ("/dev/tty0", "/dev/null"):
            registration = virtual_file_registration(self.startup, path)
            self.assertIn("K__S_IFCHR", registration,
                          f"{path} is no longer a character device")

    def test_stderr_and_dev_null_are_different_devices(self) -> None:
        # This inequality is the whole of Wine's decision to keep talking.
        tty = mdev(*registered_device(self.startup, "/dev/tty0"))
        null = mdev(*registered_device(self.startup, "/dev/null"))
        self.assertNotEqual(
            tty, null,
            "stderr and /dev/null report the same device, so Wine will clear "
            "its default channel flags and never read WINEDEBUG")

    def test_the_initial_stderr_is_the_device_this_test_measured(self) -> None:
        # A process inherits descriptor 2 from KProcess::initStdio.
        stdio = read(REPO / "source" / "kernel" / "kprocess.cpp")
        setup = function_body(stdio, "void KProcess::initStdio() {")
        self.assertIn('B("/dev/tty0"), K_O_WRONLY, 0, 2', setup,
                      "descriptor 2 is no longer /dev/tty0")


class TheStatWriterReportsADevice(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(SYSCALL64)

    def test_the_writer_takes_a_device_number(self) -> None:
        self.assertIn(
            "U64 ino, U32 uid, U32 gid, U64 mtime, U64 rdev) {", self.source,
            "writeStatBuf64 no longer takes the device number")

    def test_the_device_number_lands_in_st_rdev(self) -> None:
        body = function_body(self.source, "static void writeStatBuf64(")
        match = re.search(r"put64\(40, ([^)]*)\);", body)
        self.assertIsNotNone(match, "st_rdev is no longer written at offset 40")
        self.assertEqual(match.group(1).strip(), "rdev",
                         "st_rdev is a constant again")

    def test_no_caller_passes_a_constant_device(self) -> None:
        for call in re.finditer(r"writeStatBuf64\((?:[^;]*?)\);", self.source):
            text = " ".join(call.group(0).split())
            if text.startswith("writeStatBuf64(KMemory64"):
                continue  # the definition itself
            self.assertTrue(
                text.endswith("rdev);") or text.endswith("node->rdev);"),
                f"a caller supplies its own device number: {text}")

    def test_the_path_stat_asks_the_node(self) -> None:
        body = function_body(self.source, "static U64 sys_stat_path64(")
        self.assertIn("node->rdev", body,
                      "stat() no longer reports the node's device")

    def test_the_descriptor_stat_asks_the_node(self) -> None:
        body = function_body(self.source, "static U64 sys_fstat64(")
        self.assertIn("rdev  = kfile->openFile->node->rdev;", body,
                      "fstat() no longer reports the node's device")

    def test_a_pipe_is_not_a_device(self) -> None:
        # The non-file branch claims S_IFCHR so callers do not assume a
        # seekable object. It must not also claim /dev/null's device number,
        # or a Wine process whose stderr is a pipe would mute itself.
        body = function_body(self.source, "static U64 sys_fstat64(")
        self.assertIn("U64 rdev = 0;", body,
                      "the non-file branch no longer starts from device 0")
        startup = read(STARTUP_ARGS)
        self.assertNotEqual(
            0, mdev(*registered_device(startup, "/dev/null")),
            "/dev/null now reports device 0, which is what a pipe reports")


class TheComparisonIsWitnessed(unittest.TestCase):
    """A log has to say which way the comparison went, not leave it to be
    inferred from an absence of output -- which is what cost the last run."""

    def setUp(self) -> None:
        self.source = read(SYSCALL64)

    def test_both_halves_are_reported(self) -> None:
        self.assertIn('"stderr", 1,', self.source)
        self.assertIn('"/dev/null",\n', self.source.replace("\r\n", "\n"))

    def test_the_line_carries_the_two_numbers_that_decide(self) -> None:
        body = function_body(self.source,
                             "void reportWineDebugStderrIdentity(")
        for field in ("subject=%s", "mode=0%o", "rdev=0x%llx", "chr=%d"):
            self.assertIn(field, body, f"the witness dropped {field}")
        self.assertIn("BOXEDWINE_X64_WINEDEBUG", body)

    def test_the_witness_is_one_line_per_process_per_subject(self) -> None:
        claim = function_body(self.source, "bool claimWineDebugStderrWitness(")
        self.assertIn("return false;", claim,
                      "the witness would repeat for every stat")
        report = function_body(self.source,
                               "void reportWineDebugStderrIdentity(")
        self.assertIn("if (!claimWineDebugStderrWitness(pid, slot)) {", report,
                      "the witness is no longer bounded")

    def test_the_witness_is_locked(self) -> None:
        claim = function_body(self.source, "bool claimWineDebugStderrWitness(")
        self.assertIn("std::lock_guard", claim,
                      "the seen-list is shared by every guest thread")

    def test_stderr_is_the_descriptor_reported(self) -> None:
        body = function_body(self.source, "static U64 sys_fstat64(")
        self.assertIn("if (fd == 2) {", body,
                      "the witness no longer fires on the descriptor Wine asks "
                      "about")


class TheMetalCacheDirectory(unittest.TestCase):
    """Metal refused the shader cache path, and the pinned DXMT checkout says
    it cannot be the path: resolve_cache_dir in src/winemetal/unix/cache.c puts
    confstr in front of a relative path only, but creates the directory either
    way, and returns an absolute path unchanged. So the setter ran against an
    existing directory and MTLGetShaderCachePath still disagreed -- Metal had
    already opened its cache, which this app's own presentation layer causes by
    creating an MTLDevice half a second earlier. What is checked here is that
    the substituted path is one this app can create, so a bad substitution can
    never be misread in the log as Metal refusing a good one."""

    def setUp(self) -> None:
        self.source = read(SYSCALL64)

    def test_the_path_is_proven_creatable_before_metal_is_asked(self) -> None:
        body = function_body(self.source,
                             "static void boxedwineDxmtBeginShaderCachePath64(")
        self.assertIn("BVNPathEnsureDirectory(absolute)", body,
                      "the substituted path is handed over unchecked")
        self.assertLess(
            body.index("BVNPathEnsureDirectory(absolute)"),
            body.index("block->path = boxedwine_dxmt_tag_host_pointer"),
            "the path is checked after the guest is given it")

    def test_the_result_is_in_the_line(self) -> None:
        body = function_body(self.source,
                             "static void boxedwineDxmtEndShaderCachePath64(")
        self.assertIn("directory=%d", body,
                      "the log cannot tell an unusable substitution from a "
                      "refused one")
        self.assertIn("accepted=%llu", body)

    def test_every_field_of_the_witness_is_initialised(self) -> None:
        # The dispatcher deliberately does not zero the 512-byte buffer.
        dispatch = function_body(self.source,
                                 "static U64 boxedwineDxmtUnixCall64(")
        for field in ("cachePath.requested[0] = 0;", "cachePath.used = nullptr;",
                      "cachePath.blockReadable = false;",
                      "cachePath.directoryReady = false;"):
            self.assertIn(field, dispatch,
                          f"the per-frame path leaves {field} unset")

    def test_the_helper_is_declared_and_defined(self) -> None:
        self.assertIn("bool BVNPathEnsureDirectory(const char* path);",
                      read(BVN_RUNTIME_HEADER))
        body = function_body(read(BVN_PATHS),
                             'extern "C" bool BVNPathEnsureDirectory(')
        self.assertIn("withIntermediateDirectories:YES",
                      read(BVN_PATHS),
                      "the parents of the cache directory are not created")
        self.assertIn("path[0] != '/'", body,
                      "a relative path would be created somewhere this app "
                      "does not own")


if __name__ == "__main__":
    unittest.main()
