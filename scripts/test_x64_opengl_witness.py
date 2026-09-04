"""BoxedVN - contract tests for the 64-bit lane's OpenGL state.

A device run of a 32-bit Direct3D 11 program reached Wine's own d3d11, which
loaded wined3d, which loaded opengl32, which asked the guest loader for
libGL.so.1 and was given the IA-32 lane's shim from the root filesystem:

    err:wgl:init_opengl Failed to load libGL: libGL.so.1: wrong ELF class:
                        ELFCLASS32
    err:wgl:init_opengl OpenGL support is disabled.
    err:d3d:wined3d_adapter_gl_init Failed to get a GL context for adapter

Two facts about that sequence are what these tests hold.

The first is that the ELFCLASS32 message is not the defect. init_opengl
disables OpenGL on any dlopen failure, so removing that file would change the
sentence and nothing else -- and it is the other lane's working GL client
library, found second rather than shipped for us. This build defines no GL
backend at all (docs/KNOWN_LIMITATIONS_IOS.md section 3), the host is
Metal-only, and the 64-bit X11 client libraries bridge to BoxedWine's built-in
X server, which serves no GLX. So the lane ships no OpenGL, deliberately; what
the packaging has to refuse is a libGL.so.1 of its OWN that a 64-bit guest
cannot bind to, because the shim directory heads LD_LIBRARY_PATH and a file
there is found ahead of everything else.

The second is that wined3d never chose OpenGL because OpenGL was there. It
chose it because HKCU\\Software\\Wine\\Direct3D value "renderer" was absent and
Wine 9 reads an absent value as WINED3D_RENDERER_AUTO, which is its OpenGL
adapter. The iOS prefix policy that writes that value prepares the IA-32 lane's
prefix only, so the 64-bit launch writes it -- gated, like the audio driver
value beside it, on the backend's client library actually being packaged.

Source-level contracts on purpose: building the emulator needs the iOS
toolchain, and the C++ behaviour of the helpers is checked in
ios/tests/test_guest_wine64_layout.cpp where a compiler exists.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
LAYOUT_HEADER = REPO / "include" / "guest_wine64_layout.h"
STARTUP_ARGS = REPO / "source" / "sdl" / "startupArgs.cpp"
RUNTIME_BUILDER = REPO / "scripts" / "build-wine64-runtime-ci.sh"
RUNTIME_VALIDATOR = REPO / "scripts" / "validate-wine64-runtime.sh"
LAYOUT_TESTS = REPO / "ios" / "tests" / "test_guest_wine64_layout.cpp"

# The soname Wine's opengl32 dlopens, and the file the device run was actually
# given for it: the IA-32 lane's own shim, which lives in the shared root
# filesystem and belongs to the other lane.
OPENGL_SONAME = "libGL.so.1"
LANE32_OPENGL = "/lib/" + OPENGL_SONAME
# The directory a 64-bit launch puts first on LD_LIBRARY_PATH.
SHIM_DIR = "/usr/lib/boxedwine64-x11"


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


def function_body(source: str, name: str) -> str:
    """The text of one C or C++ function, from its opening brace to the
    matching close. Used to hold a claim about one function rather than about
    whichever file happens to contain the words."""
    start = source.index(name)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:index + 1]
    raise AssertionError(f"{name} has no closing brace")


class TheLaneShipsNoOpenGl(unittest.TestCase):
    """Absence is the state, and it is stated rather than left to be found."""

    def setUp(self) -> None:
        self.header = read(LAYOUT_HEADER)

    def test_the_soname_and_the_one_path_this_lane_owns_are_named(self) -> None:
        self.assertEqual(macro_value(self.header, "K_X64_GUEST_OPENGL_SONAME"),
                         OPENGL_SONAME)
        # The lane's own candidate is inside the directory that heads
        # LD_LIBRARY_PATH, which is what makes a wrong file there the one the
        # loader reaches first.
        self.assertEqual(macro_value(self.header, "K_X64_GUEST_OPENGL_LIB_PATH"),
                         SHIM_DIR + "/" + OPENGL_SONAME)

    def test_the_search_order_starts_where_the_lane_stages_and_reaches_the_lane32_shim(self) -> None:
        dirs = self.search_directories()
        # LD_LIBRARY_PATH first: a file this lane staged is found ahead of
        # everything else, which is what makes a wrong one there worse than
        # none at all.
        self.assertEqual(dirs[0], SHIM_DIR)
        # And the directory the device run's open actually answered from: the
        # IA-32 lane's shim lives at /lib/libGL.so.1.
        self.assertIn("/lib", dirs)
        count = re.search(
            r"#define K_X64_GUEST_LIBRARY_SEARCH_DIR_COUNT\s+(\d+)", self.header)
        assert count is not None
        self.assertEqual(len(dirs), int(count.group(1)),
                         "the search directory count does not match the list")

    def search_directories(self) -> list:
        body = re.search(r"#define K_X64_GUEST_LIBRARY_SEARCH_DIRS"
                         r"((?:[^\n\\]*\\\r?\n)*[^\n]*)", self.header)
        assert body is not None
        text = re.sub(r"\\\r?\n", " ", body.group(1))
        return [macro_value(self.header, identifier) if identifier else literal
                for literal, identifier
                in re.findall(r'"([^"]*)"|([A-Za-z_][A-Za-z0-9_]+)', text)]

    def test_the_reason_no_gl_library_is_supplied_is_recorded(self) -> None:
        # Three independent reasons, any one of which is sufficient. They are
        # in the header because this is the question that gets asked again
        # every time a log shows the ELFCLASS32 line.
        for reason in ("no GL backend", "Metal-only", "GLX"):
            self.assertIn(reason, self.header,
                          f"the header does not say why: {reason}")

    def test_the_lane32_shim_is_not_removed_or_replaced(self) -> None:
        # Nothing may stage, delete or shadow the other lane's file. If some
        # change ever wants to, it has to say so somewhere other than here.
        for source in (read(STARTUP_ARGS), read(RUNTIME_BUILDER),
                       read(RUNTIME_VALIDATOR)):
            self.assertNotIn('"' + LANE32_OPENGL + '"', source)


class ThePackagingRefusesAnUnusableClientLibrary(unittest.TestCase):
    """A libGL.so.1 in this archive is found before the other lane's.

    Absent is allowed and is the documented state. Present and wrong is the
    failure this gate exists for: it fails exactly as the IA-32 shim does,
    while looking like something this lane supplied on purpose.
    """

    def test_the_builder_requires_an_elf64_object_if_one_is_ever_staged(self) -> None:
        builder = read(RUNTIME_BUILDER)
        self.assertIn("OPENGL_SHIM_SONAME", builder)
        self.assertEqual(
            re.search(r'OPENGL_SHIM_SONAME="([^"]*)"', builder).group(1),
            OPENGL_SONAME)
        # The check is conditional on the file existing, and only the class is
        # required -- the builder must not start demanding a library nobody
        # builds.
        self.assertRegex(
            builder,
            r'if \[\[ -e "\$\{opengl_client\}" \]\]; then\s*\n\s*'
            r'is_elf64_x86_64 "\$\{opengl_client\}"')
        self.assertNotIn("--opengl-shim", builder,
                         "the builder must not offer to stage a GL library "
                         "that cannot work on this target")

    def test_the_validator_allows_absence_and_refuses_a_wrong_class(self) -> None:
        validator = read(RUNTIME_VALIDATOR)
        self.assertIn("WINE64_OPENGL", validator)
        # Every libGL.so* entry in the archive is checked, not only the one in
        # the shim directory: the multiarch directories are on the same search
        # path.
        self.assertIn(r"libGL\.so", validator)
        self.assertIn("check_zip_entry_elf64_x86_64 \"${WINE_ARCHIVE}\" "
                      "\"${opengl_entry}\"", validator)
        # No check_zip_path for it anywhere: requiring the entry would fail
        # every archive this project produces.
        self.assertNotIn("check_zip_path \"${WINE_ARCHIVE}\" "
                         "\"${X11_SHIM_DIR}/libGL.so.1\"", validator)
        self.assertIn("client=none", validator,
                      "the validator does not report the absent state, so a "
                      "log cannot tell it from the check never running")


class TheWitnessNamesWhatTheGuestFound(unittest.TestCase):
    """One line, so a future log answers this without Wine's wgl channel."""

    def setUp(self) -> None:
        self.source = read(STARTUP_ARGS)
        self.witness = function_body(self.source, "static void reportX64GuestOpenGl")

    def test_the_witness_is_emitted_for_every_x64_launch(self) -> None:
        # Inside the requestedFEX64 block, beside the audio driver's witness,
        # which is where the guest filesystem is fully mounted.
        launch = self.source[self.source.index("if (requestedFEX64) {"):]
        launch = launch[:launch.index("if (!this->ddrawOverridePath")]
        self.assertIn("reportX64GuestOpenGl();", launch)
        self.assertIn("configureX64WineD3dRenderer(winePrefix);", launch)

    def test_it_reports_the_path_that_answered_and_its_elf_class(self) -> None:
        self.assertIn("BOXEDWINE_X64_OPENGL", self.witness)
        for field in ("soname=%s", "found=%s", "class=%s", "binds=%s",
                      "lane64=%s"):
            self.assertIn(field, self.witness,
                          f"the witness does not report {field}")

    def test_it_asks_the_guest_filesystem_rather_than_the_build(self) -> None:
        # A compile-time define that no build ever sets reports nothing; the
        # Vulkan witness was that once. The class has to be read from the file
        # the guest would open.
        self.assertIn("guestLibrarySearchPaths", self.witness)
        self.assertIn("x64GuestFileElfClass", self.witness)
        prober = function_body(self.source, "static const char* x64GuestFileElfClass")
        self.assertIn("Fs::getNodeFromLocalPath", prober)
        self.assertIn("guestElfClassName", prober)

    def test_the_first_file_opened_and_the_first_usable_one_are_both_named(self) -> None:
        # They are different files whenever both exist: the loader opens the
        # IA-32 shim, rejects it and keeps going. A witness reporting only one
        # of them cannot tell "nothing on the path" from "something wrong on
        # the path".
        self.assertIn("firstPath", self.witness)
        self.assertIn("bindsPath", self.witness)
        self.assertIn("guestElfClassIsUsable", self.witness)

    def test_the_absent_state_is_explained_once_rather_than_left_bare(self) -> None:
        self.assertIn("status=absent", self.witness)
        self.assertIn("KNOWN_LIMITATIONS_IOS.md", self.witness)


class TheRendererIsNamedBecauseAutoMeansOpenGl(unittest.TestCase):
    """wined3d picks OpenGL when nothing tells it otherwise."""

    def setUp(self) -> None:
        self.header = read(LAYOUT_HEADER)
        self.source = read(STARTUP_ARGS)
        self.configure = function_body(
            self.source, "static void configureX64WineD3dRenderer")

    def test_the_registry_location_is_fixed_in_the_header(self) -> None:
        # macro_value returns the literal as it is written, so the C escaping
        # is still in it: four backslashes per separator in the source are the
        # two the registry format uses, and user.reg spells the section
        # "[Software\\Wine\\Direct3D]" on disk. Compared in that form on
        # purpose -- the audio driver value reaches the same bytes the same
        # way, and a single-backslash spelling here would silently create a
        # second, wrong section.
        self.assertEqual(
            macro_value(self.header, "K_X64_WINED3D_REGISTRY_SECTION"),
            r"Software\\\\Wine\\\\Direct3D")
        self.assertEqual(macro_value(self.header, "K_X64_WINED3D_RENDERER_NAME"),
                         "renderer")
        self.assertEqual(macro_value(self.header,
                                     "K_X64_WINED3D_RENDERER_VULKAN"),
                         "vulkan")

    def test_the_write_is_gated_on_the_vulkan_client_being_usable(self) -> None:
        # Same rule as the audio driver value: naming a backend that is not
        # there leaves wined3d with no adapter rather than with a fallback.
        self.assertIn("shouldConfigureX64WineD3dVulkan", self.configure)
        self.assertIn("K_X64_GUEST_VULKAN_LIB_PATH", self.configure)
        self.assertIn("guestElfClassIsUsable", self.configure)
        self.assertIn("status=no-vulkan-client", self.configure)

    def test_the_value_is_written_atomically(self) -> None:
        # A truncated user.reg is an empty registry as far as Wine is
        # concerned, and it silently reinitialises the prefix.
        self.assertIn("std::filesystem::rename", self.configure)
        self.assertIn("setGuestWineRegistryValue", self.configure)

    def test_it_reports_the_value_it_found_and_the_adapter_that_follows(self) -> None:
        self.assertIn("BOXEDWINE_X64_WINED3D_RENDERER", self.configure)
        for field in ("vulkan_client=%s", "registry=%s", "renderer=%s",
                      "adapter=%s"):
            self.assertIn(field, self.configure)
        self.assertIn("wined3dAdapterForRenderer", self.configure)
        # An unset value is reported as unset rather than as whatever the
        # default happens to be, and the adapter field says what that means.
        self.assertIn("(unset)", self.configure)

    def test_an_unset_renderer_is_documented_as_the_opengl_adapter(self) -> None:
        adapter = function_body(
            self.header, "inline const char* wined3dAdapterForRenderer")
        self.assertRegex(adapter,
                         r"renderer\.empty\(\)\s*\)\s*\{\s*\n\s*return \"opengl\";")


class TheBehaviourIsCoveredWhereACompilerExists(unittest.TestCase):
    def test_the_cpp_tests_exercise_the_helpers(self) -> None:
        source = read(LAYOUT_TESTS)
        for helper in ("guestElfClassName",
                       "guestElfClassIsUsable",
                       "guestLibrarySearchPaths",
                       "wined3dAdapterForRenderer",
                       "shouldConfigureX64WineD3dVulkan"):
            self.assertIn(helper, source, f"{helper} has no compiled test")


if __name__ == "__main__":
    unittest.main()
