/*
 * BoxedVN - launch command regression tests.
 * GPLv2; see license.txt.
 */

#include "boxedvn_test.h"

#include "BVNLaunchArguments.h"
#include "../../source/emulation/cpu/common/anonymousCodePolicy.h"

#include <algorithm>

BOXEDVN_TEST(anonymous_browser_policy_keeps_system_relays_on_jit) {
    CHECK(shouldInterpretAnonymousExecutableAddress(0x386cbacfu));
    CHECK(shouldInterpretAnonymousExecutableAddress(0x4350cef4u));
    CHECK(!shouldInterpretAnonymousExecutableAddress(0x7d400000u));
    CHECK(!shouldInterpretAnonymousExecutableAddress(0x7d401298u));
    CHECK(!shouldInterpretAnonymousExecutableAddress(0xf0014190u));
    CHECK(shouldInterpretAnonymousExecutableAddress(0x7d410000u));
}

BOXEDVN_TEST(imported_game_keeps_wined3d_vulkan_when_profile_disables_dxvk) {
    BVNLaunchConfiguration launch;
    launch.runThroughWine = true;
    launch.gameDirectoryHostPath = "/games/grisaia";
    launch.executablePath = "/bin/wine";
    launch.arguments = {"D:\\BootMenu.exe"};

    BVNApplyDefaultRendererPolicy(launch);
    CHECK(launch.useWineD3DVulkanRenderer);
    CHECK(!launch.enableWineD3DVulkan);

    CHECK(BVNApplyKnownCompatibilityProfile(launch));
    CHECK(launch.useWineD3DVulkanRenderer);
    CHECK(!launch.enableWineD3DVulkan);
}

BOXEDVN_TEST(automatic_renderer_uses_wined3d_for_unknown_imported_games) {
    BVNLaunchConfiguration launch;
    launch.runThroughWine = true;
    launch.gameDirectoryHostPath = "/games/white-album-2";
    launch.executablePath = "D:\\WA2.exe";

    BVNApplyDefaultRendererPolicy(launch);

    CHECK_EQ(launch.useWineD3DVulkanRenderer, true);
    CHECK_EQ(launch.enableWineD3DVulkan, false);
}

BOXEDVN_TEST(renderer_override_can_force_dxvk_for_an_imported_game) {
    BVNLaunchConfiguration launch;
    launch.runThroughWine = true;
    launch.gameDirectoryHostPath = "/games/unknown-d3d11-game";
    launch.executablePath = "D:\\game.exe";
    launch.requestedWineRenderer = 2;

    BVNApplyDefaultRendererPolicy(launch);

    CHECK_EQ(launch.enableWineD3DVulkan, true);
}

// The automatic choice is now made from what the game's binaries link, so it
// is tested against real files in test_direct3d_profile.cpp rather than
// against a list of executable names here.

BOXEDVN_TEST(wine_notepad_command_uses_valueless_nozip_switch) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs/boxedwine.zip";
    launch.writableRootPath = "/prefixes/default";
    launch.executablePath = "notepad";
    launch.width = 800;
    launch.height = 600;
    launch.bitsPerPixel = 32;

    const std::vector<std::string> actual =
        BVNBuildLaunchArguments(launch);
    const std::vector<std::string> expected = {
        "boxedvn", "-root", "/prefixes/default",
        "-zip", "/rootfs/boxedwine.zip",
        "-nozip",
        "-resolution", "800x600",
        "-bpp", "32",
        "/bin/wine", "notepad",
    };

    CHECK(actual == expected);
}

BOXEDVN_TEST(fex64_launch_mounts_runtime_layers_and_enters_wine64) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs/boxedwine.zip";
    launch.rootFilesystemOverlayZipPaths = {
        "/runtime/glibc-rootfs64.zip",
        "/runtime/wine64.zip",
    };
    launch.writableRootPath = "/prefixes/x64";
    launch.executablePath = "d:\\probe.exe";
    launch.runThroughWine = true;
    launch.useFEX64 = true;

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    const std::vector<std::string> expected = {
        "boxedvn", "-root", "/prefixes/x64",
        "-zip", "/rootfs/boxedwine.zip",
        "-zip", "/runtime/glibc-rootfs64.zip",
        "-zip", "/runtime/wine64.zip",
        "-nozip", "-env", "BOXEDWINE_CPU64=fex",
        "-env", "WINEPREFIX=/home/username/.wine64",
        "-env", "WINEARCH=win64",
        "-env", "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine",
        "-env", "LD_LIBRARY_PATH=/usr/lib/boxedwine64-x11",
        "/usr/lib/x86_64-linux-gnu/wine/wine", "d:\\probe.exe",
    };
    CHECK(actual == expected);
}

BOXEDVN_TEST(game_command_preserves_mount_environment_and_arguments) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.writableRootPath = "/prefix";
    launch.gameDirectoryHostPath = "/games/example";
    launch.sharedDirectoryHostPath = "/documents/shared";
    launch.executablePath = "d:\\game.exe";
    launch.arguments = {"--language", "ja"};
    launch.environment = {"LANG=ja_JP.UTF-8"};
    launch.workingDirectory =
        "/home/username/.wine/dosdevices/d:/content";
    launch.soundEnabled = false;
    launch.enableWineD3DVulkan = true;

    const std::vector<std::string> actual =
        BVNBuildLaunchArguments(launch);
    const std::vector<std::string> expected = {
        "boxedvn", "-root", "/prefix", "-zip", "/rootfs.zip", "-nozip",
        "-mount_drive", "/games/example", "d",
        "-mount_drive", "/documents/shared", "e",
        "-resolution", "1366x1024", "-nosound",
        "-dxvk", "1", "-w",
        "/home/username/.wine/dosdevices/d:/content",
        "-env", "WINEDEBUG=warn+d3d_shader,-d3d",
        "-env", "LANG=ja_JP.UTF-8", "/bin/wine", "d:\\game.exe",
        "--language", "ja",
    };

    CHECK(actual == expected);
}

BOXEDVN_TEST(container_command_uses_configured_drive_letters_and_resolution) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.writableRootPath = "/prefixes/container-vn";
    launch.gameDirectoryHostPath = "/documents/Containers/vn/Files";
    launch.sharedDirectoryHostPath = "/documents/Shared";
    launch.gameDriveLetter = 'd';
    launch.sharedDriveLetter = 's';
    launch.executablePath = "explorer";
    launch.arguments = {"/desktop=shell,800x600", "winefile", "D:\\"};
    launch.width = 800;
    launch.height = 600;

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    const std::vector<std::string> expectedMount = {
        "-mount_drive", "/documents/Shared", "s"};
    CHECK(std::search(actual.begin(), actual.end(),
                      expectedMount.begin(), expectedMount.end())
          != actual.end());
    CHECK(std::find(actual.begin(), actual.end(), "800x600") != actual.end());
    CHECK(std::find(actual.begin(), actual.end(), "winefile") != actual.end());
}

BOXEDVN_TEST(explicit_game_resolution_overrides_ios_default_monitor) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.writableRootPath = "/prefix";
    launch.gameDirectoryHostPath = "/games/example";
    launch.executablePath = "d:\\game.exe";
    launch.width = 1024;
    launch.height = 576;

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    const auto option = std::find(actual.begin(), actual.end(), "-resolution");
    CHECK(option != actual.end());
    CHECK(option + 1 != actual.end());
    CHECK(*(option + 1) == "1024x576");
    CHECK(std::count(actual.begin(), actual.end(), "-resolution") == 1);
}

BOXEDVN_TEST(explicit_wine_dll_policy_is_not_overwritten) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.writableRootPath = "/prefix";
    launch.executablePath = "d:\\game.exe";
    launch.environment = {
        "WINEDLLOVERRIDES=winebth=;d3d9=n,b",
    };

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    CHECK(std::count(actual.begin(), actual.end(),
                     "WINEDLLOVERRIDES=winebth=;d3d9=n,b") == 1);
    CHECK(std::find(actual.begin(), actual.end(),
                    "WINEDLLOVERRIDES=winebth=;ddraw=n,b") == actual.end());
}

BOXEDVN_TEST(explicit_wine_debug_policy_is_preserved) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.writableRootPath = "/prefix";
    launch.executablePath = "notepad";
    launch.environment = {"WINEDEBUG=-all"};

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    CHECK(std::count(actual.begin(), actual.end(), "WINEDEBUG=-all") == 1);
}

BOXEDVN_TEST(shader_translation_failures_are_visible_by_default) {
    // Saya's stage-less pipelines proved WineD3D's DXBC -> SPIR-V step can
    // fail without saying so at Wine's default verbosity.
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.writableRootPath = "/prefix";
    launch.executablePath = "d:\\game.exe";
    launch.enableWineD3DVulkan = true;

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    const auto found = std::find_if(actual.begin(), actual.end(),
        [](const std::string& entry) {
            return entry.rfind("WINEDEBUG=", 0) == 0;
        });
    CHECK(found != actual.end());
    CHECK(found->find("d3d_shader") != std::string::npos);
}

BOXEDVN_TEST(plain_wine_apps_get_no_shader_debug_channel) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.writableRootPath = "/prefix";
    launch.executablePath = "notepad";

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    CHECK(std::none_of(actual.begin(), actual.end(),
        [](const std::string& entry) {
            return entry.rfind("WINEDEBUG=", 0) == 0;
        }));
}

BOXEDVN_TEST(default_wine_debug_policy_yields_to_the_caller) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.writableRootPath = "/prefix";
    launch.executablePath = "d:\\game.exe";
    launch.environment = {"WINEDEBUG=-all"};

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    // Exactly one WINEDEBUG, and it is the caller's.
    CHECK(std::count_if(actual.begin(), actual.end(),
        [](const std::string& entry) {
            return entry.rfind("WINEDEBUG=", 0) == 0;
        }) == 1);
    CHECK(std::count(actual.begin(), actual.end(), "WINEDEBUG=-all") == 1);
}

BOXEDVN_TEST(interpreter_module_profile_is_passed_before_the_guest_command) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.writableRootPath = "/prefix";
    launch.executablePath = "d:\\Saya_en.exe";
    launch.interpreterModules = {"mvware"};

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    const auto option = std::find(actual.begin(), actual.end(),
                                  "-interpreterModule");
    const auto command = std::find(actual.begin(), actual.end(), "/bin/wine");
    CHECK(option != actual.end());
    CHECK(option + 1 != actual.end());
    CHECK(*(option + 1) == "mvware");
    CHECK(command != actual.end());
    CHECK(option < command);
}

BOXEDVN_TEST(song_of_saya_profile_never_disables_direct3d11) {
    // Mware.dll imports d3d11.dll statically, so removing it stops the game
    // loading at all (build 44, status c0000135). Guard against reintroducing
    // that override.
    BVNLaunchConfiguration launch;
    launch.executablePath = "/bin/wine";
    launch.arguments = {"D:\\Games\\SAYA_EN.EXE"};
    CHECK(BVNApplyKnownCompatibilityProfile(launch));
    CHECK(std::none_of(launch.environment.begin(), launch.environment.end(),
        [](const std::string& entry) {
            return entry.find("d3d11=") != std::string::npos;
        }));
}

BOXEDVN_TEST(song_of_saya_interprets_d3d11_as_a_jit_diagnostic) {
    // Build 35 removed Saya's interpreter profile once the ARMv8 REP MOVS
    // defect was fixed, and a test guarded against reintroducing one. Build 57
    // reintroduces exactly one module deliberately, as a diagnostic: build 56
    // measured the stalled thread consuming 98% of a core in JIT-compiled code
    // inside d3d11.dll's BindFramebuffer loop, which cannot legitimately spin.
    // Interpreting that module tests the ARM64 translation directly.
    //
    // This is temporary. If the JIT is exonerated it must be removed again,
    // and it must never ship: interpreting DXVK is far too slow to play.
    BVNLaunchConfiguration launch;
    launch.executablePath = "/bin/wine";
    launch.arguments = {"D:\\Games\\SAYA_EN.EXE"};

    CHECK(BVNApplyKnownCompatibilityProfile(launch));
    CHECK(launch.interpreterModules.size() == 1);
    CHECK(launch.interpreterModules[0] == "d3d11");
    // No address range: module matching is by name, so it survives a different
    // load base, which an absolute range would not.
    CHECK(launch.interpreterRanges.empty());

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    const auto option = std::find(actual.begin(), actual.end(),
                                  "-interpreterModule");
    CHECK(option != actual.end());
    CHECK(option + 1 != actual.end());
    CHECK(*(option + 1) == "d3d11");
    CHECK(std::find(actual.begin(), actual.end(),
                    "-interpreterRange") == actual.end());

    // Unrelated apps must keep full JIT.
    BVNLaunchConfiguration notepad;
    notepad.executablePath = "notepad";
    CHECK(!BVNApplyKnownCompatibilityProfile(notepad));
    CHECK(notepad.interpreterModules.empty());
    CHECK(notepad.interpreterRanges.empty());
}

BOXEDVN_TEST(grisaia_profile_keeps_dxvk_out_of_a_direct3d9_engine) {
    // Build 65 on device: DXVK failed vkCreateDevice six times, every attempt
    // rejected by MoltenVK for geometryShader, shaderCullDistance,
    // robustBufferAccess2 and nullDescriptor, and the game then dereferenced
    // the interface pointer it never received. wined3d's Vulkan renderer built
    // a device in the same session, and this engine is Direct3D 9, so DXVK has
    // nothing to contribute here and is actively in the way.
    for (const char* executable : {"D:\\BootMenu.exe", "d:\\grisaia.EXE"}) {
        BVNLaunchConfiguration launch;
        launch.executablePath = "/bin/wine";
        launch.arguments = {executable};
        launch.gameDirectoryHostPath = "/games/grisaia";
        launch.useWineD3DVulkanRenderer = true;
        launch.enableWineD3DVulkan = true;

        CHECK(BVNApplyKnownCompatibilityProfile(launch));
        CHECK(!launch.enableWineD3DVulkan);
        CHECK(launch.useWineD3DVulkanRenderer);
        // It must not pick up Saya's interpreter diagnostic by accident.
        CHECK(launch.interpreterModules.empty());

        const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
        CHECK(std::find(actual.begin(), actual.end(), "-dxvk") == actual.end());
    }

    // Saya still gets DXVK: it is the title that cannot run without it.
    BVNLaunchConfiguration saya;
    saya.executablePath = "/bin/wine";
    saya.arguments = {"D:\\Games\\SAYA_EN.EXE"};
    saya.enableWineD3DVulkan = true;
    CHECK(BVNApplyKnownCompatibilityProfile(saya));
    CHECK(saya.enableWineD3DVulkan);

    // The launch-settings selector must remain authoritative even when an
    // automatic compatibility profile exists for that executable name.
    BVNLaunchConfiguration forced;
    forced.executablePath = "D:\\grisaia.exe";
    forced.requestedWineRenderer = 2;
    forced.enableWineD3DVulkan = true;
    CHECK(BVNApplyKnownCompatibilityProfile(forced));
    CHECK(forced.enableWineD3DVulkan);
}

BOXEDVN_TEST(mirrors_edge_profile_avoids_the_faulting_guest_audio_codec) {
    BVNLaunchConfiguration launch;
    launch.executablePath = "/bin/wine";
    launch.arguments = {
        "D:\\Mirrors-Edge\\Mirror's Edge\\Binaries\\MirrorsEdge.exe"};
    launch.soundEnabled = true;

    CHECK(BVNApplyKnownCompatibilityProfile(launch));
    CHECK(!launch.soundEnabled);

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    CHECK(std::find(actual.begin(), actual.end(), "-nosound") != actual.end());

    BVNLaunchConfiguration unrelated;
    unrelated.executablePath = "D:\\Games\\OtherGame.exe";
    CHECK(!BVNApplyKnownCompatibilityProfile(unrelated));
    CHECK(unrelated.soundEnabled);
}

BOXEDVN_TEST(command_does_not_let_boxedwine_truncate_frontend_log) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.writableRootPath = "/prefix";
    launch.executablePath = "notepad";

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    CHECK(std::find(actual.begin(), actual.end(), "-log") == actual.end());
}

BOXEDVN_TEST(interpret_sentinel_retires_ffmpeg_workaround_and_never_reaches_guest) {
    BVNLaunchConfiguration launch;
    launch.executablePath = "d:\\Game.exe";
    launch.arguments.push_back("--bvn-interpret=ffmpeg");
    launch.arguments.push_back("--no-sandbox");
    launch.arguments.push_back("--bvn-interpret=d3d11");

    BVNApplyGeneralArgumentSentinels(launch);

    CHECK_EQ(launch.interpreterModules.size(), static_cast<std::size_t>(1));
    CHECK_EQ(launch.interpreterModules[0], std::string("d3d11"));

    // The guest must receive only its own switches. A Chromium process given
    // an unknown --bvn- switch would at best ignore it and at worst refuse.
    CHECK_EQ(launch.arguments.size(), static_cast<std::size_t>(1));
    CHECK_EQ(launch.arguments[0], std::string("--no-sandbox"));

    const std::vector<std::string> argv = BVNBuildLaunchArguments(launch);
    for (const std::string& argument : argv) {
        CHECK(argument.rfind("--bvn-interpret=", 0) != 0);
    }
    // And the selection has to actually reach Boxedwine.
    CHECK(std::find(argv.begin(), argv.end(),
                    std::string("-interpreterModule")) != argv.end());
    CHECK(std::find(argv.begin(), argv.end(), std::string("ffmpeg")) ==
          argv.end());
    CHECK(std::find(argv.begin(), argv.end(), std::string("d3d11")) !=
          argv.end());
}

BOXEDVN_TEST(interpret_sentinel_ignores_an_empty_module_name) {
    BVNLaunchConfiguration launch;
    launch.executablePath = "d:\\Game.exe";
    launch.arguments.push_back("--bvn-interpret=");

    BVNApplyGeneralArgumentSentinels(launch);

    // A name-less request selects nothing rather than an empty module, and
    // the line is still consumed so the guest never sees it.
    CHECK(launch.interpreterModules.empty());
    CHECK_EQ(launch.arguments.size(), static_cast<std::size_t>(1));
}

BOXEDVN_TEST(interpret_range_sentinel_reaches_boxedwine) {
    BVNLaunchConfiguration launch;
    launch.executablePath = "d:\\Game.exe";
    launch.arguments.push_back("--bvn-interpret-range=76b60000-76b8e000");
    launch.arguments.push_back("--no-sandbox");

    BVNApplyGeneralArgumentSentinels(launch);

    CHECK_EQ(launch.interpreterRanges.size(), static_cast<std::size_t>(1));
    CHECK_EQ(launch.interpreterRanges[0].first, 0x76b60000u);
    CHECK_EQ(launch.interpreterRanges[0].second, 0x76b8e000u);
    CHECK_EQ(launch.arguments.size(), static_cast<std::size_t>(1));

    const std::vector<std::string> argv = BVNBuildLaunchArguments(launch);
    CHECK(std::find(argv.begin(), argv.end(),
                    std::string("-interpreterRange")) != argv.end());
    CHECK(std::find(argv.begin(), argv.end(),
                    std::string("76b60000-76b8e000")) != argv.end());
}

BOXEDVN_TEST(interpret_range_sentinel_rejects_what_it_cannot_parse) {
    // A range that is silently dropped looks exactly like a range that was
    // applied and found innocent, which is how a bisection walks confidently
    // in the wrong direction. Anything unparseable must select nothing.
    for (const char* bad : {"--bvn-interpret-range=nothex-76b8e000",
                            "--bvn-interpret-range=76b8e000-76b60000",
                            "--bvn-interpret-range=76b60000",
                            "--bvn-interpret-range=-",
                            "--bvn-interpret-range=76b60000-76b60000"}) {
        BVNLaunchConfiguration launch;
        launch.executablePath = "d:\\Game.exe";
        launch.arguments.push_back(bad);
        BVNApplyGeneralArgumentSentinels(launch);
        CHECK(launch.interpreterRanges.empty());
        // Still consumed: the guest must never see BoxedVN's own words.
        CHECK(launch.arguments.empty());
    }
}

// The root filesystem's only prefix is a 32-bit Wine installation, so Wine64
// refused to start in it: "'/home/username/.wine' is a 32-bit installation,
// it cannot support 64-bit applications". Giving the launch a separate
// writable host root was not enough, because the guest path is what Wine
// reads and that path still resolved to the bundled 32-bit prefix.

BOXEDVN_TEST(fex64_launch_gets_its_own_prefix_and_win64_arch) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.rootFilesystemOverlayZipPaths = {"/glibc.zip", "/wine64.zip"};
    launch.writableRootPath = "/prefixes/x64";
    launch.executablePath = "d:\\game.exe";
    launch.runThroughWine = true;
    launch.useFEX64 = true;

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    CHECK(std::count(actual.begin(), actual.end(),
                     "WINEPREFIX=/home/username/.wine64") == 1);
    CHECK(std::count(actual.begin(), actual.end(), "WINEARCH=win64") == 1);
    CHECK(std::count(actual.begin(), actual.end(),
                     "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine") == 1);
    // The BoxedWine X11 client libraries come first on the guest's library
    // path, so winex11.so binds to the bridge rather than the distro libX11.
    CHECK(std::count(actual.begin(), actual.end(),
                     "LD_LIBRARY_PATH=/usr/lib/boxedwine64-x11") == 1);
    CHECK(std::find(actual.begin(), actual.end(),
                    "/usr/lib/x86_64-linux-gnu/wine/wine") != actual.end());
    // The 32-bit prefix is never named, and never converted or renamed: it
    // stays exactly where it is as the default for 32-bit programs.
    CHECK(std::find(actual.begin(), actual.end(),
                    "WINEPREFIX=/home/username/.wine") == actual.end());
}

BOXEDVN_TEST(fex64_launch_keeps_a_caller_supplied_prefix_and_arch) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.rootFilesystemOverlayZipPaths = {"/glibc.zip", "/wine64.zip"};
    launch.writableRootPath = "/prefixes/x64";
    launch.executablePath = "d:\\game.exe";
    launch.runThroughWine = true;
    launch.useFEX64 = true;
    launch.environment = {
        "WINEPREFIX=/home/username/.wine-experiment",
        "WINEARCH=win32",
    };

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    CHECK(std::count(actual.begin(), actual.end(),
                     "WINEPREFIX=/home/username/.wine-experiment") == 1);
    CHECK(std::count(actual.begin(), actual.end(), "WINEARCH=win32") == 1);
    // The defaults must not be appended alongside the caller's own choice:
    // the guest would then see two assignments for the same variable.
    CHECK(std::find(actual.begin(), actual.end(),
                    "WINEPREFIX=/home/username/.wine64") == actual.end());
    CHECK(std::find(actual.begin(), actual.end(), "WINEARCH=win64") ==
          actual.end());
}

BOXEDVN_TEST(fex64_launch_keeps_a_caller_supplied_wine_dll_path) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.rootFilesystemOverlayZipPaths = {"/glibc.zip", "/wine64.zip"};
    launch.writableRootPath = "/prefixes/x64";
    launch.executablePath = "d:\\game.exe";
    launch.runThroughWine = true;
    launch.useFEX64 = true;
    launch.environment = {"WINEDLLPATH=/opt/wine-experiment"};

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    CHECK(std::count(actual.begin(), actual.end(),
                     "WINEDLLPATH=/opt/wine-experiment") == 1);
    CHECK(std::find(actual.begin(), actual.end(),
                    "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine") ==
          actual.end());
}

BOXEDVN_TEST(fex64_launch_keeps_a_caller_supplied_library_path) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.rootFilesystemOverlayZipPaths = {"/glibc.zip", "/wine64.zip"};
    launch.writableRootPath = "/prefixes/x64";
    launch.executablePath = "d:\\game.exe";
    launch.runThroughWine = true;
    launch.useFEX64 = true;
    launch.environment = {"LD_LIBRARY_PATH=/opt/x11-experiment"};

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    CHECK(std::count(actual.begin(), actual.end(),
                     "LD_LIBRARY_PATH=/opt/x11-experiment") == 1);
    CHECK(std::find(actual.begin(), actual.end(),
                    "LD_LIBRARY_PATH=/usr/lib/boxedwine64-x11") == actual.end());
}

BOXEDVN_TEST(a_dxmt_launch_names_its_module_directory_for_the_module_root_overlay) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.rootFilesystemOverlayZipPaths = {"/glibc.zip", "/wine64.zip"};
    launch.writableRootPath = "/prefixes/x64";
    launch.executablePath = "d:\\.boxedvn-x64-diagnostics\\probe.exe";
    launch.workingDirectory = "/mnt/drive_d/.boxedvn-x64-diagnostics";
    launch.runThroughWine = true;
    launch.useFEX64 = true;
    launch.useDXMT = true;

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    const auto option = std::find(actual.begin(), actual.end(), "-x64modules");
    CHECK(option != actual.end());
    if (option != actual.end() && option + 1 != actual.end()) {
        CHECK_EQ(*(option + 1), std::string("/mnt/drive_d/.boxedvn-x64-diagnostics"));
    }
    // The working directory is still passed on its own; the overlay does
    // not replace it.
    CHECK(std::find(actual.begin(), actual.end(), "-w") != actual.end());
}

BOXEDVN_TEST(a_dxmt_launch_keeps_its_module_directory_when_the_program_runs_elsewhere) {
    // Running a program of the user's own: its own folder is the working
    // directory, so its DLLs and data resolve, while the DXMT modules are
    // still projected from where the app staged them. Taking -x64modules
    // from the working directory would have projected nothing at all.
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.rootFilesystemOverlayZipPaths = {"/glibc.zip", "/wine64.zip"};
    launch.writableRootPath = "/prefixes/x64";
    launch.executablePath = "D:\\Program\\program.exe";
    launch.workingDirectory = "/mnt/drive_d/Program";
    launch.dxmtModuleDirectory = "/mnt/drive_d/.boxedvn-x64-diagnostics";
    launch.runThroughWine = true;
    launch.useFEX64 = true;
    launch.useDXMT = true;

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    const auto modules = std::find(actual.begin(), actual.end(), "-x64modules");
    CHECK(modules != actual.end());
    if (modules != actual.end() && modules + 1 != actual.end()) {
        CHECK_EQ(*(modules + 1),
                 std::string("/mnt/drive_d/.boxedvn-x64-diagnostics"));
    }
    const auto working = std::find(actual.begin(), actual.end(), "-w");
    CHECK(working != actual.end());
    if (working != actual.end() && working + 1 != actual.end()) {
        CHECK_EQ(*(working + 1), std::string("/mnt/drive_d/Program"));
    }
    CHECK_EQ(actual.back(), std::string("D:\\Program\\program.exe"));
}

BOXEDVN_TEST(a_program_on_the_prefix_drive_c_runs_from_its_own_folder) {
    // The same launch for a program on C:. The guest path of that drive is
    // the 64-bit prefix's drive_c, not /mnt/drive_d.
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.rootFilesystemOverlayZipPaths = {"/glibc.zip", "/wine64.zip"};
    launch.writableRootPath = "/prefixes/x64";
    launch.executablePath = "C:\\Program Files\\Vendor\\program.exe";
    launch.workingDirectory = "/home/username/.wine64/drive_c/Program Files/Vendor";
    launch.dxmtModuleDirectory = "/mnt/drive_d/.boxedvn-x64-diagnostics";
    launch.runThroughWine = true;
    launch.useFEX64 = true;
    launch.useDXMT = true;

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    const auto working = std::find(actual.begin(), actual.end(), "-w");
    CHECK(working != actual.end());
    if (working != actual.end() && working + 1 != actual.end()) {
        CHECK_EQ(*(working + 1),
                 std::string("/home/username/.wine64/drive_c/Program Files/Vendor"));
    }
    const auto modules = std::find(actual.begin(), actual.end(), "-x64modules");
    CHECK(modules != actual.end());
    if (modules != actual.end() && modules + 1 != actual.end()) {
        CHECK_EQ(*(modules + 1),
                 std::string("/mnt/drive_d/.boxedvn-x64-diagnostics"));
    }
}

BOXEDVN_TEST(a_plain_fex64_launch_projects_no_module_overlay) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.rootFilesystemOverlayZipPaths = {"/glibc.zip", "/wine64.zip"};
    launch.writableRootPath = "/prefixes/x64";
    launch.executablePath = "d:\\game.exe";
    launch.workingDirectory = "/mnt/drive_d";
    launch.runThroughWine = true;
    launch.useFEX64 = true;

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    CHECK(std::find(actual.begin(), actual.end(), "-x64modules") == actual.end());
}

BOXEDVN_TEST(a_fex64_launch_mounts_the_visible_drive_c_over_the_64bit_prefix) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.rootFilesystemOverlayZipPaths = {"/glibc.zip", "/wine64.zip"};
    launch.writableRootPath = "/prefixes/x64";
    launch.winePrefixDriveCHostPath = "/documents/Containers/vn/Drive C (64-bit)";
    launch.executablePath = "explorer";
    launch.runThroughWine = true;
    launch.useFEX64 = true;
    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    const auto option = std::find(actual.begin(), actual.end(), "-mount");
    CHECK(option != actual.end());
    if (option != actual.end() && option + 2 < actual.end()) {
        CHECK_EQ(*(option + 1),
                 std::string("/documents/Containers/vn/Drive C (64-bit)"));
        CHECK_EQ(*(option + 2), std::string("/home/username/.wine64/drive_c"));
    }
}

BOXEDVN_TEST(a_32bit_launch_mounts_the_visible_drive_c_over_the_default_prefix) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.writableRootPath = "/prefix";
    launch.winePrefixDriveCHostPath = "/documents/Containers/vn/Drive C";
    launch.executablePath = "explorer";
    launch.runThroughWine = true;
    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    const auto option = std::find(actual.begin(), actual.end(), "-mount");
    CHECK(option != actual.end());
    if (option != actual.end() && option + 2 < actual.end()) {
        CHECK_EQ(*(option + 2), std::string("/home/username/.wine/drive_c"));
    }
}

BOXEDVN_TEST(no_drive_c_directory_means_no_prefix_mount) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.writableRootPath = "/prefix";
    launch.executablePath = "explorer";
    launch.runThroughWine = true;
    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    CHECK(std::find(actual.begin(), actual.end(), "-mount") == actual.end());
}

BOXEDVN_TEST(a_32bit_launch_does_not_get_the_x64_library_path) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.writableRootPath = "/prefix";
    launch.executablePath = "d:\\game.exe";
    launch.runThroughWine = true;

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    // The IA-32 guest keeps its own int 0x9b libX11 from the rootfs; the
    // x86-64 shim directory is never named for it.
    CHECK(std::find(actual.begin(), actual.end(),
                    "LD_LIBRARY_PATH=/usr/lib/boxedwine64-x11") == actual.end());
}

BOXEDVN_TEST(fex64_launch_keeps_one_default_when_only_the_other_is_given) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.rootFilesystemOverlayZipPaths = {"/glibc.zip", "/wine64.zip"};
    launch.writableRootPath = "/prefixes/x64";
    launch.executablePath = "d:\\game.exe";
    launch.runThroughWine = true;
    launch.useFEX64 = true;
    launch.environment = {"WINEPREFIX=/home/username/.wine-experiment"};

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    CHECK(std::count(actual.begin(), actual.end(),
                     "WINEPREFIX=/home/username/.wine-experiment") == 1);
    CHECK(std::find(actual.begin(), actual.end(),
                    "WINEPREFIX=/home/username/.wine64") == actual.end());
    // WINEARCH was not supplied, so the 64-bit default still applies.
    CHECK(std::count(actual.begin(), actual.end(), "WINEARCH=win64") == 1);
}

BOXEDVN_TEST(ia32_launch_is_left_on_the_default_prefix) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.writableRootPath = "/prefixes/default";
    launch.gameDirectoryHostPath = "/games/example";
    launch.executablePath = "d:\\game.exe";
    launch.runThroughWine = true;

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    // A 32-bit launch must not be moved to a 64-bit prefix, and must not be
    // told it is win64.
    CHECK(std::find(actual.begin(), actual.end(),
                    "WINEPREFIX=/home/username/.wine64") == actual.end());
    CHECK(std::find(actual.begin(), actual.end(), "WINEARCH=win64") ==
          actual.end());
    for (const std::string& entry : actual) {
        CHECK(entry.rfind("WINEPREFIX=", 0) != 0);
        CHECK(entry.rfind("WINEARCH=", 0) != 0);
        CHECK(entry.rfind("WINEDLLPATH=", 0) != 0);
    }
    // And it still enters Wine through the 32-bit loader.
    CHECK(std::find(actual.begin(), actual.end(), "/bin/wine") != actual.end());
}
