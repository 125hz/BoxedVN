/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 *
 *  ---------------------------------------------------------------------
 *  Which Direct3D generation an imported game actually needs.
 *
 *  This exists because the answer decides which translation layer can run
 *  the game at all, and getting it wrong is silent. Metal has no geometry
 *  shader stage, so MoltenVK reports geometryShader = false on every device
 *  and wined3d's Vulkan adapter can never advertise Direct3D 10. A game that
 *  needs D3D10 or D3D11 therefore has to go through DXVK, which compiles DXBC
 *  to SPIR-V itself and needs no geometry stage.
 *
 *  Until now that choice was a list of executable names. A list only ever
 *  covers titles someone has already sat down with, and every game outside it
 *  silently got the renderer that structurally cannot run it: wined3d reports
 *  no supported feature level, the game's D3D11 device creation fails, and
 *  whatever fallback it has - usually a software rasteriser - takes over. The
 *  binaries already say which Direct3D they need, so ask them.
 *  ---------------------------------------------------------------------
 */

#ifndef BOXEDVN_DIRECT3D_PROFILE_H
#define BOXEDVN_DIRECT3D_PROFILE_H

#include <cstddef>
#include <string>
#include <vector>

namespace boxedvn {

// What a scan of a game's files concluded.
struct Direct3DUsage {
    // True when something in the game links or ships Direct3D 10/11/12.
    bool needsModernDirect3D = false;

    // The specific file and module that proved it, for the session log.
    // Empty when nothing was found.
    std::string evidence;

    // How many files were opened. Reported so a scan that hit its bound - and
    // therefore might have missed the proof - is visible rather than assumed
    // to be a negative result.
    std::size_t filesInspected = 0;
    bool reachedLimit = false;
};

// True for the module names that only a Direct3D 10 or newer client links:
// d3d10*.dll, d3d11*.dll, d3d12*.dll and dxgi.dll. Direct3D 9 and earlier
// titles link d3d9.dll/ddraw.dll and never these. Case-insensitive.
bool isModernDirect3DModule(const std::string& moduleName);

// True for files that are themselves a Direct3D 10/11 client shipped beside
// the game: ANGLE's libGLESv2.dll and libEGL.dll, which every Chromium,
// Electron and NW.js title carries and which reach D3D11 through
// LoadLibrary rather than an import table, plus a bundled d3d11.dll itself.
bool isModernDirect3DFileName(const std::string& fileName);

// Reads the names in a PE file's import and delay-import directories.
// Bounded and bounds-checked at every step: a truncated, hostile or simply
// non-PE file yields an empty list, never a crash and never a large read.
// Only the headers, the section table and the descriptor arrays are read, so
// the cost is a few kilobytes per file regardless of how large it is.
std::vector<std::string> readPeImportedModules(const std::string& path);

// Walks `gameDirectory` looking for the proof above. Stops at the first
// positive, at `maxFiles` files, or at `maxDepth` directory levels, whichever
// comes first, so an enormous game directory cannot stall a launch.
Direct3DUsage detectDirect3DUsage(const std::string& gameDirectory,
                                  std::size_t maxFiles = 256,
                                  std::size_t maxDepth = 4);

}  // namespace boxedvn

#endif  // BOXEDVN_DIRECT3D_PROFILE_H
