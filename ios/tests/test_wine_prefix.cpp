/* BoxedVN Wine-prefix policy tests. GPLv2; see license.txt. */

#include "boxedvn/wine_prefix.h"
#include "boxedvn_test.h"

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

extern "C" {
#include "zip.h"
}

namespace fs = std::filesystem;
using namespace boxedvn;

namespace {

bool addZipEntry(zipFile zip, const char* name, const std::string& contents) {
    zip_fileinfo info;
    std::memset(&info, 0, sizeof(info));
    if (zipOpenNewFileInZip64(zip, name, &info, nullptr, 0, nullptr, 0,
                              nullptr, Z_DEFLATED, Z_DEFAULT_COMPRESSION,
                              0) != ZIP_OK) {
        return false;
    }
    const bool wrote =
        zipWriteInFileInZip(zip, contents.data(),
                            static_cast<unsigned>(contents.size())) == ZIP_OK;
    return zipCloseFileInZip(zip) == ZIP_OK && wrote;
}

std::string readTestFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

std::size_t countSubstring(const std::string& value,
                           const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t position = 0;
         (position = value.find(needle, position)) != std::string::npos;
         position += needle.size()) {
        ++count;
    }
    return count;
}

}  // namespace

BOXEDVN_TEST(wine_registry_updates_an_existing_value) {
    std::string registry =
        "WINE REGISTRY Version 2\n\n"
        "[Software\\\\Wine\\\\Direct3D] 123\n"
        "#time=abc\n"
        "\"renderer\"=\"gl\"\n\n"
        "[Software\\\\Wine\\\\Other] 124\n";

    CHECK_EQ(setWineRegistryValue(registry, "Software\\Wine\\Direct3D",
                                  "renderer", "\"gdi\""), true);
    CHECK_CONTAINS(registry, "\"renderer\"=\"gdi\"");
    CHECK_EQ(registry.find("\"gl\""), std::string::npos);
    CHECK_EQ(setWineRegistryValue(registry, "Software\\Wine\\Direct3D",
                                  "renderer", "\"gdi\""), false);
}

BOXEDVN_TEST(wine_registry_inserts_a_value_before_the_next_section) {
    std::string registry =
        "WINE REGISTRY Version 2\n\n"
        "[System\\\\ControlSet001\\\\Services\\\\winebth] 123\n"
        "\"Start\"=dword:00000003\n\n"
        "[System\\\\ControlSet001\\\\Services\\\\winebus] 124\n";

    CHECK_EQ(setWineRegistryValue(
                 registry, "System\\ControlSet001\\Services\\winebth",
                 "Start", "dword:00000004"), true);
    CHECK_CONTAINS(registry, "\"Start\"=dword:00000004");
    CHECK_EQ(registry.find("dword:00000003"), std::string::npos);
}

BOXEDVN_TEST(wine_registry_creates_a_missing_section) {
    std::string registry = "WINE REGISTRY Version 2\n";
    CHECK_EQ(setWineRegistryValue(registry, "Software\\Wine\\Direct3D",
                                  "renderer", "\"gdi\""), true);
    CHECK_CONTAINS(registry, "[Software\\\\Wine\\\\Direct3D]");
    CHECK_CONTAINS(registry, "\"renderer\"=\"gdi\"");
}

BOXEDVN_TEST(wine_prefix_is_extracted_and_patched_end_to_end) {
    const fs::path temporary = fs::temp_directory_path() /
        ("boxedvn-prefix-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(temporary, ec);
    fs::create_directories(temporary, ec);
    const fs::path archive = temporary / "rootfs.zip";
    const fs::path prefix = temporary / "prefix";

    zipFile zip = zipOpen64(archive.string().c_str(), APPEND_STATUS_CREATE);
    CHECK(zip != nullptr);
    if (zip != nullptr) {
        CHECK(addZipEntry(
            zip, "home/username/.wine/user.reg",
            "WINE REGISTRY Version 2\n\n[Software\\\\Wine] 1\n"));
        CHECK(addZipEntry(
            zip, "home/username/.wine/system.reg",
            "WINE REGISTRY Version 2\n\n"
            "[System\\\\ControlSet001\\\\Services\\\\winebth] 2\n"
            "\"Start\"=dword:00000003\n\n"
            "[System\\\\ControlSet001\\\\Services\\\\winebus] 3\n"
            "\"Start\"=dword:00000004\n\n"
            "[System\\\\ControlSet001\\\\Services\\\\NDIS] 31\n"
            "\"Start\"=dword:00000003\n\n"
            "[System\\\\ControlSet001\\\\Enum\\\\ROOT\\\\WINE\\\\WINEBTH] 4\n"
            "\"Service\"=\"winebth\"\n\n"
            "[System\\\\ControlSet001\\\\Enum\\\\ROOT\\\\WINE\\\\WINEBUS] 5\n"
            "\"Service\"=\"\"\n"));
        zipClose(zip, nullptr);
    }

    const WinePrefixPreparationResult prepared =
        prepareWinePrefix(archive.string(), prefix.string(),
                          WineRenderer::GDI);
    CHECK_EQ(prepared.ok, true);
    CHECK_EQ(prepared.changed, true);
    CHECK_CONTAINS(readTestFile(prefix / "home/username/.wine/user.reg"),
                   "\"renderer\"=\"gdi\"");
    const std::string systemRegistry =
        readTestFile(prefix / "home/username/.wine/system.reg");
    CHECK_EQ(std::count(systemRegistry.begin(), systemRegistry.end(), '\0'), 0);
    CHECK_EQ(systemRegistry.find("\"Service\"=\"winebth\""),
             std::string::npos);
    CHECK_EQ(std::count(systemRegistry.begin(), systemRegistry.end(), '\n') > 0,
             true);
    CHECK_EQ(countSubstring(systemRegistry,
                            "\"Start\"=dword:00000004"), 2U);
    CHECK_EQ(countSubstring(systemRegistry,
                            "\"Start\"=dword:00000003"), 1U);
    CHECK_EQ(countSubstring(systemRegistry, "\"Service\"=\"\""), 1U);
    CHECK_EQ(countSubstring(systemRegistry,
                            "\"Service\"=\"winebus\""), 1U);
    CHECK_CONTAINS(systemRegistry,
                   "[System\\\\ControlSet001\\\\Services\\\\NDIS]");

    const WinePrefixPreparationResult repeated =
        prepareWinePrefix(archive.string(), prefix.string(),
                          WineRenderer::GDI);
    CHECK_EQ(repeated.ok, true);
    CHECK_EQ(repeated.changed, false);

    fs::remove_all(temporary, ec);
}

BOXEDVN_TEST(wine_prefix_repairs_gdi_to_vulkan_for_accelerated_games) {
    const fs::path temporary = fs::temp_directory_path() /
        ("boxedvn-prefix-vulkan-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(temporary, ec);
    fs::create_directories(temporary / "prefix/home/username/.wine", ec);
    const fs::path archive = temporary / "rootfs.zip";
    const fs::path prefix = temporary / "prefix";

    zipFile zip = zipOpen64(archive.string().c_str(), APPEND_STATUS_CREATE);
    CHECK(zip != nullptr);
    if (zip != nullptr) {
        CHECK(addZipEntry(zip, "home/username/.wine/user.reg",
                          "WINE REGISTRY Version 2\n"));
        CHECK(addZipEntry(zip, "home/username/.wine/system.reg",
                          "WINE REGISTRY Version 2\n"));
        zipClose(zip, nullptr);
    }

    std::ofstream(prefix / "home/username/.wine/user.reg")
        << "WINE REGISTRY Version 2\n\n"
           "[Software\\\\Wine\\\\Direct3D] 1\n"
           "\"renderer\"=\"gdi\"\n";
    std::ofstream(prefix / "home/username/.wine/system.reg")
        << "WINE REGISTRY Version 2\n";

    const WinePrefixPreparationResult prepared = prepareWinePrefix(
        archive.string(), prefix.string(), WineRenderer::Vulkan);
    CHECK_EQ(prepared.ok, true);
    CHECK_EQ(prepared.changed, true);
    const std::string userRegistry =
        readTestFile(prefix / "home/username/.wine/user.reg");
    CHECK_CONTAINS(userRegistry, "\"renderer\"=\"vulkan\"");
    CHECK_EQ(userRegistry.find("\"renderer\"=\"gdi\""), std::string::npos);
    // MoltenVK can never expose geometry shaders, so wined3d can never honour
    // shader model 4. Claim feature level 9_3 honestly instead.
    CHECK_CONTAINS(userRegistry, "\"MaxShaderModelVS\"=dword:00000003");
    CHECK_CONTAINS(userRegistry, "\"MaxShaderModelPS\"=dword:00000003");
    CHECK_CONTAINS(userRegistry, "\"MaxShaderModelGS\"=dword:00000000");

    fs::remove_all(temporary, ec);
}

BOXEDVN_TEST(bundled_dxvk_overwrites_a_same_size_stale_module) {
    // The exact regression from build 47: a rebuilt DXVK module kept the same
    // byte size, a size-based freshness check skipped the copy, and the device
    // kept running the previous build.
    const fs::path temporary = fs::temp_directory_path() /
        ("boxedvn-dxvk-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(temporary, ec);
    const fs::path source = temporary / "bundle";
    const fs::path root = temporary / "root";
    const fs::path destination =
        root / "home/username/.wine/drive_c/dxvk";
    fs::create_directories(source, ec);
    fs::create_directories(destination, ec);

    std::ofstream(source / "d3d11.dll", std::ios::binary) << "NEWNEWNEW";
    std::ofstream(destination / "d3d11.dll", std::ios::binary) << "OLDOLDOLD";
    // Non-DLL files are ignored.
    std::ofstream(source / "README.txt") << "notes";

    bool changed = false;
    std::string error;
    CHECK(installBundledDxvk(source.string(), root.string(), changed, error));
    CHECK(changed);
    CHECK_EQ(readTestFile(destination / "d3d11.dll"), std::string("NEWNEWNEW"));
    CHECK(!fs::exists(destination / "README.txt", ec));

    fs::remove_all(temporary, ec);
}

BOXEDVN_TEST(bundled_dxvk_tolerates_a_build_without_the_modules) {
    const fs::path temporary = fs::temp_directory_path() /
        ("boxedvn-dxvk-absent-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(temporary, ec);
    bool changed = true;
    std::string error;
    // A build that ships no DXVK is not an error; the runtime reports it.
    CHECK(installBundledDxvk((temporary / "missing").string(),
                             (temporary / "root").string(), changed, error));
    CHECK(!changed);
    CHECK(error.empty());
}

BOXEDVN_TEST(wine_prefix_leaves_shader_model_alone_without_vulkan) {
    // The cap exists only because of MoltenVK's missing geometry shader
    // stage. A software prefix has no reason to carry it.
    const fs::path temporary = fs::temp_directory_path() /
        ("boxedvn-prefix-gdi-sm-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(temporary, ec);
    fs::create_directories(temporary / "prefix/home/username/.wine", ec);
    const fs::path archive = temporary / "rootfs.zip";
    const fs::path prefix = temporary / "prefix";

    zipFile zip = zipOpen64(archive.string().c_str(), APPEND_STATUS_CREATE);
    CHECK(zip != nullptr);
    if (zip != nullptr) {
        CHECK(addZipEntry(zip, "home/username/.wine/user.reg",
                          "WINE REGISTRY Version 2\n"));
        CHECK(addZipEntry(zip, "home/username/.wine/system.reg",
                          "WINE REGISTRY Version 2\n"));
        zipClose(zip, nullptr);
    }
    std::ofstream(prefix / "home/username/.wine/user.reg")
        << "WINE REGISTRY Version 2\n\n"
           "[Software\\\\Wine\\\\Direct3D] 1\n"
           "\"renderer\"=\"vulkan\"\n";
    std::ofstream(prefix / "home/username/.wine/system.reg")
        << "WINE REGISTRY Version 2\n";

    const WinePrefixPreparationResult prepared = prepareWinePrefix(
        archive.string(), prefix.string(), WineRenderer::GDI);
    CHECK_EQ(prepared.ok, true);
    const std::string userRegistry =
        readTestFile(prefix / "home/username/.wine/user.reg");
    CHECK_EQ(userRegistry.find("MaxShaderModelVS"), std::string::npos);

    fs::remove_all(temporary, ec);
}

BOXEDVN_TEST(wine_prefix_suppresses_the_missing_runtime_download_prompts) {
    // The root filesystem ships neither Wine Mono nor Wine Gecko, so Wine
    // offers to download them the first time an application touches mscoree
    // or mshtml. That offer is a dead end on a phone and it recurs for every
    // imported game, because every game gets its own prefix.
    const fs::path temporary = fs::temp_directory_path() /
        ("boxedvn-prefix-runtimes-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(temporary, ec);
    fs::create_directories(temporary, ec);
    const fs::path archive = temporary / "rootfs.zip";
    const fs::path prefix = temporary / "prefix";

    zipFile zip = zipOpen64(archive.string().c_str(), APPEND_STATUS_CREATE);
    CHECK(zip != nullptr);
    if (zip != nullptr) {
        CHECK(addZipEntry(zip, "home/username/.wine/user.reg",
                          "WINE REGISTRY Version 2\n"));
        CHECK(addZipEntry(zip, "home/username/.wine/system.reg",
                          "WINE REGISTRY Version 2\n"));
        zipClose(zip, nullptr);
    }

    // Applied for every prefix, including one that makes no renderer choice.
    const WinePrefixPreparationResult prepared = prepareWinePrefix(
        archive.string(), prefix.string(), WineRenderer::Default);
    CHECK_EQ(prepared.ok, true);
    CHECK_EQ(prepared.changed, true);

    const std::string userRegistry =
        readTestFile(prefix / "home/username/.wine/user.reg");
    CHECK_CONTAINS(userRegistry, "[Software\\\\Wine\\\\DllOverrides]");
    CHECK_CONTAINS(userRegistry, "\"mscoree\"=\"\"");
    CHECK_CONTAINS(userRegistry, "\"mshtml\"=\"\"");
    // A renderer was not requested, so nothing else in the user registry may
    // have been touched.
    CHECK_EQ(userRegistry.find("\"renderer\""), std::string::npos);

    // Re-preparing an already-patched prefix must be a no-op, or every launch
    // rewrites the registry Wine is about to read.
    const WinePrefixPreparationResult repeated = prepareWinePrefix(
        archive.string(), prefix.string(), WineRenderer::Default);
    CHECK_EQ(repeated.ok, true);
    CHECK_EQ(repeated.changed, false);

    fs::remove_all(temporary, ec);
}

BOXEDVN_TEST(guest_fonts_are_installed_where_wine_scans_for_them) {
    const fs::path temporary = fs::temp_directory_path() /
        ("boxedvn-fonts-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(temporary, ec);
    const fs::path source = temporary / "Fonts";
    const fs::path root = temporary / "prefix";
    fs::create_directories(source, ec);

    std::ofstream(source / "NotoSansJP-Regular.otf") << "otf";
    std::ofstream(source / "meiryo.ttc") << "ttc";
    std::ofstream(source / "arial.TTF") << "ttf";
    // Not a font. A user's Fonts folder collects whatever the Files app put
    // there, and a stray file must not be copied or counted.
    std::ofstream(source / "readme.txt") << "notes";

    const GuestFontInstallResult result =
        installGuestFonts(source.string(), root.string());
    CHECK(result.ok);
    CHECK(result.error.empty());
    CHECK_EQ(result.available, static_cast<std::size_t>(3));
    CHECK_EQ(result.installed, static_cast<std::size_t>(3));

    // Wine's font backend scans drive_c/windows/Fonts, so anywhere else is
    // the same as not installing them at all.
    const fs::path destination =
        root / "home/username/.wine/drive_c/windows/Fonts";
    CHECK(fs::exists(destination / "NotoSansJP-Regular.otf", ec));
    CHECK(fs::exists(destination / "meiryo.ttc", ec));
    CHECK(fs::exists(destination / "arial.TTF", ec));
    CHECK(!fs::exists(destination / "readme.txt", ec));

    fs::remove_all(temporary, ec);
}

BOXEDVN_TEST(guest_fonts_already_present_are_not_recopied) {
    const fs::path temporary = fs::temp_directory_path() /
        ("boxedvn-fonts-repeat-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(temporary, ec);
    const fs::path source = temporary / "Fonts";
    const fs::path root = temporary / "prefix";
    fs::create_directories(source, ec);
    std::ofstream(source / "cjk.ttf") << "font";

    CHECK_EQ(installGuestFonts(source.string(), root.string()).installed,
             static_cast<std::size_t>(1));

    // These are the user's files and BoxedVN has no newer version of them, so
    // a second launch must be free rather than recopying a large CJK face.
    const GuestFontInstallResult again =
        installGuestFonts(source.string(), root.string());
    CHECK(again.ok);
    CHECK_EQ(again.available, static_cast<std::size_t>(1));
    CHECK_EQ(again.installed, static_cast<std::size_t>(0));

    fs::remove_all(temporary, ec);
}

BOXEDVN_TEST(guest_fonts_tolerate_a_user_who_supplied_none) {
    const fs::path temporary = fs::temp_directory_path() /
        ("boxedvn-fonts-none-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(temporary, ec);

    // No Fonts directory at all: the ordinary case, and not an error.
    const GuestFontInstallResult absent =
        installGuestFonts((temporary / "Fonts").string(),
                          (temporary / "prefix").string());
    CHECK(absent.ok);
    CHECK_EQ(absent.available, static_cast<std::size_t>(0));
    CHECK(absent.error.empty());

    // An empty one must not write a stray directory into the prefix.
    fs::create_directories(temporary / "Fonts", ec);
    const GuestFontInstallResult empty =
        installGuestFonts((temporary / "Fonts").string(),
                          (temporary / "prefix").string());
    CHECK(empty.ok);
    CHECK_EQ(empty.installed, static_cast<std::size_t>(0));
    CHECK(!fs::exists(temporary / "prefix/home/username/.wine/drive_c/windows/Fonts", ec));

    CHECK(installGuestFonts("", (temporary / "prefix").string()).ok);

    fs::remove_all(temporary, ec);
}
