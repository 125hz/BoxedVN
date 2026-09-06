# Audio callbacks and shared-view replacement

Device baseline `ed385e1c`, build 137; captures 001137 and 001438.

The visual application still deadlocks, but the frame chains now identify
both paths. With XAudio2_8.dll at 77ef0000, RVA 4c79 resolves to
XA2VCB_OnBufferStart, reached from FAudio_INTERNAL_GenerateOutput and the
audio-client thread. Its application callback waits on 047c93d8 held by
thread 00fc. That thread is inside FAudioSourceVoice_SubmitSourceBuffer
(RVA 10479), waiting on the buffer mutex 00343d90 held by the callback
thread 00d0. The local matching PE's COFF symbols establish these names.

Wine 9's bundled FAudio holds bufferLock and sendLock across these
callbacks. Backport upstream FAudio commit
[c2ef8d3](https://github.com/FNA-XNA/FAudio/commit/c2ef8d3104401a79a7886062c4a5871db0b7b6f6),
with source paths adjusted only. It releases the voice locks around client
callbacks and reacquires them before inspecting queue state. The source
builder now rebuilds all XAudio2 versions for i386 and x86-64 and packages
them in their respective Wine layers. Audio compilation is a required CI
step. This is a shared Wine compatibility repair, with no executable-name
condition or application patch.

The regression executes the actual start/loop callback blocks with a
competing thread holding an application mutex and attempting voice API
locks. Original Wine 9 blocks that thread; the backport allows progress.
Both negative controls and both repaired cases passed under MSVC; CI repeats
them against the exact Wine source used for the rebuilt DLLs.

The D3D9 application now successfully maps the 76 KiB buffer. Its first
fault is at 0113acd2 while scanning UTF-16 text; the last character read is
8080 and the scan reaches a no-access reservation. The preceding file read
completed with all 50168 bytes. Later access violations in Wine's unwinder
and the eventual stack overflow are secondary, not the first failure.

A shared-runtime defect remains in nativeMapAnonymous: a reused tracked
host range gets mprotect plus memset, even when it is still a physical
shared-file alias. This can clear other views and leave subsequent private
heap allocations exposed to old buffer writes. Before zeroing, detach the
requested shared host pages using Darwin copy-on-write remapping from the
stable canonical backing. Reject a partial request that would detach an
adjacent live shared guest page. No-access reservations and KUSER retain
their separate rules. This defect is established by code inspection; its
causal role in the text scan requires device confirmation.

The Darwin test executes the production detachment helper and shared-file
mapper with native and 16 KiB test granules. It checks repeated mapping of
an existing view, zeroing on private replacement, isolation in both
directions, and refusal to detach a live shared neighbour. Bounded first
i386 fault data snapshots will help distinguish stale pointers from
overwritten contents if another fault remains.

Delivery requires both the new increased-memory IPA and replacement
wine64-pe32.zip. Test both applications with verbose off, and recheck the
32-bit cube/frame limiter. If the logo still stalls, capture at least 70
seconds. No games were launched on the PC or game/save files modified.
