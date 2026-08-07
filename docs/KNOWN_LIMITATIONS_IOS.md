# BoxedVN — known limitations

Read this before reporting a bug, and before assuming a feature exists. It is
kept deliberately blunt.

**Status as of 2026-08-07:** BoxedVN builds, links and packages into an
installable IPA. **It has never been run on a device.** Nothing in the
"Runtime" sections below has been observed working; see `PROGRESS.md` section 2
for the exact list of what has and has not been tried.

---

## 1. Not supported, by design, in this version

| | |
|---|---|
| **x86-64 Windows programs** | Boxedwine runs a 32-bit Wine. PE32+ executables are detected and refused with a specific message. There is a reserved `FutureX64` backend identifier that implements nothing. |
| **OpenGL, Direct3D acceleration** | No GL backend is compiled in. See section 3. |
| **DXVK, VKD3D, D3DMetal, DirectX 12** | Not present and not planned for this milestone. |
| **The iOS Simulator** | Boxedwine's ARM64 JIT cannot run there. The simulator preset is a compile check only. |
| **Background execution** | The guest does not run while the app is backgrounded. |
| **Multiple simultaneous sessions** | One guest at a time, enforced in the runtime. A second launch request is refused with an explanation. |
| **Steam and other commercial launchers** | Out of scope. |
| **DRM circumvention, anti-cheat, online activation bypass** | Out of scope and will not be added. |
| **App Store distribution** | Sideloading only. |
| **Frame generation, FSR, "performance" toggles** | Not present. Any such control would be decoration. |

---

## 2. JIT is required and BoxedVN cannot enable it

A sideloaded iOS app cannot map executable memory unless the kernel has granted
it `dynamic-codesigning`, which happens only while a debugger is attached.

- **Signing the IPA does not enable JIT.** You must attach StikDebug or an
  equivalent JIT enabler after installing.
- Without it, `mmap(PROT_EXEC | MAP_JIT)` fails with `EPERM`, `BVNJITProbe`
  reports Unavailable with the `errno`, and the app refuses to start a guest.
- **There is no interpreter fallback path on iOS.** Boxedwine does contain an
  interpreting CPU core, but the JIT build cannot reach it when executable
  memory is unavailable at all. Rather than silently running at an unusable
  speed, BoxedVN stops and says why.

---

## 3. No OpenGL

Upstream's macOS ARM64 build gets software OpenGL from a prebuilt
`libOSMesa.8.dylib` backed by a 124 MB `libLLVM.dylib`. Both are macOS ARM64
binaries; neither exists for iOS. Cross-building Mesa and LLVM for iOS is a
substantial project, and llvmpipe would itself need JIT.

So this build defines no GL backend at all. Practically:

- **Programs that need OpenGL will fail.** `KNativeSystem::getOpenGL()` logs
  "Failed to load OpenGL, will probably crash".
- **2D programs are fine in principle.** Classic visual novels use GDI and
  DirectDraw, which go through Boxedwine's CPU framebuffer onto SDL2's
  Metal-backed renderer.

The intended next step is `BOXEDWINE_ES` with `source/opengl/es`, translating
GL to GLES 2, which SDL2 supports on iOS. Not OSMesa.

---

## 4. Runtime behaviour that is untested

Everything here is written and compiles. None of it has been observed.

- Whether `boxedmain()` can be called a **second time** in one process. The
  design returns to the library UI after a session and allows another launch,
  but Boxedwine calls `SDL_Init`/`SDL_Quit` inside that span. If a second
  session fails, the fallback is to require an app restart between sessions.
- Whether SDL 2.32.10's UIKit backend **cooperates with a second `UIWindow`**
  (the SwiftUI library window sits below SDL's).
- Whether the app survives on a device without the
  `increased-memory-limit` entitlement, which free-Apple-ID signing strips.
- Audio: latency, underruns, interruption and route-change handling. **No
  claim is made about latency.** Underruns and initialisation failures are
  logged when they happen; nobody has seen one yet.
- Input: touch-as-mouse, hardware keyboard, and GameController mapping all go
  through SDL2's own iOS backends. They have not been exercised.

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
- Wine 10.0 is the pinned default. Wine 11.0 is also pinned. Neither has been
  booted on iOS.

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
