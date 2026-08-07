/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  GPLv2; see license.txt.
 */

#include "boxedvn/path_safety.h"
#include "boxedvn_test.h"

using namespace boxedvn;

BOXEDVN_TEST(plain_relative_path_is_accepted) {
    const SanitisedPath path = sanitiseArchiveEntryName("Game/data/script.ks");
    CHECK_EQ(path.accepted, true);
    CHECK_EQ(path.normalised, std::string("Game/data/script.ks"));
    CHECK_EQ(path.isDirectory, false);
}

BOXEDVN_TEST(backslash_separators_are_normalised) {
    const SanitisedPath path = sanitiseArchiveEntryName("Game\\data\\script.ks");
    CHECK_EQ(path.accepted, true);
    CHECK_EQ(path.normalised, std::string("Game/data/script.ks"));
}

BOXEDVN_TEST(trailing_separator_marks_a_directory) {
    const SanitisedPath path = sanitiseArchiveEntryName("Game/data/");
    CHECK_EQ(path.accepted, true);
    CHECK_EQ(path.isDirectory, true);
    CHECK_EQ(path.normalised, std::string("Game/data"));
}

BOXEDVN_TEST(dot_components_are_removed) {
    const SanitisedPath path = sanitiseArchiveEntryName("./Game/./data/x.dat");
    CHECK_EQ(path.accepted, true);
    CHECK_EQ(path.normalised, std::string("Game/data/x.dat"));
}

BOXEDVN_TEST(interior_dotdot_is_resolved_when_it_stays_inside) {
    const SanitisedPath path = sanitiseArchiveEntryName("Game/data/../music/a.ogg");
    CHECK_EQ(path.accepted, true);
    CHECK_EQ(path.normalised, std::string("Game/music/a.ogg"));
}

BOXEDVN_TEST(leading_dotdot_traversal_is_rejected) {
    const SanitisedPath path = sanitiseArchiveEntryName("../../../etc/passwd");
    CHECK_EQ(path.accepted, false);
    CHECK_EQ(std::string(toString(path.reason)), std::string("traversal"));
    CHECK_CONTAINS(path.diagnostic, "escape the destination");
}

BOXEDVN_TEST(dotdot_that_escapes_after_descending_is_rejected) {
    const SanitisedPath path = sanitiseArchiveEntryName("Game/../../evil.sh");
    CHECK_EQ(path.accepted, false);
    CHECK_EQ(std::string(toString(path.reason)), std::string("traversal"));
}

BOXEDVN_TEST(absolute_posix_path_is_rejected) {
    const SanitisedPath path = sanitiseArchiveEntryName("/etc/passwd");
    CHECK_EQ(path.accepted, false);
    CHECK_EQ(std::string(toString(path.reason)), std::string("absolute"));
}

BOXEDVN_TEST(absolute_windows_path_is_rejected) {
    const SanitisedPath path = sanitiseArchiveEntryName("C:\\Windows\\system32\\a");
    CHECK_EQ(path.accepted, false);
    CHECK_EQ(std::string(toString(path.reason)), std::string("absolute"));
}

BOXEDVN_TEST(unc_path_is_rejected) {
    const SanitisedPath path = sanitiseArchiveEntryName("\\\\server\\share\\a");
    CHECK_EQ(path.accepted, false);
    CHECK_EQ(std::string(toString(path.reason)), std::string("absolute"));
}

BOXEDVN_TEST(embedded_nul_is_rejected) {
    const std::string name = std::string("safe.txt\0/../../evil", 20);
    const SanitisedPath path = sanitiseArchiveEntryName(name);
    CHECK_EQ(path.accepted, false);
    CHECK_EQ(std::string(toString(path.reason)), std::string("null-byte"));
}

BOXEDVN_TEST(control_characters_are_rejected) {
    const SanitisedPath path = sanitiseArchiveEntryName("Game/da\ta.txt");
    CHECK_EQ(path.accepted, false);
    CHECK_EQ(std::string(toString(path.reason)), std::string("control-character"));
}

BOXEDVN_TEST(overlong_component_is_rejected) {
    const std::string name = "Game/" + std::string(300, 'a') + "/x";
    const SanitisedPath path = sanitiseArchiveEntryName(name);
    CHECK_EQ(path.accepted, false);
    CHECK_EQ(std::string(toString(path.reason)), std::string("too-long"));
}

BOXEDVN_TEST(path_that_normalises_to_nothing_is_rejected) {
    const SanitisedPath path = sanitiseArchiveEntryName("./././");
    CHECK_EQ(path.accepted, false);
    CHECK_EQ(std::string(toString(path.reason)), std::string("empty"));
}

BOXEDVN_TEST(redundant_top_level_directory_is_detected) {
    const std::vector<std::string> entries = {
        "Game/data/a.dat",
        "Game/data/b.dat",
        "Game/game.exe",
    };
    CHECK_EQ(redundantTopLevelDirectory(entries), std::string("Game"));
}

BOXEDVN_TEST(mixed_top_level_entries_have_no_redundant_directory) {
    const std::vector<std::string> entries = {
        "Game/data/a.dat",
        "readme.txt",
    };
    CHECK_EQ(redundantTopLevelDirectory(entries), std::string());
}

BOXEDVN_TEST(single_top_level_file_is_not_a_redundant_directory) {
    const std::vector<std::string> entries = {"game.exe"};
    CHECK_EQ(redundantTopLevelDirectory(entries), std::string());
}

BOXEDVN_TEST(stripping_the_top_level_directory) {
    CHECK_EQ(stripTopLevelDirectory("Game/data/a.dat", "Game"),
             std::string("data/a.dat"));
    CHECK_EQ(stripTopLevelDirectory("Game", "Game"), std::string());
    // A different directory whose name merely starts with the prefix must be
    // left alone.
    CHECK_EQ(stripTopLevelDirectory("GameExtra/a.dat", "Game"),
             std::string("GameExtra/a.dat"));
}
