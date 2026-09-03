# BoxedVN — known limitations

Read this before reporting a bug, and before assuming a feature exists. It is
kept deliberately blunt.

**Status as of 2026-08-08:** BoxedVN builds, links, packages, and renders Wine
Notepad on a physical iPhone. The StikDebug universal-script JIT path, Wine 11
root filesystem, native-scale 2D SDL framebuffer and basic touch input are
device-proven. Build 16 also proves the pre-SDL landscape handoff and that
`boxedmain()` can return once. Build 17 proved that SDL's field can become
first responder, and build 18 proved shared `UIWindowScene` ownership, but iOS
still did not create a keyboard scene and its open-in-place picker still did
not complete selections. Build 19 adds a UIKit-independent guest keyboard and
an import-as-copy picker. That keyboard is now device-proven through Notepad.
Saya no Uta reaches WineD3D graphics setup, proving imported PE32 execution and
the corrected working directory. Build 20 then stopped at guest call 2897,
which is `glXChooseVisual`; this is the expected boundary of a build with no GL
backend, not a JIT freeze. Build 21 then tried to force GDI and disable
`winebth`, but its device log proved both attempted registry values ineffective
under Wine 10. Build 22 uses the current `renderer=gdi` no-3D setting and
disconnects the root `winebth` device association that overrides `Start=4`.
Device logs prove those changes reach Song of Saya's launcher, but Start Game
then faults because its D3D9 engine receives no usable feature level. Starting
Wine's debugger also exhausted the 64 MiB JIT arena, so build 23 raises it to
128 MiB. Build 23 device logs prove WineDbg now fully attaches. Build 24 then
proves disconnecting winebus does not reduce the 57–61 second cold-start gap,
while a second guest in the same process starts in seconds. Build 25 restores
winebus and proves Boxedwine Vulkan, MoltenVK 1.4.2 and the Apple A19 GPU work
on-device, but DXVK 2.5.2 rejects MoltenVK's missing geometry-shader and
transform-feedback features. Build 26 repairs the persistent GDI/no-3D prefix
policy and uses WineD3D's Vulkan renderer instead. Build 27 device logs prove
that route creates WineD3D Vulkan devices and swapchains on the Apple A19 GPU,
but also expose SDL's missing Vulkan-surface view teardown as the cause of the
subsequent black screen. Build 28 proves native view teardown, but its device
log shows it restored the still-live 113x2 capability probe. Build 29 makes
that probe offscreen and is device-proven through Saya's visible launcher,
D3D11 setup, audio startup and 1280x960 swapchain creation. The game then
faults on a write to `0x03BE0000` before its first frame while its last Metal
surface remains live. Two build 30 runs at 800x600 and 1280x960 then reproduce
the same `0x03BD0000` write fault at `0x7F444DDF`, outside every tracked live
or recently unmapped Vulkan range. WineDbg itself faults before mapping, so
the title-based reveal cannot run. Build 31's TTY fallback successfully
restores X11 and leaves input/keyboard responsive. Its snapshot proves the
faulting `REP MOVSB` has `ECX=0xFFE1D4F0`, but also reports EIP `0x7F444DDF`
as `Unknown`; the `mvware` module-name selector consequently never activates.
Build 32 replaces that ineffective selector with a Saya-only
`0x7F300000–0x7F500000` interpreter range covering both observed mvware EIPs,
while Wine/WineD3D retain JIT. Two device runs prove that range activates and
passes the former engine fault: Saya initializes D3D11 and audio, creates and
destroys two setup swapchains, then leaves its final swapchain alive. The old
logs could not tell whether the guest ever called `vkQueuePresentKHR`, so the
opaque Metal view looked like a regression even though execution progressed.
Build 33 tracks swapchain-to-surface identity, records the first queue submit
and present result, and keeps a native waiting-for-first-frame indicator over
the Metal layer until presentation succeeds. Its device log proves a transient
surface submits and presents, then the final 800x600 surface stops after PNG
loading without presenting. Build 34's heartbeat proves the final surface is
waiting behind 25 million interpreted instructions at `0x7F444BDE`. Build 35
fixes the underlying optimized `REP MOVS` overlap loop—which incorrectly
tested an invariant address delta, underflowed `ECX`, and explains the build
31 fault—and removes every Saya interpreter range so the engine retains full
JIT. Build 35 device evidence confirms the old fault is gone but still stops
after the final surface and PNG load, before its first acquire. The active
root was Wine 10.0, while WineHQ lists Song of Saya's
`RtlpWaitForCriticalSection` hang as fixed in Wine 10.11. Build 36 changed the
default to the pinned Wine 11.0 root containing that fix. Its device log proves
Wine 11 is active but reaches the same final-surface boundary, ruling that Wine
bug out as the only cause. The last X11 messages were unsupported `GXand` and
`GXxor` image operations. Build 37 implements all 16 core X11 raster functions
with plane-mask semantics and adds automatic 12/30-second guest thread and
futex snapshots if the first frame remains absent. No main-menu claim is made
until build 37 is tested on-device.
Build 26 also proves that reinstalling can lose StikDebug's
script assignment while leaving `CS_DEBUGGED` set; build 27 guards the
otherwise-fatal handshake trap and reports that setup error in-app. Audio and
relaunch remain untested. See
`PROGRESS.md` for exact evidence.

---

## 1. Not supported, by design, in this version

| | |
|---|---|
| **x86-64 Windows programs** | Boxedwine runs a 32-bit Wine. PE32+ executables are detected and refused with a specific message. There is a reserved `FutureX64` backend identifier that implements nothing. |
| **OpenGL acceleration** | No GL backend is compiled in. See section 3. |
| **Direct3D 8–11** | Build 37 routes Wine 11 WineD3D through Vulkan/MoltenVK, contains Wine's upstream Saya critical-section fix, fixes Boxedwine's `REP MOVS` overlap JIT defect and missing X11 raster operations, and leaves the complete engine JIT-compiled. Final-swapchain creation is device-proven; a visible game frame is not yet. Rootfs DXVK is present but cannot satisfy its required feature set on current MoltenVK. |
| **VKD3D, D3DMetal, DirectX 12** | Not present and not planned for this milestone. |
| **The iOS Simulator** | Boxedwine's ARM64 JIT cannot run there. The simulator preset is a compile check only. |
| **Background execution** | The guest does not run while the app is backgrounded. |
| **Multiple simultaneous sessions** | One guest at a time, enforced in the runtime. A second launch request is refused with an explanation. |
| **Steam and other commercial launchers** | Out of scope. |
| **DRM circumvention, anti-cheat, online activation bypass** | Out of scope and will not be added. |
| **App Store distribution** | Sideloading only. |
| **Frame generation, FSR, "performance" toggles** | Not present. Any such control would be decoration. |

---

## 2. JIT is required and BoxedVN cannot enable it

**Build 98 takes the executable arena at app startup rather than at guest
launch.** StikDebug's script session ends on its own, and the arena request
used to be the first thing a *launch* did - minutes later, after the user had
browsed their library - by which time it often could not be serviced, and the
only cure was to restart the app through StikDebug and launch a game
immediately. Preparation is the half with the deadline and it executes
nothing, so it is done at startup, off the main thread with the same six
second timeout; the execution test, which is the half that can crash, stays
at guest launch. A refusal is not remembered, so attaching StikDebug after
opening the app and then launching still works. The session log says which
happened under `jit:`.

A sideloaded iOS app cannot prepare executable memory by itself. On iOS 26/27,
`CS_DEBUGGED` is necessary but no longer sufficient: the target stops once at
StikDebug's universal JIT breakpoint and StikDebug/debugserver prepares every
16 KiB page of a bounded arena through TXM. BoxedVN then maps those
physical pages twice—an `r-x` address used for execution and an `rw-` alias
used for code generation—and suballocates live code locally. This is a
**different mechanism from macOS's `MAP_JIT`**; BoxedVN never requests
`MAP_JIT` on iOS (see `docs/ARCHITECTURE_IOS.md` §4).

- **Signing the IPA does not enable JIT.** In current StikDebug, enable
  Advanced Options, long-press BoxedVN, assign `universal.js`, launch BoxedVN
  through StikDebug, and leave the script active.
- Without `CS_DEBUGGED`, BoxedVN refuses to issue the breakpoint request.
- With `CS_DEBUGGED` but no active compatible script, the breakpoint request
  can stall or fall through as SIGTRAP. Build 27 times out the former and
  catches only the expected handshake trap in the latter case, explaining the
  required StikDebug setup instead of freezing or terminating the app.
- **Reassign `universal.js` after every reinstall or re-sign.** A signer may
  create a new application identifier, and StikDebug's old target assignment
  does not follow it automatically.
- **Runtime status** reports `CS_DEBUGGED` safely. It cannot prove page
  preparation without issuing the breakpoint, so the full RX/RW mapping and
  execution test is intentionally deferred until a guest launch.
- The executable arena is deliberately bounded, and the bound is decided
  before the guest starts. Asking StikDebug for another region once the guest
  is running would stop it at a live debugger breakpoint that a
  background-suspended StikDebug would never service, so the whole budget is
  prepared during the one startup handshake. `BVNPlanJitArena` sizes it at an
  eighth of the smaller of `os_proc_available_memory()` and device RAM, with a
  128 MiB floor and a 512 MiB ceiling, split into 64 MiB segments so that a
  kernel refusal part way through costs capacity rather than the whole JIT.
  A workload that still exceeds the prepared total fails with an explicit
  arena-exhaustion log naming the capacity, not with the "no JIT enabler
  attached" message - those are opposite problems and used to share wording.
- **Guests that run a browser engine are the demanding case.** Chromium,
  Electron and NW.js titles start six to ten guest processes, each translating
  its own copy of the engine. A fixed 128 MiB ran dry mid-launch for one of
  them.
- **There is no whole-runtime fallback when JIT is unavailable.** Boxedwine
  does contain a decoded interpreter, but Wine startup still requires the JIT
  arena. Build 32 can mark a named guest module or bounded guest address range
  interpreter-only after JIT is available; this is a narrow compatibility
  tool, not a way to launch BoxedVN without StikDebug.
- **Cold Wine startup still takes roughly 1.5 minutes.** Build 24 disproves the
  winebus hypothesis. Warm guest launches in the same process take seconds, so
  later work should preserve/prewarm that state rather than disable devices.

---

## 3. No OpenGL

Upstream's macOS ARM64 build gets software OpenGL from a prebuilt
`libOSMesa.8.dylib` backed by a 124 MB `libLLVM.dylib`. Both are macOS ARM64
binaries; neither exists for iOS. Cross-building Mesa and LLVM for iOS is a
substantial project, and llvmpipe would itself need JIT.

So this build defines no GL backend at all. Practically:

- **Programs that need OpenGL will fail.** `KNativeSystem::getOpenGL()` logs
  "Failed to load OpenGL, will probably crash".
- **The accelerated experiment is now Direct3D through Vulkan.** Build 26 sets
  Wine 10's `Software\\Wine\\Direct3D / renderer` to `vulkan` for imported
  games, including existing prefixes that still contain build 22's `gdi`
  value. WineD3D then routes D3D9 through guest winevulkan and Boxedwine's
  Vulkan bridge to MoltenVK/Metal. This is locally built but awaits a device
  game frame. It does not add an OpenGL implementation.

The repository contains an old `BOXEDWINE_ES`/`source/opengl/es` experiment.
A build 23 compile experiment proved it no longer matches 26R2's generated GL
marshaler or extension table, so it is not a usable backend. Build 25 compiles
Boxedwine's generated Vulkan bridge, statically links official Khronos iOS
MoltenVK 1.4.2 and maps guest Xlib surfaces to SDL UIKit's Metal surface.
Device logs prove that bridge reaches the Apple A19 GPU. They also prove the
rootfs DXVK 2.5.2 overlay cannot create its minimum device because it requires
geometry shaders and `VK_EXT_transform_feedback`, neither exposed by MoltenVK.
Build 26 therefore bypasses DXVK and supplies WineD3D's Vulkan path, including
fallback from promoted KHR instance-procedure names to their Vulkan core
names. Build 27 reaches two on-device swapchains, but SDL2's UIKit backend
pushes a new `SDL_uikitmetalview` for every `SDL_Vulkan_CreateSurface()` and
has no corresponding public surface-destroy API. Wine destroyed its temporary
113x2 and 794x568 probe surfaces while their dead Metal views stayed above the
live X11 launcher, leaving the user on black. Build 28 associates every host
`VkSurfaceKHR` with the exact UIKit view SDL installed, detaches it through
SDL's pinned `setSDLWindow:nil` bookkeeping path when Wine destroys the
surface, restores the preceding live surface, and recreates the X11 SDL
renderer when no Vulkan surfaces remain. Its device log proves teardown works,
but the preceding live surface was Wine's 113x2 capability probe, so restoring
it still produced black. Build 29 creates sub-320x200 probes against an
unattached `CAMetalLayer`, never enters them in the UIKit presentation/input
stack, and restores X11 when no display-sized Vulkan surface remains even if a
helper is still live. The next device run reached a real 1280x960 game surface
but the game process faulted before presenting a frame, leaving that live view
black above WineDbg. Build 30 detects Wine's debugger/error X11 titles,
detaches the visible Metal layer without prematurely destroying its Vulkan
surface, and records whether the fault lands in live/recent Vulkan-mapped
memory or its guard pages. This improves failure visibility and isolates the
next compatibility change. Build 32 subsequently passes that fault under the
bounded interpreter profile and reaches a live final swapchain. Build 33 makes
the next boundary explicit: a native overlay remains visible until the first
successful queue present, and the log names the swapchain, surface, attempt
and result. A removed overlay with a still-black picture means the guest
successfully presented black; an overlay that remains means it never reached
presentation. OpenGL remains unsupported.

---

## 3a. Games built on a browser engine

An NW.js or Electron game - which is what RPG Maker MV and MZ deploy for
Windows - is a Chromium application wearing a game's name. That changes what
"compatible" means: the Direct3D layer underneath can be completely healthy
and the game still shows nothing, because the failure is in Chromium's process
model, its compositor or its font stack rather than in the renderer.

Build 96 recognises the engine from the files a game ships (`nw.dll`,
`nw.pak`, `package.nw`, `app.asar`) and passes Chromium's own switches for
the facilities in question: `--no-sandbox`, `--disable-gpu`,
`--disable-software-rasterizer`, `--disable-direct-composition`,
`--disable-features=CalculateNativeWinOcclusion`,
`--disable-background-timer-throttling`, `--disable-renderer-backgrounding`
and `--disable-backgrounding-occluded-windows`. It also passes
`--start-maximized`, `--kiosk` and `--start-fullscreen`, because NW.js package
defaults otherwise leave a decorated, desktop-sized game window centred
inside BoxedVN's virtual monitor. Fullscreen is the engine-owned path that
removes that frame; BoxedVN's native menu remains available outside the guest.
The engine and the evidence for it are logged under `engine:`.

Anything in a game's launch settings is *added to* these rather than
replacing them. A switch the user names wins over BoxedVN's copy of the same
switch, `--disable-features` lists are combined value by value because
Chromium keeps only the last one it sees, and
`--bvn-no-default-switches` - stripped before launch, never passed to the
guest - turns the whole set off. Build 96 stood down entirely as soon as any
switch appeared, which meant adding `--enable-logging=stderr` to debug a
guest silently removed the eight switches keeping it alive.

Three device runs of the same NW.js/RPG Maker MV title established the set:

| run | switches | result |
|---|---|---|
| 1 | none | black window; renderer aborts in `FontCache`; guest exits 1 |
| 2 | `--no-sandbox --in-process-gpu --disable-direct-composition --disable-features=CalculateNativeWinOcclusion` | no abort; the game's own 816x624 window appears; three composited presents, then a white page and no further frames |
| 3 | GPU pair swapped for `--disable-gpu --disable-software-rasterizer`, throttling switches added | PixiJS initialises in Canvas mode, audio opens, the render loop runs and draws the game's loading screen |

So: `--no-sandbox` is what stops the `FontCache` abort, and software
compositing is what actually produces frames - the hardware path in run 2
created a presentation swapchain and never presented to it. Run 3 changed
several switches at once, so the individual contributions are not isolated
and the set is shipped as a set.

**Where it stops, established rather than guessed.** Runs 3-6 reach RPG
Maker's `Scene_Boot` and stay there. Run 6 printed the three conditions that
gate it, every three seconds, from a probe added to the game's own
`index.html`:

```
BOOTCHK 41 img=true db=[object Object] font=false errUrl=null
```

Forty-one identical lines. Images are ready, the database is loaded (`&&`
yields the truthy object a plugin's wrapper returns), no resource failed to
load, and `Graphics.isFontLoaded('GameFont')` is false and stays false. The
guest is blocked on the font and on nothing else.

The mechanism is RPG Maker's, and it has no timeout. `Graphics._fontLoaded`
is assigned from `document.fonts.ready`; `isFontLoaded` returns false while
that field is null. A `document.fonts.ready` that never settles therefore
hangs `Scene_Boot` silently and permanently.

These runs also refuted two whole classes of explanation. Waiting does not
help: run 5 was left six and a half minutes, and its last JIT arena
allocation is four minutes before the session ended, so no new guest code was
translated in that window. Cross-process IPC is not involved: run 6 added
`--single-process` and the result was identical. `--no-proxy-server
--disable-background-networking` also changed nothing, and the DNS retries
continue regardless.

Run 7 added `--disable-remote-fonts` and changed nothing: forty-one more
`font=false` samples. That removes the game's `@font-face` from the picture
entirely, so what is stuck is not the web font load - it is
`document.fonts.ready` itself never settling.

That narrows it usefully. In Blink, `FontFaceSet.ready` is resolved from the
document lifecycle, not from the font loads alone; a renderer that never
completes a layout never resolves it. Timers are demonstrably alive in this
state - the probe's own `setInterval` fires forty-one times - so "the
renderer is dead" is not the explanation either. The open question is whether
the document lifecycle is running at all, which the next probe answers
directly by logging `document.fonts.status` and whether `ready` ever
resolves, and by bypassing `Graphics.isFontLoaded` to see whether anything
downstream of the gate renders.

Build 99 stops this being unanswerable from the host. `censusGuestFonts`
counts the font files a guest can actually see - the user's own in the
prefix, the root filesystem's `drive_c/windows/Fonts`, and the faces Wine
ships in `share/wine/fonts` that `wine.inf` installs into a new prefix - by
reading the archive's central directory, and every launch logs the totals
under `fonts:`. A total of zero is logged as an error in its own right,
because a guest with no font cannot run a program that draws text and
nothing else in the log would say so.

**Solved, and device-proven.** RPG Maker will not leave `Scene_Boot` until
`Graphics.isFontLoaded("GameFont")` is true, and on the CSS font-loading path
it waits for that without any timeout. Under Wine the engine's own trigger -
a hidden element styled `font-size: 0px` - never causes Blink to load the
face, so the wait never ends and nothing anywhere reports an error.

Asking Blink for the face directly is the entire repair:

```
BOXEDVN fontfix requested GameFont, Blink returned 1 face(s)
BOXEDVN fontfix not needed; GameFont loaded on its own
BOXEDVN boot 1 ... faces=GameFont:loaded|azuki:error|kaku:error gameFont=true scene=Scene_Map
```

The ten-second gate override never fired. Build 105 installs that request
for every recognised RPG Maker guest; `--bvn-no-font-fix` declines it.

Getting there cost nine device runs and four wrong theories - the GPU
process, `.fon` bitmap fonts, font registration, and the render lifecycle -
every one of them eliminated by a measurement rather than by argument. The
lesson worth keeping is in the order: the fault was named within one run of
the guest being able to report its own state, and not before.

Fonts were, in the end, genuinely involved - but not through availability,
format or registration, all of which were healthy. Note also `azuki:error`
and `kaku:error`: two further faces this title declares do fail to load, and
the game proceeds regardless.

**After the font gate: a divide by zero in FFmpeg.** The guest reaches
`Scene_Map` and a renderer dies seconds later. Build 108's PE-image naming
finally attributed it:

```
Guest divide exception: pid 99 thread ab divide by zero;
    EIP 76B8D515 ffmpeg.dll+0002D515 (image at 76B60000);
    EAX 00000001 ECX 00000000 ... bytes F7 F1 50 FF 37 68 4C 60 D0 76 6A 30 6A 00
```

`F7 F1` is `DIV ECX` with `ECX` zero, so the divisor really was zero and the
emulator executed the instruction correctly - the JIT is not at fault. The
instruction stream after it pushes five arguments ending in the quotient,
with `0x30` among them, which is the shape of an `av_log(..., AV_LOG_DEBUG,
fmt, x, 1/rate)` call: FFmpeg computing a ratio for a log line whose
arguments are evaluated even when the message is discarded.

Two threads of the same process hit the identical instruction with identical
registers, so it is deterministic rather than a race.

Turning BoxedVN's sound off does not avoid it, because that removes the
audio *device* and the engine goes on fetching its music and handing it to
`decodeAudioData` regardless. Build 109 makes the setting mean what it says:
with sound off, an RPG Maker guest also gets its `AudioManager` play entry
points silenced, so nothing is fetched or decoded. That is a workaround for
this crash, not a diagnosis of why FFmpeg sees a zero rate.

**Build 100's census, for the record.****Build 100's census, for the record.** The guest reported 56 font files and
`coue1255.fon, coue1256.fon, coue1257.fon` as examples: the root filesystem
ships Wine's bundled fonts and they are legacy `.fon` bitmap faces.
Chromium reaches fonts through DirectWrite, which loads TrueType and
OpenType and does not load `.fon` at all - that is GDI's format. So a guest
with a full Fonts directory presents an empty font collection to a browser
engine, which is both failures at once: the `FontCache` abort before
`--no-sandbox`, and `document.fonts.ready` never settling after it. It also
explains why seven runs of command-line switches achieved nothing. Build 101
counts scalable and bitmap faces separately and says this in one line rather
than leaving it to be inferred from example file names.

The fix is a scalable font in `Documents/Fonts` (section 7); a Japanese
title wants a CJK face there in any case. It addresses a different failure - Blink having no usable face at
all, the weakness that aborted the renderer outright before `--no-sandbox` -
and should not be assumed to fix this one.

`--enable-logging=stderr` puts Chromium's console output, including page JS
errors, into the session log and is the right first thing to add to any run
that stalls. It is what identified the Canvas-mode PixiJS line above.

Other things seen in these logs that are *not* the problem: a "Profile error
occurred" dialog (Chromium reporting corrupt SQLite in its own user-data
directory, explicitly non-fatal), `Lost UI shared context` on the software
path, and `RoGetActivationFactory` failing for `windows.ui.dll`.

---

## 4. Runtime and display behaviour

### Only the launched process can use Direct3D

A 64-bit session translates exactly one process with FEX, and only that
process can reach Metal. Starting a Direct3D program from inside the running
desktop - double-clicking an `.exe` in winefile, or any other `CreateProcess`
- produces a program that runs but cannot draw.

The chain is short and each link is in the tree:

- `CreateProcess` becomes a `fork` on the emulator, and `KProcess::clone`
  (source/kernel/kprocess.cpp:2926-2934) gives the child `useFEX64 = false`
  and `new KMemory64(child, false)`: a sparse address space, not the native
  identity map. It has to. The identity map places guest pages at their own
  host addresses inside this one iOS process, and the parent still holds
  every one of those addresses - it is waiting on the child, so its
  `KMemory64` is not torn down.
- Every DXMT unix call is refused for such a process at
  source/kernel/syscall64.cpp:3693: the native DXMT table dereferences the
  unix-call parameter block at its host address, which only exists under the
  identity map. The call returns `-ENOSYS` and the log carries
  `BOXEDWINE_DXMT_RETURN ... reason=native-memory pid= fex=0`.
- The child therefore never creates a device. Wine reports
  `D3D11CreateDevice: No default adapter available` and the program exits;
  the session log shows the X11 window being created and mapped first, so the
  window really does appear briefly.

Nothing about presentation is pid-filtered - `BVNDXMTDisplayNotePresented`
accepts any pid and `gDisplayPresenterPid` simply records the last presenter
- so this is not a compositor restriction and cannot be fixed there. Making
it work means giving the child the identity map, which means handing over the
translator, which cannot happen while the parent's address space is still
mapped. The supported way to run a Direct3D program is to launch it as the
session's own process, which is what the cube entry does.

**Run program…** on the container page is that entry for a program of the
user's own. It lists the `.exe` files on the container's two 64-bit drives
(D: is the container's Files folder, C: is `Drive C (64-bit)` beside it),
remembers the last choice, and launches the selected program the way the cube
is launched: `useFEX64` and `useDXMT`, the container's resolution, and the
DXMT environment. Two details are specific to it. The working directory is
the program's own folder, as a guest *Linux* path
(`/mnt/drive_d/<folder>` or `/home/username/.wine64/drive_c/<folder>`), so a
DLL beside the program and the data it opens by relative path both resolve -
a Windows path there leaves the process with no valid current directory at
all (`open(".") -> -2`). And because `-x64modules` used to be derived from
the working directory, the staged DXMT module directory is now passed
separately (`BVNLaunchConfiguration::dxmtModuleDirectory`); without that
split, running a program from its own folder would have projected no DXMT
modules over Wine's module root at all.

### A failed import names itself in the log

A guest that exits `3221225781` (`STATUS_DLL_NOT_FOUND`) could not resolve an
import. Wine says which one in an `err:module:import_dll` line on stderr, but
device sessions have carried no Wine debug channel output at all, so that
line cannot be relied on. Instead the module-search recorder
(`include/dll_search_trace.h`) remembers, per process and without spending
any of its reporting budget, which module names the loader searched for and
never found on any path, and the exit dump prints the most recent one:

```
BOXEDWINE_X64_IMPORT_MISSING pid=45 module=fmod64.dll status=0xc0000135 probes=812 trace_budget_left=0
```

The per-process budget for the full `BOXEDWINE_X64_DLL_SEARCH` trace is 1024
operations (it was 128, which a real program's import tree spent on its first
dozen imports, leaving the log naming every module that loaded and not the
one that did not).

The following remains unproven or deliberately constrained:

- Whether `boxedmain()` can be called a **second time** in one process. The
  design returns to the library UI after a session and allows another launch,
  but Boxedwine calls `SDL_Init`/`SDL_Quit` inside that span. If a second
  session fails, the fallback is to require an app restart between sessions.
- **Guest sessions start landscape-locked, and rotation is opt-in from build
  64.** BoxedVN still finishes the UIKit landscape transition before SDL
  creates its Metal drawable. The in-game overlay can unlock rotation for the
  rest of the session; the presenter then re-fits the picture and the pointer
  transform on every layout pass, so a new drawable is expected rather than
  fatal. Live guest rotation froze rendering and input on build 15, before any
  of that existed; whether it is now clean is **untested on device**.
- Build 16 proved that the **KEYBOARD** button and UIKit bridge execute, but
  SDL 2.32.10's hidden zero-sized `UITextField` did not display a keyboard on
  the current device OS. Build 17 makes the pinned SDL field a 1x1 nearly
  transparent attached responder and verifies `isFirstResponder`. The build
  17 device log reached `firstResponder=yes` but never received
  `UIKeyboardDidShowNotification`, proving responder focus alone was not the
  missing step. Build 18 declares a scene lifecycle and attaches SDL's legacy
  `initWithFrame:` window to the library's active scene before renderer or
  keyboard creation. Build 18 device logs prove attachment succeeded but the
  native keyboard still did not appear. Build 19 draws and hit-tests its own
  QWERTY keyboard in the guest renderer. Letters, digits, Shift, Space,
  Backspace, Enter and arrows therefore no longer depend on UIKit keyboard
  presentation and are device-proven in Notepad. Build 20 stops simultaneously
  requesting UIKit's invisible keyboard, which caused redundant responder and
  layout transitions. **Build 64 replaces all of this with a UIKit overlay**
  (`BVNGuestOverlay.mm`): the SDL-drawn keyboard only existed while SDL owned a
  renderer, so it was invisible for every Vulkan guest - which is every Direct3D
  game. The overlay adds a function row and latching Ctrl/Alt/Shift.
  Native IME/Japanese composition remains unimplemented.
- The default desktop is still **800x600 (4:3)**. It is aspect-fit into the
  wider phone display with black bars, so side bars are expected and Windows
  cannot use the full phone width. (Before build 64 the Vulkan path stretched
  it instead: the fit was scheduled on the main dispatch queue, which is not
  drained while the emulator owns the main thread, so it never ran.) Per-game guest resolution/display profiles are the
  planned fix; do not reintroduce independent X/Y stretching.
- Whether the app survives its intended game workloads without the
  `increased-memory-limit` entitlement. Build 26's first GetMoreRam attempt
  still reports `Standard limit` and 3.29 GB of current process headroom,
  proving that signing path did not authorize the entitlement.
- Audio: latency, underruns, interruption and route-change handling. **No
  claim is made about latency.** Underruns and initialisation failures are
  logged when they happen; nobody has seen one yet.
- Input: touch-as-mouse is device-proven, and build 19's SDL-drawn keyboard was
  device-proven for basic ASCII entry before build 64 replaced it. **The build
  64 UIKit overlay keyboard is untested on device.** Hardware keyboards, native
  IME and GameController mapping still require device tests.
- Japanese locale profiles are not implemented. The Wine 10 root has CP932
  NLS data but defaults to an English (ACP 1252) prefix. A Japanese game's
  error text may therefore appear as mojibake even when the underlying error
  is unrelated. Do not interpret garbled text as a renderer failure by itself.
  The font half of this is now addressable by the user: build 95 copies any
  `.ttf`, `.ttc`, `.otf` or `.fon` file placed in `Documents/Fonts` into every
  prefix's `drive_c/windows/Fonts` at launch, and the session log reports how
  many were found. BoxedVN still ships no fonts - the faces these games ask
  for are Microsoft's - and the locale itself remains English.

---

## 5. A `kpanic` kills the app

Boxedwine's `internal_kpanic` writes the message, then calls `exit(1)`. There
is no recovery path, on any platform.

The message *is* captured: stdout and stderr are redirected into the session
log unbuffered, so the reason survives in
`Documents/Logs/boxedvn-<timestamp>.log` even though the process dies. Open the
log viewer after relaunching, or export the file through Files.

The app will appear to quit without warning. That is what has happened.

---

## 6. Saves are inside the Wine prefix

Wine prefixes live in Application Support, which is excluded from backup and
not visible in Files. Game saves are inside them.

- Deleting a game deletes its prefix, and therefore its saves.
- There is currently **no save export**. Copying saves into Documents is an
  open gap, not a completed feature.

Imported game content and logs *are* in Documents and *are* backed up and
visible in Files.

---

## 7. Root filesystem

- BoxedVN does not ship one. `scripts/fetch-rootfs.sh` downloads a pinned
  upstream Boxedwine archive on demand.
- The contents of those archives have **not been reviewed for
  redistribution**. Do not publish a release with a bundled root filesystem
  until they have been.
- CI builds never bundle one.
- Wine 11.0 is the pinned default because it contains Wine 10.11's fix for
  Song of Saya's known critical-section hang. Wine 10.0 remains pinned as a
  legacy fallback and is device-proven through Notepad. Wine 11.0 has not yet
  been booted on iOS.
- **Fonts.** The archive is a TinyCore image and BoxedVN ships no fonts of its
  own, so a guest sees only whatever faces the archive's Wine installed.
  Anything the user puts in `Documents/Fonts` (`.ttf`, `.ttc`, `.otf`,
  `.fon`) is copied into every prefix's `drive_c/windows/Fonts` at launch,
  where Wine's font backend picks it up on the next start. Existing files are
  never overwritten and the session log reports the counts under `fonts:`.
  This is the supported route for a CJK face, and for a browser-engine game
  that needs glyph coverage Wine does not supply. Build 131 also maps
  Chromium's synthetic last-resort `Sans` name to Wine's bundled scalable
  Tahoma. That prevents Chromium 64's deliberate `FontCache::CrashWithFontInfo`
  abort when every named fallback is absent; it does not provide CJK glyphs or
  replace a real family the game requested.
- A bundled root takes precedence over an imported root. This lets a targeted
  runtime-migration build reliably replace an older imported Wine version.
  Unbundled builds continue to use the imported archive.
- **The archive contains neither Wine Mono nor Wine Gecko.** Verified against
  the pinned Wine 11.0 archive: it has no `opt/wine/share/wine/mono` and no
  `opt/wine/share/wine/gecko`. Left alone, Wine offers to download each of
  them the first time an application touches `mscoree` or `mshtml`, and it
  does so once per prefix - which means once per imported game. BoxedVN's
  prefix policy disables those two modules, which is Wine's own documented
  way to suppress the offer (the registry equivalent of
  `WINEDLLOVERRIDES="mscoree,mshtml="`).
- A game can carry its own guest environment entries in launch settings, one
  `NAME=VALUE` per line. They reach the guest as Boxedwine `-env` values and
  override BoxedVN's own, which is the supported way to set `LANG` for a title
  that expects a locale, to add a per-game `WINEDLLOVERRIDES`, or to raise
  `WINEDEBUG` on a game that starts but draws nothing so the session log says
  why.
- **Consequence: .NET Framework applications and embedded Internet Explorer
  do not run.** They did not run before either - the runtime was never
  present - but the failure is now "module not found" rather than a download
  prompt. To restore the prompt for one prefix, delete the `mscoree` and
  `mshtml` values under `[Software\Wine\DllOverrides]` in that prefix's
  `user.reg`. Bundling wine-mono into the root filesystem would remove the
  limitation properly and has not been done: it adds roughly 80 MB to a
  download that is already 155 MB.

---

## 8. Import

Working and unit tested on the host:

- ZIP import with path-traversal, absolute-path, UNC, NUL-byte,
  control-character and over-length defences, verified end to end against real
  hostile archives.
- A single redundant top-level directory is flattened.
- Recursive `.exe`/`.com`/`.bat`/`.pif` discovery with PE architecture
  detection on each, ordered so the most plausible target comes first.
- Versioned manifests with a refusal path for a newer schema.

On-device import routes:

- The system Files picker remains available, but builds through 18 showed a
  device-only failure where a ZIP row highlighted and never completed.
- Build 19 replaces SwiftUI's security-scoped open-in-place `.fileImporter`
  with an explicit UIKit picker using `asCopy: true` and a delegate. That uses
  the import-copy hand-off and returns a sandbox-local URL.
- Rootfs and game ZIPs can also be copied to **On My iPhone → BoxedVN** and
  selected inside BoxedVN. This route never presents a document picker and is
  the guaranteed fallback while the scene fix is being device-validated.

Not done:

- **Folder import copies, it does not reference in place.** Importing a large
  folder duplicates it.
- No progress reporting during extraction beyond a spinner; the callback exists
  in the C++ layer but is not yet surfaced.
- No cover art, no metadata scraping. Deliberate.

---

## 9. Build and tooling

- The development machine used for this port is an **Intel Mac**. The ARM64
  macOS Boxedwine target was therefore never *run*, only reasoned about from
  source. See `PROGRESS.md` section 3.
- `ios/BoxedVN.xcodeproj` is generated by XcodeGen and is not committed. Do not
  edit it; edit `ios/project.yml`.
- Two vendored third-party portability problems are worked around in the build
  rather than by patching `lib/`:
  - zlib's `zutil.h` sees `TARGET_OS_MAC` on modern Apple SDKs and defines
    `fdopen` to `NULL`; compiled with `-Dfdopen=fdopen`.
  - asmjit calls `sys_icache_invalidate` on all `__APPLE__` targets but only
    includes `<libkern/OSCacheControl.h>` under `TARGET_OS_OSX`; compiled with
    `-include libkern/OSCacheControl.h`.

---

## 10. The x64 future, stated honestly

There is no x64 support. There is a reserved `RuntimeBackendID::FutureX64` that
reports `implemented == false` and executes nothing, and a manifest field
recording which backend wrote each entry.

Adding x64 later would mean a genuinely different runtime — Box64, FEX,
Hangover, Wine WOW64 or another translator — not a flag on this one. The seam
exists so the library, importer, save management and frontend survive that
change. Nothing more should be read into it.
