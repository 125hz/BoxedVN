# Wine 11 stream continuity and command recording

## Device evidence

The September 7 02:03–02:23 logs identify revision 277421cd with the device
build marker `dirty`. They show the following independent boundaries:

- A 32-bit D3D9 workload reaches gameplay through DXVK and MoltenVK. Samples
  spend substantial CPU time in extended-precision arithmetic and syscall
  transitions. This path does not use DXMT, which serves D3D10/11.
- A 32-bit audio workload converts 44.1 kHz floating-point stereo to the
  device's 48 kHz stream. Stateless conversion of each OSS packet discards
  resampler history; queue underruns also remain visible in the device log.
- Another D3D9 application destroys a graphics-probe surface, then maps a
  277-by-76 resolution-warning dialog. The native presentation view was not
  removed with the surface, obscuring the subsequent GDI dialog.
- A 64-bit managed application presents its loading title. Earlier recovered
  unaligned atomic faults cease before the later stall; one worker continues
  spending substantial CPU time in the Unix read/cancellation path. The exact
  repeated syscall and result need further device evidence.
- The separate 022325 desktop log successfully enumerates files on D: while
  Wine File Manager and the shell repeatedly report unsupported cross-process
  WM_ERASEBKGND/WM_NCPAINT packing. Their blank panes and taskbar remain
  unresolved. This is not evidence that the game files are absent.

## Changes

1. Keep one SDL audio stream per native OSS voice, preserving filter history
   across packets. Count retained input in OSS queue accounting and flush only
   at close. Include a final silent frame to drain SDL2's block-aligned staging
   buffer. Queue errors propagate instead of silently accepting lost audio.
2. Match the existing IA-32 Vulkan surface-destruction path by removing the
   native presentation record/view after destroying the host surface.
3. Return the configured CPU count from x86-64 sched_getaffinity, with Linux
   mask sizing. The former hardcoded one-core result contradicted the reported
   processor topology and could constrain worker pools.
4. Dispatch marshalled vkCmd recording operations directly from the FEX frame
   without copying all SIMD/x87 state into and out of the interpreter CPU.
   Only the Unix guest lane qualifies. Queue operations, allocation, waits,
   presentation and deliverable signals keep the full syscall path. Replace
   the linear Vulkan ordinal search with a switch.
5. Report the top three high-rate Linux syscalls per thread at bounded
   intervals, including arguments and return values. This targets the loading
   stall without enabling full Wine relay logging.

No executable names, game-specific configuration, guest binaries or Wine
runtime pins are changed. Fast x87 remains disabled. The current Wine ZIP and
existing containers remain applicable; delivery requires only the new IPA.

## Validation and limits

- Real pinned SDL 2.32.10 signal tests cover continuous DC and sine conversion
  at three packet sizes, duration, discontinuities and retained input.
  Thirty-second streams at 22.05, 44.1 and 96 kHz check bounded queue accounting.
- Compiled production-function fixtures cover affinity mask boundaries,
  Vulkan surface teardown order, FEX frame updates and fast-path exclusions.
- Existing surface/input fixtures and 147 Vulkan shim tests pass locally.
- Eight existing host CTest tests passed during this investigation.

CI compilation/package success is separate from physical-device acceptance.
The audio changes do not prove all starvation is resolved; the rendering
changes do not establish a minimum frame rate. The managed loading stall and
desktop paint failure still require follow-up evidence.

References: [SDL stream flush semantics](https://wiki.libsdl.org/SDL2/SDL_AudioStreamFlush),
[DXMT supported Direct3D versions](https://github.com/3Shain/dxmt).
