# Wine 11 server fork mapping correction

Device revision: `d09038d0+dirty`, build 137. Captures `200531`, `200741`,
`200912`, and `201024` all stop during the same server startup sequence,
before desktop or graphics initialization. The earlier address-space probe
assertion is no longer the blocker.

The client waits in connect while wineserver forks. The daemon child then
exits with status 132 (`cpu64-illegal-instruction`). Its parent exiting with
status zero is normal daemonization, not the failure.

In `200531`, the server inherited libc at `0x700005000`, length `0x212000`,
followed by a `0x2000` TLS mapping. After fork, two small mappings consume
`0x700000000` and `0x700001000`. A third, length `0x144000`, incorrectly
starts at `0x700002000`, overwriting libc through `0x700146000`.

`KMemory64::cloneFrom` copied page contents and sparse reservations but lost
both the allocator occupancy ranges and its cursor. The child therefore
considered inherited executable memory free. The illegal-instruction report
at `0x700146002` is a consequence: in the matching Ubuntu glibc 2.39 binary,
that byte is inside `c1 e3 08` (`shl $8,%ebx`), not a missing JRCXZ opcode.
The matching package is `libc6_2.39-0ubuntu8.8_amd64.deb` used by runtime CI.
No interpreter opcode workaround is appropriate.

The shared fork implementation now snapshots ranges, cursor, reservations,
and pages together under the existing mmap-to-pages lock order. Private
page copies and shared-page aliases retain their prior semantics. Self-copy
returns without destroying the address space. No executable-specific rule
or Wine runtime patch is involved.

A regression compiles the production clone and allocator functions against
an in-memory page backend. It replays the observed allocation sizes, forces
an allocator cursor reset to exercise gap scanning, checks that inherited
code remains occupied, and checks private-write isolation and shared writes.
Removing the inherited ranges makes the overlap assertion fail even with the
cursor fix retained. All eight MSVC host test suites pass. Physical-device
startup still needs verification after installing the new IPA.

The Starting Wine title and progress labels now use smaller monospaced fonts
and centered text within the existing centered startup stack, retaining
Dynamic Type scaling.

Wine runtime inputs are unchanged. Keep the existing Wine 11 PE32 ZIP and
base filesystem; only the IPA needs updating.

References:
- [Linux fork mapping semantics](https://man7.org/linux/man-pages/man2/fork.2.html).
- [POSIX fork private/shared mapping semantics](https://www.man7.org/linux/man-pages/man3/fork.3p.html).
- [Matching Ubuntu glibc package](https://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_2.39-0ubuntu8.8_amd64.deb).
