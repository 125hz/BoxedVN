# Startup retest at revision 4e82a049

The four captures with an application header identify `4e82a049+dirty`.
The 22:07:37 retry omits that header but names the same installed bundle.
The new allocator marker in the cube also confirms the updated PE32 DLL.

## Findings

* 22:06:51 and 22:07:37, replacement 64-bit executable: the previous masked
  AVX access violation is absent. Both runs exhaust FEX's 64 MiB executable
  pool when requesting a 32 MiB code buffer, then fault in `ClearCache` while
  writing through the failed allocation. Three 16 MiB buffers plus dispatcher
  allocations already occupy the pool. The shared StikDebug arena has eight
  64 MiB segments, but the adapter originally assigned only one to FEX.
* 22:13:38, desktop launch: a child application reaches the same executable
  pool exhaustion. This is a shared runtime limit, independent of launch mode.
* 22:10:14, 32-bit cube: `allocator_reject_size_unknown` reports D3D9 RVA
  `0x13a678`. The matching rebuilt DLL resolves that to the shader hash table's
  bucket allocator after `_Prime_rehash_policy::_M_need_rehash`. This is a
  size-limit rejection before operator new, not proof of exhausted host RAM.
  The input state and returned bucket count were not recorded.
* 22:08:20, 32-bit text application: `gdi32!ScriptPlace+0x80` receives glyph
  count `0x12df90`; its caller is `ScriptStringAnalyse+0x763`. The new frame
  report completes and records that invalid argument before the access fault.
  Its origin remains unknown. Do not clamp the count or unprotect the page.

## Changes and checks

FEX now claims further segments from the already-prepared executable arena as
needed. Each segment retains its RX/RW pair and exact-size free list. Published
mapping entries are immutable and visible to signal handlers without locks.
The maintained translator patch selects the write alias separately for the
dispatcher, each code-buffer rollover, final code publication and backpatches.
Ordinary temporary emitter buffers retain direct writes. Apple code buffers
are capped at 32 MiB so a buffer plus guard padding fits a 64 MiB segment.
True arena exhaustion propagates an allocation exception instead of returning
the non-null `MAP_FAILED` value to FEX's unchecked emitter path.

Host tests cover distinct positive/negative alias offsets, range boundaries,
publication validation, rollover after the observed three-buffer sequence,
and released-buffer reuse. A portable check compiles the actual patched
DualMap header's Apple path and verifies writes reach the corresponding alias.

The cube's bounded hash-policy reports preserve the original C++ allocator
and record input counts, raw load-factor bits, and output bucket count. Native
i386 checks verify the member-call ABI, three load factors, and 1,000 map
insertions/lookups. The translator fixture additionally exercises the x87
division, rounding-mode changes, and integer conversion used by this policy.
Verbose Wine mode now includes its existing `uniscribe` trace for the invalid
glyph-count investigation. The 32 relay tests and host support tests pass.

References: [GCC hash-policy contract](https://github.com/gcc-mirror/gcc/blob/master/libstdc%2B%2B-v3/include/bits/hashtable_policy.h),
[Wine 9 text shaping](https://github.com/wine-mirror/wine/blob/wine-9.0/dlls/gdi32/uniscribe/usp10.c),
and the pinned FEX source at `04cbb90c715519136da771af3cc8f1dac9b821a6`.

## Device acceptance

Retest the replacement 64-bit executable directly and from the desktop, with
verbose logging enabled. Check for `BOXEDWINE_FEX64_CODE_SEGMENT index=1` and
progress beyond the old exhaustion point. Retest the 32-bit cube with verbose
off and the text application with verbose on. Cube rendering and successful
text initialization remain unverified. Deliver one increased-memory IPA and
the matching external `main/wine64-pe32.zip`; no local IPA/hash download.
