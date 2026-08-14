/*
 * BoxedVN - pure Boxedwine command-line construction.
 * GPLv2; see license.txt.
 */

#include "BVNLaunchArguments.h"

#include "boxedvn/direct3d_profile.h"
#include "boxedvn/engine_profile.h"

#include <initializer_list>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace {

bool endsWithIgnoringCase(std::string candidate, const char* suffix) {
    std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    const std::string normalizedSuffix(suffix);
    return candidate.size() >= normalizedSuffix.size() &&
           candidate.compare(candidate.size() - normalizedSuffix.size(),
                             normalizedSuffix.size(), normalizedSuffix) == 0;
}

bool equalsIgnoringCase(std::string left, const char* right) {
    std::transform(left.begin(), left.end(), left.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return left == right;
}

bool launchesAnyOf(const BVNLaunchConfiguration& launch,
                   std::initializer_list<const char*> executables) {
    for (const char* executable : executables) {
        if (endsWithIgnoringCase(launch.executablePath, executable)) {
            return true;
        }
        for (const std::string& argument : launch.arguments) {
            if (endsWithIgnoringCase(argument, executable)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

const char kInterpretModulePrefix[] = "--bvn-interpret=";
const char kInterpretRangePrefix[] = "--bvn-interpret-range=";
const char kLocalePrefix[] = "--bvn-locale=";

namespace {

// "0076b60000-0076b8e000" or "76b60000-76b8e000". Returns false for anything
// that is not two hexadecimal numbers with a non-empty range between them,
// because a silently-ignored range would look exactly like a range that was
// applied and found innocent.
bool parseInterpreterRange(const std::string& text, uint32_t& start,
                           uint32_t& end) {
    const std::size_t dash = text.find('-');
    if (dash == std::string::npos || dash == 0 ||
        dash + 1 >= text.size()) {
        return false;
    }
    const std::string startText = text.substr(0, dash);
    const std::string endText = text.substr(dash + 1);
    if (startText.size() > 8 || endText.size() > 8) {
        return false;
    }
    auto parseHex = [](const std::string& value, uint32_t& out) {
        out = 0;
        for (const char character : value) {
            uint32_t digit;
            if (character >= '0' && character <= '9') {
                digit = static_cast<uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                digit = static_cast<uint32_t>(character - 'a') + 10;
            } else if (character >= 'A' && character <= 'F') {
                digit = static_cast<uint32_t>(character - 'A') + 10;
            } else {
                return false;
            }
            out = (out << 4) | digit;
        }
        return true;
    };
    return parseHex(startText, start) && parseHex(endText, end) && start < end;
}

}  // namespace

void BVNApplyGeneralArgumentSentinels(BVNLaunchConfiguration& launch) {
    const std::string prefix(kInterpretModulePrefix);
    std::vector<std::string> remaining;
    remaining.reserve(launch.arguments.size());

    for (const std::string& argument : launch.arguments) {
        if (argument.size() > prefix.size() &&
            argument.compare(0, prefix.size(), prefix) == 0) {
            const std::string module = argument.substr(prefix.size());
            // This exact value was issued as a temporary diagnostic while an
            // ARM64 RCR carry defect was isolated.  Keeping it in an existing
            // game manifest after the core correction would still interpret
            // all of Chromium's media DLL and make its event loop appear
            // frozen. Retire the old broad workaround; a future explicit
            // diagnostic can still name ffmpeg.dll or use an address range.
            if (!module.empty() && !equalsIgnoringCase(module, "ffmpeg")) {
                launch.interpreterModules.push_back(module);
            }
            continue;  // BoxedVN's word; the guest must never see it.
        }
        const std::string localePrefix(kLocalePrefix);
        if (argument.size() > localePrefix.size() &&
            argument.compare(0, localePrefix.size(), localePrefix) == 0) {
            launch.guestLocale = argument.substr(localePrefix.size());
            continue;
        }
        const std::string rangePrefix(kInterpretRangePrefix);
        if (argument.size() > rangePrefix.size() &&
            argument.compare(0, rangePrefix.size(), rangePrefix) == 0) {
            uint32_t start = 0;
            uint32_t end = 0;
            if (parseInterpreterRange(argument.substr(rangePrefix.size()),
                                      start, end)) {
                launch.interpreterRanges.emplace_back(start, end);
            }
            continue;
        }
        remaining.push_back(argument);
    }
    launch.arguments = remaining;
}

void BVNApplyDefaultRendererPolicy(BVNLaunchConfiguration& launch) {
    const bool importedWineGame = launch.runThroughWine &&
        !launch.gameDirectoryHostPath.empty();
    launch.useWineD3DVulkanRenderer = importedWineGame;
    // WineD3D-over-Vulkan is the broad compatibility path for the classic
    // D3D8/D3D9 engines BoxedVN primarily targets. Forcing DXVK into every
    // imported game is unsafe: games may probe its D3D10/11 DLLs even when
    // their main renderer is older, and DXVK can request Metal-impossible
    // features before the game has a chance to fall back. Users can override
    // this choice per game.
    launch.enableWineD3DVulkan = importedWineGame &&
        launch.requestedWineRenderer == 2;
    if (!importedWineGame) {
        launch.rendererReason.clear();
        return;
    }
    if (launch.requestedWineRenderer != 0) {
        launch.rendererReason = launch.requestedWineRenderer == 2
            ? "DXVK was selected explicitly in this game's launch settings."
            : "WineD3D was selected explicitly in this game's launch "
              "settings.";
        return;
    }

    // Which games get DXVK used to be a list of executable names, which meant
    // every unlisted Direct3D 10/11 title silently got the one renderer that
    // structurally cannot run it. wined3d's Vulkan adapter cannot advertise
    // Direct3D 10 on Metal - there is no geometry shader stage - so it
    // reports no supported feature level, the game's device creation fails,
    // and whatever fallback it has takes over. For a Chromium-based engine
    // that fallback is a software rasteriser, which then translates enough
    // x86 to exhaust the JIT arena before the first frame.
    //
    // Ask the binaries instead. They say which Direct3D they link, and that
    // answer covers titles nobody has sat down with.
    const std::string& compatibilityDirectory =
        launch.compatibilityDirectoryHostPath.empty()
            ? launch.gameDirectoryHostPath
            : launch.compatibilityDirectoryHostPath;
    const boxedvn::Direct3DUsage usage =
        boxedvn::detectDirect3DUsage(compatibilityDirectory);
    if (usage.needsModernDirect3D) {
        launch.enableWineD3DVulkan = true;
        launch.rendererReason =
            "DXVK: " + usage.evidence +
            ", and WineD3D cannot reach Direct3D 10 on Metal.";
        return;
    }
    launch.rendererReason = usage.reachedLimit
        ? "WineD3D: no Direct3D 10/11 module was found, but the scan stopped "
          "at its file limit, so this is not conclusive."
        : "WineD3D: nothing in this game links Direct3D 10, 11 or 12.";
}

BVNEngineProfileResult BVNApplyEngineCompatibilityProfile(
    BVNLaunchConfiguration& launch) {
    BVNEngineProfileResult result;
    if (!launch.runThroughWine || launch.gameDirectoryHostPath.empty()) {
        return result;
    }

    const std::string& compatibilityDirectory =
        launch.compatibilityDirectoryHostPath.empty()
            ? launch.gameDirectoryHostPath
            : launch.compatibilityDirectoryHostPath;
    const boxedvn::GuestEngineProfile profile =
        boxedvn::detectGuestEngine(compatibilityDirectory);
    if (!profile.isChromium()) {
        return result;
    }

    std::string engine = boxedvn::toString(profile.engine);
    if (profile.framework != boxedvn::GuestFramework::Unknown) {
        engine += " (";
        engine += boxedvn::toString(profile.framework);
        engine += ")";
    }

    const boxedvn::ChromiumSwitchMerge merged =
        boxedvn::mergeChromiumSwitches(launch.arguments);
    launch.arguments = merged.arguments;
    result.bootDiagnostics = merged.bootDiagnostics;
    // Only for RPG Maker: the shim knows that engine's boot gate by name, and
    // installing it into an unrelated NW.js application would be a change
    // with no reason behind it.
    result.rpgMaker =
        profile.framework == boxedvn::GuestFramework::RpgMakerMv ||
        profile.framework == boxedvn::GuestFramework::RpgMakerMz;
    result.fontGateShim = merged.fontGateShim && result.rpgMaker;

    if (merged.optedOut) {
        result.reason = std::string(engine) + " detected (" +
            profile.evidence + "), but this game's launch settings ask for no "
            "default switches, so BoxedVN added none.";
        return result;
    }

    // V8 emits x86 into anonymous executable heaps. On ARM64 iOS that x86
    // otherwise goes through Boxedwine's JIT as a second JIT tier; the device
    // hang snapshot showed the browser main thread spending 97.7% of a core
    // in exactly such a block while Blink's decoded-image callback stopped.
    // Build 124 proved the renderer completes this path when generated code
    // uses the decoded interpreter. Builds 125-126 tried to retain JIT for
    // most V8 pages and detect only a repeated EIP; both regressed to the
    // title-only stall without ever triggering, because this failure stops
    // forward progress without staying at one dispatcher entry. Keep Wine
    // and every mapped PE/ELF module JIT compiled, but interpret V8's
    // anonymous executable heaps consistently. The CPU policy recognises
    // Wine's 0x7D400000 relay and Boxedwine's 0xF0000000 callback stubs as
    // system-generated anonymous code and leaves those hot transitions on
    // the ARM64 translator.
    launch.interpretAnonymousExecutable = true;

    result.applied = merged.added > 0 || merged.mergedFeatures;
    result.reason = std::string(engine) + " detected (" + profile.evidence +
        "): a browser engine draws through Windows facilities Wine only "
        "partly implements, so BoxedVN passes Chromium's own switches for "
        "the sandbox, the GPU process, DirectComposition, occlusion and "
        "window sizing, and decodes anonymous browser-generated code while "
        "mapped Wine and browser modules remain JIT compiled. ";

    char detail[224];
    snprintf(detail, sizeof(detail),
             "%zu added, %zu left to this game's own launch settings%s. Put "
             "\"%s\" in the arguments field to add none.",
             merged.added, merged.deferredToUser,
             merged.mergedFeatures ? ", one feature list combined" : "",
             boxedvn::kChromiumDefaultsOptOut);
    result.reason += detail;
    return result;
}

bool BVNApplyKnownCompatibilityProfile(BVNLaunchConfiguration& launch) {
    // The Fruit of Grisaia: run it WITHOUT DXVK.
    //
    // Its build-65 device log shows DXVK failing to create a Vulkan device six
    // times in a row, every attempt rejected by MoltenVK for the same four
    // features:
    //
    //   VK_ERROR_FEATURE_NOT_PRESENT: ... the 5th flag in VkPhysicalDeviceFeatures
    //       (geometryShader)
    //   ... the 39th flag in VkPhysicalDeviceFeatures  (shaderCullDistance)
    //   ... the 1st flag in VkPhysicalDeviceRobustness2FeaturesKHR
    //       (robustBufferAccess2)
    //   ... the 3rd flag  (nullDescriptor)
    //   err:   DxvkAdapter: Failed to create device
    //
    // Those are exactly the four the MoltenVK patch relaxes, and Saya's DXVK
    // device in the same build requests none of them - so this title reaches a
    // DXVK path the patch does not cover. The game then shows its "DirectX
    // failed" dialog and dereferences the interface pointer it never got
    // (page fault reading 0x0000000F in grisaia+0x12a2bc).
    //
    // In the same session wined3d's Vulkan renderer created a device without
    // complaint. This engine is Direct3D 9, which wined3d handles on Metal -
    // the shader-model-4 wall that forces Saya through DXVK does not apply.
    // So the cheapest correct thing is to keep DXVK out of its way.
    //
    // This is an experiment with a clear falsifier: if the DirectX dialog
    // still appears, DXVK was not the cause and this profile should be
    // removed rather than elaborated.
    //
    // NOT YET TESTED. Build 66 shipped this profile but BVNRuntime called
    // BVNApplyKnownCompatibilityProfile *before* it assigned
    // enableWineD3DVulkan, so the flag was overwritten two lines later and the
    // device log still shows "-dxvk 1" with the same six DXVK failures. The
    // ordering is fixed in build 67; the falsifier above applies to that run,
    // not to build 66's.
    if (launchesAnyOf(launch, {"bootmenu.exe", "grisaia.exe"})) {
        // Automatic and WineD3D honor the measured compatibility result, but
        // an explicit DXVK selection in launch settings is a real override.
        if (launch.requestedWineRenderer != 2) {
            launch.enableWineD3DVulkan = false;
            launch.rendererReason =
                "WineD3D: a per-title compatibility profile overrides the "
                "automatic choice, because DXVK failed to create a device "
                "for this engine on device.";
        }
        return true;
    }

    if (!launchesAnyOf(launch, {"saya_en.exe"})) {
        return false;
    }
    // Builds 31-34 isolated the fault to Boxedwine's optimized REP MOVS JIT
    // loop, not to mvware as a whole. The generic CPU translator now handles
    // short overlapping copies correctly, so Saya must retain JIT for all of
    // its hot engine code. Keep this selector for an explicit runtime log and
    // future game-specific policy, but do not install an interpreter range.

    // Build 43 established that this title cannot render through wined3d on
    // Metal, and why: Metal has no geometry shader stage, MoltenVK therefore
    // reports geometryShader = false on every device, Direct3D 10 mandates
    // geometry shaders, so wined3d never advertises shader model 4 and skips
    // every SM4 code chunk. Saya's rendering is 331 SM4 shaders; across a full
    // run exactly four shaders are created, all SM2. Capping the reported
    // shader model changed nothing, proving the game does not adapt to the
    // feature level it is given.
    //
    // Build 44 asked whether the engine has any non-Direct3D-11 path, by
    // disabling d3d11/dxgi. The answer is no, and it is final:
    //
    //   import_dll Library d3d11.dll (which is needed by L"D:\Mware.dll") not found
    //   loader_init Importing dlls for L"D:\Saya_en.exe" failed, status c0000135
    //
    // Mware.dll links d3d11.dll statically in its PE import table, so the
    // dependency is resolved by the loader before any game code runs. There is
    // no software or DirectDraw fallback to reach. That override is therefore
    // reverted rather than kept: it can only stop the game launching at all.

    // Build 57 RUN A. Build 56 settled what the stalled thread is doing: the
    // kernel reports it RUNNABLE while consuming 981,821 us of CPU in a
    // 1000 ms window - 98% of a core - with the host PC moving inside a small
    // window and a constant return into the dispatcher. It is spinning in
    // JIT-compiled code, not blocked and not starved.
    //
    // The guest block it spins in is d3d11.dll+0x259B8F, the fixed-size
    // BindFramebuffer render-target loop, which cannot legitimately burn a
    // core for tens of seconds. Running that module through the interpreter
    // instead of the ARM64 JIT tests the translation directly: if the spin
    // disappears, the JIT's rendering of that block is at fault. This is a
    // diagnostic, not a fix - interpreting DXVK is far too slow to ship.
    launch.interpreterModules.push_back("d3d11");
    return true;
}

std::vector<std::string> BVNBuildLaunchArguments(
    const BVNLaunchConfiguration& launch) {
    std::vector<std::string> argv;
    argv.push_back("boxedvn");

    argv.push_back("-root");
    argv.push_back(launch.writableRootPath);

    argv.push_back("-zip");
    argv.push_back(launch.rootFilesystemZipPath);

    // StartUpArgs::parseStartupArgs treats -nozip as a valueless switch.
    // Supplying "1" here leaves it unconsumed, making Boxedwine launch a
    // guest executable literally named "1" instead of /bin/wine.
    argv.push_back("-nozip");

    // Do not pass BoxedVN's own session-log path through Boxedwine's -log.
    // StartUpArgs opens that path with createNew(), truncating all frontend
    // and JIT diagnostics written before boxedmain(). Boxedwine's klog output
    // already goes to stdout/stderr, which BVNLog captures into the same
    // continuous session file without a competing writer.

    if (!launch.gameDirectoryHostPath.empty()) {
        argv.push_back("-mount_drive");
        argv.push_back(launch.gameDirectoryHostPath);
        argv.push_back(std::string(1, launch.gameDriveLetter));
    }
    if (!launch.sharedDirectoryHostPath.empty()) {
        argv.push_back("-mount_drive");
        argv.push_back(launch.sharedDirectoryHostPath);
        argv.push_back(std::string(1, launch.sharedDriveLetter));
    }

    if (launch.width > 0 && launch.height > 0) {
        argv.push_back("-resolution");
        argv.push_back(std::to_string(launch.width) + "x" +
                       std::to_string(launch.height));
    } else if (launch.runThroughWine &&
               !launch.gameDirectoryHostPath.empty()) {
        // A game's "default" resolution used to leave Boxedwine's X11 root
        // at 800x600. Games are free to create a larger client afterwards
        // (Grisaia switches to 1280x720), but Wine has already cached the
        // original 800-pixel monitor geometry by then. Its X11 driver scales
        // absolute pointer positions back through that stale monitor, so an
        // otherwise-correct x=1218 arrives at the Windows game near x=761.
        //
        // Start generic game sessions on a coherent 1366x1024 virtual
        // monitor. Wine caches Windows monitor geometry before a game creates
        // its rendering client. A monitor equal to a 1280x960 client is still
        // too small for the real decorated window (the measured Fate client
        // begins at y=53 and ends at y=1013), so Wine rejects lower-screen
        // input after BoxedVN has delivered it correctly. This desktop
        // contains common 1280-wide 16:9 and 4:3 windows plus ordinary Wine
        // decorations. A user-selected per-game resolution remains
        // authoritative.
        argv.push_back("-resolution");
        argv.push_back("1366x1024");
    }
    if (launch.bitsPerPixel == 16 || launch.bitsPerPixel == 32) {
        argv.push_back("-bpp");
        argv.push_back(std::to_string(launch.bitsPerPixel));
    }
    if (!launch.soundEnabled) {
        argv.push_back("-nosound");
    }

    // Route Direct3D through DXVK rather than wined3d. wined3d gates shader
    // model 4 on Direct3D feature level 10, which mandates geometry shaders,
    // and Metal has no geometry shader stage - so wined3d can never run a
    // Direct3D 11 title on MoltenVK (builds 40-45). DXVK translates DXBC to
    // SPIR-V itself and needs no geometry stage to compile a vertex or pixel
    // shader; its GetMaxFeatureLevel() floor is 10_1.
    //
    // Boxedwine's own switch does the rest: it maps drive_c/dxvk over system32
    // and sets WINEDLLOVERRIDES so the native modules win over Wine's builtins.
    if (launch.enableWineD3DVulkan) {
        argv.push_back("-dxvk");
        argv.push_back("1");
    }
    if (!launch.workingDirectory.empty()) {
        argv.push_back("-w");
        argv.push_back(launch.workingDirectory);
    }

    // WineD3D reaching a graphics pipeline with no shader stages means its
    // DXBC -> SPIR-V translation produced nothing, and at Wine's default
    // verbosity that failure is silent: only the downstream "Failed to get
    // graphics pipeline" is visible, which names the symptom rather than the
    // cause. Raising the d3d_shader channel to warn costs a bounded number of
    // lines because it fires per shader, not per draw. A caller-supplied
    // WINEDEBUG always wins, so this never overrides a deliberate choice.
    // Only for the accelerated path: the shader translator is what fails
    // there, and a plain Wine app has no reason to carry the extra channel.
    bool callerSetWineDebug = false;
    for (const std::string& entry : launch.environment) {
        if (entry.rfind("WINEDEBUG=", 0) == 0) {
            callerSetWineDebug = true;
            break;
        }
    }
    if (launch.enableWineD3DVulkan && !callerSetWineDebug) {
        // This Wine build has no separate vkd3d channel: the DXBC container
        // parse lives inside d3d_shader itself. At warn level it reports only
        // the outer "Failed to parse DXBC, hr 0x80070057", and notably none of
        // the specific warnings the container checks emit ("Wrong tag",
        // "unexpected DXBC version"), so the reason is below warn level.
        // Trace on this one channel prints the section tags and version, which
        // is what names the fault. It stays bounded because the failing
        // shaders stop early, and only wined3d's two internal shaders parse
        // far enough to trace in full.
        //
        // The d3d channel is silenced deliberately: a stuck guest retries the
        // same draw forever and it contributed 5,792 identical lines to the
        // previous log, crowding out the evidence. BoxedVN's own host-side
        // pipeline log already records that path.
        // Full d3d_shader trace did its job in build 42 and is not worth its
        // cost now that the cause is known. Keep warn, which still reports a
        // shader that fails to load, and keep the d3d channel silenced: a
        // stuck guest retries the same draw forever and contributed 5,792
        // identical lines before it was turned off.
        argv.push_back("-env");
        argv.push_back("WINEDEBUG=warn+d3d_shader,-d3d");
    }

    for (const std::string& entry : launch.environment) {
        argv.push_back("-env");
        argv.push_back(entry);
    }
    for (const std::string& module : launch.interpreterModules) {
        argv.push_back("-interpreterModule");
        argv.push_back(module);
    }
    if (launch.interpretAnonymousExecutable) {
        argv.push_back("-interpreterAnonymousExecutable");
    }
    for (const auto& range : launch.interpreterRanges) {
        char encodedRange[18];
        snprintf(encodedRange, sizeof(encodedRange), "%08x-%08x",
                 range.first, range.second);
        argv.push_back("-interpreterRange");
        argv.emplace_back(encodedRange);
    }

    if (launch.runThroughWine) {
        argv.push_back("/bin/wine");
    }
    argv.push_back(launch.executablePath);
    for (const std::string& argument : launch.arguments) {
        argv.push_back(argument);
    }
    return argv;
}
