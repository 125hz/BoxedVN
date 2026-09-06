# Partial shared views and lock caller attribution

Device baseline: `a3f711fb`, build 137. Captures 232907, 233105,
233231, 233758 and 233852 are the latest supplied sessions; 233852 is a
continuation after the application header in 233758.

The PE32 D3D9 captures now successfully alias the 1 MiB sections that failed
in the preceding build. The next allocation fails at length `0x13000`,
offset `0x1f0000`, with Windows error 87. The native mapper required lengths
to be multiples of 16 KiB, then rejected this request in the legacy path
because its file pages already had stable native aliases. D3D9 reports
`InitTexture: map failed` before the terminal null access at `0x18b7662`.

Allow a final partial host page when the start and file offset are aligned.
Preflight the trailing guest pages under the memory locks: only free pages
or untouched, uncommitted anonymous reservations can be covered by the host
alias. Live pages and retained data remain protected by an EBUSY refusal.
Only requested guest pages gain mapping permissions. Unaligned starts and
file offsets remain outside this change, as do KUSER mappings.

The Darwin regression uses the production mapper and physical VM aliases.
It now runs with the native granule and an explicit 16 KiB test granule,
including a 76 KiB view, refusal to overwrite a live neighbour or retained
no-access data, and acceptance over an unused reservation. The override is
defined only by the test runner. Windows host suites pass: six CTest targets,
61 futex tests, and 121 arena/packaging cases (51 platform skips).

The visual application in 233105 still has a critical-section cycle:
Windows thread 00d0 waits on 049cd500 held by 00fc, while 00fc waits on
00343d90 held by 00d0. The reporting Linux thread IDs are 66 and 77,
respectively; they must not be inferred from Windows thread IDs. The first
stack scan contains stale CRT cleanup addresses and cannot establish the
responsible component. This change does not claim to repair that deadlock.

Add bounded EBP-chain return addresses and a best-effort snapshot of actual
module ranges from the i386 PEB loader list. Log only modules containing a
captured address. Do not take guest loader locks or alter guest state.
Malformed lists, cycles, overflow and out-of-stack frames terminate the
diagnostic. Frame-pointer omission can still truncate the chain. These
records remain limited to two timeout reports per host thread.

Reference for the i386 PEB/LDR and TEB layouts:
[Wine 9 winternl.h](https://github.com/wine-mirror/wine/blob/wine-9.0/include/winternl.h).
Shared-memory semantics and Darwin alias references remain in
[the preceding investigation](DIAGNOSTICS_2026-09-05_SHARED_VIEWS_AND_LOCKS.md).

Acceptance: retain the current Wine ZIP and test with verbose off. For the
visual application, leave the stalled logo visible for at least 70 seconds.
The D3D9 application should no longer encounter this particular partial-view
refusal; reaching its menu still requires device confirmation. Recheck the
32-bit cube for rendering and frame limiting. No title-specific patches,
game-file edits, save changes or Wine upgrade are included.
