# Plan: 32-bit Windows programs on the 64-bit FEX path (new WoW64)

Status: planning only. Nothing in this document is implemented. It records
the direction agreed on 2026-09-01 so the work can start later without
re-deriving it.

## Why

BoxedVN runs two engines today:

| Lane | CPU | Wine | Memory model | Direct3D | Used by |
|---|---|---|---|---|---|
| Legacy 32-bit | BoxedWine's own IA-32 JIT | 32-bit Wine, bundled prefix | software page table, per-access helpers | wined3d over OpenGL / MoltenVK | every existing title |
| 64-bit FEX | FEX translator (ARM64) | Wine64 from the Ubuntu layer, `.wine64` prefix | direct dereference through KMemory64 alias windows | DXMT over Metal | the x86-64 probe |

The legacy lane is adequate for 2D titles. A 32-bit 3D title would hit three
ceilings there: the translator, the memory model, and the renderer. Swapping
the translator alone (a "FEX32" backend inside the 32-bit kernel; see
`docs/ARCHITECTURE_FEX32_IOS.md` and the `fex32_*` admission contracts in
`ios/support`) removes one of the three.

Wine's own answer is *new WoW64*: a Wine built for both architectures runs
32-bit Windows programs inside a 64-bit Unix process. The 32-bit PE code is
executed in the CPU's compatibility mode, every Unix-side call is thunked
through 64-bit ntdll, and Direct3D from a 32-bit program reaches 64-bit
graphics modules. No 32-bit Linux libraries are needed. Under BoxedVN that
means a 32-bit game gets the FEX translator, the 64-bit memory model, the
64-bit X11 bridge and DXMT, all of which exist or are being proven now.

Decision: extend the 64-bit FEX lane with new WoW64. Keep the legacy lane
selectable per container until the WoW64 lane has run the existing library,
then make WoW64 the default for 32-bit programs. Do not port FEX into the
32-bit kernel.

## Prerequisites (must be true before phase 1 starts)

- The x86-64 Direct3D 11 probe renders through DXMT on a device (the
  current goal). WoW64 graphics thunk into the same DXMT modules; nothing
  below is worth starting while 64-bit D3D11 is unproven.
- At least one plain 64-bit Windows program runs end to end on the lane
  (window, input, present), so the lane's stability is known independently
  of the probe.

## Architecture of the finished lane

```
 32-bit PE (game.exe)      64-bit PE builtins            Unix side (x86-64 ELF)
 ┌──────────────────┐      ┌────────────────────────┐    ┌──────────────────────┐
 │ game code (i386) │ ──►  │ i386-windows/*.dll     │ ─► │ x86_64-unix/ntdll.so │
 │ d3d11.dll (i386) │      │ (32-bit PE builtins)   │    │ winex11.so           │
 └──────────────────┘      │ wow64.dll, wow64win.dll│    │ winemetal.so (DXMT)  │
        compat mode        │ x86_64-windows/*.dll   │    └──────────────────────┘
        (CPU in 32-bit)    │ (64-bit PE builtins,   │            ▲
                           │  DXMT d3d11/dxgi)      │            │ private syscalls
                           └────────────────────────┘            │
                                        ▲                        │
                                        │ FEX (mode switches)    │
                                  BoxedWine kernel: KMemory64, syscall64, X11 bridge,
                                  DXMT dispatcher, VFS overlays, SDL presentation
```

Everything stays under BoxedWine. FEX remains the CPU only. The process is
64-bit from the kernel's point of view. The 32-bit code segments run in a
second FEX context configured for 32-bit mode; a BoxedVN `wow64cpu.dll`
hands each 32-bit slice to it and takes the registers back (phase 2
decision below).

## Phases

Each phase has a host-side gate (CI) and a device gate (log markers). A phase
is not done until its device gate is met on a physical device.

### Phase 0: Inventory (no code)

- Record which Wine version the runtime layer ships and whether the distro
  provides a dual-architecture build. Ubuntu's `wine64` package does not ship
  the `i386-windows` PE tree; `wine32` ships the old WoW64 (needs 32-bit
  Unix libraries) and is not what we want.
- Decide the Wine source: a Wine build configured with
  `--enable-archs=i386,x86_64`, either built in the runtime job (Ubuntu,
  long) or fetched as a pinned prebuilt with recorded hashes, in the style
  of `scripts/dependencies.fex64.lock.sh`.
- Confirm DXMT's 32-bit thunk story at the pinned commit
  (`willfaust/dxmt` `ios-port`): its PE side already carries `wow64`
  entries (`__wine_unix_call_wow64_funcs`, `UInt32ToPtr`), which is the path
  a 32-bit d3d11.dll takes.

Gate: a written note in this file naming the Wine source and the DXMT thunk
status.

**Phase 0 result (2026-09-02).**

- *Prerequisite met:* the x86-64 Direct3D 11 probe draws a rotating cube
  through DXMT on device (commit b16685a4: `shaders ok`, `geometry ok`,
  240 presents, `complete ok`).
- *Wine source:* no change of source is needed. The runtime layer ships
  Ubuntu 24.04's `libwine` 9.0~repack-4build3 (amd64), whose
  `x86_64-windows` tree already contains `wow64.dll`, `wow64win.dll` and
  `wow64cpu.dll`, and whose 64-bit unix libraries carry the WoW64 thunk
  tables (they are compiled for every 64-bit build, not only
  `--enable-archs` builds). The matching 32-bit PE tree is the same
  version's `libwine:i386`, at `/usr/lib/i386-linux-gnu/wine/i386-windows/`
  (`kernel32.dll`, `ntdll.dll`, `user32.dll`, `d3d11.dll`, ...). Its
  `i386-unix` tree is the old WoW64 and is not packaged. The runtime job
  downloads that one package and extracts only `i386-windows`, so the
  PE builtins and the unix side always come from one Wine version.
- *DXMT thunk status:* the pinned fork's unix side carries the 32-bit
  entry points (`thunk32_SM50Initialize`, `thunk32_SM50Compile`,
  `thunk32_SM50GetArgumentsInfo`, ... with `UInt32ToPtr`) beside the
  64-bit ones, in a second dispatch table. They compile into the native
  archive today and have not been exercised. The 32-bit PE thunks
  (`d3d11.dll`, `dxgi.dll`, `winemetal.dll` for i386) still have to be
  built with the pinned llvm-mingw for `i686-w64-mingw32`; that is phase
  1's last bullet.

### Phase 1: Dual-architecture Wine layer

- Extend `scripts/build-wine64-runtime-ci.sh` to package the
  `i386-windows` PE tree beside `x86_64-windows` and `x86_64-unix`, and
  `wow64.dll` / `wow64win.dll` / `wow64cpu.dll` (Wine's own), under the same
  module root (`include/guest_wine64_layout.h`).
- Extend `scripts/validate-wine64-runtime.sh` and
  `scripts/test_wine64_runtime_packaging.py` with the new required paths and
  an ELF/PE class check (i386 PE builtins are PE32, not PE32+).
- `projectX64WineSystemModules` must project both trees: Wine expects
  `syswow64` for 32-bit builtins and `system32` for 64-bit ones.
- The DXMT module overlay (`overlayX64WineModules`) must also place DXMT's
  32-bit PE thunks (built with `-Dwine_builtin_dll=true` for i386) into
  `i386-windows`, so a 32-bit program's `d3d11.dll` import resolves to
  DXMT's 32-bit thunk, which forwards to the 64-bit DXMT modules.

Gate (CI): validator passes with both trees; PE32 checks pass. Gate
(device): a 32-bit `notepad.exe`-class builtin starts under the 64-bit prefix
and reaches `BOXEDWINE_X64_PROC_FIRST_SYSCALL` from a WoW64 process.

### Phase 2: Mode switching in the FEX integration

This is the risk that decides the schedule.

- Wine's WoW64 layer switches between the 64-bit and 32-bit code segments
  with far transfers (`wow64cpu.dll`; on ARM64 Wine uses the FEX WoW64 DLL
  instead of an x86 CPU). Decide which of the two shapes BoxedVN uses:
  1. Let FEX translate the 32-bit code segments in the same context, i.e.
     honour `CS` mode changes (the shape FEX's own WoW64 product uses).
  2. Provide a BoxedVN `wow64cpu` replacement that hands 32-bit code to a
     second FEX context configured for 32-bit mode.
  Decision (2026-09-02): shape (2). The pinned translator fixes the
  operating mode per context: the decoder picks its machine mode from
  `Config.Is64BitMode` when a block is compiled (`Core.cpp`), and the
  dispatcher asserts `Is64BitMode == CTX->Config.Is64BitMode` with the
  message "Expected operating mode to not change at runtime!"
  (`OpcodeDispatcher.cpp`). Honouring `CS` inside one context means a
  per-block mode across the frontend, dispatcher and JIT of a translator we
  patch but do not fork. A second context in 32-bit mode is what the
  translator already offers, and it is the shape FEX's own WoW64 backend
  uses on Windows on ARM.
- Refinement (2026-09-02, after reading Wine 9.0's `dlls/wow64cpu/cpu.c`
  and the pinned translator's far-transfer lowering): Wine's own
  `wow64cpu.dll` already does every register marshal. It enters 32-bit
  code with `ljmp *(%r14)` (an indirect far jump to the 32-bit code
  selector) after loading the 32-bit registers into the 64-bit register
  file, and the 32-bit ntdll returns with the same instruction the other
  way (`ff 2d` far indirect jump to the 64-bit selector, written into a
  page below 4 GiB by `BTCpuProcessInit`). The translator lowers RETF and
  IRET as a RIP change plus a stored `cs_idx`, without a mode change. So
  the mode switch can live in the translator's far-transfer lowering: a
  far jump, far call, far return or iret whose new CS has the other
  bitness exits to BoxedVN with the full register file, and BoxedVN
  resumes the other context (64-bit or 32-bit) at that RIP with the same
  registers, stack and memory. Nothing replaces `wow64cpu.dll`; steps 2
  and 3 below reduce to that exit and the two contexts. The two selector
  values are the ones BoxedWine reports in a captured context (the
  backend reads `cs64_sel` from `RtlCaptureContext`).
- Prerequisite found the same day: Wine's loader must be named `wine`,
  not `wine64`, or a 32-bit image goes to `start.exe` instead of the
  in-process WoW64 path (`dlls/ntdll/unix/env.c`,
  `get_alternate_wineloader`). The lane launches through an alias now.
  `BTCpuProcessInit` also refuses to run when `wow64cpu.dll` is mapped
  above 4 GiB, so the loader's low-address mapping of WoW64 modules must
  succeed inside `KMemory64`'s low window.
- The far transfer is reached (device run 2026-09-02 19:08). After
  `set_thread_area` and the FS selector write, the 64-bit loader opened
  `i386-windows/ntdll.dll`, then `wow64.dll`, `wow64cpu.dll`, `wow64win.dll`
  and `win32u`; `wow64.dll`'s `init_image_mapping` protected a page of the
  32-bit ntdll back to `PAGE_EXECUTE_READ` (`mprotect(0x7bce8000, 0x1000, r-x)`
  from Wine's `NtProtectVirtualMemory`), which is the last 64-bit step before
  `BTCpuSimulate`. The next event is a page fault whose guest state is
  unambiguously 32-bit: `rip=0x7bd4321c` (32-bit ntdll's image range, below
  4 GiB), `rsp=0x22fc20` / `rbp=0x22fcd8` (the WoW64 32-bit stack), against a
  fault address of `0x78f7aed7ce` that no 32-bit instruction can form. Wine
  turned it into an access violation and the process left with
  `exit_group(0xC0000005)`. So the far jump is taken, the translator keeps
  decoding for the context's fixed 64-bit mode, and the first block of 32-bit
  code is misdecoded -- exactly the risk the phase names, now observed rather
  than predicted, and the gate it has to clear.
  Instrumentation added with this finding, because the run above named the
  boundary only by inference: every far jump, far call and far return notes
  itself while a small budget lasts, and the block compiled straight after one
  reports `BOXEDWINE_FEX64_FAR_TRANSFER_POST` with the target RIP, `cs`, `ss`,
  `ds`, `rsp`, the segment bases, the context's decode width and the first
  sixteen bytes at the target
  (`scripts/fex64-patches/fex-boxedwine-far-transfer-witness.patch`). The
  adapter's fault witness now also reads guest bytes through the kernel's page
  table when the identity alias does not cover the address, which is every
  address below 4 GiB, and prints `cs`/`ss` with the fault.
- **Decision reversed (2026-09-02, second reading of the pinned translator):
  shape (1), per-block decode mode in one context.** The earlier decision
  rested on a claim that reading the sources does not support: that the
  translator "picks its machine mode from `Config.Is64BitMode` when a block is
  compiled". It does not. `Decoder::DecodeInstructionsAtEntry` already reads
  the L bit of the descriptor the thread's CS selector names and stores the
  answer in `DecodedBlockInformation::Is64BitMode`; every width decision in
  the frontend reads that field, `Core.cpp` hands the same field to
  `OpDispatchBuilder::BeginFunction`, and every width decision in the
  dispatcher reads the member it sets. The decode mode is already per block.
  What made it look per context is two assertions -- one in the decoder, one
  in `BeginFunction` -- that the per-block answer equals
  `CTX->Config.Is64BitMode`, and the fact that this port's descriptor table
  gives *every* entry L=1, so the per-block answer could never differ.
  Nothing else consults the context's mode on the compile path: the JIT's
  register allocation, the memory alias, the host ABI and the exit lowering
  are all width-agnostic or take their width from the IR (`GetGPROpSize`),
  and `_InlineEntrypointOffset` already masks a 32-bit RIP to 32 bits.
  So shape (1) costs the two assertions, the one descriptor, and the
  descriptor-table read that a 32-bit block would otherwise emit for
  `mov seg, r/m` and for the far jump back -- against a second context, a
  replacement `wow64cpu.dll` and a trap page for shape (2). Implemented:
  `scripts/fex64-patches/fex-boxedwine-per-block-decode-mode.patch` plus the
  32-bit code descriptor in `BVNFEXBackend.mm`.
  Costs and limits recorded with the decision:
  - The lookup cache stays keyed by guest RIP alone; widening the key would
    reach into the dispatcher's emitted L1 probe. Instead the mode is
    recorded per guest code page at compile time and a page that changes
    bitness is invalidated before it is compiled again
    (`BOXEDWINE_FEX64_MODE_PAGE_CONFLICT`). Under WoW64 the 32-bit and 64-bit
    images occupy disjoint pages, so this is bookkeeping, not a hot path.
  - Segment bases for CS, SS, DS and ES are substituted as zero in both
    modes, because the descriptor table is host memory that a translated load
    cannot reach. That is exact for the flat WoW64 descriptors (0x23, 0x2b,
    0x33) and would be wrong for a non-flat one, which Wine does not create.
    FS and GS keep real bases: their writes trap to the host in both modes
    now, and the base comes from the descriptor the selector names.
  - The x87 stack optimisation pass is still built with the context's GPR
    size, so a 32-bit x87 memory operand whose base plus displacement crosses
    4 GiB would not wrap the way hardware does. No such address exists under
    the low alias.
  - Shape (2) is not deleted from this document: if the per-block mode proves
    unworkable it is still the fallback, and the notes below stand.
- The mode switch is taken and 32-bit code runs (device run 2026-09-02 20:21):
  `MODE_SWITCH rip=0x7bd65a00 cs=0x23 mode=32`, then several thousand bytes of
  32-bit ntdll across a three-frame call chain before a fault. The fault is a
  null-pointer read in 32-bit code (`mov eax,[ebx+0x14]` with EBX zero), not a
  segment or width defect. What made it fatal is signal delivery: the guest
  frame was built with CS still at the 32-bit code selector, so Wine's 64-bit
  handler was decoded as 32-bit code and re-faulted forever. Delivery now
  saves the interrupted CS/SS in the frame and enters the handler in the
  64-bit code segment, and `rt_sigreturn` restores what the frame names --
  which is also the mechanism Wine's `restore_context` uses to resume 32-bit
  code after an exception, so the return path exists for free. Witness:
  `BOXEDWINE_X64_SIGNAL_CS`.
- FS and GS bases in 32-bit blocks need nothing beyond what the translator
  already does: a 32-bit block loads `fs_cached`/`gs_cached` at GPR width and
  adds it to the offset in a 32-bit add, so the address wraps at 4 GiB before
  the low alias is applied. The host owns those two bases in both modes, and
  both opcodes that can write the selectors -- `mov fs/gs, r/m16` and
  `pop fs/gs`; FEX leaves LFS and LGS unimplemented -- now trap to it
  (`fex-boxedwine-host-served-segment-base.patch`).
- Shape (2) in BoxedVN terms (steps 2 and 3 superseded by the refinement):
  1. Context pair. `createFEXContext(FexGuestMode::X86_32, ...)` for the
     process, sharing `KMemory64` with its 64-bit context; the low-alias
     window and the fex32 window binding already exist for this.
  2. `wow64cpu.dll`. An x86-64 PE built with the probe toolchain, projected
     over Wine's `wow64cpu.dll` in the prefix, exporting the set the
     packaged Wine 9.0 module exports (read from libwine 9.0~repack-4build3
     amd64 on 2026-09-02): `BTCpuGetBopCode`, `BTCpuGetContext`,
     `BTCpuIsProcessorFeaturePresent`, `BTCpuProcessInit`,
     `BTCpuResetToConsistentState`, `BTCpuSetContext`, `BTCpuSimulate`,
     `BTCpuTurboThunkControl`, `__wine_get_unix_opcode`. It imports only
     `Wow64SystemServiceEx` from `wow64.dll` and `RtlWow64GetThreadContext`
     / `RtlWow64SetThreadContext`, `NtProtectVirtualMemory`,
     `RtlCaptureContext` and `RtlFindExportedRoutineByName` from ntdll.
     `wow64.dll` picks the module by name (`wow64cpu.dll`, with
     `xtajit.dll` as the other built-in choice), so the projection replaces
     the file and nothing else changes. `BTCpuSimulate` copies the
     `I386_CONTEXT` from the thread's WOW64 CPU area into a private-syscall
     request (the DXMT dispatcher's pattern) and asks the kernel to run the
     32-bit context; `BTCpuGetBopCode` and `__wine_get_unix_opcode` return
     the 32-bit stubs the 32-bit ntdll jumps to for a system call and a
     unix call, which under shape (2) are the trap addresses of step 3.
  3. Transition. The 32-bit ntdll leaves compat mode through
     `Wow64Transition`; the backend points it at a BoxedVN trap page in the
     low alias. The 32-bit context stops there, the kernel returns the
     32-bit registers to `BTCpuSimulate`, which calls
     `Wow64SystemServiceEx` (64-bit) and re-enters 32-bit with the result.
     No far transfer is ever executed by the translator.
  4. Exceptions. A fault in 32-bit code arrives in the 32-bit context;
     the kernel turns it into a return from `BTCpuSimulate` with an
     `EXCEPTION_RECORD`/`I386_CONTEXT` so `wow64.dll` raises it on the
     32-bit side (the existing `raiseSyncFault` plumbing supplies the
     record).
- `KMemory64` placement: 32-bit code needs its stack, heap and images below
  4 GiB. On iOS those addresses are served through the low alias
  (`include/guest_low_alias.h`, `K64_NATIVE_LOW_ALIAS_BASE`), which already
  exists for Wine's TEB and KUSER_SHARED_DATA. The mmap placement policy
  (`chooseGuestMmapPlacement`) needs a "32-bit process" mode that keeps
  automatic placements under 4 GiB.
- `syscall64.cpp`: WoW64 processes still enter the 64-bit syscall path (the
  Unix side is 64-bit), so no 32-bit Linux syscall ABI is required. The
  interpreter (`cpu64.cpp`) only runs forked children before exec and needs
  no 32-bit decoding.
- Signals and exceptions: a fault in 32-bit code must be delivered with a
  32-bit context (`WOW64_CONTEXT`) through Wine's WoW64 exception path.
  Reuse the existing `raiseSyncFault` plumbing; add the context shape.
  Done on the Unix side (2026-09-02): the guest signal frame carries the
  interrupted CS and SS, delivery switches to the 64-bit code segment so the
  handler is decoded as 64-bit code, and `rt_sigreturn` restores the frame's
  pair. Wine builds the `WOW64_CONTEXT` itself from there; nothing on this
  side has to know about it.

Gate (host): a freestanding fixture through the pinned translator simulator
(the same harness `scripts/test-fex-exit-dispatch-contract.py` and the
`fex-translator-probe` job use) in which a 32-bit context runs code that
touches a low-alias page and reaches the transition address, the host
reads the stopped 32-bit registers with truncation checked, and the 64-bit
context resumes afterwards. Gate (device): a 32-bit console program prints through the 64-bit
ntdll and exits 0.

### Phase 3: Windows and input for a 32-bit program

- The X11 driver stays 64-bit (`winex11.so` through the x86-64 bridge in
  `source/x11/x11bridge64.cpp`); user32 calls from 32-bit code are thunked
  by `wow64win.dll`. Expect no new bridge operations, but expect new
  `BOXEDWINE_X64_X11_UNIMPLEMENTED` markers from paths a real title takes
  that the probe did not (XGetImage, XShm, XShape are the known gaps).
- Verify keyboard and mouse delivery into a 32-bit window.

Gate (device): a 32-bit windowed program shows a window, moves it, and
receives input.

### Phase 4: Direct3D from 32-bit through DXMT

- Build DXMT's i386 PE thunks in `scripts/build-dxmt-x64-pe.sh` (a second
  meson cross file targeting i386) and stage them in
  `scripts/stage-x64-graphics-assets.sh` under `dxmt-i386/`.
- The DXMT dispatcher (`boxedwineDxmtUnixCall64`) already translates the
  parameter block and the rewritten unix sources translate nested pointers
  (`scripts/rewrite-dxmt-guest-pointers.py`). The WoW64 entries use 32-bit
  pointer fields (`UInt32ToPtr`); the rewrite must cover
  `__wine_unix_call_wow64_funcs` sites too, and the 32-bit pointers go
  through the low alias.
- Extend the graphics probe: a 32-bit build of the same Direct3D 11 cube.

Gate (device): the 32-bit cube renders its first frame through DXMT.

### Phase 5: Library validation and default flip

- Run every existing 32-bit title in the library on the WoW64 lane; record
  boundaries the way `PROGRESS.md` does today.
- Per-container engine selection in the app (`Runtime.swift` /
  `AppModel.swift`): "Legacy 32-bit" vs "FEX (WoW64)", defaulting to WoW64
  only once the library passes.
- Keep the legacy lane buildable and tested until no container uses it.

## Risks

- **Mode switching under FEX inside BoxedWine (phase 2).** Honouring `CS`
  changes in one context is implemented and unproven on device. The residual
  risks are named with the decision above: the RIP-only block cache (covered
  by the per-page mode record), the substituted zero segment bases, and the
  x87 pass's context-wide GPR size. If it proves unworkable the fallback is
  still a second context per thread, which is heavier and touches thread
  scheduling.
- **Low-alias pressure.** All 32-bit allocations must live below 4 GiB and
  are aliased; large 32-bit games can exhaust that space faster than under
  the legacy lane. Measure early with a real title.
- **Wine source.** A dual-arch Wine is not a distro package; building it in
  CI is slow, so a pinned prebuilt with recorded hashes is preferable.
- **DXMT 32-bit thunks.** The pinned fork carries them, but they have not
  been exercised on iOS.

## Non-goals

- Porting FEX into the 32-bit BoxedWine kernel (FEX32). The admission
  contract stays as a gate; no backend work follows it.
- Replacing SDL or BoxedWine with a native Wine launcher.
- Changing the legacy 32-bit lane while WoW64 is unproven.

## Tracking

Progress lands in `PROGRESS.md` per iteration, as now. This file is updated
when a phase gate is met or a decision above changes; the date and commit
of each gate go in the table below.

| Phase | Gate met | Commit |
|---|---|---|
| 0 | 2026-09-02 | see PROGRESS.md, "WoW64 phase 0" |
| 1 | | |
| 2 | decision reversed 2026-09-02 to shape 1 (per-block decode mode implemented); gate open | see PROGRESS.md, "Per-block decode mode" |
| 3 | | |
| 4 | | |
| 5 | | |
