# Direct3D 9 Metal test path

The app builds the complete `dacevedo12/dxmt` `v0.4-d3d9` runtime at
`e8dd4c656dcb74a6d970a30a397d1558b0e3fb2b`, with the iOS adaptations in
`scripts/d3d9-metal-patches`. Both PE architectures and the native renderer
come from that revision. This replaces the previous D3D11-only DXMT source;
it does not load the desktop macOS unix library.

The execution path remains Windows program → Wine 11 inside BoxedWine → FEX
for guest CPU execution. D3D9 goes through DXMT's PE frontend, the ELF64
`winemetal.so` syscall veneer, native DXSO/fixed-function shader compilation,
and native Metal commands and presentation. There is no Vulkan step in this
D3D9 route. PE32 D3D11 still uses the existing DXVK path.

Settings → Direct3D 9 renderer selects Metal (experimental) or Vulkan. Metal
is the initial default. Selection applies on the next container launch and
also to the 32-bit cube and applications opened from the desktop. The
module projections are per session, so selecting Vulkan restores its D3D9
module on the next launch without modifying game files.

The native table and both guest unix-call tables contain 151 entries. WoW64
uses an explicit flag on the private host call because its ELF64 thunk has
already switched to 64-bit code. Shader handles stay opaque 64-bit values;
guest argument pointers use the BoxedWine alias. CPU-visible Metal buffers
are backed by guest allocations aligned and rounded to the 16 KB iOS page.

Build validation covers table indices, both PE architectures and exports,
module dependency resolution, packaged hashes, and native/WoW64 shader
argument conversion. iPhone rendering and performance require device tests.
Start with the spinning 32-bit cube, then test a D3D9 application with verbose
logging off. Retest the 64-bit D3D11 cube because it shares the new native
runtime. `BOXEDWINE_D3D9_METAL` confirms module projection;
`BOXEDWINE_DXMT_D3D9` records shader dispatch failures.

The iOS adaptations do not accelerate x87 CPU instructions. A D3D9 Metal
route alone therefore does not establish a target frame rate for CPU-bound
programs.
