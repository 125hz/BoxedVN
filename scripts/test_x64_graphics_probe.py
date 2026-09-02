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
    "shaders",
    "geometry",
    "present",
)
SHADER_HLSL = REPO / "scripts" / "guest-probes" / "x64-d3d11-cube.hlsl"
SHADER_HEADER = REPO / "scripts" / "guest-probes" / "x64-d3d11-cube-shaders.h"
SHADER_GENERATOR = REPO / "scripts" / "generate-x64-probe-shaders.py"


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
        for stage in ("register-class", "create-window", "d3d11-create",
                      "shaders", "geometry"):
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


class CubeGeometryAndShaders(unittest.TestCase):
    """The probe draws a cube through embedded DXBC.

    The first device runs only cleared the render target, which proved
    presentation and nothing about shaders: a clear is a render pass load
    action, not a pipeline. The cube exercises the DXBC-to-Metal translation,
    an input layout, three buffers, a viewport, and an indexed draw. The DXBC
    is compiled ahead of time with fxc, which exists only in the Windows SDK,
    so the generated header is committed and pinned to the HLSL it came from.
    """

    def setUp(self) -> None:
        self.source = PROBE_SOURCE.read_text(encoding="utf-8")
        self.header = SHADER_HEADER.read_text(encoding="utf-8")

    def test_the_probe_draws_an_indexed_cube_every_frame(self) -> None:
        loop_at = self.source.find('stage_begin("present")')
        loop = self.source[loop_at:]
        for call in ("ID3D11DeviceContext_UpdateSubresource(",
                     "ID3D11DeviceContext_ClearRenderTargetView(",
                     "ID3D11DeviceContext_DrawIndexed(",
                     "IDXGISwapChain_Present("):
            self.assertIn(call, loop)
        # The clear precedes the draw, the draw precedes the present.
        self.assertLess(loop.find("ClearRenderTargetView"),
                        loop.find("DrawIndexed"))
        self.assertLess(loop.find("DrawIndexed"), loop.find("_Present("))

    def test_the_loop_runs_until_the_window_closes_without_vsync(self) -> None:
        # A 240-frame cut-off froze the picture on device after four seconds
        # and looked like a hang; the acceptance marker still fires at 240.
        loop = self.source[self.source.find('stage_begin("present")'):]
        self.assertIn("for (frames = 0; ; ++frames) {", loop)
        self.assertIn("if (message.message == WM_QUIT) {", loop)
        self.assertIn("IDXGISwapChain_Present(swapchain, 0, 0)", loop)
        self.assertIn("if (frames + 1 == kProbeAcceptanceFrames) {", loop)
        self.assertIn('stage_line("complete ok");', loop)
        self.assertIn('"exit ok frames=%d"', loop)

    def test_pipeline_state_is_complete(self) -> None:
        # Direct3D 11 has no default viewport; a missing one draws nothing
        # and reports nothing, which would look exactly like a DXMT defect.
        for call in ("RSSetViewports", "IASetInputLayout", "IASetVertexBuffers",
                     "IASetIndexBuffer", "IASetPrimitiveTopology",
                     "VSSetShader", "VSSetConstantBuffers", "PSSetShader"):
            self.assertIn(call, self.source, f"{call} is never called")

    def test_shaders_and_geometry_report_the_failing_object(self) -> None:
        for step in ("vertex-shader", "pixel-shader", "input-layout",
                     "vertex-buffer", "index-buffer", "constant-buffer"):
            self.assertIn(f"stage={step}", self.source,
                          f"a failed {step} would not be named")

    def test_the_cube_is_wound_clockwise_from_outside(self) -> None:
        # Direct3D's default rasterizer culls counter-clockwise triangles in
        # its left-handed convention. Without a depth buffer the cube only
        # renders correctly if every triangle is clockwise seen from outside,
        # which for these coordinates means the right-hand-rule normal points
        # away from the centre.
        vertex_block = re.search(r"kCubeVertices\[8\] = \{(.*?)\};",
                                 self.source, re.S).group(1)
        vertices = [tuple(float(v) for v in m)
                    for m in re.findall(
                        r"\{\{\s*(-?[\d.]+)f,\s*(-?[\d.]+)f,\s*(-?[\d.]+)f\}",
                        vertex_block)]
        self.assertEqual(len(vertices), 8)
        index_block = re.search(r"kCubeIndices\[36\] = \{(.*?)\};",
                                self.source, re.S).group(1)
        indices = [int(v) for v in re.findall(r"\b(\d+)\b", index_block)]
        self.assertEqual(len(indices), 36)
        for t in range(0, 36, 3):
            a, b, c = (vertices[i] for i in indices[t:t + 3])
            ab = [b[i] - a[i] for i in range(3)]
            ac = [c[i] - a[i] for i in range(3)]
            n = (ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
                 ab[0] * ac[1] - ab[1] * ac[0])
            centre = [(a[i] + b[i] + c[i]) / 3 for i in range(3)]
            self.assertGreater(sum(n[i] * centre[i] for i in range(3)), 0,
                               f"triangle {indices[t:t + 3]} faces inward")

    def test_the_matrix_layout_matches_the_hlsl(self) -> None:
        # The C side uploads a row-major array applied as M * v; the HLSL
        # declares the matrix row_major and multiplies mul(mvp, v). Either
        # side changing alone would silently render nothing recognisable.
        hlsl = SHADER_HLSL.read_text(encoding="utf-8")
        self.assertIn("row_major float4x4 mvp", hlsl)
        self.assertIn("mul(mvp, float4(input.position, 1.0))", hlsl)
        self.assertIn("m[4][4]", self.source)
        self.assertIn("r.m[0][3] = x;", self.source)

    def test_the_embedded_dxbc_is_pinned_to_the_hlsl(self) -> None:
        import hashlib
        digest = hashlib.sha256(SHADER_HLSL.read_bytes()).hexdigest()
        self.assertIn(f'#define BOXEDVN_PROBE_HLSL_SHA256 "{digest}"',
                      self.header,
                      "the HLSL changed; run scripts/generate-x64-probe-"
                      "shaders.py on a machine with fxc")
        self.assertIn('#include "x64-d3d11-cube-shaders.h"', self.source)
        self.assertTrue(SHADER_GENERATOR.is_file())

    def test_the_embedded_blobs_are_well_formed_dxbc(self) -> None:
        import struct
        for name in ("kProbeVertexShader", "kProbePixelShader"):
            match = re.search(
                rf"static const BYTE {name}\[(\d+)\] = \{{(.*?)\}};",
                self.header, re.S)
            self.assertIsNotNone(match, f"{name} is missing")
            declared = int(match.group(1))
            data = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})",
                                                        match.group(2)))
            self.assertEqual(len(data), declared)
            self.assertEqual(data[:4], b"DXBC")
            self.assertEqual(struct.unpack_from("<I", data, 24)[0], len(data))

    def test_the_builder_checks_the_new_markers(self) -> None:
        builder = PROBE_BUILDER.read_text(encoding="utf-8")
        self.assertIn("'shaders'", builder)
        self.assertIn("'geometry'", builder)

    def test_the_shader_header_invalidates_the_cache(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("'scripts/guest-probes/x64-d3d11-cube-shaders.h'",
                      workflow)


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


class WindowBoundaryDiagnostics(unittest.TestCase):
    """What the probe reports around the window-creation boundary.

    A device run stopped at CreateWindowExW with ERROR_INVALID_WINDOW_HANDLE
    and no X11 activity anywhere in the log. The error names a handle, but not
    which one, so the probe reports what the window would have been created
    into before it tries.
    """

    def setUp(self) -> None:
        self.source = PROBE_SOURCE.read_text(encoding="utf-8")

    def test_the_desktop_environment_is_reported_before_the_attempt(self) -> None:
        desktop_at = self.source.find("desktop-env")
        attempt_at = self.source.find('stage_begin("create-window")')
        self.assertNotEqual(desktop_at, -1, "the desktop state is never reported")
        self.assertLess(desktop_at, attempt_at,
                        "the desktop state is reported after the attempt that "
                        "needs it")
        for probe in ("GetDesktopWindow()", "GetThreadDesktop(",
                      "GetProcessWindowStation()",
                      'GetEnvironmentVariableA("DISPLAY"'):
            self.assertIn(probe, self.source)

    def test_the_popup_fallback_is_separate_and_never_success(self) -> None:
        # A narrower window separates "no user driver at all" from "a driver
        # that cannot build a full overlapped frame".
        self.assertIn('stage_begin("create-window-popup")', self.source)
        self.assertIn('stage_ok("create-window-popup"', self.source)
        self.assertIn('stage_fail("create-window-popup"', self.source)
        # It must not become the window the rest of the program uses: a frame
        # the driver could not build properly is not a presentation surface.
        popup_at = self.source.find('stage_begin("create-window-popup")')
        tail = self.source[popup_at:popup_at + 1200]
        self.assertIn("DestroyWindow(window);", tail)
        self.assertIn("return BOXEDVN_EXIT_CREATE_WINDOW;", tail)

    def test_the_fallback_does_not_change_the_reported_stage_code(self) -> None:
        # The process still exits on the create-window stage, so the status
        # keeps naming the boundary that actually failed.
        self.assertNotIn("BOXEDVN_EXIT_CREATE_WINDOW_POPUP", self.source)
