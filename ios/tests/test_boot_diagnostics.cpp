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

const char kMvDocument[] =
    "<!DOCTYPE html>\n<html><body><script src=\"js/main.js\"></script>"
    "</body></html>\n";

}  // namespace

BOXEDVN_TEST(boot_diagnostics_instruments_the_rpg_maker_mv_document) {
    const Scratch game("mv");
    const fs::path document = game.path() / "www" / "index.html";
    write(document, kMvDocument);

    const BootDiagnosticsResult result =
        setBootDiagnostics(game.string(), true);
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

    CHECK(setBootDiagnostics(game.string(), true).ok);
    const BootDiagnosticsResult removed =
        setBootDiagnostics(game.string(), false);
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

    CHECK(setBootDiagnostics(game.string(), true).ok);
    const std::string once = readAll(document);

    // Installing twice must not stack two copies of the probe.
    const BootDiagnosticsResult again =
        setBootDiagnostics(game.string(), true);
    CHECK(again.ok);
    CHECK(!again.changed);
    CHECK_EQ(readAll(document), once);

    CHECK(setBootDiagnostics(game.string(), false).ok);
    const BootDiagnosticsResult removedAgain =
        setBootDiagnostics(game.string(), false);
    CHECK(removedAgain.ok);
    CHECK(!removedAgain.changed);
    CHECK_EQ(readAll(document), std::string(kMvDocument));
}

BOXEDVN_TEST(boot_diagnostics_finds_the_rpg_maker_mz_layout) {
    const Scratch game("mz");
    const fs::path document = game.path() / "index.html";
    write(document, kMvDocument);

    CHECK(setBootDiagnostics(game.string(), true).changed);
    CHECK_CONTAINS(readAll(document), kBootDiagnosticsMarker);
}

BOXEDVN_TEST(boot_diagnostics_appends_when_there_is_no_body_close) {
    const Scratch game("nobody");
    const fs::path document = game.path() / "www" / "index.html";
    write(document, "<html><script src=\"js/main.js\"></script>");

    CHECK(setBootDiagnostics(game.string(), true).changed);
    CHECK_CONTAINS(readAll(document), kBootDiagnosticsMarker);
}

BOXEDVN_TEST(boot_diagnostics_leaves_a_game_with_no_document_alone) {
    // A native Win32 game. Nothing to instrument, and that is not an error -
    // this runs on every launch, including for guests it does not apply to.
    const Scratch game("native");
    write(game.path() / "Game.exe", "MZ");

    const BootDiagnosticsResult result =
        setBootDiagnostics(game.string(), true);
    CHECK(result.ok);
    CHECK(!result.changed);
    CHECK(result.documentPath.empty());

    CHECK(setBootDiagnostics("/boxedvn/no/such/game", true).ok);
    CHECK(setBootDiagnostics("", true).ok);
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
