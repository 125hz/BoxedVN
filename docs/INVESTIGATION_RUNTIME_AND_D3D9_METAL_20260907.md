# Runtime and D3D9 Metal development

Device baseline: `305d0200+dirty`, app 137, Wine 11.0. Logs are local device evidence,
not executable instructions. The BoxedWine kernel and FEX CPU remain in use.

## Findings and this build

* `181813`: the 32-bit 3D workload still has a worker at about 97% CPU in
  full-precision x87 addition. Another worker repeatedly samples inside
  `mach_msg2_trap` while polling thread resource usage. Cache the cumulative
  Mach CPU-time result per guest thread for 1 ms. Keep the voluntary scheduling
  counter fresh on every call. This removes repeated kernel RPCs; it does not
  make x87 arithmetic faster. Reduced x87 precision remains disabled.
* Audio output in that run supplies roughly 148 KB/s while 44.1 kHz stereo
  PCM16 needs 176.4 KB/s. The 32-bit 2D workload supplies roughly 190 KB/s while
  float32 stereo needs 352.8 KB/s. These are measured starvation, not a muted
  device. The timing cache may help scheduling, but uninterrupted playback is
  not established. Separately fix OSS GETOPTR: return consumed bytes, fragment
  transitions since the last query, and the circular DMA position. Wine 11's
  normal OSS streaming path uses GETOSPACE, so GETOPTR is not claimed as the
  fix for its stutter.
* `182822`: `SESSION_CHILD_CONTINUES` proves the launcher lifetime fix worked.
  Child 49 started while its ancestor owned FEX, continued on CPU64, and failed
  on `c2 04 00` at `0x7bf647bf` after entering WoW64. The fallback interpreter
  is not a complete compatibility-mode CPU. Do not pretend an isolated RET
  implementation would fix this. Report the child's nonzero exit after a
  successful launcher exits. Safe live address-space promotion or a complete
  fallback compatibility CPU remains necessary for this launcher chain.
* `182225`: over five million handled alignment faults precede a contained
  null access. Repeated opcode `f8ea806a` is an unaligned SWPAL in generated
  code. The previous helper fault fix allows loading to advance, but every
  such access still incurs a signal. The eventual fault is outside the current
  thread's registered code buffer. Do not skip it, make page zero accessible,
  or resume arbitrary pool code. Loading and the later freeze remain unresolved.
* `182916`: audio starves and the log ends during repeated pointer interaction,
  with no recorded terminal guest exception. There is not enough evidence to
  claim the late freeze repaired.

## Input and controls

Raw XInput2 motion must supplement core MotionNotify, not consume it whenever
any client selects raw events. Relative swipes retain their full deltas at
screen edges. The overlay draws its relative cursor at the center while the
guest can still query and warp the actual pointer. Device camera acceptance
is required; the change does not manufacture repeated inverse raw events.

The live control bar cycles fit aspect, fill aspect (crop), and stretch.
The landscape fullscreen menu exposes an initial on-screen control editor:
keyboard and mouse buttons, hold/release edges, normalized drag positions,
44–140 point sizes, deletion, and persisted shared control opacity. Inputs
release when leaving fullscreen or resigning active. The editor is UIKit in
the existing overlay and does not present a modal controller while Wine runs.

## D3D9 decision

Use the **direct D3D9-to-Metal implementation** as the port foundation, rather
than the archived DXUP D3D9-to-D3D11 wrapper. Research found a DXMT derivative
with a D3D9 frontend and DXSO shader compiler:

* https://github.com/dacevedo12/dxmt/releases/tag/v0.4-d3d9
* https://github.com/dacevedo12/dxmt/tree/e8dd4c656dcb74a6d970a30a397d1558b0e3fb2b/src/d3d9
* https://github.com/misyltoad/dxup (archived November 2019)
* https://github.com/3Shain/dxmt (main project remains D3D10/11)

The candidate source is pinned separately in `dependencies.d3d9-metal.lock.json`.
`prepare-d3d9-metal-port.py` fetches/validates the exact revision and audits the
shader-call ABI against our pinned iOS source. The first audit finds 21 D3D9
translation units and five new DXSO calls in slots 145–149. These do not exist
in our shipping native bridge. The candidate also needs a matching DXSO
compiler, iOS native integration, guest-pointer conversion, and i386 WoW64
parameter-layout validation. Existing slot equality does not prove structure
layout compatibility.

Preparation example (no guest programs execute):

```text
python scripts/prepare-d3d9-metal-port.py --fetch --source build/d3d9-metal-source --ios-source build/investigation-dxmt-ios --report build/d3d9-metal-port/abi-report.json
```

This is source preparation and ABI audit, **not a working D3D9 DXMT runtime**.
It is deliberately not selectable or installed into the container yet. The
current build retains DXVK → Vulkan bridge → MoltenVK for D3D9. Enabling the
new path requires matching 32/64-bit PE modules and native libraries, pointer
marshalling, and device cube/reset/shader/resource tests. No Wine ZIP change
is needed for the runtime/control fixes above.

## Verification boundary

Host fixtures exercise Mach-cache refresh/failure, fresh scheduling counters,
child-exit reporting, OSS playback positions including wraparound/reset, raw
event encoding, and native pointer/warp ordering. CI compiles the iOS overlay.
These checks do not prove camera behavior, stable frame rate, successful
launcher handoff to FEX, or uninterrupted device audio.
