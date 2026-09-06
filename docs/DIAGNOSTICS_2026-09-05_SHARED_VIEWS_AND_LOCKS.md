# Native shared views and WoW64 lock waits

Baseline: `fb2022a0`, device build 137. The 225545 capture launches a PE32
D3D9 application. Near the terminal null write it reports a 1 MiB shared
mapping failure at `0x68020000`, EBUSY, and a non-persisted buffer allocation
failure. Earlier permanent-pin refusals at `0x4f80000` have no active leases.
This is distinct from the temporary futex pin defect repaired previously.

The native mapper previously promoted each shared file page to one guest
address and rejected a second native view. Promoted addresses stayed pinned
for the identity address space's lifetime to protect sparse readers. That
cannot serve applications mapping one shared buffer at both high and low
addresses, as WoW64 graphics allocation can require.

On Darwin, full host-page shared mappings now use stable backing pages and
`vm_remap` with copy disabled. Native guest views and sparse interpreter
accesses see the same physical storage. Replacing a guest view does not
retarget the canonical pointer or require a permanent guest-address pin.
File offsets share backing by host-page chunk; only newly created guest
file pages are seeded. Mapping operations are serialized by mmapMutex.

Sub-host-page mappings and KUSER retain their existing behavior. Requests
that would migrate a live legacy view or require unsupported subpage aliases
remain rejected; this does not claim arbitrary 4 KiB alias support on a
16 KiB host. Partial VM alias creation failures clean up the target mapping.

The two 32-bit visual application captures (224448 and 224558) have no
remaining EBUSY reports. Both reach a circular critical-section wait:
thread 00d0 waits for 047c9a50, owned by 00fc/00f4; that owner waits for
00343d90, owned by 00d0. The main thread also waits on the first lock.
This establishes the captured deadlock, but not the component that caused it.

When Wine reports a critical-section timeout, bounded diagnostics now read
the reporting thread's saved WoW64 context from TEB64 TLS slot 1, validate
the i386 machine and TEB32 stack bounds, and report executable-address
candidates from that stack. These are candidates, not an unwound backtrace.
No sibling CPU state is read and no guest lock is changed. This adds evidence
for the next repair; it is not itself a deadlock fix.

Host tests cover valid and invalid context decoding and existing memory,
frame pacing, and futex behavior. macOS CI additionally extracts the shipping
shared-file registry and mmapSharedFile implementation into a real Darwin VM
test: two views, two-way writes, sparse visibility, replacement of one view,
and persistence after both guest views are released. iPhone acceptance still
requires device tests. Retain the current Wine ZIP and verbose off; for the
visual application, leave the stalled screen for 70 seconds so Wine emits
its timeout and the new caller candidates.

References:
- [Apple XNU VM interfaces](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/mach_vm.defs)
- [Shared mmap semantics](https://man7.org/linux/man-pages/man2/mmap.2.html)
- [Wine 9 WoW64 context storage](https://github.com/wine-mirror/wine/blob/wine-9.0/dlls/wow64cpu/cpu.c)

Game executables were inspected only. No game files, saves, title-specific
overrides, or Wine version changes are included.
