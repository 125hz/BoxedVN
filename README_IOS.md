# BoxedVN

An iOS/iPadOS port of [Boxedwine](https://github.com/danoon2/Boxedwine),
aimed at running older 32-bit Windows visual novels on iPhone and iPad.

Boxedwine runs Windows programs by emulating a Linux kernel and an x86 CPU and
running a real 32-bit Wine inside it. BoxedVN is that emulator, unchanged in
substance, with an iOS application around it.

> **Status: builds and packages; not yet run on a device.**
> The IPA is produced end to end by the documented commands and validated, but
> as of 2026-08-07 nobody has launched it on hardware. See
> [PROGRESS.md](PROGRESS.md) for exactly what has and has not been tried.

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
- It does **not** enable JIT by itself, and cannot. You must attach StikDebug
  or an equivalent JIT enabler after installing.
- It has **no OpenGL backend** in this build, so programs needing OpenGL will
  fail. Classic 2D visual novels use GDI/DirectDraw and do not.
- It ships **no games and no root filesystem**.

The full list is in [docs/KNOWN_LIMITATIONS_IOS.md](docs/KNOWN_LIMITATIONS_IOS.md).

---

## Requirements

| | |
|---|---|
| Device | physical ARM64 iPhone or iPad, iOS/iPadOS **17.0** or newer |
| Install | sideloading — SideStore, Sideloadly, AltStore or equivalent |
| JIT | StikDebug or an equivalent JIT enabler, **required** |
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

**3. Enable JIT.** Attach StikDebug or an equivalent enabler.
**Signing does not do this.** Open **Runtime status** in the app to confirm —
it tells you whether executable memory could be obtained, and the exact `errno`
if not.

**4. Install a root filesystem.** BoxedVN cannot run anything without
Boxedwine's Linux/Wine root filesystem.

```bash
./scripts/fetch-rootfs.sh --list    # what is pinned
./scripts/fetch-rootfs.sh           # download the default (Wine 10.0, ~149 MB)
```

Copy it to the device through Files, then **Settings → Import root filesystem
ZIP…**. Or build with `./scripts/fetch-rootfs.sh --bundle` to put it inside the
app, at the cost of a much larger IPA.

**5. Try Wine Notepad** from the library. That exercises the whole stack:
root filesystem mount, kernel emulation, Wine start, framebuffer presentation.

**6. Import a game.** ZIP or folder, through the Files picker. BoxedVN scans
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
