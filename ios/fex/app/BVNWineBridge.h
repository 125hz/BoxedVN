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
} BVNWineTarget;

/// True only in builds that link the native Wine archives and bundle their
/// runtime inputs.
bool BVNWineAvailable(void);

/// Selects the bundled acceptance executable for the next bootstrap. The
/// selection is locked once Wine starts.
bool BVNWineSetTarget(BVNWineTarget target);
BVNWineTarget BVNWineSelectedTarget(void);
const char* BVNWineTargetName(BVNWineTarget target);

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
