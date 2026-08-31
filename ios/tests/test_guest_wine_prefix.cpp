#include "boxedvn_test.h"
#include "guest_wine_prefix.h"

#include <string>

using namespace boxedvn;

// The root filesystem ships one prefix and it is a 32-bit Wine installation.
// Wine64 refuses it outright, so a 64-bit launch is given .wine64 and
// everything in the emulator that used to hardcode the one fixed prefix
// resolves it from the guest environment instead. Getting that resolution
// wrong is not cosmetic: the dosdevices drive links go with it, so a
// mis-resolved prefix means D: and E: do not exist for the session.

BOXEDVN_TEST(guest_wine_prefix_defaults_to_the_32_bit_installation) {
    // No WINEPREFIX at all is the ordinary IA-32 case and must not change.
    CHECK(resolveGuestWinePrefix(nullptr) == "/home/username/.wine");
    CHECK(std::string(K_DEFAULT_GUEST_WINE_PREFIX) == "/home/username/.wine");
}

BOXEDVN_TEST(guest_wine_prefix_accepts_an_absolute_override) {
    CHECK(resolveGuestWinePrefix("/home/username/.wine64") ==
          "/home/username/.wine64");
    CHECK(resolveGuestWinePrefix("/opt/prefixes/x64") == "/opt/prefixes/x64");
    // A trailing slash is cosmetic; callers append "/dosdevices" to this.
    CHECK(resolveGuestWinePrefix("/home/username/.wine64/") ==
          "/home/username/.wine64");
    CHECK(resolveGuestWinePrefix("/home/username/.wine64///") ==
          "/home/username/.wine64");
}

BOXEDVN_TEST(guest_wine_prefix_falls_back_for_unusable_values) {
    // Empty or relative: everything downstream concatenates onto this, so a
    // relative value would silently write into the current directory.
    CHECK(resolveGuestWinePrefix("") == "/home/username/.wine");
    CHECK(resolveGuestWinePrefix(".wine64") == "/home/username/.wine");
    CHECK(resolveGuestWinePrefix("home/username/.wine64") ==
          "/home/username/.wine");
    // A lone root would put dosdevices at the filesystem root.
    CHECK(resolveGuestWinePrefix("/") == "/home/username/.wine");
    CHECK(resolveGuestWinePrefix("///") == "/home/username/.wine");
    // A ".." component makes the result depend on symlink resolution rather
    // than on the text. A prefix that moves is worse than one that is refused.
    CHECK(resolveGuestWinePrefix("/home/username/../root/.wine64") ==
          "/home/username/.wine");
    CHECK(resolveGuestWinePrefix("/..") == "/home/username/.wine");
    CHECK(resolveGuestWinePrefix("/home/username/.wine64/..") ==
          "/home/username/.wine");
}

BOXEDVN_TEST(guest_wine_prefix_usability_matches_resolution) {
    CHECK(isUsableGuestWinePrefix("/home/username/.wine64"));
    CHECK(isUsableGuestWinePrefix("/a/b"));
    CHECK(isUsableGuestWinePrefix("/ab"));
    CHECK(!isUsableGuestWinePrefix(nullptr));
    CHECK(!isUsableGuestWinePrefix(""));
    CHECK(!isUsableGuestWinePrefix("/"));
    CHECK(!isUsableGuestWinePrefix("relative"));
    // "..wine" is a directory name, not a parent reference.
    CHECK(isUsableGuestWinePrefix("/home/..wine"));
}

BOXEDVN_TEST(guest_wine_prefix_assignment_reads_only_wineprefix_entries) {
    const char* value =
        guestWinePrefixAssignment("WINEPREFIX=/home/username/.wine64");
    CHECK(value != nullptr);
    CHECK(std::string(value) == "/home/username/.wine64");
    // An empty assignment is still an assignment; rejecting it is the
    // resolver's job, not the parser's.
    const char* empty = guestWinePrefixAssignment("WINEPREFIX=");
    CHECK(empty != nullptr);
    CHECK(std::string(empty).empty());

    CHECK(guestWinePrefixAssignment(nullptr) == nullptr);
    CHECK(guestWinePrefixAssignment("WINEARCH=win64") == nullptr);
    // A longer name that merely starts with the same letters is a
    // different variable and must not be read as this one.
    CHECK(guestWinePrefixAssignment("WINEPREFIXES=/x") == nullptr);
    CHECK(guestWinePrefixAssignment("WINEPREFI") == nullptr);
    CHECK(guestWinePrefixAssignment("HOME=/home/username") == nullptr);
    CHECK(guestWinePrefixAssignment("MYWINEPREFIX=/x") == nullptr);
}

BOXEDVN_TEST(guest_wine_prefix_constants_keep_the_two_prefixes_apart) {
    // Wine records the architecture in the prefix, so the 32-bit and 64-bit
    // prefixes cannot be the same directory.
    CHECK(std::string(K_X64_GUEST_WINE_PREFIX) == "/home/username/.wine64");
    CHECK(std::string(K_X64_GUEST_WINE_ARCH) == "win64");
    CHECK(std::string(K_X64_GUEST_WINE_PREFIX) !=
          std::string(K_DEFAULT_GUEST_WINE_PREFIX));
    // The 64-bit prefix must not sit inside the 32-bit one, or Wine64 would
    // be initialising a directory the 32-bit installation owns.
    CHECK(std::string(K_X64_GUEST_WINE_PREFIX)
              .rfind(std::string(K_DEFAULT_GUEST_WINE_PREFIX) + "/", 0) !=
          0);
}

// wineboot --init exits 0 without creating the C: drive link in a prefix it
// did not initialise itself, and the guest then reopens the missing path
// forever: one device run logged 468,768 failed opens of
// /home/username/.wine64/dosdevices/c:.

BOXEDVN_TEST(guest_wine_prefix_completes_a_fresh_x64_prefix) {
    // Nothing exists yet: Wine64 has been given a prefix directory and has not
    // populated it.
    const GuestWinePrefixSetup setup = planGuestWinePrefixSetup(false, false,
                                                                false);
    CHECK(setup.createDriveC);
    CHECK(setup.createDosDevices);
    CHECK(setup.createDriveCLink);
    CHECK(setup.anyWorkToDo());
}

BOXEDVN_TEST(guest_wine_prefix_leaves_a_complete_prefix_alone) {
    // The bundled 32-bit prefix, and any prefix Wine has already initialised.
    const GuestWinePrefixSetup setup = planGuestWinePrefixSetup(true, true,
                                                                true);
    CHECK(!setup.createDriveC);
    CHECK(!setup.createDosDevices);
    CHECK(!setup.createDriveCLink);
    CHECK(!setup.anyWorkToDo());
}

BOXEDVN_TEST(guest_wine_prefix_never_replaces_an_existing_c_link) {
    // An existing c: is left exactly where it points, even when the rest of
    // the prefix is missing. Replacing it would silently move the guest's C:
    // drive out from under whatever created it.
    const GuestWinePrefixSetup setup = planGuestWinePrefixSetup(false, true,
                                                                true);
    CHECK(setup.createDriveC);
    CHECK(!setup.createDosDevices);
    CHECK(!setup.createDriveCLink);
}

BOXEDVN_TEST(guest_wine_prefix_adds_only_the_missing_link) {
    // The case the device hit: wineboot made the directories and exited 0
    // without the link.
    const GuestWinePrefixSetup setup = planGuestWinePrefixSetup(true, true,
                                                                false);
    CHECK(!setup.createDriveC);
    CHECK(!setup.createDosDevices);
    CHECK(setup.createDriveCLink);
    CHECK(setup.anyWorkToDo());
}

BOXEDVN_TEST(guest_wine_prefix_paths_follow_the_resolved_prefix) {
    // The paths come from the resolved WINEPREFIX, never from a hardcoded
    // .wine64, so a caller-supplied prefix is completed rather than ignored.
    const std::string x64 = resolveGuestWinePrefix(K_X64_GUEST_WINE_PREFIX);
    CHECK(guestWineDriveCPath(x64) == "/home/username/.wine64/drive_c");
    CHECK(guestWineDosDevicesPath(x64) == "/home/username/.wine64/dosdevices");
    CHECK(guestWineDriveCLinkPath(x64) ==
          "/home/username/.wine64/dosdevices/c:");
    CHECK(guestWineSystem32Path(x64) ==
          "/home/username/.wine64/drive_c/windows/system32");

    const std::string custom = resolveGuestWinePrefix("/opt/prefixes/x64");
    CHECK(guestWineDriveCPath(custom) == "/opt/prefixes/x64/drive_c");
    CHECK(guestWineDriveCLinkPath(custom) ==
          "/opt/prefixes/x64/dosdevices/c:");

    // And the 32-bit default is composed the same way, so existing prefixes
    // are addressed exactly as before.
    const std::string ia32 = resolveGuestWinePrefix(nullptr);
    CHECK(guestWineDriveCLinkPath(ia32) ==
          "/home/username/.wine/dosdevices/c:");
}

BOXEDVN_TEST(guest_wine_system_overlay_projects_only_missing_files) {
    CHECK(shouldProjectGuestWineSystemModule(true, false, false));
    // Existing prefix content, including a user-supplied native override,
    // always keeps precedence over the packaged builtin.
    CHECK(!shouldProjectGuestWineSystemModule(true, false, true));
    CHECK(!shouldProjectGuestWineSystemModule(true, true, false));
    CHECK(!shouldProjectGuestWineSystemModule(false, false, false));
    CHECK(std::string(K_GUEST_WINE_WINDOWS) == "windows");
    CHECK(std::string(K_GUEST_WINE_SYSTEM32) == "system32");
}

BOXEDVN_TEST(guest_wine_prefix_c_link_target_is_relative_to_dosdevices) {
    // Wine writes a relative target so the prefix stays relocatable, and the
    // guest resolves it against the link's own directory: dosdevices/../drive_c
    // is the prefix's drive_c.
    CHECK(std::string(K_GUEST_WINE_C_LINK_TARGET) == "../drive_c");
    CHECK(std::string(K_GUEST_WINE_C_LINK) == "c:");
    CHECK(std::string(K_GUEST_WINE_DRIVE_C) == "drive_c");
    CHECK(std::string(K_GUEST_WINE_DOSDEVICES) == "dosdevices");
    const std::string prefix = "/home/username/.wine64";
    CHECK(guestWineDosDevicesPath(prefix) + "/" + K_GUEST_WINE_C_LINK_TARGET ==
          "/home/username/.wine64/dosdevices/../drive_c");
}
