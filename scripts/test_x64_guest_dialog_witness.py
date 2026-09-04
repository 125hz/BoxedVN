"""BoxedVN - contract tests for the guest-dialog witness and the Metal cache path.

A 64-bit Direct3D 11 program stopped during startup, painted a small window
and waited. The device capture named every fact about that window -- created,
166x45, mapped, presented, top-left pixel -- and not one character of what it
said. Guessing what a dialog says is not diagnosis, so two things changed.

The first is a witness. BoxedWine owns the X server, but the server has no
string to log: Wine rasterises text in-process with FreeType and hands over
finished pixels through XPutImage, and source/x11 implements no XDrawString,
XDrawText or XDrawImageString at all. What it does see is the window's name,
which winex11.drv sets from the caption through XChangeProperty and
XSetTextProperty. So the bridge records those, plus a line naming a small or
transient window when it is mapped, bounded the way the module-search trace
is bounded. The body text comes from Wine's own `msgbox` channel instead,
which the launch now turns on.

The second is the Metal shader cache path. DXMT's dxgi.dll asks for a
relative path in its DllMain; the unix side resolves it against
confstr(_CS_DARWIN_USER_CACHE_DIR) and then checks whether Metal took it,
and every device run has printed the failure. The dispatcher now substitutes
an absolute path under the app's own Caches directory -- which exists, is
writable and survives a launch -- and prints whether Metal accepted it, so
the next capture answers the question instead of restating it.

Source-level contracts on purpose: building the emulator needs the iOS
toolchain, and no compiler runs on the host these were written on. What is
checked here is that each piece is wired to the place it has to be wired to,
and that the bounds are real.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DIALOG_HEADER = REPO / "include" / "guest_dialog_trace.h"
X11_BRIDGE = REPO / "source" / "x11" / "x11bridge64.cpp"
SYSCALL64 = REPO / "source" / "kernel" / "syscall64.cpp"
APP_MODEL = REPO / "ios" / "app" / "Sources" / "AppModel.swift"
DXMT_POINTER_HEADER = REPO / "include" / "boxedwine_dxmt_guest_pointer.h"

# The properties whose value is text somebody wrote. _NET_WM_NAME is the one
# modern Wine actually sets and it is interned per session, which is why the
# witness matches atom names rather than atom numbers.
NAME_PROPERTIES = (
    "WM_NAME",
    "WM_ICON_NAME",
    "WM_CLASS",
    "WM_WINDOW_ROLE",
    "_NET_WM_NAME",
    "_NET_WM_ICON_NAME",
)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def define_value(source: str, name: str) -> str:
    match = re.search(
        r"^#define[ \t]+" + re.escape(name) + r"[ \t]+([^\r\n]*)$",
        source, re.MULTILINE)
    if match is None:
        raise AssertionError(f"{name} is not defined")
    return match.group(1).strip()


class TheWitnessIsBounded(unittest.TestCase):
    def setUp(self) -> None:
        self.header = read(DIALOG_HEADER)

    def test_a_whole_session_budget_exists_and_is_small(self) -> None:
        budget = int(define_value(self.header, "K_GUEST_DIALOG_TRACE_BUDGET"))
        self.assertGreaterEqual(budget, 8, "too few lines to see a dialog at all")
        self.assertLessEqual(budget, 256, "a title set on a timer would own the log")

    def test_one_property_cannot_own_a_line(self) -> None:
        limit = int(define_value(self.header, "K_GUEST_DIALOG_TEXT_MAX"))
        self.assertGreaterEqual(limit, 64)
        self.assertLessEqual(limit, 1024)

    def test_the_counter_stops_instead_of_wrapping(self) -> None:
        # An unconditional fetch_add would come back around to zero after
        # four billion calls and start printing again.
        self.assertIn("compare_exchange_weak", self.header)
        self.assertIn("K_GUEST_DIALOG_TRACE_QUIET", self.header)

    def test_the_budget_says_when_it_is_spent(self) -> None:
        self.assertIn("K_GUEST_DIALOG_TRACE_LAST", self.header)
        self.assertIn("BOXEDWINE_X64_GUEST_DIALOG budget=spent",
                      read(X11_BRIDGE))


class PropertyTextIsSafeToPrint(unittest.TestCase):
    def setUp(self) -> None:
        self.header = read(DIALOG_HEADER)

    def test_every_name_property_wine_sets_is_matched(self) -> None:
        for name in NAME_PROPERTIES:
            self.assertIn(f'"{name}"', self.header,
                          f"{name} would not be recognised as window text")

    def test_atoms_are_matched_by_name_not_by_number(self) -> None:
        # _NET_WM_NAME has no fixed id; a numeric list would silently stop
        # matching the property Wine actually sets.
        self.assertIn("guestDialogPropertyIsName(const char* atomName)",
                      self.header)
        self.assertIn("std::strcmp(atomName, name)", self.header)

    def test_a_property_cannot_forge_a_field_or_end_the_line(self) -> None:
        # Property bytes are whatever the guest wrote. Anything outside
        # printable ASCII, and the apostrophe that closes the quoted field,
        # become dots.
        sanitize = self.header.split("guestDialogSanitizeText", 1)[1]
        self.assertIn("c >= 0x20 && c < 0x7f", sanitize)
        self.assertIn("c != 0x27", sanitize)
        self.assertIn("out[written++] = '.';", sanitize)

    def test_the_two_halves_of_wm_class_stay_distinguishable(self) -> None:
        sanitize = self.header.split("guestDialogSanitizeText", 1)[1]
        self.assertIn("out[written++] = '|';", sanitize)

    def test_the_result_is_always_terminated(self) -> None:
        sanitize = self.header.split("guestDialogSanitizeText", 1)[1]
        self.assertIn("out[written] = 0;", sanitize)


class OverrideRedirectWindowsAreNotDialogs(unittest.TestCase):
    def test_menus_and_tooltips_are_excluded(self) -> None:
        header = read(DIALOG_HEADER)
        body = header.split("guestDialogWindowIsInteresting", 1)[1]
        self.assertIn("if (overrideRedirect) {", body)
        self.assertIn("if (hasTransientFor) {", body)

    def test_a_size_ceiling_exists_for_a_window_with_no_transient_parent(self) -> None:
        header = read(DIALOG_HEADER)
        width = int(define_value(header, "K_GUEST_DIALOG_MAX_WIDTH"))
        height = int(define_value(header, "K_GUEST_DIALOG_MAX_HEIGHT"))
        # The emulated desktop is 800x600, so a program's main window must not
        # qualify on size alone.
        self.assertLess(width, 800)
        self.assertLess(height, 600)


class TheBridgeRecordsWhereWineWrites(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(X11_BRIDGE)

    def test_the_policy_header_is_the_one_that_is_used(self) -> None:
        self.assertIn('#include "guest_dialog_trace.h"', self.source)

    def test_both_property_operations_reach_the_witness(self) -> None:
        # XSetTextProperty is how winex11.drv sets WM_NAME; XChangeProperty is
        # how it sets _NET_WM_NAME. Missing either loses half the captions.
        for handler in ("op_CHANGE_PROPERTY", "op_SET_TEXT_PROPERTY"):
            body = self.source.split(f"S64 {handler}(Call& call) {{", 1)
            self.assertEqual(len(body), 2, f"{handler} moved")
            self.assertIn("noteWindowText(call,", body[1].split("\n}\n", 1)[0],
                          f"{handler} does not record the text it stores")

    def test_a_mapped_dialog_is_named_with_its_caption(self) -> None:
        body = self.source.split("S64 op_MAP_WINDOW(Call& call) {", 1)
        self.assertEqual(len(body), 2, "op_MAP_WINDOW moved")
        self.assertIn("reportDialogWindow(call, w);", body[1].split("\n}\n", 1)[0])

    def test_the_line_carries_what_a_reader_needs(self) -> None:
        self.assertIn("BOXEDWINE_X64_GUEST_DIALOG pid=%u window=0x%x property=%s",
                      self.source)
        self.assertIn("transient_for=0x%x class='%s' title='%s'", self.source)

    def test_a_repeated_caption_is_one_line(self) -> None:
        # Wine re-sets a window name whenever the caption changes; the same
        # text on the same property of the same window is worth one line.
        report = self.source.split("void reportDialogText(", 1)[1]
        self.assertIn('firstTime(call.pid, std::string("dialog:")', report)


class TheBodyTextComesFromWine(unittest.TestCase):
    def test_the_launch_turns_on_the_channel_that_carries_it(self) -> None:
        # The 64-bit lane's own environment, not the 32-bit probes that set a
        # WINEDEBUG of their own earlier in the file.
        source = read(APP_MODEL).split("static let environment = [", 1)
        self.assertEqual(len(source), 2, "X64Runtime.environment moved")
        winedebug = re.search(r'"WINEDEBUG=([^"]*)"', source[1])
        self.assertIsNotNone(winedebug, "the 64-bit launch sets no WINEDEBUG")
        channels = winedebug.group(1).split(",")
        self.assertIn("+msgbox", channels,
                      "nothing would carry the text of a message box")

    def test_the_launch_says_whether_that_value_arrived(self) -> None:
        # Three 64-bit captures contain no Wine debug output at all, so the
        # channel list is worth nothing until a log says it reached the
        # process. One line per exec, beside WINELOADER.
        source = read(SYSCALL64)
        self.assertIn('"WINELOADER=", "WINEDEBUG=", "WINEDLLOVERRIDES=",', source)
        self.assertIn('klog_fmt("sys_execve64:   env %s", e.c_str());', source)


class TheMetalShaderCachePath(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(SYSCALL64)

    def test_the_call_this_acts_on_is_the_one_the_table_names(self) -> None:
        index = int(re.search(
            r"kDxmtUnixCallSetMetalShaderCachePath = (\d+)",
            self.source).group(1))
        self.assertEqual(index, 119)
        self.assertIn(f'/* {index} */ "WMTSetMetalShaderCachePath"', self.source)

    def test_the_substitution_is_gated_on_the_native_build(self) -> None:
        # The helpers call into the DXMT pointer header and BVNPathCaches; a
        # build without the native archive must not see either.
        head = self.source.split("// ---- The Metal shader cache path", 1)[0]
        self.assertTrue(head.rstrip().endswith("#if defined(BOXEDWINE_DXMT_NATIVE)"),
                        "the cache-path helpers are not inside the DXMT guard")
        self.assertIn("#endif  // BOXEDWINE_DXMT_NATIVE", self.source)

    def test_the_replacement_path_is_absolute_and_writable(self) -> None:
        # DXMT's resolver only skips confstr when the path starts with a
        # slash, and BVNPathCaches is the one directory the app creates and
        # the system may purge.
        begin = self.source.split("boxedwineDxmtBeginShaderCachePath64(", 2)[1]
        self.assertIn("BVNPathCaches()", begin)
        self.assertIn("candidate.back() != '/'", begin)
        # The block keeps pointing at whatever is handed over, so the storage
        # must never move or be freed.
        self.assertIn("std::vector<std::string*> handedToMetal", begin)
        self.assertIn("new std::string(candidate)", begin)

    def test_an_absolute_request_is_left_alone(self) -> None:
        begin = self.source.split("boxedwineDxmtBeginShaderCachePath64(", 2)[1]
        self.assertIn("if (witness.requested[0] == '/') {", begin)

    def test_the_replacement_is_tagged_as_a_host_pointer(self) -> None:
        # The unix side translates every pointer inside a parameter block
        # through the guest address rule. A host string has to carry the tag
        # that rule strips, or it would be read at a relocated address.
        self.assertIn("boxedwine_dxmt_tag_host_pointer(", self.source)
        self.assertIn('#include "boxedwine_dxmt_guest_pointer.h"', self.source)
        header = read(DXMT_POINTER_HEADER)
        self.assertIn("BOXEDWINE_DXMT_HOST_POINTER_TAG", header)
        self.assertIn("boxedwine_dxmt_tag_host_pointer", header)

    def test_the_block_layout_matches_the_pinned_thunk(self) -> None:
        # struct unixcall_setmetalcachepath is { WMTConstMemoryPointer path;
        # uint64_t ret_success; } and WMTConstMemoryPointer is one const void*
        # on x86-64, so both fields are eight bytes.
        block = self.source.split("struct DxmtSetCachePathBlock {", 1)[1]
        block = block.split("};", 1)[0]
        self.assertIn("U64 path;", block)
        self.assertIn("U64 retSuccess;", block)
        self.assertEqual(len([line for line in block.splitlines() if line.strip()]), 2)

    def test_the_string_is_read_through_the_same_validation_as_the_block(self) -> None:
        reader = self.source.split("boxedwineDxmtReadGuestString64(CPU64*", 1)[1]
        self.assertIn("boxedvn::guestRangeHostable(guest, 1)", reader)
        self.assertIn("nativeRangeCoversForPlan(host, host + 1)", reader)

    def test_the_result_reaches_the_log_either_way(self) -> None:
        self.assertIn("BOXEDWINE_DXMT_SHADER_CACHE requested='%s' used='%s' ",
                      self.source)
        self.assertIn("accepted=%llu status=%d", self.source)


if __name__ == "__main__":
    unittest.main()
