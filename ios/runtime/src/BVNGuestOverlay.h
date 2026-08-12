/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 *
 *  ---------------------------------------------------------------------
 *  The in-game overlay: a floating menu button, on-screen keyboard, pointer
 *  controls, performance display and quit control behind it.
 *
 *  This is the interface BVNGuestOverlay.mm needs from the rest of the
 *  runtime, and the interface the rest of the runtime needs from it.  It is
 *  private to ios/runtime/src; nothing in ios/runtime/include exposes it,
 *  because the Swift frontend has no business driving in-game controls.
 *
 *  Threading, in one line: every function here must be called on the main
 *  thread, and during a guest session "on the main thread" means from inside
 *  a DISPATCH_MAIN_THREAD_BLOCK or a UIKit callback - NOT from a block
 *  submitted to the main dispatch queue, which is not drained while boxedmain
 *  owns the main thread.
 *  ---------------------------------------------------------------------
 */

#ifndef BVN_GUEST_OVERLAY_H
#define BVN_GUEST_OVERLAY_H

#import <UIKit/UIKit.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Implemented by BVNGuestOverlay.mm
// ---------------------------------------------------------------------------

// Attaches the overlay to SDL's guest window, or moves it to the current one
// if SDL has recreated its window.  Idempotent.
void BVNGuestOverlayInstall(void);

// Detaches the overlay and releases any keys it was holding down.
void BVNGuestOverlayRemove(void);

// The Wine startup notice: proper UIKit text over SDL's backdrop, replacing the
// bitmap font that could not be read at a useful size.
void BVNGuestStartupNoticeSetVisible(bool visible);
void BVNGuestStartupNoticeSetProgress(size_t jitBlocks);

// Applies state requested off the main thread and keeps the overlay above
// SDL's views.  Cheap; called from the emulator's own event loop.
void BVNGuestOverlayApplyPendingState(void);

// Cancels UIKit control/gesture tracking after a live scene-geometry change.
// Rotation can cancel a touch without delivering the final callback to the
// view that owned it; carrying that stale tracking state forward makes every
// later touch appear dead until process restart.
void BVNGuestOverlayGeometryDidChange(void);

// Records one visible guest update (Vulkan frame or X11/GDI patch) for the
// optional FPS and frame-time overlay. Calls from different render paths are
// coalesced at display cadence. Safe from either presentation thread.
void BVNGuestPerformanceFramePresented(void);

// ---------------------------------------------------------------------------
// Implemented by BVNAppDelegate.mm
// ---------------------------------------------------------------------------

// SDL's own UIWindow for the running guest, bypassing the library-window
// fallback in -[BVNAppDelegate window].  nil when no session is running.
UIWindow* BVNGuestUIWindow(void);

// Re-fits the guest picture when the window has changed shape since the last
// fit, and reports whether it did.  Cheap when nothing has moved.  This is the
// reliable path: it is polled from Boxedwine's own main loop rather than
// depending on a UIKit layout callback, which on build 65 stopped arriving
// after the first rotation.
bool BVNSyncGuestPresentationGeometry(void);

// ---------------------------------------------------------------------------
// Implemented by platform/sdl/knativescreenSDL.cpp
//
// The overlay must not include SDL or Boxedwine headers, and the scancode
// numbers must not be duplicated on the UIKit side where they could silently
// drift from SDL's.  So keys are named the way SDL names them
// (SDL_scancode_names in SDL_keyboard.c: "Escape", "Left Ctrl", "PageUp", …)
// and resolved through SDL here.
// ---------------------------------------------------------------------------

// SDL_SCANCODE_UNKNOWN (0) for a name SDL does not know.  The overlay logs
// those once at install time rather than shipping a dead key.
uint32_t BVNGuestControlsScancodeForName(const char* name);

void BVNGuestControlsSendKey(uint32_t scancode, bool down);

// Guest pointer input, in the presenting view's bounds - which since build 65
// are the guest resolution.  phase: 0 = move, 1 = press, 2 = release.
//
// The overlay owns this instead of letting SDL's view receive the touch.
// Depending on UIKit to deliver a touch to a transformed view inside a
// hierarchy UIKit itself owns broke in three different ways across builds 62,
// 64 and 66; -[UITouch locationInView:] resolves the transform for us and
// nothing about the intervening hierarchy can matter.
void BVNGuestControlsSendPointer(int x, int y, int phase);

// Returns the current emulated screen size for software-rendered sessions.
// Vulkan sessions normally use BVNGuestPresentationView's guest-sized bounds.
bool BVNGuestControlsScreenSize(int* width, int* height);

// SDL_RenderSetLogicalSize owns the Wine desktop's viewport. These helpers
// apply that exact renderer transform for the UIKit overlay, avoiding a
// separate whole-window approximation near the right edge.
bool BVNGuestControlsMapSoftwarePoint(float windowX, float windowY,
                                      float* guestX, float* guestY);
bool BVNGuestControlsMapSoftwarePointToWindow(float guestX, float guestY,
                                              float* windowX, float* windowY);

// A right click at a point in the same space.  Used by the two-finger tap.
void BVNGuestControlsSendRightClick(int x, int y);

// The Metal view the guest is presenting through, so the overlay can convert a
// touch into its coordinate space.  nil when no guest surface exists.
UIView* BVNGuestPresentationView(void);

bool BVNGuestPresentationIsStretched(void);
void BVNGuestSetPresentationStretched(bool stretched);

// Re-derives the window-to-guest pointer transform from the rectangle the
// presenter measured.  Presentation and input are never derived independently:
// build 62 broke every tap by assuming a letterbox that had not happened.
void BVNGuestPresentationGeometryChanged(void);

// ---------------------------------------------------------------------------
// Implemented by BVNRuntime.mm (declared in BVNRuntime.h, repeated here so the
// overlay does not have to pull in the whole frontend ABI).
// ---------------------------------------------------------------------------

bool BVNRuntimeRequestShutdown(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BVN_GUEST_OVERLAY_H
