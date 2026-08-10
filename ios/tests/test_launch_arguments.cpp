/*
 * BoxedVN - launch command regression tests.
 * GPLv2; see license.txt.
 */

#include "boxedvn_test.h"

#include "BVNLaunchArguments.h"

#include <algorithm>

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

BOXEDVN_TEST(game_command_preserves_mount_environment_and_arguments) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.writableRootPath = "/prefix";
    launch.gameDirectoryHostPath = "/games/example";
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
        "-mount_drive", "/games/example", "d", "-nosound",
        "-dxvk", "1", "-w",
        "/home/username/.wine/dosdevices/d:/content",
        "-env", "WINEDEBUG=warn+d3d_shader,-d3d",
        "-env", "LANG=ja_JP.UTF-8", "/bin/wine", "d:\\game.exe",
        "--language", "ja",
    };

    CHECK(actual == expected);
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
        launch.enableWineD3DVulkan = true;

        CHECK(BVNApplyKnownCompatibilityProfile(launch));
        CHECK(!launch.enableWineD3DVulkan);
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
}

BOXEDVN_TEST(command_does_not_let_boxedwine_truncate_frontend_log) {
    BVNLaunchConfiguration launch;
    launch.rootFilesystemZipPath = "/rootfs.zip";
    launch.writableRootPath = "/prefix";
    launch.executablePath = "notepad";

    const std::vector<std::string> actual = BVNBuildLaunchArguments(launch);
    CHECK(std::find(actual.begin(), actual.end(), "-log") == actual.end());
}
