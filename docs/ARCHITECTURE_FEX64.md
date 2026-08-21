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

## The executable pool is the scarce resource

Everything that runs here is a copy. iOS will not make an existing mapping
executable, so Wine's iOS side copies each PE image into a pool leased from
the StikDebug-prepared arena and executes the copy; FEX carves its translated
code buffers from the far end of that same pool. The pool is leased once,
before the debugger detaches, and can never grow.

Two things then follow, and both have cost a build:

- **Every guest process pays for its own copies.** A Windows "process" is a
  thread here, but it still gets its own PE views, so a guest whose launcher
  starts a second binary duplicates the whole set — including the ARM64EC
  emulator DLL, which is a little over 39 MiB by itself. Two device logs of a
  D3D11 title ended this way: 46 images copied, the pool full, the second
  process's emulator copy refused. Nothing recovers. The image stays
  non-executable, every entry faults `c0000005` at the same guest PC, and the
  redelivery storm walks the thread's 1 MiB stack off its bottom — so the
  session dies about ten seconds in reporting a stack fault that names none
  of the above. `[jit-pool] EXHAUSTED` is the line that does name it.
- **The head and the tail compete.** Image copies grow from the bottom, FEX's
  code buffers are reserved from the top, and the tail is capped at half the
  pool. A refused tail carve is survivable — FEX halves its request and the
  thread gets a smaller code buffer — but it is paid in recompilation, and
  `[jit-pool] tail CAP` in a log means frames are being spent on it.

`kBVNJitArenaSegmentBytes` is the number, because a lease is first fit inside
one segment and never spans two. It is sized for two concurrent guest
processes. A third would fail exactly as the second used to, and the fix that
scales past counting is **dedup of the duplicate emulator copies**: the pool
allocator's own comments record that `.text` is already byte-identical across
copies of a module, which is the precondition for sharing them. Upstream's
tuning constants were measured against a 896 MiB head and a 256 MiB tail, so
there is a long way to grow before this stack is generously fed.

The `[phys-map] bands` line in a session log is what says whether arena
growth is paid in address space or in resident memory; read it before
raising the segment again.

## 32-bit guests

The stack is x86-64 by construction, and older visual novels are i386. There
are two routes and they are not close to equivalent.

**Wine WoW64 with `libwow64fex.dll`** — built. Wine's WoW64 runs an i386 PE
side against a 64-bit unix side, and FEX already carries the emulator for it:
`Source/Windows/WOW64` builds `libwow64fex.dll`, the same BTCpu interface
`libarm64ecfex.dll` implements, for an aarch64 host and an x86 guest. The
pieces, and where each lives:

| Piece | Where |
|---|---|
| The emulator DLL | `scripts/build-fex64-wow64.sh` — same FEX tree and toolchain as the ARM64EC one, configured for plain `aarch64` so its CMake descends into `WOW64` instead of `ARM64EC`. Descending into one means the other's iOS-host symbols are missing, so the patch adds an `IosHostStubs.cpp` supplying them |
| The i386 Wine PE side | `--enable-archs=arm64ec,aarch64,i386` in `scripts/build-fex64-wine.sh`; gathered out of Wine's per-DLL layout by `scripts/collect-fex64-i386.sh` |
| Staging | `scripts/build-fex64-app.sh` puts the emulator in the ARM64EC set as `xtajit.dll` and the i386 set in its own directory |
| The prefix | the same script rewrites `Software\Microsoft\Wow64\x86` from `wow64cpu.dll` to `xtajit.dll`. The pinned template value is correct for an x86-64 host, where 32-bit code runs on the hardware; on ARM64 it has to name an emulator |
| The prefix at runtime | `BVNWineBridge` links the i386 set into `syswow64`, not `system32` — an i386 `ntdll.dll` and an ARM64EC `ntdll.dll` share a name and are not interchangeable |

Every one of those is additive: a build where the i386 arch or the emulator
DLL fails still ships, still runs x86-64 exactly as before, and reports which
half is missing when a 32-bit program is selected. That property is load
bearing and was got wrong once — the `i386-windows` resource is declared
optional in `ios/project.yml`, but XcodeGen's `optional` only stops it
refusing to *generate*; the resource still reaches the build phase and Xcode
fails the whole package on a path it cannot `lstat`. The directory is now
always staged, empty when there is nothing to put in it, and the app decides
whether a 32-bit side exists by looking for the i386 `ntdll.dll` inside it.

The address space is settled, and the answer is no — for now.

A 32-bit guest's pointers are 32 bits, so its images, heaps and stacks must all
live below 4 GiB, and a 64-bit Mach-O reserves exactly that range as
`__PAGEZERO` unless linked with `-pagezero_size`. Build 32445525083 shipped an
IPA linked with `-Wl,-pagezero_size,0x4000` — one 16 KiB page — and **the app
did not launch on device at all**: no crash inside the app, no session log,
nothing to read. iOS will not load this binary with a page-zero that small. The
flag is gone from `ios/project.yml`; `scripts/validate-app.sh` now prints the
reservation on every build so the state is never in doubt again.

Everything else here still builds and is still in the IPA. It reports itself
unreachable at startup (`low address space:` in the session log) and refuses a
32-bit executable with a message naming which half is missing.

**Before trying a different page-zero, know which sizes are worth trying.**
The obvious next move — keep a large `__PAGEZERO` and settle for the range
above it — does not work, and the reason is the catalogue rather than the
kernel. Old 32-bit Windows executables are based at `0x400000`, 4 MiB, and many
of them have their relocations stripped, so they can be loaded at that address
or not at all. A page-zero of 1 GiB would leave 3 GiB free — more than a 32-bit
Windows process can address anyway — and still fail to load the exact
executables this is for. The usable window is therefore a page-zero *below*
4 MiB: `0x100000` is the next experiment, not `0x40000000`. Whether iOS objects
to a small page-zero as such, or to one below some threshold, is what that
would establish, and it is a coin flip worth one build.

**The `ios` branch's Boxedwine backend.** It already runs 32-bit visual
novels, today, with no address-space question at all, because the guest's
memory is emulated rather than mapped. Making one app serve both would mean
carrying two runtimes and selecting between them on the executable's machine
type. That is integration work rather than research, and it is the route that
cannot fail for a reason outside this project's control.

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
| **B1** | Wine's macOS and ARM64EC trees configure and build in CI. | **Done.** Run 31973620692 produced the generated headers. The finalized native and ARM64EC runtime sets come from the pinned iOS integration at the exact same Wine commit; the combined build is validated before they are packaged. |
| **B2** | Wine's unix side, wineserver and the iOS display driver build for `iphoneos`. | **Native core done.** Run 31976822078 built 24 ntdll objects and 47 wineserver objects. The display driver remains part of the graphics ladder. |
| **B3** | Wine starts in one process; `wineboot` creates a prefix. | **Native bootstrap device-proven; x64 acceptance next.** A physical-device log on 2026-08-16 shows the embedded server and native ntdll loading the pre-seeded prefix, running the ARM64 acceptance PE, and propagating its expected exit code 42. The next package selects the ARM64EC runtime and launches a deterministic x64 PE through FEX. |
| **B4** | A console x86-64 PE runs to completion under FEX. | Its output in the session log. |
| **B5** | Display and input: a GDI program draws and responds to touch. | On device. |
| **B6** | DXMT builds, initialises and presents. | A D3D11 test program on screen. |
| **B7** | A real 3D title reaches a frame, then a soak. | Frame rate, and memory below the jetsam ceiling. |

B1 is the gate. Everything from B2 onwards includes generated headers and a
`config.h` from those trees, so nothing downstream can be attempted until a
Wine build exists.

## What this branch is not

- It does **not** replace the `ios` branch. 32-bit visual novels stay there;
  this stack is x86-64 and would regress them to nothing. See "32-bit guests"
  above for what changing that would take, and for the one measurement that
  decides whether the cheaper of the two routes is available at all.
- It has **no working runtime yet**. At this commit it is pinned forks, a
  toolchain build and CI. Nothing past A0 has run.
- Its performance on BoxedVN is **unmeasured**. The 142 FPS figure is Mythic's,
  on its own build, on an A15.
