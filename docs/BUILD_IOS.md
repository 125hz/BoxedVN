# Building BoxedVN for iOS

BoxedVN is an iOS/iPadOS port of [Boxedwine](https://github.com/danoon2/Boxedwine).
It builds from a clean clone with no hand-editing of Xcode settings: the
`.xcodeproj` is generated and is not committed.

- **Upstream base:** `danoon2/Boxedwine` commit
  `379bf2414a67fc6509d506a6eefdf6ffa7ebf82d` (Boxedwine `26R2`)
- **Supported guest architecture:** 16-bit and 32-bit x86 Windows programs.
  Not x86-64 — see [KNOWN_LIMITATIONS_IOS.md](KNOWN_LIMITATIONS_IOS.md).
- **Minimum iOS/iPadOS:** 17.0
- **Devices:** physical ARM64 iPhone and iPad only
- **JIT:** required, and must be enabled externally

---

## 1. What you need

| Requirement | Why |
|-------------|-----|
| macOS with Xcode (full install, not just Command Line Tools) | the iPhoneOS SDK |
| CMake 3.24 or newer | `CMakePresets.json` uses schema version 6 |
| Ninja | the generator every preset uses |
| ~10 GB free disk | SDL2 source and build, Xcode DerivedData, the emulator objects |
| A sideloading tool | SideStore, Sideloadly, AltStore or equivalent |
| A JIT enabler | StikDebug or equivalent |

Check the machine and get told exactly what is missing:

```bash
./scripts/bootstrap-macos.sh
```

Add `--install` to have it install the missing Homebrew formulae for you.

---

## 2. Build

```bash
git clone https://github.com/<your-fork>/Boxedwine.git boxedvn
cd boxedvn
git checkout ios

./scripts/bootstrap-macos.sh
./scripts/fetch-dependencies.sh --platform ios
./scripts/build-ios.sh --configuration Release
./scripts/package-ipa.sh \
  --app build/ios-Release/DerivedData/Build/Products/Release-iphoneos/BoxedVN.app \
  --output-dir build/artifacts
```

The result is `build/artifacts/BoxedVN-unsigned.ipa` and a `.sha256` beside it.

Every script accepts `--help`, uses `set -euo pipefail`, prints the versions of
the tools it used, and validates its inputs before doing anything.

### What each step does

**`scripts/bootstrap-macos.sh`** — reports (or installs) missing tools, checks
that `xcode-select` points at a real Xcode rather than the Command Line Tools,
and confirms the iPhoneOS SDK is present.

**`scripts/fetch-dependencies.sh`** — downloads and builds the pinned
dependencies. Every download is pinned by version, URL and SHA-256 in
`scripts/dependencies.lock.sh`; a checksum mismatch is fatal and the file is
left in place for inspection. Nothing in this repository downloads anything
that is not listed there.

- **SDL2 2.32.10**, built as a static library for iOS arm64. The tree vendors
  SDL 2.0.12 headers in `lib/sdl2` and upstream's macOS build links a prebuilt
  SDL2 2.0.14 framework, but 2.0.12's UIKit backend predates the modern Metal
  renderer and is not a reasonable base for iOS 17+.
- **XcodeGen 2.46.0**, which generates the application project.

**`scripts/build-ios.sh`** — two stages. CMake builds every C/C++/Objective-C++
target and merges them into one `libboxedvn.a`; XcodeGen regenerates
`ios/BoxedVN.xcodeproj` from `ios/project.yml` and `xcodebuild` links the Swift
shell against that archive for `generic/platform=iOS`. Code signing is disabled
by default so the same command works on a CI runner with no Apple ID; pass
`--sign` to use a locally configured identity.

**`scripts/package-ipa.sh`** — validates the bundle, stages it as
`Payload/BoxedVN.app`, archives it as an IPA, writes a SHA-256, then unzips the
result and re-validates the unpacked copy as a smoke test.

**`scripts/validate-app.sh`** — run by both of the above. Confirms the
executable exists and is executable, has an arm64 slice, and was built for iOS
(`LC_BUILD_VERSION` platform 2) rather than macOS (1) or the simulator (7);
lints `Info.plist` and checks its required keys; reports whether a root
filesystem is bundled.

---

## 3. CMake presets

For working on the native code directly, without the packaging scripts:

| Preset | What it is for |
|--------|----------------|
| `ios-device-release` | the shipping configuration; what `build-ios.sh` uses |
| `ios-device-debug` | the same, with debug symbols and no optimisation |
| `macos-dev` | compiles the core for the machine you are on and runs the tests |
| `tests-only` | `ios/support` and its tests, no SDL2 and no emulator core |
| `ios-simulator-compile-check` | compiles against the simulator SDK to catch `#ifdef` mistakes |

```bash
cmake --preset tests-only
cmake --build --preset tests-only
ctest --preset tests-only
```

`ios-simulator-compile-check` is a compile check and nothing more. Boxedwine's
ARM64 JIT cannot run under the simulator, so no guest will ever start there.

---

## 4. The root filesystem

Boxedwine runs a real 32-bit Wine inside an emulated Linux, and needs a root
filesystem archive to do it. BoxedVN does not redistribute one.

```bash
./scripts/fetch-rootfs.sh --list        # what is pinned
./scripts/fetch-rootfs.sh               # download the default
./scripts/fetch-rootfs.sh --bundle      # ...and put it in the app bundle
```

The archives are Boxedwine's own TinyCore + Wine builds, pinned by exact
version, URL and SHA-256 in `scripts/dependencies.lock.sh`. There is no
"latest" URL and there will not be one.

**Two ways to get it onto the device:**

1. **Bundled.** Run `scripts/fetch-rootfs.sh --bundle` before
   `scripts/build-ios.sh`. The archive is copied into `ios/app/Bundled/` and
   ends up inside the `.app`. This makes the IPA several hundred megabytes.
2. **Imported.** Build without `--bundle` (the default). The app starts with no
   root filesystem, says so, and offers **Settings → Import root filesystem
   ZIP…**. Copy the archive to the device through Files first. This keeps the
   IPA small and is the recommended development workflow.

CI builds do **not** bundle a root filesystem. The contents have not been
license-reviewed for redistribution, and until they have, no public release
should ship one.

---

## 5. Signing and installing

The IPA is unsigned. Sign it with SideStore, Sideloadly, AltStore or an
equivalent tool and install it on a physical ARM64 device.

### Entitlements

`ios/app/BoxedVN.entitlements` contains exactly two keys, both documented
inline in that file:

| Key | Purpose | Can a signing tool change it? |
|-----|---------|-------------------------------|
| `get-task-allow` | lets a debugger attach, which is how a JIT enabler causes the kernel to grant `dynamic-codesigning` | Tools normally **set** this themselves. It must end up present, or JIT cannot work. |
| `com.apple.developer.kernel.increased-memory-limit` | raises the jetsam limit for the emulated guest address space | **Frequently stripped.** Free Apple IDs are not entitled to it. BoxedVN works without it on devices with plenty of RAM. |

Nothing else is requested. In particular there are no macOS Hardened Runtime
keys (`com.apple.security.cs.*` mean nothing on iOS), no `com.apple.private.*`
keys, and no hypervisor, USB or jailbreak entitlements — BoxedVN emulates
entirely in userspace.

### Signing does not enable JIT

This trips people up, so it is worth stating plainly: **signing the IPA does
not give the app JIT.** After installing, you must attach StikDebug or an
equivalent JIT enabler. Until you do:

- `mmap(PROT_EXEC | MAP_JIT)` fails with `EPERM`,
- `BVNJITProbe` reports **Unavailable** and includes the `errno`,
- the app refuses to start a guest and says why.

BoxedVN cannot enable JIT by itself and does not pretend it can.

To check: open **Runtime status** in the app. It shows whether the ARM64 JIT
is compiled in, whether executable memory could be obtained, and the exact
reason if not. Press **Re-check** after attaching the JIT enabler.

---

## 6. Building through GitHub Actions

`.github/workflows/build-ios.yml` produces the same artefact on a
GitHub-hosted macOS runner with **no Apple ID, certificate, provisioning
profile or repository secret**.

- Triggers on `workflow_dispatch`, and on pushes and pull requests that touch
  the build, the emulator sources or the workflow itself.
- Prints the selected Xcode and iPhoneOS SDK versions.
- The Xcode version can be chosen with the `xcode_version` workflow input or
  the `BOXEDVN_XCODE_VERSION` repository variable; if the requested version is
  not on the runner the job fails and lists what is installed.
- Caches only `third_party/`, keyed on the lock file, so changing a pin
  invalidates the cache rather than silently reusing an old build.
- Runs the host-independent tests first, as a separate fast job.
- Uploads `BoxedVN-unsigned.ipa`, its `.sha256` and the build logs, with 30-
  and 14-day retention.

Download the artefact, unzip it, and sign the IPA inside as above.

---

## 7. When a build fails

The scripts try to fail with the actual cause rather than a generic message.
Some specific ones:

**"The iPhoneOS SDK is not installed"** — `xcode-select` is pointing at the
Command Line Tools:
```bash
sudo xcode-select -s /Applications/Xcode.app
xcodebuild -downloadPlatform iOS
```

**"Checksum mismatch"** — the pinned artefact changed upstream or the download
is corrupt. The file is left in place; delete it to retry. Do not update the
pin without checking what changed.

**"xcodebuild failed"** — the last 60 lines are printed and the full log path is
given (`build/<config>/xcodebuild.log`).

**"does not contain an arm64 slice"** — the CMake build produced the wrong
architecture. Check `CMAKE_OSX_ARCHITECTURES` and that the build directory was
not reused from a different preset. `rm -rf build/` and retry.

---

## 8. Licensing

Boxedwine, and therefore BoxedVN, is GPLv2. See `license.txt`. Every
third-party dependency and its licence is listed in `THIRD_PARTY_NOTICES.md`.
