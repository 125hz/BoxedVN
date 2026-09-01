"""BoxedVN - contract tests for the instrumented x86-64 Direct3D 11 probe.

Two device runs showed the staged probe initialising Wine, finding both DXMT
DLLs, executing about fourteen thousand guest syscalls, and then leaving
through exit_group with status 1400. That status was ambiguous by construction:
three separate branches could produce it, and each displays a message box
before reading GetLastError -- and a message box overwrites the thread's last
error, so the number that came out was not necessarily the one that mattered.

The probe is now built from source in this repository and announces the stage
it reached. These tests hold that property in place: the markers exist, the
error is captured at the failing call rather than after it, the exit codes are
distinct per stage, and the staging script refuses anything that is not the
instrumented binary.

They are source-level contracts on purpose. Compiling a Windows PE needs the
pinned llvm-mingw toolchain, which is a macOS CI artifact; the compile itself
is checked in the x64-graphics-assets job, where the toolchain exists.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PROBE_SOURCE = REPO / "scripts" / "guest-probes" / "x64-d3d11-cube.c"
PROBE_BUILDER = REPO / "scripts" / "build-x64-graphics-probe.sh"
STAGER = REPO / "scripts" / "stage-x64-graphics-assets.sh"
WORKFLOW = REPO / ".github" / "workflows" / "build-ios.yml"

# Every stage the device log has to be able to distinguish.
STAGES = (
    "register-class",
    "create-window",
    "d3d11-create",
    "dxgi-factory",
    "swapchain",
    "render-target",
    "present",
)


class ProbeMarkers(unittest.TestCase):
    def setUp(self) -> None:
        self.source = PROBE_SOURCE.read_text(encoding="utf-8")

    def test_every_stage_announces_begin_and_both_outcomes(self) -> None:
        for stage in STAGES:
            self.assertIn(f'stage_begin("{stage}")', self.source,
                          f"{stage} never announces that it started")
            self.assertTrue(
                f'stage_ok("{stage}"' in self.source
                or f'stage_line("{stage}' in self.source,
                f"{stage} never reports success")
        # A stage that can fail has to say so with the captured error.
        for stage in ("register-class", "create-window", "d3d11-create"):
            self.assertIn(f'stage_fail("{stage}"', self.source,
                          f"{stage} never reports failure")

    def test_markers_carry_the_shared_prefix(self) -> None:
        self.assertIn('"BOXEDVN_X64_CUBE_STAGE "', self.source)

    def test_markers_go_to_the_windows_stderr_handle(self) -> None:
        # Not the CRT: the bytes have to reach the guest's fd 2 in one
        # unbuffered write, so a process that dies immediately afterwards
        # still leaves its last marker behind.
        self.assertIn("GetStdHandle(STD_ERROR_HANDLE)", self.source)
        self.assertIn("WriteFile(err", self.source)

    def test_the_error_is_captured_at_the_failing_call(self) -> None:
        # The defect this replaces: reading GetLastError after a message box.
        # Every call whose failure is reported must be immediately followed by
        # the capture, with nothing between them.
        pattern = re.compile(
            r"SetLastError\(0\);\s*\n\s*(?:hr = |atom = |window = )[^;]*;\s*\n"
            r"\s*captured = GetLastError\(\);",
            re.MULTILINE)
        self.assertGreaterEqual(len(pattern.findall(self.source)), 5,
                                "a reported call does not capture its error "
                                "immediately")

    def test_no_message_box_can_overwrite_the_error(self) -> None:
        self.assertNotIn("MessageBox", self.source)

    def test_stage_exit_codes_are_distinct_and_documented(self) -> None:
        codes = dict(re.findall(r"(BOXEDVN_EXIT_[A-Z_]+) = (\d+)", self.source))
        self.assertIn("BOXEDVN_EXIT_OK", codes)
        self.assertEqual(codes["BOXEDVN_EXIT_OK"], "0")
        values = list(codes.values())
        self.assertEqual(len(values), len(set(values)),
                         "two stages share an exit code, so the status cannot "
                         "name the stage")
        # Every failure path returns its own stage's code.
        for name in codes:
            if name == "BOXEDVN_EXIT_OK":
                continue
            self.assertIn(f"return {name};", self.source)

    def test_the_original_error_is_reported_not_returned(self) -> None:
        # A process status has room for one number; the marker does not. The
        # HRESULT and the Win32 error stay in the marker.
        self.assertIn("win32=%lu", self.source)
        self.assertIn("hr=0x%08lx", self.source)

    def test_the_render_loop_does_not_narrate_every_frame(self) -> None:
        # A per-frame marker would be the largest thing in a device log.
        self.assertIn("if (!presented) {", self.source)
        self.assertIn("presented = 1;", self.source)

    def test_the_probe_uses_the_import_the_abi_validator_requires(self) -> None:
        # validate-dxmt-guest-abi.py requires d3d11.dll!D3D11CreateDevice.
        # The AndSwapChain form would satisfy the program and fail the check,
        # and would also collapse two stages that fail for different reasons.
        self.assertIn("D3D11CreateDevice(", self.source)
        self.assertNotIn("D3D11CreateDeviceAndSwapChain", self.source)


class ProbeBuildAndStaging(unittest.TestCase):
    def test_builder_targets_the_pinned_mingw_and_console_subsystem(self) -> None:
        builder = PROBE_BUILDER.read_text(encoding="utf-8")
        self.assertIn("x86_64-w64-mingw32-clang", builder)
        # -mwindows would leave the program with no stderr handle, which is
        # the one thing it exists to write to. Checked against the compile
        # invocation only; the comment above it says the same thing in prose.
        code = [line for line in builder.splitlines()
                if not line.lstrip().startswith("#")]
        self.assertNotIn("-mwindows", chr(10).join(code))
        self.assertIn("-ld3d11", builder)
        self.assertIn("-ldxgi", builder)

    def test_builder_verifies_the_image_and_its_markers(self) -> None:
        builder = PROBE_BUILDER.read_text(encoding="utf-8")
        self.assertIn("PE32", builder)
        self.assertIn("BOXEDVN_X64_CUBE_STAGE ", builder)

    def test_stager_refuses_a_probe_without_markers(self) -> None:
        stager = STAGER.read_text(encoding="utf-8")
        self.assertIn("stage marker", stager)
        self.assertIn("BOXEDVN_X64_CUBE_STAGE ", stager)

    def test_stager_no_longer_copies_the_opaque_prebuilt_binary(self) -> None:
        stager = STAGER.read_text(encoding="utf-8")
        # The staged OUTPUT keeps its name; what must be gone is the copy out
        # of the pinned integration checkout and the option that pointed at it.
        self.assertNotIn("arm64ec-windows", stager)
        self.assertNotIn("${MYTHIC}", stager)
        self.assertNotIn("--mythic", stager)
        self.assertIn("--probe", stager)

    def test_workflow_builds_the_probe_before_staging_it(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        build_at = workflow.find("build-x64-graphics-probe.sh")
        stage_at = workflow.find("stage-x64-graphics-assets.sh")
        self.assertNotEqual(build_at, -1, "the probe is never built")
        self.assertNotEqual(stage_at, -1, "the probe is never staged")
        self.assertLess(build_at, stage_at,
                        "the probe is staged before it is built")
        self.assertIn("--probe build/guest-probes/boxedvn-d3d11-cube-x64.exe",
                      workflow)

    def test_probe_source_and_builder_invalidate_the_cache(self) -> None:
        # A cached third-party tree must not hide a changed probe.
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("'scripts/guest-probes/x64-d3d11-cube.c'", workflow)
        self.assertIn("'scripts/build-x64-graphics-probe.sh'", workflow)


if __name__ == "__main__":
    unittest.main()
