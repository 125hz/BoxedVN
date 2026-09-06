# D3D9 sampler variants, Wine64 fonts and prediction guards

Device baseline: d8525df8, build 137. Captures 010144, 010627 and 011751.

The first 32-bit visual novel now progresses past the audio callback lock
cycle and is playable. Its remaining silence is not an unopened OSS device:
float32 stereo writes reach an opened 48 kHz SDL device from a 44.1 kHz guest.
The existing underrun count means an empty queue observed between writes,
not measured silent samples. Add five-second, per-voice guest/converted peak
levels and invalid-float counts to distinguish zero input from conversion or
output failure. Report SDL conversion errors instead of queueing invalid output.
No audible-audio acceptance is claimed. A replacement streaming converter was
tested locally and excluded because it did not preserve duration across packet
boundaries with the shipping SDL version.

## Descriptor aliasing

The 32-bit D3D9 capture no longer contains the earlier localization-buffer
fault. It presents over 1,500 frames, but fragment pipeline creation fails with
undeclared `s0_2d_shadowSmplr`, `s1_2d_shadowSmplr` and `s2_2d_shadowSmplr`.
The later device loss explicitly reports background GPU execution prohibition;
it occurs after the shader errors and does not explain the initial black screen.
The second visual novel also reports this shader failure.

This matches [MoltenVK issue 2768](https://github.com/KhronosGroup/MoltenVK/issues/2768).
The repository patch adapts the binding layout approach proposed by MiloszP in
[DXVK-macOS PR 20](https://github.com/Gcenx/DXVK-macOS/pull/20), commit
217f1c03e89bdcce5f31af442f58810f66d6556e, to the pinned DXVK 2.5.2 interfaces.
Automatic activation uses the Vulkan driver ID, with no executable matching.

Reserve five distinct bindings per D3D9 texture for 2D, 3D, cube, depth-2D and
depth-cube. Register precise view types when declaring each DXSO sampler;
route the runtime view to its matching slot and clear inactive slots on every
rebind/unbind/reset. Bind samplers to all variants. Fixed-function bindings use
the same texture type and depth-compare selector. DXVK 2.5.2 uses its existing
texture-binding masks rather than the per-descriptor bound specialization
constants in the older proposal. SWVP/specification buffers remain disjoint.

The host fixture compiles the actual patched binding helpers and checks every
stage, texture, variant, constant buffer and helper slot for collisions and
resource-table overflow. The complete PE32 DXVK build and embedded marker
validation are required by CI. Real Metal pipeline creation remains a device
acceptance check; unbound resources retain the pinned backend's existing policy.

## Return-prediction helper guard

The second visual novel's settings-return freeze records an aligned predictor
push (`a9bf7f3f`, `stp xzr,xzr,[x25,#-16]!`) at host PC 113db2280, x25=15dc10000,
fault=15dc0fff0. This is an owned FEX runtime helper outside the translated-block
ranges. The existing recovery was bypassed by its `inCodeBuffer` condition.
Admit helpers in the immutable executable-pool registry as well, while retaining
the current thread's guard mapping, precise address, x25, width, offset and
instruction checks. Reset only the host predictor and retry the instruction;
never alter Wine's guest stack or skip a guest fault.

## Fonts and cursor

Font import previously ran only in the non-FEX prefix preparation branch and
always targeted `.wine/drive_c`, even if another directory was mounted over C:.
Import user fonts before Wine64 starts into the active mounted C: directory,
or `.wine64/drive_c` when no mount is configured. Preserve installed files and
the legacy lane. [Wine 9 font initialization](https://github.com/wine-mirror/wine/blob/wine-9.0/dlls/win32u/font.c)
scans the Windows font directory. No font redistribution or application font
substitution is introduced.

Apply the existing opacity preference to the Wine cursor image as well as the
touch indicator, including cursor bitmap changes. A zero opacity hides the host
overlay while leaving a cursor rendered into the game's own picture untouched.

The startup delay is still unresolved: the new capture spends substantial CPU
time in Wine kernelbase collation and conversion (actual module base 7b620000;
7b646eb0=append_sortkey, 7b648e74=append_weights), with ntdll memcmp/free activity.
Do not label ordinary service waits as its cause or bypass application loading.

## Acceptance

Host CTest suite and FEX exit-dispatch source contracts pass locally. The added
font fixture covers mounted Wine64 C:, unmounted Wine64 C:, and preserving an
installed font; existing legacy cases remain. The predictor fixture covers the
new helper guard address. CI must compile the full DXVK and iOS runtime before
publication. The rebuilt d3d9.dll requires replacing `main/wine64-pe32.zip` along
with the increased-memory IPA. No game files or saves were modified or executed.
