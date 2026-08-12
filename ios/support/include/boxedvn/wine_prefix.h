/*
 * BoxedVN - writable Wine-prefix preparation.
 * GPLv2; see license.txt.
 */

#ifndef BOXEDVN_WINE_PREFIX_H
#define BOXEDVN_WINE_PREFIX_H

#include <cstddef>
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

struct GuestFontInstallResult {
    bool ok = true;
    // Font files copied into the prefix by this call.
    std::size_t installed = 0;
    // Font files the user has supplied, whether or not they were copied now.
    std::size_t available = 0;
    std::string error;
};

// Copies the user's own font files into the prefix at
// drive_c/windows/Fonts, where Wine's font backend scans for them on the next
// start. Accepts .ttf, .ttc, .otf and .fon; anything else in the directory is
// ignored rather than treated as an error.
//
// This exists because a font the guest cannot find is not a cosmetic problem.
// A Chromium-family game aborts outright when Blink's font collection comes
// back empty - the renderer stops at a CHECK in FontCache and takes the whole
// guest with it - and a Japanese title with no CJK face renders its text as
// mojibake, which then gets misread as a renderer fault. BoxedVN cannot ship
// fonts: the ones these games expect are Microsoft's, and the root filesystem
// archives have not been reviewed for redistribution either. What it can do
// is make the user's own fonts reachable, which is the same thing Wine's
// documented manual workaround does.
//
// A missing source directory is not an error; it is the ordinary case for a
// user who has not supplied any. Existing files are skipped rather than
// overwritten: unlike the DXVK modules these are the user's files, BoxedVN
// has no newer version of them, and recopying every face on every launch
// would be slow for a large CJK font.
GuestFontInstallResult installGuestFonts(const std::string& sourceDirectory,
                                         const std::string& writableRootPath);

struct GuestFontCensus {
    bool ok = false;

    // Font files the user supplied, already copied into the prefix.
    std::size_t inPrefix = 0;
    // Font files the root filesystem ships in drive_c/windows/Fonts, which a
    // prefix created from it inherits.
    std::size_t inRootFilesystem = 0;
    // Font files Wine ships in share/wine/fonts. wine.inf installs these into
    // a new prefix, so they count as available even when the prefix's own
    // Fonts directory is empty in the archive.
    std::size_t wineBundled = 0;

    // Of the totals above, how many are in a format DirectWrite can load.
    //
    // This split is the whole point of the census. Chromium reaches fonts
    // through DirectWrite, which understands TrueType and OpenType and does
    // not understand the legacy .fon bitmap format at all - GDI does. A guest
    // whose only faces are .fon therefore has a full Fonts directory and an
    // empty font collection as far as a browser engine is concerned, which
    // looks like every other font failure and is not one: adding more .fon
    // files would change nothing.
    std::size_t scalable = 0;  // .ttf, .ttc, .otf
    std::size_t bitmap = 0;    // .fon

    // A few names, so the log shows what kind of faces these are rather than
    // only how many. Comma separated, possibly empty.
    std::string examples;

    std::string error;

    std::size_t total() const {
        return inPrefix + inRootFilesystem + wineBundled;
    }
};

// Counts the font files a guest can actually see, across the read-only root
// filesystem archive and the writable prefix overlaid on it.
//
// This exists because "does the guest have a font" turned out to be both
// decisive and unanswerable from outside. A Chromium-family guest aborts in
// Blink's FontCache when its font collection is empty, and RPG Maker hangs
// forever in Scene_Boot when document.fonts.ready never settles - two very
// different-looking failures with one candidate cause, and no way to tell
// from the host whether that cause is present. Every switch tried against
// the second failure was a guess, because a command line cannot conjure a
// font. This turns the question into a log line.
//
// Reads the archive's central directory only; nothing is extracted and no
// entry data is decompressed.
GuestFontCensus censusGuestFonts(const std::string& rootFilesystemZipPath,
                                 const std::string& writableRootPath);

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
