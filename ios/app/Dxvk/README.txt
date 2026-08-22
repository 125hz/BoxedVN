DXVK 2.5.2 (upstream b4faf0b), cross-built i686-w64-mingw32, patched for MoltenVK.
Patch: geometryShader, shaderCullDistance, textureCompressionBC,
VK_EXT_transform_feedback, robustBufferAccess2, and nullDescriptor are requested
only when the Vulkan implementation reports them. Metal lacks several of these
features, but DXVK's feature-level calculation does not depend on them, so the
resulting device remains shader-model-4 capable. Workloads that actually require
an unavailable optional feature may still fail later and must be diagnosed from
their DXVK log.
Rebuild patch: third_party/patches/dxvk-2.5.2-moltenvk.patch
Source: https://github.com/doitsujin/dxvk (zlib/libpng licence)
