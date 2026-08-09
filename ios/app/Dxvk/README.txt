DXVK 2.5.2 (upstream b4faf0b), cross-built i686-w64-mingw32, patched for MoltenVK.
Patch: geometryShader, shaderCullDistance, textureCompressionBC and
VK_EXT_transform_feedback are treated as optional rather than required, because
Metal has no geometry shader stage and MoltenVK never exposes them. DXVK
GetMaxFeatureLevel() does not consult any of these; its floor is 10_1, so the
resulting device is still shader-model-4 capable.
Source: https://github.com/doitsujin/dxvk (zlib/libpng licence)
