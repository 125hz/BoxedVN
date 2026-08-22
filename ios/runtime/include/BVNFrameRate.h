/*
 * BoxedVN guest presentation frame-rate control.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BVN_FRAME_RATE_H
#define BVN_FRAME_RATE_H

#ifdef __cplusplus
extern "C" {
#endif

// 0 = uncapped/adaptive, 1 = 60 FPS, 2 = 120 FPS. The system may still
// impose a lower display cadence for hardware, power or thermal reasons.
int BVNGuestFrameRateMode(void);
void BVNGuestSetFrameRateMode(int mode);

// Called immediately before a guest GPU present. This is a no-op in uncapped
// mode and otherwise paces all supported graphics backends through one clock.
void BVNGuestFrameLimiterWait(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BVN_FRAME_RATE_H
