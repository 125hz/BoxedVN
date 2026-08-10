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

A sideloaded iOS app cannot prepare executable memory by itself. On iOS 26/27,
`CS_DEBUGGED` is necessary but no longer sufficient: the target stops once at
StikDebug's universal JIT breakpoint and StikDebug/debugserver prepares every
16 KiB page in a bounded 128 MiB arena through TXM. BoxedVN then maps those
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
- The executable arena is deliberately bounded. Build 22 proved that Saya plus
  Wine's debugger exceeds 64 MiB, so build 23 raises the limit to 128 MiB. If
  a future workload needs more than 128 MiB of simultaneous native code,
  BoxedVN fails with an explicit
  arena-exhaustion log instead of stopping the guest at a surprise debugger
  breakpoint.
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

## 4. Runtime and display behaviour

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
- Japanese locale/font profiles are not implemented. The Wine 10 root has
  CP932 NLS data but defaults to an English (ACP 1252) prefix and lacks a CJK
  font. A Japanese game's error text may therefore appear as mojibake even
  when the underlying error is unrelated. Do not interpret garbled text as a
  renderer failure by itself.

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
- A bundled root takes precedence over an imported root. This lets a targeted
  runtime-migration build reliably replace an older imported Wine version.
  Unbundled builds continue to use the imported archive.

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
