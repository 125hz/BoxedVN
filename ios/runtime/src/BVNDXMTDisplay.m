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
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    // The guest swapchain owns the pixel extent. UIKit may resize the view for
    // rotation or safe-area changes, but must not silently resize its drawable.
    self.metalLayer.contentsScale = 1.0;
    self.metalLayer.drawableSize = self.guestDrawableSize;
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

    UIView* container = BVNGuestUIWindow().rootViewController.view;
    if (container == nil) {
        return;
    }
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
// main thread by the ordering poll below.
static _Atomic(bool) gDisplayPresented = false;
static bool gDisplayFronted = false;

void BVNDXMTDisplayNotePresented(void) {
    atomic_store_explicit(&gDisplayPresented, true, memory_order_release);
}

void BVNDXMTDisplaySyncOrdering(void) {
    if (!NSThread.isMainThread) {
        return;
    }
    if (!atomic_load_explicit(&gDisplayPresented, memory_order_acquire)) {
        return;
    }
    pthread_mutex_lock(&gDisplayLock);
    BVNDXMTMetalView* view = gDisplayView;
    pthread_mutex_unlock(&gDisplayLock);
    UIWindow* window = BVNGuestUIWindow();
    if (view == nil || window == nil) {
        return;
    }
    // SDL 2.32 (SDL_uikitview setSDLWindow:) replaces the root view
    // controller's view with each renderer view it creates and removes the
    // previous one from the window. A DXMT view attached under that earlier
    // view left the window with it: a device run presented 240 frames into a
    // layer that was "frontmost" in a detached container. The view therefore
    // lives directly in the window, immediately above whatever view SDL owns
    // as the root view controller's view, and below the overlay that
    // BVNGuestOverlayInstall keeps at the front.
    UIView* root = window.rootViewController.view;
    if (root == nil || root.superview != window) {
        return;
    }
    BOOL changed = NO;
    if (view.superview != window) {
        [view removeFromSuperview];
        view.frame = window.bounds;
        [window addSubview:view];
        changed = YES;
    }
    NSArray<UIView*>* subviews = window.subviews;
    const NSUInteger rootIndex = [subviews indexOfObjectIdenticalTo:root];
    const NSUInteger viewIndex = [subviews indexOfObjectIdenticalTo:view];
    if (rootIndex == NSNotFound || viewIndex == NSNotFound) {
        return;
    }
    if (viewIndex != rootIndex + 1) {
        [window insertSubview:view aboveSubview:root];
        changed = YES;
    }
    if (!CGRectEqualToRect(view.frame, window.bounds)) {
        view.frame = window.bounds;
    }
    if (changed && !gDisplayFronted) {
        gDisplayFronted = true;
        const CGSize drawable = view.metalLayer.drawableSize;
        char message[256];
        snprintf(message, sizeof(message),
                 "BOXEDVN_DXMT_LAYER_FRONT: placed the DXMT layer in the guest "
                 "window above SDL's root view (%s) after the first presented "
                 "frame; frame=%.0fx%.0f drawable=%.0fx%.0f window_subviews=%lu",
                 NSStringFromClass(root.class).UTF8String,
                 view.bounds.size.width, view.bounds.size.height,
                 drawable.width, drawable.height,
                 (unsigned long)window.subviews.count);
        BVNLogWrite(BVNLogLevelInfo, "dxmt", message);
    }
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
