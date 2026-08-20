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
    // Additional read-only root layers, mounted after the ordinary IA-32
    // archive. The x86-64 runtime uses these for glibc and Wine64 while the
    // writable overlay and BoxedWine kernel remain the same.
    std::vector<std::string> rootFilesystemOverlayZipPaths;
    std::string writableRootPath;
    std::string gameDirectoryHostPath;
    // Directory containing the selected executable, used for engine and
    // Direct3D inspection. In a shared container this is deliberately narrower
    // than gameDirectoryHostPath, so one installed title cannot donate nw.dll
    // or d3d11.dll to another title's compatibility decision.
    std::string compatibilityDirectoryHostPath;
    // One host directory mounted as E: in every Wine session. Games retain
    // independent prefixes for save/registry compatibility while documents
    // intentionally shared between games and the file browser live here.
    std::string sharedDirectoryHostPath;
    char gameDriveLetter = 'd';
    char sharedDriveLetter = 'e';
    // Wine's documented compatibility version name (for example win10,
    // win7, or winxp). Empty keeps the root filesystem default.
    std::string windowsVersion;
    std::string executablePath;
    std::vector<std::string> arguments;
    std::vector<std::string> environment;
    std::vector<std::string> interpreterModules;
    // Interpret executable guest memory that is neither an ELF mapping nor a
    // PE image. Browser engines generate x86 into anonymous heaps; mapped
    // Wine/ELF and browser PE code stays ARM64-JIT compiled.
    bool interpretAnonymousExecutable = false;
    // Inclusive start, exclusive end in the 32-bit guest address space.
    std::vector<std::pair<uint32_t, uint32_t>> interpreterRanges;
    std::string workingDirectory;
    /// "ja" selects a CP932 prefix; empty leaves the root filesystem's own.
    std::string guestLocale;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitsPerPixel = 0;
    bool soundEnabled = true;
    bool runThroughWine = true;
    // Select the x86-64 process path and its optional FEX CPU backend. This is
    // deliberately per launch; IA-32 sessions continue to use BoxedWine's
    // existing CPU/JIT and root filesystem.
    bool useFEX64 = false;
    // Present Direct3D through the native DXMT Metal bridge for this launch.
    bool useDXMT = false;
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
    // Why the renderer above was chosen, in one sentence, for the session
    // log. A wrong renderer shows up as a game that starts and then renders
    // nothing, so the reason has to be recoverable from the log rather than
    // reconstructed from the source.
    std::string rendererReason;
};

/// A launch-settings line asking for one guest module to run through the
/// interpreter instead of the ARM64 JIT: `--bvn-interpret=<name>`, repeatable.
/// Stripped before launch and never passed to the guest.
extern const char kInterpretModulePrefix[];

/// A launch-settings line asking for one guest address range to run through
/// the interpreter: `--bvn-interpret-range=START-END`, hexadecimal, inclusive
/// start and exclusive end, repeatable. Stripped before launch.
///
/// This is how a defect is narrowed from a module to an instruction: halve
/// the range, see which half the fault follows.
extern const char kInterpretRangePrefix[];

/// `--bvn-locale=ja` in launch settings: present the prefix to this game as a
/// Japanese Windows install (ANSI codepage 932). Stripped before launch.
extern const char kLocalePrefix[];

/// Handles the launch-settings lines that are BoxedVN's own words rather than
/// the guest's, for every launch rather than only for a recognised engine.
///
/// Currently one: interpreter selection. That exists because it is the only
/// experiment that cleanly separates "this guest computed a bad value" from
/// "this emulator translated its code wrongly" - run the suspect module
/// through the interpreter and see whether the fault survives. Builds 31-34
/// used exactly that to isolate a REP MOVS defect, but the selector was
/// hardcoded per title; this puts it in the hands of whoever is holding the
/// device.
void BVNApplyGeneralArgumentSentinels(BVNLaunchConfiguration& launch);

/// Selects the generic iOS renderer policy before a title profile narrows it.
/// Imported games always use a Vulkan host path; a profile may disable DXVK
/// without disabling WineD3D-over-Vulkan.
void BVNApplyDefaultRendererPolicy(BVNLaunchConfiguration& launch);

struct BVNEngineProfileResult {
    /// True when switches were actually appended to `launch.arguments`.
    bool applied = false;
    /// True when this game's launch settings asked BoxedVN to instrument the
    /// guest's own HTML. The caller performs the installation; deciding it
    /// here keeps the sentinel's handling next to the other one.
    bool bootDiagnostics = false;
    /// True when the RPG Maker font-gate shim should be installed. On by
    /// default for a recognised RPG Maker guest, because without it that
    /// guest does not finish starting.
    bool fontGateShim = false;
    /// True when the guest is RPG Maker, so the caller can apply the engine
    /// shims that only make sense there.
    bool rpgMaker = false;
    /// One sentence for the session log, naming the engine and what was done
    /// about it. Non-empty whenever an engine was recognised, including the
    /// case where BoxedVN deliberately added nothing.
    std::string reason;
};

/// Recognises the application engine an imported game is built on and passes
/// that engine's compatibility switches. Unlike the per-title profile below,
/// this is keyed to the engine, so it covers every game built on it. Applies
/// only to imported Wine games; a bare Wine program is left alone.
BVNEngineProfileResult BVNApplyEngineCompatibilityProfile(
    BVNLaunchConfiguration& launch);

/// Adds a narrowly scoped engine workaround when the requested Windows
/// program is one for which device diagnostics have established that need.
/// Returns true when a profile was selected.
bool BVNApplyKnownCompatibilityProfile(BVNLaunchConfiguration& launch);

/// Builds the exact argv passed to Boxedwine. This is pure C++ so the command
/// line can be regression-tested without UIKit or an iOS device.
std::vector<std::string> BVNBuildLaunchArguments(
    const BVNLaunchConfiguration& launch);

#endif  // BVN_LAUNCH_ARGUMENTS_H
