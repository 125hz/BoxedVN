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
 * The x86-64 guest libXext.so.6 for BoxedWine.
 *
 * winex11.so links libXext directly for the SHAPE and MIT-SHM entry points.
 * Neither extension is offered: the query functions say so, shape requests
 * are accepted as no-ops (a window without a shape is a rectangle, which is
 * what Wine draws anyway), and shared-memory images are refused so Wine
 * takes its XPutImage path.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/XShm.h>

#include <stdint.h>
#include <stdlib.h>

#include "boxedwine_x64_x11_bridge.h"

#define BW_EXPORT __attribute__((visibility("default")))

#define BW_STUB(name) \
    do { \
        static int reported; \
        if (!reported) { \
            reported = 1; \
            uint64_t args[1] = { (uint64_t)(uintptr_t)(name) }; \
            boxedwine_x64_x11_call(BOXEDWINE_X64_X11_OP_REPORT_UNIMPLEMENTED, args, 1); \
        } \
    } while (0)

BW_EXPORT Bool XShapeQueryExtension(Display *dpy, int *event_base, int *error_base)
{
    (void)dpy;
    if (event_base) *event_base = 0;
    if (error_base) *error_base = 0;
    return False;
}

BW_EXPORT Status XShapeQueryVersion(Display *dpy, int *major_version, int *minor_version)
{
    (void)dpy;
    if (major_version) *major_version = 0;
    if (minor_version) *minor_version = 0;
    return 0;
}

BW_EXPORT void XShapeCombineMask(Display *dpy, Window dest, int dest_kind, int x_off, int y_off,
                                 Pixmap src, int op)
{
    (void)dpy; (void)dest; (void)dest_kind; (void)x_off; (void)y_off; (void)src; (void)op;
    BW_STUB("XShapeCombineMask");
}

BW_EXPORT void XShapeCombineRectangles(Display *dpy, Window dest, int dest_kind, int x_off,
                                       int y_off, XRectangle *rectangles, int n_rects, int op,
                                       int ordering)
{
    (void)dpy; (void)dest; (void)dest_kind; (void)x_off; (void)y_off; (void)rectangles;
    (void)n_rects; (void)op; (void)ordering;
    BW_STUB("XShapeCombineRectangles");
}

BW_EXPORT void XShapeOffsetShape(Display *dpy, Window dest, int dest_kind, int x_off, int y_off)
{
    (void)dpy; (void)dest; (void)dest_kind; (void)x_off; (void)y_off;
    BW_STUB("XShapeOffsetShape");
}

BW_EXPORT Bool XShmQueryExtension(Display *dpy)
{
    (void)dpy;
    return False;
}

BW_EXPORT Bool XShmQueryVersion(Display *dpy, int *major, int *minor, Bool *pixmaps)
{
    (void)dpy;
    if (major) *major = 0;
    if (minor) *minor = 0;
    if (pixmaps) *pixmaps = False;
    return False;
}

BW_EXPORT Bool XShmAttach(Display *dpy, XShmSegmentInfo *shminfo)
{
    (void)dpy;
    (void)shminfo;
    BW_STUB("XShmAttach");
    return False;
}

BW_EXPORT Bool XShmDetach(Display *dpy, XShmSegmentInfo *shminfo)
{
    (void)dpy;
    (void)shminfo;
    return False;
}

BW_EXPORT XImage *XShmCreateImage(Display *dpy, Visual *visual, unsigned int depth, int format,
                                  char *data, XShmSegmentInfo *shminfo, unsigned int width,
                                  unsigned int height)
{
    (void)dpy; (void)visual; (void)depth; (void)format; (void)data; (void)shminfo; (void)width;
    (void)height;
    BW_STUB("XShmCreateImage");
    return NULL;
}

BW_EXPORT Bool XShmPutImage(Display *dpy, Drawable d, GC gc, XImage *image, int src_x, int src_y,
                            int dst_x, int dst_y, unsigned int src_width, unsigned int src_height,
                            Bool send_event)
{
    (void)dpy; (void)d; (void)gc; (void)image; (void)src_x; (void)src_y; (void)dst_x; (void)dst_y;
    (void)src_width; (void)src_height; (void)send_event;
    BW_STUB("XShmPutImage");
    return False;
}

BW_EXPORT Bool XShmGetImage(Display *dpy, Drawable d, XImage *image, int x, int y,
                            unsigned long plane_mask)
{
    (void)dpy; (void)d; (void)image; (void)x; (void)y; (void)plane_mask;
    return False;
}

BW_EXPORT int XShmGetEventBase(Display *dpy)
{
    (void)dpy;
    return 0;
}

BW_EXPORT int XShmPixmapFormat(Display *dpy)
{
    (void)dpy;
    return 0;
}
