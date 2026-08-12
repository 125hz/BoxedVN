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
  var n = 0, frames = 0, ready = "pending";
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

  setInterval(function () {
    say("boot " + (++n)
      + " raf=" + frames
      + " docState=" + ask(function () { return document.readyState; })
      + " fontsReady=" + ready
      + " fontsStatus=" + ask(function () { return document.fonts.status; })
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

BootDiagnosticsResult setBootDiagnostics(const std::string& gameDirectory,
                                         bool enabled) {
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

    const fs::path backup =
        document.string() + kBootDiagnosticsBackupSuffix;
    const bool haveBackup = fs::is_regular_file(backup, ec);
    ec.clear();

    if (!enabled) {
        if (!haveBackup) {
            result.ok = true;
            return result;
        }
        // Restore the original bytes rather than trying to remove the
        // injected text from the current file: an edit made in between would
        // otherwise be silently reverted or, worse, half-reverted.
        std::string original;
        if (!readWholeFile(backup, original)) {
            result.error = "Could not read the saved copy of " +
                           result.documentPath + ".";
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

    std::string html;
    if (!readWholeFile(document, html)) {
        result.error = "Could not read " + result.documentPath + ".";
        return result;
    }
    if (html.find(kBootDiagnosticsMarker) != std::string::npos) {
        result.ok = true;  // Already instrumented.
        return result;
    }

    if (!haveBackup && !writeWholeFile(backup, html)) {
        result.error = "Could not save an untouched copy of " +
                       result.documentPath +
                       ", so BoxedVN did not modify it.";
        return result;
    }

    const std::string script = bootDiagnosticsScript();
    const std::size_t close = findBodyClose(html);
    if (close == std::string::npos) {
        html += script;
    } else {
        html.insert(close, script);
    }
    if (!writeWholeFile(document, html)) {
        result.error = "Could not write " + result.documentPath + ".";
        return result;
    }

    result.ok = true;
    result.changed = true;
    return result;
}

}  // namespace boxedvn
