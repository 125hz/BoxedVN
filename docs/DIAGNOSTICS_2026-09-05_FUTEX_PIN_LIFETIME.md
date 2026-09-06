# Releasing native futex pointer leases

Device baseline: `6dd0d6d3`, build 137. Captures `221107`, `221156`, and
`221439` on 2026-09-05 confirm that the D3D9 frame cap works and the PE32
DXVK route advances the 32-bit application to its legal screen. The user
confirmed that tapping advances to a black screen, where progress stops.

The 32-bit application subsequently reports EBUSY when mapping 1 MiB at
`0x101700000`, then `0x102300000`, and finally fails its free-area search.
The 64-bit application also reports EBUSY at previously used 1 MiB ranges,
but its later fault is still the null read at image offset `0xdb6c5`.
These captures do not establish that both symptoms have the same cause.

## Shared defect

KMemory64::getRamPtr permanently set K64_PAGE_PINNED on every futex word's
page. This included value-mismatch returns and wake calls with no waiters.
Munmap retained pinned backing but removed the guest mapping; a subsequent
anonymous mapping at the same address was rejected with EBUSY. Thus a
finished synchronization operation could permanently prevent Wine from
reusing a released allocation.

Native multithreaded futex calls now acquire reference-counted pointer
leases and release them with a scope guard after the final pointer use.
Overlapping wait/wake calls each retain a lease. Shared native Wine mapping
pins and legacy unscoped handouts remain permanent. Cooperative waits keep
their prior lifetime rule because they return to the scheduler while still
parked. Fork snapshots do not inherit the parent's host-pointer pins.

The change does not permit replacement while a lease remains active.
`BOXEDWINE_X64_PINNED_MAP` reports up to 16 remaining anonymous-map refusals,
including the conflicting page, lease count, and permanent-pin state.

## Save evidence and acceptance

The 64-bit capture finds its save directory, reads its 12-byte index, then
reads 1273 bytes from its selected save file before EOF. Settings files and
many asset-cache files also open and read. This disproves a missing save
directory in that capture; it does not verify the save's contents or explain
the empty array at the fault. No game files, saves, or title-specific code
were changed.

Host tests compile the production pointer acquisition/release functions and
the actual futex scope guard against a deterministic memory backend. They
cover normal and exceptional exits, overlapping leases, release after guest
unmapping, 20,000 concurrent scopes, preservation of permanent pins, and a
failed cross-page request. All host suites and 61 futex behavior/contract
checks pass. These tests do not emulate Darwin VM calls.

Device acceptance: retain verbose off, try advancing past the legal screen,
repeat the 64-bit Play action, and check that the former EBUSY allocations
no longer fail. The working cube and its 30/60 FPS limits remain a regression
check. The existing Wine ZIP is unchanged.

References: [Linux futex lifetime and wait/wake contract](https://man7.org/linux/man-pages/man2/futex.2.html),
[Linux v6.12 futex key handling](https://github.com/torvalds/linux/blob/v6.12/kernel/futex/core.c).
