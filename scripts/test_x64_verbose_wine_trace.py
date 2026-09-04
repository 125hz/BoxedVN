"""BoxedVN - contract tests for the "Verbose Wine trace" setting.

A 64-bit Direct3D 11 program stops during startup and paints its own error
dialog. The capture of that run names every file the program opened, every
window it created and the caption on the one it stopped at, and none of that
says what the program asked for: between its imports finishing and the dialog
appearing it opened no file, created no thread of its own and made no system
call this project traces. A check that reads the registry, asks for a named
object, looks at an environment variable or calls into winsock is invisible
here, and those are exactly the checks a startup makes.

Wine can see them. `WINEDEBUG=+relay` writes one line per call into a traced
module with its arguments, its return value and the caller's return address.
Unrestricted it buries the answer rather than giving it, and the restriction
is not settable from the environment: Wine reads RelayInclude and RelayExclude
from HKCU\\Software\\Wine\\Debug and from nowhere else, once, when the first
traced call is made. So the setting has two halves that have to agree - an
environment the app builds and a pair of registry values the emulator writes
into the prefix - and this file is what holds them together.

Source-level contracts on purpose: building the emulator needs the iOS
toolchain and no compiler runs on the host these were written on. What is
checked is that each half is wired to the other, that the trace is off unless
it is asked for, that the include list stayed small, and that the log's flood
limiter cannot drop the lines the trace exists to produce.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
LAYOUT_HEADER = REPO / "include" / "guest_wine64_layout.h"
STARTUP_ARGS = REPO / "source" / "sdl" / "startupArgs.cpp"
APP_MODEL = REPO / "ios" / "app" / "Sources" / "AppModel.swift"
RUNTIME = REPO / "ios" / "app" / "Sources" / "Runtime.swift"
VIEWS = REPO / "ios" / "app" / "Sources" / "Views.swift"
LOG = REPO / "ios" / "runtime" / "src" / "BVNLog.mm"

SETTING_KEY = "BoxedVN.x64.verboseWineTrace"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def string_macro(source: str, name: str) -> str:
    """The value of a #define whose body is one or more quoted strings.

    Line continuations are joined and adjacent string literals concatenated,
    which is what the C preprocessor does with them.
    """
    # The lookahead keeps K_X64_WINE_RELAY_INCLUDE from matching the line that
    # defines K_X64_WINE_RELAY_INCLUDE_NAME.
    match = re.search(
        r"^#define[ \t]+" + re.escape(name) + r"(?![A-Za-z0-9_])"
        r"((?:[^\r\n\\]*\\\r?\n)*[^\r\n]*)$",
        source, re.MULTILINE)
    if match is None:
        raise AssertionError(f"{name} is not defined")
    body = re.sub(r"\\\r?\n", " ", match.group(1))
    pieces = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
    if not pieces:
        raise AssertionError(f"{name} is not a string")
    return "".join(pieces)


def unescape(value: str) -> str:
    """The bytes a C compiler produces from a string literal's body."""
    return value.replace("\\\\", "\\")


class TheTwoHalvesAgree(unittest.TestCase):
    """The app builds the environment; the emulator writes the registry."""

    def setUp(self) -> None:
        self.header = read(LAYOUT_HEADER)
        self.app = read(APP_MODEL)

    def test_the_channels_are_spelled_the_same_on_both_sides(self) -> None:
        # Swift cannot include the C header, so the two literals are compared
        # here rather than trusted to stay in step by memory.
        header = string_macro(self.header, "K_X64_WINE_TRACE_CHANNELS")
        swift = re.search(r'static let verboseTraceChannels = "([^"]*)"',
                          self.app)
        self.assertIsNotNone(swift, "the app sets no verbose trace channels")
        self.assertEqual(swift.group(1), header)
        self.assertIn("+relay", header.split(","),
                      "relay is the only channel that can see an API call")

    def test_the_variable_the_emulator_reads_is_the_one_the_app_sets(self) -> None:
        env = string_macro(self.header, "K_X64_WINE_TRACE_ENV")
        value = string_macro(self.header, "K_X64_WINE_TRACE_RELAY")
        swift = re.search(r'static let verboseTraceAssignment = "([^"]*)"',
                          self.app)
        self.assertIsNotNone(swift, "the app sets no verbose trace variable")
        self.assertEqual(swift.group(1), f"{env}={value}")

    def test_the_registry_section_is_the_on_disk_form(self) -> None:
        # user.reg spells a key with doubled backslashes, so the macro carries
        # four in the source. A single-backslash value would find no section
        # and silently write a second one Wine never reads.
        section = string_macro(self.header, "K_X64_WINE_DEBUG_REGISTRY_SECTION")
        self.assertEqual(unescape(section), "Software\\\\Wine\\\\Debug")


class TheIncludeListStaysSmall(unittest.TestCase):
    """An unrestricted relay trace answers nothing, so the bound is the point."""

    def setUp(self) -> None:
        self.header = read(LAYOUT_HEADER)
        self.include = [entry for entry in string_macro(
            self.header, "K_X64_WINE_RELAY_INCLUDE").split(";") if entry]
        self.exclude = [entry for entry in string_macro(
            self.header, "K_X64_WINE_RELAY_EXCLUDE").split(";") if entry]

    def test_it_is_a_handful_of_modules_and_not_a_process(self) -> None:
        self.assertGreaterEqual(len(self.include), 2,
                                "too few modules to see a startup check")
        self.assertLessEqual(len(self.include), 8,
                             "this is no longer a restriction")

    def test_it_names_modules_rather_than_functions(self) -> None:
        # A module entry traces everything in it; the include list is meant to
        # be broad within a few modules, not a guess at function names.
        for entry in self.include:
            self.assertNotIn(".", entry,
                             f"{entry} narrows the include list to one call")

    def test_the_modules_a_startup_check_actually_calls_are_there(self) -> None:
        for module in ("advapi32", "kernel32", "kernelbase", "user32"):
            self.assertIn(module, self.include)

    def test_ntdll_is_not_traced(self) -> None:
        # Every heap allocation and every critical section enters through it,
        # so including it is indistinguishable from tracing everything.
        self.assertNotIn("ntdll", self.include)

    def test_every_exclusion_belongs_to_an_included_module(self) -> None:
        for entry in self.exclude:
            self.assertIn(".", entry, f"{entry} would exclude a whole module")
            module = entry.split(".", 1)[0]
            self.assertIn(module, self.include,
                          f"{entry} excludes from a module nothing traces")

    def test_the_modal_message_loop_is_excluded(self) -> None:
        # The dialog this exists to explain is modal: once it is up its own
        # message loop runs for as long as it stands, and would fill the log
        # after the answer had already been written.
        for name in ("user32.PeekMessageA", "user32.PeekMessageW",
                     "user32.GetMessageA", "user32.GetMessageW",
                     "user32.DispatchMessageA", "user32.DispatchMessageW"):
            self.assertIn(name, self.exclude)

    def test_the_per_iteration_bookkeeping_is_excluded(self) -> None:
        for name in ("kernel32.GetLastError", "kernelbase.GetLastError",
                     "kernel32.TlsGetValue", "kernelbase.TlsGetValue"):
            self.assertIn(name, self.exclude)


class TheSettingIsOffUntilItIsAskedFor(unittest.TestCase):
    def test_the_toggle_and_the_launch_use_one_key(self) -> None:
        runtime = read(RUNTIME)
        views = read(VIEWS)
        self.assertIn(f'static let verboseWineTraceKey = "{SETTING_KEY}"',
                      runtime)
        # The view binds to the same constant rather than to a second spelling
        # of the string, which is how the two could drift apart silently.
        self.assertIn("@AppStorage(Preferences.verboseWineTraceKey)", views)
        self.assertIn("private var verboseWineTrace = false", views,
                      "the toggle would come up on")
        self.assertIn("Toggle(\"Verbose Wine trace\", isOn: $verboseWineTrace)",
                      views)

    def test_an_unset_key_reads_as_off(self) -> None:
        # bool(forKey:) is false for an absent key, which is the wanted
        # default: the sound preference next to it needs an explicit object()
        # check precisely because its default is the other way round.
        body = read(RUNTIME).split("static var verboseWineTrace: Bool {", 1)
        self.assertEqual(len(body), 2, "Preferences.verboseWineTrace moved")
        self.assertIn("UserDefaults.standard.bool(forKey: verboseWineTraceKey)",
                      body[1].split("}", 1)[0])

    def test_the_environment_is_unchanged_when_it_is_off(self) -> None:
        body = read(APP_MODEL).split("static func withVerboseTrace(", 1)
        self.assertEqual(len(body), 2, "withVerboseTrace moved")
        body = body[1]
        self.assertIn("guard enabled else { return base }", body)

    def test_the_channels_are_appended_and_not_substituted(self) -> None:
        # +msgbox is what carries the text of the dialog the trace is being
        # turned on to explain. Replacing WINEDEBUG would trade the answer for
        # the evidence.
        body = read(APP_MODEL).split("static func withVerboseTrace(", 1)[1]
        self.assertIn("hasPrefix(wineDebugAssignmentPrefix)", body)
        self.assertIn('$0 + "," + verboseTraceChannels : $0', body)
        self.assertIn("+ [verboseTraceAssignment]", body)


class OnlyTheRunProgramLaunchGetsIt(unittest.TestCase):
    def setUp(self) -> None:
        self.app = read(APP_MODEL)

    def test_the_program_launch_asks_for_the_preference(self) -> None:
        body = self.app.split("func launchX64Program(", 1)
        self.assertEqual(len(body), 2, "launchX64Program moved")
        body = body[1].split("\n    /// ", 1)[0]
        self.assertIn("X64Runtime.withVerboseTrace(", body)
        self.assertIn("enabled: Preferences.verboseWineTrace", body)

    def test_nothing_else_does(self) -> None:
        # The probes and the desktop run unattended and long; relay on those
        # is a cost with no question attached.
        self.assertEqual(self.app.count("X64Runtime.withVerboseTrace("), 1)
        for launcher in ("func launchX64GraphicsProbe(",
                         "func launchX64Desktop("):
            body = self.app.split(launcher, 1)
            self.assertEqual(len(body), 2, f"{launcher} moved")
            self.assertNotIn("withVerboseTrace",
                             body[1].split("\n    }\n", 1)[0])


class TheEmulatorWritesTheFilter(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(STARTUP_ARGS)

    def test_it_is_gated_on_the_exact_value(self) -> None:
        body = self.source.split(
            "static bool guestWantsWineRelayTrace(", 1)
        self.assertEqual(len(body), 2, "guestWantsWineRelayTrace is missing")
        body = body[1].split("\n}\n", 1)[0]
        self.assertIn('B(K_X64_WINE_TRACE_ENV "=")', body)
        self.assertIn("B(K_X64_WINE_TRACE_RELAY)", body)

    def test_it_runs_for_a_64_bit_launch_that_asked_for_it(self) -> None:
        self.assertIn("if (guestWantsWineRelayTrace(envValues)) {\n"
                      "            configureX64WineRelayFilter(winePrefix);\n"
                      "        }", self.source)

    def test_both_values_are_written(self) -> None:
        body = self.source.split(
            "static void configureX64WineRelayFilter(", 1)
        self.assertEqual(len(body), 2, "configureX64WineRelayFilter is missing")
        body = body[1]
        self.assertIn("K_X64_WINE_DEBUG_REGISTRY_SECTION", body)
        self.assertIn("K_X64_WINE_RELAY_INCLUDE_NAME", body)
        self.assertIn("K_X64_WINE_RELAY_EXCLUDE_NAME", body)
        # |= and not ||=: the second edit has to be made whether or not the
        # first one changed anything.
        self.assertIn("changed |= setGuestWineRegistryValue(", body)

    def test_a_killed_process_cannot_truncate_the_registry(self) -> None:
        # Wine treats a truncated user.reg as an empty registry and silently
        # reinitialises the prefix, so the write goes through a sibling and a
        # rename, as the audio and renderer values do.
        body = self.source.split(
            "static void configureX64WineRelayFilter(", 1)[1]
        self.assertIn('nativePath + ".boxedvn-relay"', body)
        self.assertIn("std::filesystem::rename(", body)
        self.assertIn("std::filesystem::remove(", body)

    def test_the_log_says_what_it_did(self) -> None:
        self.assertIn("BOXEDWINE_X64_WINE_RELAY registry=%s", self.source)


class TheFloodLimiterCannotDropTheTrace(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(LOG)

    def test_relay_lines_are_exempt(self) -> None:
        body = self.source.split("bool floodExemptRelay(", 1)
        self.assertEqual(len(body), 2, "floodExemptRelay is missing")
        body = body[1].split("\n}\n", 1)[0]
        # Wine writes "<hex thread id>:Call " and "<hex thread id>:Ret  ".
        self.assertIn('"Call "', body)
        self.assertIn('"Ret  "', body)
        self.assertIn("isxdigit(", body)

    def test_it_is_decided_before_a_slot_is_claimed(self) -> None:
        # Otherwise relay output would occupy all sixty-four slots and leave a
        # genuine flood of something else unlimited.
        body = self.source.split("bool floodAdmit(", 1)
        self.assertEqual(len(body), 2, "floodAdmit moved")
        body = body[1]
        exempt = body.find("floodExemptRelay(text, length)")
        slots = body.find("FloodSlot* match = nullptr;")
        self.assertNotEqual(exempt, -1, "floodAdmit does not exempt the trace")
        self.assertNotEqual(slots, -1, "the slot search moved")
        self.assertLess(exempt, slots)

    def test_the_window_summaries_are_still_written(self) -> None:
        # The exemption returns early, so it has to carry the summary flush
        # the rest of floodAdmit does.
        body = self.source.split("bool floodAdmit(", 1)[1]
        exempt = body.split("floodExemptRelay(text, length)", 1)[1]
        exempt = exempt.split("FloodSlot* match", 1)[0]
        self.assertIn("pthread_mutex_unlock(&gFloodMutex);", exempt)
        self.assertIn("writeToSinks(summaries.data(), summaries.size());",
                      exempt)
        self.assertIn("return true;", exempt)


class TheLaneAnswersOneNameForItself(unittest.TestCase):
    """A candidate the trace was added to find, closed while adding it.

    The last thing the failing program touched before it stopped was Wine's
    ws2_32, and every 64-bit session opens with
    `err:winediag:getaddrinfo Failed to resolve your host name IP` while no
    IA-32 session does. The reason is here: glibc's gethostname() is
    uname().nodename and getaddrinfo() of that name is answered from
    /etc/hosts, both of which this project synthesises - but the 64-bit uname
    answered a constant that /etc/hosts never mentions, so the local host
    resolved to nothing.
    """

    def setUp(self) -> None:
        self.source = read(REPO / "source" / "kernel" / "syscall64.cpp")

    def test_the_node_name_comes_from_the_file_that_defines_it(self) -> None:
        body = self.source.split("static void x64GuestNodeName(", 1)
        self.assertEqual(len(body), 2, "x64GuestNodeName is missing")
        body = body[1].split("\n}\n", 1)[0]
        self.assertIn('B("/etc/hostname")', body)

    def test_uname_uses_it(self) -> None:
        body = self.source.split("static U64 sys_uname64(", 1)
        self.assertEqual(len(body), 2, "sys_uname64 moved")
        body = body[1].split("\n}\n", 1)[0]
        self.assertIn("x64GuestNodeName(nodeName, sizeof(nodeName))", body)
        self.assertIn("setField(1, nodeName)", body)
        self.assertNotIn('setField(1, "', body,
                         "the node name is a constant again")

    def test_it_is_the_same_file_the_hosts_table_is_built_from(self) -> None:
        # openHostname and openHosts write the same host name, so the three
        # answers agree only while all three read the one source.
        sockets = read(REPO / "source" / "kernel" / "knativesocket.cpp")
        self.assertIn("FsOpenNode* openHostname(", sockets)
        self.assertIn("FsOpenNode* openHosts(", sockets)
        startup = read(STARTUP_ARGS)
        self.assertIn('Fs::addVirtualFile(B("/etc/hostname"), openHostname',
                      startup)
        self.assertIn('Fs::addVirtualFile(B("/etc/hosts"), openHosts', startup)

    def test_an_unreadable_file_leaves_a_usable_name(self) -> None:
        body = self.source.split("static void x64GuestNodeName(", 1)[1]
        body = body.split("\n}\n", 1)[0]
        self.assertIn('std::strncpy(out, "boxedwine64", size - 1)', body)


class TheRelayOutputReachesTheLog(unittest.TestCase):
    def test_guest_stderr_is_mirrored_into_the_session_log(self) -> None:
        # Relay is written to the traced process's stderr and to nothing else,
        # so the trace is only worth turning on because sys_write64 tees fd 1
        # and 2 into klog, which the capture thread above reads.
        source = read(REPO / "source" / "kernel" / "syscall64.cpp")
        self.assertIn('klog_fmt("[guest fd=%llu pid=%u %s] %s"', source)


if __name__ == "__main__":
    unittest.main()
