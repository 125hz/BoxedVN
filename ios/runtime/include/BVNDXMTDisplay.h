/*
 * BoxedVN - DXMT presentation surface owned by the BoxedWine frontend.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BVN_DXMT_DISPLAY_H
#define BVN_DXMT_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Creates the CAMetalLayer-backed view before the x86-64 guest starts. The
 * SDL guest window does not exist yet at that point, so attachment happens
 * separately when BoxedWine publishes its UIWindow.
 */
bool BVNDXMTDisplayPrepare(uint32_t width, uint32_t height);

/* Reparents the prepared view into the current BoxedWine guest window. */
void BVNDXMTDisplayAttach(void);

/* Removes the view and clears every pointer DXMT could still resolve. */
void BVNDXMTDisplayFinish(void);

/* True only while a presentation layer is registered for DXMT. */
bool BVNDXMTDisplayHasLayer(void);

// Called from the DXMT dispatcher on any thread when the guest presents a
// drawable. Records that the layer carries frames; nothing else.
void BVNDXMTDisplayNotePresented(void);

// Main thread, polled from Boxedwine's own loop. Once a frame has been
// presented, brings the DXMT view above SDL's views in the guest window.
// SDL creates its renderer view after the DXMT view was attached, and a
// device run drew 240 frames into a layer nobody could see because that
// later view covered it. The overlay is a subview of the window itself, so
// it stays above both.
void BVNDXMTDisplaySyncOrdering(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BVN_DXMT_DISPLAY_H
