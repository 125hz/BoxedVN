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
    // WineD3D's Vulkan renderer is the compatibility path for 32-bit D3D9
    // games on iOS. Upstream DXVK 2.5.2 requires Vulkan geometry shaders and
    // transform feedback, neither of which MoltenVK currently exposes.
    bool enableWineD3DVulkan = false;
    // Some event-driven Windows engines only advance their render loop while
    // receiving pointer/window activity. The compatibility profile may ask
    // Boxedwine to emit an unchanged X11 motion event at 30 Hz.
    bool x11MotionHeartbeat = false;
};

/// Adds a narrowly scoped engine workaround when the requested Windows
/// program is one for which device diagnostics have established that need.
/// Returns true when a profile was selected.
bool BVNApplyKnownCompatibilityProfile(BVNLaunchConfiguration& launch);

/// Builds the exact argv passed to Boxedwine. This is pure C++ so the command
/// line can be regression-tested without UIKit or an iOS device.
std::vector<std::string> BVNBuildLaunchArguments(
    const BVNLaunchConfiguration& launch);

#endif  // BVN_LAUNCH_ARGUMENTS_H
