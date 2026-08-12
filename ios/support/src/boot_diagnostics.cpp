/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 */

#include "boxedvn/boot_diagnostics.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace boxedvn {

const char kBootDiagnosticsBackupSuffix[] = ".boxedvn-original";
const char kBootDiagnosticsMarker[] = "BOXEDVN-BOOT-DIAGNOSTICS";

namespace {

bool readWholeFile(const fs::path& path, std::string& contents) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    contents = buffer.str();
    return true;
}

bool writeWholeFile(const fs::path& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    return static_cast<bool>(stream);
}

// The layouts RPG Maker deploys: MV puts the game under www/, MZ puts it at
// the top level. Checked in that order because an MV game has both a www/
// directory and, occasionally, an unrelated index.html beside the launcher.
fs::path findDocument(const fs::path& gameDirectory) {
    std::error_code ec;
    for (const char* candidate : {"www/index.html", "index.html"}) {
        const fs::path path = gameDirectory / fs::path(candidate);
        if (fs::is_regular_file(path, ec) && !ec) {
            return path;
        }
        ec.clear();
    }
    return {};
}

// Case-insensitive search for the last "</body>", so the probe runs after
// the game's own scripts are in the document.
std::size_t findBodyClose(const std::string& html) {
    static const std::string needle = "</body>";
    std::size_t found = std::string::npos;
    std::size_t scan = 0;
    while (scan < html.size()) {
        std::size_t index = std::string::npos;
        for (std::size_t i = scan; i + needle.size() <= html.size(); ++i) {
            bool match = true;
            for (std::size_t j = 0; j < needle.size(); ++j) {
                const char left =
                    static_cast<char>(std::tolower(html[i + j]));
                if (left != needle[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                index = i;
                break;
            }
        }
        if (index == std::string::npos) {
            break;
        }
        found = index;
        scan = index + needle.size();
    }
    return found;
}

}  // namespace

std::string bootDiagnosticsScript() {
    // Deliberately defensive throughout. This runs inside somebody's game,
    // and a probe that throws would turn a stalled boot into a broken one.
    return
        "<script>/* " + std::string(kBootDiagnosticsMarker) + " */\n"
        R"JS((function () {
  var n = 0, frames = 0, ready = "pending", forced = "not-tried";
  function say(text) { try { console.log("BOXEDVN " + text); } catch (e) {} }
  function ask(fn) { try { return String(fn()); } catch (e) { return "ERR"; } }

  try {
    if (document.fonts && document.fonts.ready && document.fonts.ready.then) {
      document.fonts.ready.then(function () { ready = "resolved"; },
                                function () { ready = "rejected"; });
    } else {
      ready = "absent";
    }
  } catch (e) { ready = "ERR"; }

  try {
    (function tick() { frames++; requestAnimationFrame(tick); })();
  } catch (e) {}

  // Every @font-face the document declares, with the status Blink gives it.
  // "unloaded" means nobody has asked for it yet, "error" means the file
  // could not be used, and the difference decides whether this is the game's
  // trigger not firing or the font itself being rejected.
  function faces() {
    var out = [];
    try {
      document.fonts.forEach(function (face) {
        out.push(face.family + ":" + face.status);
      });
    } catch (e) { return "ERR"; }
    return out.length ? out.join("|") : "none";
  }

  // Ask Blink to load the game's face directly. RPG Maker triggers it with a
  // zero-pixel hidden element and then waits for check() to agree, so if that
  // trigger is what does not fire here, this both proves it and clears the
  // wait. Reported either way; it is an experiment, not a silent repair.
  setTimeout(function () {
    try {
      forced = "requested";
      document.fonts.load('10px "GameFont"').then(function (list) {
        forced = "loaded(" + (list ? list.length : 0) + ")";
      }, function (error) {
        forced = "failed(" + error + ")";
      });
    } catch (e) { forced = "threw"; }
  }, 5000);

  setInterval(function () {
    say("boot " + (++n)
      + " raf=" + frames
      + " docState=" + ask(function () { return document.readyState; })
      + " fontsReady=" + ready
      + " fontsStatus=" + ask(function () { return document.fonts.status; })
      + " forcedLoad=" + forced
      + " faces=" + faces()
      + " img=" + ask(function () { return ImageManager.isReady(); })
      + " db=" + ask(function () { return !!DataManager.isDatabaseLoaded(); })
      + " gameFont=" + ask(function () { return Graphics.isFontLoaded("GameFont"); })
      + " scene=" + ask(function () {
            return SceneManager._scene.constructor.name; })
      + " errUrl=" + ask(function () { return DataManager._errorUrl; }));
  }, 3000);
})();)JS"
        "\n</script>\n";
}

std::string fontGateShimScript() {
    return
        "<script>/* " + std::string(kBootDiagnosticsMarker) + "-FONT-FIX */\n"
        R"JS((function () {
  function say(t) { try { console.log("BOXEDVN " + t); } catch (e) {} }

  // Ask for the face directly. RPG Maker triggers it with a hidden element
  // styled font-size:0px and then waits for check() to agree; if that trigger
  // is what fails here, this is the whole repair and nothing below fires.
  try {
    document.fonts.load('10px "GameFont"').then(function (list) {
      say("fontfix requested GameFont, Blink returned " +
          (list ? list.length : 0) + " face(s)");
    }, function (error) {
      say("fontfix could not load GameFont: " + error);
    });
  } catch (e) { say("fontfix could not ask for GameFont: " + e); }

  var tries = 0;
  var timer = setInterval(function () {
    tries++;
    try {
      if (typeof Graphics === "undefined" || !Graphics.isFontLoaded) { return; }
      if (Graphics.isFontLoaded("GameFont")) {
        say("fontfix not needed; GameFont loaded on its own");
        clearInterval(timer);
        return;
      }
      if (tries >= 5) {
        // Ten seconds. RPG Maker's own non-CSS path gives up at sixty and
        // throws; this gives up sooner and continues, because a game drawn in
        // a fallback face beats a game that never starts.
        Graphics.isFontLoaded = function () { return true; };
        say("fontfix opened the boot gate after 10s; GameFont never reached " +
            "loaded status, so text may use a fallback face");
        clearInterval(timer);
      }
    } catch (e) { clearInterval(timer); }
  }, 2000);
})();)JS"
        "\n</script>\n";
}

std::string silenceAudioScript() {
    return
        "<script>/* " + std::string(kBootDiagnosticsMarker) + "-SILENCE */\n"
        R"JS((function () {
  function say(t) { try { console.log("BOXEDVN " + t); } catch (e) {} }
  var names = ["playBgm", "playBgs", "playMe", "playSe", "playStaticSe"];
  var applied = 0;
  var timer = setInterval(function () {
    try {
      if (typeof AudioManager === "undefined") { return; }
      for (var i = 0; i < names.length; i++) {
        if (typeof AudioManager[names[i]] === "function") {
          AudioManager[names[i]] = function () {};
          applied++;
        }
      }
      clearInterval(timer);
      say("silenced " + applied + " audio entry point(s); sound is off, so " +
          "the engine will not fetch or decode audio either");
    } catch (e) { clearInterval(timer); }
  }, 250);
  setTimeout(function () { clearInterval(timer); }, 30000);
})();)JS"
        "\n</script>\n";
}

BootDiagnosticsResult setGuestBootScripts(const std::string& gameDirectory,
                                          const GuestBootScripts& scripts) {
    BootDiagnosticsResult result;
    if (gameDirectory.empty()) {
        result.ok = true;
        return result;
    }

    std::error_code ec;
    const fs::path root(gameDirectory);
    if (!fs::is_directory(root, ec)) {
        result.ok = true;
        return result;
    }

    const fs::path document = findDocument(root);
    if (document.empty()) {
        // Not every guest is a browser engine, and not every browser engine
        // keeps its document where RPG Maker does. Nothing to do, and
        // nothing wrong.
        result.ok = true;
        return result;
    }
    result.documentPath = fs::relative(document, root, ec).string();
    if (ec) {
        result.documentPath = document.filename().string();
        ec.clear();
    }

    const fs::path backup = document.string() + kBootDiagnosticsBackupSuffix;
    const bool haveBackup = fs::is_regular_file(backup, ec);
    ec.clear();

    std::string current;
    if (!readWholeFile(document, current)) {
        result.error = "Could not read " + result.documentPath + ".";
        return result;
    }

    // The pristine document is always the starting point, never the file as
    // it stands. Editing the current contents is how build 104 ended up
    // running an older BoxedVN's probe on a newer build: it saw its own
    // marker, concluded the work was done, and left stale code in place for a
    // whole device run.
    std::string original = current;
    if (haveBackup && !readWholeFile(backup, original)) {
        result.error = "Could not read the saved copy of " +
                       result.documentPath + ".";
        return result;
    }

    if (!scripts.any()) {
        if (!haveBackup) {
            result.ok = true;
            return result;
        }
        if (!writeWholeFile(document, original)) {
            result.error = "Could not restore " + result.documentPath + ".";
            return result;
        }
        fs::remove(backup, ec);
        result.ok = true;
        result.changed = true;
        return result;
    }

    std::string injected;
    if (scripts.diagnostics) {
        injected += bootDiagnosticsScript();
    }
    if (scripts.fontFix) {
        injected += fontGateShimScript();
    }
    if (scripts.silenceAudio) {
        injected += silenceAudioScript();
    }

    std::string desired = original;
    const std::size_t close = findBodyClose(desired);
    if (close == std::string::npos) {
        desired += injected;
    } else {
        desired.insert(close, injected);
    }

    if (desired == current) {
        result.ok = true;  // Already exactly this, down to the byte.
        return result;
    }

    if (!haveBackup && !writeWholeFile(backup, original)) {
        result.error = "Could not save an untouched copy of " +
                       result.documentPath +
                       ", so BoxedVN did not modify it.";
        return result;
    }
    if (!writeWholeFile(document, desired)) {
        result.error = "Could not write " + result.documentPath + ".";
        return result;
    }

    result.ok = true;
    result.changed = true;
    return result;
}

}  // namespace boxedvn
