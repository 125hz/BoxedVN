/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  GPLv2; see license.txt.
 *
 *  These tests assemble real PE32 images with real import directories, so the
 *  reader is exercised through section mapping and RVA translation rather
 *  than only at the string level. A game's Direct3D generation decides which
 *  translation layer can run it at all, and reading it wrong shows up as a
 *  title that launches and then renders nothing.
 */

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "BVNLaunchArguments.h"
#include "boxedvn/direct3d_profile.h"
#include "boxedvn_test.h"

namespace fs = std::filesystem;
using namespace boxedvn;

namespace {

struct TemporaryDirectory {
    fs::path path;

    explicit TemporaryDirectory(const std::string& name) {
        path = fs::temp_directory_path() /
               ("boxedvn-test-" + name + "-" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }

    ~TemporaryDirectory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void putU16(std::vector<std::uint8_t>& image, std::size_t offset,
            std::uint16_t value) {
    image[offset] = static_cast<std::uint8_t>(value & 0xFF);
    image[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

void putU32(std::vector<std::uint8_t>& image, std::size_t offset,
            std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        image[offset + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF);
    }
}

// Builds a minimal but structurally valid PE32 whose import directory names
// `modules`. Layout: DOS stub, PE headers, one section header, then the
// section's raw data holding the descriptor array and the name strings.
//
// `delayed` puts the names in the delay-import directory instead, which uses
// a different descriptor size and a different name field offset - the two
// details most likely to be transposed.
std::vector<std::uint8_t> buildPe(const std::vector<std::string>& modules,
                                  bool delayed = false) {
    constexpr std::size_t kLfanew = 0x80;
    constexpr std::size_t kOptionalSize = 224;  // PE32 with 16 directories
    constexpr std::uint32_t kSectionRva = 0x1000;

    const std::size_t coff = kLfanew + 4;
    const std::size_t optional = coff + 20;
    const std::size_t sectionTable = optional + kOptionalSize;
    const std::size_t sectionRaw = sectionTable + 40;

    const std::size_t descriptorSize = delayed ? 32u : 20u;
    const std::size_t nameField = delayed ? 4u : 12u;

    // Descriptors first, terminated by an all-zero entry, then the names.
    const std::size_t descriptorBytes = (modules.size() + 1) * descriptorSize;
    std::vector<std::size_t> nameOffsets;
    std::size_t cursor = descriptorBytes;
    for (const std::string& module : modules) {
        nameOffsets.push_back(cursor);
        cursor += module.size() + 1;
    }
    const std::size_t sectionBytes = cursor;

    std::vector<std::uint8_t> image(sectionRaw + sectionBytes, 0);

    putU16(image, 0, 0x5A4D);                     // 'MZ'
    putU32(image, 0x3C, static_cast<std::uint32_t>(kLfanew));
    putU32(image, kLfanew, 0x00004550);           // 'PE\0\0'
    putU16(image, coff, 0x014C);                  // machine: i386
    putU16(image, coff + 2, 1);                   // NumberOfSections
    putU16(image, coff + 16,
           static_cast<std::uint16_t>(kOptionalSize));
    putU16(image, optional, 0x010B);              // PE32
    putU32(image, optional + 28, 0x00400000);     // ImageBase
    putU32(image, optional + 92, 16);             // NumberOfRvaAndSizes

    const std::size_t directories = optional + 96;
    const std::size_t index = delayed ? 13u : 1u;
    putU32(image, directories + index * 8, kSectionRva);
    putU32(image, directories + index * 8 + 4,
           static_cast<std::uint32_t>(descriptorBytes));

    std::memcpy(image.data() + sectionTable, ".rdata\0", 7);
    putU32(image, sectionTable + 8,
           static_cast<std::uint32_t>(sectionBytes));   // VirtualSize
    putU32(image, sectionTable + 12, kSectionRva);      // VirtualAddress
    putU32(image, sectionTable + 16,
           static_cast<std::uint32_t>(sectionBytes));   // SizeOfRawData
    putU32(image, sectionTable + 20,
           static_cast<std::uint32_t>(sectionRaw));     // PointerToRawData

    for (std::size_t i = 0; i < modules.size(); ++i) {
        const std::size_t descriptor = sectionRaw + i * descriptorSize;
        putU32(image, descriptor + nameField,
               static_cast<std::uint32_t>(kSectionRva + nameOffsets[i]));
        std::memcpy(image.data() + sectionRaw + nameOffsets[i],
                    modules[i].data(), modules[i].size());
    }
    return image;
}

void writeFile(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

bool contains(const std::vector<std::string>& values, const char* needle) {
    for (const std::string& value : values) {
        if (value == needle) {
            return true;
        }
    }
    return false;
}

}  // namespace

BOXEDVN_TEST(direct3d_module_names_separate_modern_from_legacy) {
    CHECK(isModernDirect3DModule("d3d11.dll"));
    CHECK(isModernDirect3DModule("D3D11.DLL"));
    CHECK(isModernDirect3DModule("d3d10core.dll"));
    CHECK(isModernDirect3DModule("d3d10_1.dll"));
    CHECK(isModernDirect3DModule("d3d12.dll"));
    CHECK(isModernDirect3DModule("dxgi.dll"));

    // WineD3D handles these on Metal. Routing them through DXVK is what
    // stopped a Direct3D 9 title from starting in build 65, so they must not
    // read as modern.
    CHECK(!isModernDirect3DModule("d3d9.dll"));
    CHECK(!isModernDirect3DModule("d3d8.dll"));
    CHECK(!isModernDirect3DModule("ddraw.dll"));
    CHECK(!isModernDirect3DModule("d3dx9_43.dll"));
    CHECK(!isModernDirect3DModule("kernel32.dll"));
}

BOXEDVN_TEST(direct3d_file_names_recognise_a_bundled_angle) {
    // ANGLE loads d3d11.dll through LoadLibrary, so no import table names it.
    CHECK(isModernDirect3DFileName("libGLESv2.dll"));
    CHECK(isModernDirect3DFileName("libEGL.dll"));
    CHECK(!isModernDirect3DFileName("game.exe"));
    CHECK(!isModernDirect3DFileName("d3dx9_43.dll"));
}

BOXEDVN_TEST(pe_import_reader_reads_the_import_directory) {
    TemporaryDirectory temporary("d3d-imports");
    const fs::path file = temporary.path / "engine.dll";
    writeFile(file, buildPe({"KERNEL32.dll", "d3d11.dll", "USER32.dll"}));

    const std::vector<std::string> modules =
        readPeImportedModules(file.string());
    CHECK(contains(modules, "kernel32.dll"));
    CHECK(contains(modules, "d3d11.dll"));
    CHECK(contains(modules, "user32.dll"));
}

BOXEDVN_TEST(pe_import_reader_reads_the_delay_import_directory) {
    TemporaryDirectory temporary("d3d-delay");
    const fs::path file = temporary.path / "engine.dll";
    writeFile(file, buildPe({"dxgi.dll"}, /*delayed=*/true));

    const std::vector<std::string> modules =
        readPeImportedModules(file.string());
    CHECK(contains(modules, "dxgi.dll"));
}

BOXEDVN_TEST(pe_import_reader_refuses_files_that_are_not_pe_images) {
    TemporaryDirectory temporary("d3d-garbage");

    const fs::path text = temporary.path / "readme.exe";
    writeFile(text, std::vector<std::uint8_t>{'h', 'e', 'l', 'l', 'o'});
    CHECK(readPeImportedModules(text.string()).empty());

    // A PE truncated part way through its headers must yield nothing rather
    // than reading past the end of the file.
    std::vector<std::uint8_t> truncated = buildPe({"d3d11.dll"});
    truncated.resize(truncated.size() / 3);
    const fs::path cut = temporary.path / "cut.dll";
    writeFile(cut, truncated);
    CHECK(readPeImportedModules(cut.string()).empty());

    CHECK(readPeImportedModules(
              (temporary.path / "absent.dll").string()).empty());
}

BOXEDVN_TEST(direct3d_scan_finds_the_dependency_in_a_bundled_dll) {
    // The shape that used to need an entry in a hard-coded list of executable
    // names: the launched program links a private engine DLL, and only that
    // DLL names Direct3D 11.
    TemporaryDirectory temporary("d3d-scan-dll");
    writeFile(temporary.path / "game.exe", buildPe({"engine.dll"}));
    writeFile(temporary.path / "engine.dll", buildPe({"d3d11.dll"}));

    const Direct3DUsage usage = detectDirect3DUsage(temporary.path.string());
    CHECK(usage.needsModernDirect3D);
    CHECK_CONTAINS(usage.evidence, "engine.dll");
    CHECK_CONTAINS(usage.evidence, "d3d11.dll");
}

BOXEDVN_TEST(direct3d_scan_finds_a_bundled_angle_in_a_subdirectory) {
    TemporaryDirectory temporary("d3d-scan-angle");
    std::error_code ec;
    fs::create_directories(temporary.path / "package" / "bin", ec);
    writeFile(temporary.path / "Game.exe", buildPe({"KERNEL32.dll"}));
    writeFile(temporary.path / "package" / "bin" / "libGLESv2.dll",
              buildPe({"KERNEL32.dll"}));

    const Direct3DUsage usage = detectDirect3DUsage(temporary.path.string());
    CHECK(usage.needsModernDirect3D);
    CHECK_CONTAINS(usage.evidence, "libGLESv2.dll");
}

BOXEDVN_TEST(direct3d_scan_leaves_a_direct3d_9_game_alone) {
    TemporaryDirectory temporary("d3d-scan-legacy");
    writeFile(temporary.path / "game.exe",
              buildPe({"KERNEL32.dll", "d3d9.dll", "d3dx9_43.dll"}));
    writeFile(temporary.path / "movie.dll", buildPe({"winmm.dll"}));

    const Direct3DUsage usage = detectDirect3DUsage(temporary.path.string());
    CHECK(!usage.needsModernDirect3D);
    CHECK(!usage.reachedLimit);
    CHECK(usage.evidence.empty());
}

BOXEDVN_TEST(direct3d_scan_reports_when_it_stopped_early) {
    TemporaryDirectory temporary("d3d-scan-limit");
    for (int i = 0; i < 6; ++i) {
        writeFile(temporary.path / ("mod" + std::to_string(i) + ".dll"),
                  buildPe({"KERNEL32.dll"}));
    }

    const Direct3DUsage usage =
        detectDirect3DUsage(temporary.path.string(), /*maxFiles=*/3);
    CHECK(!usage.needsModernDirect3D);
    // A negative result from a scan that gave up is not the same claim as a
    // negative result from a complete one, and the caller has to be able to
    // tell them apart.
    CHECK(usage.reachedLimit);
}

BOXEDVN_TEST(direct3d_scan_tolerates_a_missing_directory) {
    const Direct3DUsage usage = detectDirect3DUsage("/no/such/game");
    CHECK(!usage.needsModernDirect3D);
    CHECK(!usage.reachedLimit);
    CHECK(detectDirect3DUsage("").needsModernDirect3D == false);
}

// The renderer policy itself, driven by real files. This is the case that
// used to require the game's executable name to appear in a hard-coded list:
// the launched program links a private engine DLL, and only that DLL names
// Direct3D 11.
BOXEDVN_TEST(automatic_renderer_enables_dxvk_from_the_binaries) {
    TemporaryDirectory temporary("d3d-policy-modern");
    writeFile(temporary.path / "Saya_en.exe", buildPe({"Mware.dll"}));
    writeFile(temporary.path / "Mware.dll", buildPe({"d3d11.dll"}));

    BVNLaunchConfiguration launch;
    launch.runThroughWine = true;
    launch.gameDirectoryHostPath = temporary.path.string();
    launch.executablePath = "D:\\Saya_en.exe";

    BVNApplyDefaultRendererPolicy(launch);

    CHECK_EQ(launch.useWineD3DVulkanRenderer, true);
    CHECK_EQ(launch.enableWineD3DVulkan, true);
    CHECK_CONTAINS(launch.rendererReason, "d3d11.dll");
}

BOXEDVN_TEST(automatic_renderer_keeps_wined3d_for_a_direct3d_9_game) {
    TemporaryDirectory temporary("d3d-policy-legacy");
    writeFile(temporary.path / "game.exe", buildPe({"d3d9.dll"}));

    BVNLaunchConfiguration launch;
    launch.runThroughWine = true;
    launch.gameDirectoryHostPath = temporary.path.string();
    launch.executablePath = "D:\\game.exe";

    BVNApplyDefaultRendererPolicy(launch);

    CHECK_EQ(launch.useWineD3DVulkanRenderer, true);
    CHECK_EQ(launch.enableWineD3DVulkan, false);
}

BOXEDVN_TEST(automatic_renderer_enables_dxvk_for_a_chromium_engine) {
    // A Chromium, Electron or NW.js title: the launcher itself links nothing
    // graphical, and the D3D11 client is ANGLE in the GPU process. Under
    // WineD3D that process finds no supported feature level and falls back to
    // rasterising in software, which translates enough x86 to run the JIT
    // arena dry before the first frame.
    TemporaryDirectory temporary("d3d-policy-angle");
    writeFile(temporary.path / "Game.exe", buildPe({"KERNEL32.dll"}));
    writeFile(temporary.path / "libGLESv2.dll", buildPe({"KERNEL32.dll"}));

    BVNLaunchConfiguration launch;
    launch.runThroughWine = true;
    launch.gameDirectoryHostPath = temporary.path.string();
    launch.executablePath = "D:\\Game.exe";

    BVNApplyDefaultRendererPolicy(launch);

    CHECK_EQ(launch.enableWineD3DVulkan, true);
    CHECK_CONTAINS(launch.rendererReason, "libGLESv2.dll");
}

BOXEDVN_TEST(automatic_renderer_is_not_consulted_when_the_user_chose_one) {
    TemporaryDirectory temporary("d3d-policy-override");
    writeFile(temporary.path / "engine.dll", buildPe({"d3d11.dll"}));

    BVNLaunchConfiguration launch;
    launch.runThroughWine = true;
    launch.gameDirectoryHostPath = temporary.path.string();
    launch.executablePath = "D:\\game.exe";
    launch.requestedWineRenderer = 1;  // force WineD3D

    BVNApplyDefaultRendererPolicy(launch);

    CHECK_EQ(launch.enableWineD3DVulkan, false);
    CHECK_CONTAINS(launch.rendererReason, "explicitly");
}
