# Runtime boundaries: device retest 2026-09-07

## Evidence

The four logs below identify revision `333acf63+dirty` (version 137). They are
observations from the prior device build, not acceptance of the changes here.

- `boxedvn-20260907-000105.log`: the 32-bit 3D workload reaches about 2940
  presents. There are only two swapchain creations and one resize to 800x600;
  the earlier repeated resize/rebuild storm is absent. Busy samples put one
  worker near 98% CPU in precise extF80 multiply/add and softfloat helpers, and
  another near 96% around Linux sched_yield and full FEX state reconstruction.
  Audio reports 57-61 underruns per five-second interval, with output below
  the 176400 guest bytes/second needed for stereo 44.1 kHz S16.
- `boxedvn-20260907-000659.log`: the desktop still has blank/overlapping child
  panes and cross-process WM_ERASEBKGND/WM_NCPAINT fixmes. No desktop rendering
  fix is claimed in this change.
- `boxedvn-20260907-000750.log`: the managed-runtime workload loads its base
  assembly, creates a DXMT device, and begins assembly reload. It later waits
  on the Wine wake pipe: fd8 read of 16 bytes, following request 29 (REQ_select).
  This is an object wait, not an unanswered ordinary server request. Handled
  unaligned faults do not establish the cause of the stall.
- `boxedvn-20260907-001107.log`: the legacy 32-bit graphics probe creates an
  Xlib surface, then surface-support dispatch returns `-4294967304`, the
  bridge's E_NOPROC sentinel. Wine subsequently reports no presentation queue.
  This is a missing resolved command, not evidence that the GPU cannot present.

## Shared corrections

Physical devices now retain their enumerating Vulkan instance. Physical-device
commands resolve through that owner instead of whichever adapter-probe instance
was most recently created. Enumeration handles partial results and count-only
queries; destroying an instance removes its physical-device mappings.

Relative touch movement keeps the virtual cursor at the screen center and sends
unbounded swipe deltas separately through XI2. It no longer accumulates into a
small box. Recentering never becomes an inverse raw delta. Absolute pointer mode
is preserved. The core-motion fallback emits center plus delta for each sample;
XI2-aware FPS applications are the primary target and still require device testing.

Queued 64-bit signals now wake registered host conditions. Blocking Unix reads
and futex waits check deliverability before sleeping and again when registering
the condition, closing a lost-wakeup race. Guest signal frames are built after
the syscall result is saved, on the receiving thread. This also gives FEX signal
delivery at syscall returns without waiting for ExecuteThread to finish.
SA_RESTART rewinds interrupted reads and untimed futex waits. Timed futex restart
with a preserved deadline remains outside this change. These are actual shared
runtime defects; the managed-runtime stall is not yet proven resolved by them.

Linux sched_yield in the ELF address lane avoids copying/reconstructing unchanged
SIMD/x87 state. Pending signals and PE syscall thunks use the full existing path.
Precise x87 remains enabled: its measured cost is still a major performance limit.
No 30 FPS outcome is promised from this optimization.

OSS GETOSPACE now reports partial-fragment free bytes within a fragment-rounded
capacity. Wine derives playback position from this field; whole-fragment rounding
made position advance in coarse jumps. The bound prevents unsigned underflow in
Wine's queued-frame calculation. CPU starvation can still produce underruns.

An attempted SDL streaming-resampler change was rejected during validation. The
pinned SDL 2.32.10 host resampler lost roughly 560 frames at stream closure for
some write sizes, and chunk-boundary continuity was not uniformly clean. The
existing converter is unchanged in this build.

## Validation and acceptance

- 147 guest Vulkan shim contract tests pass.
- Compiled production-method fixtures cover independent Vulkan instances,
  masked/ignored signals, read restart versus EINTR, signal arrival before and
  after condition registration, scalar-yield state preservation, and OSS partial
  fragment accounting without capacity underflow.
- Compiled input fixtures verify repeated large swipes keep the virtual pointer
  centered while preserving all raw movement, plus existing surface and warp cases.
- These host tests do not replace iPhone execution, actual guest signal-frame
  execution, camera feel, measured frame rate, or audible playback validation.

Retest with verbose off: both cubes; FPS swipes and audio during the same training
section; the managed-runtime startup for about 90 seconds; the legacy 32-bit
startup. Keep the existing Wine 11 runtime ZIP and container. No runtime archive,
CPU backend, or graphics backend was replaced. All changes are emulator-wide.

## Primary references

- Vulkan instance-proc-address ownership rules:
  https://docs.vulkan.org/refpages/latest/refpages/source/vkGetInstanceProcAddr.html
- Linux signal delivery at kernel return and SA_RESTART:
  https://man7.org/linux/man-pages/man7/signal.7.html
- Relative pointer behavior:
  https://wiki.libsdl.org/SDL2/SDL_SetRelativeMouseMode
- Wine 11 source: dlls/wineoss.drv/oss.c (oss_write_data),
  dlls/winex11.drv/mouse.c (map_raw_event_coords), and
  include/wine/server_protocol.h (select request and wake-up protocol).
- SDL 2.32.10 source: src/audio/SDL_audiocvt.c
  (SDL_AudioStreamFlush only drains padding when staging_buffer_filled is nonzero).
