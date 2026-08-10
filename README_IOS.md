# BoxedVN

An iOS/iPadOS port of [Boxedwine](https://github.com/danoon2/Boxedwine),
aimed at running older 32-bit Windows visual novels on iPhone and iPad.

Boxedwine runs Windows programs by emulating a Linux kernel and an x86 CPU and
running a real 32-bit Wine inside it. BoxedVN is that emulator, unchanged in
substance, with an iOS application around it.

> **Status: build 37 boots Wine and renders interactive, high-resolution
> Wine Notepad on a physical iPhone.** The StikDebug universal handshake,
> retained 128 MiB JIT
> arena, Boxedwine ARM64 translation, root filesystem, Wine startup, native
> scale SDL framebuffer and touch input are device-proven. Device testing found
> that rotating an active guest could freeze SDL and that its keyboard control
> did not establish UIKit first-responder focus. Build 16 device logs prove the
> corrected pre-SDL landscape geometry and successful guest shutdown, but also
> prove that merely invoking SDL's keyboard controller is insufficient: its
> hidden zero-sized field never displayed the keyboard. Build 17 made that
> same SDL field focusable; build 18 attached both windows to one real scene,
> but device testing still produced no native keyboard and the Files picker
> still did not complete selections. Build 19 removes both blockers: Wine has
> an SDL-rendered touch QWERTY keyboard independent of UIKit, and file imports
> use UIKit's import-as-copy contract plus the direct Documents route. The
> first real game launch then exposed two concrete compatibility costs. Build
> 20 starts an imported executable in its own D: folder. Build 21 device
> evidence proved that its attempted graphics and Bluetooth registry values
> were ineffective under Wine 10. Build 22 now selects Wine 10's actual GDI/
> no-3D backend and disconnects the root `winebth` PnP association. Device
> logs prove this reaches Song of Saya's launcher. Build 23's 128 MiB arena
> lets WineDbg fully attach and expose the engine's real no-feature-level/null-
> read failure. Build 24 proves disconnecting winebus does not improve the
> 57–61 second cold-start pause, but also proves a second guest starts in
> seconds inside the same process. Build 25 proved Boxedwine's Vulkan bridge,
> MoltenVK 1.4.2 and the Apple A19 GPU all initialize on-device, but upstream
> DXVK 2.5.2 rejects MoltenVK because it does not expose DXVK's required
> geometry-shader and transform-feedback features. It also exposed a stale
> persistent `renderer=gdi` value that disabled WineD3D. Build 26 repairs
> existing game prefixes to `renderer=vulkan`, routes D3D9 through WineD3D,
> Boxedwine Vulkan and MoltenVK/Metal, supplies promoted Vulkan KHR aliases,
> and reports the actually signed increased-memory entitlement on the home
> screen. A reinstall then proved that StikDebug script assignments do not
> follow a newly signed app identity: build 27 catches that otherwise-fatal
> handshake trap and reports how to reassign `universal.js`. After the script
> was reassigned, build 27 proved WineD3D creates MoltenVK devices and two
> UIKit swapchains for Saya, but SDL left destroyed probe-surface views above
> the live X11 window, producing the black screen. Build 28 tracks each Vulkan
> surface to its UIKit Metal view, removes that exact view on destruction,
> restores the previous surface, and returns to the SDL X11 compositor after
> the last surface. Build 28 device evidence then showed that the earlier,
> still-live 113x2 capability probe was what it restored, so black remained.
> Build 29 creates tiny probes on an unattached Metal layer, excludes them from
> display/input targeting, and restores X11 after the last real presentation
> surface disappears even if probes remain. Its device run proves that fix:
> Saya's launcher is visible, Start Game reaches D3D11 shader setup, SDL audio
> and 1280x960 MoltenVK swapchains, then the game process faults while writing
> `0x03BE0000` before its first frame. Two build 30 runs at different game
> resolutions reproduce the same `0x03BD0000` write fault at `0x7F444DDF`,
> outside every live or recently unmapped Vulkan range; WineDbg then faults
> before mapping its own window. Build 31 successfully restores X11 on that
> failure—the app and keyboard stay responsive—and captures the fatal
> `REP MOVSB` state, but proves Linux mapped-file names cannot identify the
> Wine PE image: the selector never activates and the EIP is `Unknown`. Build
> 32 therefore interprets only the 2 MiB guest range containing both observed
> `mvware` EIPs while Wine and WineD3D retain the ARM64 JIT. Two build 32
> device runs activate that profile, pass the former engine crash, initialize
> D3D11 and audio, and create the game's final swapchain. The remaining full
> black screen is a live Metal surface whose first presentation was previously
> unobservable. Build 33 associates swapchains with surfaces, logs the first
> queue submission and presentation result, and shows a native first-frame
> indicator over the Metal layer until `vkQueuePresentKHR` succeeds. Its device
> log proves one transient game surface presents, but the final surface stops
> after PNG loading before acquire/submit/present. Build 34's interpreter
> heartbeat then revealed 25 million instructions executing in one narrowed
> window. That evidence exposed the actual Boxedwine defect: the optimized
> `REP MOVS` overlap path looped on an invariant address delta instead of
> `ECX`, so Saya's four-byte-overlap copy never terminated and underflowed the
> counter. Build 35 corrects both copy directions, removes Saya's interpreter
> ranges entirely, and keeps the complete engine on ARM64 JIT. Its device log
> reaches the final 800x600 surface and PNG loading without the old fault, but
> still waits before the first final-frame acquire. The project was pinned to
> Wine 10.0; WineHQ records a Song of Saya `RtlpWaitForCriticalSection` hang
> as fixed in Wine 10.11. Build 36 moved the default to Boxedwine's pinned Wine
> 11.0 root containing that fix. Device evidence proved Wine 11 is active and
> ruled out that Wine bug as the sole remaining cause: the final surface still
> stops at the same boundary, where Boxedwine logged unsupported X11 raster
> functions `GXand` and `GXxor`. Build 37 implements all 16 X11 raster
> operations with plane-mask semantics. It also takes safe guest-thread and
> futex snapshots at 12 and 30 seconds if the final surface still has not
> presented, so one device run either reaches the menu or identifies the
> blocked guest thread and lock. See
> [PROGRESS.md](PROGRESS.md) for the exact evidence boundary.

---

## What it is

- Boxedwine's own **ARM64 JIT**, emulated Linux kernel, x86 CPU emulation and
  32-bit Wine environment, cross-compiled for iOS arm64.
- A small SwiftUI frontend for importing and launching games.
- A reproducible, scripted build that produces an **unsigned IPA** with no
  Apple ID, certificate or repository secret — locally or on a GitHub-hosted
  macOS runner.

## What it is not

- It does **not** run 64-bit Windows programs. Boxedwine runs a 32-bit Wine;
  PE32+ executables are detected and refused with a clear message.
- It does **not** enable JIT by itself, and cannot. On iOS 26/27 you must run
  BoxedVN through StikDebug with its `universal.js` script assigned.
- It has **no accelerated OpenGL backend** in this build. Build 37 uses Wine
  11's Vulkan WineD3D renderer for imported Direct3D games; that D3D9 path is
  built and reaches on-device swapchain creation and audio startup. Build 32
  passes the former Saya engine fault, but no visible game frame is proven yet.
  Build 33 proves the final surface never presents; build 34 identifies the
  compatibility interpreter as the active stall; build 35 fixes the underlying
  JIT overlap loop and removes that interpreter workaround. The bundled upstream DXVK
  2.5.2 cannot run on MoltenVK's current feature set.
- It ships **no games**. Normal/public builds ship no root filesystem; the
  private build 37 device-test IPA deliberately bundles Wine 11 to make this
  compatibility test deterministic.

The full list is in [docs/KNOWN_LIMITATIONS_IOS.md](docs/KNOWN_LIMITATIONS_IOS.md).

---

## Requirements

| | |
|---|---|
| Device | physical ARM64 iPhone or iPad, iOS/iPadOS **17.0** or newer |
| Install | sideloading — SideStore, Sideloadly, AltStore or equivalent |
| JIT | Current StikDebug with `universal.js` assigned, **required on iOS 26/27** |
| Root filesystem | downloaded separately, see below |
| Building | macOS with a full Xcode, CMake 3.24+, Ninja |

The iOS Simulator is not supported: Boxedwine's ARM64 JIT cannot run there.

---

## Getting it running

**1. Get an IPA.** Either build one:

```bash
git clone https://github.com/<your-fork>/Boxedwine.git boxedvn
cd boxedvn && git checkout ios
./scripts/bootstrap-macos.sh
./scripts/fetch-dependencies.sh --platform ios
./scripts/build-ios.sh
./scripts/package-ipa.sh --app build/ios-Release/DerivedData/Build/Products/Release-iphoneos/BoxedVN.app
```

…or run the **Build iOS IPA** workflow in GitHub Actions and download the
artefact. Full detail: [docs/BUILD_IOS.md](docs/BUILD_IOS.md).

**2. Sign and install it** with your sideloading tool.

**3. Enable JIT.** In current StikDebug, turn on **Advanced Options**,
long-press BoxedVN, choose **Assign Script**, and select `universal.js`. Launch
BoxedVN through StikDebug and leave that script session active while the guest
runs. **Signing and `CS_DEBUGGED` alone do not do this on iOS 26/27.** Open
**Runtime status** in BoxedVN to confirm the debugger attached; the complete
page-preparation and execution test happens when you start a guest.

**4. Install a root filesystem.** BoxedVN cannot run anything without
Boxedwine's Linux/Wine root filesystem.

```bash
./scripts/fetch-rootfs.sh --list    # what is pinned
./scripts/fetch-rootfs.sh           # download the default (Wine 11.0, ~155 MB)
```

Copy it to the device through Files, then **Settings → Import root filesystem
ZIP…**. Or build with `./scripts/fetch-rootfs.sh --bundle` to put it inside the
app, at the cost of a much larger IPA.

**5. Try Wine Notepad** from the library. That exercises the whole stack:
root filesystem mount, kernel emulation, Wine start, framebuffer presentation.

**6. Import a game.** ZIP or folder through the Files picker, or copy a ZIP to
**On My iPhone → BoxedVN** and select it under **Games copied to BoxedVN
Documents**. BoxedVN scans
for executables, reports each one's architecture and format, and lets you pick
which to run, with per-game arguments and working directory.

---

## Importing games

- ZIP archives and already-extracted folders.
- Archive entries are sanitised before anything is written: `../` traversal,
  absolute POSIX/DOS/UNC paths, embedded NUL bytes, control characters and
  over-length names are all refused, and the reason is shown.
- A single redundant top-level directory is flattened.
- `.exe`, `.com`, `.bat` and `.pif` files are found recursively and inspected;
  installers and uninstallers are demoted in the ordering.
- Each executable's guest architecture is detected from its PE headers. A
  64-bit one is listed and marked not runnable rather than hidden.
- Selection, working directory and arguments persist in a versioned
  `manifest.json` next to the game.

**Bring your own legally owned, DRM-free games.** BoxedVN ships none, and
contains no DRM circumvention.

---

## Where things are stored

| | |
|---|---|
| Imported games, saves you can back up, logs | **Documents** — visible in Files |
| Root filesystem, Wine prefixes | Application Support — excluded from backup |
| Regenerable caches | Caches |

Logs are viewable in the app and exportable through the share sheet. They
contain the exact command line the emulator was started with, the JIT verdict,
and everything Boxedwine and Wine printed — send one with any bug report.

Note that **game saves live inside the Wine prefix**, which is in Application
Support and is deleted with the game. Save export is not implemented yet.

---

## Documentation

| | |
|---|---|
| [PROGRESS.md](PROGRESS.md) | current status, what is proven, what is untried, what is next |
| [docs/WINDOWS_SETUP.md](docs/WINDOWS_SETUP.md) | picking the project up on a Windows PC: CI builds the IPA, what to read, what is open |
| [docs/BUILD_IOS.md](docs/BUILD_IOS.md) | building locally and in CI, signing, entitlements |
| [docs/ARCHITECTURE_IOS.md](docs/ARCHITECTURE_IOS.md) | lifecycle, threading, JIT, rendering, the backend seam |
| [docs/TESTING_IOS.md](docs/TESTING_IOS.md) | the automated suite and the on-device checklist |
| [docs/KNOWN_LIMITATIONS_IOS.md](docs/KNOWN_LIMITATIONS_IOS.md) | what does not work, and why |
| [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) | every dependency and its licence |

---

## Licence

Boxedwine, and therefore BoxedVN, is released under the **GNU General Public
License, version 2**. See [license.txt](license.txt).

Upstream base: `danoon2/Boxedwine` commit
`379bf2414a67fc6509d506a6eefdf6ffa7ebf82d`, Boxedwine version `26R2`.
Upstream history is preserved; the iOS work is on the `ios` branch.
