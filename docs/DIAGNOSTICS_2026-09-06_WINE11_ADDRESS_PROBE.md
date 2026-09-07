# Wine 11 startup address-probe correction

Device revision: `d637b39e+dirty`, build 137. All three captures
(`194252`, `194331`, `194347`) stop at the same Wine initialization boundary,
before Windows graphics initialization.

The first causal operation is an anonymous PROT_NONE MAP_FIXED_NOREPLACE at
`0x8000000000000000`, length `0x1000`. The sparse reservation path accepts it.
Wine 11's `get_host_addr_space_limit()` treats the exact return as a successful
probe and overflows its derived limit. `virtual_init()` then requests
`0x800200000` bytes for its view/protection table, receives ENOMEM, and aborts
at `view_block_start != MAP_FAILED`. The later FEX fault is the abort path.
This is not a graphics failure or exhaustion of the phone's physical RAM.

The shared mmap policy and memory entry points now enforce the emulated
Linux process's lower 48-bit canonical user range, including the final-page
guard. Invalid fixed ranges return ENOMEM before occupancy tests or metadata
allocation. Ordinary invalid hints may relocate. Length and alignment checks
also run before syscall page rounding/scanning. Valid high sparse reservations
are preserved. No Wine source patch or executable-specific rule is involved.

The regression replays Wine's descending bit-63 probe: 17 invalid addresses
are rejected, bit 46 succeeds, the derived limit is `0x7fffffff0000`, and the
internal allocation becomes `0x240000` (2.25 MiB). Boundary, wraparound,
occupied-invalid, fixed/no-replace, ordinary-hint, and existing high-reservation
cases are covered. Seven MSVC test suites pass. Device startup remains to be
verified with both graphics probes after installing the new IPA.

Wine runtime build inputs did not change; retain the Wine 11 PE32 ZIP and
existing base filesystem.

References:
- Wine 11.0 source, `dlls/ntdll/unix/virtual.c`: `get_host_addr_space_limit`
  and `virtual_init`, from the SHA-pinned source used by CI.
- [Linux mmap address validation](https://github.com/torvalds/linux/blob/master/arch/x86/mm/mmap.c).
- [Linux x86 user address limits](https://github.com/torvalds/linux/blob/master/arch/x86/include/asm/processor.h).
- [Linux mmap error semantics](https://man7.org/linux/man-pages/man2/munmap.2.html).
