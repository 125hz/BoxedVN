# Launching programs from the 64-bit desktop

Status: open problem, written 2026-09-03 for a fresh reader. Nothing here is
implemented yet. An investigation was running when this was written; treat its
findings as superseding anything below that it contradicts.

Goal: a user opens the 64-bit Wine desktop, browses to a program, double-clicks
it, and the program runs on the translator. Today it runs on the interpreter and
dies immediately.

## The observation

Device log `boxedvn-20260903-221042.log`, revision `fe082c36`. The session was
started with "Open 64-bit desktop". Three double-clicks on the same executable
produced three identical sequences:

```
BOXEDWINE_X64_FORK parent=43 child=45 parent_fex=0 child_fex=0 native=0
BOXEDWINE_X64_EXEC pid=45 path=/usr/lib/x86_64-linux-gnu/wine/wine fex=0 native=0
BOXEDWINE_X64_PROC_EXIT pid=45 parent=43 status=3221225477 group=1
```

`3221225477` is `0xC0000005`, an access violation. `fex=0` is the whole story:
the program ran on `CPU64`, the interpreter.

The same executable launched through "Run program" gets `fex=1` and reaches its
own startup dialog, which is much further.

Who holds the role in that session:

```
BOXEDWINE_X64_LAUNCH request=fex executable=/usr/lib/x86_64-linux-gnu/wine/wine
BOXEDWINE_X64_FEX_PROCESS pid=10 cmd=.../wine C:\windows\system32\explorer.exe /desktop=shell,800x600 winefile D:
```

The desktop shell claimed it at startup and never gives it up.

## Why only one process is translated

`KProcess::useFEX64` (`include/kprocess.h:443`) is the flag. It is set in one
place, `KProcess::start` (`source/kernel/kprocess.cpp:1152`), for the process the
app launches, and it is explicitly cleared for every child at fork
(`kprocess.cpp:2925-2926`, with the comment that exec replaces the address
space). `normalPlatformMultiThreaded.cpp:101,156` dispatches on it: true means
run the FEX backend, false means step the interpreter.

The underlying reason is memory. The translated process runs with the guest
address space mapped so that a guest address can be turned into a host address
with two arithmetic instructions, which is what
`scripts/fex64-patches/fex-boxedwine-low-address-alias.patch` emits and what
`include/guest_low_alias.h` documents. That mapping occupies fixed host address
ranges. Two guest processes cannot both own them.

**What has not been established, and is the first thing to check:** whether the
mapping is the *only* exclusive resource. Candidates to rule in or out are the
FEX context and its thread objects, the signal handlers the adapter installs,
the executable arena obtained at startup, and any static state in
`ios/runtime/src/BVNFEXBackend.mm` and `BVNFEXCPU64Adapter.mm`. Some of these may
be exclusive only by assumption. The answer decides whether the role can move at
all.

## Why this matters more than it looks

The desktop is the visible symptom, not the problem. The same limit applies to
any program that starts another program:

- a launcher that spawns the real executable
- an installer, and the program it installs
- a game that re-launches itself with different arguments

All of them put the interesting work in a child, and every child is interpreted.
Advising users to prefer "Run program" hides one case of this and leaves the
rest. Under the old IA-32 interpreter everything was equally slow, so launching
from the desktop cost nothing; under FEX the gap between the two paths is large
enough that the policy now decides whether a program is usable.

## Two designs

### A. Hand the role over at exec

The current owner releases the exclusive resource and the newly execed process
acquires it. Exec is the natural seam: the new process's address space is fresh
and nothing of the old one survives it.

Open questions:

- Can a running process be demoted to the interpreter, or only at a safe point?
  Its threads are inside translated code; they would have to be brought to a
  quiescent point and resumed on `CPU64` with converted state.
- What happens to threads blocked in a host call at the moment of transfer?
- Is the executable arena reusable by a different process, or does it have to be
  torn down and rebuilt?

This is the better outcome and the harder proof.

### B. Never give the role to the shell

Simpler, and probably enough. When the launch is the desktop, do not set
`useFEX64` on the shell. Reserve it for the first process the shell execs that
looks like a user program rather than Wine infrastructure.

- The shell does nothing performance-critical; it is a file manager.
- No live demotion is needed, so the hard proof in design A disappears.
- The rule has to distinguish a user program from infrastructure such as
  `wineboot`, `services.exe`, `winedevice.exe`, `rpcss`, `plugplay` and
  `explorer.exe` itself. A path-based rule is crude but auditable, and the
  witness below makes a wrong decision obvious.

Risk to check: with the shell interpreted, is the desktop still responsive
enough to use? It boots and paints on the interpreter today, but it has never
been the only interpreted thing in a session under load.

### Recommendation

Ship B first because it is provable, then attempt A, which also fixes the
launcher and installer cases that B does not.

## What to add regardless of design

A witness at every point the role is decided, in the existing style: which
process held it, which asked for it, whether it moved, and the reason it did
not. Without that, a wrong policy decision is invisible in the log, and the
current logging only reports the outcome (`fex=0`) rather than the choice.

## Related constraints, so they are not rediscovered

- Fork children cannot be translated even in principle while the parent lives,
  because both would need the same mapping. Only exec, which replaces the
  address space, is a candidate seam.
- Interpreted processes are not merely slow. `CPU64` implements a narrower
  instruction set, which is why the child dies with an access violation rather
  than running slowly. Recent fixes closed several gaps there (`int 0x80`, the
  segment-selector loads, the breakpoint traps), but it will never be complete.
- The glibc tunables applied at launch keep every process off the AVX paths, so
  interpreted helpers survive; that is what makes the desktop boot at all.

## Test that decides it

Open the 64-bit desktop, double-click a program, and read the log for:

```
BOXEDWINE_X64_EXEC pid=N ... fex=1
```

Anything else means the role did not move, and the new witness should say why.
