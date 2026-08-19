// IOSDisplayShim.m - the macdrv_* surface DXMT expects, backed by a UIKit layer.
//
// Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
//
// Ported from the integration's shim, minus its desktop compositor. That branch
// resolves a per-window layer through winios_metal_layer_for_hwnd, which lives
// in a Wine driver this application does not have; here there is one screen and
// one layer, which is all a full-screen D3D11 title needs.
//
// DXMT reaches these through dlsym(RTLD_DEFAULT, "macdrv_functions"), so they
// have to survive the link as exported symbols in the main binary. See the note
// on `used` below - it is the difference between a picture and a white screen.

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <pthread.h>

#include "IOSDisplayShim.h"

// Mirrors what DXMT's winemetal_unix.c expects to find. Only the fields it
// actually dereferences are meaningful; the rest exist to keep the layout.
typedef struct macdrv_opaque_metal_device *macdrv_metal_device;
typedef struct macdrv_opaque_metal_view *macdrv_metal_view;
typedef struct macdrv_opaque_metal_layer *macdrv_metal_layer;
typedef struct macdrv_opaque_view *macdrv_view;
typedef struct macdrv_opaque_window *macdrv_window;
typedef struct opaque_HWND *HWND;

struct macdrv_win_data {
    HWND hwnd;
    macdrv_window cocoa_window;
    macdrv_view cocoa_view;
    macdrv_view client_cocoa_view;
};

struct macdrv_functions_t {
    void (*macdrv_init_display_devices)(BOOL);
    struct macdrv_win_data *(*get_win_data)(HWND hwnd);
    void (*release_win_data)(struct macdrv_win_data *data);
    macdrv_window (*macdrv_get_cocoa_window)(HWND hwnd, BOOL require_on_screen);
    macdrv_metal_device (*macdrv_create_metal_device)(void);
    void (*macdrv_release_metal_device)(macdrv_metal_device d);
    macdrv_metal_view (*macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d);
    macdrv_metal_layer (*macdrv_view_get_metal_layer)(macdrv_metal_view v);
    void (*macdrv_view_release_metal_view)(macdrv_metal_view v);
    void (*on_main_thread)(dispatch_block_t b);
};

static CAMetalLayer *g_layer = nil;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void bvn_display_set_layer(CAMetalLayer *layer) {
    pthread_mutex_lock(&g_lock);
    g_layer = layer;
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[bvn-display] layer registered: %p\n", (__bridge void *)layer);
    fflush(stderr);
}

bool bvn_display_has_layer(void) {
    pthread_mutex_lock(&g_lock);
    bool present = g_layer != nil;
    pthread_mutex_unlock(&g_lock);
    return present;
}

// DXMT only dereferences client_cocoa_view, handing it straight back to
// view_create_metal_view, so that field carries the HWND through. One static
// struct is enough: the get/create/release sequence is not concurrent and the
// value is consumed before release.
static struct macdrv_win_data g_win_data = {
    .hwnd = NULL,
    .cocoa_window = NULL,
    .cocoa_view = (macdrv_view)(uintptr_t)0x1,
    .client_cocoa_view = (macdrv_view)(uintptr_t)0x1,
};

static struct macdrv_win_data *bvn_get_win_data(HWND hwnd) {
    g_win_data.hwnd = hwnd;
    g_win_data.client_cocoa_view = (macdrv_view)hwnd;
    return &g_win_data;
}

static void bvn_release_win_data(struct macdrv_win_data *data) {
    (void)data;
}

static macdrv_metal_device bvn_create_metal_device(void) {
    // DXMT creates its own MTLDevice on the path this build takes; this exists
    // so the Wine-flavoured entry point does not read as a failure.
    return (macdrv_metal_device)(uintptr_t)0x1;
}

static void bvn_release_metal_device(macdrv_metal_device d) {
    (void)d;
}

static macdrv_metal_view bvn_view_create_metal_view(macdrv_view v, macdrv_metal_device d) {
    (void)v;
    (void)d;
    pthread_mutex_lock(&g_lock);
    CAMetalLayer *layer = g_layer;
    pthread_mutex_unlock(&g_lock);
    if (!layer) {
        fprintf(stderr, "[bvn-display] swapchain asked for a layer before one was registered\n");
        fflush(stderr);
        return NULL;
    }
    return (macdrv_metal_view)CFBridgingRetain(layer);
}

static macdrv_metal_layer bvn_view_get_metal_layer(macdrv_metal_view v) {
    return (macdrv_metal_layer)v;
}

static void bvn_view_release_metal_view(macdrv_metal_view v) {
    if (v) CFBridgingRelease((CFTypeRef)v);
}

static void bvn_on_main_thread(dispatch_block_t b) {
    if ([NSThread isMainThread]) {
        b();
    } else {
        dispatch_async(dispatch_get_main_queue(), b);
    }
}

// `used` is load-bearing rather than decoration. Nothing in this program
// references these by name - DXMT reaches them only through
// dlsym(RTLD_DEFAULT, "macdrv_functions"), which the linker cannot see - so
// under a Release link with -dead_strip they are unreferenced and get removed.
// visibility governs whether a surviving symbol is exported, not whether it
// survives. The integration lost exactly these six symbols that way when its
// host app was rebuilt Release, and the failure surfaced as DXMT aborting with
// "your Wine has no exported symbols needed by DXMT" and a white screen, which
// is a long way from the cause. The app build verifies the export by content
// afterwards rather than trusting this attribute.
__attribute__((used, visibility("default")))
struct macdrv_functions_t macdrv_functions = {
    .macdrv_init_display_devices = NULL,
    .get_win_data = bvn_get_win_data,
    .release_win_data = bvn_release_win_data,
    .macdrv_get_cocoa_window = NULL,
    .macdrv_create_metal_device = bvn_create_metal_device,
    .macdrv_release_metal_device = bvn_release_metal_device,
    .macdrv_view_create_metal_view = bvn_view_create_metal_view,
    .macdrv_view_get_metal_layer = bvn_view_get_metal_layer,
    .macdrv_view_release_metal_view = bvn_view_release_metal_view,
    .on_main_thread = bvn_on_main_thread,
};

// DXMT checks for the individual symbols as well as the table.
__attribute__((used, visibility("default")))
struct macdrv_win_data *get_win_data(HWND hwnd) {
    return bvn_get_win_data(hwnd);
}

__attribute__((used, visibility("default")))
void release_win_data(struct macdrv_win_data *data) {
    bvn_release_win_data(data);
}

__attribute__((used, visibility("default")))
macdrv_metal_view macdrv_view_create_metal_view(macdrv_view v, macdrv_metal_device d) {
    return bvn_view_create_metal_view(v, d);
}

__attribute__((used, visibility("default")))
macdrv_metal_layer macdrv_view_get_metal_layer(macdrv_metal_view v) {
    return bvn_view_get_metal_layer(v);
}

__attribute__((used, visibility("default")))
void macdrv_view_release_metal_view(macdrv_metal_view v) {
    bvn_view_release_metal_view(v);
}
