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

// A marker inside the injected block, so an already-instrumented document is
// recognised rather than instrumented twice.
extern const char kBootDiagnosticsMarker[];

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

// Finds the game's HTML entry point - www/index.html as RPG Maker MV
// deploys it, or index.html at the top level as MZ does - and installs or
// removes the probe.
//
// Installing twice is a no-op. Removing when nothing was installed is a
// no-op. A game with no recognisable HTML entry point is reported through an
// empty documentPath, not an error.
BootDiagnosticsResult setBootDiagnostics(const std::string& gameDirectory,
                                         bool enabled);

}  // namespace boxedvn

#endif  // BOXEDVN_BOOT_DIAGNOSTICS_H
