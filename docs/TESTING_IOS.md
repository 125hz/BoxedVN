# Testing BoxedVN

Two kinds of testing exist here, and it is worth being clear about which is
which:

- **Automated, host-independent** — runs anywhere CMake does, including a CI
  runner with nothing but Xcode and CMake. This is what `ctest` covers.
- **Manual, on-device** — everything involving JIT, the emulated Linux kernel,
  Wine, rendering, audio and input. None of it can be automated on a CI runner
  and, as of 2026-08-07, none of it has been done yet.

---

## 1. Automated tests

```bash
cmake --preset tests-only
cmake --build --preset tests-only
ctest --preset tests-only
```

This preset builds `ios/support` and its test suite with **no SDL2, no emulator
core and no Apple SDK beyond a C++20 compiler**, so it is fast and it fails for
real reasons.

The harness is dependency-free (`ios/tests/boxedvn_test.h`), because requiring
a test framework to be installed would make "a clean clone builds through
documented commands" false.

### What is covered

**PE architecture detection** (`test_pe_inspector.cpp`) — images are built byte
by byte in the tests, so the exact header field under test is visible:

| Case | Expected |
|------|----------|
| PE32 / `IMAGE_FILE_MACHINE_I386` | `x86_32`, runnable, backend `boxedwine-x86` |
| PE32+ / `AMD64` | `x86_64`, **not** runnable, message contains "x64 is not supported by the current runtime" |
| PE32+ / `ARM64` | `unknown`, not runnable, diagnostic names ARM64 |
| PE32+ magic with an I386 machine | rejected as contradictory, **not** treated as runnable 32-bit |
| empty file | rejected, "empty" |
| plain text | rejected, diagnostic mentions the missing `MZ` |
| `e_lfanew` past end of file | rejected, "past the end" |
| truncated inside the COFF header | rejected, "truncated" |
| DOS `MZ` with no extended header | `x86_16`, runnable |
| `NE` | 16-bit Windows, runnable |
| `LE`/`LX` | recognised, not runnable, diagnostic mentions VxD |
| `SizeOfOptionalHeader == 0` | rejected as an object file |
| unknown optional-header magic | rejected, diagnostic quotes the magic |
| .NET COM descriptor present | reported, still runnable |
| missing file | rejected, diagnostic quotes the path |

**ZIP traversal and path normalisation** (`test_path_safety.cpp`,
`test_zip_import.cpp`) — the string-level rules *and* an end-to-end test that
builds real hostile archives with minizip's writer and tries to extract them:

- `../../../../tmp/pwned` — refused, and the file is confirmed absent outside
  the destination afterwards
- `/etc/passwd`, `C:\Windows\...`, `\\server\share\...` — refused as absolute
- embedded NUL, control characters, over-length components — refused
- interior `..` that stays inside the destination — resolved, accepted
- backslash separators — normalised
- redundant single top-level directory — detected and flattened, and *not*
  flattened when the option is off
- a directory whose name merely starts with the prefix — left alone
- zip-bomb ceiling — enforced, nothing written
- a non-ZIP file — specific error, not "invalid archive"

**Manifests and JSON** (`test_manifest.cpp`) — round trip, stable
serialisation, refusal of a newer `schemaVersion` with an actionable message,
missing/empty required fields, malformed JSON, non-object root, `\u` escapes
including surrogate pairs, unterminated strings, trailing content.

**Runtime backend selection** (`test_manifest.cpp`) — that
`selectBackend(X86_64)` is `none`, that **no** backend claims to execute
`X86_64`, and that `FutureX64` reports `implemented == false` with an empty
architecture list.

**Import heuristics** — identifier derivation is filesystem-safe and bounded;
installers and uninstallers are demoted; `buildManifest` picks the first
runnable executable and leaves the selection empty when nothing is runnable.

**Dependency pinning** (`scripts/test-pins.sh`, registered with ctest) — every
pin has an https URL, a well-formed SHA-256, a positive size and a
description; no URL is mutable (`latest`, `master`, `main`, `nightly`); ids are
unique; the default rootfs id exists. It then verifies that `download_pinned`
**accepts** a correctly checksummed cached file and **refuses** one whose
checksum does not match, and that `verify_sha256` fails on a mismatch. No
network access.

Current count: **59 C++ tests + 26 pin checks, all passing.**

### What is deliberately not covered

Anything needing SDL, UIKit, a device or the emulator. A test that mocked those
would assert that the mock works.

---

## 2. Artefact validation

Run automatically by `scripts/build-ios.sh` and `scripts/package-ipa.sh`, and
in CI:

`scripts/validate-app.sh` checks that the executable exists and is executable;
has an **arm64** slice; was built for **iOS** (`LC_BUILD_VERSION` platform 2)
rather than macOS (1) or the simulator (7); that `Info.plist` lints and has
`CFBundleIdentifier`, `CFBundleExecutable`, `CFBundleName`,
`CFBundleShortVersionString`, `CFBundleVersion` and `MinimumOSVersion`; that
`CFBundleExecutable` names a file that exists; and reports whether a root
filesystem is bundled. Set `BOXEDVN_EXPECT_ROOTFS=1` to make a missing one
fatal.

`scripts/package-ipa.sh` then unzips the IPA it just produced, confirms
`Payload/BoxedVN.app` with its executable and `Info.plist`, and re-runs the
validator on the unpacked copy — which catches anything the archive step
mangled that a checksum of the archive cannot.

---

## 3. On-device testing

**None of this has been done yet.** Work through it in order; each step depends
on the previous one.

### 3.1 Install

1. Build or download `BoxedVN-unsigned.ipa`.
2. Sign it with SideStore, Sideloadly, AltStore or equivalent.
3. Install on a physical ARM64 iPhone or iPad running iOS 17 or newer.
4. **Expected:** the app launches to the library list. If it launches to a
   black screen, the `BoxedVNFrontend` lookup failed — check the log for the
   message from `BVNAppDelegate`.

### 3.2 JIT, before enabling it

Open **Runtime status**.

**Expected:** *JIT unavailable*, ARM64 JIT compiled in *yes*, executable memory
*no*, and a detail line naming `mmap(PROT_EXEC | MAP_JIT)` with an `errno`.

This is the correct result before a JIT enabler is attached, and confirms the
probe is doing real work.

### 3.3 JIT, after enabling it

Attach StikDebug (or equivalent) and press **Re-check**.

**Expected:** *JIT available*, and a detail line saying the page was mapped,
written, cache-flushed and executed.

If it still fails, the `errno` in the detail is the diagnosis. Capture it.

### 3.4 Root filesystem

```bash
./scripts/fetch-rootfs.sh --id wine10
```

Copy the archive to the device through Files, then **Settings → Import root
filesystem ZIP…**.

**Expected:** Settings shows the file name and size instead of "Not installed".

### 3.5 Wine Notepad

**Library → Run Wine Notepad.**

This is the first real end-to-end test: root filesystem mount, Linux kernel
emulation, Wine start, framebuffer presentation. Expect problems, and expect
the session log to explain them.

**Expected:** the screen switches to the guest and Notepad appears.

Whatever happens, export the log (**Logs → share**) — it contains the exact
`boxedmain` command line, the JIT verdict, and everything Boxedwine and Wine
printed.

### 3.6 Input

With Notepad running: tap to position the caret, type on the software or a
hardware keyboard.

**Expected:** characters appear in Notepad. Anything less means input is not
reaching the guest.

### 3.7 Exit

Quit the guest.

**Expected:** the library UI returns and the app is still running.
`BVNRuntimeGetState` should read `stopped` and **Runtime status** should show
the guest's exit code.

This is the acceptance criterion most at risk: whether `boxedmain()` survives
being called and returning. See `docs/KNOWN_LIMITATIONS_IOS.md` section 4.

### 3.8 Import a game

Import a DRM-free 32-bit visual novel as a ZIP, from Files.

**Expected:** it appears in the library; **Game details** shows the discovered
executables with architecture, format and subsystem; the most plausible one is
preselected.

To confirm the architecture detector on real data, import something containing
a 64-bit executable. **Expected:** it is listed, marked not runnable, with the
"x64 is not supported by the current runtime" message — not hidden, not
silently skipped.

### 3.9 Run it

**Launch**, from Game details.

**Expected:** the game runs. Test saving and loading, then quit and confirm the
save survives a relaunch.

---

## 4. Reporting a failure

Include all of:

- the exact step from section 3
- device model and iOS version
- the signing tool and the JIT enabler used
- **the exported log file** — it has the command line, the JIT verdict and the
  emulator's own output
- the IPA's SHA-256, so the exact build is identifiable

If the app disappeared without warning, that is probably a `kpanic`; the reason
is in the log even though the process died. See
`docs/KNOWN_LIMITATIONS_IOS.md` section 5.
