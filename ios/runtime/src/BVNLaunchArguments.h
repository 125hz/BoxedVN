/*
 * BoxedVN - pure Boxedwine command-line construction.
 * GPLv2; see license.txt.
 */

#ifndef BVN_LAUNCH_ARGUMENTS_H
#define BVN_LAUNCH_ARGUMENTS_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct BVNLaunchConfiguration {
    std::string rootFilesystemZipPath;
    std::string writableRootPath;
    std::string gameDirectoryHostPath;
    // One host directory mounted as E: in every Wine session. Games retain
    // independent prefixes for save/registry compatibility while documents
    // intentionally shared between games and the file browser live here.
    std::string sharedDirectoryHostPath;
    std::string executablePath;
    std::vector<std::string> arguments;
    std::vector<std::string> environment;
    std::vector<std::string> interpreterModules;
    // Inclusive start, exclusive end in the 32-bit guest address space.
    std::vector<std::pair<uint32_t, uint32_t>> interpreterRanges;
    std::string workingDirectory;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitsPerPixel = 0;
    bool soundEnabled = true;
    bool runThroughWine = true;
    // 0 automatic, 1 force WineD3D, 2 force DXVK. Mirrors the C launch ABI
    // without making this host-independent unit dependent on Objective-C.
    int requestedWineRenderer = 0;
    // Every imported game needs WineD3D's Vulkan renderer on iOS because the
    // iPhone build has no host OpenGL backend. This is independent of DXVK:
    // Direct3D 9 titles can use WineD3D-over-Vulkan while DXVK stays disabled.
    bool useWineD3DVulkanRenderer = false;
    // WineD3D's Vulkan renderer is the compatibility path for 32-bit D3D9
    // games on iOS. This flag specifically selects Boxedwine's patched DXVK
    // DLL overlay for titles that require it.
    bool enableWineD3DVulkan = false;
};

/// Selects the generic iOS renderer policy before a title profile narrows it.
/// Imported games always use a Vulkan host path; a profile may disable DXVK
/// without disabling WineD3D-over-Vulkan.
void BVNApplyDefaultRendererPolicy(BVNLaunchConfiguration& launch);

/// Adds a narrowly scoped engine workaround when the requested Windows
/// program is one for which device diagnostics have established that need.
/// Returns true when a profile was selected.
bool BVNApplyKnownCompatibilityProfile(BVNLaunchConfiguration& launch);

/// Builds the exact argv passed to Boxedwine. This is pure C++ so the command
/// line can be regression-tested without UIKit or an iOS device.
std::vector<std::string> BVNBuildLaunchArguments(
    const BVNLaunchConfiguration& launch);

#endif  // BVN_LAUNCH_ARGUMENTS_H
