# Fullscreen presentation, audio latency, and DXSO port

Input logs: 191016, 191429, and 191837 on 2026-09-07, revision 70849ac4.

The hosted fullscreen view still exposed the legacy menu. It now hides that
menu and exposes the existing custom-control editor through a compact toolbar.
Keyboard and display mode remain accessible beside Controls. Held controls
retain the existing touch-down/cancel/release lifecycle.

The X11 compositor had independently filled the host, covering Vulkan's
correctly fitted picture with stretched GDI content. It now copies the actual
presentation transform, including letterboxing and safe-area placement.
DXMT's separate view also follows the saved mode without resizing its drawable.

The float stereo stream in 191429 supplies about 190 KB/s instead of the
required 352.8 KB/s, with about 270 underruns per five seconds. PCM16 in
191016 sustains its required rate. SDL2 retains 512 source frames for its
resampling filter: its lookup table has 512 subdivisions per zero crossing,
but the convolution only reads five crossings plus one adjacent frame.
The source patch reduces lookahead to six frames (scaled conservatively for
downsampling). At 44100 Hz this reduces retained input from 11.6 ms to 0.14 ms.
Short Wine client buffers no longer surrender most of their capacity to
inaudible filter lookahead. We retain filter history rather than flushing
individual packets. This targets buffer starvation; device audio still needs
testing. Waveform, continuity, long-session accounting, and latency tests run
against the pinned patched SDL source in CI.

Primary source: https://github.com/libsdl-org/SDL/blob/release-2.32.10/src/audio/SDL_audiocvt.c

The direct D3D9 Metal port now has real implementation beyond the earlier
source audit: `boxedwine_dxmt_dxso_bridge.cpp` marshals all five DXSO calls,
native/WoW64 argument chains, opaque handles, and returned bitcode pointers.
Host fixtures exercise the actual bridge against compiler-boundary stubs.
The iPhoneOS build compiles the pinned candidate's DXSO and fixed-function
shader compilers, includes the bridge, and links against our iOS LLVM build.
The development artifact is a compiler archive, not an additional IPA.

This is not yet a working/selectable D3D9 renderer. Matching PE modules, native
Metal command integration, complete ABI validation, and device rendering/reset
tests remain. Shipping D3D9 continues to use DXVK and MoltenVK.

The 64-bit managed-runtime log still ends at a null access outside the current
thread's recognized code buffer, after roughly 5.16 million handled alignment
faults. Its loading animation before that point does not establish useful
progress afterwards. The cause is not yet established; no null-page mapping,
instruction skip, game modification, or speculative exception recovery is
included. It remains an unresolved device failure in this build.
