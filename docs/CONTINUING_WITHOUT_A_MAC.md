# Continuing BoxedVN without a Mac

Everything needed to build a testable unsigned IPA is committed. GitHub Actions
(`.github/workflows/build-ios.yml`) runs on `macos-15` and produces the same
slim IPA that reached Saya no Uta's main menu, so no local macOS is required.

## Building

Push to the `ios` branch, or run the workflow manually from the Actions tab.
It fetches the pinned dependencies, builds `BoxedVN.app`, packages
`BoxedVN-unsigned.ipa` and uploads it as a workflow artifact. Download it,
sign it (SideStore / Sideloadly / AltStore), and install.

The CI IPA is **slim**: it does not embed the ~160 MB root filesystem. Import
the runtime ZIP on the device once — it survives reinstalls. See
`docs/BUILD_IOS.md`.

JIT still requires StikDebug with `universal.js` assigned to the installed app,
and the assignment must be redone after every reinstall.

## Where the current state is

`PROGRESS.md` is the authoritative record. As of the last Mac session:

- The game **renders its main menu**. This required routing Direct3D 11 through
  a patched DXVK rather than wined3d, because Metal has no geometry shader
  stage and wined3d therefore refuses shader model 4 outright.
- It only works with `-interpreterModule d3d11`, i.e. **DXVK runs under the
  interpreter, not the ARM64 JIT**. That is a diagnostic workaround, not a fix,
  and it is the dominant cause of the low frame rate.

## The three open problems, in priority order

### 1. The ARM64 JIT miscompiles DXVK (biggest win)

Measured with `THREAD_BASIC_INFO`: the guest thread consumed 981,821 us of CPU
in a 1000 ms window (98% of a core) while making zero guest progress, spinning
in JIT-compiled code. Interpreting `d3d11` removes it entirely.

The likely shape, from the eventual crash in `Mware.dll`:
```
mov  (%ecx),%eax     ; EAX = [ECX]      load vtable   -- executed correctly
call *0x6c(%eax)     ; CALL [EAX+slot]                -- branched to ECX instead
```
Register state at fault had `EIP == ECX == EDI` (the object) while `EAX` held
the correct vtable pointer, i.e. the indirect call used the addressing base
register instead of the loaded target. Audit the ARMv8 backend's indirect-call
lowering (`source/emulation/cpu/armv8/jitArmV8CodeGen.cpp`) and Boxedwine's
generic `CALL E32` path, which computes `newEip = read32(calculateEaa(op))`
before pushing the return address.

Fixing this lets `-interpreterModule d3d11` be removed, which should transform
performance. The temporary flag lives in `BVNApplyKnownCompatibilityProfile`
(`ios/runtime/src/BVNLaunchArguments.cpp`) and is covered by a test that
documents it must not ship.

Precedent: build 35 fixed a real ARMv8 `REP MOVS` miscompilation in this same
backend, so a second codegen defect is plausible.

### 2. Aspect ratio and safe area

The SDL renderer path letterboxes correctly (`viewport 252,0 800x600,
scale 2.010`), but the Vulkan path registers `SDL_uikitmetalview` at the full
window (`874x402`), so DXVK's swapchain blit stretches 4:3 content across the
whole 2.2:1 display, including under the Dynamic Island. Size that view to the
guest aspect within the safe-area insets; that also makes touch coordinates
1:1 for free.

### 3. Touch input does not reach the guest

Only `SDL_MOUSEBUTTONDOWN` / `SDL_MOUSEBUTTONUP` are handled
(`platform/sdl/knativeinputSDL.cpp`). SDL synthesises those from touches, but
the Vulkan metal view is layered over SDL's view and is the prime suspect for
consuming them. Fixing (2) first is likely to make this tractable.

## The DXVK fork

`third_party/patches/dxvk-2.5.2-moltenvk.patch` applies to upstream DXVK at tag
`v2.5.2` (commit `b4faf0b`). It relaxes device features MoltenVK cannot provide
- `geometryShader`, `shaderCullDistance`, `textureCompressionBC`,
`VK_EXT_transform_feedback`, and robustness2's `robustBufferAccess2` /
`nullDescriptor` - from required to optional. Upstream fails device creation
without them; nothing in `GetMaxFeatureLevel()` consults them and its floor is
`D3D_FEATURE_LEVEL_10_1`, so the resulting device is still shader-model-4
capable.

The prebuilt 32-bit modules are committed at `ios/app/Dxvk/` so CI needs no
mingw toolchain. To rebuild them (Linux or Windows/WSL both work):

```bash
git clone --branch v2.5.2 --recurse-submodules https://github.com/doitsujin/dxvk
cd dxvk && git apply /path/to/dxvk-2.5.2-moltenvk.patch
meson setup --cross-file build-win32.txt --buildtype release \
      -Denable_d3d9=false -Denable_d3d8=false build.w32
ninja -C build.w32
i686-w64-mingw32-strip build.w32/src/{d3d11/d3d11.dll,dxgi/dxgi.dll,d3d10/d3d10core.dll}
```
D3D9/D3D8 are disabled because current mingw headers already define
`_D3DDEVINFO_RESOURCEMANAGER`, colliding with DXVK's own copy. The engine
imports only `d3d11.dll`, so this costs nothing here.

Copy the three stripped DLLs over `ios/app/Dxvk/` and commit.

## Things already ruled out, with evidence

Recorded so they are not re-investigated:

- **wined3d for this title** - architectural, not a bug. Metal has no geometry
  shader stage, D3D10 mandates it, so wined3d never advertises feature level 10
  and skips every SM4 chunk. Saya's rendering is 331 SM4 shaders.
- **A software or DirectDraw fallback** - `Mware.dll` imports `d3d11.dll` in its
  PE import table; removing it fails the loader with `c0000135`.
- **Scheduler starvation and host-mutex blocking** for the main stall - the
  kernel reports the thread RUNNABLE and burning a full core.
- **A guest page-fault loop** - the per-thread fault counter stays at 2.
- **`IID_ID3D12CommandQueue` QueryInterface** - DXVK's own swapchain
  constructor queries it and nulls the out pointer; the warning is normal.
- **The D32_SFLOAT_S8_UINT render-pass diagnostic** - `BindFramebuffer` never
  has a depth-stencil view in these runs, so it is unrelated to the stall.
