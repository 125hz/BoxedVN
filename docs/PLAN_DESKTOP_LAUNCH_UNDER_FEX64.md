# Launching programs from the 64-bit desktop

Goal: a user opens the 64-bit Wine desktop, browses to a program, double-clicks
it, and the program runs on the translator.

Status: **design B shipped at `957383ad`**, and a device capture proves the
first program double-clicked from the desktop is translated. Two defects behind
it did not survive that capture and are fixed here: the role was never given
back when the program exited, and a 32-bit program started from the desktop
reached a Direct3D 9 the desktop lane had not been told to project. What is left
is written at the end, as what is left rather than as a design.

## What was wrong

Device log `boxedvn-20260903-221042.log`, revision `fe082c36`. The session was
started with "Open 64-bit desktop". Three double-clicks on the same executable
produced three identical sequences:

```
BOXEDWINE_X64_FORK parent=43 child=45 parent_fex=0 child_fex=0 native=0
BOXEDWINE_X64_EXEC pid=45 path=/usr/lib/x86_64-linux-gnu/wine/wine fex=0 native=0
BOXEDWINE_X64_PROC_EXIT pid=45 parent=43 status=3221225477 group=1
```

`3221225477` is `0xC0000005`, an access violation. `fex=0` was the whole story:
the program ran on `CPU64`, the interpreter. The desktop shell (pid 10) had
claimed the translated role at startup, made no Direct3D call all session, and
never gave it up.

## Why only one process can be translated

`KProcess::useFEX64` (`include/kprocess.h`) is the flag, and
`normalPlatformMultiThreaded.cpp` dispatches on it: true means run the FEX
backend, false means step the interpreter.

The exclusive resource, established since this document was first written and
now recorded in `source/kernel/kprocess.cpp` above `translatorRoleTryTake`, is
the identity mapping and nothing else. `KMemory64` builds three host windows at
compile-time constant addresses -- the low alias, the image lane and Wine's
relocated top-down arena -- and FEX's translated code turns a guest pointer into
a host pointer by arithmetic into them. Two address spaces cannot both hold
those windows.

Everything else that was suspected of being exclusive is not.
`ios/runtime/src/BVNFEXBackend.mm` keys `gLiveProcesses` on `KProcess*`, so each
translated process gets its own FEX context, dispatcher, JIT and signal
delegator; the host fault handlers are installed once with `std::call_once` and
chain to whatever was there before; the dispatcher's escape state is
thread-local; and the translated-code pool is shared by construction. That
answers the question this document opened with, and it is what makes any
placement rule possible at all.

## Design B, as shipped (`957383ad`)

The role is no longer seeded on a launch whose command line names Wine's own
infrastructure. It is claimed at `execve`, by the first process whose arguments
name a top-level Windows program, at the one moment where a process has released
one address space and not yet built the next.

- `argsNameWineInfrastructure` -- the names Wine's own processes go by
  (`wineboot.exe`, `services.exe`, `winedevice.exe`, `plugplay.exe`,
  `explorer.exe`, `rpcss.exe`, `start.exe`, `winefile.exe`, `wineserver`,
  `conhost.exe`, `winemenubuilder.exe`). None of these ever claims.
- `argIsTopLevelWindowsProgram` -- an absolute Windows path to a `.exe` outside
  the Windows directory. Deliberately narrower than "any exec", because Wine
  re-execs its loader constantly and the desktop's own command line names
  `explorer.exe` by absolute path too.
- `CPU64::backendHandoff` -- `CPU64::run()`'s only exit was `yield`, which ends
  the host thread rather than returning to the scheduler. A process that takes
  the role mid-flight needs a second way out, or the interpreter goes on
  stepping the image the loader just built. Both interpreter loops honour it and
  every dispatcher clears it before running, so it cannot latch.

### What the device log proved

`boxedvn-20260903-230131.log`, a desktop session. The shell defers, every Wine
helper declines, and the program the user double-clicked takes the role at its
own exec:

```
BOXEDWINE_X64_TRANSLATOR_ROLE pid=10 action=seed holder=0 moved=0 reason=wine-infrastructure-defers-to-first-program-exec
BOXEDWINE_X64_TRANSLATOR_ROLE pid=12 action=claim holder=0 moved=0 reason=not-a-top-level-program
BOXEDWINE_X64_TRANSLATOR_ROLE pid=45 action=claim holder=0 moved=1 reason=top-level-program-took-free-role
BOXEDWINE_X64_EXEC_REMAP pid=45 fex=1 native=1
```

`fex=1 native=1` is the pair that matters: the process is marked translated and
it has the identity mapping. Everything between the shell's launch and that line
-- wineboot, services, winedevice, the file manager, the loader re-execs -- ran
interpreted and reached the point where the user could double-click, which is
the answer to design B's stated risk.

The same log also proved something the design did not anticipate, below.

## What is fixed here

### The role was never given back

Same log, further down. pid 45 exits cleanly, and the next two double-clicks are
refused:

```
BOXEDWINE_X64_PROC_EXIT pid=45 parent=43 status=0 group=1
BOXEDWINE_X64_TRANSLATOR_ROLE pid=49 action=claim holder=45 moved=0 reason=role-held-by-live-owner
BOXEDWINE_X64_PROC_EXIT pid=49 parent=47 status=3221225501 group=1
BOXEDWINE_X64_TRANSLATOR_ROLE pid=53 action=claim holder=45 moved=0 reason=role-held-by-live-owner
BOXEDWINE_X64_PROC_EXIT pid=53 parent=51 status=3221225501 group=1
```

`0xC000001D` is an illegal instruction: the interpreter's narrower instruction
set, exactly as before. And pid 45 was not a live owner -- it had been gone for
a hundred lines.

The release was tied to `~KProcess`, on the reasoning that the role must not
outlive the unmapping of the windows. That reasoning is right and the placement
was wrong, because `~KProcess` is later than it reads as. `KProcess::exitgroup`
does not destroy a `KProcess`: it kills the threads, runs `cleanupProcess` and
leaves the object in `KSystem::processes` as a zombie for `waitpid` to collect.
A zombie's `KMemory64` is still mapped, so the windows -- and the role with them
-- outlive the program for as long as nobody reaps it, which for a program
started from the desktop is indefinitely.

The teardown now happens in `KProcess::deleteThread`, when the owner's last
thread has been destroyed:

- it is the last moment at which any thread of the process could have executed a
  guest instruction, which is what makes unmapping safe -- the same argument
  that makes the claim at `execve` safe;
- it is after `delete thread`, because `~KThread` writes the guest's
  `clear_child_tid` through this process's `KMemory64` and declines to delete a
  `CPU64` that is still the process's own;
- it covers every ending, because every ending funnels through `exitgroup`: a
  guest `exit_group`, a fatal signal turned into one by `kfatalProcessExit64`, a
  kill from another process, and the zombie any of them leaves behind.

`~KProcess` keeps its release as a backstop; both deletes there are null-safe,
so it is a no-op once `deleteThread` has run. A session in which
`reason=owner-address-space-destroyed` is the line that fires is a session where
an owner was destroyed without its last thread passing through `deleteThread`,
and that is worth knowing.

### A holder that has already exited is not a live owner

`execve` now tries `reclaimTranslatorRoleFromExitedHolder` before it asks to
take. It looks the holder up by id in the process table -- never by a kept
pointer, which would be a use-after-free the moment it was collected -- confirms
it is the registered holder, and requires the two facts that make a holder dead:
`isTerminated()` and no surviving thread. Then it runs the same teardown.

This is the only transfer from another process that can be proven safe, and it
is what makes a second double-click work in the ordering the device log actually
showed, where the first program had already exited when the second one asked.

### The claim witness now says what blocked it

```
BOXEDWINE_X64_TRANSLATOR_ROLE pid=N action=claim holder=H moved=M reclaimed=R blocking=B holder_rel=REL reason=...
```

`blocking` is who still holds the role after the reclaim had its chance, and
`holder_rel` is `self`, `ancestor`, `unrelated` or `unknown` -- the relation of
that process to the one asking, from a bounded walk up the parent chain. A
launcher that is still running while the program it started execs reads as
`holder_rel=ancestor`, and that field is the only evidence that says whether
launcher chains happen in a real session or are a theoretical worry.

### Environment parity between the two launch paths

The app has two doors: `launchX64Program`, which names one executable, and
`launchX64Desktop`, which names the shell. A program must not behave differently
depending on which it came through. Comparing them:

| Setting | Program launch | Desktop launch |
| --- | --- | --- |
| `WINEDEBUG`, `WINEDLLOVERRIDES`, `DXMT_LOG_LEVEL`, `DXMT_LOG_PATH` | `X64Runtime.environment` | same |
| `WINEPREFIX`, `WINEARCH`, `WINEDLLPATH`, `LD_LIBRARY_PATH` | runtime defaults | same |
| DXMT module directory (`-x64modules`) | named outright | falls back to the working directory, which is that same directory |
| Working directory | the program's own folder | the shell sets it per program |
| 32-bit Direct3D 9 renderer | DXVK, when the PE header says i386 | **was missing** |

The first two rows are session-wide: the values are passed to the emulator once,
as `-env`, so every guest process inherits them. The device log confirms it --
`sys_execve64: pid=49 ... envc=24` carries `WINEDLLOVERRIDES` into a process
three forks deep from the shell.

The working directory is the shell's to set, and it does. From the same capture,
for the program the user double-clicked:

```
BOXEDWINE_X64_GETCWD pid=45 cwd='/home/username/.wine64/dosdevices/d:/<program folder>'
```

So that is per-program by construction, and nothing in the desktop launch should
try to carry it.

The last row was a real gap. The app asks for DXVK's 32-bit d3d9 only when it has
read the program's PE header and found i386 -- and a launch that opens the
desktop names no program at all, because the user picks one afterwards. A 32-bit
program started from the desktop therefore reached wined3d, which needs OpenGL or
Vulkan, has neither here, and returns `E_FAIL` into a message box, while the
identical program started from the app's menu reached DXVK and ran.

The fix is in `BVNLaunchArguments.cpp`: the translated lane now defaults
`BOXEDVN_WOW64_D3D9=dxvk` when the caller did not set it, so it carries both
renderers and the user's choice of program decides which one is used. It costs a
64-bit program nothing -- `projectX64WineDxvkD3d9` writes into the prefix's
`syswow64` only, which no 64-bit image loads from, and it reports and returns
when the DXVK tree is not staged. A caller that set the variable still wins.

## Is the interpreted shell usable?

Yes, on the evidence there is. Design B's stated risk was that the desktop might
not be responsive enough once it was the interpreted thing in the session, and
the 2026-09-03 23:01 capture answers it: `explorer.exe` created its desktop and
shell windows, `winefile` opened on D:, the pointer events reached the guest, and
three double-clicks each started a program. Nothing in the log shows the shell
short of CPU -- no reentry storm on its pids, no gap between a click being queued
and the fork it caused.

That is not a claim about how it feels under load, which no log can answer.
Should it become a problem, the smallest mitigation is not to translate the shell
-- that is the defect this whole document exists to fix -- but to stop it
repainting when nothing changed. The desktop is a static background with a file
list on it; the cost of an interpreted shell is proportional to how often it
redraws, not to how long it is open.

## What is still open

1. **A launcher that outlives the program it starts.** The role is claimed by
   whichever top-level program execs first, and a launcher is a top-level
   program. If it stays alive -- most do, waiting on the game -- the game execs
   into `reason=role-held-by-live-owner` and runs interpreted. The exited-holder
   reclaim does not help, because the exec happens while the launcher is still
   running. `holder_rel=ancestor` in the log is what identifies this case.

2. **Design A, taking the role from a live owner, remains impossible** and the
   reason has not changed since it was first written: an idle owner is a live
   owner. Its guest registers and stack hold host addresses inside the identity
   windows, its translated blocks have those addresses compiled into them, and it
   resumes into both. Narrowing the rule to "the owner is idle, and the process
   asking is its descendant" does not change any of that -- idleness is not a
   property of the address space. The only safe subset of A is "the owner has
   exited", which is implemented, and it is a smaller thing than A was.

   What would make more of A possible is a way to demote a live owner: quiesce
   every one of its threads at a translated-block boundary, convert its state to
   `CPU64`'s register file, discard its compiled blocks, and rebuild its address
   space sparsely. Each of those is a project, and the last one is the one nobody
   has shown is possible -- a sparse rebuild has to move every mapped page out of
   the windows while the guest's own pointers to them stay valid.

3. **An installer and the program it installs**, and a program that re-launches
   itself with different arguments, are the same shape as (1) when the first
   process lives, and are already covered when it exits.

4. **Two programs at once.** Only one is translated, by construction. The second
   gets `role-held-by-live-owner` and says so. There is no plan to change this.

5. **The reclaim's timing is not free.** `reclaimTranslatorRoleFromExitedHolder`
   destroys another process's address space on the exec path of the process
   asking. That work is bounded by the dead process's mapping count, but it is
   done while the new program is starting, so a very large exited program could
   make the next launch visibly slower to begin. Nothing has measured it.

## Test that decides it

Open the 64-bit desktop, double-click a program, quit it, double-click another,
and read the log for:

```
BOXEDWINE_X64_TRANSLATOR_ROLE pid=N action=claim ... moved=1
BOXEDWINE_X64_EXEC_REMAP pid=N fex=1 native=1
BOXEDWINE_X64_TRANSLATOR_ROLE pid=N action=release ... reason=owner-last-thread-gone
BOXEDWINE_X64_TRANSLATOR_ROLE pid=M action=claim ... moved=1
```

The third line is the one that was missing. If it does not appear and the fourth
says `role-held-by-live-owner` with `blocking=N` for a pid that has already
logged `BOXEDWINE_X64_PROC_EXIT`, the teardown did not run, and the next thing
to look at is whether that process's last thread reached
`KProcess::deleteThread` at all.

## Related constraints, so they are not rediscovered

- Fork children cannot be translated even in principle while the parent lives,
  because both would need the same mapping. Only exec, which replaces the
  address space, is a candidate seam.
- Interpreted processes are not merely slow. `CPU64` implements a narrower
  instruction set, which is why an interpreted program dies with an access
  violation or an illegal instruction rather than running slowly. Recent fixes
  closed several gaps there (`int 0x80`, the segment-selector loads, the
  breakpoint traps), but it will never be complete.
- The glibc tunables applied at launch keep every process off the AVX paths, so
  interpreted helpers survive; that is what makes the desktop boot at all.
- The rules here are pinned by `scripts/test_x64_translator_role_transfer.py`,
  which is source-level on purpose: building the emulator needs the iOS
  toolchain, and there is no host here on which a 64-bit guest can run.
