/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 */

#include "boxedvn_test.h"

#include "boxedvn/boot_diagnostics.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using namespace boxedvn;

namespace {

std::string readAll(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void write(const fs::path& path, const std::string& contents) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << contents;
}

class Scratch {
public:
    explicit Scratch(const char* name) {
        std::error_code ec;
        root_ = fs::temp_directory_path(ec) / "boxedvn_boot_diagnostics" / name;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    const fs::path& path() const { return root_; }
    std::string string() const { return root_.string(); }

private:
    fs::path root_;
};

GuestBootScripts diagnostics() {
    GuestBootScripts scripts;
    scripts.diagnostics = true;
    return scripts;
}

const char kMvDocument[] =
    "<!DOCTYPE html>\n<html><body><script src=\"js/main.js\"></script>"
    "</body></html>\n";

}  // namespace

BOXEDVN_TEST(boot_diagnostics_instruments_the_rpg_maker_mv_document) {
    const Scratch game("mv");
    const fs::path document = game.path() / "www" / "index.html";
    write(document, kMvDocument);

    const BootDiagnosticsResult result =
        setGuestBootScripts(game.string(), diagnostics());
    CHECK(result.ok);
    CHECK(result.changed);
    CHECK_CONTAINS(result.documentPath, "index.html");

    const std::string html = readAll(document);
    CHECK_CONTAINS(html, kBootDiagnosticsMarker);
    // The game's own scripts must still be there, and the probe has to run
    // after them rather than replacing the document.
    CHECK_CONTAINS(html, "js/main.js");
    CHECK(html.find(kBootDiagnosticsMarker) > html.find("js/main.js"));
    CHECK(html.find("</body>") != std::string::npos);

    // An untouched copy has to exist before anything is modified, or a
    // failure part way through would leave the user without their game.
    const fs::path backup =
        document.string() + kBootDiagnosticsBackupSuffix;
    std::error_code ec;
    CHECK(fs::is_regular_file(backup, ec));
    CHECK_EQ(readAll(backup), std::string(kMvDocument));
}

BOXEDVN_TEST(boot_diagnostics_restores_the_original_byte_for_byte) {
    const Scratch game("restore");
    const fs::path document = game.path() / "www" / "index.html";
    write(document, kMvDocument);

    CHECK(setGuestBootScripts(game.string(), diagnostics()).ok);
    const BootDiagnosticsResult removed =
        setGuestBootScripts(game.string(), GuestBootScripts{});
    CHECK(removed.ok);
    CHECK(removed.changed);

    // Byte for byte, from the saved copy - not by trying to un-edit the
    // injected text out of the current file.
    CHECK_EQ(readAll(document), std::string(kMvDocument));
    std::error_code ec;
    CHECK(!fs::exists(document.string() + kBootDiagnosticsBackupSuffix, ec));
}

BOXEDVN_TEST(boot_diagnostics_is_idempotent_in_both_directions) {
    const Scratch game("idempotent");
    const fs::path document = game.path() / "www" / "index.html";
    write(document, kMvDocument);

    CHECK(setGuestBootScripts(game.string(), diagnostics()).ok);
    const std::string once = readAll(document);

    // Installing twice must not stack two copies of the probe.
    const BootDiagnosticsResult again =
        setGuestBootScripts(game.string(), diagnostics());
    CHECK(again.ok);
    CHECK(!again.changed);
    CHECK_EQ(readAll(document), once);

    CHECK(setGuestBootScripts(game.string(), GuestBootScripts{}).ok);
    const BootDiagnosticsResult removedAgain =
        setGuestBootScripts(game.string(), GuestBootScripts{});
    CHECK(removedAgain.ok);
    CHECK(!removedAgain.changed);
    CHECK_EQ(readAll(document), std::string(kMvDocument));
}

BOXEDVN_TEST(boot_diagnostics_finds_the_rpg_maker_mz_layout) {
    const Scratch game("mz");
    const fs::path document = game.path() / "index.html";
    write(document, kMvDocument);

    CHECK(setGuestBootScripts(game.string(), diagnostics()).changed);
    CHECK_CONTAINS(readAll(document), kBootDiagnosticsMarker);
}

BOXEDVN_TEST(boot_diagnostics_appends_when_there_is_no_body_close) {
    const Scratch game("nobody");
    const fs::path document = game.path() / "www" / "index.html";
    write(document, "<html><script src=\"js/main.js\"></script>");

    CHECK(setGuestBootScripts(game.string(), diagnostics()).changed);
    CHECK_CONTAINS(readAll(document), kBootDiagnosticsMarker);
}

BOXEDVN_TEST(boot_diagnostics_leaves_a_game_with_no_document_alone) {
    // A native Win32 game. Nothing to instrument, and that is not an error -
    // this runs on every launch, including for guests it does not apply to.
    const Scratch game("native");
    write(game.path() / "Game.exe", "MZ");

    const BootDiagnosticsResult result =
        setGuestBootScripts(game.string(), diagnostics());
    CHECK(result.ok);
    CHECK(!result.changed);
    CHECK(result.documentPath.empty());

    CHECK(setGuestBootScripts("/boxedvn/no/such/game", diagnostics()).ok);
    CHECK(setGuestBootScripts("", diagnostics()).ok);
}

BOXEDVN_TEST(boot_diagnostics_script_reports_every_gate_that_matters) {
    const std::string script = bootDiagnosticsScript();
    // Each of these corresponds to a question a device run could not answer
    // without it; see the header.
    CHECK_CONTAINS(script, "raf=");
    CHECK_CONTAINS(script, "fontsReady=");
    CHECK_CONTAINS(script, "gameFont=");
    CHECK_CONTAINS(script, "db=");
    CHECK_CONTAINS(script, "scene=");
    // Added after a device run showed every other gate healthy and only the
    // game's own web font unresolved: the face list says whether Blink ever
    // tried to load it, and the forced load says whether it can be loaded.
    CHECK_CONTAINS(script, "faces=");
    CHECK_CONTAINS(script, "forcedLoad=");
    CHECK_CONTAINS(script, "GameFont");
    CHECK_CONTAINS(script, kBootDiagnosticsMarker);
    // It runs inside somebody's game: every read is guarded, so a guest that
    // is not RPG Maker reports what it can instead of throwing.
    CHECK_CONTAINS(script, "catch");
}

BOXEDVN_TEST(boot_scripts_refresh_when_the_injected_text_changes) {
    // Build 104 skipped re-injection whenever it saw its own marker, so a
    // device running a newer BoxedVN kept executing an older probe and a
    // whole run reported fields that had been replaced. Installation must
    // rebuild from the saved original, not recognise itself and stop.
    const Scratch game("refresh");
    const fs::path document = game.path() / "www" / "index.html";
    write(document, kMvDocument);

    GuestBootScripts probe;
    probe.diagnostics = true;
    CHECK(setGuestBootScripts(game.string(), probe).changed);

    GuestBootScripts both;
    both.diagnostics = true;
    both.fontFix = true;
    const BootDiagnosticsResult updated =
        setGuestBootScripts(game.string(), both);
    CHECK(updated.ok);
    CHECK(updated.changed);

    const std::string html = readAll(document);
    CHECK_CONTAINS(html, "fontfix");
    // Exactly one copy of the probe: rebuilding from the original is what
    // stops repeated installs from stacking.
    const std::string marker(kBootDiagnosticsMarker);
    std::size_t count = 0, at = 0;
    while ((at = html.find(marker, at)) != std::string::npos) {
        count++;
        at += marker.size();
    }
    CHECK_EQ(count, static_cast<std::size_t>(2));  // probe + font fix banner

    CHECK(setGuestBootScripts(game.string(), GuestBootScripts{}).changed);
    CHECK_EQ(readAll(document), std::string(kMvDocument));
}

BOXEDVN_TEST(font_gate_shim_opens_the_gate_and_says_so) {
    const std::string script = fontGateShimScript();
    // It must ask Blink for the face before overriding anything: if the load
    // succeeds there is nothing to work around.
    CHECK_CONTAINS(script, "document.fonts.load");
    CHECK_CONTAINS(script, "GameFont");
    CHECK_CONTAINS(script, "Graphics.isFontLoaded");
    // And it must report what it did. A silent behaviour change inside
    // somebody's game is not acceptable even when it is the right one.
    CHECK_CONTAINS(script, "fontfix");
    CHECK_CONTAINS(script, "fallback");
}

BOXEDVN_TEST(font_gate_shim_installs_without_diagnostics) {
    const Scratch game("fontfix_only");
    const fs::path document = game.path() / "www" / "index.html";
    write(document, kMvDocument);

    GuestBootScripts scripts;
    scripts.fontFix = true;
    CHECK(setGuestBootScripts(game.string(), scripts).changed);

    const std::string html = readAll(document);
    CHECK_CONTAINS(html, "fontfix");
    // The reporting probe is a separate opt-in and must not come along.
    CHECK(html.find("BOXEDVN boot") == std::string::npos);
}

BOXEDVN_TEST(silence_audio_shim_stops_the_engine_asking_for_sound) {
    const std::string script = silenceAudioScript();
    // The point is to prevent the decode, not to mute the output, so it has
    // to reach the engine's play entry points rather than a volume.
    CHECK_CONTAINS(script, "AudioManager");
    CHECK_CONTAINS(script, "playBgm");
    CHECK_CONTAINS(script, "playSe");
    CHECK_CONTAINS(script, "BOXEDVN");
    // It must give up rather than poll a guest that is not RPG Maker forever.
    CHECK_CONTAINS(script, "clearInterval");
}

BOXEDVN_TEST(silence_audio_installs_independently_of_the_other_scripts) {
    const Scratch game("silence_only");
    const fs::path document = game.path() / "www" / "index.html";
    write(document, kMvDocument);

    GuestBootScripts scripts;
    scripts.silenceAudio = true;
    CHECK(setGuestBootScripts(game.string(), scripts).changed);

    const std::string html = readAll(document);
    CHECK_CONTAINS(html, "AudioManager");
    CHECK(html.find("BOXEDVN boot") == std::string::npos);
    CHECK(html.find("fontfix") == std::string::npos);

    CHECK(setGuestBootScripts(game.string(), GuestBootScripts{}).changed);
    CHECK_EQ(readAll(document), std::string(kMvDocument));
}

BOXEDVN_TEST(boot_diagnostics_reports_which_resources_failed) {
    const std::string script = bootDiagnosticsScript();
    // "img=false" says the engine is not ready; it does not say what is not
    // ready. A title screen with no buttons needs the failing URLs.
    CHECK_CONTAINS(script, "ImageManager._imageCache");
    CHECK_CONTAINS(script, "_loadingState");
    CHECK_CONTAINS(script, "bitmaps=");
    CHECK_CONTAINS(script, "failed=");
    // Encrypted assets decrypt in JavaScript before decoding, so whether
    // this title uses them changes where a broken image can come from.
    CHECK_CONTAINS(script, "hasEncryptedImages");
}
