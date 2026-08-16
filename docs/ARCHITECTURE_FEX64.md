# The `fex64` branch — ARM64EC Wine, FEX and DXMT on iOS

This branch replaces the emulator. It is not an optimisation of the `ios`
branch and shares no runtime with it.

The `ios` branch runs Boxedwine: an emulated Linux kernel, an emulated 32-bit
x86 CPU, and a real 32-bit Wine inside that emulated Linux. Every syscall,
page fault and draw call is emulated in-process.

This branch runs the stack that ARM64 desktop Wine uses, ported to iOS:

```
  Windows program (x86-64 PE)
        |
        |  x86-64 -> ARM64, entered through xtajit64.dll (ARM64EC "Path B")
        v
  FEX  .......................... CPU emulation ONLY
        |
        |  every syscall and Win32 call leaves emulation here
        v
  Wine 11.4, ARM64EC             PE side ARM64EC, unix side native arm64 iOS
        |                        wineserver runs as a THREAD, not a process
        v
  DXMT  ......................... D3D11 / D3D10 -> Metal
        |
        v
  Metal / CAMetalLayer
```

**FEX emulates instructions and nothing else.** There is no emulated kernel.
When the guest enters ntdll, execution leaves the JIT and continues as native
ARM64 code compiled for iOS. That is why this stack can be fast enough for 3D,
and why almost none of Boxedwine survives here.

## Why this branch exists

Two on-device measurements from the `ios` branch decided it.

1. **Graphics was never the limit for the CPU-bound titles.** The build-130
   trace reports the GPU seeing 5.4 GB available and 12 MB in use while the
   guest is presented. A faster D3D translation layer cannot move a frame rate
   no D3D call is waiting on.
2. **Boxedwine's ceiling is its CPU emulation.** The same trace shows a
   browser-engine guest growing to 16,384 interpreted anonymous code blocks,
   because runtime-generated x86 cannot safely enter the ARM64 translator.

And the `ios` branch is 32-bit by construction, while the 3D catalogue and
DXMT's own deployment layout are both x86-64.

## The reference implementation

[willfaust/mythic](https://github.com/willfaust/mythic) is this stack, working,
on a stock non-jailbroken iPhone 13 Pro (A15, iOS 27) sideloaded with a free
Apple ID. Its own status document, dated 2026-08-05, records:

- **A commercial x86-64 3D title is playable with audio at 142 FPS typical.**
- Steam's CEF login window renders and is interactive. CEF is Chromium 126.
- The whole thing is one Mach task.

That is a working existence proof, not a projection, and it is why this branch
is worth opening. It also means the engineering problem here is **integration,
not invention**: the port exists, and BoxedVN's job is to build it and put its
own application around it.

Mythic's own history is squashed to a single commit, so it cannot be read as a
sequence of decisions; the recipes in its `build/` scripts and the constraints
in its handoff document are the durable part.

### Licensing

| Component | Licence | Usable here |
|---|---|---|
| `willfaust/FEX` (fork of FEX-Emu) | MIT | yes |
| `willfaust/wine` (fork of Wine) | LGPL-2.1-or-later | yes |
| `willfaust/dxmt` (fork of DXMT) | LGPL-2.1-or-later | yes |
| The `mythic` repository itself | MIT, granted by its author 2026-08-16 | yes |

All four combine with GPLv2, so Mythic's application layer — the SwiftUI
shell, `JITAllocator.c`, `FEXBridge.mm`, the wineserver and process bridges,
the iOS Wine unix sources under `build/` — can be ported rather than
re-derived. That is the difference between weeks and months on this branch.

**The grant arrived by email and the repository still carries no `LICENSE`
file.** Ask its author to commit one. An email is a real grant but it lives in
one inbox; a file in the tree is what anyone auditing a GPLv2 project later
can actually check, and this port is public. Until that file exists, every
file ported here keeps a header naming its origin and the grant date, and
THIRD_PARTY_NOTICES.md records both.

## What BoxedVN brings

| Asset | Where | Why it matters |
|---|---|---|
| Executable-memory arena | `ios/runtime/src/BVNExecMemory.cpp` | Device-proven, and already the exact design this stack requires: StikDebug's `brk #0xf00d` universal request, per-16 KiB-page preparation, and a writable alias written through while the executable address stays `r-x`. Mythic reserves one dual-mapped RX/RW pool up front for the same reason. |
| StikDebug handshake and its failure modes | `BVNJIT.mm`, runtime status UI | Including the reinstall trap where a newly signed identity silently loses its `universal.js` assignment. |
| App shell | `ios/app` | Import, per-game manifests, containers, settings, log capture and export. |
| Unsigned-IPA packaging and CI | `scripts/package-ipa.sh`, `prepare-entitled-app.sh`, `.github/workflows/` | Mythic has no CI at all; it is built by hand on a Mac. This is the one place BoxedVN is ahead. |

The arena is the largest asset. Getting executable memory on a jailed iPhone
is the problem the public write-ups of this stack spend most of their length
on, and on the `ios` branch it is already solved, shipped and understood.

## The constraints that shape everything

From Mythic's handoff document, all of them log- or binary-verified there.
None is negotiable, and each one has killed a design.

- **One Mach task.** Every Windows "process" — the game, `services.exe`,
  `rpcss.exe`, a browser helper — is a *pseudo-process*: a thread. There is no
  process isolation, so **any unhandled fault anywhere kills the whole app**.
  Fault containment is a permanent tax, not a phase.
- **JIT memory is reserved once.** After the debugger detaches, no new
  executable mapping can be created. The pool must be sized up front.
- **Usable virtual address space is ~64 GB, not 512 GB.** iOS reserves a GPU
  carveout at `[64G, 448G)`. Mythic records this as tested extensively and
  undefeatable. It is why a browser engine has to be squeezed into the same
  address space rather than given its own process.
- **The jetsam ceiling is 4096 MB**, with real runs peaking around 3.0–3.4 GB.
  The increased-memory entitlement is mandatory, and headroom is thin.
- **Free-Apple-ID sideloading only**: 7-day provisioning, three apps, no paid
  entitlements, JIT via StikDebug.

## How it is actually built

The recipes, in dependency order. This is what the CI has to reproduce.

1. **llvm-mingw** (prebuilt, aarch64-w64-mingw32) — builds DXMT's PE side.
2. **LLVM 15.0.7 cross-built for iphoneos** — DXMT's `airconv` translates DXBC
   to Metal AIR through it. Two stages: `llvm-tblgen` for the host, then the
   iOS libraries reusing it. Needs LLVM's `AddLLVM.cmake` taught that iOS is
   Darwin-like, or it hands Apple's linker `--gc-sections` instead of
   `-dead_strip`. This is the longest single item in the build.
3. **FreeType**, static, for `win32u`'s unix side.
4. **GnuTLS stack** (GMP → nettle/hogweed → GnuTLS), static, for `bcrypt`,
   `secur32` and `crypt32`. Only needed once something wants TLS.
5. **Wine, built on macOS first** — both a macOS tree and an ARM64EC tree. This
   is not wasted work: the generated `config.h` and widl-generated headers from
   those trees are what the iOS compile includes.
6. **Wine's unix side, compiled for iphoneos by hand** — not by Wine's build
   system. Each DLL's unix half is compiled with `-DWINE_IOS=1` against the
   macOS tree's `config.h`, with its `__wine_unix_call_funcs` table renamed per
   library so the tables do not collide when everything links into one binary,
   and registered by name at runtime. Where Wine would `dlopen` a library, iOS
   links it statically and resolves through a generated symbol table.
7. **wineserver**, likewise, as a thread.
8. **DXMT** — PE side via meson and llvm-mingw against the Wine build tree;
   unix side compiled for iphoneos and combined with the LLVM iOS static libs
   into a single archive the app links.
9. **FEX** for iphoneos, reached through `xtajit64`.
10. **The app**, then an unsigned IPA.

## Staged plan

There are two ladders, and they diverge early. **Testing JIT does not require
Wine, DXMT, or any of the long toolchain work.** Keeping them apart is what
stops the first device answer from being three weeks away.

### Ladder A — a testable JIT

Nothing here needs LLVM, llvm-mingw, FreeType, GnuTLS or a Wine tree.

| Stage | Goal | Answered by |
|---|---|---|
| **A0** | Pinned forks resolve and check out. | CI `pins` job. **Done.** |
| **A1** | FEX builds for `iphoneos` as a static library. | A green CI job. No device. |
| **A2** | An app target links FEX and the arena, and reports the handshake. | Installs and shows a runtime status page. |
| **A3** | FEX translates and executes an x86-64 block **from the arena**. | The known return value in the log, `BVNExecMemExecutionConfirmed()` true, arena used-bytes rising. |

A3 is the real JIT answer: not "can this process make an executable page" —
the `ios` branch already proved that — but "does a translator's generated code
run from a debugger-prepared page on this device".

### Ladder B — games

| Stage | Goal | Answered by |
|---|---|---|
| **B0** | llvm-mingw, FreeType, GnuTLS and LLVM-for-iphoneos build and cache. | CI `toolchains` job. Written; not yet run. |
| **B1** | Wine's macOS and ARM64EC trees configure and build in CI. | **Recipe not yet established.** The configure lines are in nobody's repository; they have to be derived. Biggest unknown on the branch. |
| **B2** | Wine's unix side, wineserver and the iOS display driver build for `iphoneos`. | Artefacts in CI. |
| **B3** | Wine starts in one process; `wineboot` creates a prefix. | Prefix on disk, registry written, clean exit. |
| **B4** | A console x86-64 PE runs to completion under FEX. | Its output in the session log. |
| **B5** | Display and input: a GDI program draws and responds to touch. | On device. |
| **B6** | DXMT builds, initialises and presents. | A D3D11 test program on screen. |
| **B7** | A real 3D title reaches a frame, then a soak. | Frame rate, and memory below the jetsam ceiling. |

B1 is the gate. Everything from B2 onwards includes generated headers and a
`config.h` from those trees, so nothing downstream can be attempted until a
Wine build exists.

## What this branch is not

- It does **not** replace the `ios` branch. 32-bit visual novels stay there;
  this stack is x86-64 and would regress them to nothing.
- It has **no working runtime yet**. At this commit it is pinned forks, a
  toolchain build and CI. Nothing past A0 has run.
- Its performance on BoxedVN is **unmeasured**. The 142 FPS figure is Mythic's,
  on its own build, on an A15.
