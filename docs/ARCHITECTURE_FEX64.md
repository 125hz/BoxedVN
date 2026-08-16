# The `fex64` branch — native ARM64 Wine, FEX and DXMT on iOS

This branch replaces the emulator. It is not an optimisation of the `ios`
branch and it does not share a runtime with it.

The `ios` branch runs Boxedwine: an emulated Linux kernel, an emulated 32-bit
x86 CPU, and a real 32-bit Wine living inside that emulated Linux. Everything
the guest does — a syscall, a page fault, a Vulkan call — is emulated by
Boxedwine in-process.

This branch runs the stack that current ARM64 desktop Wine uses:

```
  Windows program (x86-64 PE)
        |
        |  executed by
        v
  FEX  ..................... CPU emulation only: x86-64 -> ARM64
        |
        |  every syscall / Win32 call leaves emulation here
        v
  Wine, built native ARM64 for Darwin
        |  ntdll.so, win32u.so, wineserver: native arm64 code, no emulation
        v
  DXMT  ................... D3D11/D3D10 -> Metal
        |
        v
  Metal / CAMetalLayer on iOS
```

The essential difference: **FEX emulates instructions and nothing else.**
There is no emulated kernel. When the guest calls into ntdll, execution
leaves the JIT and continues as native ARM64 code compiled for iOS. That is
why this stack can be fast enough for 3D games while Boxedwine is not, and it
is also why almost none of Boxedwine's source survives here.

## Why this branch exists

Two measurements from the `ios` branch, both on-device, decided it.

1. **The graphics stack was never the limit for the CPU-bound titles.** The
   build-130 trace reports the GPU seeing 5.4 GB available and 12 MB in use
   while the guest is presented. A faster D3D translation layer cannot move a
   frame rate that no D3D call is waiting on.

2. **Boxedwine's ceiling is its CPU emulation, not its backend.** The same
   trace shows a browser-engine guest growing to 16,384 interpreted anonymous
   code blocks, because runtime-generated x86 cannot safely enter the ARM64
   translator. Work on that path buys a constant factor on 2D titles. It does
   not reach 3D.

Separately, the `ios` branch is **32-bit only** by construction, and the
interesting 3D catalogue is x86-64. DXMT's own installation layout is
`x86_64-unix` / `x86_64-windows`; there is no 32-bit deployment of it. So
"run 3D games" and "stay on Boxedwine" are not compatible goals, and this
branch takes the other one.

## Prior art this is modelled on

The stack is proven on desktop: CrossOver's ARM64 Wine uses FEX for x86
emulation, and DXMT is a shipping Metal backend for D3D11/10 under Wine on
Apple Silicon.

On iOS the only public report of it working is a write-up by niixolabs
(`zenn.dev/niixolabs/articles/ios-x86-jit-native-execution`), which combines
FEX, Wine's unix libraries and DXMT in a single iOS process and gets **a
rotating D3D11 test cube to render**. Its author is explicit that this is a
minimal test program and that real games — many DLLs, audio, large working
sets — are untested. Any performance number quoted for this stack on iOS
should be treated as unmeasured until we measure it ourselves.

That report is nonetheless the most valuable document available for this
branch, because it enumerates the iOS-specific failures in the order they are
hit. They are reproduced below as work items.

## What carries over from the `ios` branch

More than is obvious, and this is the reason the pivot is affordable at all.

| Asset | Where | Why it survives |
|---|---|---|
| Executable-memory arena | `ios/runtime/src/BVNExecMemory.cpp` | Device-proven. Already the exact TXM-aware design the reference port describes: StikDebug's `brk #0xf00d` universal request, per-16 KiB-page preparation, and a separate `rw-` alias written through while the executable address stays `r-x`. FEX needs precisely this and nothing else. |
| StikDebug handshake and its failure modes | `BVNJIT.mm`, runtime status UI | Including the reinstall trap where a newly signed identity loses its `universal.js` assignment. |
| App shell | `ios/app` | Import, per-game manifests, containers, settings, log capture and export. |
| Packaging | `scripts/package-ipa.sh`, `prepare-entitled-app.sh`, `.github/workflows/build-ios.yml` | Unsigned IPA, entitlements, macOS CI runner. |
| Presentation and input plumbing | `ios/runtime/src/BVNGuestOverlay.mm` and the iOS side of the SDL/Metal work | The layer/scale/orientation and pointer-geometry lessons transfer even though the producer above them changes completely. |

The arena is the single biggest asset. It is the problem the reference port
spends most of its write-up on, and on this branch it is already solved and
shipped.

## What does not carry over

Boxedwine's CPU cores, its emulated Linux kernel (`source/kernel`), its soft
MMU, its X11 implementation, its OpenGL and Vulkan bridges, the MoltenVK and
DXVK path, and the 32-bit Wine root filesystem. None of them have a role in
this stack. They stay in the tree while both branches are alive, but nothing
new should be built on them here.

## The hard problems, in the order they bite

Each item states what it is, what the reference port did about it, and the
falsifier — the one observation that proves it is actually solved. Nothing on
this branch should be called done without its falsifier.

### 1. Wine must run in one process, with no `fork` and no `exec`

iOS permits neither. Wine's design assumes a `wineserver` process plus one
host process per Windows process, and its loader `exec`s.

This is the largest single piece of work on the branch and the one most
likely to fail. The reference port runs Wine's unix libraries inside one iOS
process, which means `wineserver` runs in-process — as a thread, reached
through the same socket protocol rather than a separate program. Multi-process
Windows programs (anything that spawns a helper, an installer, a browser
engine with a GPU process) are out of scope until that is answered.

*Falsifier:* `wineboot` completes and `winecfg` — or any single PE — starts,
runs and exits cleanly in one iOS process, with the registry written back.

### 2. FEX's code buffers must come from the StikDebug arena

FEX allocates its own executable memory. On iOS every executable page must
have been prepared by the debugger before it will execute; ordinary
`mmap(PROT_EXEC)` succeeds and yields a page that faults. FEX must be built
against `BVNExecMem*` instead of its own allocator, writing generated code
through the writable alias and calling through the executable address.

The reference port uses a 256 MB pool; the `ios` branch already prepares 512
MiB in eight segments.

*Falsifier:* FEX executes a translated block from an arena address, with
`BVNExecMemExecutionConfirmed()` true and the arena's used-byte count rising
as translation proceeds.

### 3. 16 KiB pages

iOS pages are 16 KiB. Wine's PE loader and section protections assume 4 KiB.
The reference port's answer is that FEX's own memory model absorbs this,
because the x86-64 guest's page granularity is FEX's business rather than the
host's — which is only true for guest mappings, not for the native ARM64 Wine
libraries themselves.

*Falsifier:* a PE with adjacent sections carrying different protections loads
and runs without a spurious access violation.

### 4. `x18` is reserved by Darwin

ARM64 Wine keeps the TEB in `x18`. iOS reserves that register for the
platform and clears it across context switches, so any Wine code that
dereferences it faults at an unpredictable point. The reference port
binary-patches Wine's `x18` accesses and notes that its first patch set missed
some load/store addressing forms, which produced exactly the kind of rare,
late crash that is expensive to chase.

Prefer a source change to Wine's TEB accessor over binary patching if the
build is ours anyway.

*Falsifier:* a long soak — minutes of a real program, many threads — with no
fault whose faulting address derives from a zero `x18`.

### 5. There is no `winemac.drv` on iOS

DXMT's documented requirement is CrossOver Wine 24+, or Wine 8+ *"with
specific APIs exposed from `winemac.drv`"*. `winemac.drv` is AppKit. iOS has
UIKit and no `NSView`, `NSWindow` or `NSOpenGLContext`.

So this branch needs its own Wine display driver: a `wineios.drv` providing
window, cursor and input, backed by UIKit, plus whatever surface handle DXMT
requires to attach a `CAMetalLayer`. The minimum viable version is a single
fullscreen window bound to one layer — which is also all the reference port's
cube needed.

*Falsifier:* DXMT presents to a layer that is actually on screen, and a
touch delivered by UIKit arrives at the Windows program as a mouse message in
the right coordinate space.

### 6. Native code must be signed, generated code must not be

Everything native — Wine's arm64 `.so` files, FEX, DXMT's unix half — ships
inside the signed app bundle. Nothing may be `dlopen`ed from Documents. Wine's
own loader must find its unix libraries inside the bundle, so its search paths
have to be redirected at build time rather than by environment variable.

The PE side (`d3d11.dll`, `dxgi.dll`, `winemetal.dll`, Wine's own builtins) is
data as far as iOS is concerned — it is executed by FEX, not by dyld — and can
live in the prefix.

*Falsifier:* a build installed by an ordinary sideload starts Wine with no
`dlopen` outside the bundle.

### 7. Shader compilation on device

DXMT converts DXBC to Metal's AIR through its own LLVM-based `airconv`. That
has to build and run on iOS arm64, inside the app. Runtime shader creation
from source is permitted on iOS; the open question is `airconv`'s own size and
build.

*Falsifier:* a non-trivial pixel shader from a real game compiles on device
and draws.

## Staged plan

Each stage is a shippable IPA and has one question to answer. Stages 0–2 do
not need a game.

| Stage | Goal | Answered by |
|---|---|---|
| **M0** | Pinned sources; FEX, Wine (arm64 Darwin) and DXMT each *build* for `iphoneos` in CI. | A green CI job producing the libraries. No device. |
| **M1** | FEX runs from the BVNExecMem arena and executes a hand-written x86-64 basic block. | Arena-backed execution confirmed on device. |
| **M2** | Wine's unix side starts in one process; `wineboot` creates a prefix. | Prefix on disk, registry written, clean exit. |
| **M3** | A console x86-64 PE runs to completion under FEX + Wine. | Its stdout in the session log. |
| **M4** | `wineios.drv` shows one fullscreen UIKit window and delivers input. | A GDI program draws and responds to touch. |
| **M5** | DXMT initialises and presents. Reproduce the reference port's cube. | A rotating cube on device. |
| **M6** | A real 3D title reaches a frame. | Frame rate and a stable soak. |

M0–M2 are where this branch either becomes real or does not. Problem 1
(single-process Wine) is the gate, and it is worth failing fast on it: if
`wineserver` cannot be made to run in-thread, everything after M2 is moot and
the effort is better spent on the `ios` branch's CPU path.

## What this branch is not

- It does **not** replace the `ios` branch. 32-bit visual novels stay there.
  FEX's own wiki records its WoW64 (32-bit) path as non-functional, so this
  stack starts x86-64-only, and the titles that work today would regress to
  nothing.
- It has **no working runtime yet**. At this commit it is pinned sources, a
  fetch script and this document. Nothing in the M-table has been reached.
- Its performance is **unmeasured**. The only public iOS data point for this
  stack is a test cube.
