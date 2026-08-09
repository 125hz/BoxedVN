# Third-party notices

BoxedVN is a port of [Boxedwine](https://github.com/danoon2/Boxedwine). Its
source headers grant the **GNU General Public License, version 2 or later**
(the repository includes GPLv2 in `license.txt`). Builds that statically link
Apache-2.0 MoltenVK must use the GPLv3 option for combined distribution;
Apache-2.0 is not compatible with GPLv2-only distribution.

Two groups: libraries **vendored in this repository** under `lib/`, inherited
from upstream Boxedwine, and dependencies **downloaded at build time**, pinned
by version, URL and SHA-256 in `scripts/dependencies.lock.sh`.

Only the components actually linked into the iOS build are marked as such.
`cmake/BoxedwineSources.cmake` and `CMakeLists.txt` are the authority on which
those are.

---

## Boxedwine

| | |
|---|---|
| Upstream | https://github.com/danoon2/Boxedwine |
| Commit | `379bf2414a67fc6509d506a6eefdf6ffa7ebf82d` (2026-08-05), version `26R2` |
| Licence | GNU General Public License v2 or later (`license.txt` contains v2) |
| Copyright | © 2012-2026 The BoxedWine Team |
| In the iOS build | **yes** — this is the emulator |

Upstream git history is preserved. BoxedVN's changes to upstream files are
limited to three files, all narrowly guarded; they are listed in
`PROGRESS.md` section 5 and every touched file keeps its GPL header.

Boxedwine runs a 32-bit **Wine** inside its emulated Linux. Wine is LGPL v2.1+
and is **not** distributed with BoxedVN — it comes from a root filesystem
archive the user downloads separately; see below.

---

## Vendored in `lib/` (from upstream Boxedwine)

| Library | Version | Licence | Licence file | Linked into iOS |
|---------|---------|---------|--------------|-----------------|
| **asmjit** | vendored snapshot | zlib | `lib/asmjit/LICENSE.md` | **yes** — the ARM64 assembler used by the JIT |
| **zlib** | 1.2.11 | zlib | (in `lib/zlib/zlib.h`) | **yes** |
| **minizip** | zlib contrib | zlib | (in `lib/zlib/contrib/minizip/`) | **yes** — ZIP root filesystem and game import |
| **Berkeley SoftFloat** | Release 3e | BSD 3-clause | `lib/softfloat/COPYING.txt` | **yes** — 80-bit x87 emulation |
| **pugixml** | vendored snapshot | MIT | `lib/pugixml/LICENSE.md` | **yes** |
| **SIMDe** | vendored snapshot | MIT | `lib/simde/COPYING` | **yes** — headers only |
| **Dear ImGui** | vendored snapshot | MIT | `lib/imgui/LICENSE.txt` | no — `BOXEDWINE_DISABLE_UI` |
| **GLEW** | vendored snapshot | modified BSD / MIT | `lib/glew/LICENSE.txt` | no — no OpenGL backend |
| **SDL 1.2 headers** | 1.2 | LGPL v2.1 | `lib/sdl1/COPYING.txt` | no |
| **SDL2 (2.0.12 tree)** | 2.0.12 | zlib | `lib/sdl2/COPYING.txt` | no — superseded, see below |
| **RtMidi** | vendored snapshot | RtMidi (MIT-like) | `lib/rtmidi/LICENSE` | no — MIDI is off on iOS |
| **tiny-process-library** | vendored snapshot | MIT | `lib/tiny-process/LICENSE` | no |
| **Mesa headers** | headers only | MIT | — | no |
| **DirectX import libs** | Windows only | Microsoft | — | no |

`lib/mac/` is not part of the repository: it is fetched by upstream's
`project/mac-xcode/fetchDepends.sh` for macOS builds and is gitignored. Nothing
in it is used by, or shipped in, the iOS build. It contains prebuilt macOS
arm64/x86_64 binaries of SDL2, Mesa/OSMesa, LLVM, MoltenVK, libxml2, libiconv,
ncurses, ICU, zstd, lzma, libedit, libffi and libc++, each under its own
licence.

---

## Downloaded at build time

Pinned in `scripts/dependencies.lock.sh`. Nothing in this repository downloads
anything that is not listed there, and a checksum mismatch is fatal.

### SDL 2.32.10

| | |
|---|---|
| Source | https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz |
| SHA-256 | `5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165` |
| Licence | **zlib** — GPLv2-compatible, permits static linking |
| Copyright | © 1997-2025 Sam Lantinga |
| In the iOS build | **yes**, statically linked |

Built from source for iOS arm64 rather than using the vendored 2.0.12 tree,
whose UIKit backend predates the modern Metal renderer.

BoxedVN subclasses SDL's `SDLUIKitDelegate` through the documented
`+getAppDelegateClassName` hook and does not patch SDL. The interface is
redeclared in `ios/runtime/src/BVNAppDelegate.mm`, pinned to this SDL version.

### XcodeGen 2.46.0

| | |
|---|---|
| Source | https://github.com/yonaskolb/XcodeGen/releases/download/2.46.0/xcodegen.zip |
| SHA-256 | `4d9e34b62172d645eed6457cac13fc222569974098ef4ee9c3368bedf0196806` |
| Licence | MIT |
| Copyright | © 2017 Yonas Kolb |
| In the iOS build | **no** — a build-time tool only |

Generates `ios/BoxedVN.xcodeproj` from `ios/project.yml`. Nothing from it ends
up in the app.

### MoltenVK 1.4.2

| | |
|---|---|
| Source | https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-ios.tar |
| SHA-256 | `b5d947b1660e6e9fed40b9cd2387e160aaab9e80b775c0cef7e14059405178c1` |
| Licence | Apache License 2.0 — combined static distribution uses Boxedwine's GPLv3 option |
| Copyright | © 2015-2026 The Brenwill Workshop Ltd. and Khronos Group contributors |
| In the iOS build | **yes**, statically linked; implements Vulkan over Metal |

The official Khronos iOS arm64 static XCFramework is linked into BoxedVN. SDL
creates its `CAMetalLayer` surface and Boxedwine forwards guest Vulkan calls to
MoltenVK. No dynamic framework is embedded or separately signed. The official
MoltenVK `LICENSE` and this notice file are copied into the app resources.

---

## Root filesystem archives

`scripts/fetch-rootfs.sh` downloads a Boxedwine root filesystem on demand:

| Id | Source | SHA-256 |
|----|--------|---------|
| `wine10` | https://boxedwine.org/v2/5/TinyCore15Wine10.0.zip | `f0ed13ea…5572` |
| `wine11` | https://boxedwine.org/v2/7/TinyCore15Wine11.0.zip | `41835c49…6493` |

These are upstream Boxedwine's own builds and contain a Tiny Core Linux
userland plus a 32-bit **Wine** (LGPL v2.1+), each component under its own
licence.

**BoxedVN does not redistribute them.** They are downloaded by the developer or
the user. The contents have **not** been reviewed for redistribution, so:

- CI builds never bundle a root filesystem.
- No public release should bundle one until that review has happened.

`scripts/fetch-rootfs.sh --bundle` exists for local development and prints this
warning when used.

---

## Not used

For the avoidance of doubt, BoxedVN contains no code from, and no dependency
on: VKD3D, D3DMetal, Box64, FEX, Hangover, UTM, or the
experimental `zaiahgaming/boxedwine-ios` repository. UTM's public
documentation of iOS JIT behaviour informed the design of
`ios/runtime/src/BVNJIT.mm` and the entitlement set; no UTM code was copied.
DXVK is supplied by the separately downloaded Boxedwine Wine rootfs, not by
the BoxedVN IPA; MoltenVK is the pinned static dependency documented above.

---

## Attribution requirements

If you redistribute a build without MoltenVK, comply with GPLv2 or a later
version, including offering complete corresponding source. A build statically
containing Apache-2.0 MoltenVK must use GPLv3 for the combined work. The
permissive licences additionally require their notices to be preserved; the
app bundles MoltenVK's license and this notice file.
