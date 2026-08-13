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

#include "boxedvn/engine_profile.h"

#include "BVNLaunchArguments.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace boxedvn;

namespace {

// A scratch directory that removes itself, so a failing assertion cannot
// leave a tree behind for the next test to trip over.
class TemporaryTree {
public:
    explicit TemporaryTree(const char* name) {
        std::error_code ec;
        root_ = fs::temp_directory_path(ec) / "boxedvn_engine_tests" / name;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
    }

    ~TemporaryTree() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    TemporaryTree(const TemporaryTree&) = delete;
    TemporaryTree& operator=(const TemporaryTree&) = delete;

    const fs::path& path() const { return root_; }
    std::string string() const { return root_.string(); }

    // Creates `relative` and every directory above it, with token contents.
    // Contents are irrelevant to this detector: every marker is a name.
    void addFile(const std::string& relative) const {
        const fs::path target = root_ / fs::path(relative);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary);
        out << "x";
    }

private:
    fs::path root_;
};

}  // namespace

BOXEDVN_TEST(engineForFileNameRecognisesNwJs) {
    CHECK(engineForFileName("nw.dll") == GuestEngine::NwJs);
    CHECK(engineForFileName("nw.pak") == GuestEngine::NwJs);
    CHECK(engineForFileName("package.nw") == GuestEngine::NwJs);
    CHECK(engineForFileName("nw_elf.dll") == GuestEngine::NwJs);
    // Shipped file names are not consistently cased across NW.js versions.
    CHECK(engineForFileName("NW.DLL") == GuestEngine::NwJs);
    CHECK(engineForFileName("Package.NW") == GuestEngine::NwJs);
}

BOXEDVN_TEST(engineForFileNameRecognisesElectron) {
    CHECK(engineForFileName("app.asar") == GuestEngine::Electron);
    CHECK(engineForFileName("electron.asar") == GuestEngine::Electron);
}

BOXEDVN_TEST(engineForFileNameRejectsOrdinaryGameFiles) {
    // A native Win32 game must never pick up Chromium switches, so the
    // markers have to be exact rather than substring matches.
    CHECK(engineForFileName("game.exe") == GuestEngine::Unknown);
    CHECK(engineForFileName("d3d11.dll") == GuestEngine::Unknown);
    CHECK(engineForFileName("nwindow.dll") == GuestEngine::Unknown);
    CHECK(engineForFileName("mynw.dll") == GuestEngine::Unknown);
    CHECK(engineForFileName("asar") == GuestEngine::Unknown);
    CHECK(engineForFileName("") == GuestEngine::Unknown);
}

BOXEDVN_TEST(detectGuestEngineFindsRpgMakerMvLayout) {
    // The layout RPG Maker MV deploys for Windows: the NW.js runtime at the
    // top level, the game under www/.
    const TemporaryTree tree("rpgmaker_mv");
    tree.addFile("Game.exe");
    tree.addFile("nw.dll");
    tree.addFile("d3dcompiler_47.dll");
    tree.addFile("www/index.html");
    tree.addFile("www/js/rpg_core.js");
    tree.addFile("www/js/rpg_managers.js");

    const GuestEngineProfile profile = detectGuestEngine(tree.string());
    CHECK(profile.engine == GuestEngine::NwJs);
    CHECK(profile.framework == GuestFramework::RpgMakerMv);
    CHECK(profile.isChromium());
    CHECK(!profile.reachedLimit);
    CHECK_CONTAINS(profile.evidence, "nw.dll");
}

BOXEDVN_TEST(detectGuestEngineFindsRpgMakerMzLayout) {
    const TemporaryTree tree("rpgmaker_mz");
    tree.addFile("Game.exe");
    tree.addFile("package.nw");
    tree.addFile("js/rmmz_core.js");

    const GuestEngineProfile profile = detectGuestEngine(tree.string());
    CHECK(profile.engine == GuestEngine::NwJs);
    CHECK(profile.framework == GuestFramework::RpgMakerMz);
}

BOXEDVN_TEST(detectGuestEngineReportsEngineWithoutFramework) {
    // An NW.js application that is not RPG Maker still needs the switches;
    // only the log line is less specific.
    const TemporaryTree tree("plain_nwjs");
    tree.addFile("Launcher.exe");
    tree.addFile("nw.pak");

    const GuestEngineProfile profile = detectGuestEngine(tree.string());
    CHECK(profile.engine == GuestEngine::NwJs);
    CHECK(profile.framework == GuestFramework::Unknown);
    CHECK(profile.isChromium());
}

BOXEDVN_TEST(detectGuestEngineLeavesNativeGamesAlone) {
    const TemporaryTree tree("native_win32");
    tree.addFile("Game.exe");
    tree.addFile("d3d9.dll");
    tree.addFile("data/scripts.dat");

    const GuestEngineProfile profile = detectGuestEngine(tree.string());
    CHECK(profile.engine == GuestEngine::Unknown);
    CHECK(!profile.isChromium());
    CHECK(profile.evidence.empty());
}

BOXEDVN_TEST(detectGuestEngineHandlesMissingDirectory) {
    const GuestEngineProfile absent =
        detectGuestEngine("/boxedvn/no/such/directory");
    CHECK(absent.engine == GuestEngine::Unknown);
    CHECK_EQ(absent.filesInspected, static_cast<std::size_t>(0));

    const GuestEngineProfile empty = detectGuestEngine("");
    CHECK(empty.engine == GuestEngine::Unknown);
}

BOXEDVN_TEST(detectGuestEngineReportsWhenItStoppedEarly) {
    // A scan that ran out of budget before finding a marker must say so.
    // Treating that as "not a browser engine" is how a game silently loses
    // the switches it needed.
    const TemporaryTree tree("bounded");
    for (int index = 0; index < 8; ++index) {
        tree.addFile("assets/file" + std::to_string(index) + ".dat");
    }
    tree.addFile("nw.dll");

    const GuestEngineProfile profile =
        detectGuestEngine(tree.string(), /*maxFiles=*/2);
    CHECK(profile.reachedLimit);
    CHECK_EQ(profile.filesInspected, static_cast<std::size_t>(2));
}

BOXEDVN_TEST(detectGuestEngineRespectsDepthLimit) {
    const TemporaryTree tree("deep");
    tree.addFile("Game.exe");
    tree.addFile("a/b/c/d/e/nw.dll");

    CHECK(detectGuestEngine(tree.string(), 4096, /*maxDepth=*/2).engine ==
          GuestEngine::Unknown);
    CHECK(detectGuestEngine(tree.string(), 4096, /*maxDepth=*/8).engine ==
          GuestEngine::NwJs);
}

BOXEDVN_TEST(chromiumSwitchesCoverTheKnownDeviceFailures) {
    const std::vector<std::string> options = chromiumCompatibilitySwitches();
    const std::string joined = [&options] {
        std::string all;
        for (const std::string& option : options) {
            all += option;
            all += ' ';
        }
        return all;
    }();

    // Each of these is tied to an observed device failure; see the header.
    CHECK_CONTAINS(joined, "--no-sandbox");
    CHECK_CONTAINS(joined, "--disable-gpu ");
    CHECK_CONTAINS(joined, "--disable-software-rasterizer");
    CHECK_CONTAINS(joined, "--disable-direct-composition");
    CHECK_CONTAINS(joined, "CalculateNativeWinOcclusion");
    CHECK_CONTAINS(joined, "--disable-background-timer-throttling");
    CHECK_CONTAINS(joined, "--disable-renderer-backgrounding");
    CHECK_CONTAINS(joined, "--disable-backgrounding-occluded-windows");

    // Chromium resolves a repeated --disable-features to the last one it
    // sees, so the set must never carry two of them.
    std::size_t featureSwitches = 0;
    for (const std::string& option : options) {
        if (option.rfind("--disable-features=", 0) == 0) {
            featureSwitches++;
        }
    }
    CHECK_EQ(featureSwitches, static_cast<std::size_t>(1));

    // Every entry must be a switch. A bare word here would be handed to the
    // game as a positional argument, and NW.js treats the first positional as
    // the application to open.
    for (const std::string& option : options) {
        CHECK(option.rfind("--", 0) == 0);
    }
}

BOXEDVN_TEST(argumentsCarryChromiumSwitchDetectsAUserOverride) {
    CHECK(argumentsCarryChromiumSwitch({"--disable-gpu"}));
    CHECK(argumentsCarryChromiumSwitch({"save.dat", "--no-sandbox"}));
    CHECK(!argumentsCarryChromiumSwitch({}));
    CHECK(!argumentsCarryChromiumSwitch({"save.dat"}));
    // "--" alone ends switch parsing; it is not a switch.
    CHECK(!argumentsCarryChromiumSwitch({"--"}));
    CHECK(!argumentsCarryChromiumSwitch({"-window"}));
}

BOXEDVN_TEST(engineProfileAppendsSwitchesForAnImportedNwJsGame) {
    const TemporaryTree tree("launch_nwjs");
    tree.addFile("Game.exe");
    tree.addFile("nw.dll");

    BVNLaunchConfiguration launch;
    launch.runThroughWine = true;
    launch.gameDirectoryHostPath = tree.string();
    launch.executablePath = "d:\\Game.exe";

    const BVNEngineProfileResult result =
        BVNApplyEngineCompatibilityProfile(launch);
    CHECK(result.applied);
    CHECK_CONTAINS(result.reason, "NW.js");
    CHECK_EQ(launch.arguments.size(),
             chromiumCompatibilitySwitches().size());
    CHECK_EQ(launch.arguments[0], std::string("--no-sandbox"));
    CHECK(launch.interpretAnonymousExecutable);

    // The switches must reach the guest after the executable, which is where
    // Chromium parses its command line from.
    const std::vector<std::string> argv = BVNBuildLaunchArguments(launch);
    const auto executable =
        std::find(argv.begin(), argv.end(), std::string("d:\\Game.exe"));
    const auto sandbox =
        std::find(argv.begin(), argv.end(), std::string("--no-sandbox"));
    CHECK(executable != argv.end());
    CHECK(sandbox != argv.end());
    CHECK(executable < sandbox);
    CHECK(std::find(argv.begin(), argv.end(),
                    std::string("-interpreterAnonymousExecutable")) !=
          argv.end());
}

BOXEDVN_TEST(engineProfileAddsToTheUsersOwnSwitchesRatherThanStandingDown) {
    const TemporaryTree tree("launch_merge");
    tree.addFile("Game.exe");
    tree.addFile("nw.dll");

    BVNLaunchConfiguration launch;
    launch.runThroughWine = true;
    launch.gameDirectoryHostPath = tree.string();
    launch.executablePath = "d:\\Game.exe";
    // A diagnostic switch, which is how console output reaches the session
    // log. Adding it must not cost the compatibility switches: an earlier
    // version stood down entirely here, so turning on logging silently broke
    // the guest being debugged.
    launch.arguments.push_back("--enable-logging=stderr");

    const BVNEngineProfileResult result =
        BVNApplyEngineCompatibilityProfile(launch);
    CHECK(result.applied);
    CHECK_EQ(launch.arguments.size(),
             chromiumCompatibilitySwitches().size() + 1);
    CHECK_EQ(launch.arguments[0], std::string("--enable-logging=stderr"));

    const auto has = [&launch](const std::string& option) {
        return std::find(launch.arguments.begin(), launch.arguments.end(),
                         option) != launch.arguments.end();
    };
    CHECK(has("--no-sandbox"));
    CHECK(has("--disable-gpu"));
}

BOXEDVN_TEST(engineProfileLetsAUserSwitchWinOverItsOwn) {
    const TemporaryTree tree("launch_override");
    tree.addFile("Game.exe");
    tree.addFile("nw.dll");

    BVNLaunchConfiguration launch;
    launch.runThroughWine = true;
    launch.gameDirectoryHostPath = tree.string();
    launch.executablePath = "d:\\Game.exe";
    launch.arguments.push_back("--in-process-gpu");
    launch.arguments.push_back("--disable-gpu");

    BVNApplyEngineCompatibilityProfile(launch);

    // Exactly one --disable-gpu: the user's. A duplicate would be harmless to
    // Chromium but would make the log lie about who chose what.
    CHECK_EQ(std::count(launch.arguments.begin(), launch.arguments.end(),
                        std::string("--disable-gpu")),
             1);
    CHECK(std::find(launch.arguments.begin(), launch.arguments.end(),
                    std::string("--in-process-gpu")) != launch.arguments.end());
}

BOXEDVN_TEST(engineProfileCombinesFeatureListsRatherThanReplacingThem) {
    const TemporaryTree tree("launch_features");
    tree.addFile("Game.exe");
    tree.addFile("nw.dll");

    BVNLaunchConfiguration launch;
    launch.runThroughWine = true;
    launch.gameDirectoryHostPath = tree.string();
    launch.executablePath = "d:\\Game.exe";
    launch.arguments.push_back("--disable-features=NetworkService");

    const BVNEngineProfileResult result =
        BVNApplyEngineCompatibilityProfile(launch);
    CHECK(result.applied);

    // Chromium keeps only the last --disable-features it sees, so there must
    // be exactly one, and it must carry both sides.
    std::size_t featureSwitches = 0;
    std::string combined;
    for (const std::string& argument : launch.arguments) {
        if (argument.rfind("--disable-features=", 0) == 0) {
            featureSwitches++;
            combined = argument;
        }
    }
    CHECK_EQ(featureSwitches, static_cast<std::size_t>(1));
    CHECK_CONTAINS(combined, "NetworkService");
    CHECK_CONTAINS(combined, "CalculateNativeWinOcclusion");
}

BOXEDVN_TEST(engineProfileHonoursAnExplicitOptOut) {
    const TemporaryTree tree("launch_optout");
    tree.addFile("Game.exe");
    tree.addFile("nw.dll");

    BVNLaunchConfiguration launch;
    launch.runThroughWine = true;
    launch.gameDirectoryHostPath = tree.string();
    launch.executablePath = "d:\\Game.exe";
    launch.arguments.push_back(kChromiumDefaultsOptOut);
    launch.arguments.push_back("--disable-gpu");

    const BVNEngineProfileResult result =
        BVNApplyEngineCompatibilityProfile(launch);
    CHECK(!result.applied);
    CHECK_CONTAINS(result.reason, "no default switches");
    CHECK(!launch.interpretAnonymousExecutable);

    // The opt-out is BoxedVN's own word and must never reach the guest, which
    // would reject it or treat it as an unknown switch.
    CHECK_EQ(launch.arguments.size(), static_cast<std::size_t>(1));
    CHECK_EQ(launch.arguments[0], std::string("--disable-gpu"));
}

BOXEDVN_TEST(engineProfileLeavesNativeAndNonImportedLaunchesAlone) {
    const TemporaryTree tree("launch_native");
    tree.addFile("Game.exe");
    tree.addFile("d3d9.dll");

    BVNLaunchConfiguration native;
    native.runThroughWine = true;
    native.gameDirectoryHostPath = tree.string();
    native.executablePath = "d:\\Game.exe";
    CHECK(!BVNApplyEngineCompatibilityProfile(native).applied);
    CHECK(native.arguments.empty());
    CHECK(BVNApplyEngineCompatibilityProfile(native).reason.empty());

    // Wine Notepad and anything else with no imported game directory.
    BVNLaunchConfiguration bare;
    bare.runThroughWine = true;
    bare.executablePath = "notepad";
    CHECK(!BVNApplyEngineCompatibilityProfile(bare).applied);
    CHECK(bare.arguments.empty());
}
