# Wine 11 scalar dispatch, prefix data, input and audio

Device logs: 20260907-011152, 011251, 011808, 012122, 012621,
012844. All report 5a44e268+dirty, app build 137. Findings below distinguish
implemented repairs from device acceptance, which remains pending.

## Shared runtime findings and changes

- The 64-bit managed application reaches Unity GUI initialization, but
  `System.CurrentSystemTimeZone` throws `Can't get timezone name`. The
  projected builtin tree omits setupapi's WINE_REGISTRY installation pass.
  Seed missing timezone keys from Wine 11.0 kernelbase.rgs before launch,
  preserving existing keys and their customized rules. The generator records
  its source digest; the data includes historical Dynamic DST entries.
- Shell COM marshaling fails for IServiceProvider with E_NOINTERFACE.
  Register its packaged actxprxy proxy and factory in both registry views,
  preserving existing registrations. This repairs the observed COM failure;
  it does not prove that all blank desktop painting is resolved.
- Extracted ntdll.so from the exact prior CI runtime artifact. The sampled
  `0x7a4026164b` is module offset `0x3d64b`, the syscall in
  `__wine_syscall_dispatcher` that executes arch_prctl(ARCH_SET_FS).
  Wine 11 changes FS around NT calls. Avoid full x87/SIMD reconstruction and
  copying for SET_FS/SET_GS, as well as sched_yield, when no signal is pending.
  Correct the existing fast-path guard to include the actual libc/ntdll ELF
  lane, not just the high-address ELF interpreter. PE thunks, memory-writing
  GET operations, pending signals and shutdown use the complete path.
- Relative mouse input now advances QueryPointer's position and honors the
  application's explicit recenter operation. Previously each swipe reset the
  queried position, so cursor-polling applications saw no accumulated movement.
  Raw deltas stay unbounded at screen edges, while explicit iOS warps cannot
  feed inverse motion back through the forced-relative branch.
- The 32-bit launcher creates a full-sized Vulkan probe but never presents it,
  then creates a GDI launcher window. Allow a later mapped window from the same
  display to replace that unpresented target even if its parent stays mapped.
  A successfully presented surface wins over a pending probe; its first real
  frame restores its own target. This addresses visibility, not unproven
  application execution past the launcher.
- New visual-novel logs show repeated OSS underruns and a saturated main guest
  thread, but no captured fatal guest exception. The default 4096-byte OSS
  fragment is shorter than one SDL callback for float stereo. Round the default
  up to a power of two covering a host period; preserve explicit SETFRAGMENT
  requests. Wine's three-fragment write-ahead then has more scheduling slack.
  Long CPU stalls can still starve audio.
- The 32-bit 3D crash involves a generated store at CPU-state offset 0x420.
  Add bounded fault-only x28/frame/index-register and preceding-instruction
  diagnostics. Do not hide the fault, mask an index, or patch application code
  without knowing which value is wrong.

## Validation

- MSVC support/ABI tests: 8/8 CTest targets pass, including new timezone and
  service-provider registry preservation/idempotence cases.
- Production-method native fixtures pass for scalar state preservation,
  signal fallback, the actual libc address, TLS updates, pointer accumulation
  and game-controlled warps, and pending/presented surface selection.
- Vulkan bridge suite: 147 tests pass.
- Windows host tests cannot verify iOS audio deadlines, ARM64 JIT execution,
  camera feel, desktop pixels, game startup or sustained FPS. The next device
  run must establish those outcomes. No claim of a sustained 30 FPS is made.

## Primary references

- Wine 11: dlls/kernelbase/kernelbase.rgs; dlls/ntdll/unix/system.c;
  dlls/actxprxy/actxprxy_servprov.idl; dlls/winex11.drv/mouse.c;
  dlls/wineoss.drv/oss.c; dlls/setupapi/fakedll.c.
- https://raw.githubusercontent.com/wine-mirror/wine/wine-11.0/dlls/actxprxy/actxprxy_servprov.idl
- https://wiki.libsdl.org/SDL2/SDL_AudioStreamFlush (per-packet flushing can
  introduce gaps; no such streaming-converter change is included).
