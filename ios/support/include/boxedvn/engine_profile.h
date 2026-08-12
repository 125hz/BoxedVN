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
 *  Which application engine an imported game is built on.
 *
 *  Direct3D detection (boxedvn/direct3d_profile.h) answers "which translation
 *  layer can draw this", and for a Chromium-family title it answers DXVK.
 *  That is necessary and not sufficient: a browser engine has its own
 *  process model, its own compositor and its own font stack, and each of
 *  those makes assumptions about Windows that Wine only partly meets. A
 *  device log of a working DXVK device, a mapped window and no pixels is the
 *  normal shape of that failure - the Direct3D layer is healthy and the
 *  browser above it is not.
 *
 *  Those assumptions are fixed by command-line switches Chromium itself
 *  provides, so the whole repair is "recognise the engine, pass the
 *  switches". That is what this header is for. It deliberately identifies the
 *  *engine*, not the title: every NW.js game has the same compositor, so a
 *  profile keyed to the engine covers titles nobody has sat down with, which
 *  a list of executable names cannot.
 *  ---------------------------------------------------------------------
 */

#ifndef BOXEDVN_ENGINE_PROFILE_H
#define BOXEDVN_ENGINE_PROFILE_H

#include <cstddef>
#include <string>
#include <vector>

namespace boxedvn {

enum class GuestEngine {
    // Nothing recognised. Never assume this means "native Win32"; it means
    // no marker was found, which includes a game whose marker sits below the
    // scan's depth or file bound.
    Unknown,

    // NW.js: nw.dll, nw.pak, package.nw. What RPG Maker MV and MZ ship.
    NwJs,

    // Electron: resources/app.asar and friends.
    Electron,
};

// Which content framework sits on top of the engine, when that is
// recognisable. Purely informational - it changes no switch - but it is the
// difference between a log line a user can act on and one they cannot.
enum class GuestFramework {
    Unknown,
    RpgMakerMv,
    RpgMakerMz,
};

struct GuestEngineProfile {
    GuestEngine engine = GuestEngine::Unknown;
    GuestFramework framework = GuestFramework::Unknown;

    // The specific file that proved it, for the session log.
    std::string evidence;

    // How many entries the walk looked at, and whether it stopped early. A
    // negative result from a truncated scan is not a negative result.
    std::size_t filesInspected = 0;
    bool reachedLimit = false;

    // True for every engine that embeds Chromium. The compatibility switches
    // below apply to exactly these.
    bool isChromium() const {
        return engine == GuestEngine::NwJs || engine == GuestEngine::Electron;
    }
};

// Stable names for the session log and for tests.
const char* toString(GuestEngine engine);
const char* toString(GuestFramework framework);

// True for a file name that only a Chromium-family application ships.
// Case-insensitive, matched on the file name alone so it is independent of
// where in the game the file sits.
GuestEngine engineForFileName(const std::string& fileName);

// Walks `gameDirectory` looking for the markers above. Stops at the first
// conclusive answer, at `maxFiles` entries, or at `maxDepth` directory
// levels, whichever comes first, so an enormous game directory cannot stall a
// launch. Unlike the Direct3D walk this inspects names only - no file is
// opened - because every marker is a name.
GuestEngineProfile detectGuestEngine(const std::string& gameDirectory,
                                     std::size_t maxFiles = 4096,
                                     std::size_t maxDepth = 4);

// The Chromium command-line switches BoxedVN passes to a Chromium-family
// guest, in the order it passes them.
//
// Each repairs a Windows assumption that Wine under Boxedwine does not meet.
// They are Chromium's own supported switches, so they are forward-compatible
// across the NW.js versions RPG Maker has shipped and an unrecognised one is
// ignored rather than fatal. Three device runs of the same NW.js title
// established them:
//
//   run 1 (build 94, no switches)   black window, then the renderer aborts
//                                   in FontCache and the guest exits 1
//   run 2 (+first four switches)    no abort, the game's own 816x624 window
//                                   appears, three composited presents and
//                                   then nothing: a white page
//   run 3 (GPU switches swapped     PixiJS initialises in Canvas mode, audio
//          for software rendering)  opens, the render loop runs
//
//   --no-sandbox
//       The Windows sandbox is built from token, job-object and handle
//       behaviour Wine implements only partly, and it is what forces the
//       renderer to reach DirectWrite through the browser process rather
//       than directly. Run 1 aborted in Blink's FontCache with an empty
//       font collection; run 2 did not. This is what fixed that.
//
//   --disable-gpu, --disable-software-rasterizer
//       Composite in the browser process through Skia and GDI, and do not
//       fall back to SwiftShader. Run 2 kept the hardware path
//       (--in-process-gpu) and produced a window whose presentation
//       swapchain was created and never presented to. The software path is
//       the one Wine's GDI and BoxedVN's X11 compositor already handle -
//       it drew a Chromium dialog as far back as build 92 - and refusing
//       SwiftShader keeps a second x86 rasteriser out of the JIT arena.
//       WebGL is unavailable on this path; PixiJS falls back to Canvas,
//       which is what run 3 reported.
//
//   --disable-direct-composition
//       Chromium presents through DirectComposition by default. Wine's
//       dcomp is a stub, so the swap chain is created, the frame is
//       submitted, and nothing reaches the window.
//
//   --disable-features=CalculateNativeWinOcclusion,
//   --disable-background-timer-throttling,
//   --disable-renderer-backgrounding,
//   --disable-backgrounding-occluded-windows
//       Chromium stops painting, and suspends requestAnimationFrame, for a
//       window it believes is hidden or occluded. Its visibility comes from
//       window messages and a walk of the real window stack, neither of
//       which means under Wine inside a single full-screen guest what it
//       means on Windows. In run 2 BoxedVN's presenter did not attach to
//       the guest's window until a minute after the guest created it, which
//       is exactly the window of time in which Chromium would have decided
//       it was not visible.
//
// These are shipped as a set because that is how they were proven. Run 3
// changed the GPU pair and the three throttling switches together, so their
// individual contributions are not isolated, and this comment should not be
// read as claiming they are.
std::vector<std::string> chromiumCompatibilitySwitches();

// A line a user can put in launch settings to mean "add none of the switches
// above". It is stripped before launch and never reaches the guest.
//
// Chromium has no way to express the negation of a switch - there is no
// --enable-sandbox to cancel --no-sandbox - so without this there would be no
// way to turn one of BoxedVN's defaults off. Being an explicit opt-out rather
// than a side effect of typing anything is the point: see mergeChromiumSwitches.
extern const char kChromiumDefaultsOptOut[];

// A line a user can put in launch settings to ask BoxedVN to instrument the
// game's own HTML with the probe in boxedvn/boot_diagnostics.h, and to turn
// on the Chromium logging that carries its output into the session log.
// Stripped before launch; never reaches the guest.
extern const char kBootDiagnosticsOptIn[];

// A line that turns the RPG Maker font-gate shim off for one game. The shim
// is on by default for a recognised RPG Maker guest, because without it that
// guest does not start at all; this exists so the default can be disproved on
// a title where it turns out to hurt.
extern const char kFontGateShimOptOut[];

struct ChromiumSwitchMerge {
    // The full argument list to pass to the guest: the user's own, minus the
    // opt-out line, plus whatever BoxedVN contributed.
    std::vector<std::string> arguments;

    std::size_t added = 0;
    // Switches BoxedVN would have added but the user had already named, and
    // therefore left alone.
    std::size_t deferredToUser = 0;
    // True when the user's own --disable-features was combined with
    // BoxedVN's rather than one replacing the other.
    bool mergedFeatures = false;
    // True when the user asked for no defaults at all.
    bool optedOut = false;
    // True when the RPG Maker font-gate shim should be installed.
    bool fontGateShim = false;
    // True when the user asked for boot diagnostics. The caller installs the
    // probe; this only reports the request and ensures Chromium logging is
    // on, because without it the probe's output goes nowhere.
    bool bootDiagnostics = false;
};

// Combines BoxedVN's switches with whatever the user typed in launch
// settings.
//
// The first version of this stood down completely as soon as the user's
// arguments contained any switch at all, on the grounds that a merge could
// silently drop a repeated --disable-features - Chromium keeps only the last
// one it sees. That reasoning was right about the hazard and wrong about the
// fix: it meant adding one diagnostic switch such as --enable-logging=stderr
// silently removed eight compatibility switches, turning a debugging session
// into a broken guest for a reason nothing in the app explained. That is the
// same silent-footgun shape this project already had to fix once, when an
// environment variable typed into the arguments field was quietly ignored.
//
// So: merge, and handle the hazard directly. A switch the user names wins
// outright and BoxedVN does not add its own copy. --disable-features is
// combined value by value, because it is a list rather than a setting. The
// opt-out line above turns the whole set off.
ChromiumSwitchMerge mergeChromiumSwitches(
    const std::vector<std::string>& userArguments);

// True when `arguments` already contains a Chromium switch, meaning the user
// has taken over this decision in launch settings and BoxedVN must not append
// its own. Matching is on the "--switch" form only: a bare path or a game's
// own argument is not a Chromium switch.
bool argumentsCarryChromiumSwitch(const std::vector<std::string>& arguments);

}  // namespace boxedvn

#endif  // BOXEDVN_ENGINE_PROFILE_H
