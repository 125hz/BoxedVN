# Audio for the 64-bit lane

The 64-bit lane has no audio device at all. Nothing in the guest can open one,
and nothing on the host is wired to receive samples from a 64-bit process.
This plan establishes what is actually in the packaged runtime, what the IA-32
lane already does, and the cheapest route that ends in sound, with a witness
for every step.

Scope note: this is the x86-64 lane only. The IA-32 lane's audio path is
described here because it is the thing being reused, not because it changes.

---

## 1. What the 03:13 session proves about audio today

The 64-bit run in which a program loaded its own audio runtime DLL from its
program directory and then stopped at its own error dialog is the reference.
Facts from that log, by line:

- `fmod64.dll` resolves out of the program's own directory: `DLL_SEARCH ...
  op=stat ... result=0` at 3202 and `op=open ... result=32` at 3203. The
  loader's subsequent probes of Wine's own module directories for the same
  name (3204-3223) are the normal builtin-lookup misses that follow every
  native module and are not failures.
- **Not one audio module is loaded, searched for, or mentioned anywhere in the
  session.** A case-insensitive grep of the whole log for `mmdevapi`,
  `dsound`, `xaudio`, `winepulse`, `winealsa`, `wineoss`, `winecoreaudio`,
  `libpulse`, `libasound` and `/dev/dsp` returns zero lines. The same grep over
  the 03:15 desktop-session log also returns zero.
- `winmm.dll` and `msacm32.dll` **are** loaded (in the block at 3020-3200),
  but only as part of the static import closure resolved before the program's
  entry point. Wine's `winmm` builds its device list lazily, by
  `CoCreateInstance(CLSID_MMDeviceEnumerator)`, and that would pull in
  `mmdevapi.dll` — which is never searched. So no code in the process ever
  asked for an audio device.
- Nothing else failed either. Every DLL that missed one directory resolved in
  another; there is no `ksyscall64: unimplemented syscall`, no ENOSYS, no
  hostcall refusal, and `GUEST_FAULT`/`NO_BACKING`/`NULL_CALL_SITE` all have a
  zero match count for the run.
- The dialog is the program's own: window `0x10057`, 330x298 with a 238x82
  client area, transient-for its main window, created and presented at
  4335-4357 (`X11_PRESENT window=0x10057 ... first=1`), after which the main
  thread parks in `__psynch_cvwait` (`FEX64_STALL ... state=waiting` at
  03:14:32) — a modal message loop, not a hang.

**Conclusion for that run: the failure is not audio.** Audio was never
attempted, so it cannot be what the program complained about. It is also not
input (no `dinput8`, `xinput*` or HID module is searched), and not a missing
Steam module (`steam_api64.dll`, `steamclient64.dll`, `GameOverlayRenderer64.dll`
and the program's DRM shim all resolve from the program directory with
`result=0`/`result=32`, lines 2952-3191). The one real environmental defect
visible in that window is fontconfig: `/etc/fonts/fonts.conf` reports "out of
memory" at its lines 86 and 91 and then `Cannot load config file` (3269-3288
for the program, and again for the desktop process at 3832-3845), so the guest
has no fontconfig-driven font backend and falls back to Wine's bundled `.fon`
bitmap faces.

What that run does establish for *this* plan is the starting position: **a
64-bit guest today gets no audio device, and would get none even if it asked.**
Everything below is about closing that, not about that dialog.

---

## 2. What the packaged amd64 Wine ships

`scripts/build-wine64-runtime-ci.sh` copies the distro's Wine module trees
wholesale:

```
cp -aL "${WINE_UNIX}"    "${STAGE}${WINE_MODULE_ROOT}/"   # x86_64-unix
cp -aL "${WINE_WINDOWS}" "${STAGE}${WINE_MODULE_ROOT}/"   # x86_64-windows
```

and seeds the native dependency closure from *every* `.so` in `x86_64-unix`:

```
SEEDS=("${WINE64}" "${WINE_SERVER}" /lib64/ld-linux-x86-64.so.2)
while IFS= read -r module; do SEEDS+=("${module}"); done < <(
    find "${WINE_UNIX}" -maxdepth 1 -type f -name '*.so' -print
)
```

Three consequences, and one gap:

1. Whatever unix audio drivers the Ubuntu amd64 package builds are already in
   the archive. Ubuntu builds `winealsa.so`/`winealsa.drv` and
   `winepulse.so`/`winepulse.drv`. It does **not** build `wineoss` — Wine's
   configure needs `<sys/soundcard.h>`, which Ubuntu does not ship; this
   repository's own `docs/How-To-Build-Wine-FileSystem.md` step 4 exists
   precisely because OSS headers have to be installed by hand before
   `wineoss.drv.so` will build. `winecoreaudio` is macOS-only and is not in a
   Linux package at all.
2. Because the seed set includes those drivers, their `DT_NEEDED`
   (`libasound.so.2` for winealsa, `libpulse.so.0` for winepulse) are pulled in
   by the `ldd` pass and staged. They are *present* but useless: see §3.
3. **Nothing checks any of this.** `scripts/validate-wine64-runtime.sh` has no
   audio assertion of any kind — its required lists are ntdll/kernel32/
   kernelbase, the winex11 driver pair, FreeType, the X11 client libraries, the
   NLS files and (optionally) winemetal. `scripts/build-wine64-runtime-ci.sh`
   likewise fails the build for a missing font backend or a missing 32-bit
   builtin but says nothing about audio.

**Step 0 of this plan is therefore a fact-finding step with a witness**, not an
assumption. See §5.0.

---

## 3. What each unix driver needs from the guest

| Driver | Guest library | Guest kernel/device | Viable here? |
|---|---|---|---|
| `winepulse.so` | `libpulse.so.0` | a running PulseAudio daemon on a unix socket at `$XDG_RUNTIME_DIR/pulse/native` | No. There is no pulse daemon in the rootfs and writing one is a far larger job than the audio itself. |
| `winealsa.so` | `libasound.so.2` | `/dev/snd/*` character devices, plus `/usr/share/alsa/alsa.conf` and the plugin tree | No, as shipped. `snd_pcm_open` walks ALSA's config and then opens a kernel device that BoxedWine does not emulate. Making it work means either emulating `/dev/snd` (a large, undocumented ioctl surface) or replacing `libasound.so.2` with a shim. |
| `wineoss.so` | none (uses raw syscalls) | `/dev/dsp`, `/dev/mixer` with the OSS ioctl set | **Yes** — and BoxedWine already emulates exactly those two devices. The one problem is that the Ubuntu package does not build this driver. |
| `winecoreaudio.so` | — | macOS CoreAudio HAL | No. Absent on iOS; `docs/ARCHITECTURE_IOS.md` §6 already records why. |

---

## 4. How the IA-32 lane does audio today

Not through any of Wine's Linux backends. BoxedWine emulates an OSS sound card
in its own kernel and hands the samples to SDL:

- `source/kernel/devs/devdsp.cpp` — `DevDsp`, a virtual character device
  implementing `SNDCTL_DSP_RESET/SPEED/STEREO/SETFMT/SETFRAGMENT/GETFMTS/
  GETOSPACE/GETCAPS/SETTRIGGER/GETOPTR/SETDUPLEX/GETODELAY` and the OSSv4
  `SNDCTL_ENGINEINFO`, plus `readNative`/`writeNative`/`waitForEvents`/
  `isWriteReady`.
- `source/kernel/devs/devmixer.cpp` — `/dev/mixer`.
- `source/kernel/devs/oss.h` — the OSS structures those two answer with.
- `platform/sdl/kdspaudio.cpp` — `KDspAudioSdl`, the backend: it opens an
  `SDL_AudioDeviceID` with `SDL_OpenAudioDevice(nullptr, 0, ...,
  SDL_AUDIO_ALLOW_ANY_CHANGE)`, converts with `SDL_AudioStream`/`SDL_AudioCVT`
  when the device does not accept the guest's format, and pushes with
  `SDL_QueueAudio`. `this->want.callback = nullptr` — it is a **push** model
  with no callback into guest memory.
- `source/sdl/startupArgs.cpp:667-669` registers `/dev/dsp`, `/dev/mixer` and
  `/dev/sequencer` as virtual files in the shared filesystem — the *same*
  filesystem the 64-bit lane mounts, so the nodes are already visible to a
  64-bit process.
- `platform/sdl/knativesystem.cpp:52-62` adds `SDL_INIT_AUDIO` whenever audio
  is allowed, and `source/sdl/main.cpp:107` passes `true` unconditionally.
  `KSystem::soundEnabled` is true unless `-nosound` is on the command line;
  the 03:13 launch line carries no `-nosound`.
- On the guest side the IA-32 lane runs a Wine built *with* OSS support, which
  is what `docs/How-To-Build-Wine-FileSystem.md` step 4 is for.

So the host half of a sound path is already built, already compiled into the
iOS target, and already initialised. Only the guest half is missing on this
lane.

---

## 5. Route

**Chosen: `wineoss` over BoxedWine's existing `/dev/dsp`.** It reuses a working
host mixer, needs no new hostcall, no new guest library, and no new ABI to keep
in sync with a Wine version. The cost is that the packaged Wine has to carry a
driver Ubuntu does not build, which is a packaging problem with a known
solution (build the OSS driver from the same Wine version's source with the OSS
headers installed, exactly as the 32-bit filesystem doc already prescribes).

**Fallback, if the OSS driver cannot be produced from the same Wine version:**
a guest-side `libasound.so.2` shim over a new hostcall `0x7fff0004`, built the
way `scripts/build-boxedwine-x64-x11.sh` builds the X11 shim
(`0x7fff0002` in `include/boxedwine_x64_x11_bridge.h`) and the Vulkan shim
(`0x7fff0003` in `include/boxedwine_x64_vulkan_bridge.h`). This is written down
as a fallback and not chosen because the surface is much larger: `winealsa.so`
uses `snd_pcm_*`, `snd_ctl_*`, `snd_config_*`, `snd_device_name_hint` and
`snd_pcm_hw_params_*` — well over a hundred entry points, several of which take
opaque structures whose size the shim would have to reproduce. The OSS route
needs zero new guest symbols.

### 5.0 Establish what is in the archive (before writing any code)

Add to `scripts/build-wine64-runtime-ci.sh` and
`scripts/validate-wine64-runtime.sh` an audio inventory that names what it
finds instead of assuming. For each of `winealsa`, `winepulse`, `wineoss`,
report whether `x86_64-unix/<name>.so` and `x86_64-windows/<name>.drv` are both
present, both absent, or half present (which is always a packaging bug), and do
the same for `x86_64-windows/mmdevapi.dll`, `dsound.dll`, `xaudio2_9.dll` and
`winmm.dll`.

*Witness:* one build-log line per driver,
`WINE64_AUDIO_DRIVER name= unix= pe= status=`, plus a summary
`WINE64_AUDIO_INVENTORY drivers= mmdevapi= dsound= xaudio2=`. This turns "we
think Ubuntu ships winealsa" into a recorded fact and immediately answers
whether the fallback route is even available.

### 5.1 Make the 64-bit lane able to drive `/dev/dsp`

`/dev/dsp` is already reachable: `openat` resolves it through the shared
virtual filesystem, `sys_write64` (`source/kernel/syscall64.cpp:537`) already
bounces the guest buffer into a host `std::vector<U8>` and calls
`fdesc->kobject->writeNative(buffer.data(), count)` — so PCM already flows —
and `poll`/`ppoll` (`syscall64.cpp:4890`) drive the open node's
`waitForEvents`, which `DevDsp` implements.

The one gap is `ioctl`. `syscall64.cpp:4060-4071` **already** routes a device
ioctl to `kf->openFile->ioctl64(cmd, a3, cpu->memory)` and falls through to
`-ENOTTY` when that returns `-ENOTTY`. But `ioctl64` has no override anywhere:
`source/io/fsopennode.h:51` defines it as `return (U32)-25;`. So every
`SNDCTL_DSP_*` from a 64-bit guest is answered `ENOTTY` and any OSS client
gives up at `SNDCTL_DSP_SPEED`.

**No change to `syscall64.cpp` is needed** — the bridge is in place and
correct. The work is entirely in `source/kernel/devs/devdsp.cpp` and
`devmixer.cpp`:

1. Factor the body of `DevDsp::ioctl(KThread*, U32 request)` into a
   width-agnostic core that takes the request code and a pair of
   read-dword/write-dword callables rather than reaching for
   `thread->memory` and the 32-bit `IOCTL_ARG1` stack slot directly.
2. Keep `ioctl()` as the 32-bit caller of that core, reading/writing through
   `thread->memory` exactly as now, so the IA-32 lane is bit-for-bit unchanged.
3. Add `DevDsp::ioctl64(U32 request, U64 argAddress, KMemory64* memory)` that
   calls the same core with `memory->readd(argAddress)` /
   `memory->writed(argAddress, v)`. Do the same for `DevMixer`.
4. Replace the `kpanic`/`kpanic_fmt` calls on unexpected lengths and values
   with a logged refusal returning `-EINVAL`. A guest handing an OSS device a
   value the emulation does not model must not take the emulator down; on this
   lane the caller is Wine, not a 1998 game, and the correct answer to an
   unsupported format is an error the driver can fall back from.

*Witness:* `BOXEDWINE_X64_DSP op= request=0x%x arg=0x%llx value= result=` on
the first call of each distinct request code, budgeted the way
`BOXEDWINE_X64_X11_BRIDGE ... first-call` is. A run that reaches sound shows
`SNDCTL_DSP_SETFMT`, `SNDCTL_DSP_SPEED` and `SNDCTL_DSP_STEREO` once each,
followed by silence from this witness and a steady stream of writes.

### 5.2 Get a `wineoss` driver pair into the archive

Build `wineoss.drv` + `wineoss.so` from the same Wine version the amd64 package
provides, with OSS headers installed, and stage them beside the existing
drivers. This is the same shape as the existing `--i386-pe-dir` and
`--dxmt-unixlib` inputs to `scripts/build-wine64-runtime-ci.sh`: a new
`--oss-driver-dir` whose two files are checked for the right architecture
(`is_elf64_x86_64` for the `.so`, PE for the `.drv`) and copied into
`x86_64-unix` and `x86_64-windows`.

The version has to match. Wine's PE/unix driver boundary
(`__wine_unix_call_funcs`) is a private, unversioned interface; a `wineoss.so`
from a different Wine version paired with this `mmdevapi.dll` is undefined
behaviour, not a degraded experience.

*Witness:* the §5.0 inventory line flips to
`WINE64_AUDIO_DRIVER name=oss unix=present pe=present status=ok`, and the
validator gates the shipped archive on it.

### 5.3 Point Wine's `mmdevapi` at it

`mmdevapi` picks its driver from `HKCU\Software\Wine\Drivers` value `Audio`,
falling back to its built-in order (`pulse`, `alsa`, `oss`, `coreaudio`) when
the value is absent. Leaving it absent means Wine tries pulse (no daemon) and
alsa (no `/dev/snd`) first and burns a startup delay on each. Set it
explicitly.

The mechanism exists: `setWineRegistryValue` in
`ios/support/src/wine_prefix.cpp:182`, already used for
`Software\Wine` `Version`, `Software\Wine\DllOverrides` and
`Software\Wine\Direct3D`. Add `Software\Wine\Drivers` `Audio` = `"oss"` on the
same pass, gated on the runtime actually carrying the driver (§5.0), so a
build without it does not force a driver that is not there.

*Witness:* the first 64-bit run afterwards searches for and opens
`x86_64-windows/wineoss.drv` and `x86_64-unix/wineoss.so` in `DLL_SEARCH` with
`result=32`/`result=13`, then opens `/dev/dsp`.

### 5.4 Make the host mixer audible on iOS

Two host-side items, both outside the guest:

1. **Audio session category.** Nothing in `ios/` configures `AVAudioSession` —
   a grep for `AVAudioSession`, `AVFAudio` and `setCategory` over `ios/`
   returns nothing. SDL2's iOS backend picks the *ambient* category by default,
   which is silenced by the hardware ring/silent switch and ducked by other
   apps. Set `SDL_HINT_AUDIO_CATEGORY` (present in the bundled
   `lib/sdl2/include/SDL_hints.h`) to `playback` before `SDL_Init`, so a user
   with the switch on still hears the guest.
2. **A user-visible sound toggle that means something.** `KSystem::soundEnabled`
   is already honoured throughout `kdspaudio.cpp` (it drains into a discard
   buffer at real-time pace rather than opening a device), and
   `StartUpArgs` already parses `-nosound`. Wire the app's setting to that
   argument for the 64-bit lane so "sound off" costs no device and no CPU.

*Witness:* `KDspAudioSdl::openAudio` already logs
`Failed to open audio: %s` on failure; add a success line naming the negotiated
format — `BOXEDWINE_AUDIO_DEVICE want=%d/%uHz/%uch got=%d/%uHz/%uch
converted=%d` — so one line says whether the device opened and whether the
guest's format is being converted.

### 5.5 End-to-end verification

A run is finished when, in order, the log shows: the audio inventory line from
§5.0; `wineoss.drv`/`wineoss.so` resolving; `/dev/dsp` opening;
`BOXEDWINE_X64_DSP` naming `SETFMT`/`SPEED`/`STEREO` once each;
`BOXEDWINE_AUDIO_DEVICE` with a negotiated format; and no
`BOXEDWINE_X64_DSP ... result=-22`. Sound itself is confirmed by the user, not
by the log.

---

## 6. Memory and threading constraints

These are the constraints the design above is shaped by; a different route has
to satisfy them too.

- **No host audio thread may touch guest memory.** SDL's coreaudio backend runs
  its callback on a real-time host thread that knows nothing about
  `KMemory64`'s page map, the identity-map window, or the page-cache
  generation. `KDspAudioSdl` avoids the problem entirely by setting
  `want.callback = nullptr` and pushing with `SDL_QueueAudio`: the guest's
  bytes are copied into a host-owned buffer on the *guest's* thread, inside the
  `write` syscall, before SDL ever sees them. Keep it that way. A pull-model
  backend would have to copy out of guest memory on a thread that may be
  running concurrently with a `munmap` or a fork, and there is no lock in the
  audio path that would make that safe.
- **The copy already happens, and is the right place for it.**
  `sys_write64` bounces up to 256 MB of guest bytes into a `std::vector<U8>`
  before calling `writeNative`. That is one memcpy per `write`, on the calling
  guest thread; a driver writing 4-8 KB fragments will not notice it.
- **`ioctl64` must read and write through `KMemory64`, never `thread->memory`.**
  The 32-bit `DevDsp::ioctl` reads its argument out of the 32-bit `KMemory`
  through `IOCTL_ARG1`. Calling that from a 64-bit guest would scribble a
  32-bit address — which is exactly why `syscall64.cpp`'s ioctl case was
  written to route through a separate `ioctl64` in the first place, and why
  step 5.1 refactors rather than reuses.
- **`/dev/dsp` state is per open file, and processes fork.** `DevDsp` holds one
  `KDspAudio` per open node. A guest that forks after opening the device gets a
  shared node; the 64-bit lane's `KProcess::clone` gives a child its own
  `KMemory64` but the descriptor table is inherited as usual. The device does
  not need to change for this, but any new state added to `DevDsp` has to
  tolerate two processes writing the same node.
- **Blocking must go through `waitForEvents`, not a host sleep.** `DevDsp`
  already reports write-readiness through `isWriteReady()` and parks on
  `BOXEDWINE_CONDITION`; a driver polling the fd is served by
  `syscall64.cpp`'s `poll`/`ppoll` case. Anything that slept on a host thread
  instead would hold the emulator's scheduler slot.
- **The 50 ms drain timer runs on SDL's timer thread.** `kdspaudio.cpp`
  installs `SDL_AddTimer(50, drainTimerCb, nullptr)` and that callback touches
  the device list and calls `SDL_CloseAudioDevice`. It does not touch guest
  memory and must not start doing so.

---

## 7. What this plan explicitly does not do

- It does not add a Vulkan-style hostcall for audio. The fallback in §5 records
  what that would cost if the OSS driver cannot be produced.
- It does not attempt PulseAudio or ALSA emulation in the guest kernel.
- It does not address MIDI. `KNativeAudio` on iOS is
  `KNativeAudioSDL` with MIDI off (`docs/ARCHITECTURE_IOS.md` §6), and
  `/dev/sequencer` is out of scope here.
- It does not address the fontconfig failure recorded in §1, which is a
  separate packaging defect in the same runtime.

---

## 8. Implementation status

### Landed

**§5.1, the guest kernel.** `DevDsp::ioctl64` and `DevMixer::ioctl64` exist and
answer the OSS set `dlls/wineoss.drv/oss.c` actually issues, which reading the
Wine 9.0 source narrowed to a much smaller list than the plan assumed: on
`/dev/mixer` only `SNDCTL_SYSINFO` and `SNDCTL_AUDIOINFO` (plus the
`SOUND_MIXER_READ_*`/`WRITE_*` pair the `aux` device uses), and on `/dev/dsp`
only `SNDCTL_ENGINEINFO`, `SETFMT`, `SPEED`, `CHANNELS`, `GETOSPACE` and
`GETISPACE`. The rest of the set (`RESET`, `SYNC`, `POST`, `GETBLKSIZE`,
`GETFMTS`, `GETCAPS`, `SETTRIGGER`/`GETTRIGGER`, `GETOPTR`, `GETODELAY`,
`SETFRAGMENT`, `NONBLOCK`) is implemented anyway, because an OSS client that
gets `ENOTTY` from one of them has no way to tell a device that will not do it
from a device that is not there.

Three things came out of that reading that the plan did not have:

- **`oss_test_connect` is the gate, not `oss_create_stream`.** It opens
  `/dev/mixer`, issues `SNDCTL_SYSINFO`, and returns `Priority_Unavailable`
  unless `sysinfo.version[0]` is in `'4'`..`'9'` and `versionnum` comes back
  without its top bit. `mmdevapi` skips a driver at that priority, so the
  mixer answering `SNDCTL_SYSINFO` correctly is what decides whether any of
  the rest is ever reached. The emulation already answered `"4.0.0a"` and
  `0x040000`, so it passes -- but nothing had ever checked, and it is now the
  first thing `BOXEDWINE_X64_MIXER` reports.
- **`GETOSPACE` has to be self-consistent, not just plausible.**
  `oss_write_data` takes `oss_bufsize_bytes` as `fragstotal * fragsize` and
  then computes `(oss_bufsize_bytes - bi.bytes)` as an *unsigned* frame count.
  The IA-32 path reports `bytes` as the raw capacity while `fragstotal` is
  the capacity divided by the fragment size, so `bytes` can exceed
  `fragstotal * fragsize` and that subtraction underflows into a multi-gigabyte
  write budget. `ioctl64` reports whole fragments, which keeps
  `bytes <= fragstotal * fragsize` by construction.
- **The host voice has to be open before `GETOSPACE` is answered.** BoxedWine
  opens its SDL device lazily on the first `write`; wineoss asks for the
  buffer geometry immediately after `SETFMT`/`SPEED`/`CHANNELS` and before any
  write, so the lazy path would have described `KDspAudio`'s 11025/mono/U8
  defaults rather than the format just negotiated. `ioctl64` opens the voice
  first.

The IA-32 `ioctl()` path keeps its own reads and writes through
`thread->memory` at `IOCTL_ARG1`. The one behavioural change on that lane is
the plan's §5.1 item 4: the four `kpanic`/`kpanic_fmt` calls on an unexpected
length or value are now a logged `-EINVAL`. The `oss_audioinfo` and
`oss_sysinfo` writers are shared between the two lanes
(`source/kernel/devs/ossioctl.h`), which also fixed a field walk both devices
had wrong in compensating ways -- `handle` written as 64 bytes and `song_name`
as 32 in the mixer, `devnode` as 16 in both -- leaving `next_play_engine` and
`next_rec_engine` landing inside `devnode`'s tail. Nothing Wine reads sat past
the damage; the walk now follows `oss.h` field for field.

**§5.2 scaffold.** `scripts/build-wine64-oss-driver.sh` builds the pair from
the upstream Wine tarball matching the installed package's version, supplying
the `<sys/soundcard.h>` Ubuntu lacks as the one-line wrapper over
`<linux/soundcard.h>` that glibc used to ship -- and *checking*, by compiling a
probe, that the header behind it really declares `oss_sysinfo`,
`oss_audioinfo`, `SNDCTL_SYSINFO`, `SNDCTL_AUDIOINFO` and `SNDCTL_ENGINEINFO`
before configuring anything. `--oss-driver-dir` stages the result;
`.github/workflows/build-ios.yml` runs the driver build as a non-fatal step and
passes the directory on only when both halves exist.

**§5.2, two bugs that made the step unbuildable.** The first CI run of that
step (`33802614807`) stopped at the probe, and reading why turned up a second
failure waiting behind it. Both are fixed.

- **`<linux/soundcard.h>` is OSSv3, not OSSv4.** The script's premise was that
  the kernel header `linux-libc-dev` installs *is* the OSSv4 header. It is
  not. It carries the `SNDCTL_DSP_*` ioctls and nothing else: no
  `oss_sysinfo`, no `oss_audioinfo`, no `SNDCTL_SYSINFO`, `SNDCTL_AUDIOINFO`
  or `SNDCTL_ENGINEINFO`. Those come from the OSS4 project's own
  `<sys/soundcard.h>`, and they are precisely the interface
  `dlls/wineoss.drv/oss.c` enumerates devices with, so the probe was right to
  fail -- it was the premise that was wrong. Ubuntu noble does package the
  real header, as **`oss4-dev`** (universe, `4.2-build2020-1ubuntu3`). It
  installs it *as* `/usr/include/linux/soundcard.h` and `dpkg-divert`s the
  kernel's copy to `soundcard.h.oss3` in its `preinst`, so it coexists with
  `linux-libc-dev` rather than conflicting with it, and the existing
  `sys/soundcard.h` wrapper needs no change to find it. The workflow step now
  installs it. `--oss-include-dir` covers a host that has a real
  `<sys/soundcard.h>` somewhere else, such as an OSS4 install under
  `/usr/lib/oss/include` -- which is, not coincidentally, the directory Wine's
  own `configure` adds to `CPPFLAGS` while probing.
- **The post-`configure` check grepped for a define Wine never emits.** It
  looked for `HAVE_SYS_SOUNDCARD_H` in `include/config.h`. Wine 9.0's
  `configure.ac` reaches the header through
  `AC_CHECK_HEADER([sys/soundcard.h], [ACTION-IF-FOUND])`, and supplying an
  action *replaces* autoconf's default action -- the one that would have
  defined that name. `include/config.h.in` has no such entry at all, so the
  grep would have failed on every tree, with or without a header, and killed
  the step immediately after a `configure` that had actually succeeded. The
  check now greps `HAVE_OSS_SYSINFO_NUMAUDIOENGINES`, which is what
  `AC_CHECK_MEMBERS([oss_sysinfo.numaudioengines])` records and is the real
  OSSv4 gate: `WINE_NOTICE_WITH` turns `enable_wineoss_drv` off when it fails.

The probe was widened at the same time, from the five enumeration names to the
whole set the driver compiles against -- `audio_buf_info`, `count_info`,
`struct synth_info`, `struct midi_info` and `struct sbi_instrument` for
`ossmidi.c` and `midipatch.c`, the `SNDCTL_SEQ_*`/`SNDCTL_SYNTH_INFO`/
`SNDCTL_MIDI_INFO` requests, and `oss_sysinfo.numaudioengines` itself, so that
anything that passes the probe and then fails `configure` is a Wine change
rather than a header surprise.

**§5.1, the OSSv4 numbering the guest had wrong or missing.** Reading the OSSv4
header against `DevDsp::ioctl64` turned up one mis-decoded request and a set of
unanswered ones:

- `('P', 16)` is **both** `SNDCTL_DSP_SETTRIGGER` and `SNDCTL_DSP_GETTRIGGER`;
  OSSv4 separates them by direction alone (`__SIOW` versus `__SIOR`), not by
  number. The switch keyed on the code alone, so a get was answered with
  nothing written back, and `('P', 17)` -- which is `SNDCTL_DSP_GETIPTR` and
  wants a three-field `count_info` -- was being answered with a one-int
  trigger mask. Both now decode on the direction bit and on the real number.
- Added, all as the honest answer for a single output-only device:
  `GETPLAYVOL`/`SETPLAYVOL` (`0x5018`), `GETERROR` (`0x5019`), `COOKEDMODE`
  (`0x501E`), `HALT_INPUT`/`HALT_OUTPUT` (`0x5021`/`0x5022`),
  `CURRENT_IPTR`/`CURRENT_OPTR` (`0x5023`/`0x5024`), `POLICY` (`0x502D`) and
  `SNDCTL_SYSINFO` on the dsp node (`0x5801`, which OSSv4 answers on any OSS
  node and not only on `/dev/mixer`).
- `DevMixer::ioctl64` now answers `SNDCTL_ENGINEINFO` as well as
  `SNDCTL_AUDIOINFO`, and stops answering the six `'M'`-group numbers that are
  not channels with a channel level: `SOUND_MIXER_INFO` (101) gets a real
  `mixer_info`, and `STEREODEVS`/`CAPS`/`RECMASK`/`DEVMASK`/`RECSRC`
  (`0xfb`..`0xff`) get bit masks -- the six channels Wine's `aux` device
  enumerates for the two device masks, zero for the rest.
- Anything still unmodelled names itself once per request code, on both nodes:
  `BOXEDWINE_X64_OSS_IOCTL node=/dev/dsp nr=0x… status=unimplemented`. It is
  emitted only for `-ENOTTY`, which is the only result that means "not
  modelled" -- every other refusal here is a deliberate answer to a request
  that *is* modelled -- and on `/dev/mixer` only for the OSS request groups,
  so `FIONBIO` and `FIONREAD` do not fill the log with witnesses for requests
  that were never this device's to answer.

None of this was needed by `wineoss` itself, which -- as §5.1 above records --
issues a much shorter list. It is there for the same reason the rest of the set
is: an OSS client that gets `ENOTTY` cannot tell a device that will not do
something from a device that is not there.

**§5.0 inventory.** Both `scripts/build-wine64-runtime-ci.sh` and
`scripts/validate-wine64-runtime.sh` report
`WINE64_AUDIO_DRIVER name= unix= pe= status=` per driver and one
`WINE64_AUDIO_INVENTORY` summary, and both fail on a half-present pair.

**§5.3 registry.** `configureX64AudioDriver` in `source/sdl/startupArgs.cpp`
sets `HKCU\Software\Wine\Drivers` `Audio` = `"oss"` in the prefix's `user.reg`,
gated on both halves of the driver being present in the mounted module tree,
and written through a temp file and a rename.

**§5.4 host device.** `SDL_HINT_AUDIO_CATEGORY` is set to `playback` before
`SDL_OpenAudioDevice`, so the ring/silent switch no longer mutes the guest, and
`openAudio` now logs `BOXEDWINE_AUDIO_DEVICE` on success as well as failure.

### Not landed

**The CI driver build has never completed.** It has now run once and failed at
its first check, for the two reasons above; both are fixed, but a Wine build
from source has still never finished in this workflow. That is why the step
stays `continue-on-error` and why the validator's hard requirement stays behind
`BOXEDVN_REQUIRE_WINE64_OSS=1` rather than always on. Two questions are left
for the next run's log, and the fixes do not answer either of them: whether
`configure` gets far enough to record `HAVE_OSS_SYSINFO_NUMAUDIOENGINES`, and
whether `make dlls/wineoss.drv` produces a real PE `wineoss.drv` rather than
the fake PE module Wine emits without a mingw cross compiler. Arm the gate once
a run has produced both halves.

**Capture is not implemented.** `SNDCTL_DSP_GETISPACE` answers an empty input
buffer rather than `-ENODEV`, because refusing makes wineoss log an error every
period, but there is no recording device behind it.

**The fallback in §5 was not needed and was not built.** The ALSA shim over a
new hostcall stays written down and unimplemented.
