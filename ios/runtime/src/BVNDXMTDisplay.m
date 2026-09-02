/*
 * BoxedVN - DXMT macdrv surface bridge backed by a UIKit CAMetalLayer.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * DXMT's Wine-side Metal bridge resolves macdrv_functions with
 * dlsym(RTLD_DEFAULT, ...). BoxedWine owns the only visible iOS window, so the
 * table below deliberately maps every guest HWND to one full-screen layer in
 * that window. It creates no second Wine runtime and no native guest window.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "BVNDXMTDisplay.h"
#include "BVNGuestOverlay.h"
#include "BVNRuntime.h"

#if defined(BOXEDWINE_DXMT_NATIVE)
// The pinned iOS DXMT cadence logger samples counters owned by its native-Wine
// exception and server transports. BoxedWine supplies neither transport, so
// expose weak zero counters for that diagnostic-only ABI. A future transport
// can provide strong definitions without changing the DXMT archive.
volatile int ios_exc_msg_count __attribute__((weak)) = 0;
volatile int ios_srv_wait_count __attribute__((weak)) = 0;
volatile int ios_srv_wait_timeouts __attribute__((weak)) = 0;
volatile int ios_srv_req_count __attribute__((weak)) = 0;
volatile long long ios_srv_wait_us __attribute__((weak)) = 0;
volatile long long ios_srv_wait_req_us __attribute__((weak)) = 0;
#endif

extern UIWindow* BVNGuestUIWindow(void);

@interface BVNDXMTMetalView : UIView
@property(nonatomic, assign) CGSize guestDrawableSize;
@property(nonatomic, readonly) CAMetalLayer* metalLayer;
@end

@implementation BVNDXMTMetalView

+ (Class)layerClass {
    return CAMetalLayer.class;
}

- (CAMetalLayer*)metalLayer {
    return (CAMetalLayer*)self.layer;
}

- (instancetype)initWithGuestWidth:(uint32_t)width height:(uint32_t)height {
    self = [super initWithFrame:CGRectZero];
    if (self == nil) {
        return nil;
    }

    _guestDrawableSize = CGSizeMake(MAX(1u, width), MAX(1u, height));
    self.backgroundColor = UIColor.blackColor;
    self.userInteractionEnabled = NO;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth |
                            UIViewAutoresizingFlexibleHeight;

    CAMetalLayer* layer = self.metalLayer;
    layer.device = MTLCreateSystemDefaultDevice();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.opaque = YES;
    layer.backgroundColor = UIColor.blackColor.CGColor;
    layer.contentsScale = 1.0;
    layer.drawableSize = _guestDrawableSize;
    layer.maximumDrawableCount = 3;
    layer.presentsWithTransaction = NO;
    // The swapchain's pixel extent rarely matches the view. Letterbox it
    // rather than stretch it: a 640x480 frame filled a 402x874 window on
    // device and the picture was unrecognisable.
    layer.contentsGravity = kCAGravityResizeAspect;
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    // Deliberately leaves the layer's contentsScale and drawableSize alone.
    // DXMT sets both from its swapchain (WMTLayerProps in winemetal_unix.c)
    // and expects them to stay; resetting them here to the guest screen size
    // put a 640x480 backbuffer in the top-left 80% of an 800x600 drawable on
    // device. The pixel extent belongs to the swapchain, the frame to UIKit.
}

@end

typedef struct macdrv_opaque_metal_device* macdrv_metal_device;
typedef struct macdrv_opaque_metal_view* macdrv_metal_view;
typedef struct macdrv_opaque_metal_layer* macdrv_metal_layer;
typedef struct macdrv_opaque_view* macdrv_view;
typedef struct macdrv_opaque_window* macdrv_window;
typedef struct opaque_HWND* HWND;

struct macdrv_win_data {
    HWND hwnd;
    macdrv_window cocoa_window;
    macdrv_view cocoa_view;
    macdrv_view client_cocoa_view;
};

struct macdrv_functions_t {
    void (*macdrv_init_display_devices)(BOOL);
    struct macdrv_win_data* (*get_win_data)(HWND hwnd);
    void (*release_win_data)(struct macdrv_win_data* data);
    macdrv_window (*macdrv_get_cocoa_window)(HWND hwnd, BOOL require_on_screen);
    macdrv_metal_device (*macdrv_create_metal_device)(void);
    void (*macdrv_release_metal_device)(macdrv_metal_device device);
    macdrv_metal_view (*macdrv_view_create_metal_view)(macdrv_view view,
                                                       macdrv_metal_device device);
    macdrv_metal_layer (*macdrv_view_get_metal_layer)(macdrv_metal_view view);
    void (*macdrv_view_release_metal_view)(macdrv_metal_view view);
    void (*on_main_thread)(dispatch_block_t block);
};

static pthread_mutex_t gDisplayLock = PTHREAD_MUTEX_INITIALIZER;
static BVNDXMTMetalView* gDisplayView = nil;
static CAMetalLayer* gDisplayLayer = nil;
// The window the view was attached to; SDL's delegate can stop reporting a
// window while the guest still presents into this one.
static __weak UIWindow* gDisplayWindow = nil;

static CAMetalLayer* BVNDXMTRegisteredLayer(void) {
    pthread_mutex_lock(&gDisplayLock);
    CAMetalLayer* layer = gDisplayLayer;
    pthread_mutex_unlock(&gDisplayLock);
    return layer;
}

bool BVNDXMTDisplayPrepare(uint32_t width, uint32_t height) {
    NSCAssert(NSThread.isMainThread,
              @"DXMT presentation must be prepared on the main thread");
    BVNDXMTDisplayFinish();

    BVNDXMTMetalView* view = [[BVNDXMTMetalView alloc]
        initWithGuestWidth:MAX(1u, width)
                    height:MAX(1u, height)];
    if (view == nil || view.metalLayer.device == nil) {
        BVNLogWrite(BVNLogLevelError, "dxmt",
                    "Could not create the BoxedWine Metal presentation layer.");
        return false;
    }

    pthread_mutex_lock(&gDisplayLock);
    gDisplayView = view;
    gDisplayLayer = view.metalLayer;
    pthread_mutex_unlock(&gDisplayLock);

    BVNLogWrite(BVNLogLevelInfo, "dxmt",
                "Prepared the BoxedWine-owned DXMT presentation layer.");
    BVNDXMTDisplayAttach();
    return true;
}

void BVNDXMTDisplayAttach(void) {
    NSCAssert(NSThread.isMainThread,
              @"DXMT presentation must be attached on the main thread");

    pthread_mutex_lock(&gDisplayLock);
    BVNDXMTMetalView* view = gDisplayView;
    pthread_mutex_unlock(&gDisplayLock);
    if (view == nil) {
        return;
    }

    UIWindow* window = BVNGuestUIWindow();
    UIView* container = window.rootViewController.view;
    if (container == nil) {
        return;
    }
    gDisplayWindow = window;
    if (view.superview != container) {
        [view removeFromSuperview];
        view.frame = container.bounds;
        [container addSubview:view];
        BVNLogWrite(BVNLogLevelInfo, "dxmt",
                    "Attached the DXMT layer to the BoxedWine guest window.");
    } else {
        view.frame = container.bounds;
    }
}

void BVNDXMTDisplayFinish(void) {
    NSCAssert(NSThread.isMainThread,
              @"DXMT presentation must be released on the main thread");

    pthread_mutex_lock(&gDisplayLock);
    BVNDXMTMetalView* view = gDisplayView;
    const BOOL hadLayer = gDisplayLayer != nil;
    gDisplayView = nil;
    gDisplayLayer = nil;
    pthread_mutex_unlock(&gDisplayLock);

    [view removeFromSuperview];
    if (hadLayer) {
        BVNLogWrite(BVNLogLevelInfo, "dxmt",
                    "Released the DXMT presentation layer.");
    }
}

bool BVNDXMTDisplayHasLayer(void) {
    return BVNDXMTRegisteredLayer() != nil;
}

// Set from the DXMT dispatcher's thread on the first present; consumed on the
// main thread by the placement below.
static _Atomic(bool) gDisplayPresented = false;
static _Atomic(bool) gDisplayPlacementQueued = false;
static _Atomic(uint32_t) gDisplayPresenterPid = 0;
static bool gDisplayFronted = false;
static bool gDisplayPollReported = false;
static bool gDisplayBlockedReported = false;

static void BVNDXMTDisplayPlaceOnMain(void);

static void BVNDXMTDisplayQueuePlacement(void) {
    if (atomic_exchange_explicit(&gDisplayPlacementQueued, true,
                                 memory_order_acq_rel)) {
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        BVNDXMTDisplayPlaceOnMain();
    });
}

void BVNDXMTDisplayNotePresented(uint32_t pid) {
    // The performance overlay counts presented frames from the Vulkan and
    // X11 paths; a DXMT present is a visible frame too, or the overlay reads
    // 0 fps while the cube spins.
    BVNGuestPerformanceFramePresented();
    atomic_store_explicit(&gDisplayPresenterPid, pid, memory_order_release);
    if (atomic_exchange_explicit(&gDisplayPresented, true,
                                 memory_order_acq_rel)) {
        return;
    }
    // Do not rely on the emulator loop alone to notice the first frame: place
    // the view from the main queue as well. SDL's event pump services the run
    // loop, so this runs even if the poll never reaches the placement.
    BVNDXMTDisplayQueuePlacement();
}

void BVNDXMTDisplayNoteProcessExited(uint32_t pid) {
    if (pid == 0 ||
        atomic_load_explicit(&gDisplayPresenterPid, memory_order_acquire) !=
            pid) {
        return;
    }
    // The presenting process is gone: nothing will draw into the layer again
    // until another process presents. Hide it so SDL's view, with the desktop
    // and any dialog Wine raises for the exit, shows through. The next
    // present from another process re-arms placement, which unhides it.
    atomic_store_explicit(&gDisplayPresenterPid, 0, memory_order_release);
    atomic_store_explicit(&gDisplayPresented, false, memory_order_release);
    atomic_store_explicit(&gDisplayPlacementQueued, false,
                          memory_order_release);
    dispatch_async(dispatch_get_main_queue(), ^{
        pthread_mutex_lock(&gDisplayLock);
        BVNDXMTMetalView* view = gDisplayView;
        pthread_mutex_unlock(&gDisplayLock);
        if (view == nil || view.hidden) {
            return;
        }
        view.hidden = YES;
        gDisplayFronted = false;
        char message[128];
        snprintf(message, sizeof(message),
                 "BOXEDVN_DXMT_LAYER_HIDDEN: presenting process %u exited; "
                 "the desktop is visible again",
                 (unsigned)pid);
        BVNLogWrite(BVNLogLevelInfo, "dxmt", message);
    });
}

static const char* BVNDXMTClassName(id object, const char* fallback) {
    return object ? NSStringFromClass([object class]).UTF8String : fallback;
}

static void BVNDXMTDisplayReportBlocked(const char* why, UIView* view,
                                        UIWindow* window, UIView* root) {
    if (gDisplayBlockedReported) {
        return;
    }
    gDisplayBlockedReported = true;
    char message[320];
    snprintf(message, sizeof(message),
             "BOXEDVN_DXMT_LAYER_BLOCKED: %s (window=%s root=%s "
             "root_in_window=%d view_superview=%s view_in_window=%d)",
             why, BVNDXMTClassName(window, "nil"),
             BVNDXMTClassName(root, "nil"),
             root != nil && root.superview == window,
             BVNDXMTClassName(view.superview, "nil"), view.window != nil);
    BVNLogWrite(BVNLogLevelWarning, "dxmt", message);
}

// Where the guest desktop is on screen. SDL letterboxes the emulated screen
// (the container's resolution, 800x600 by default) inside its view; the
// overlay's mapping helpers apply that exact transform. The DXMT layer is
// confined to the same rectangle so the guest's picture sits where its
// desktop does instead of covering the whole window. Falls back to the
// window bounds when the mapping is unavailable.
static CGRect BVNDXMTDesktopFrame(UIWindow* window) {
    int width = 0, height = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (BVNGuestControlsScreenSize(&width, &height) && width > 0 && height > 0 &&
        BVNGuestControlsMapSoftwarePointToWindow(0.0f, 0.0f, &x0, &y0) &&
        BVNGuestControlsMapSoftwarePointToWindow((float)width, (float)height,
                                                 &x1, &y1) &&
        x1 > x0 + 1.0f && y1 > y0 + 1.0f) {
        return CGRectMake(x0, y0, x1 - x0, y1 - y0);
    }
    return window.bounds;
}

static void BVNDXMTDisplayPlaceOnMain(void) {
    pthread_mutex_lock(&gDisplayLock);
    BVNDXMTMetalView* view = gDisplayView;
    pthread_mutex_unlock(&gDisplayLock);
    if (view == nil) {
        return;
    }
    UIWindow* window = BVNGuestUIWindow();
    if (window == nil) {
        window = gDisplayWindow;
    }
    if (window == nil) {
        window = view.window;
    }
    UIView* root = window.rootViewController.view;
    if (!gDisplayPollReported) {
        gDisplayPollReported = true;
        char message[320];
        snprintf(message, sizeof(message),
                 "BOXEDVN_DXMT_LAYER_POLL: first placement pass after a "
                 "presented frame (window=%s root=%s root_in_window=%d "
                 "view_superview=%s view_in_window=%d window_subviews=%lu)",
                 BVNDXMTClassName(window, "nil"),
                 BVNDXMTClassName(root, "nil"),
                 root != nil && root.superview == window,
                 BVNDXMTClassName(view.superview, "nil"),
                 view.window != nil,
                 (unsigned long)window.subviews.count);
        BVNLogWrite(BVNLogLevelInfo, "dxmt", message);
    }
    if (window == nil) {
        BVNDXMTDisplayReportBlocked("no guest window", view, window, root);
        return;
    }
    // SDL 2.32 (SDL_uikitview setSDLWindow:) replaces the root view
    // controller's view with each renderer view it creates and removes the
    // previous one from the window. A DXMT view attached under that earlier
    // view left the window with it: a device run presented 240 frames into a
    // layer that was "frontmost" in a detached container. The view therefore
    // lives directly in the window, above whatever view SDL owns as the root
    // view controller's view, and below the overlay, which
    // BVNGuestOverlayInstall re-fronts.
    const CGRect desktop = BVNDXMTDesktopFrame(window);
    BOOL changed = NO;
    if (view.hidden) {
        view.hidden = NO;
        changed = YES;
    }
    if (view.superview != window) {
        [view removeFromSuperview];
        view.autoresizingMask = UIViewAutoresizingNone;
        view.frame = desktop;
        [window addSubview:view];
        changed = YES;
    }
    NSArray<UIView*>* subviews = window.subviews;
    const NSUInteger viewIndex = [subviews indexOfObjectIdenticalTo:view];
    const NSUInteger rootIndex = (root != nil && root.superview == window)
        ? [subviews indexOfObjectIdenticalTo:root]
        : NSNotFound;
    if (viewIndex == NSNotFound) {
        BVNDXMTDisplayReportBlocked("view not in window after add", view,
                                    window, root);
        return;
    }
    if (rootIndex != NSNotFound) {
        if (viewIndex < rootIndex) {
            [window insertSubview:view aboveSubview:root];
            changed = YES;
        }
    } else if (viewIndex != subviews.count - 1) {
        // No root view to order against: sit on top of SDL's views and let
        // the overlay re-front itself below.
        [window bringSubviewToFront:view];
        changed = YES;
    }
    if (!CGRectEqualToRect(view.frame, desktop)) {
        // Rotation or a container-resolution change moved the desktop.
        view.frame = desktop;
    }
    if (changed) {
        // Keeps the menu button, cursor, and notices above the frames.
        BVNGuestOverlayInstall();
    }
    if (changed && !gDisplayFronted) {
        gDisplayFronted = true;
        gDisplayPollReported = false;
        const CGSize drawable = view.metalLayer.drawableSize;
        char message[320];
        snprintf(message, sizeof(message),
                 "BOXEDVN_DXMT_LAYER_FRONT: placed the DXMT layer in the guest "
                 "window above SDL's root view (%s) after the first presented "
                 "frame; frame=%.0fx%.0f@%.0f,%.0f window=%.0fx%.0f "
                 "drawable=%.0fx%.0f index=%lu/%lu hidden=%d alpha=%.2f",
                 BVNDXMTClassName(root, "none"),
                 view.frame.size.width, view.frame.size.height,
                 view.frame.origin.x, view.frame.origin.y,
                 window.bounds.size.width, window.bounds.size.height,
                 drawable.width, drawable.height,
                 (unsigned long)[window.subviews indexOfObjectIdenticalTo:view],
                 (unsigned long)window.subviews.count,
                 view.hidden || window.hidden, (double)window.alpha);
        BVNLogWrite(BVNLogLevelInfo, "dxmt", message);
    }
}

void BVNDXMTDisplaySyncOrdering(void) {
    if (!atomic_load_explicit(&gDisplayPresented, memory_order_acquire)) {
        return;
    }
    if (!NSThread.isMainThread) {
        // The emulator loop is expected on the main thread; if it is not,
        // say so once and still place the view from the main queue.
        if (!gDisplayBlockedReported) {
            gDisplayBlockedReported = true;
            BVNLogWrite(BVNLogLevelWarning, "dxmt",
                        "BOXEDVN_DXMT_LAYER_BLOCKED: ordering poll ran off "
                        "the main thread; placing from the main queue.");
        }
        BVNDXMTDisplayQueuePlacement();
        return;
    }
    BVNDXMTDisplayPlaceOnMain();
}

static struct macdrv_win_data* BVNDXMTGetWinData(HWND hwnd) {
    static unsigned callCount = 0;
    if (__atomic_fetch_add(&callCount, 1, __ATOMIC_RELAXED) < 8) {
        BVNLogWrite(BVNLogLevelInfo, "dxmt",
                    "DXMT requested BoxedWine window data.");
    }
    struct macdrv_win_data* data = calloc(1, sizeof(*data));
    if (data != NULL) {
        data->hwnd = hwnd;
        data->cocoa_view = (macdrv_view)hwnd;
        data->client_cocoa_view = (macdrv_view)hwnd;
    }
    return data;
}

static void BVNDXMTReleaseWinData(struct macdrv_win_data* data) {
    free(data);
}

static macdrv_metal_device BVNDXMTCreateMetalDevice(void) {
    BVNLogWrite(BVNLogLevelInfo, "dxmt",
                "DXMT requested the BoxedWine Metal device token.");
    // DXMT creates the actual MTLDevice. A non-null token keeps its Wine-facing
    // macdrv contract intact without claiming ownership of that object here.
    return (macdrv_metal_device)(uintptr_t)1;
}

static void BVNDXMTReleaseMetalDevice(macdrv_metal_device device) {
    (void)device;
}

static macdrv_metal_view BVNDXMTCreateMetalView(macdrv_view view,
                                                macdrv_metal_device device) {
    (void)view;
    (void)device;
    CAMetalLayer* layer = BVNDXMTRegisteredLayer();
    if (layer == nil) {
        BVNLogWrite(BVNLogLevelError, "dxmt",
                    "DXMT requested a swapchain before its layer was prepared.");
        return NULL;
    }
    BVNLogWrite(BVNLogLevelInfo, "dxmt",
                "DXMT acquired the BoxedWine Metal presentation view.");
    return (macdrv_metal_view)CFBridgingRetain(layer);
}

static macdrv_metal_layer BVNDXMTGetMetalLayer(macdrv_metal_view view) {
    static unsigned callCount = 0;
    if (__atomic_fetch_add(&callCount, 1, __ATOMIC_RELAXED) < 8) {
        BVNLogWrite(BVNLogLevelInfo, "dxmt",
                    "DXMT acquired the BoxedWine CAMetalLayer.");
    }
    return (macdrv_metal_layer)view;
}

static void BVNDXMTReleaseMetalView(macdrv_metal_view view) {
    if (view != NULL) {
        CFBridgingRelease((CFTypeRef)view);
    }
}

static void BVNDXMTRunOnMainThread(dispatch_block_t block) {
    if (block == nil) {
        return;
    }
    if (NSThread.isMainThread) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

/*
 * These exports are found only by dlsym, so `used` prevents Release dead-strip
 * from discarding them and default visibility puts them in the app export
 * trie. The iOS packaging step verifies the final binary rather than trusting
 * source attributes alone.
 */
__attribute__((used, visibility("default")))
struct macdrv_functions_t macdrv_functions = {
    .macdrv_init_display_devices = NULL,
    .get_win_data = BVNDXMTGetWinData,
    .release_win_data = BVNDXMTReleaseWinData,
    .macdrv_get_cocoa_window = NULL,
    .macdrv_create_metal_device = BVNDXMTCreateMetalDevice,
    .macdrv_release_metal_device = BVNDXMTReleaseMetalDevice,
    .macdrv_view_create_metal_view = BVNDXMTCreateMetalView,
    .macdrv_view_get_metal_layer = BVNDXMTGetMetalLayer,
    .macdrv_view_release_metal_view = BVNDXMTReleaseMetalView,
    .on_main_thread = BVNDXMTRunOnMainThread,
};

__attribute__((used, visibility("default")))
struct macdrv_win_data* get_win_data(HWND hwnd) {
    return BVNDXMTGetWinData(hwnd);
}

__attribute__((used, visibility("default")))
void release_win_data(struct macdrv_win_data* data) {
    BVNDXMTReleaseWinData(data);
}

__attribute__((used, visibility("default")))
macdrv_metal_view macdrv_view_create_metal_view(macdrv_view view,
                                                macdrv_metal_device device) {
    return BVNDXMTCreateMetalView(view, device);
}

__attribute__((used, visibility("default")))
macdrv_metal_layer macdrv_view_get_metal_layer(macdrv_metal_view view) {
    return BVNDXMTGetMetalLayer(view);
}

__attribute__((used, visibility("default")))
void macdrv_view_release_metal_view(macdrv_metal_view view) {
    BVNDXMTReleaseMetalView(view);
}
