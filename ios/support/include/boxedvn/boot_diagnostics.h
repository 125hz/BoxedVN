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
 *  Seeing inside a browser-engine guest.
 *
 *  A Chromium guest fails in ways no host-side measurement can reach. The
 *  worked example: an RPG Maker MV title that reaches its loading screen and
 *  stays there forever, with no error anywhere - not in Wine, not in
 *  Chromium, not in the game. Eight device runs and four host-side theories
 *  produced nothing. What finally named the gate was eleven lines of
 *  JavaScript pasted into the game's own index.html by hand, printing the
 *  three conditions the engine waits on.
 *
 *  That worked and should not have been necessary, so this does it. Given a
 *  game directory, it installs the same probe, keeps the original safe, and
 *  takes it out again on request. The output goes to the ordinary session log
 *  through Chromium's own --enable-logging=stderr, which BoxedVN turns on for
 *  the same launch.
 *
 *  This is the only thing in BoxedVN that writes inside a game's own files,
 *  so: it never runs unless asked for by name, it never overwrites a backup
 *  it did not make, and removing it restores the original byte for byte
 *  rather than trying to un-edit the injected text.
 *  ---------------------------------------------------------------------
 */

#ifndef BOXEDVN_BOOT_DIAGNOSTICS_H
#define BOXEDVN_BOOT_DIAGNOSTICS_H

#include <string>

namespace boxedvn {

struct BootDiagnosticsResult {
    bool ok = false;

    // True when the game's HTML now carries the probe (install), or is back
    // to its original contents (removal).
    bool changed = false;

    // The file that was modified or restored, relative to the game
    // directory. Empty when no HTML entry point was found, which is not an
    // error: not every guest is one of these.
    std::string documentPath;

    std::string error;
};

// The name of the untouched copy kept beside the game's HTML. Removal
// restores from this and then deletes it.
extern const char kBootDiagnosticsBackupSuffix[];

// A marker inside the injected block, so an instrumented document is
// recognised. Note that recognising one is NOT a reason to leave it alone:
// build 104 skipped re-injection whenever the marker was present, so a device
// running a newer BoxedVN kept executing the probe an older one had written,
// and a whole device run reported fields that had been replaced. Installation
// always rebuilds from the saved original instead.
extern const char kBootDiagnosticsMarker[];

// What BoxedVN injects into a browser-engine guest's document.
struct GuestBootScripts {
    // Report the boot gates every three seconds; see bootDiagnosticsScript().
    bool diagnostics = false;

    // Repair RPG Maker's font gate.
    //
    // Scene_Boot will not finish while Graphics.isFontLoaded("GameFont") is
    // false, and on the CSS font-loading path that wait has no timeout at
    // all - so a face that never reaches "loaded" hangs the game silently and
    // forever. Device runs put the guest in exactly that state with
    // everything else healthy: the renderer at 58 fps, document.fonts.ready
    // resolved, the font set reporting itself loaded, the database and images
    // ready, and no failed resource.
    //
    // The shim asks Blink for the face directly, then, if the gate is still
    // shut after ten seconds, opens it. A game that draws its text in a
    // fallback face is worse than one that draws it correctly and better than
    // one that never starts, and RPG Maker's own non-CSS path makes the same
    // trade - it gives up after sixty seconds, it just throws instead of
    // continuing.
    bool fontFix = false;

    bool any() const { return diagnostics || fontFix; }
};

// The script BoxedVN injects. Exposed for tests, and so the exact text a
// game will run is reviewable in one place rather than buried in a writer.
//
// It reports, every three seconds: whether requestAnimationFrame is being
// serviced (a frozen counter means the renderer's lifecycle is not running,
// which is a different fault from anything font-shaped), the FontFaceSet
// status and whether its ready promise ever settles, RPG Maker's three
// Scene_Boot gate conditions, and which scene is current. Everything is
// wrapped so that a guest which is not RPG Maker reports what it can rather
// than throwing.
std::string bootDiagnosticsScript();

// The RPG Maker font-gate shim. Exposed for the same reason.
std::string fontGateShimScript();

// Finds the game's HTML entry point - www/index.html as RPG Maker MV
// deploys it, or index.html at the top level as MZ does - and installs or
// removes the probe.
//
// Installing twice is a no-op. Removing when nothing was installed is a
// no-op. A game with no recognisable HTML entry point is reported through an
// empty documentPath, not an error.
BootDiagnosticsResult setGuestBootScripts(const std::string& gameDirectory,
                                          const GuestBootScripts& scripts);

}  // namespace boxedvn

#endif  // BOXEDVN_BOOT_DIAGNOSTICS_H
