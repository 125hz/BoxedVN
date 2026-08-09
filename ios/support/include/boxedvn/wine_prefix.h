/*
 * BoxedVN - writable Wine-prefix preparation.
 * GPLv2; see license.txt.
 */

#ifndef BOXEDVN_WINE_PREFIX_H
#define BOXEDVN_WINE_PREFIX_H

#include <string>

namespace boxedvn {

struct WinePrefixPreparationResult {
    bool ok = false;
    bool changed = false;
    std::string error;
};

enum class WineRenderer {
    Default,
    GDI,
    Vulkan,
};

// Copies Wine's base registry files out of the read-only rootfs ZIP on first
// use, then applies BoxedVN's iOS compatibility policy to the writable overlay.
// Imported games select Wine 10's `renderer=vulkan`, which routes D3D through
// Boxedwine's generated Vulkan bridge and MoltenVK/Metal. This deliberately
// repairs the `renderer=gdi` value written into existing prefixes by builds
// 22-24. Wine Notepad retains its existing renderer. The policy also
// disconnects Wine's unsupported root Bluetooth device, because Wine 10
// starts root PnP services even when Start is disabled. Wine's HID bus remains
// available; build 24 proved disabling it does not improve cold startup.
WinePrefixPreparationResult prepareWinePrefix(
    const std::string& rootFilesystemZipPath,
    const std::string& writableRootPath,
    WineRenderer renderer);

// Copies the patched DXVK modules from `sourceDirectory` into the writable
// overlay at /home/username/.wine/drive_c/dxvk, where Boxedwine's own `-dxvk`
// switch expects to find them (see StartUpArgs in source/sdl/startupArgs.cpp,
// which maps that directory over system32). Existing files are always
// overwritten: a rebuilt module can have an identical size, so any "is it
// already there" shortcut risks pinning a stale DLL.
//
// This exists because upstream DXVK refuses to create a device on MoltenVK:
// it requires geometryShader and VK_EXT_transform_feedback, and Metal has no
// geometry shader stage. The bundled build relaxes those to optional.
//
// Returns false and sets `error` only when a copy genuinely failed. A missing
// source directory is not an error: a build may legitimately ship without it.
bool installBundledDxvk(const std::string& sourceDirectory,
                        const std::string& writableRootPath,
                        bool& changed,
                        std::string& error);

// Updates a Wine registry text document in memory. Section names use normal
// Windows separators (for example "Software\\Wine\\Direct3D"); this function
// performs Wine's on-disk backslash escaping. `serialisedValue` is the complete
// value after '=', such as "\"gdi\"" or "dword:00000004".
bool setWineRegistryValue(std::string& registry,
                          const std::string& section,
                          const std::string& name,
                          const std::string& serialisedValue);

}  // namespace boxedvn

#endif  // BOXEDVN_WINE_PREFIX_H
