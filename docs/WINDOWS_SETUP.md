# BoxedVN on a Windows PC

Written for an agent (Claude Code, Codex) or a person picking this repository up
on Windows with **no Mac available**. It assumes nothing about the earlier
sessions.

The short version: **you cannot build an iOS app on Windows, and you do not
need to.** Everything the project needs from macOS happens on a GitHub-hosted
`macos-15` runner. Windows is where you read logs, change code, run the
host-independent tests, and push. GitHub Actions produces the unsigned IPA; the
phone signs and installs it.

---

## 1. What BoxedVN is

An iOS port of [Boxedwine](https://github.com/danoon2/Boxedwine) — an x86 PC
emulator that runs Wine — packaged as an iPhone app for playing Windows visual
novels.

| Layer | What it is |
| --- | --- |
| SwiftUI app | `ios/app/` — the game library, settings, importer |
| Objective-C++ runtime | `ios/runtime/` — the UIKit bridge, in-game overlay, presentation, logging |
| C++ support library | `ios/support/` — importer, PE detection, paths; **this is what the unit tests cover** |
| Boxedwine core | `source/`, `include/`, `platform/` — the emulator itself, upstream code with BoxedVN patches |
| Guest | Wine 11 + a Linux root filesystem inside `boxedwine.zip`, x86 32-bit |

The x86 → ARM64 JIT needs `CS_DEBUGGED`, which on a sideloaded app means
**StikDebug** running its `universal.js` script against BoxedVN. Without it the
app reports that executable memory is unavailable and refuses to start a guest.
This is a device-side step; nothing in the build can substitute for it.

Target device so far: iPhone 17, iOS 27.

---

## 2. Set up the Windows machine

You need very little.

```powershell
winget install --id Git.Git -e
winget install --id GitHub.cli -e
winget install --id Kitware.CMake -e
winget install --id Ninja-build.Ninja -e
```

Plus a C++20 compiler for the tests — either Visual Studio 2022 Build Tools
("Desktop development with C++") or the LLVM/clang toolchain.

Clone and check out the working branch:

```bash
git clone https://github.com/<owner>/<repo>.git boxedvn
```

The active branch is `ios`; `master` is the upstream-tracking branch. Work on
`ios` unless told otherwise.

### Optional but worth having: WSL2

```powershell
wsl --install -d Ubuntu
```

Two things only WSL can do:

- **Rebuild DXVK.** `third_party/patches/dxvk-2.5.2-moltenvk.patch` is applied to
  DXVK 2.5.2 and the resulting DLLs are committed as prebuilt binaries in
  `ios/app/Dxvk/`. Rebuilding them needs `mingw-w64` and `meson`, which never
  worked on the Mac used for earlier sessions. On WSL it is straightforward.
  Nothing currently needs a rebuild — Grisaia runs with DXVK disabled — but if
  the MoltenVK feature gaps get worked around, this is where that happens.
- **Run the emulator core on a desktop Linux build** for logic that is not
  iOS-specific. Far faster to iterate than a device round trip.

---

## 3. The build loop on Windows

```
edit on Windows  ->  push to the `ios` branch  ->  Actions builds the IPA
   ->  download artifact  ->  sign  ->  install  ->  attach StikDebug  ->  test
   ->  export the log from the app  ->  read it on Windows  ->  repeat
```

### 3a. Run the tests locally first

This is the only part of the build that works natively on Windows. It compiles
`ios/support` and its suite with no Apple SDK, no SDL2 and no emulator core.

```bash
cmake --preset tests-only
cmake --build --preset tests-only
ctest --preset tests-only
```

If this fails, do not push — CI runs the same preset as a gate before the IPA
job and you will just wait 20 minutes to be told the same thing.

### 3b. Bump the build number

Every IPA the user tests must carry a distinct build number, or a device log
cannot be matched to a commit. On Windows, edit `ios/project.yml` directly
(`scripts/bump-build.sh` is a shell script):

```yaml
    CURRENT_PROJECT_VERSION: "75"   # was 74
```

The app logs `BoxedVN 0.1.0 (75) · <git sha>` on its first line. That line is
how you know which binary produced a log.

### 3c. Push, and let Actions build

`.github/workflows/build-ios.yml` runs on every push to `ios`, `main` or
`master` that touches `ios/**`, `source/**`, `platform/**`, `include/**`,
`scripts/**`, `cmake/**` or the workflow itself.

```bash
git push origin ios
gh run watch                 # follow it
gh run download --name BoxedVN-unsigned-ipa-Release
```

Or trigger it by hand with a chosen Xcode version:

```bash
gh workflow run build-ios.yml -f configuration=Release -f xcode_version=26.2
```

The workflow has two jobs. `tests` runs the same `tests-only` preset you ran
locally. `ipa` then fetches the pinned dependencies (SDL2 2.32.10, MoltenVK
1.4.2, XcodeGen 2.46.0 — all checksum-pinned in
`scripts/dependencies.lock.sh`), builds `BoxedVN.app`, and packages
`BoxedVN-unsigned.ipa`. The run summary shows the commit, the Xcode and SDK
versions, the size and the SHA-256.

Artifacts are retained 30 days; build logs 14.

**No Apple ID, certificate, provisioning profile or repository secret is
needed.** The IPA is unsigned on purpose.

### 3d. Install on the phone

1. Download the artifact (a zip containing the `.ipa`).
2. Sign and install with SideStore, AltStore or Sideloadly.
3. Attach **StikDebug** with `universal.js` assigned to the installed app. This
   must be redone after every reinstall.
4. Launch a guest. If the log says executable memory is unavailable, step 3 did
   not take.

If the build carries no root filesystem (the CI IPA is slim), import the
runtime ZIP once in Settings. It survives reinstalls.

---

## 4. Reading logs — the part that actually matters

**Always ask the user to export the log from inside the app** (Settings →
Diagnostics → export) rather than pulling a live console. A truncated live log
once produced a wrong diagnosis that survived several builds.

Logging can be turned off in Settings; if a log is empty, check that first.

The lines worth grepping for:

| Line | Tells you |
| --- | --- |
| `BoxedVN 0.1.0 (N) · <sha>` | which binary this is |
| `iOS guest performance: … frames/sec … cores busy` | frame rate against host CPU, every 5 s |
| `iOS guest present path: … inside vkQueuePresentKHR` | how much of the wall clock the compositor took |
| `=== Guest first-frame hang snapshot` | automatic per-thread dump when the guest drops under 2 fps twice running |
| `Overlay hit test at X,Y in bounds WxH … menu button …` | where a touch landed and whether the overlay or the guest took it |
| `iOS SDL mouse down: button 0 at logical X,Y` | the coordinate the guest actually received |
| `jit:` | StikDebug arena handshake and allocations |
| `Guest fault snapshot` | a guest-side protection fault, with EIP and module |

The thread snapshot is the highest-value diagnostic in the project. Each thread
prints its guest wait condition (`KUnixSocketObject::lockCond` is the X11
socket), its native state, its CPU microseconds, and — if it is inside a host
Vulkan call — `hostVulkanIndex=N`, which indexes `source/vulkan/vkdef.h`
(`169` = `AcquireNextImageKHR`, `170` = `QueuePresentKHR`).

---

## 5. Where things are

| Path | Contents |
| --- | --- |
| `ios/runtime/src/BVNAppDelegate.mm` | presentation geometry, letterbox, refresh-rate hold, window/scene plumbing |
| `ios/runtime/src/BVNGuestOverlay.mm` | the draggable menu button, keyboard, trackpad cursor, quit confirmation, all guest pointer input |
| `ios/runtime/src/BVNRuntime.mm` | session lifecycle, `BVNRuntimeRequestShutdown` |
| `ios/runtime/src/BVNLaunchArguments.cpp` | per-game compatibility profiles (applied **last**, after the generic flags) |
| `ios/app/Sources/AppModel.swift` | launch paths for games, the file browser and Notepad |
| `platform/sdl/knativescreenSDL.cpp` | SDL screen, the C bridges the overlay calls, guest pointer transform |
| `platform/sdl/knativeinputSDL.cpp` | SDL event loop, the 200 ms geometry/overlay poll, `SDL_QUIT` handling |
| `platform/sdl/kvulkanSDL.cpp` | Vulkan surface bookkeeping, the 5-second performance report |
| `source/kernel/kprocess.cpp` | `logThreadSnapshot` |
| `source/vulkan/vulkancommon.cpp` | the guest→host Vulkan dispatch, and the present timing |
| `include/getrusagefairness.h` | the spin-loop fairness throttle |
| `include/bvnhostpresent.h` | present/acquire timing counters |

### Invariants that have each been re-learned the hard way

- **The main dispatch queue is not drained while the guest runs.**
  `boxedmain` owns the main thread. `dispatch_async(dispatch_get_main_queue())`
  never fires during a session. Use `DISPATCH_MAIN_THREAD_BLOCK` (an SDL user
  event via `sdlDispatch`), or the 200 ms poll in
  `KNativeInputSDL::processEvents`.
- **Never call `layoutIfNeeded` from inside `-layoutSubviews` on a sibling
  subtree.** Build 65 did; UIKit stopped laying that subtree out at all after
  the first rotation. Geometry is re-fitted from the poll, not from a layout
  callback.
- **Read the safe area from the Metal view's superview**, not the window.
  `UIWindow.safeAreaInsets` returned portrait insets while the device was in
  landscape.
- **`UIButtonTypeSystem` ignores a later `setTitle:forState:` on iOS 15+.** The
  overlay's panel rows are `UIButtonTypeCustom` because of it.
- **Compatibility profiles must be applied after the generic launch flags**, or
  the flag they mean to override has not been assigned yet.
- **Do not do wholesale block replacements in `BVNGuestOverlay.mm`.** One in
  build 70 silently deleted four layout methods; the menu button was invisible
  for three builds and nothing failed loudly.

---

## 6. Open problems, in the order they matter

1. **X11-over-Vulkan partial-present device acceptance.** Build 75 proved the
   Grisaia motion heartbeat ran but did not change the 0.4 fps symptom. Wine's
   upstream partial-COPY path sends incremental frames through GDI, while
   Boxedwine had disabled and stopped consuming its X11 compositor after the
   Vulkan window appeared. Build 76 composites only those dirty X11 rectangles
   over the Metal view and removes the title-specific heartbeat. Confirm
   `Composited X11 partial present` appears and dialogue updates normally.
2. **The ARM64 JIT miscompiles DXVK.** Saya no Uta needs
   `-interpreterModule d3d11`, which is a workaround, not a fix. See
   `docs/CONTINUING_WITHOUT_A_MAC.md` for the register-level evidence and the
   file to audit (`source/emulation/cpu/armv8/jitArmV8CodeGen.cpp`).
3. **Japanese text is mojibake.** Needs a CP932 locale plus a CJK font in the
   prefix. Not started.
4. **Wine cold-boot reduction device acceptance.** Build 74 measured a 57–59
   second `NDIS` service-start timeout between JIT allocation 320 and 384 in
   three independent launches. Build 75 disables that unsupported driver while
   leaving Wine services and `nsiproxy` enabled. Confirm that gap disappears.
5. **DXVK cannot run under MoltenVK** — it requests `geometryShader`,
   `shaderCullDistance`, `robustBufferAccess2` and `nullDescriptor`, none of
   which MoltenVK exposes. Grisaia works only because DXVK is disabled for it in
   `BVNLaunchArguments.cpp`.
6. **`increased-memory-limit` device acceptance.** CI now publishes
   `BoxedVN-entitlements-ready.ipa`, an ad-hoc template containing the request;
   the ordinary unsigned IPA cannot carry entitlements. Enable the capability
   on the sideloader's exact App ID with GetMoreRAM, then re-sign/reinstall the
   template and confirm Settings reports `Increased limit`.

---

## 7. Working agreements with the user

- **Build an IPA and push after every change.** The user can only evaluate
  BoxedVN by sideloading, so "committed" is not done. On Windows this means:
  bump the build number, push, and tell them the run to download.
- **Diagnose from exported logs only.**
- Screenshots plus the log together; either alone has misled before.

---

## 8. If the CI build breaks

- `Xcode <v> is not installed on this runner` — the `xcode_version` input or the
  `BOXEDVN_XCODE_VERSION` repository variable names a version the runner image
  does not carry. The step lists what is installed; pick one or clear it.
- A dependency fails to fetch — the cache key includes
  `scripts/dependencies.lock.sh`, so a changed pin invalidates it rather than
  silently reusing an old build. Check the checksum in that file against
  upstream.
- `xcodebuild` failure — the `build-logs-Release` artifact carries
  `build/ci/xcodebuild.log` and the third-party build logs.
- Anything that needs a real device (JIT, StikDebug, Metal) cannot be reproduced
  in CI at all. There is no simulator path for a guest session.
