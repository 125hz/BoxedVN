/*
 * BoxedVN fex64 - minimal embedded Wine bootstrap for iPhoneOS.
 * Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
 */

#ifndef BVN_WINE_BRIDGE_H
#define BVN_WINE_BRIDGE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BVNWineStageUnavailable = 0,
    BVNWineStageIdle = 1,
    BVNWineStageResourcesReady = 2,
    BVNWineStagePrefixReady = 3,
    BVNWineStageServerStarted = 4,
    BVNWineStageProcessStarted = 5,
    BVNWineStageExited = 6,
    BVNWineStageFailed = 7,
} BVNWineStage;

typedef enum {
    BVNWineTargetNative = 0,
    BVNWineTargetX64 = 1,
    BVNWineTargetDXMT = 2,
    BVNWineTargetDesktop = 3,
    /// An executable the user supplied, rather than one shipped in the IPA.
    BVNWineTargetInstalled = 4,
} BVNWineTarget;

/// True only in builds that link the native Wine archives and bundle their
/// runtime inputs.
bool BVNWineAvailable(void);

/// True when this build can also run 32-bit x86 programs: the i386 Wine PE
/// side is bundled and the WoW64 CPU backend is staged beside the 64-bit one.
/// Independent of BVNWineAvailable - a build can have a complete x86-64 stack
/// and no 32-bit side, which is what every build before this one was.
bool BVNWineThirtyTwoBitAvailable(void);

/// Selects the bundled acceptance executable for the next bootstrap. The
/// selection is locked once Wine starts.
bool BVNWineSetTarget(BVNWineTarget target);
BVNWineTarget BVNWineSelectedTarget(void);
const char* BVNWineTargetName(BVNWineTarget target);

/// Publishes whatever the user has copied into Documents/games so that Wine
/// can see it, then lists the executables inside. Safe to call before the
/// prefix exists and before Wine starts; returns the number found.
///
/// The drop directory is separate from the prefix on purpose - the prefix is
/// disposable and gets replaced when the template changes, and nothing the
/// user copied in should go with it. Each entry is published as a link under
/// drive C, so an executable is addressed the ordinary Windows way and its
/// own directory is what it runs from.
int BVNWineScanInstalled(void);
int BVNWineInstalledCount(void);
/// Path below the publishing directory, e.g. "Some Game\\game_dx11.exe".
/// NULL if the index is out of range.
const char* BVNWineInstalledName(int index);
/// Selects which discovered executable BVNWineTargetInstalled runs.
bool BVNWineSelectInstalled(int index);
int BVNWineSelectedInstalled(void);
/// Absolute filesystem path of the drop directory, for the on-screen hint.
const char* BVNWineInstallRoot(void);

/// Starts the embedded wineserver and the selected PE acceptance program.
/// Returns after the worker threads have been created; progress is reported by
/// BVNWineStageReached and BVNWineReport.
bool BVNWineStart(void);

BVNWineStage BVNWineStageReached(void);
const char* BVNWineStageName(BVNWineStage stage);
const char* BVNWineReport(void);
/// Stable Documents path used by both Wine halves and stderr. The file remains
/// available after a crash and can be exported on the next app launch.
const char* BVNWineLogPath(void);
/// Up to the most recent 256 KiB from BVNWineLogPath.
const char* BVNWinePersistentLog(void);

#ifdef __cplusplus
}
#endif

#endif /* BVN_WINE_BRIDGE_H */
