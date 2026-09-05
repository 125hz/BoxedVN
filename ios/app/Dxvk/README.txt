DXVK 2.5.2 (upstream b4faf0b), cross-built i686-w64-mingw32, patched for MoltenVK.
Patch: geometryShader, shaderCullDistance, robustBufferAccess2, and
nullDescriptor are disabled for the MoltenVK device; textureCompressionBC and
VK_EXT_transform_feedback are requested only when the Vulkan implementation
reports them. Metal lacks several of these features, but DXVK's feature-level
calculation does not depend on them, so the resulting device remains
shader-model-4 capable. The same mask is compiled into D3D9, D3D10, and D3D11;
a warning in the DXVK log records the active mask.
Rebuild patch: third_party/patches/dxvk-2.5.2-moltenvk.patch
Source: https://github.com/doitsujin/dxvk (zlib/libpng licence)

The iOS CI workflow rebuilds this pinned version using
scripts/build-dxvk-i386.sh and replaces these resource DLLs and the Wine64
PE overlay together. It also applies dxvk-2.5.2-worker-errors.patch so worker
exceptions report their cause to guest stderr before returning status 1.
The checked-in DLLs remain the fallback for local builds without that CI step.
