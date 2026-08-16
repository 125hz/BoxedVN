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

/// True only in builds that link the native Wine archives and bundle their
/// runtime inputs.
bool BVNWineAvailable(void);

/// Starts the embedded wineserver and a headless Wine prefix initialization.
/// Returns after the worker threads have been created; progress is reported by
/// BVNWineStageReached and BVNWineReport.
bool BVNWineStart(void);

BVNWineStage BVNWineStageReached(void);
const char* BVNWineStageName(BVNWineStage stage);
const char* BVNWineReport(void);

#ifdef __cplusplus
}
#endif

#endif /* BVN_WINE_BRIDGE_H */
