# Startup, input, and frame pacing at 1748826f

The four captures ending 005628, 010304, 010340, and 010416 are from the
increased-memory build 137. The two cube captures pass the x87 bit-pattern
probe, create/acquire a Vulkan swapchain image, and crash in native
`MVKBuffer::getMTLBuffer` with fault address `0x58`. The call stack reaches
that method from `vkCmdBindVertexBuffers2`.

The compatibility patch disables `nullDescriptor`, but DXVK 2.5.2 still
binds null handles for unbound vertex inputs. Vulkan requires non-null
handles when that feature is disabled, and MoltenVK 1.4.2 dereferences
each buffer in this command. The maintained DXVK patch now uses its existing
device-owned zero buffer, with zero offset/stride and its full buffer range.
That buffer no longer requests transform-feedback usage on devices without
transform feedback. Its access/stage masks include vertex and index reads.
This fixes the identified invalid binding; a first frame is not yet proven.

The 64-bit application plays sound, with the OSS witness reporting 44.1 kHz
stereo and no underruns in the observed interval. During the 4 FPS interval,
`PRESENT_GAP` reports 258.4 ms average between calls but at most 0.1 ms inside
Present. Native drawable calls are also short. This rules out a long blocking
Present for that interval, but does not identify the guest-side wait.

DXMT's periodic memory census scans allocation tables and calls Metal while
the dynamic-buffer mutex is held. It previously emitted dozens of lines
every 30 completed submissions regardless of verbose mode. Normal runs now
skip census reports and scan only the eligible FIFO prefix needed for safe
trimming. Verbose runs retain the detailed scan. Both modes retain the same
64 eligible-buffer reserve and never trim GPU-fenced entries. Present timing
is reported every five seconds rather than every 64 calls.

Repeated selection of the same Wine cursor no longer invalidates and rebuilds
its UIKit image. Repeated unchanged pointer positions also leave the overlay
revision unchanged; guest motion and button delivery remain intact.

The 32-bit visual novel reaches its launcher and records Vulkan draw commands
after startup, but the capture does not show a queue submission/first game
frame. The fixed eight-entry sampler selected threads in hash-map order;
new workers displaced the main thread from observations. It now keeps the
oldest live thread and rotates the other seven slots through workers. No
claim is made that this diagnostic change fixes the black screen.

Toolbar keys now send key-down on initial touch and key-up on release or
cancellation, including teardown and app deactivation. One-shot toolbar
actions also run on touch-down. The held joystick pad is 88 points across,
with 24 points of knob travel and a 6-point dead zone (previously 120/40/12).

Validation before CI: 518 host support cases passed; 252 Python cases ran
with 51 platform-dependent skips. Both maintained graphics patches apply to
their pinned sources. Compiled comparison of the actual before/after DXMT
FIFO scan over 20,000 ordered queues preserved eligible counts in quiet and
verbose modes, including queues with unfinished GPU entries. CI must still
compile the real iOS app and 32-bit DXVK modules before publication.

Install the increased-memory IPA and replace `main/wine64-pe32.zip`: the
cube correction lives in the rebuilt 32-bit DLLs. Retest all three paths
with verbose off. Compare idle gameplay against mouse hover and held drag;
check immediate toolbar key-down and sustained held-key behavior. Capture
the launcher-to-black-screen transition for at least 60 seconds so rotating
thread samples can identify its blocking call. All runtime execution still
uses BoxedWine, with FEX as its CPU backend.

Sources: [Vulkan vertex-buffer binding requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBindVertexBuffers2.html),
[MoltenVK 1.4.2 vertex binding implementation](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.2/MoltenVK/MoltenVK/Commands/MVKCmdDraw.mm),
DXVK 2.5.2 `dxvk_context.cpp`/`dxvk_unbound.cpp`, and pinned DXMT
`d6bd546dc685189f4434f87fd15ea7b21009e64f` allocation/census/present sources.
