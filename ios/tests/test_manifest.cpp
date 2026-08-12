/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  GPLv2; see license.txt.
 */

#include "boxedvn/architecture.h"
#include "boxedvn/game_import.h"
#include "boxedvn/json.h"
#include "boxedvn/manifest.h"
#include "boxedvn_test.h"

using namespace boxedvn;

namespace {

GameManifest sampleManifest() {
    GameManifest manifest;
    manifest.id = "kanon";
    manifest.title = "Kanon \"Standard\" Edition";
    manifest.contentDirectory = "games/kanon/content";
    manifest.selectedExecutable = "bin/kanon.exe";
    manifest.workingDirectory = "bin";
    manifest.winePrefix = "kanon";
    manifest.arguments = {"-window", "--lang=ja"};
    manifest.environment = {"LANG=ja_JP.UTF-8"};
    manifest.requestedWidth = 800;
    manifest.requestedHeight = 600;
    manifest.importedAtUnixSeconds = 1770000000;

    ManifestExecutable executable;
    executable.relativePath = "bin/kanon.exe";
    executable.formatName = "pe32";
    executable.architecture = "x86_32";
    executable.runnable = true;
    executable.subsystem = 2;
    executable.diagnostic = "32-bit x86 Windows executable";
    manifest.discoveredExecutables.push_back(executable);
    return manifest;
}

}  // namespace

BOXEDVN_TEST(manifest_round_trips_through_json) {
    const GameManifest original = sampleManifest();
    const std::string text = serialiseManifest(original);

    const ManifestParseResult parsed = parseManifest(text);
    CHECK_EQ(parsed.ok, true);
    CHECK_EQ(parsed.error, std::string());

    const GameManifest& result = parsed.manifest;
    CHECK_EQ(result.schemaVersion, kManifestSchemaVersion);
    CHECK_EQ(result.id, original.id);
    CHECK_EQ(result.title, original.title);
    CHECK_EQ(result.contentDirectory, original.contentDirectory);
    CHECK_EQ(result.selectedExecutable, original.selectedExecutable);
    CHECK_EQ(result.workingDirectory, original.workingDirectory);
    CHECK_EQ(result.winePrefix, original.winePrefix);
    CHECK_EQ(result.backend, std::string("boxedwine-x86"));
    CHECK_EQ(result.arguments.size(), original.arguments.size());
    CHECK_EQ(result.arguments[1], std::string("--lang=ja"));
    CHECK_EQ(result.environment[0], std::string("LANG=ja_JP.UTF-8"));
    CHECK_EQ(result.requestedWidth, 800u);
    CHECK_EQ(result.requestedHeight, 600u);
    CHECK_EQ(result.importedAtUnixSeconds, static_cast<int64_t>(1770000000));
    CHECK_EQ(result.discoveredExecutables.size(), size_t(1));
    CHECK_EQ(result.discoveredExecutables[0].relativePath,
             std::string("bin/kanon.exe"));
    CHECK_EQ(result.discoveredExecutables[0].runnable, true);
}

BOXEDVN_TEST(manifest_serialisation_is_stable) {
    const GameManifest manifest = sampleManifest();
    CHECK_EQ(serialiseManifest(manifest), serialiseManifest(manifest));
}

BOXEDVN_TEST(newer_schema_version_is_refused_with_an_explanation) {
    GameManifest manifest = sampleManifest();
    manifest.schemaVersion = kManifestSchemaVersion + 1;
    const ManifestParseResult parsed = parseManifest(serialiseManifest(manifest));

    CHECK_EQ(parsed.ok, false);
    CHECK_CONTAINS(parsed.error, "Update BoxedVN");
}

BOXEDVN_TEST(manifest_without_schema_version_is_refused) {
    const ManifestParseResult parsed =
        parseManifest("{\"id\":\"x\",\"contentDirectory\":\"y\"}");
    CHECK_EQ(parsed.ok, false);
    CHECK_CONTAINS(parsed.error, "schemaVersion");
}

BOXEDVN_TEST(manifest_with_empty_id_is_refused) {
    const ManifestParseResult parsed = parseManifest(
        "{\"schemaVersion\":1,\"id\":\"\",\"contentDirectory\":\"y\"}");
    CHECK_EQ(parsed.ok, false);
    CHECK_CONTAINS(parsed.error, "'id' is empty");
}

BOXEDVN_TEST(malformed_json_is_refused_with_an_offset) {
    const ManifestParseResult parsed = parseManifest("{\"schemaVersion\": }");
    CHECK_EQ(parsed.ok, false);
    CHECK_CONTAINS(parsed.error, "not valid JSON");
}

BOXEDVN_TEST(non_object_manifest_is_refused) {
    const ManifestParseResult parsed = parseManifest("[1, 2, 3]");
    CHECK_EQ(parsed.ok, false);
    CHECK_CONTAINS(parsed.error, "must be a JSON object");
}

BOXEDVN_TEST(json_escapes_round_trip) {
    const json::ParseResult parsed =
        json::parse("{\"k\": \"line\\nbreak \\u00e9 \\ud83c\\udf38\"}");
    CHECK_EQ(parsed.ok, true);
    const json::Value* value = parsed.value->find("k");
    CHECK(value != nullptr);
    if (value != nullptr) {
        CHECK_EQ(value->stringValue,
                 std::string("line\nbreak \xc3\xa9 \xf0\x9f\x8c\xb8"));
    }
}

BOXEDVN_TEST(json_rejects_unterminated_string) {
    const json::ParseResult parsed = json::parse("{\"k\": \"oops}");
    CHECK_EQ(parsed.ok, false);
    CHECK_CONTAINS(parsed.error, "Unterminated string");
}

BOXEDVN_TEST(json_rejects_trailing_content) {
    const json::ParseResult parsed = json::parse("{} garbage");
    CHECK_EQ(parsed.ok, false);
    CHECK_CONTAINS(parsed.error, "Trailing content");
}

// --- Backend selection -----------------------------------------------------

BOXEDVN_TEST(backend_selection_prefers_the_implemented_x86_backend) {
    CHECK_EQ(std::string(toString(selectBackend(GuestArchitecture::X86_32))),
             std::string("boxedwine-x86"));
    CHECK_EQ(std::string(toString(selectBackend(GuestArchitecture::X86_16))),
             std::string("boxedwine-x86"));
}

BOXEDVN_TEST(no_backend_claims_to_run_x86_64) {
    CHECK_EQ(std::string(toString(selectBackend(GuestArchitecture::X86_64))),
             std::string("none"));
    for (const RuntimeBackendCapabilities& caps : allBackends()) {
        CHECK_EQ(caps.canExecute(GuestArchitecture::X86_64), false);
    }
}

BOXEDVN_TEST(the_reserved_x64_backend_reports_itself_as_unimplemented) {
    const RuntimeBackendCapabilities caps = futureX64Capabilities();
    CHECK_EQ(caps.implemented, false);
    CHECK_EQ(caps.executableArchitectures.empty(), true);
}

// --- Identifier derivation -------------------------------------------------

BOXEDVN_TEST(identifiers_are_filesystem_safe) {
    CHECK_EQ(makeIdentifier("Kanon (Standard Edition)"),
             std::string("kanon-standard-edition"));
    CHECK_EQ(makeIdentifier("  ///  "), std::string("game"));
    CHECK_EQ(makeIdentifier(""), std::string("game"));
    CHECK_EQ(makeIdentifier(std::string(200, 'a')).size(), size_t(64));
}

BOXEDVN_TEST(support_executables_are_demoted) {
    CHECK_EQ(looksLikeSupportExecutable("unins000.exe"), true);
    CHECK_EQ(looksLikeSupportExecutable("redist/vcredist_x86.exe"), true);
    CHECK_EQ(looksLikeSupportExecutable("kanon.exe"), false);
}

BOXEDVN_TEST(installed_game_candidates_exclude_wine_and_setup_helpers) {
    CHECK_EQ(looksLikeInstalledGameExecutable(
                 "Program Files/My VN/game.exe"), true);
    CHECK_EQ(looksLikeInstalledGameExecutable(
                 "users/username/AppData/Local/My VN/game.exe"), true);
    CHECK_EQ(looksLikeInstalledGameExecutable(
                 "windows/system32/notepad.exe"), false);
    CHECK_EQ(looksLikeInstalledGameExecutable(
                 "Program Files/Common Files/helper.exe"), false);
    CHECK_EQ(looksLikeInstalledGameExecutable(
                 "users/username/AppData/Local/Temp/setup.exe"), false);
    CHECK_EQ(looksLikeInstalledGameExecutable(
                 "Program Files/My VN/unins000.exe"), false);
}

BOXEDVN_TEST(build_manifest_picks_the_first_runnable_executable) {
    std::vector<DiscoveredExecutable> discovered;

    DiscoveredExecutable notRunnable;
    notRunnable.relativePath = "setup64.exe";
    notRunnable.info.architecture = GuestArchitecture::X86_64;
    notRunnable.info.runnable = false;

    DiscoveredExecutable runnable;
    runnable.relativePath = "bin/game.exe";
    runnable.info.architecture = GuestArchitecture::X86_32;
    runnable.info.runnable = true;

    discovered.push_back(notRunnable);
    discovered.push_back(runnable);

    const GameManifest manifest =
        buildManifest("game", "Game", "games/game/content", discovered, "", 42);

    CHECK_EQ(manifest.selectedExecutable, std::string("bin/game.exe"));
    CHECK_EQ(manifest.workingDirectory, std::string("bin"));
    CHECK_EQ(manifest.discoveredExecutables.size(), size_t(2));
    CHECK_EQ(manifest.importedAtUnixSeconds, static_cast<int64_t>(42));
}

BOXEDVN_TEST(build_manifest_leaves_selection_empty_when_nothing_is_runnable) {
    std::vector<DiscoveredExecutable> discovered;
    DiscoveredExecutable notRunnable;
    notRunnable.relativePath = "game64.exe";
    notRunnable.info.architecture = GuestArchitecture::X86_64;
    notRunnable.info.runnable = false;
    discovered.push_back(notRunnable);

    const GameManifest manifest =
        buildManifest("game", "Game", "games/game/content", discovered, "", 0);

    CHECK_EQ(manifest.selectedExecutable, std::string());
    CHECK_EQ(manifest.discoveredExecutables.size(), size_t(1));
}
