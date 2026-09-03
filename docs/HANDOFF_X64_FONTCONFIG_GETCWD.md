# The 64-bit lane's fontconfig failure was `getcwd`

Every 64-bit Wine session printed the same three lines the moment winex11/gdi
started enumerating fonts:

```
Fontconfig error: "/etc/fonts/fonts.conf", line 86: out of memory
Fontconfig error: "/etc/fonts/fonts.conf", line 91: out of memory
Fontconfig error: Cannot load config file from /etc/fonts/fonts.conf
```

It is not a memory problem, and lines 86 and 91 are not the `<dir>`/`<cachedir>`
elements the message suggests.

## What lines 86 and 91 actually are

Ubuntu 24.04 ships fontconfig 2.15.0, and its `/etc/fonts/fonts.conf` carries
two Debian-specific blocks that upstream's `fonts.conf.in` does not:

```
83	<selectfont>
84		<rejectfont>
85			<glob>*.dpkg-tmp</glob>
86		</rejectfont>
87	</selectfont>
88	<selectfont>
89		<rejectfont>
90			<glob>*.dpkg-new</glob>
91		</rejectfont>
92	</selectfont>
```

Lines 86 and 91 are the two `</rejectfont>` end tags. Fontconfig's
`FcParseAcceptRejectFont` (`src/fcxml.c:2951`) runs there, and the only way it
reports "out of memory" is `FcConfigGlobAdd` returning false. In 2.15.0 that
function (`src/fccfg.c:2914`) canonicalises the glob first:

```c
FcChar8 *realglob = FcStrCopyFilename (glob);
if (!realglob)
    return FcFalse;
```

`*.dpkg-tmp` is a relative path, so `FcStrCopyFilename` -> `FcStrCanonFilename`
(`src/fcstr.c:1210`) takes the branch that calls **`getcwd()`**, and a NULL
return propagates out as "out of memory". `FcConfigMessage` with
`FcSevereError` also sets `parse->error`, which is what turns two element-level
complaints into `Cannot load config file from /etc/fonts/fonts.conf`.

## Why `getcwd` returned NULL only sometimes

`source/kernel/syscall64.cpp`, `case X64_SYS_getcwd`, returned the guest buffer
address. The raw Linux system call returns the **length** of the string it
wrote, including the terminating NUL; returning the pointer is what the libc
wrapper does for *its* caller, not what the kernel does.

glibc's `sysdeps/unix/sysv/linux/getcwd.c` assigns the syscall result to an
`int` and accepts it only if `retval > 0 && path[0] == '/'`. A guest buffer at
`0x7fff_fexx_xxxx` — every thread stack in this lane — truncates to
`0xfexx_xxxx`, a negative `int`, so glibc fell through to its failure return
and handed back NULL. A heap buffer usually truncated positive, which is why
almost every other `getcwd` caller in the session worked and only fontconfig,
which puts its `cwd` buffer on the stack (`FcChar8 cwd[FC_MAX_FILE_LEN + 2]`),
failed on every process, every run.

`KProcess::getcwd` (`source/kernel/kprocess.cpp:3313`), the 32-bit lane's
implementation, has always returned `currentDirectory.length() + 1`. The
64-bit case now matches it, and a size that cannot hold the path is `-ERANGE`
rather than `-EFAULT`.

## The witness

Two lines, both bounded to one per process:

- `BOXEDWINE_X64_GETCWD pid= cwd= buf= size= ret= low32=` — the first `getcwd`
  each process makes. `low32=` is the half of the buffer address glibc actually
  looked at, so the hazard that caused this is visible in the log rather than
  inferred.
- `BOXEDWINE_X64_FONTCONFIG pid= config='/etc/fonts/fonts.conf' status=…` —
  `status=failed detail='<the guest's own first error line>'` when the process
  writes "Fontconfig error" to fd 2, or `status=loaded first_cache='…'` when it
  instead goes on to probe a per-directory cache (`.../fontconfig/<md5>-le64.cache-N`),
  which only happens once `FcInit` holds a configuration. Whichever comes first
  is the verdict; the other is suppressed.

## Font inventory of the 64-bit rootfs

Taken from the same device run, not from the packaging script.

**Wine's own faces are all there, TrueType included.** The guest resolves the
Wine data root to `/usr/lib/share/wine/fonts` (via
`/usr/lib/x86_64-linux-gnu/wine/../../share/wine`) and successfully opens 13
TrueType files — `tahoma.ttf`, `tahomabd.ttf`, `courier.ttf`, `fixedsys.ttf`,
`fixedsys_jp.ttf`, `marlett.ttf`, `ms_sans_serif.ttf`, `small_fonts.ttf`,
`small_fonts_jp.ttf`, `symbol.ttf`, `system.ttf`, `webdings.ttf`,
`wingding.ttf` — alongside ~50 `.fon` bitmap faces. Only two names in the whole
directory fail to open: `vgaoem.fon` and `serife.fon`.

So dialogs rendering with bitmap faces was never a missing-file problem.
Wine's TrueType set is found and opened; what was absent was the fontconfig
configuration that drives family substitution for everything Wine's own set
does not carry.

**System scalable fonts are staged**: `/usr/share/fonts/truetype/` carries
`dejavu`, `lato`, `liberation` and `noto`. Fontconfig probes each of them (and
`/usr/share/fonts` itself) for a `.uuid` and a cache under
`/home/username/.cache/fontconfig/`; both are absent on a first run, so every
session re-scans. That is expected until a cache is written, and is separate
from the parse failure above.
