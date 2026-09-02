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
// drawable. Records which process draws into the layer and, on the first
// present, queues the placement below on the main queue.
void BVNDXMTDisplayNotePresented(uint32_t pid);

// Called from the kernel when a guest process exits. If it was the process
// presenting into the layer, the layer is hidden from the main queue so the
// desktop shows through until another process presents.
void BVNDXMTDisplayNoteProcessExited(uint32_t pid);

// Main thread, polled from Boxedwine's own loop. Once a frame has been
// presented, keeps the DXMT view a direct subview of the guest window,
// immediately above the root view controller's view. SDL replaces that root
// view with each renderer view it creates and removes the previous one from
// the window, so a DXMT view parented under it silently leaves the window.
// The overlay is installed as a later window subview, so it stays on top.
void BVNDXMTDisplaySyncOrdering(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BVN_DXMT_DISPLAY_H
