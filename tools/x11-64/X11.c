/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

/*
 * The x86-64 guest libX11.so.6 for BoxedWine.
 *
 * Wine's winex11.so links against libX11.so.6 and libXext.so.6 and reads
 * Display, Screen, Visual and XEvent fields through Xlib's own macros. This
 * library exports the same symbols, keeps every one of those structures in
 * memory it allocates itself, and reaches BoxedWine's X server through the
 * private syscall described in include/boxedwine_x64_x11_bridge.h.
 *
 * What stays in the guest: structure ownership (malloc/free), the client
 * side of Xlib (contexts, quarks, XImage, visual lookup, error handlers, the
 * IM stubs), and every function whose answer is a field of the Display.
 * What crosses to the host: everything that touches a window, a property, a
 * drawable, an atom or the event queue.
 *
 * The layouts this file relies on are asserted against the system headers
 * in layout_check.c, compiled alongside it. The host asserts the same
 * numbers in source/x11/x11layout64.h.
 */
#define XUTIL_DEFINE_FUNCTIONS

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/XKBlib.h>

#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "boxedwine_x64_x11_bridge.h"

#if !defined(__x86_64__)
#error "tools/x11-64 is the x86-64 guest shim"
#endif

#define BW_EXPORT __attribute__((visibility("default")))

/* ---- Bridge helpers ------------------------------------------------------ */

#define A(x) ((uint64_t)(x))
#define P(x) ((uint64_t)(uintptr_t)(x))

static int64_t bw_call(uint64_t op, uint64_t *args, uint64_t count)
{
    return boxedwine_x64_x11_call(op, args, count);
}

#define BW(op, ...) \
    bw_call(BOXEDWINE_X64_X11_OP_##op, (uint64_t[]){ __VA_ARGS__ }, \
            sizeof((uint64_t[]){ __VA_ARGS__ }) / sizeof(uint64_t))

#define BW0(op) bw_call(BOXEDWINE_X64_X11_OP_##op, NULL, 0)

/* A result the host sizes: {buffer, capacity} live in args[slot], args[slot+1].
 * Returns the malloc'd buffer (the caller frees it or hands it to the
 * application, which frees it with XFree) and the final status. */
static void *bw_sized(uint64_t op, uint64_t *args, uint64_t count, unsigned slot,
                      int64_t *status_out)
{
    int64_t status = BOXEDWINE_X64_X11_E_BUFFER;
    void *buffer = NULL;
    int attempt;
    args[slot] = 0;
    args[slot + 1] = 0;
    for (attempt = 0; attempt < 3; attempt++) {
        status = bw_call(op, args, count);
        if (status != BOXEDWINE_X64_X11_E_BUFFER) {
            break;
        }
        size_t needed = (size_t)args[slot + 1];
        free(buffer);
        buffer = calloc(1, needed ? needed : 1);
        if (!buffer) {
            status = BOXEDWINE_X64_X11_E_FAULT;
            break;
        }
        args[slot] = P(buffer);
        args[slot + 1] = needed;
    }
    if (status < 0) {
        free(buffer);
        buffer = NULL;
    }
    *status_out = status;
    return buffer;
}

/* One report per stub per process, then silence. */
#define BW_STUB(name) \
    do { \
        static int reported; \
        if (!reported) { \
            reported = 1; \
            BW(REPORT_UNIMPLEMENTED, P(name)); \
        } \
    } while (0)

static void bw_trace(const char *text)
{
    BW(TRACE, P(text));
}

/* ---- Display --------------------------------------------------------------- */

/* The public part of Display, as Xlib.h lays it out for this ABI. */
#define PRIV(dpy) ((_XPrivDisplay)(dpy))

/* Private tail the shim keeps after the public structure. */
#define BW_DISPLAY_SERVER_FD_OFFSET (BOXEDWINE_X64_X11_DISPLAY_ID_OFFSET + 4)

static int *display_server_fd(Display *dpy)
{
    return (int *)((char *)dpy + BW_DISPLAY_SERVER_FD_OFFSET);
}

BW_EXPORT Status XInitThreads(void)
{
    /* The host records the call as an acceptance marker; the shim is
     * thread-safe regardless of the answer. */
    BW0(INIT_THREADS);
    return 1;
}

BW_EXPORT Display *XOpenDisplay(_Xconst char *display_name)
{
    int fds[2];
    void *arena;
    int64_t result;
    (void)display_name;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        bw_trace("XOpenDisplay: socketpair failed");
        return NULL;
    }
    arena = calloc(1, BOXEDWINE_X64_X11_DISPLAY_ARENA_BYTES);
    if (!arena) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    /* Touch every page so the host sees a mapped, writable arena. */
    memset(arena, 0, BOXEDWINE_X64_X11_DISPLAY_ARENA_BYTES);
    result = BW(OPEN_DISPLAY, P(arena), A(BOXEDWINE_X64_X11_DISPLAY_ARENA_BYTES),
                A(fds[0]), A(fds[1]));
    if (result <= 0) {
        bw_trace("XOpenDisplay: bridge refused the display");
        close(fds[0]);
        close(fds[1]);
        free(arena);
        return NULL;
    }
    *display_server_fd((Display *)arena) = fds[1];
    return (Display *)arena;
}

BW_EXPORT int XCloseDisplay(Display *dpy)
{
    int server_fd;
    if (!dpy) {
        return 0;
    }
    server_fd = *display_server_fd(dpy);
    BW(CLOSE_DISPLAY, P(dpy));
    close(PRIV(dpy)->fd);
    if (server_fd > 0) {
        close(server_fd);
    }
    free(dpy);
    return 0;
}

BW_EXPORT char *XDisplayName(_Xconst char *string)
{
    (void)string;
    return (char *)":0";
}

BW_EXPORT char *XDisplayString(Display *dpy)
{
    return PRIV(dpy)->display_name;
}

BW_EXPORT char *XServerVendor(Display *dpy)
{
    return PRIV(dpy)->vendor;
}

BW_EXPORT int XVendorRelease(Display *dpy)
{
    return PRIV(dpy)->release;
}

BW_EXPORT int XProtocolVersion(Display *dpy)
{
    return PRIV(dpy)->proto_major_version;
}

BW_EXPORT int XProtocolRevision(Display *dpy)
{
    return PRIV(dpy)->proto_minor_version;
}

BW_EXPORT int XConnectionNumber(Display *dpy)
{
    return PRIV(dpy)->fd;
}

BW_EXPORT int XDefaultScreen(Display *dpy)
{
    return PRIV(dpy)->default_screen;
}

BW_EXPORT int XScreenCount(Display *dpy)
{
    return PRIV(dpy)->nscreens;
}

BW_EXPORT Screen *XScreenOfDisplay(Display *dpy, int scr)
{
    return &PRIV(dpy)->screens[scr];
}

BW_EXPORT Screen *XDefaultScreenOfDisplay(Display *dpy)
{
    return XScreenOfDisplay(dpy, XDefaultScreen(dpy));
}

BW_EXPORT Window XRootWindow(Display *dpy, int scr)
{
    return XScreenOfDisplay(dpy, scr)->root;
}

BW_EXPORT Window XDefaultRootWindow(Display *dpy)
{
    return XRootWindow(dpy, XDefaultScreen(dpy));
}

BW_EXPORT Window XRootWindowOfScreen(Screen *screen)
{
    return screen->root;
}

BW_EXPORT Visual *XDefaultVisual(Display *dpy, int scr)
{
    return XScreenOfDisplay(dpy, scr)->root_visual;
}

BW_EXPORT Visual *XDefaultVisualOfScreen(Screen *screen)
{
    return screen->root_visual;
}

BW_EXPORT int XDefaultDepth(Display *dpy, int scr)
{
    return XScreenOfDisplay(dpy, scr)->root_depth;
}

BW_EXPORT int XDefaultDepthOfScreen(Screen *screen)
{
    return screen->root_depth;
}

BW_EXPORT Colormap XDefaultColormap(Display *dpy, int scr)
{
    return XScreenOfDisplay(dpy, scr)->cmap;
}

BW_EXPORT Colormap XDefaultColormapOfScreen(Screen *screen)
{
    return screen->cmap;
}

BW_EXPORT GC XDefaultGC(Display *dpy, int scr)
{
    return XScreenOfDisplay(dpy, scr)->default_gc;
}

BW_EXPORT unsigned long XBlackPixel(Display *dpy, int scr)
{
    return XScreenOfDisplay(dpy, scr)->black_pixel;
}

BW_EXPORT unsigned long XWhitePixel(Display *dpy, int scr)
{
    return XScreenOfDisplay(dpy, scr)->white_pixel;
}

BW_EXPORT int XDisplayWidth(Display *dpy, int scr)
{
    return XScreenOfDisplay(dpy, scr)->width;
}

BW_EXPORT int XDisplayHeight(Display *dpy, int scr)
{
    return XScreenOfDisplay(dpy, scr)->height;
}

BW_EXPORT int XDisplayWidthMM(Display *dpy, int scr)
{
    return XScreenOfDisplay(dpy, scr)->mwidth;
}

BW_EXPORT int XDisplayHeightMM(Display *dpy, int scr)
{
    return XScreenOfDisplay(dpy, scr)->mheight;
}

BW_EXPORT int XWidthOfScreen(Screen *screen)
{
    return screen->width;
}

BW_EXPORT int XHeightOfScreen(Screen *screen)
{
    return screen->height;
}

BW_EXPORT Display *XDisplayOfScreen(Screen *screen)
{
    return screen->display;
}

BW_EXPORT int XBitmapUnit(Display *dpy)
{
    return PRIV(dpy)->bitmap_unit;
}

BW_EXPORT int XBitmapPad(Display *dpy)
{
    return PRIV(dpy)->bitmap_pad;
}

BW_EXPORT int XBitmapBitOrder(Display *dpy)
{
    return PRIV(dpy)->bitmap_bit_order;
}

BW_EXPORT int XImageByteOrder(Display *dpy)
{
    return PRIV(dpy)->byte_order;
}

BW_EXPORT VisualID XVisualIDFromVisual(Visual *visual)
{
    return visual->visualid;
}

BW_EXPORT unsigned long XNextRequest(Display *dpy)
{
    return PRIV(dpy)->request + 1;
}

BW_EXPORT unsigned long XLastKnownRequestProcessed(Display *dpy)
{
    return PRIV(dpy)->last_request_read;
}

BW_EXPORT int XQLength(Display *dpy)
{
    return PRIV(dpy)->qlen;
}

BW_EXPORT long XExtendedMaxRequestSize(Display *dpy)
{
    (void)dpy;
    return 4 * 1024 * 1024;
}

BW_EXPORT long XMaxRequestSize(Display *dpy)
{
    return (long)PRIV(dpy)->max_request_size;
}

BW_EXPORT int XNoOp(Display *dpy)
{
    (void)dpy;
    return 1;
}

BW_EXPORT int XSync(Display *dpy, Bool discard)
{
    (void)discard;
    BW(SYNC, P(dpy));
    return 1;
}

BW_EXPORT int XFlush(Display *dpy)
{
    BW(FLUSH, P(dpy));
    return 1;
}

BW_EXPORT int XGrabServer(Display *dpy)
{
    BW(GRAB_SERVER, P(dpy));
    return 1;
}

BW_EXPORT int XUngrabServer(Display *dpy)
{
    BW(UNGRAB_SERVER, P(dpy));
    return 1;
}

BW_EXPORT void XLockDisplay(Display *dpy)
{
    (void)dpy;
}

BW_EXPORT void XUnlockDisplay(Display *dpy)
{
    (void)dpy;
}

BW_EXPORT int XBell(Display *dpy, int percent)
{
    (void)dpy;
    (void)percent;
    return 1;
}

BW_EXPORT int XGetScreenSaver(Display *dpy, int *timeout_return, int *interval_return,
                              int *prefer_blanking_return, int *allow_exposures_return)
{
    (void)dpy;
    if (timeout_return) *timeout_return = 0;
    if (interval_return) *interval_return = 0;
    if (prefer_blanking_return) *prefer_blanking_return = 0;
    if (allow_exposures_return) *allow_exposures_return = 0;
    return 1;
}

BW_EXPORT int XSetScreenSaver(Display *dpy, int timeout, int interval, int prefer_blanking,
                              int allow_exposures)
{
    (void)dpy; (void)timeout; (void)interval; (void)prefer_blanking; (void)allow_exposures;
    return 1;
}

BW_EXPORT int XResetScreenSaver(Display *dpy)
{
    (void)dpy;
    return 1;
}

BW_EXPORT char *XGetDefault(Display *dpy, _Xconst char *program, _Xconst char *option)
{
    (void)dpy; (void)program; (void)option;
    return NULL;
}

BW_EXPORT int XFree(void *data)
{
    free(data);
    return 1;
}

/* ---- Error handlers --------------------------------------------------------- */

static XErrorHandler bw_error_handler;
static XIOErrorHandler bw_io_error_handler;

BW_EXPORT XErrorHandler XSetErrorHandler(XErrorHandler handler)
{
    XErrorHandler previous = bw_error_handler;
    bw_error_handler = handler;
    return previous;
}

BW_EXPORT XIOErrorHandler XSetIOErrorHandler(XIOErrorHandler handler)
{
    XIOErrorHandler previous = bw_io_error_handler;
    bw_io_error_handler = handler;
    return previous;
}

BW_EXPORT int (*XSynchronize(Display *dpy, Bool onoff))(Display *)
{
    (void)dpy;
    (void)onoff;
    return NULL;
}

BW_EXPORT int XGetErrorText(Display *dpy, int code, char *buffer_return, int length)
{
    (void)dpy;
    if (buffer_return && length > 0) {
        snprintf(buffer_return, (size_t)length, "X error %d", code);
    }
    return 0;
}

/* ---- Atoms ------------------------------------------------------------------ */

BW_EXPORT Atom XInternAtom(Display *dpy, _Xconst char *atom_name, Bool only_if_exists)
{
    int64_t result = BW(INTERN_ATOM, P(dpy), P(atom_name), A(only_if_exists));
    return result > 0 ? (Atom)result : None;
}

BW_EXPORT Status XInternAtoms(Display *dpy, char **names, int count, Bool onlyIfExists,
                              Atom *atoms_return)
{
    int64_t result = BW(INTERN_ATOMS, P(dpy), P(names), A(count), A(onlyIfExists),
                        P(atoms_return));
    return result >= 0 ? 1 : 0;
}

BW_EXPORT char *XGetAtomName(Display *dpy, Atom atom)
{
    uint64_t args[4] = { P(dpy), A(atom), 0, 0 };
    int64_t status;
    char *name = (char *)bw_sized(BOXEDWINE_X64_X11_OP_GET_ATOM_NAME, args, 4, 2, &status);
    if (status <= 0) {
        free(name);
        return NULL;
    }
    return name;
}

BW_EXPORT Status XGetAtomNames(Display *dpy, Atom *atoms, int count, char **names_return)
{
    int i;
    Status result = 1;
    for (i = 0; i < count; i++) {
        names_return[i] = XGetAtomName(dpy, atoms[i]);
        if (!names_return[i]) {
            result = 0;
        }
    }
    return result;
}

/* ---- Contexts and quarks (client side in Xlib) ------------------------------ */

BW_EXPORT XrmQuark XrmUniqueQuark(void)
{
    static int next_quark;
    return __atomic_add_fetch(&next_quark, 1, __ATOMIC_SEQ_CST);
}

struct bw_context_entry {
    XID rid;
    XContext context;
    XPointer data;
    struct bw_context_entry *next;
};

#define BW_CONTEXT_BUCKETS 1024
static struct bw_context_entry *bw_context_table[BW_CONTEXT_BUCKETS];
static pthread_mutex_t bw_context_mutex = PTHREAD_MUTEX_INITIALIZER;

static unsigned bw_context_bucket(XID rid, XContext context)
{
    return (unsigned)((rid * 2654435761u) ^ ((unsigned)context * 40503u)) % BW_CONTEXT_BUCKETS;
}

BW_EXPORT int XSaveContext(Display *dpy, XID rid, XContext context, _Xconst char *data)
{
    unsigned bucket = bw_context_bucket(rid, context);
    struct bw_context_entry *entry;
    (void)dpy;
    pthread_mutex_lock(&bw_context_mutex);
    for (entry = bw_context_table[bucket]; entry; entry = entry->next) {
        if (entry->rid == rid && entry->context == context) {
            entry->data = (XPointer)data;
            pthread_mutex_unlock(&bw_context_mutex);
            return 0;
        }
    }
    entry = (struct bw_context_entry *)malloc(sizeof(*entry));
    if (!entry) {
        pthread_mutex_unlock(&bw_context_mutex);
        return XCNOMEM;
    }
    entry->rid = rid;
    entry->context = context;
    entry->data = (XPointer)data;
    entry->next = bw_context_table[bucket];
    bw_context_table[bucket] = entry;
    pthread_mutex_unlock(&bw_context_mutex);
    return 0;
}

BW_EXPORT int XFindContext(Display *dpy, XID rid, XContext context, XPointer *data_return)
{
    unsigned bucket = bw_context_bucket(rid, context);
    struct bw_context_entry *entry;
    (void)dpy;
    pthread_mutex_lock(&bw_context_mutex);
    for (entry = bw_context_table[bucket]; entry; entry = entry->next) {
        if (entry->rid == rid && entry->context == context) {
            *data_return = entry->data;
            pthread_mutex_unlock(&bw_context_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&bw_context_mutex);
    return XCNOENT;
}

BW_EXPORT int XDeleteContext(Display *dpy, XID rid, XContext context)
{
    unsigned bucket = bw_context_bucket(rid, context);
    struct bw_context_entry **link;
    (void)dpy;
    pthread_mutex_lock(&bw_context_mutex);
    for (link = &bw_context_table[bucket]; *link; link = &(*link)->next) {
        if ((*link)->rid == rid && (*link)->context == context) {
            struct bw_context_entry *entry = *link;
            *link = entry->next;
            free(entry);
            pthread_mutex_unlock(&bw_context_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&bw_context_mutex);
    return XCNOENT;
}

/* ---- Visuals and pixmap formats (read from the Display) --------------------- */

static int bw_visual_matches(long mask, XVisualInfo *tmpl, int screen, Depth *depth, Visual *visual)
{
    if ((mask & VisualIDMask) && tmpl->visualid != visual->visualid) return 0;
    if ((mask & VisualScreenMask) && tmpl->screen != screen) return 0;
    if ((mask & VisualDepthMask) && tmpl->depth != depth->depth) return 0;
    if ((mask & VisualClassMask) && tmpl->class != visual->class) return 0;
    if ((mask & VisualRedMaskMask) && tmpl->red_mask != visual->red_mask) return 0;
    if ((mask & VisualGreenMaskMask) && tmpl->green_mask != visual->green_mask) return 0;
    if ((mask & VisualBlueMaskMask) && tmpl->blue_mask != visual->blue_mask) return 0;
    if ((mask & VisualColormapSizeMask) && tmpl->colormap_size != visual->map_entries) return 0;
    if ((mask & VisualBitsPerRGBMask) && tmpl->bits_per_rgb != visual->bits_per_rgb) return 0;
    return 1;
}

static void bw_fill_visual_info(XVisualInfo *info, int screen, Depth *depth, Visual *visual)
{
    info->visual = visual;
    info->visualid = visual->visualid;
    info->screen = screen;
    info->depth = depth->depth;
    info->class = visual->class;
    info->red_mask = visual->red_mask;
    info->green_mask = visual->green_mask;
    info->blue_mask = visual->blue_mask;
    info->colormap_size = visual->map_entries;
    info->bits_per_rgb = visual->bits_per_rgb;
}

BW_EXPORT XVisualInfo *XGetVisualInfo(Display *dpy, long vinfo_mask, XVisualInfo *vinfo_template,
                                      int *nitems_return)
{
    _XPrivDisplay priv = PRIV(dpy);
    int count = 0;
    int pass;
    XVisualInfo *list = NULL;
    for (pass = 0; pass < 2; pass++) {
        int index = 0;
        int s;
        for (s = 0; s < priv->nscreens; s++) {
            Screen *screen = &priv->screens[s];
            int d;
            for (d = 0; d < screen->ndepths; d++) {
                Depth *depth = &screen->depths[d];
                int v;
                for (v = 0; v < depth->nvisuals; v++) {
                    Visual *visual = &depth->visuals[v];
                    if (!bw_visual_matches(vinfo_mask, vinfo_template, s, depth, visual)) {
                        continue;
                    }
                    if (pass == 1) {
                        bw_fill_visual_info(&list[index], s, depth, visual);
                    }
                    index++;
                }
            }
        }
        if (pass == 0) {
            count = index;
            if (!count) {
                *nitems_return = 0;
                return NULL;
            }
            list = (XVisualInfo *)calloc((size_t)count, sizeof(XVisualInfo));
            if (!list) {
                *nitems_return = 0;
                return NULL;
            }
        }
    }
    *nitems_return = count;
    return list;
}

BW_EXPORT Status XMatchVisualInfo(Display *dpy, int screen, int depth, int class,
                                  XVisualInfo *vinfo_return)
{
    _XPrivDisplay priv = PRIV(dpy);
    Screen *scr;
    int d;
    if (screen < 0 || screen >= priv->nscreens) {
        return 0;
    }
    scr = &priv->screens[screen];
    for (d = 0; d < scr->ndepths; d++) {
        Depth *dep = &scr->depths[d];
        int v;
        if (dep->depth != depth) {
            continue;
        }
        for (v = 0; v < dep->nvisuals; v++) {
            if (dep->visuals[v].class == class) {
                bw_fill_visual_info(vinfo_return, screen, dep, &dep->visuals[v]);
                return 1;
            }
        }
    }
    return 0;
}

BW_EXPORT XPixmapFormatValues *XListPixmapFormats(Display *dpy, int *count_return)
{
    _XPrivDisplay priv = PRIV(dpy);
    int n = priv->nformats;
    XPixmapFormatValues *list;
    int i;
    if (n <= 0 || !priv->pixmap_format) {
        *count_return = 0;
        return NULL;
    }
    list = (XPixmapFormatValues *)calloc((size_t)n, sizeof(XPixmapFormatValues));
    if (!list) {
        *count_return = 0;
        return NULL;
    }
    for (i = 0; i < n; i++) {
        list[i].depth = priv->pixmap_format[i].depth;
        list[i].bits_per_pixel = priv->pixmap_format[i].bits_per_pixel;
        list[i].scanline_pad = priv->pixmap_format[i].scanline_pad;
    }
    *count_return = n;
    return list;
}

/* ---- Windows ---------------------------------------------------------------- */

BW_EXPORT Window XCreateWindow(Display *dpy, Window parent, int x, int y, unsigned int width,
                               unsigned int height, unsigned int border_width, int depth,
                               unsigned int class, Visual *visual, unsigned long valuemask,
                               XSetWindowAttributes *attributes)
{
    int64_t result = BW(CREATE_WINDOW, P(dpy), A(parent), A(x), A(y), A(width), A(height),
                        A(border_width), A(depth), A(class), P(visual), A(valuemask),
                        P(attributes));
    /* Small positive results are X error codes from the server; a window
     * id is always larger than the largest error code. */
    return result > BadImplementation ? (Window)result : None;
}

BW_EXPORT Window XCreateSimpleWindow(Display *dpy, Window parent, int x, int y,
                                     unsigned int width, unsigned int height,
                                     unsigned int border_width, unsigned long border,
                                     unsigned long background)
{
    XSetWindowAttributes attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.border_pixel = border;
    attributes.background_pixel = background;
    return XCreateWindow(dpy, parent, x, y, width, height, border_width, CopyFromParent,
                         InputOutput, CopyFromParent, CWBackPixel | CWBorderPixel, &attributes);
}

BW_EXPORT int XDestroyWindow(Display *dpy, Window w)
{
    return (int)BW(DESTROY_WINDOW, P(dpy), A(w));
}

BW_EXPORT int XMapWindow(Display *dpy, Window w)
{
    return (int)BW(MAP_WINDOW, P(dpy), A(w));
}

BW_EXPORT int XMapRaised(Display *dpy, Window w)
{
    return XMapWindow(dpy, w);
}

BW_EXPORT int XUnmapWindow(Display *dpy, Window w)
{
    return (int)BW(UNMAP_WINDOW, P(dpy), A(w));
}

BW_EXPORT int XSelectInput(Display *dpy, Window w, long event_mask)
{
    return (int)BW(SELECT_INPUT, P(dpy), A(w), A(event_mask));
}

BW_EXPORT int XMoveResizeWindow(Display *dpy, Window w, int x, int y, unsigned int width,
                                unsigned int height)
{
    return (int)BW(MOVE_RESIZE_WINDOW, P(dpy), A(w), A(x), A(y), A(width), A(height));
}

BW_EXPORT int XConfigureWindow(Display *dpy, Window w, unsigned int value_mask,
                               XWindowChanges *values)
{
    return (int)BW(CONFIGURE_WINDOW, P(dpy), A(w), A(value_mask), P(values));
}

BW_EXPORT Status XReconfigureWMWindow(Display *dpy, Window w, int screen_number,
                                      unsigned int mask, XWindowChanges *changes)
{
    (void)screen_number;
    return (Status)BW(CONFIGURE_WINDOW, P(dpy), A(w), A(mask), P(changes)) == Success ? 1 : 0;
}

BW_EXPORT int XMoveWindow(Display *dpy, Window w, int x, int y)
{
    XWindowChanges changes;
    memset(&changes, 0, sizeof(changes));
    changes.x = x;
    changes.y = y;
    return XConfigureWindow(dpy, w, CWX | CWY, &changes);
}

BW_EXPORT int XResizeWindow(Display *dpy, Window w, unsigned int width, unsigned int height)
{
    XWindowChanges changes;
    memset(&changes, 0, sizeof(changes));
    changes.width = (int)width;
    changes.height = (int)height;
    return XConfigureWindow(dpy, w, CWWidth | CWHeight, &changes);
}

BW_EXPORT int XRaiseWindow(Display *dpy, Window w)
{
    XWindowChanges changes;
    memset(&changes, 0, sizeof(changes));
    changes.stack_mode = Above;
    return XConfigureWindow(dpy, w, CWStackMode, &changes);
}

BW_EXPORT int XLowerWindow(Display *dpy, Window w)
{
    XWindowChanges changes;
    memset(&changes, 0, sizeof(changes));
    changes.stack_mode = Below;
    return XConfigureWindow(dpy, w, CWStackMode, &changes);
}

BW_EXPORT int XChangeWindowAttributes(Display *dpy, Window w, unsigned long valuemask,
                                      XSetWindowAttributes *attributes)
{
    return (int)BW(CHANGE_WINDOW_ATTRIBUTES, P(dpy), A(w), A(valuemask), P(attributes));
}

BW_EXPORT int XSetWindowBackground(Display *dpy, Window w, unsigned long background_pixel)
{
    XSetWindowAttributes attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.background_pixel = background_pixel;
    return XChangeWindowAttributes(dpy, w, CWBackPixel, &attributes);
}

BW_EXPORT Status XGetWindowAttributes(Display *dpy, Window w, XWindowAttributes *window_attributes_return)
{
    int64_t result = BW(GET_WINDOW_ATTRIBUTES, P(dpy), A(w), P(window_attributes_return));
    return result == 1 ? 1 : 0;
}

BW_EXPORT int XReparentWindow(Display *dpy, Window w, Window parent, int x, int y)
{
    return (int)BW(REPARENT_WINDOW, P(dpy), A(w), A(parent), A(x), A(y));
}

BW_EXPORT Bool XTranslateCoordinates(Display *dpy, Window src_w, Window dest_w, int src_x,
                                     int src_y, int *dest_x_return, int *dest_y_return,
                                     Window *child_return)
{
    int64_t result = BW(TRANSLATE_COORDINATES, P(dpy), A(src_w), A(dest_w), A(src_x), A(src_y),
                        P(dest_x_return), P(dest_y_return), P(child_return));
    return result == 1 ? True : False;
}

BW_EXPORT Status XGetGeometry(Display *dpy, Drawable d, Window *root_return, int *x_return,
                              int *y_return, unsigned int *width_return,
                              unsigned int *height_return, unsigned int *border_width_return,
                              unsigned int *depth_return)
{
    int64_t result = BW(GET_GEOMETRY, P(dpy), A(d), P(root_return), P(x_return), P(y_return),
                        P(width_return), P(height_return), P(border_width_return),
                        P(depth_return));
    return result == 1 ? 1 : 0;
}

BW_EXPORT Status XQueryTree(Display *dpy, Window w, Window *root_return, Window *parent_return,
                            Window **children_return, unsigned int *nchildren_return)
{
    uint64_t args[6] = { P(dpy), A(w), P(root_return), P(parent_return), 0, 0 };
    int64_t status;
    Window *children = (Window *)bw_sized(BOXEDWINE_X64_X11_OP_QUERY_TREE, args, 6, 4, &status);
    if (status < 0) {
        *children_return = NULL;
        *nchildren_return = 0;
        return 0;
    }
    *children_return = children;
    *nchildren_return = (unsigned int)status;
    return 1;
}

BW_EXPORT Status XWithdrawWindow(Display *dpy, Window w, int screen_number)
{
    (void)screen_number;
    return (Status)BW(WITHDRAW_WINDOW, P(dpy), A(w)) == Success ? 1 : 0;
}

BW_EXPORT Status XIconifyWindow(Display *dpy, Window w, int screen_number)
{
    (void)screen_number;
    return (Status)BW(ICONIFY_WINDOW, P(dpy), A(w));
}

BW_EXPORT int XSetInputFocus(Display *dpy, Window focus, int revert_to, Time time)
{
    return (int)BW(SET_INPUT_FOCUS, P(dpy), A(focus), A(revert_to), A(time));
}

BW_EXPORT int XGetInputFocus(Display *dpy, Window *focus_return, int *revert_to_return)
{
    return (int)BW(GET_INPUT_FOCUS, P(dpy), P(focus_return), P(revert_to_return));
}

BW_EXPORT int XSetTransientForHint(Display *dpy, Window w, Window prop_window)
{
    return (int)BW(SET_TRANSIENT_FOR_HINT, P(dpy), A(w), A(prop_window));
}

BW_EXPORT int XClearArea(Display *dpy, Window w, int x, int y, unsigned int width,
                         unsigned int height, Bool exposures)
{
    return (int)BW(CLEAR_AREA, P(dpy), A(w), A(x), A(y), A(width), A(height), A(exposures));
}

BW_EXPORT int XClearWindow(Display *dpy, Window w)
{
    return XClearArea(dpy, w, 0, 0, 0, 0, False);
}

/* ---- Properties ------------------------------------------------------------- */

BW_EXPORT int XChangeProperty(Display *dpy, Window w, Atom property, Atom type, int format,
                              int mode, _Xconst unsigned char *data, int nelements)
{
    return (int)BW(CHANGE_PROPERTY, P(dpy), A(w), A(property), A(type), A(format), A(mode),
                   P(data), A(nelements));
}

BW_EXPORT int XDeleteProperty(Display *dpy, Window w, Atom property)
{
    return (int)BW(DELETE_PROPERTY, P(dpy), A(w), A(property));
}

BW_EXPORT int XGetWindowProperty(Display *dpy, Window w, Atom property, long long_offset,
                                 long long_length, Bool delete, Atom req_type,
                                 Atom *actual_type_return, int *actual_format_return,
                                 unsigned long *nitems_return, unsigned long *bytes_after_return,
                                 unsigned char **prop_return)
{
    uint64_t args[13] = { P(dpy), A(w), A(property), A(long_offset), A(long_length), A(delete),
                          A(req_type), P(actual_type_return), P(actual_format_return),
                          P(nitems_return), P(bytes_after_return), 0, 0 };
    int64_t status;
    unsigned char *value = (unsigned char *)bw_sized(BOXEDWINE_X64_X11_OP_GET_WINDOW_PROPERTY,
                                                     args, 13, 11, &status);
    if (status < 0) {
        *prop_return = NULL;
        return BadImplementation;
    }
    if (status == Success && !value) {
        /* Xlib always returns one zeroed byte, even for an absent property. */
        value = (unsigned char *)calloc(1, 1);
    }
    *prop_return = value;
    return (int)status;
}

BW_EXPORT int XSetWMHints(Display *dpy, Window w, XWMHints *wm_hints)
{
    return (int)BW(SET_WM_HINTS, P(dpy), A(w), P(wm_hints));
}

BW_EXPORT XWMHints *XGetWMHints(Display *dpy, Window w)
{
    XWMHints *hints = (XWMHints *)calloc(1, sizeof(XWMHints));
    if (!hints) {
        return NULL;
    }
    if (BW(GET_WM_HINTS, P(dpy), A(w), P(hints)) != 1) {
        free(hints);
        return NULL;
    }
    return hints;
}

BW_EXPORT void XSetWMNormalHints(Display *dpy, Window w, XSizeHints *hints)
{
    BW(SET_WM_NORMAL_HINTS, P(dpy), A(w), P(hints));
}

BW_EXPORT Status XGetWMNormalHints(Display *dpy, Window w, XSizeHints *hints_return,
                                   long *supplied_return)
{
    return BW(GET_WM_NORMAL_HINTS, P(dpy), A(w), P(hints_return), P(supplied_return)) == 1 ? 1 : 0;
}

BW_EXPORT void XSetTextProperty(Display *dpy, Window w, XTextProperty *text_prop, Atom property)
{
    BW(SET_TEXT_PROPERTY, P(dpy), A(w), P(text_prop), A(property));
}

BW_EXPORT void XSetWMName(Display *dpy, Window w, XTextProperty *text_prop)
{
    XSetTextProperty(dpy, w, text_prop, XA_WM_NAME);
}

BW_EXPORT void XSetWMIconName(Display *dpy, Window w, XTextProperty *text_prop)
{
    XSetTextProperty(dpy, w, text_prop, XA_WM_ICON_NAME);
}

static Atom bw_utf8_string(Display *dpy)
{
    return XInternAtom(dpy, "UTF8_STRING", False);
}

/* Concatenate a string list into one NUL-separated property value. */
static int bw_text_list_to_property(Display *dpy, char **list, int count, XICCEncodingStyle style,
                                    XTextProperty *text_prop_return)
{
    size_t total = 0;
    unsigned char *value;
    unsigned char *cursor;
    int i;
    for (i = 0; i < count; i++) {
        total += strlen(list[i] ? list[i] : "") + 1;
    }
    value = (unsigned char *)malloc(total ? total + 1 : 1);
    if (!value) {
        return XNoMemory;
    }
    cursor = value;
    for (i = 0; i < count; i++) {
        const char *item = list[i] ? list[i] : "";
        size_t length = strlen(item) + 1;
        memcpy(cursor, item, length);
        cursor += length;
    }
    *cursor = 0;
    text_prop_return->value = value;
    text_prop_return->encoding = style == XUTF8StringStyle ? bw_utf8_string(dpy) : XA_STRING;
    text_prop_return->format = 8;
    /* Xlib excludes the trailing NUL of the last string from nitems. */
    text_prop_return->nitems = total ? total - 1 : 0;
    return Success;
}

BW_EXPORT int XmbTextListToTextProperty(Display *dpy, char **list, int count,
                                        XICCEncodingStyle style, XTextProperty *text_prop_return)
{
    return bw_text_list_to_property(dpy, list, count, style, text_prop_return);
}

BW_EXPORT int Xutf8TextListToTextProperty(Display *dpy, char **list, int count,
                                          XICCEncodingStyle style, XTextProperty *text_prop_return)
{
    return bw_text_list_to_property(dpy, list, count, style, text_prop_return);
}

BW_EXPORT Status XStringListToTextProperty(char **list, int count, XTextProperty *text_prop_return)
{
    return bw_text_list_to_property(NULL, list, count, XStringStyle, text_prop_return) == Success ? 1 : 0;
}

static int bw_text_property_to_list(_Xconst XTextProperty *text_prop, char ***list_return,
                                    int *count_return)
{
    unsigned long i;
    int count = 0;
    char **list;
    char *storage;
    int index = 0;
    unsigned long start = 0;
    if (!text_prop->value || !text_prop->nitems) {
        *list_return = NULL;
        *count_return = 0;
        return Success;
    }
    for (i = 0; i < text_prop->nitems; i++) {
        if (text_prop->value[i] == 0) {
            count++;
        }
    }
    if (text_prop->value[text_prop->nitems - 1] != 0) {
        count++;
    }
    list = (char **)malloc(sizeof(char *) * (size_t)(count + 1) + text_prop->nitems + 1);
    if (!list) {
        return XNoMemory;
    }
    storage = (char *)(list + count + 1);
    memcpy(storage, text_prop->value, text_prop->nitems);
    storage[text_prop->nitems] = 0;
    for (i = 0; i <= text_prop->nitems && index < count; i++) {
        if (i == text_prop->nitems || storage[i] == 0) {
            list[index++] = storage + start;
            start = i + 1;
        }
    }
    list[count] = NULL;
    *list_return = list;
    *count_return = count;
    return Success;
}

BW_EXPORT int XmbTextPropertyToTextList(Display *dpy, _Xconst XTextProperty *text_prop,
                                        char ***list_return, int *count_return)
{
    (void)dpy;
    return bw_text_property_to_list(text_prop, list_return, count_return);
}

BW_EXPORT int Xutf8TextPropertyToTextList(Display *dpy, _Xconst XTextProperty *text_prop,
                                          char ***list_return, int *count_return)
{
    (void)dpy;
    return bw_text_property_to_list(text_prop, list_return, count_return);
}

BW_EXPORT void XFreeStringList(char **list)
{
    free(list);
}

BW_EXPORT int XSetClassHint(Display *dpy, Window w, XClassHint *class_hints)
{
    char *list[2];
    XTextProperty prop;
    int result;
    list[0] = class_hints->res_name ? class_hints->res_name : (char *)"";
    list[1] = class_hints->res_class ? class_hints->res_class : (char *)"";
    if (bw_text_list_to_property(dpy, list, 2, XStringStyle, &prop) != Success) {
        return BadAlloc;
    }
    /* WM_CLASS keeps both terminators. */
    prop.nitems += 1;
    result = (int)BW(SET_TEXT_PROPERTY, P(dpy), A(w), P(&prop), A(XA_WM_CLASS));
    free(prop.value);
    return result;
}

BW_EXPORT void XSetWMProperties(Display *dpy, Window w, XTextProperty *window_name,
                                XTextProperty *icon_name, char **argv, int argc,
                                XSizeHints *normal_hints, XWMHints *wm_hints,
                                XClassHint *class_hints)
{
    static const char machine[] = "debian";
    static const char locale[] = "en_US.UTF-8";
    (void)argv;
    (void)argc;
    if (window_name) {
        XSetWMName(dpy, w, window_name);
    }
    if (icon_name) {
        XSetWMIconName(dpy, w, icon_name);
    }
    if (normal_hints) {
        XSetWMNormalHints(dpy, w, normal_hints);
    }
    if (wm_hints) {
        XSetWMHints(dpy, w, wm_hints);
    }
    if (class_hints) {
        XSetClassHint(dpy, w, class_hints);
    }
    XChangeProperty(dpy, w, XA_WM_CLIENT_MACHINE, XA_STRING, 8, PropModeReplace,
                    (const unsigned char *)machine, (int)strlen(machine));
    XChangeProperty(dpy, w, XInternAtom(dpy, "WM_LOCALE_NAME", False), XA_STRING, 8,
                    PropModeReplace, (const unsigned char *)locale, (int)strlen(locale));
}

BW_EXPORT XWMHints *XAllocWMHints(void)
{
    return (XWMHints *)calloc(1, sizeof(XWMHints));
}

BW_EXPORT XSizeHints *XAllocSizeHints(void)
{
    return (XSizeHints *)calloc(1, sizeof(XSizeHints));
}

BW_EXPORT XClassHint *XAllocClassHint(void)
{
    return (XClassHint *)calloc(1, sizeof(XClassHint));
}

BW_EXPORT XIconSize *XAllocIconSize(void)
{
    return (XIconSize *)calloc(1, sizeof(XIconSize));
}

/* ---- Events ----------------------------------------------------------------- */

static long bw_event_mask_for_type(int type)
{
    switch (type) {
    case KeyPress: return KeyPressMask;
    case KeyRelease: return KeyReleaseMask;
    case ButtonPress: return ButtonPressMask;
    case ButtonRelease: return ButtonReleaseMask;
    case MotionNotify:
        return PointerMotionMask | PointerMotionHintMask | ButtonMotionMask | Button1MotionMask |
               Button2MotionMask | Button3MotionMask | Button4MotionMask | Button5MotionMask;
    case EnterNotify: return EnterWindowMask;
    case LeaveNotify: return LeaveWindowMask;
    case FocusIn:
    case FocusOut: return FocusChangeMask;
    case KeymapNotify: return KeymapStateMask;
    case Expose:
    case GraphicsExpose:
    case NoExpose: return ExposureMask;
    case VisibilityNotify: return VisibilityChangeMask;
    case CreateNotify: return SubstructureNotifyMask;
    case DestroyNotify:
    case UnmapNotify:
    case MapNotify:
    case ReparentNotify:
    case ConfigureNotify:
    case GravityNotify:
    case CirculateNotify: return StructureNotifyMask | SubstructureNotifyMask;
    case MapRequest:
    case ConfigureRequest:
    case CirculateRequest: return SubstructureRedirectMask;
    case ResizeRequest: return ResizeRedirectMask;
    case PropertyNotify: return PropertyChangeMask;
    case ColormapNotify: return ColormapChangeMask;
    default: return 0;
    }
}

typedef Bool (*bw_event_predicate)(Display *, XEvent *, XPointer);

/* Scan the queue under the host lock; the predicate runs in the guest. */
static Bool bw_scan_events(Display *dpy, XEvent *event_return, bw_event_predicate predicate,
                           XPointer arg)
{
    int64_t count = BW(LOCK_EVENTS, P(dpy));
    int64_t i;
    if (count < 0) {
        return False;
    }
    for (i = 0; i < count; i++) {
        XEvent event;
        memset(&event, 0, sizeof(event));
        if (BW(GET_EVENT, P(dpy), A(i), P(&event)) != 1) {
            break;
        }
        if (predicate(dpy, &event, arg)) {
            *event_return = event;
            BW(REMOVE_EVENT, P(dpy), A(i));
            BW(UNLOCK_EVENTS, P(dpy));
            return True;
        }
    }
    BW(UNLOCK_EVENTS, P(dpy));
    return False;
}

static Bool bw_any_event(Display *dpy, XEvent *event, XPointer arg)
{
    (void)dpy; (void)event; (void)arg;
    return True;
}

struct bw_window_mask {
    Window window;
    long mask;
    int type;
    int match_window;
    int match_type;
};

static Bool bw_window_mask_predicate(Display *dpy, XEvent *event, XPointer arg)
{
    struct bw_window_mask *filter = (struct bw_window_mask *)arg;
    (void)dpy;
    if (filter->match_window && event->xany.window != filter->window) {
        return False;
    }
    if (filter->match_type) {
        return event->type == filter->type;
    }
    return (bw_event_mask_for_type(event->type) & filter->mask) != 0;
}

/* Wait until the wakeup socket says the queue changed, or a short timeout. */
static void bw_wait_for_events(Display *dpy)
{
    struct pollfd pfd;
    pfd.fd = PRIV(dpy)->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    poll(&pfd, 1, 50);
}

BW_EXPORT Bool XCheckIfEvent(Display *dpy, XEvent *event_return,
                             Bool (*predicate)(Display *, XEvent *, XPointer), XPointer arg)
{
    return bw_scan_events(dpy, event_return, predicate, arg);
}

BW_EXPORT int XIfEvent(Display *dpy, XEvent *event_return,
                       Bool (*predicate)(Display *, XEvent *, XPointer), XPointer arg)
{
    while (!bw_scan_events(dpy, event_return, predicate, arg)) {
        bw_wait_for_events(dpy);
    }
    return 0;
}

BW_EXPORT int XNextEvent(Display *dpy, XEvent *event_return)
{
    return XIfEvent(dpy, event_return, bw_any_event, NULL);
}

BW_EXPORT int XPending(Display *dpy)
{
    int64_t count = BW(LOCK_EVENTS, P(dpy));
    BW(UNLOCK_EVENTS, P(dpy));
    return count > 0 ? (int)count : 0;
}

BW_EXPORT int XEventsQueued(Display *dpy, int mode)
{
    (void)mode;
    return XPending(dpy);
}

BW_EXPORT int XWindowEvent(Display *dpy, Window w, long event_mask, XEvent *event_return)
{
    struct bw_window_mask filter = { w, event_mask, 0, 1, 0 };
    while (!bw_scan_events(dpy, event_return, bw_window_mask_predicate, (XPointer)&filter)) {
        bw_wait_for_events(dpy);
    }
    return 0;
}

BW_EXPORT Bool XCheckWindowEvent(Display *dpy, Window w, long event_mask, XEvent *event_return)
{
    struct bw_window_mask filter = { w, event_mask, 0, 1, 0 };
    return bw_scan_events(dpy, event_return, bw_window_mask_predicate, (XPointer)&filter);
}

BW_EXPORT Bool XCheckMaskEvent(Display *dpy, long event_mask, XEvent *event_return)
{
    struct bw_window_mask filter = { 0, event_mask, 0, 0, 0 };
    return bw_scan_events(dpy, event_return, bw_window_mask_predicate, (XPointer)&filter);
}

BW_EXPORT int XMaskEvent(Display *dpy, long event_mask, XEvent *event_return)
{
    while (!XCheckMaskEvent(dpy, event_mask, event_return)) {
        bw_wait_for_events(dpy);
    }
    return 0;
}

BW_EXPORT Bool XCheckTypedEvent(Display *dpy, int event_type, XEvent *event_return)
{
    struct bw_window_mask filter = { 0, 0, event_type, 0, 1 };
    return bw_scan_events(dpy, event_return, bw_window_mask_predicate, (XPointer)&filter);
}

BW_EXPORT Bool XCheckTypedWindowEvent(Display *dpy, Window w, int event_type, XEvent *event_return)
{
    return BW(CHECK_TYPED_WINDOW_EVENT, P(dpy), A(w), A(event_type), P(event_return)) == 1 ? True : False;
}

BW_EXPORT Bool XFilterEvent(XEvent *event, Window window)
{
    (void)event;
    (void)window;
    return False;
}

BW_EXPORT Status XSendEvent(Display *dpy, Window w, Bool propagate, long event_mask,
                            XEvent *event_send)
{
    int64_t result = BW(SEND_EVENT, P(dpy), A(w), A(propagate), A(event_mask), P(event_send));
    return result == 1 ? 1 : 0;
}

BW_EXPORT int XPutBackEvent(Display *dpy, XEvent *event)
{
    return (int)BW(PUT_BACK_EVENT, P(dpy), P(event));
}

BW_EXPORT Bool XGetEventData(Display *dpy, XGenericEventCookie *cookie)
{
    (void)dpy;
    (void)cookie;
    return False;
}

BW_EXPORT void XFreeEventData(Display *dpy, XGenericEventCookie *cookie)
{
    (void)dpy;
    (void)cookie;
}

/* ---- Pointer ---------------------------------------------------------------- */

BW_EXPORT int XGrabPointer(Display *dpy, Window grab_window, Bool owner_events,
                           unsigned int event_mask, int pointer_mode, int keyboard_mode,
                           Window confine_to, Cursor cursor, Time time)
{
    return (int)BW(GRAB_POINTER, P(dpy), A(grab_window), A(owner_events), A(event_mask),
                   A(pointer_mode), A(keyboard_mode), A(confine_to), A(cursor), A(time));
}

BW_EXPORT int XUngrabPointer(Display *dpy, Time time)
{
    return (int)BW(UNGRAB_POINTER, P(dpy), A(time));
}

BW_EXPORT int XWarpPointer(Display *dpy, Window src_w, Window dest_w, int src_x, int src_y,
                           unsigned int src_width, unsigned int src_height, int dest_x, int dest_y)
{
    return (int)BW(WARP_POINTER, P(dpy), A(src_w), A(dest_w), A(src_x), A(src_y), A(src_width),
                   A(src_height), A(dest_x), A(dest_y));
}

BW_EXPORT Bool XQueryPointer(Display *dpy, Window w, Window *root_return, Window *child_return,
                             int *root_x_return, int *root_y_return, int *win_x_return,
                             int *win_y_return, unsigned int *mask_return)
{
    int64_t result = BW(QUERY_POINTER, P(dpy), A(w), P(root_return), P(child_return),
                        P(root_x_return), P(root_y_return), P(win_x_return), P(win_y_return),
                        P(mask_return));
    return result == 1 ? True : False;
}

BW_EXPORT int XGrabKeyboard(Display *dpy, Window grab_window, Bool owner_events, int pointer_mode,
                            int keyboard_mode, Time time)
{
    (void)dpy; (void)grab_window; (void)owner_events; (void)pointer_mode; (void)keyboard_mode; (void)time;
    return GrabSuccess;
}

BW_EXPORT int XUngrabKeyboard(Display *dpy, Time time)
{
    (void)dpy;
    (void)time;
    return 1;
}

/* ---- Colormaps -------------------------------------------------------------- */

BW_EXPORT Colormap XCreateColormap(Display *dpy, Window w, Visual *visual, int alloc)
{
    int64_t result = BW(CREATE_COLORMAP, P(dpy), A(w), P(visual), A(alloc));
    return result > 0 ? (Colormap)result : None;
}

BW_EXPORT int XFreeColormap(Display *dpy, Colormap colormap)
{
    return (int)BW(FREE_COLORMAP, P(dpy), A(colormap));
}

BW_EXPORT int XInstallColormap(Display *dpy, Colormap colormap)
{
    (void)dpy;
    (void)colormap;
    return 1;
}

BW_EXPORT Status XAllocColor(Display *dpy, Colormap colormap, XColor *screen_in_out)
{
    return BW(ALLOC_COLOR, P(dpy), A(colormap), P(screen_in_out)) == 1 ? 1 : 0;
}

BW_EXPORT Status XAllocColorCells(Display *dpy, Colormap colormap, Bool contig,
                                  unsigned long *plane_masks_return, unsigned int nplanes,
                                  unsigned long *pixels_return, unsigned int npixels)
{
    return BW(ALLOC_COLOR_CELLS, P(dpy), A(colormap), A(contig), P(plane_masks_return),
              A(nplanes), P(pixels_return), A(npixels)) == 1 ? 1 : 0;
}

BW_EXPORT int XFreeColors(Display *dpy, Colormap colormap, unsigned long *pixels, int npixels,
                          unsigned long planes)
{
    return (int)BW(FREE_COLORS, P(dpy), A(colormap), P(pixels), A(npixels), A(planes));
}

BW_EXPORT int XQueryColor(Display *dpy, Colormap colormap, XColor *def_in_out)
{
    return (int)BW(QUERY_COLOR, P(dpy), A(colormap), P(def_in_out));
}

BW_EXPORT int XQueryColors(Display *dpy, Colormap colormap, XColor *defs_in_out, int ncolors)
{
    return (int)BW(QUERY_COLORS, P(dpy), A(colormap), P(defs_in_out), A(ncolors));
}

BW_EXPORT int XStoreColor(Display *dpy, Colormap colormap, XColor *color)
{
    return (int)BW(STORE_COLOR, P(dpy), A(colormap), P(color));
}

/* ---- Graphics contexts and drawing ------------------------------------------ */

BW_EXPORT GC XCreateGC(Display *dpy, Drawable d, unsigned long valuemask, XGCValues *values)
{
    int64_t result = BW(CREATE_GC, P(dpy), A(d), A(valuemask), P(values));
    /* GC is an opaque pointer in Xlib; the server id is carried in it. */
    return result > 0 ? (GC)(uintptr_t)result : NULL;
}

BW_EXPORT int XFreeGC(Display *dpy, GC gc)
{
    return (int)BW(FREE_GC, P(dpy), P(gc));
}

BW_EXPORT int XChangeGC(Display *dpy, GC gc, unsigned long valuemask, XGCValues *values)
{
    return (int)BW(CHANGE_GC, P(dpy), P(gc), A(valuemask), P(values));
}

BW_EXPORT GContext XGContextFromGC(GC gc)
{
    return (GContext)(uintptr_t)gc;
}

BW_EXPORT int XSetFunction(Display *dpy, GC gc, int function)
{
    return (int)BW(SET_GC_VALUE, P(dpy), P(gc), A(BOXEDWINE_X64_X11_GC_FUNCTION), A(function));
}

BW_EXPORT int XSetBackground(Display *dpy, GC gc, unsigned long background)
{
    return (int)BW(SET_GC_VALUE, P(dpy), P(gc), A(BOXEDWINE_X64_X11_GC_BACKGROUND), A(background));
}

BW_EXPORT int XSetForeground(Display *dpy, GC gc, unsigned long foreground)
{
    return (int)BW(SET_GC_VALUE, P(dpy), P(gc), A(BOXEDWINE_X64_X11_GC_FOREGROUND), A(foreground));
}

BW_EXPORT int XSetSubwindowMode(Display *dpy, GC gc, int subwindow_mode)
{
    return (int)BW(SET_GC_VALUE, P(dpy), P(gc), A(BOXEDWINE_X64_X11_GC_SUBWINDOW_MODE), A(subwindow_mode));
}

BW_EXPORT int XSetGraphicsExposures(Display *dpy, GC gc, Bool graphics_exposures)
{
    return (int)BW(SET_GC_VALUE, P(dpy), P(gc), A(BOXEDWINE_X64_X11_GC_GRAPHICS_EXPOSURES), A(graphics_exposures));
}

BW_EXPORT int XSetClipMask(Display *dpy, GC gc, Pixmap pixmap)
{
    return (int)BW(SET_GC_VALUE, P(dpy), P(gc), A(BOXEDWINE_X64_X11_GC_CLIP_MASK), A(pixmap));
}

BW_EXPORT int XSetFillStyle(Display *dpy, GC gc, int fill_style)
{
    return (int)BW(SET_GC_VALUE, P(dpy), P(gc), A(BOXEDWINE_X64_X11_GC_FILL_STYLE), A(fill_style));
}

BW_EXPORT int XSetArcMode(Display *dpy, GC gc, int arc_mode)
{
    return (int)BW(SET_GC_VALUE, P(dpy), P(gc), A(BOXEDWINE_X64_X11_GC_ARC_MODE), A(arc_mode));
}

BW_EXPORT int XSetClipRectangles(Display *dpy, GC gc, int clip_x_origin, int clip_y_origin,
                                 XRectangle *rectangles, int n, int ordering)
{
    return (int)BW(SET_CLIP_RECTANGLES, P(dpy), P(gc), A(clip_x_origin), A(clip_y_origin),
                   P(rectangles), A(n), A(ordering));
}

BW_EXPORT int XSetDashes(Display *dpy, GC gc, int dash_offset, _Xconst char *dash_list, int n)
{
    (void)dpy; (void)gc; (void)dash_offset; (void)dash_list; (void)n;
    BW_STUB("XSetDashes");
    return 1;
}

BW_EXPORT int XCopyArea(Display *dpy, Drawable src, Drawable dest, GC gc, int src_x, int src_y,
                        unsigned int width, unsigned int height, int dest_x, int dest_y)
{
    return (int)BW(COPY_AREA, P(dpy), A(src), A(dest), P(gc), A(src_x), A(src_y), A(width),
                   A(height), A(dest_x), A(dest_y));
}

BW_EXPORT int XCopyPlane(Display *dpy, Drawable src, Drawable dest, GC gc, int src_x, int src_y,
                         unsigned int width, unsigned int height, int dest_x, int dest_y,
                         unsigned long plane)
{
    (void)dpy; (void)src; (void)dest; (void)gc; (void)src_x; (void)src_y; (void)width;
    (void)height; (void)dest_x; (void)dest_y; (void)plane;
    BW_STUB("XCopyPlane");
    return 1;
}

BW_EXPORT int XDrawLine(Display *dpy, Drawable d, GC gc, int x1, int y1, int x2, int y2)
{
    return (int)BW(DRAW_LINE, P(dpy), A(d), P(gc), A(x1), A(y1), A(x2), A(y2));
}

BW_EXPORT int XDrawLines(Display *dpy, Drawable d, GC gc, XPoint *points, int npoints, int mode)
{
    int i;
    (void)mode;
    for (i = 1; i < npoints; i++) {
        XDrawLine(dpy, d, gc, points[i - 1].x, points[i - 1].y, points[i].x, points[i].y);
    }
    return 1;
}

BW_EXPORT int XDrawRectangle(Display *dpy, Drawable d, GC gc, int x, int y, unsigned int width,
                             unsigned int height)
{
    return (int)BW(DRAW_RECTANGLE, P(dpy), A(d), P(gc), A(x), A(y), A(width), A(height));
}

BW_EXPORT int XFillRectangle(Display *dpy, Drawable d, GC gc, int x, int y, unsigned int width,
                             unsigned int height)
{
    return (int)BW(FILL_RECTANGLE, P(dpy), A(d), P(gc), A(x), A(y), A(width), A(height));
}

BW_EXPORT int XFillRectangles(Display *dpy, Drawable d, GC gc, XRectangle *rectangles,
                              int nrectangles)
{
    int i;
    for (i = 0; i < nrectangles; i++) {
        XFillRectangle(dpy, d, gc, rectangles[i].x, rectangles[i].y, rectangles[i].width,
                       rectangles[i].height);
    }
    return 1;
}

BW_EXPORT int XDrawPoint(Display *dpy, Drawable d, GC gc, int x, int y)
{
    return XFillRectangle(dpy, d, gc, x, y, 1, 1);
}

BW_EXPORT int XDrawArc(Display *dpy, Drawable d, GC gc, int x, int y, unsigned int width,
                       unsigned int height, int angle1, int angle2)
{
    (void)dpy; (void)d; (void)gc; (void)x; (void)y; (void)width; (void)height; (void)angle1; (void)angle2;
    BW_STUB("XDrawArc");
    return 1;
}

BW_EXPORT int XFillArc(Display *dpy, Drawable d, GC gc, int x, int y, unsigned int width,
                       unsigned int height, int angle1, int angle2)
{
    (void)dpy; (void)d; (void)gc; (void)x; (void)y; (void)width; (void)height; (void)angle1; (void)angle2;
    BW_STUB("XFillArc");
    return 1;
}

BW_EXPORT int XFillPolygon(Display *dpy, Drawable d, GC gc, XPoint *points, int npoints, int shape,
                           int mode)
{
    (void)dpy; (void)d; (void)gc; (void)points; (void)npoints; (void)shape; (void)mode;
    BW_STUB("XFillPolygon");
    return 1;
}

/* ---- Images ----------------------------------------------------------------- */

static int bw_destroy_image(XImage *image);
static unsigned long bw_get_pixel(XImage *image, int x, int y);
static int bw_put_pixel(XImage *image, int x, int y, unsigned long pixel);
static XImage *bw_sub_image(XImage *image, int x, int y, unsigned int width, unsigned int height);
static int bw_add_pixel(XImage *image, long value);

static void bw_init_image_funcs(XImage *image)
{
    image->f.create_image = XCreateImage;
    image->f.destroy_image = bw_destroy_image;
    image->f.get_pixel = bw_get_pixel;
    image->f.put_pixel = bw_put_pixel;
    image->f.sub_image = bw_sub_image;
    image->f.add_pixel = bw_add_pixel;
}

static int bw_round_up(int value, int multiple)
{
    if (multiple <= 0) {
        return value;
    }
    return ((value + multiple - 1) / multiple) * multiple;
}

BW_EXPORT XImage *XCreateImage(Display *dpy, Visual *visual, unsigned int depth, int format,
                               int offset, char *data, unsigned int width, unsigned int height,
                               int bitmap_pad, int bytes_per_line)
{
    XImage *image = (XImage *)calloc(1, sizeof(XImage));
    int bits_per_pixel;
    if (!image) {
        return NULL;
    }
    if (depth == 1) {
        bits_per_pixel = 1;
    } else if (depth <= 8) {
        bits_per_pixel = 8;
    } else if (depth <= 16) {
        bits_per_pixel = 16;
    } else {
        bits_per_pixel = 32;
    }
    image->width = (int)width;
    image->height = (int)height;
    image->xoffset = offset;
    image->format = format;
    image->data = data;
    image->byte_order = PRIV(dpy)->byte_order;
    image->bitmap_unit = bits_per_pixel == 1 ? 32 : bits_per_pixel;
    image->bitmap_bit_order = PRIV(dpy)->bitmap_bit_order;
    image->bitmap_pad = bitmap_pad ? bitmap_pad : 32;
    image->depth = (int)depth;
    image->bits_per_pixel = bits_per_pixel;
    if (bytes_per_line == 0) {
        bytes_per_line = bw_round_up((int)(bits_per_pixel * width), image->bitmap_pad) / 8;
    }
    image->bytes_per_line = bytes_per_line;
    if (visual) {
        image->red_mask = visual->red_mask;
        image->green_mask = visual->green_mask;
        image->blue_mask = visual->blue_mask;
    }
    bw_init_image_funcs(image);
    return image;
}

BW_EXPORT Status XInitImage(XImage *image)
{
    bw_init_image_funcs(image);
    return 1;
}

static int bw_destroy_image(XImage *image)
{
    if (image) {
        free(image->data);
        free(image);
    }
    return 1;
}

BW_EXPORT int XDestroyImage(XImage *image)
{
    return bw_destroy_image(image);
}

static unsigned long bw_get_pixel(XImage *image, int x, int y)
{
    unsigned char *row;
    if (!image->data || x < 0 || y < 0 || x >= image->width || y >= image->height) {
        return 0;
    }
    row = (unsigned char *)image->data + (size_t)y * (size_t)image->bytes_per_line;
    switch (image->bits_per_pixel) {
    case 32: return *(uint32_t *)(row + (size_t)x * 4);
    case 16: return *(uint16_t *)(row + (size_t)x * 2);
    case 8: return row[x];
    case 1: return (row[x / 8] >> (x % 8)) & 1;
    default: return 0;
    }
}

static int bw_put_pixel(XImage *image, int x, int y, unsigned long pixel)
{
    unsigned char *row;
    if (!image->data || x < 0 || y < 0 || x >= image->width || y >= image->height) {
        return 0;
    }
    row = (unsigned char *)image->data + (size_t)y * (size_t)image->bytes_per_line;
    switch (image->bits_per_pixel) {
    case 32: *(uint32_t *)(row + (size_t)x * 4) = (uint32_t)pixel; break;
    case 16: *(uint16_t *)(row + (size_t)x * 2) = (uint16_t)pixel; break;
    case 8: row[x] = (unsigned char)pixel; break;
    case 1:
        if (pixel & 1) row[x / 8] |= (unsigned char)(1u << (x % 8));
        else row[x / 8] &= (unsigned char)~(1u << (x % 8));
        break;
    default: break;
    }
    return 1;
}

static XImage *bw_sub_image(XImage *image, int x, int y, unsigned int width, unsigned int height)
{
    XImage *sub = (XImage *)calloc(1, sizeof(XImage));
    unsigned int row;
    if (!sub) {
        return NULL;
    }
    *sub = *image;
    sub->width = (int)width;
    sub->height = (int)height;
    sub->bytes_per_line = bw_round_up((int)(image->bits_per_pixel * width), image->bitmap_pad) / 8;
    sub->data = (char *)calloc((size_t)sub->bytes_per_line * height + 1, 1);
    if (!sub->data) {
        free(sub);
        return NULL;
    }
    for (row = 0; row < height; row++) {
        unsigned int column;
        for (column = 0; column < width; column++) {
            bw_put_pixel(sub, (int)column, (int)row, bw_get_pixel(image, x + (int)column, y + (int)row));
        }
    }
    return sub;
}

static int bw_add_pixel(XImage *image, long value)
{
    int x, y;
    for (y = 0; y < image->height; y++) {
        for (x = 0; x < image->width; x++) {
            bw_put_pixel(image, x, y, bw_get_pixel(image, x, y) + (unsigned long)value);
        }
    }
    return 1;
}

BW_EXPORT unsigned long XGetPixel(XImage *image, int x, int y)
{
    return bw_get_pixel(image, x, y);
}

BW_EXPORT int XPutPixel(XImage *image, int x, int y, unsigned long pixel)
{
    return bw_put_pixel(image, x, y, pixel);
}

BW_EXPORT XImage *XSubImage(XImage *image, int x, int y, unsigned int width, unsigned int height)
{
    return bw_sub_image(image, x, y, width, height);
}

BW_EXPORT int XAddPixel(XImage *image, long value)
{
    return bw_add_pixel(image, value);
}

BW_EXPORT int XPutImage(Display *dpy, Drawable d, GC gc, XImage *image, int src_x, int src_y,
                        int dest_x, int dest_y, unsigned int width, unsigned int height)
{
    return (int)BW(PUT_IMAGE, P(dpy), A(d), P(gc), P(image), A(src_x), A(src_y), A(dest_x),
                   A(dest_y), A(width), A(height));
}

BW_EXPORT XImage *XGetImage(Display *dpy, Drawable d, int x, int y, unsigned int width,
                            unsigned int height, unsigned long plane_mask, int format)
{
    (void)dpy; (void)d; (void)x; (void)y; (void)width; (void)height; (void)plane_mask; (void)format;
    BW_STUB("XGetImage");
    return NULL;
}

/* ---- Pixmaps and cursors ---------------------------------------------------- */

BW_EXPORT Pixmap XCreatePixmap(Display *dpy, Drawable d, unsigned int width, unsigned int height,
                               unsigned int depth)
{
    int64_t result = BW(CREATE_PIXMAP, P(dpy), A(d), A(width), A(height), A(depth));
    return result > 0 ? (Pixmap)result : None;
}

BW_EXPORT Pixmap XCreateBitmapFromData(Display *dpy, Drawable d, _Xconst char *data,
                                       unsigned int width, unsigned int height)
{
    int64_t result = BW(CREATE_BITMAP_FROM_DATA, P(dpy), A(d), P(data), A(width), A(height));
    return result > 0 ? (Pixmap)result : None;
}

BW_EXPORT int XFreePixmap(Display *dpy, Pixmap pixmap)
{
    return (int)BW(FREE_PIXMAP, P(dpy), A(pixmap));
}

BW_EXPORT Cursor XCreateFontCursor(Display *dpy, unsigned int shape)
{
    int64_t result = BW(CREATE_FONT_CURSOR, P(dpy), A(shape));
    return result > 0 ? (Cursor)result : None;
}

BW_EXPORT Cursor XCreatePixmapCursor(Display *dpy, Pixmap source, Pixmap mask,
                                     XColor *foreground_color, XColor *background_color,
                                     unsigned int x, unsigned int y)
{
    int64_t result = BW(CREATE_PIXMAP_CURSOR, P(dpy), A(source), A(mask), P(foreground_color),
                        P(background_color), A(x), A(y));
    return result > 0 ? (Cursor)result : None;
}

BW_EXPORT int XDefineCursor(Display *dpy, Window w, Cursor cursor)
{
    return (int)BW(DEFINE_CURSOR, P(dpy), A(w), A(cursor));
}

BW_EXPORT int XUndefineCursor(Display *dpy, Window w)
{
    return XDefineCursor(dpy, w, None);
}

BW_EXPORT int XFreeCursor(Display *dpy, Cursor cursor)
{
    return (int)BW(FREE_CURSOR, P(dpy), A(cursor));
}

/* ---- Keyboard --------------------------------------------------------------- */

BW_EXPORT int XDisplayKeycodes(Display *dpy, int *min_keycodes_return, int *max_keycodes_return)
{
    *min_keycodes_return = PRIV(dpy)->min_keycode;
    *max_keycodes_return = PRIV(dpy)->max_keycode;
    return 1;
}

BW_EXPORT KeySym *XGetKeyboardMapping(Display *dpy,
#if NeedWidePrototypes
                                      unsigned int first_keycode,
#else
                                      KeyCode first_keycode,
#endif
                                      int keycode_count, int *keysyms_per_keycode_return)
{
    uint64_t args[5] = { P(dpy), A(first_keycode), A(keycode_count), 0, 0 };
    int64_t status;
    KeySym *keysyms = (KeySym *)bw_sized(BOXEDWINE_X64_X11_OP_GET_KEYBOARD_MAPPING, args, 5, 3, &status);
    if (status <= 0 || !keysyms) {
        free(keysyms);
        *keysyms_per_keycode_return = 0;
        return NULL;
    }
    *keysyms_per_keycode_return = (int)status;
    return keysyms;
}

BW_EXPORT XModifierKeymap *XGetModifierMapping(Display *dpy)
{
    XModifierKeymap *map = (XModifierKeymap *)calloc(1, sizeof(XModifierKeymap));
    if (!map) {
        return NULL;
    }
    map->modifiermap = (KeyCode *)calloc(8, sizeof(KeyCode));
    if (!map->modifiermap) {
        free(map);
        return NULL;
    }
    map->max_keypermod = (int)BW(GET_MODIFIER_MAPPING, P(dpy), P(map->modifiermap));
    if (map->max_keypermod <= 0) {
        map->max_keypermod = 1;
    }
    return map;
}

BW_EXPORT int XFreeModifiermap(XModifierKeymap *modmap)
{
    if (modmap) {
        free(modmap->modifiermap);
        free(modmap);
    }
    return 1;
}

BW_EXPORT KeyCode XKeysymToKeycode(Display *dpy, KeySym keysym)
{
    return (KeyCode)BW(KEYSYM_TO_KEYCODE, P(dpy), A(keysym));
}

BW_EXPORT KeySym XkbKeycodeToKeysym(Display *dpy,
#if NeedWidePrototypes
                                    unsigned int kc,
#else
                                    KeyCode kc,
#endif
                                    int group, int level)
{
    return (KeySym)BW(KEYCODE_TO_KEYSYM, P(dpy), A(kc), A(group), A(level));
}

BW_EXPORT KeySym XKeycodeToKeysym(Display *dpy,
#if NeedWidePrototypes
                                  unsigned int kc,
#else
                                  KeyCode kc,
#endif
                                  int index)
{
    return XkbKeycodeToKeysym(dpy, kc, 0, index);
}

struct bw_keysym_name {
    KeySym keysym;
    char *name;
};

static struct bw_keysym_name *bw_keysym_names;
static size_t bw_keysym_name_count;
static pthread_mutex_t bw_keysym_mutex = PTHREAD_MUTEX_INITIALIZER;

BW_EXPORT char *XKeysymToString(KeySym keysym)
{
    size_t i;
    uint64_t args[3];
    int64_t status;
    char *name;
    struct bw_keysym_name *grown;
    pthread_mutex_lock(&bw_keysym_mutex);
    for (i = 0; i < bw_keysym_name_count; i++) {
        if (bw_keysym_names[i].keysym == keysym) {
            name = bw_keysym_names[i].name;
            pthread_mutex_unlock(&bw_keysym_mutex);
            return name;
        }
    }
    pthread_mutex_unlock(&bw_keysym_mutex);
    args[0] = A(keysym);
    name = (char *)bw_sized(BOXEDWINE_X64_X11_OP_KEYSYM_TO_STRING, args, 3, 1, &status);
    if (status <= 0 || !name) {
        free(name);
        return NULL;
    }
    /* Xlib returns a static string; keep ours for the life of the process. */
    pthread_mutex_lock(&bw_keysym_mutex);
    grown = (struct bw_keysym_name *)realloc(bw_keysym_names, (bw_keysym_name_count + 1) * sizeof(*grown));
    if (grown) {
        bw_keysym_names = grown;
        bw_keysym_names[bw_keysym_name_count].keysym = keysym;
        bw_keysym_names[bw_keysym_name_count].name = name;
        bw_keysym_name_count++;
    }
    pthread_mutex_unlock(&bw_keysym_mutex);
    return name;
}

BW_EXPORT int XLookupString(XKeyEvent *event_struct, char *buffer_return, int bytes_buffer,
                            KeySym *keysym_return, XComposeStatus *status_in_out)
{
    XEvent copy;
    (void)status_in_out;
    memset(&copy, 0, sizeof(copy));
    copy.xkey = *event_struct;
    return (int)BW(LOOKUP_STRING, P(&copy), P(buffer_return), A(bytes_buffer), P(keysym_return));
}

BW_EXPORT int XkbTranslateKeySym(Display *dpy, KeySym *sym_return, unsigned int modifiers,
                                 char *buffer, int nbytes, int *extra_rtrn)
{
    if (extra_rtrn) {
        *extra_rtrn = 0;
    }
    return (int)BW(KB_TRANSLATE_KEYSYM, P(dpy), P(sym_return), A(modifiers), P(buffer), A(nbytes),
                   P(extra_rtrn));
}

BW_EXPORT KeySym XLookupKeysym(XKeyEvent *key_event, int index)
{
    XEvent copy;
    memset(&copy, 0, sizeof(copy));
    copy.xkey = *key_event;
    return (KeySym)BW(LOOKUP_KEYSYM, P(&copy), A(index));
}

BW_EXPORT int XRefreshKeyboardMapping(XMappingEvent *event_map)
{
    (void)event_map;
    return 1;
}

BW_EXPORT Bool XkbUseExtension(Display *dpy, int *major_rtrn, int *minor_rtrn)
{
    (void)dpy;
    if (major_rtrn) *major_rtrn = 1;
    if (minor_rtrn) *minor_rtrn = 0;
    return True;
}

BW_EXPORT Bool XkbSetDetectableAutoRepeat(Display *dpy, Bool detectable, Bool *supported)
{
    (void)dpy;
    if (supported) {
        *supported = True;
    }
    return detectable;
}

BW_EXPORT Bool XkbLibraryVersion(int *lib_major, int *lib_minor)
{
    if (lib_major) *lib_major = 1;
    if (lib_minor) *lib_minor = 0;
    return True;
}

/* ---- Extensions, selections ------------------------------------------------- */

BW_EXPORT Bool XQueryExtension(Display *dpy, _Xconst char *name, int *major_opcode_return,
                               int *first_event_return, int *first_error_return)
{
    int64_t result = BW(QUERY_EXTENSION, P(dpy), P(name), P(major_opcode_return),
                        P(first_event_return), P(first_error_return));
    return result == 1 ? True : False;
}

BW_EXPORT int XSetSelectionOwner(Display *dpy, Atom selection, Window owner, Time time)
{
    return (int)BW(SET_SELECTION_OWNER, P(dpy), A(selection), A(owner), A(time));
}

BW_EXPORT Window XGetSelectionOwner(Display *dpy, Atom selection)
{
    int64_t result = BW(GET_SELECTION_OWNER, P(dpy), A(selection));
    return result > 0 ? (Window)result : None;
}

BW_EXPORT int XConvertSelection(Display *dpy, Atom selection, Atom target, Atom property,
                                Window requestor, Time time)
{
    return (int)BW(CONVERT_SELECTION, P(dpy), A(selection), A(target), A(property), A(requestor),
                   A(time));
}

/* ---- Input methods and fonts (no input method is offered) ------------------- */

BW_EXPORT Bool XSupportsLocale(void)
{
    return True;
}

BW_EXPORT char *XSetLocaleModifiers(_Xconst char *modifier_list)
{
    (void)modifier_list;
    return (char *)"";
}

BW_EXPORT XIM XOpenIM(Display *dpy, struct _XrmHashBucketRec *rdb, char *res_name, char *res_class)
{
    (void)dpy; (void)rdb; (void)res_name; (void)res_class;
    return NULL;
}

BW_EXPORT Status XCloseIM(XIM im)
{
    (void)im;
    return 1;
}

BW_EXPORT char *XGetIMValues(XIM im, ...)
{
    (void)im;
    return (char *)"unsupported";
}

BW_EXPORT char *XSetIMValues(XIM im, ...)
{
    (void)im;
    return (char *)"unsupported";
}

BW_EXPORT Display *XDisplayOfIM(XIM im)
{
    (void)im;
    return NULL;
}

BW_EXPORT char *XLocaleOfIM(XIM im)
{
    (void)im;
    return (char *)"C";
}

BW_EXPORT Bool XRegisterIMInstantiateCallback(Display *dpy, struct _XrmHashBucketRec *rdb,
                                              char *res_name, char *res_class, XIDProc callback,
                                              XPointer client_data)
{
    (void)dpy; (void)rdb; (void)res_name; (void)res_class; (void)callback; (void)client_data;
    return False;
}

BW_EXPORT Bool XUnregisterIMInstantiateCallback(Display *dpy, struct _XrmHashBucketRec *rdb,
                                                char *res_name, char *res_class, XIDProc callback,
                                                XPointer client_data)
{
    (void)dpy; (void)rdb; (void)res_name; (void)res_class; (void)callback; (void)client_data;
    return False;
}

BW_EXPORT XIC XCreateIC(XIM im, ...)
{
    (void)im;
    return NULL;
}

BW_EXPORT void XDestroyIC(XIC ic)
{
    (void)ic;
}

BW_EXPORT void XSetICFocus(XIC ic)
{
    (void)ic;
}

BW_EXPORT void XUnsetICFocus(XIC ic)
{
    (void)ic;
}

BW_EXPORT char *XSetICValues(XIC ic, ...)
{
    (void)ic;
    return (char *)"unsupported";
}

BW_EXPORT char *XGetICValues(XIC ic, ...)
{
    (void)ic;
    return (char *)"unsupported";
}

BW_EXPORT char *XmbResetIC(XIC ic)
{
    (void)ic;
    return NULL;
}

BW_EXPORT XVaNestedList XVaCreateNestedList(int unused, ...)
{
    (void)unused;
    return NULL;
}

BW_EXPORT int XmbLookupString(XIC ic, XKeyPressedEvent *event, char *buffer_return, int bytes_buffer,
                              KeySym *keysym_return, Status *status_return)
{
    int length = XLookupString(event, buffer_return, bytes_buffer, keysym_return, NULL);
    (void)ic;
    if (status_return) {
        *status_return = length > 0 ? XLookupBoth : (keysym_return && *keysym_return ? XLookupKeySym : XLookupNone);
    }
    return length;
}

BW_EXPORT XFontSet XCreateFontSet(Display *dpy, _Xconst char *base_font_name_list,
                                  char ***missing_charset_list, int *missing_charset_count,
                                  char **def_string)
{
    (void)dpy;
    (void)base_font_name_list;
    if (missing_charset_list) *missing_charset_list = NULL;
    if (missing_charset_count) *missing_charset_count = 0;
    if (def_string) *def_string = NULL;
    return (XFontSet)1;
}

BW_EXPORT void XFreeFontSet(Display *dpy, XFontSet font_set)
{
    (void)dpy;
    (void)font_set;
}

BW_EXPORT int XFreeFont(Display *dpy, XFontStruct *font_struct)
{
    (void)dpy;
    (void)font_struct;
    return 1;
}
