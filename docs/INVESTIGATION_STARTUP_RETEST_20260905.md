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


## Device revision c83e2ea7, 02:09-02:15 retest

The five device logs identify c83e2ea7+dirty. The cube now completes three
vkQueuePresentKHR calls. Its subsequent null branch originates in DXVK
Presenter::runFrameThread: the exact CI d3d9.dll disassembly at preferred VA
0x6252e587 calls the function slot for vkWaitForPresentKHR, returning to
0x6252e58d (device 0x7b2ee58d). The worker caller also matches
thread::threadProc at preferred VA 0x6256e6fa. The 64-bit guest ICD advertised
present-wait features but had no entry point for that command. Add the real
host dispatch with full-width swapchain, present ID and timeout. The existing
null-target diagnostic assumes an eight-byte stack slot even in 32-bit code;
its `rsp_before_pop` must not be treated as proof of a bad RET.

The 32-bit visual novel reaches its launcher, then requests wineoss.drv;
all syswow64 searches fail with c0000135. Build both OSS PE architectures
from the same Wine 9.0 tree, retaining one ELF64 Unix library and its upstream
WoW64 call table. Stage the i386 driver into wine64-pe32.zip and require it
in CI validation. This repairs a demonstrated missing dependency; black-screen
resolution still requires device testing.

Some toolbar taps have key-down and key-up with identical X11 timestamps
(for example 58694 in the 02:11 log). Disable delaysContentTouches on ancestor
scroll views so UIKit does not postpone control touch-down until the tap ends.
Actual lift/cancel still releases the guest key. Coalesce trackpad motion to
60 samples/s before Wine's message pump while updating the local pointer on
every touch. Flush the last point before releasing a drag. Reduce the remaining
DXMT native-present and frame-stat logs to a five-second cadence.

The 64-bit game fails a read at 0x1400db6c5, instruction `mov r8d,[rcx+r8*8]`,
with both base and index zero. New-game behavior has not been tested. This is
not evidence that the graphics driver crashed, nor enough to justify skipping
the instruction or changing game state. Add bounded, safely read instruction
context around guest faults to identify how those operands were produced.

Validation before CI: 259 Python checks (51 platform skips); host support suite
passes; guest ICD compiles with GCC syntax validation; shell syntax and DXMT
patch application checks pass. CI and device acceptance are separate.

Primary references:
- https://developer.apple.com/documentation/uikit/uiscrollview/delayscontenttouches
- https://docs.vulkan.org/refpages/latest/refpages/source/vkWaitForPresentKHR.html
- https://github.com/wine-mirror/wine/blob/wine-9.0/dlls/wineoss.drv/oss.c

## Device revision e070cb97, morning retest

All three new logs identify e070cb97+dirty, build 137. The D3D9 cube
completes dozens of presents and vkWaitForPresentKHR now returns success.
Its reported rate is about two presents per second, with little host CPU
use and short acquire/queue-present calls. The final interval contains a
54-second gap in both sampler and main-loop progress; the log alone does
not distinguish an event-pump stall from application suspension. Add
separate cumulative timing for present completion, which releases DXVK's
frame-latency signal and was missing from the existing counters.

SDL 2.32.10's UIKit_PumpEvents drains each CFRunLoop mode until no ready
sources remain. A source that keeps becoming ready can monopolize that
pass, delaying SDL callbacks and guest work despite processing UIKit
events. Bound each mode to 32 sources or two milliseconds, preserving both
default and tracking modes. This bounds draining, not the execution time
of an individual UIKit callback. A compiled harness exercises the actual
patched function with continuously ready, timed, slow, idle and disabled
sources. The dependency cache and per-prefix SDL build stamp include the
patch, and the harness runs before the dependency build.

The 32-bit visual novel loads the new i386 OSS driver, completes its first
640x360 present, then faults at host address 0x20 during another graphics
pipeline creation. MoltenVK 1.4.2's symbol offset +0xbc decodes to a load
from pVertexSS->pName with pVertexSS null. Wine 9's pipeline creation error
path can reject the draw, but cannot run if the native driver crashes.
Port the existing IA-32 structural guard to the x64 Vulkan bridge and
extend both callers to reject complete fragment-only pipelines. Preserve
mesh stages, partial libraries and pipelines linking libraries. Clear all
output handles on refusal and bound repeated error logging. This contains
the demonstrated native crash; it does not synthesize a missing shader or
prove that the subsequent game scene renders.

Read-only inspection of the locally supplied 64-bit executable confirms
the same null array read at image offset 0xdb6c5. The array is supplied as
a stack argument through the caller at 0xdb6f0; its data field at +0x10 is
zero. The surrounding code uses an eight-byte-element container with
count/capacity at +8/+12. The user confirms copying the complete replacement
folder, including cache. No executable was launched or modified. Do not
skip this instruction or claim a graphics fix for this guest exception.
Capture bounded 256-byte fault stacks and safely copied operand/argument
headers to distinguish an empty container from corrupted state next run.
The origin of that empty array remains unresolved in this build.

Pre-CI validation: 259 Python cases, with 51 platform skips; host support
suite passes, including vertex/fragment/mesh/library guard cases. The SDL
patch applies to the exact pinned source and its five behavior checks pass.
All execution remains inside BoxedWine with FEX for CPU translation.

References:
- https://github.com/libsdl-org/SDL/blob/release-2.32.10/src/video/uikit/SDL_uikitevents.m
- https://github.com/KhronosGroup/MoltenVK/blob/v1.4.2/MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm
- https://github.com/wine-mirror/wine/blob/wine-9.0/dlls/wined3d/context_vk.c
- https://docs.vulkan.org/refpages/latest/refpages/source/VkGraphicsPipelineCreateInfo.html
