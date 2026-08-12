# Testing BoxedVN

Two kinds of testing exist here, and it is worth being clear about which is
which:

- **Automated, host-independent** — runs anywhere CMake does, including a CI
  runner with nothing but Xcode and CMake. This is what `ctest` covers.
- **Manual, on-device** — everything involving JIT, the emulated Linux kernel,
  Wine, rendering, audio and input. None of it can be automated on a CI runner.
  As of 2026-08-08, JIT, rootfs loading, Wine startup, native-scale Notepad
  rendering, landscape handoff, built-in touch keyboard and basic interaction
  have passed on hardware. Build 32's Saya-only CPU compatibility range is
  the immediate manual test.

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

**Launch arguments and compatibility profiles** (`test_launch_arguments.cpp`)
— valueless Boxedwine switches remain correctly positioned, mounts,
environment and working directories survive argv construction, Wine cannot
truncate BoxedVN's session log, interpreter-module options precede the guest
command, and `Saya_en.exe` selects exactly one evidence-bounded interpreter
range whether it is the direct executable or a Wine-wrapper argument.

**Dependency pinning** (`scripts/test-pins.sh`, registered with ctest) — every
pin has an https URL, a well-formed SHA-256, a positive size and a
description; no URL is mutable (`latest`, `master`, `main`, `nightly`); ids are
unique; the default rootfs id exists. It then verifies that `download_pinned`
**accepts** a correctly checksummed cached file and **refuses** one whose
checksum does not match, and that `verify_sha256` fails on a mismatch. No
network access.

**getrusage fairness** (`ios/tests/test_getrusage_fairness.cpp`) — the
pathological-polling detector activates only after a sustained run of rapid
calls, stays active while polling continues, and releases as soon as the guest
stops. It is a pure state machine with no sleeping or logging, so its
transitions are testable on the host even though the behaviour it guards
(yielding a core to the Metal shader compiler) only matters on device.

Current count: **92 C++ tests + 30 pin checks, all passing in CI.**

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

The physical iPhone now proves the full JIT handshake, Wine Notepad,
native-scale landscape presentation, touch input, built-in keyboard, imported
PE32 launch and Song of Saya's launcher/WineD3D/MoltenVK/audio initialization.
Build 30's two-resolution comparison isolates Saya's remaining first-frame
fault from resolution and Vulkan mapped-memory lifetime. Build 31 proves X11
recovery/input remain functional but its PE module name cannot be resolved.
Continue with build 37's bundled Wine 11 runtime using section 3.9; do not repeat
the resolved keyboard, picker or surface-probe investigations unless their
regression markers fail.

### 3.1 Install

1. Build or download `BoxedVN-unsigned.ipa`.
2. Sign it with SideStore, Sideloadly, AltStore or equivalent.
3. Install on a physical ARM64 iPhone or iPad running iOS 17 or newer.
4. **Expected:** the app launches to the library list. If it launches to a
   black screen, the `BoxedVNFrontend` lookup failed — check the log for the
   message from `BVNAppDelegate`.

### 3.2 JIT, before enabling it

Open **Runtime status**.

**Expected:** *JIT unavailable*, ARM64 JIT compiled in *yes*, and "Debugger
attached (CS_DEBUGGED)" *no*. The safe status check does not allocate or
execute memory and must never issue StikDebug's breakpoint request.

This is the correct result before a JIT enabler is attached, and confirms the
probe is doing real work.

### 3.3 JIT, after enabling it

In current StikDebug, enable **Advanced Options**, long-press BoxedVN, choose
**Assign Script**, select `universal.js`, and launch BoxedVN through StikDebug.
Leave the script session active, then press **Re-check** in BoxedVN. Repeat the
assignment after every reinstall or re-sign; the new app identity is a new
StikDebug target even if the home badge still says `JIT ready`.

**Expected before launching a guest:** *JIT expected*, `CS_DEBUGGED` *yes*, and
a detail line explaining that the universal handshake is deferred.

Tap **Run Wine Notepad** for the definitive test. **Expected:** StikDebug logs
one `brk #0xf00d` prepare-region request per arena segment, all of them during
this one handshake; BoxedVN reports the total capacity it prepared (at least
the 128 MiB floor, more on a device with headroom), mapped as separate RX/RW
aliases, written, cache-flushed, and executed. The session log should then
show live arena allocations explicitly saying that no further StikDebug
breakpoint was issued. If the universal
script is not servicing the initial request, BoxedVN should report a timeout
after about six seconds or a guarded-handshake error instead of freezing or
terminating. Build 27 acceptance specifically includes launching once with the
script deliberately unassigned: the app must remain open and explain how to
reassign it; no `EXC_BREAKPOINT / SIGTRAP` `.ips` should be produced.

### 3.4 Root filesystem

```bash
./scripts/fetch-rootfs.sh                 # Wine 11.0 default
```

Copy the archive to the device through Files, then **Settings → Import root
filesystem ZIP…**.

If tapping the ZIP in the system picker highlights it but does not complete,
copy the archive to **On My iPhone → BoxedVN**, return to Settings, tap
**Refresh**, then select it under **Install from BoxedVN's Documents folder**.
The equivalent game fallback is on the main Library screen under **Games
copied to BoxedVN Documents**.

**Expected:** Settings shows the file name and size instead of "Not installed".

### 3.5 Wine Notepad

**Library → Run Wine Notepad.**

This is the first real end-to-end test: root filesystem mount, Linux kernel
emulation, Wine start, framebuffer presentation. Expect problems, and expect
the session log to explain them.

**Expected:** the screen switches to the guest and Notepad appears.

**Build 18 scene markers:** before the library is usable the log must contain
both `scene-owned library window created` and
`UIWindowScene connected to BoxedVN library`. During guest creation it must
contain `SDL guest window attached to the active UIWindowScene.` before the
loading screen is presented.

In build 16 and later, starting from portrait must rotate the library first and
only create SDL after landscape is settled. The guest then remains locked in
landscape for its lifetime; turning the phone must not rotate or freeze it.
The log must place `Landscape guest geometry settled before SDL startup`
before `boxedmain`, and `iOS guest presentation updated (create)` must show a
native-scale landscape output (for example 2622x1206, not portrait 1206x2622
or one-pixel-per-point 874x402). An SDL-rendered **STARTING WINE** screen must
appear immediately instead of an unexplained black framebuffer. It is
intentionally static while the pre-main-loop startup owns the thread and must
disappear automatically when the first mapped Wine window is drawn; there is
no loading view to dismiss or block guest taps. Build 21 device evidence proved
that `Start=4` does not suppress a root PnP service. Build 22 clears the root
WINEBTH device's service association instead. On its first run, record
launch-to-first-window time and confirm the log contains `Wine prefix ready:
unsupported Bluetooth root device disconnected; Wine HID bus available.`
Touch and the overlay keyboard must remain functional. Cold launch may still
take roughly 1.5 minutes; build 24 proved winebus was not the fix.

The log must place
`iOS guest window shown directly for pre-loop startup` immediately before
`iOS guest loading screen presented`. If the former is the final line, the
native present itself is blocked; if neither appears, window creation failed.

Whatever happens, export the log (**Logs → share**) — it contains the exact
`boxedmain` command line, the JIT verdict, and everything Boxedwine and Wine
printed.

### 3.6 Input

With a guest running, tap the floating **menu button** at the top-left of the
screen and choose **Show keyboard**. A hardware keyboard should work without it.

**Expected in build 64 and later:** a UIKit keyboard panel slides in along the
bottom, with a function row (F1-F12), a full QWERTY layout and latching `ctrl`,
`alt` and `shift` keys. Verify all of:

- Letters, digits, Space, Backspace and Enter affect the guest.
- A latched modifier stays lit and **stays held**: latch `ctrl` in a visual
  novel and the text should keep skipping for as long as it is lit, not for one
  keystroke. Tap it again to release.
- Hiding the keyboard releases every latched modifier - nothing should behave
  as if a key is stuck down afterwards.
- Tapping the game *outside* the panel still reaches the guest. The overlay
  passes through every touch that is not on one of its own controls.

The log records `Guest overlay keyboard shown` / `hidden`. At install time it
records either `SDL resolved every key` or a warning naming any key SDL did not
recognise - a key in that list would be a button that does nothing, and is a
bug in the layout table, not in the guest.

This overlay replaces the SDL-drawn **KEYBOARD** button used up to build 63.
That one was drawn by the SDL renderer and therefore did not exist at all once
a guest switched to Vulkan. Japanese IME is still not part of it.

### 3.6a Menu, rotation and quit

From the same menu button:

- **Rotation: locked / free.** Unlock, turn the device to portrait, and check
  that the guest is drawn as a correctly proportioned band centred in the
  screen with black above and below - *not* stretched to the portrait window.
  Taps must still land where you touch after rotating; that is the part most
  likely to regress. Re-lock and confirm the device returns to landscape.
- **Quit to library.** It asks for confirmation inside the same panel. Confirm,
  and the library UI should return exactly as in section 3.7.

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

**Expected in build 29:** before the command, the log says `Wine prefix ready:
stale GDI/no-3D policy repaired; imported game uses WineD3D Vulkan through
Boxedwine and iOS MoltenVK/Metal.` The logged command must not contain
`-dxvk 1`; it must contain `-w` pointing at the selected executable's
directory and no automatic `WINEDEBUG` trace. Record every WineD3D, Vulkan,
MoltenVK, SDL surface and Metal message. A root-level
`D:\Saya_en.exe`, for example, must use
`-w /home/username/.wine/dosdevices/d:/`; this is the build 20 fix for games
that load adjacent data with relative paths. A DirectDraw game must no longer
end at `Uknown int 99 call: 2897` (`glXChooseVisual`). Test saving and loading,
then quit and confirm the save survives a relaunch. If an error or another GL
call remains, capture both the screen and complete log before changing locale,
fonts, multimedia components, or the graphics backend. Pressing Start Game
must no longer reach Wine's deliberate `Disabling 3D support` path or DXVK's
`D3D_FEATURE_LEVEL_9_1 not supported` error. The log must say `Created
unattached 113x2 CAMetalLayer` and register that X11 window as a Vulkan
`offscreen helper`; it must not register a UIKit surface view for that probe or
make it fake-fullscreen. The 794x568 surface should be registered as a
`presentation` surface and report `Detached UIKit view` when Wine destroys it.
The next line must say `Last presentation Vulkan surface was destroyed;
restored the SDL X11 compositor (1 offscreen/helper surface(s) remain)`, and
the launcher must reappear instead of a permanent black screen. Build 29 is
now device-proven through that launcher. On the tested device, pressing Start
Game proceeds through D3D11 setup, SDL audio and three 1280x960 swapchains,
then faults on a write to `0x03BE0000` before its first frame.

Build 30 device evidence now supersedes that diagnostic expectation. Both an
800x600 and 1280x960 attempt fault on a write to `0x03BD0000` at
`0x7F444DDF`; the address is outside every live and recently unmapped Vulkan
range, and WineDbg faults before mapping a diagnostic window. Resolution and
Vulkan mapped-memory lifetime are therefore ruled out for this failure.

Build 31 proved X11 recovery and fault snapshots work: the blue side areas,
black 800x600 viewport and responsive keyboard mean the failed Metal view was
removed correctly. It also proved the name profile did not activate because
the faulting Wine PE image is `Unknown` to Boxedwine's Linux mapping table.

For build 32, press Start Game once and verify the command contains
`-interpreterRange 7f300000-7f500000` before `/bin/wine` and no
`-interpreterModule mvware`. The log must contain `Compatibility CPU activated
for guest range 7F300000-7F500000` before the engine fault or first frame.
Wine and WineD3D should continue consuming JIT blocks outside that range.
Success is a rendered first frame or main menu. If the same access violation
survives after activation, export the complete log: that proves the decoded
interpreter reproduced the invalid `REP MOVSB` state and rules out ARM64 code
generation for this failure.

Build 32 is now device-proven beyond that expectation. In both supplied logs,
the range activates at `7F444018`; the former `REP MOVSB` failure does not
recur; WineD3D initializes D3D11, SDL opens audio, and the final game
swapchain remains alive. The full-black screen is therefore not evidence that
the old crash returned. Those builds did not instrument presentation.

For build 33, leave the game on screen long enough to export a complete log.
The final Metal surface must initially show **STARTING GAME — WAITING FOR THE
FIRST VIDEO FRAME**, not an unexplained black screen. Interpret the new markers
as follows:

- no `Vulkan first queue submission` means the game did not submit GPU work;
- a submission marker but no `queue-present attempt` means rendering setup
  progressed but the guest never requested display;
- a negative queue-present result identifies the Vulkan presentation error;
- result `0` or `1000001003` (`VK_SUBOPTIMAL_KHR`) must be followed by
  `completed its first successful queue present` and removal of the native
  indicator. If the screen is black after that, the guest rendered/presented
  black and the next investigation is game content, shaders or video playback,
  not UIKit surface visibility.

Also record whether the indicator remains animated and whether audio is heard.
Do not change resolution between that observation and exporting the log.

Build 33 device evidence: the indicator that remained belonged to the final
surface, not the preceding successful present. The log shows the transient
surface submit and return `VK_SUBOPTIMAL_KHR`, remove its indicator, and then
be destroyed. The new final 800x600 surface stops after PNG loading without a
present or exception.

For build 35, the command must contain neither `-interpreterRange` nor
`-interpreterModule`. The compatibility log must say the corrected `REP MOVS`
overlap path is active and the entire engine retains JIT. After Start Game,
preserve one resolution and export the full log. For the final surface, record
the first applicable marker in this order:

1. Any `Compatibility interpreter heartbeat` or `Compatibility CPU activated`
   line is a regression: build 35 must not interpret Saya engine ranges.
2. `acquire attempt 1` proves WineD3D entered the final render loop.
3. `QueueSubmit submission attempt 1` proves it submitted final-surface work.
4. `queue-present attempt 1` and removal of the indicator proves display.

The old `REP MOVSB` fault and underflowed `ECX` must not recur. If either does,
the complete snapshot is required; do not hide it with another address-range
workaround.

For build 36, first open Settings and verify the root description begins
`Bundled runtime`. The IPA intentionally contains Wine 11.0, and the bundled
archive must take priority even if Wine 10 was imported before the upgrade.
Launch Saya without deleting or reimporting the game. The command must still
contain no interpreter selector. After Start Game, success is a visible intro
or menu plus a first acquire/submit/present sequence for the final 800x600
surface. If it still waits, export the complete log; the Wine-version variable
will then be controlled and the next build can add a focused synchronization
trace rather than changing graphics again.

For build 37, keep the same imported game, prefix and 800x600 resolution. The
previous `XPixmap::putImage function not supported 1` and `... 6` warnings must
be absent because `GXand` and `GXxor`, along with every other core X11 raster
operation, are now implemented with plane-mask semantics. Press Start Game
once. Success is a visible intro or menu and a final-surface
acquire/submit/present sequence.

If the first-frame screen remains, leave it visible for at least **35 seconds**
before exporting the log. Build 37 records guest process/thread and futex
snapshots at 12 and 30 seconds without reading the live CPU register file from
another thread. Compare each thread's `dispatches` count between samples:

- an increasing count means guest code is still executing;
- an unchanged `RUNNING` thread identifies a host-blocked call or a long JIT
  block, with its last safe guest `module+offset`;
- a `WAITING` thread plus an active futex record identifies the guest address,
  expected value, wait age and owning PID/TID needed for the next lock fix.

Do not change resolution or add an interpreter range before collecting both
snapshots. One build 37 run is intended to be decisive if the raster fix does
not itself reveal the menu.

### 3.10 Verify the installed memory entitlement

Before and after applying a signing/GetMoreRam modification, record the
**Memory** row directly below **JIT status** on the home screen.

**Expected:** the row says either `Increased limit signed` or `Standard limit`
and includes an available-before-limit value. Runtime status shows the same
signed-entitlement result and byte count. This is a check of the running app's
actual signature plus `os_proc_available_memory()`, not a claim based on the
project entitlement file. A changed entitlement without changed headroom (or
vice versa) is still useful evidence and should be reported with the log.

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
