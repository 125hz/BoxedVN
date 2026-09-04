"""BoxedVN - contract tests for who owns the x86-64 translated role.

A user opened the 64-bit desktop, browsed to a program in the file manager and
double-clicked it. Three times, the program died at once:

    BOXEDWINE_X64_EXEC pid=45 path=/usr/lib/x86_64-linux-gnu/wine/wine fex=0
    BOXEDWINE_DXMT_RETURN ordinal=0 status=-38 reason=native-memory pid=45 fex=0
    BOXEDWINE_X64_PROC_EXIT pid=45 parent=43 status=3221225477 group=1

0xC0000005 is an access violation. `fex=0` and `reason=native-memory` are the
cause: the program ran on the interpreter with a sparse address space, so
Direct3D was refused before it began.

Exactly one process per session can be translated, and in that session it was
pid 10 -- Wine's desktop shell -- which held the role from launch to shutdown
and made no Direct3D call in the whole log. The role sat on the one process
that had no use for it and could not reach the one that did.

What is actually exclusive is the identity mapping: host address windows at
compile-time constant addresses, which FEX's translated code dereferences
guest pointers into. Two address spaces cannot both hold them. Nothing else
about the backend is exclusive -- it keeps a FEX context per KProcess,
installs its host fault handlers once and chains them, and keeps its
dispatcher state in thread-locals.

So the role cannot be taken from a live owner -- that owner's registers, stack
and translated blocks all hold host addresses inside those windows -- but it
can be taken when it is free, at the one point where a process has given up
one address space and not yet built the next: execve. These tests pin that
rule. A launch that names Wine's own infrastructure does not take the role;
the first top-level Windows program started from it does, at its own exec; and
the process that takes it leaves the interpreter instead of being stepped for
the rest of its life.

Source-level contracts on purpose: building the emulator needs the iOS
toolchain, and there is no host here on which a 64-bit guest can run.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
KPROCESS = REPO / "source" / "kernel" / "kprocess.cpp"
CPU64_HEADER = REPO / "include" / "cpu64.h"
CPU64 = REPO / "source" / "emulation" / "cpu" / "cpu64.cpp"
PLATFORM = (REPO / "source" / "emulation" / "cpu" / "normal" /
            "normalPlatformMultiThreaded.cpp")
SCHEDULER = REPO / "source" / "kernel" / "kscheduler.cpp"
SYSCALL64 = REPO / "source" / "kernel" / "syscall64.cpp"
ADAPTER = REPO / "ios" / "runtime" / "src" / "BVNFEXCPU64Adapter.mm"
BACKEND = REPO / "ios" / "runtime" / "src" / "BVNFEXBackend.mm"
APP_MODEL = REPO / "ios" / "app" / "Sources" / "AppModel.swift"

BACKSLASH = "\\"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


# Every exec the device session performed, taken from the `cmd=` witnesses in
# boxedvn-20260903-221042.log and written as the argument vectors
# KProcess::execve sees. The last one is the program the user double-clicked;
# it is the only one that may take the translated role.
DESKTOP_SESSION_EXECS = (
    (("/usr/lib/x86_64-linux-gnu/wine/wine",
      "C:\\windows\\system32\\explorer.exe",
      "/desktop=shell,800x600", "winefile", "D:\\"), False,
     "the desktop shell"),
    (("/usr/lib/x86_64-linux-gnu/wine/wineserver", "-p"), False,
     "the wineserver"),
    (("/usr/lib/x86_64-linux-gnu/wine/wine",
      "C:\\windows\\system32\\wineboot.exe", "--init"), False, "wineboot"),
    (("/usr/lib/x86_64-linux-gnu/wine/wine",
      "C:\\windows\\system32\\services.exe"), False, "services"),
    (("/usr/lib/x86_64-linux-gnu/wine/wine",
      "C:\\windows\\system32\\winedevice.exe"), False, "winedevice"),
    (("/usr/lib/x86_64-linux-gnu/wine/wine", "winefile", "D:\\"), False,
     "the file manager"),
    (("/usr/lib/x86_64-linux-gnu/wine/wine",
      "D:\\Games\\Program\\Program.exe"), True,
     "the program the user launched"),
)


def infrastructure_names(source: str) -> tuple:
    """The kInfrastructure table, as the C++ actually spells it."""
    table = source.split("static const char* const kInfrastructure[] = {", 1)[1]
    table = table.split("};", 1)[0]
    return tuple(re.findall(r'"([^"]+)"', table))


def names_wine_infrastructure(args, names) -> bool:
    lowered = tuple(name.lower() for name in names)
    return any(arg.lower().endswith(name) for arg in args for name in lowered)


def is_top_level_windows_program(arg: str) -> bool:
    """The Python twin of KProcess.cpp's argIsTopLevelWindowsProgram."""
    if len(arg) < 8:
        return False
    if not arg[0].isalpha() or arg[1] != ":" or arg[2] != BACKSLASH:
        return False
    if not arg.lower().endswith(".exe"):
        return False
    return not arg[3:].lower().startswith("windows" + BACKSLASH)


def claims_role(args, names) -> bool:
    if names_wine_infrastructure(args, names):
        return False
    return any(is_top_level_windows_program(arg) for arg in args)


class WhatIsActuallyExclusive(unittest.TestCase):
    """The identity mapping, and only the identity mapping."""

    def test_fex_refuses_a_process_without_the_identity_mapping(self) -> None:
        # This is why the role exists at all: the adapter will not bind a
        # process whose address space is sparse.
        adapter = read(ADAPTER)
        validator = adapter.split("static bool validAdapter(", 1)[1][:900]
        self.assertIn("nativeIdentityMode()", validator)

    def test_the_backend_itself_is_not_single_process(self) -> None:
        # Had this been exclusive too, no placement rule would have helped.
        backend = read(BACKEND)
        self.assertIn("gLiveProcesses", backend)
        self.assertIn("std::unique_ptr<LiveProcessState>>", backend)
        for per_thread in ("thread_local std::jmp_buf gExitJump",
                           "thread_local bool gCanJump",
                           "thread_local CPU64* gActiveCPU",
                           "thread_local FEXCore::SignalDelegator*"):
            self.assertIn(per_thread, backend)
        self.assertIn("std::call_once(gFEXSignalInstallOnce", backend)

    def test_the_reason_is_recorded_where_the_rule_lives(self) -> None:
        self.assertIn("whole of the constraint", read(KPROCESS))


class TheRoleIsNeverTakenFromALiveOwner(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(KPROCESS)

    def test_there_is_one_registry_with_one_owner(self) -> None:
        self.assertIn("const KProcess* gTranslatorRoleOwner = nullptr;",
                      self.source)
        self.assertIn("U32 gTranslatorRoleOwnerId = 0;", self.source)

    def test_a_take_fails_while_anyone_else_holds_it(self) -> None:
        take = self.source.split("bool translatorRoleTryTake(", 1)[1]
        take = take.split("\n}", 1)[0]
        # Re-entrant for the current owner, refused for everyone else.
        self.assertIn("if (gTranslatorRoleOwner == process) {", take)
        self.assertIn("if (gTranslatorRoleOwner != nullptr) {", take)
        self.assertIn("return false;", take)

    def test_the_release_is_tied_to_the_address_space(self) -> None:
        # Not to the exit status. A process that has exited but whose
        # KMemory64 still exists still owns the host windows, and handing the
        # role on at that point would map them twice.
        destructor = self.source.split("KProcess::~KProcess() {", 1)[1]
        destructor = destructor.split("\n}", 1)[0]
        self.assertLess(destructor.index("delete memory64;"),
                        destructor.index("translatorRoleRelease(this)"),
                        "the role must be released only after the identity "
                        "mapping is unmapped")

    def test_a_fork_child_never_inherits_it(self) -> None:
        fork = self.source.split("U32 KProcess::forkProcess64(", 1)[1][:3000]
        self.assertIn("childProcess->useFEX64 = false;", fork)


class WineInfrastructureDoesNotHoldTheRole(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(KPROCESS)
        self.names = infrastructure_names(self.source)

    def test_the_shell_is_named(self) -> None:
        # explorer.exe holding the role for a whole session is the defect this
        # rule exists to prevent.
        for name in ("explorer.exe", "wineserver", "services.exe",
                     "winedevice.exe", "wineboot.exe", "plugplay.exe",
                     "start.exe"):
            self.assertIn(name, self.names)

    def test_a_launch_that_names_infrastructure_does_not_seed_the_role(self):
        start = self.source.split("KThread* KProcess::startProcess(", 1)[1]
        start = start[:start.index("std::shared_ptr<FsNode> node =")]
        self.assertIn("argsNameWineInfrastructure(argValues)", start)
        self.assertIn("if (!infrastructure && translatorRoleTryTake(this)) {",
                      start)

    def test_the_rule_decides_the_device_session_the_way_it_must(self) -> None:
        for args, expected, description in DESKTOP_SESSION_EXECS:
            self.assertEqual(
                claims_role(args, self.names), expected,
                f"{description} should {'' if expected else 'not '}claim the "
                f"translated role")

    def test_a_program_inside_the_windows_directory_is_not_top_level(self):
        # Wine starts plenty of its own programs by absolute path, so the .exe
        # suffix alone cannot separate them from what the user launched.
        self.assertFalse(
            is_top_level_windows_program("C:\\windows\\system32\\rpcss.exe"))
        self.assertTrue(
            is_top_level_windows_program("C:\\Program Files\\Foo\\foo.exe"))
        self.assertFalse(is_top_level_windows_program("winefile"))
        self.assertFalse(is_top_level_windows_program("D:\\"))

    def test_the_predicate_in_cpp_matches_the_one_tested_here(self) -> None:
        predicate = self.source.split("bool argIsTopLevelWindowsProgram(",
                                      1)[1]
        predicate = predicate.split("\n}", 1)[0]
        self.assertIn("arg.length() < 8", predicate)
        self.assertIn("arg.charAt(1) != ':'", predicate)
        self.assertIn('arg.endsWith(".exe", true)', predicate)
        self.assertIn('.startsWith("windows' + BACKSLASH * 2 + '", true)',
                      predicate)


class TheRoleIsTakenAtExec(unittest.TestCase):
    def setUp(self) -> None:
        source = read(KPROCESS)
        self.execve = source.split("U32 KProcess::execve(", 1)[1]
        self.execve = self.execve[
            :self.execve.index("void KProcess::signalProcess(")]

    def test_only_a_process_that_holds_nothing_asks(self) -> None:
        self.assertIn("if (this->is64Bit && !this->useFEX64) {", self.execve)

    def test_the_claim_needs_all_three_conditions(self) -> None:
        claim = self.execve.split("const bool wantsRole =", 1)[1][:400]
        self.assertIn("!this->systemProcess", claim)
        self.assertIn("!argsNameWineInfrastructure(args)", claim)
        self.assertIn("argsNameTopLevelWindowsProgram(args)", claim)

    def test_the_claimer_gets_an_identity_mapping(self) -> None:
        # Without this the process is marked translated and then handed a
        # sparse address space, which the adapter refuses.
        self.assertIn(
            "const bool preserveNativeIdentity = claimedTranslatorRole ||",
            self.execve)

    def test_the_claim_happens_before_the_address_space_is_replaced(self):
        self.assertLess(
            self.execve.index("claimedTranslatorRole = true;"),
            self.execve.index("new KMemory64(this, preserveNativeIdentity)"))


class TheClaimerLeavesTheInterpreter(unittest.TestCase):
    """CPU64::run() has no non-terminal exit, so one had to be added."""

    def test_yield_still_means_stop_this_thread(self) -> None:
        # If it did not, backendHandoff would be unnecessary: the dispatcher
        # returns from the platform thread entirely when yield is set.
        self.assertIn("if (cpu64->yield) {", read(PLATFORM))

    def test_the_handoff_flag_exists_and_is_separate_from_yield(self) -> None:
        self.assertIn("bool backendHandoff = false;", read(CPU64_HEADER))

    def test_both_interpreter_loops_honour_it(self) -> None:
        source = read(CPU64)
        self.assertIn("while (!yield && !backendHandoff) {", source)
        self.assertIn("while (!yield && !backendHandoff && ran < maxInsn) {",
                      source)

    def test_execve_requests_it_only_when_the_role_moved(self) -> None:
        source = read(KPROCESS)
        tail = source.split("U32 KProcess::execve(", 1)[1]
        tail = tail[:tail.index("void KProcess::signalProcess(")]
        request = tail.split("if (claimedTranslatorRole) {", 1)[1][:900]
        self.assertIn("execCpu->backendHandoff = true;", request)

    def test_every_dispatcher_clears_it_before_running(self) -> None:
        # A latched flag would stop the interpreter on every later pass.
        for path in (PLATFORM, SCHEDULER):
            self.assertIn("cpu64->backendHandoff = false;", read(path),
                          f"{path.name} never clears the handoff request")


class TheDecisionIsWitnessed(unittest.TestCase):
    def setUp(self) -> None:
        self.source = read(KPROCESS)

    def test_every_decision_point_reports(self) -> None:
        for action in ("action=seed", "action=claim", "action=release"):
            self.assertIn(action, self.source)

    def test_the_witness_says_who_held_it_and_whether_it_moved(self) -> None:
        lines = re.findall(r'"BOXEDWINE_X64_TRANSLATOR_ROLE[^;]*', self.source)
        self.assertEqual(len(lines), 3,
                         "one witness per decision point, no more and no less")
        for line in lines:
            self.assertIn("pid=%u", line)
            self.assertIn("holder=", line)
            self.assertIn("moved=", line)
            self.assertIn("reason=", line)

    def test_a_refusal_says_which_refusal_it_was(self) -> None:
        # "no translator" is not a diagnosis. These two are.
        self.assertIn("role-held-by-live-owner", self.source)
        self.assertIn("not-a-top-level-program", self.source)


class TheSessionStillEndsWithTheLaunchedProcess(unittest.TestCase):
    """The shell is no longer translated, so its exit is reported here."""

    def test_the_report_no_longer_depends_on_the_translator(self) -> None:
        source = read(SYSCALL64)
        self.assertIn("static void noteLaunchedProcessExit64(", source)
        report = source.split("static void noteLaunchedProcessExit64(", 1)[1]
        report = report[:report.index("static U64 sys_exit64(")]
        self.assertIn("BVNRuntimeNoteLaunchedProcessExited(", report)
        # Only the launched process, so quitting a program inside the desktop
        # does not take the desktop down with it.
        self.assertIn("launched->parentId > 1", report)
        self.assertIn("if (!group ", report)

    def test_it_runs_before_the_process_is_torn_down(self) -> None:
        body = read(SYSCALL64).split("static U64 sys_exit64(", 1)[1][:4000]
        self.assertLess(
            body.index("noteLaunchedProcessExit64(cpu, status, group)"),
            body.index("process->exitgroup(cpu->thread"))


class TheLauncherAgrees(unittest.TestCase):
    def test_the_desktop_no_longer_claims_the_translator_for_the_shell(self):
        source = read(APP_MODEL)
        above = source.split("func launchX64Desktop(", 1)[0]
        comment = above[above.rindex("/// Opens the container's desktop"):]
        self.assertIn("does NOT get the translator", comment)


if __name__ == "__main__":
    unittest.main()
