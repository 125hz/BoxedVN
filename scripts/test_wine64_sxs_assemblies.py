#!/usr/bin/env python3
r"""BoxedVN - contracts for the guest prefix's side-by-side assembly store.

A 32-bit program died with an access violation and the session log named
`microsoft.windows.common-controls.dll` at the exit. Thirty-eight lines in that
log probe for that name and every one of them misses -- including the
private-assembly fallback beside the program.

That fallback is the finding. ntdll's `lookup_assembly` (dlls/ntdll/actctx.c)
tries `lookup_winsxs` FIRST and only reaches
<dir>\<name>.dll / <dir>\<name>.manifest / <dir>\<name>\<name>.* when winsxs
returned STATUS_NO_SUCH_FILE. So the probes in the log are proof that the
prefix's winsxs store answered nothing, and when they miss too,
`parse_depend_manifests` fails the whole activation context with
STATUS_SXS_CANT_GEN_ACTCTX. A program whose manifest requires
Microsoft.Windows.Common-Controls 6.0 then runs with no activation context and
reaches the version 5 common controls.

The prefix has no winsxs because nothing ever built one. Wine ships no such
tree and wine.inf never mentions it: a real prefix gets one from wineboot's
fake-DLL install, where `install_fake_dll` calls `register_fake_dll` for every
builtin it copies in, and that writes
windows\winsxs\manifests\<arch>_<name>_<key>_<version>_<lang>_deadbeef.manifest
plus windows\winsxs\<same stem>\<file> for each RT_MANIFEST resource whose
resource NAME begins with "WINE_MANIFEST" (dlls/setupapi/fakedll.c). On this
lane that pass produces nothing, because the prefix's system32 is an in-memory
projection of the packaged module tree rather than a directory wineboot filled
in -- a state a device run already recorded as "wineboot exits 0 and system32 is
empty".

So the tree is derived at packaging time from the same source of truth Wine
uses, and projected into the prefix the way system32 already is. These tests
pin the naming rule against Wine's own, drive the stager end to end over a
synthetic PE tree, and check that the builder, the validator, the projection
and the witness are all wired to it.

Source-level for the C++ half on purpose: building the emulator needs the iOS
toolchain, and no compiler runs on the host these were written on.
"""

from __future__ import annotations

import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
STAGER = REPO / "scripts" / "stage-wine64-sxs-assemblies.py"
BUILDER = REPO / "scripts" / "build-wine64-runtime-ci.sh"
VALIDATOR = REPO / "scripts" / "validate-wine64-runtime.sh"
LAYOUT_HEADER = REPO / "include" / "guest_wine64_layout.h"
PREFIX_HEADER = REPO / "include" / "guest_wine_prefix.h"
SXS_HEADER = REPO / "include" / "guest_sxs_activation.h"
DLL_SEARCH_HEADER = REPO / "include" / "dll_search_trace.h"
STARTUP_ARGS = REPO / "source" / "sdl" / "startupArgs.cpp"
SYSCALL64 = REPO / "source" / "kernel" / "syscall64.cpp"

sys.path.insert(0, str(REPO / "scripts"))
import importlib.util

stager = importlib.import_module("stage-wine64-sxs-assemblies".replace("-", "_")) \
    if False else None  # placeholder replaced below

# The module name has hyphens, so it cannot be imported by name.
_spec = importlib.util.spec_from_file_location("bvn_sxs_stager", STAGER)
stager = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(stager)


# Wine's own comctl32 manifest, verbatim from dlls/comctl32/comctl32.manifest.
# processorArchitecture is EMPTY there; `register_manifest` substitutes the
# tree's architecture into both the file name and the written document, which
# is why an unmodified copy of this file is not what a prefix should hold.
COMCTL32_MANIFEST = (
    b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
    b'<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">\n'
    b'  <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls"'
    b' version="6.0.2600.2982" processorArchitecture="" '
    b'publicKeyToken="6595b64144ccf1df"/>\n'
    b'  <file name="comctl32.dll">\n'
    b'    <windowClass>SysListView32</windowClass>\n'
    b'  </file>\n'
    b'</assembly>\n'
)

# An assembly that names its own architecture, the way msvcr90's does. Wine
# leaves such a manifest byte-for-byte alone.
MSVCR90_MANIFEST = (
    b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
    b'<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">\n'
    b'<assemblyIdentity type="win32" name="Microsoft.VC90.CRT" '
    b'version="9.0.30729.4148" processorArchitecture="x86" '
    b'publicKeyToken="1fc8b3b9a1e18e3b"/>\n'
    b'<file name="msvcr90.dll"/>\n'
    b'</assembly>\n'
)

# What an ordinary program embeds: RT_MANIFEST resource #1. `register_manifest`
# skips it because the resource NAME is an integer, and staging it as an
# assembly would put a document with no assemblyIdentity of its own into the
# store.
APPLICATION_MANIFEST = (
    b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
    b'<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">\n'
    b'<dependency><dependentAssembly><assemblyIdentity type="win32" '
    b'name="Microsoft.Windows.Common-Controls" version="6.0.0.0" '
    b'processorArchitecture="*" publicKeyToken="6595b64144ccf1df" '
    b'language="*"/></dependentAssembly></dependency>\n'
    b'</assembly>\n'
)

RT_MANIFEST = 24


def _resource_section(manifest: bytes, resource_name, base_rva: int) -> bytes:
    """One RT_MANIFEST resource, laid out the way a linker lays one out.

    `resource_name` is either a string (a named resource, which is what
    WINE_MANIFEST is) or an integer (an ordinary application manifest).
    """
    named = isinstance(resource_name, str)
    root = bytearray()

    def directory(named_count: int, id_count: int) -> bytes:
        return struct.pack("<IIHHHH", 0, 0, 4, 0, named_count, id_count)

    # Fixed offsets, chosen so the string table and the payload follow the
    # three directory levels and the data entry.
    type_dir = 0x18
    name_dir = 0x30
    data_entry = 0x48
    string_at = 0x58
    payload = string_at + 2 + len(resource_name) * 2 if named else string_at
    payload = (payload + 3) & ~3

    root += directory(0, 1)
    root += struct.pack("<II", RT_MANIFEST, type_dir | 0x80000000)
    assert len(root) == type_dir
    root += directory(1 if named else 0, 0 if named else 1)
    root += struct.pack(
        "<II",
        (string_at | 0x80000000) if named else resource_name,
        name_dir | 0x80000000)
    assert len(root) == name_dir
    root += directory(0, 1)
    root += struct.pack("<II", 1033, data_entry)
    assert len(root) == data_entry
    root += struct.pack("<IIII", base_rva + payload, len(manifest), 0, 0)
    assert len(root) == string_at
    if named:
        root += struct.pack("<H", len(resource_name))
        root += resource_name.encode("utf-16-le")
    root += b"\0" * (payload - len(root))
    root += manifest
    return bytes(root)


def pe_with_manifest(manifest: bytes, resource_name="WINE_MANIFEST",
                     machine: int = 0x8664) -> bytes:
    """A PE32+ image whose only content is one RT_MANIFEST resource."""
    lfanew = 0x40
    optional_size = 0xF0
    section_table = lfanew + 4 + 20 + optional_size
    raw_start = 0x200
    rsrc_rva = 0x1000
    rsrc = _resource_section(manifest, resource_name, rsrc_rva)

    dos = bytearray(lfanew)
    dos[0:2] = b"MZ"
    dos[0x3C:0x40] = struct.pack("<I", lfanew)

    coff = struct.pack("<IHHIIIHH", 0x00004550, machine, 1, 0, 0, 0,
                       optional_size, 0x2022)
    optional = bytearray(optional_size)
    struct.pack_into("<H", optional, 0, 0x20B)
    # Data directory 2 is the resource table; the optional header's directory
    # array starts 112 bytes in for PE32+.
    struct.pack_into("<II", optional, 112 + 16, rsrc_rva, len(rsrc))

    section = struct.pack("<8sIIIIIIHHI", b".rsrc\0\0\0", len(rsrc), rsrc_rva,
                          len(rsrc), raw_start, 0, 0, 0, 0, 0x40000040)

    image = bytearray(raw_start)
    image[0:lfanew] = dos
    image[lfanew:lfanew + 24] = coff
    image[lfanew + 24:lfanew + 24 + optional_size] = optional
    image[section_table:section_table + 40] = section
    return bytes(image) + rsrc


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


class TheNamingRuleIsWines(unittest.TestCase):
    """A file name Wine would not have written is a file Wine cannot find.

    `lookup_manifest_file` searches with the wildcard
    <arch>_<name>_<key>_<major>.<minor>.*_<lang>_deadbeef.manifest and then
    parses the build and revision back out of whatever matched. Every field is
    load-bearing and the whole stem is lowercased.
    """

    def test_the_stem_is_the_one_append_manifest_filename_builds(self) -> None:
        self.assertEqual(
            stager.assembly_base("amd64", "Microsoft.Windows.Common-Controls",
                                 "6595b64144ccf1df", "6.0.2600.2982", "none"),
            "amd64_microsoft.windows.common-controls_6595b64144ccf1df_"
            "6.0.2600.2982_none_deadbeef")

    def test_the_trailer_is_the_one_wine_recognises(self) -> None:
        # lookup_manifest_file reads the trailer back to tell a Wine-provided
        # assembly from one an installer put in the same directory, and prefers
        # the installer's. A different trailer would make ours look installed.
        self.assertEqual(stager.ASSEMBLY_TRAILER, "deadbeef")

    def test_a_manifest_with_no_language_gets_the_placeholder(self) -> None:
        assembly = stager.parse_assembly(COMCTL32_MANIFEST, "amd64", "x.dll")
        self.assertEqual(assembly.language, "none")

    def test_an_empty_architecture_is_filled_in_both_places(self) -> None:
        assembly = stager.parse_assembly(COMCTL32_MANIFEST, "amd64", "x.dll")
        self.assertEqual(assembly.arch, "amd64")
        self.assertTrue(assembly.base.startswith("amd64_"))
        # The written document carries it too: an activation context built
        # from a manifest whose processorArchitecture is empty does not match
        # the identity the file name claims.
        self.assertIn(b'processorArchitecture="amd64"', assembly.manifest)

    def test_the_32_bit_tree_gets_the_32_bit_token(self) -> None:
        assembly = stager.parse_assembly(COMCTL32_MANIFEST, "x86", "x.dll")
        self.assertTrue(assembly.base.startswith("x86_"))
        self.assertIn(b'processorArchitecture="x86"', assembly.manifest)

    def test_a_manifest_that_names_its_architecture_is_untouched(self) -> None:
        assembly = stager.parse_assembly(MSVCR90_MANIFEST, "amd64", "x.dll")
        self.assertEqual(assembly.arch, "x86")
        self.assertEqual(assembly.manifest, MSVCR90_MANIFEST)

    def test_the_files_the_assembly_redirects_to_are_read(self) -> None:
        # find_actctx_dll builds windows\winsxs\<stem>\<file> from these, so a
        # manifest staged without its <file> elements redirects a load to a
        # path that is not there -- which is worse than no redirect at all.
        assembly = stager.parse_assembly(COMCTL32_MANIFEST, "amd64", "x.dll")
        self.assertEqual(assembly.files, ("comctl32.dll",))


class OnlyWineManifestResourcesAreAssemblies(unittest.TestCase):
    """`register_manifest` skips an integer resource name outright."""

    def test_a_named_wine_manifest_is_found(self) -> None:
        image = pe_with_manifest(COMCTL32_MANIFEST, "WINE_MANIFEST")
        self.assertEqual(stager.wine_manifest_resources(image),
                         [COMCTL32_MANIFEST])

    def test_an_application_manifest_is_not_an_assembly(self) -> None:
        image = pe_with_manifest(APPLICATION_MANIFEST, 1)
        self.assertEqual(stager.wine_manifest_resources(image), [])

    def test_another_named_resource_is_not_an_assembly(self) -> None:
        image = pe_with_manifest(COMCTL32_MANIFEST, "SOMETHING_ELSE")
        self.assertEqual(stager.wine_manifest_resources(image), [])

    def test_a_module_with_no_resources_is_not_an_error(self) -> None:
        self.assertEqual(stager.wine_manifest_resources(b"MZ" + b"\0" * 128), [])
        self.assertEqual(stager.wine_manifest_resources(b"not a pe file"), [])


class TheStagerProducesWinesShape(unittest.TestCase):
    """End to end over a synthetic PE tree."""

    def setUp(self) -> None:
        self.work = tempfile.TemporaryDirectory()
        self.root = Path(self.work.name)
        self.pe_dir = self.root / "x86_64-windows"
        self.pe_dir.mkdir()
        (self.pe_dir / "comctl32.dll").write_bytes(
            pe_with_manifest(COMCTL32_MANIFEST))
        (self.pe_dir / "kernel32.dll").write_bytes(b"MZ" + b"\0" * 256)
        self.stage = self.root / "stage"

    def tearDown(self) -> None:
        self.work.cleanup()

    def run_stager(self, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(STAGER),
             "--pe-dir", str(self.pe_dir),
             "--arch", "amd64",
             "--module-guest-dir",
             "/usr/lib/x86_64-linux-gnu/wine/x86_64-windows",
             "--stage-dir", str(self.stage), *extra],
            capture_output=True, text=True)

    def test_the_manifest_lands_where_lookup_winsxs_searches(self) -> None:
        self.assertEqual(self.run_stager().returncode, 0)
        expected = (self.stage / "manifests" /
                    ("amd64_microsoft.windows.common-controls_"
                     "6595b64144ccf1df_6.0.2600.2982_none_deadbeef.manifest"))
        self.assertTrue(expected.is_file(), sorted(
            str(p) for p in self.stage.rglob("*")))
        self.assertIn(b'processorArchitecture="amd64"', expected.read_bytes())

    def test_the_assembly_directory_links_to_the_packaged_module(self) -> None:
        self.assertEqual(self.run_stager().returncode, 0)
        link = (self.stage /
                "amd64_microsoft.windows.common-controls_6595b64144ccf1df_"
                "6.0.2600.2982_none_deadbeef" / "comctl32.dll.link")
        self.assertTrue(link.is_file())
        # BoxedWine reads the whole entry as the target path: a trailing
        # newline would make the link name a file that does not exist.
        self.assertEqual(
            link.read_bytes(),
            b"/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/comctl32.dll")

    def test_a_tree_without_the_required_assembly_fails(self) -> None:
        (self.pe_dir / "comctl32.dll").unlink()
        result = self.run_stager("--require",
                                 "Microsoft.Windows.Common-Controls")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Microsoft.Windows.Common-Controls", result.stderr)

    def test_the_required_assembly_passes_when_it_is_there(self) -> None:
        result = self.run_stager("--require",
                                 "Microsoft.Windows.Common-Controls")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("WINE64_SXS_ASSEMBLY", result.stdout)
        self.assertIn("name=Microsoft.Windows.Common-Controls", result.stdout)

    def test_the_inventory_names_the_version_that_was_staged(self) -> None:
        # The whole point of the gate is that a program asking for 6.0 gets
        # 6.0, so the build log has to say which version went in.
        result = self.run_stager()
        self.assertIn("version=6.0.2600.2982", result.stdout)


class TheBuilderStagesAndGatesOnIt(unittest.TestCase):
    def setUp(self) -> None:
        self.builder = read(BUILDER)

    def test_the_builder_runs_the_stager(self) -> None:
        self.assertIn("stage-wine64-sxs-assemblies.py", self.builder)

    def test_both_architectures_are_staged(self) -> None:
        # The architecture reaches the stager as a parameter of the one
        # helper that invokes it, so the flag is spelled once and each call
        # site names its own architecture. Requiring the flag and a literal
        # architecture in the same command would forbid that factoring for
        # no benefit; the call sites are named in the test below.
        self.assertIn("stage_sxs_assemblies", self.builder)
        self.assertRegex(self.builder, r'--arch\s+"[$]{arch}"')
        helper_start = self.builder.index("stage_sxs_assemblies() {")
        helper = self.builder[helper_start:]
        self.assertRegex(helper, r'local pe_dir="[$]1" arch="[$]2"')
        self.assertIn('x86_64-windows" amd64', self.builder)
        self.assertIn('{I386_PE_GUEST_DIR}" x86', self.builder)

    def test_the_stagers_witness_lines_do_not_reach_the_count(self) -> None:
        # The helper's stdout is captured to get the manifest count, and the
        # stager prints one witness line per assembly. Without a redirect
        # those lines land in the count, and from there in the runtime
        # manifest, where the validator rejects them as malformed keys.
        helper = self.builder[self.builder.index("stage_sxs_assemblies() {"):]
        invocation = helper[helper.index('python3 "${WINSXS_STAGER}"'):]
        invocation = invocation[:invocation.index("|| die")]
        self.assertIn(">&2", invocation)

    def test_each_architecture_goes_into_its_own_archive(self) -> None:
        # A manifest in wine64.zip whose assembly directory links into the
        # i386 tree would dangle whenever the PE32 archive is not mounted,
        # which is the same argument the loader links already make.
        self.assertIn('"${PE32_STAGE}${I386_PE_GUEST_DIR}" x86', self.builder)
        self.assertIn('"${STAGE}${WINE_MODULE_ROOT}/x86_64-windows" amd64',
                      self.builder)

    def test_the_build_fails_without_the_common_controls_assembly(self) -> None:
        self.assertIn("WINSXS_COMMON_CONTROLS_PREFIX_64", self.builder)
        self.assertIn("WINSXS_COMMON_CONTROLS_PREFIX_32", self.builder)
        gate = self.builder[self.builder.index("WINSXS_COMMON_CONTROLS_PREFIX_64"):]
        self.assertIn("die ", gate)

    def test_the_manifest_records_what_was_staged(self) -> None:
        # A runtime assembled before this staging existed has to be
        # distinguishable from one that has it; zero here is exactly the shape
        # that gives every program the version 5 common controls.
        self.assertIn("winsxs_manifests_amd64=", self.builder)
        self.assertIn("winsxs_manifests_x86=", self.builder)

    def test_the_guest_path_matches_the_one_the_launch_projects(self) -> None:
        header = read(LAYOUT_HEADER)
        match = re.search(
            r'#define\s+K_X64_GUEST_WINSXS_DIR\s+"([^"]+)"', header)
        self.assertIsNotNone(match)
        self.assertIn('WINSXS_GUEST_DIR="{}"'.format(match.group(1)),
                      self.builder)


class TheValidatorRefusesARuntimeWithoutIt(unittest.TestCase):
    def setUp(self) -> None:
        self.validator = read(VALIDATOR)

    def test_both_architectures_are_required_by_name(self) -> None:
        self.assertIn("amd64_${WINSXS_COMMON_CONTROLS}_", self.validator)
        self.assertIn("x86_${WINSXS_COMMON_CONTROLS}_", self.validator)

    def test_the_assembly_directories_are_required_too(self) -> None:
        # A manifest with no directory beside it redirects the load to a path
        # that does not exist, which is worse than no activation context.
        self.assertIn("no assembly directories", self.validator)

    def test_the_check_is_by_name_and_not_by_count(self) -> None:
        self.assertIn("check_zip_winsxs_assembly", self.validator)


class TheLaunchProjectsItIntoThePrefix(unittest.TestCase):
    def setUp(self) -> None:
        self.startup = read(STARTUP_ARGS)

    def test_the_projection_exists_and_runs(self) -> None:
        self.assertIn("projectX64WineSxsAssemblies", self.startup)
        self.assertIn("projectX64WineSxsAssemblies(winePrefix);", self.startup)

    def test_it_runs_before_any_guest_process_starts(self) -> None:
        # ntdll builds a process's activation context during
        # LdrInitializeThunk, so a winsxs that appears later is one no process
        # ever saw. The system32 projection is already ordered that way; this
        # has to be beside it.
        system32_at = self.startup.index("projectX64WineSystemModules(winePrefix);")
        sxs_at = self.startup.index("projectX64WineSxsAssemblies(winePrefix);")
        self.assertLess(system32_at, sxs_at)

    def test_it_goes_beside_system32_and_not_inside_it(self) -> None:
        # lookup_winsxs builds its path from the WINDOWS directory. A tree
        # under system32 would never be looked at.
        self.assertIn("K_GUEST_WINE_WINSXS", read(PREFIX_HEADER))
        self.assertIn("K_GUEST_WINE_WINDOWS \"/\" K_GUEST_WINE_WINSXS",
                      self.startup)

    def test_a_real_prefix_file_is_never_replaced(self) -> None:
        projection = self.startup[
            self.startup.index("static void projectX64WineSxsAssemblies"):
            self.startup.index("static void projectX64WineSystemModules")]
        self.assertIn("shouldProjectGuestWineSystemModule", projection)

    def test_a_runtime_without_the_staged_tree_says_so(self) -> None:
        self.assertIn("status=no-staged-tree", self.startup)

    def test_the_projection_reports_what_it_made_activatable(self) -> None:
        self.assertIn("BOXEDWINE_X64_SXS_OVERLAY", self.startup)


class TheWitnessNamesTheOutcome(unittest.TestCase):
    """What a future log has to say without anything being inferred."""

    def setUp(self) -> None:
        self.header = read(SXS_HEADER)
        self.syscall = read(SYSCALL64)
        self.dll_search = read(DLL_SEARCH_HEADER)

    def test_the_line_names_the_assembly_the_version_and_the_source(self) -> None:
        self.assertIn("BOXEDWINE_X64_SXS_ACTIVATION", self.syscall)
        line = self.syscall[self.syscall.index("BOXEDWINE_X64_SXS_ACTIVATION"):]
        line = line[:line.index(";")]
        for field in ("assembly=%s", "version=%s", "arch=%s", "source=%s",
                      "status=%s"):
            self.assertIn(field, line)

    def test_the_witness_is_not_budgeted_against_the_module_trace(self) -> None:
        # A spent budget is exactly when a log stops saying anything, and the
        # crash this exists for happened after 800-odd probes.
        note = self.syscall[self.syscall.index("process->dllSearch.noteResult("):]
        note = note[:note.index("DllSearchTrace::Decision decision")]
        self.assertIn("sxs().takeReport", note)

    def test_the_manifest_half_of_the_search_is_visible_at_all(self) -> None:
        # A private-assembly probe is <program dir>\<assembly>.manifest, which
        # shares no text with a module tree or the system directory: the trace
        # could not see one before.
        markers = self.dll_search[
            self.dll_search.index("static const char* const markers[]"):]
        markers = markers[:markers.index("};")]
        self.assertIn('".manifest"', markers)
        self.assertIn('"winsxs"', markers)

    def test_a_private_probe_is_read_as_proof_that_winsxs_missed(self) -> None:
        self.assertIn("lookup_winsxs", self.header)
        self.assertIn("private-assembly", self.header)

    def test_an_associated_manifest_is_not_read_as_an_assembly(self) -> None:
        # services.exe.manifest and winedevice.exe.manifest are in the device
        # log and are get_manifest_in_associated_manifest, not an assembly
        # lookup at all.
        self.assertIn('sxsPathEndsWith(stem, ".exe")', self.header)
        self.assertIn('sxsPathEndsWith(stem, ".dll")', self.header)

    def test_one_line_per_assembly_per_process(self) -> None:
        self.assertIn("K_SXS_ASSEMBLY_SLOTS", self.header)
        self.assertIn("reportedCount", self.header)

    def test_the_version_comes_out_of_the_name_wine_wrote(self) -> None:
        self.assertIn("sxsParseManifestStem", self.header)


class TheStemParserMatchesTheNamer(unittest.TestCase):
    """The witness reads back exactly what the stager writes.

    Checked here rather than in C++ because no compiler runs on this host; the
    rule itself is short enough to restate, and restating it is what catches
    the two halves drifting apart.
    """

    @staticmethod
    def parse(stem: str):
        """The rule guest_sxs_activation.h implements: parse from the right."""
        marks = [i for i, c in enumerate(stem) if c == "_"]
        if len(marks) < 5:
            return None
        tail = marks[-5:]
        arch_end = stem.index("_")
        if arch_end >= tail[1]:
            return None
        return (stem[:arch_end], stem[arch_end + 1:tail[1]],
                stem[tail[2] + 1:tail[3]])

    def test_the_common_controls_stem_round_trips(self) -> None:
        stem = stager.assembly_base(
            "amd64", "Microsoft.Windows.Common-Controls", "6595b64144ccf1df",
            "6.0.2600.2982", "none")
        self.assertEqual(
            self.parse(stem),
            ("amd64", "microsoft.windows.common-controls", "6.0.2600.2982"))

    def test_a_name_containing_an_underscore_still_round_trips(self) -> None:
        stem = stager.assembly_base("x86", "Some_Vendor.Thing", "0123456789abcdef",
                                    "1.2.3.4", "none")
        self.assertEqual(self.parse(stem),
                         ("x86", "some_vendor.thing", "1.2.3.4"))


if __name__ == "__main__":
    unittest.main()
