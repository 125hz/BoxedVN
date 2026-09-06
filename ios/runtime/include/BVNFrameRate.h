/*
 * BoxedVN guest presentation frame-rate control.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BVN_FRAME_RATE_H
#define BVN_FRAME_RATE_H

#ifdef __cplusplus
extern "C" {
#endif

// 0 = uncapped/adaptive, 1 = 60 FPS, 2 = 120 FPS, 3 = 30 FPS. The system may still
// impose a lower display cadence for hardware, power or thermal reasons.
int BVNGuestFrameRateMode(void);
void BVNGuestSetFrameRateMode(int mode);

// DXMT supplies its own 60 Hz pacing; this adds its requested 30 Hz cap.
void BVNGuestFrameLimiterWait(void);
// Vulkan needs software pacing at every selected cap, independently of DXMT.
void BVNGuestVulkanFrameLimiterWait(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BVN_FRAME_RATE_H
