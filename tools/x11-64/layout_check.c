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
 * Layout assertions for the x86-64 guest X11 shim.
 *
 * The host writes Display, Screen, Visual, XEvent and the rest in the
 * layout recorded in source/x11/x11layout64.h. This file pins the same
 * numbers against the exact X11 headers the shim is compiled with, so a
 * header revision that moved a field would fail the build rather than hand
 * Wine a shifted structure. Every number here must match x11layout64.h.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>

#include <stddef.h>

#include "boxedwine_x64_x11_bridge.h"

#if !defined(__x86_64__)
#error "the layout check describes the x86-64 SysV ABI"
#endif

#define BW_PRIV_DISPLAY __typeof__(*(_XPrivDisplay)0)

#define BW_OFFSET(type, field, expected) \
    _Static_assert(offsetof(type, field) == (expected), #type "." #field " offset")
#define BW_SIZE(type, expected) \
    _Static_assert(sizeof(type) == (expected), #type " size")

/* Display: the public prefix Xlib's macros read. */
BW_OFFSET(BW_PRIV_DISPLAY, ext_data, 0);
BW_OFFSET(BW_PRIV_DISPLAY, fd, 16);
BW_OFFSET(BW_PRIV_DISPLAY, proto_major_version, 24);
BW_OFFSET(BW_PRIV_DISPLAY, proto_minor_version, 28);
BW_OFFSET(BW_PRIV_DISPLAY, vendor, 32);
BW_OFFSET(BW_PRIV_DISPLAY, resource_alloc, 72);
BW_OFFSET(BW_PRIV_DISPLAY, byte_order, 80);
BW_OFFSET(BW_PRIV_DISPLAY, bitmap_unit, 84);
BW_OFFSET(BW_PRIV_DISPLAY, bitmap_pad, 88);
BW_OFFSET(BW_PRIV_DISPLAY, bitmap_bit_order, 92);
BW_OFFSET(BW_PRIV_DISPLAY, nformats, 96);
BW_OFFSET(BW_PRIV_DISPLAY, pixmap_format, 104);
BW_OFFSET(BW_PRIV_DISPLAY, release, 116);
BW_OFFSET(BW_PRIV_DISPLAY, qlen, 136);
BW_OFFSET(BW_PRIV_DISPLAY, last_request_read, 144);
BW_OFFSET(BW_PRIV_DISPLAY, request, 152);
BW_OFFSET(BW_PRIV_DISPLAY, max_request_size, 192);
BW_OFFSET(BW_PRIV_DISPLAY, db, 200);
BW_OFFSET(BW_PRIV_DISPLAY, display_name, 216);
BW_OFFSET(BW_PRIV_DISPLAY, default_screen, 224);
BW_OFFSET(BW_PRIV_DISPLAY, nscreens, 228);
BW_OFFSET(BW_PRIV_DISPLAY, screens, 232);
BW_OFFSET(BW_PRIV_DISPLAY, motion_buffer, 240);
BW_OFFSET(BW_PRIV_DISPLAY, min_keycode, 256);
BW_OFFSET(BW_PRIV_DISPLAY, max_keycode, 260);
BW_OFFSET(BW_PRIV_DISPLAY, xdefaults, 288);
BW_SIZE(BW_PRIV_DISPLAY, 296);
_Static_assert(BOXEDWINE_X64_X11_DISPLAY_ID_OFFSET >= sizeof(BW_PRIV_DISPLAY),
               "the private display id must follow the public Display");
_Static_assert(BOXEDWINE_X64_X11_DISPLAY_PRIVATE_BYTES >= BOXEDWINE_X64_X11_DISPLAY_ID_OFFSET + 8,
               "the private tail holds the id and the server socket");

/* Screen */
BW_OFFSET(Screen, ext_data, 0);
BW_OFFSET(Screen, display, 8);
BW_OFFSET(Screen, root, 16);
BW_OFFSET(Screen, width, 24);
BW_OFFSET(Screen, height, 28);
BW_OFFSET(Screen, mwidth, 32);
BW_OFFSET(Screen, mheight, 36);
BW_OFFSET(Screen, ndepths, 40);
BW_OFFSET(Screen, depths, 48);
BW_OFFSET(Screen, root_depth, 56);
BW_OFFSET(Screen, root_visual, 64);
BW_OFFSET(Screen, default_gc, 72);
BW_OFFSET(Screen, cmap, 80);
BW_OFFSET(Screen, white_pixel, 88);
BW_OFFSET(Screen, black_pixel, 96);
BW_OFFSET(Screen, max_maps, 104);
BW_OFFSET(Screen, min_maps, 108);
BW_OFFSET(Screen, backing_store, 112);
BW_OFFSET(Screen, save_unders, 116);
BW_OFFSET(Screen, root_input_mask, 120);
BW_SIZE(Screen, 128);

/* Depth, Visual, ScreenFormat */
BW_OFFSET(Depth, depth, 0);
BW_OFFSET(Depth, nvisuals, 4);
BW_OFFSET(Depth, visuals, 8);
BW_SIZE(Depth, 16);
BW_OFFSET(Visual, ext_data, 0);
BW_OFFSET(Visual, visualid, 8);
BW_OFFSET(Visual, class, 16);
BW_OFFSET(Visual, red_mask, 24);
BW_OFFSET(Visual, green_mask, 32);
BW_OFFSET(Visual, blue_mask, 40);
BW_OFFSET(Visual, bits_per_rgb, 48);
BW_OFFSET(Visual, map_entries, 52);
BW_SIZE(Visual, 56);
BW_OFFSET(ScreenFormat, ext_data, 0);
BW_OFFSET(ScreenFormat, depth, 8);
BW_OFFSET(ScreenFormat, bits_per_pixel, 12);
BW_OFFSET(ScreenFormat, scanline_pad, 16);
BW_SIZE(ScreenFormat, 24);

/* XVisualInfo */
BW_OFFSET(XVisualInfo, visual, 0);
BW_OFFSET(XVisualInfo, visualid, 8);
BW_OFFSET(XVisualInfo, screen, 16);
BW_OFFSET(XVisualInfo, depth, 20);
BW_OFFSET(XVisualInfo, class, 24);
BW_OFFSET(XVisualInfo, red_mask, 32);
BW_OFFSET(XVisualInfo, green_mask, 40);
BW_OFFSET(XVisualInfo, blue_mask, 48);
BW_OFFSET(XVisualInfo, colormap_size, 56);
BW_OFFSET(XVisualInfo, bits_per_rgb, 60);
BW_SIZE(XVisualInfo, 64);

/* Window attributes */
BW_OFFSET(XSetWindowAttributes, background_pixmap, 0);
BW_OFFSET(XSetWindowAttributes, background_pixel, 8);
BW_OFFSET(XSetWindowAttributes, border_pixmap, 16);
BW_OFFSET(XSetWindowAttributes, border_pixel, 24);
BW_OFFSET(XSetWindowAttributes, bit_gravity, 32);
BW_OFFSET(XSetWindowAttributes, win_gravity, 36);
BW_OFFSET(XSetWindowAttributes, backing_store, 40);
BW_OFFSET(XSetWindowAttributes, backing_planes, 48);
BW_OFFSET(XSetWindowAttributes, backing_pixel, 56);
BW_OFFSET(XSetWindowAttributes, save_under, 64);
BW_OFFSET(XSetWindowAttributes, event_mask, 72);
BW_OFFSET(XSetWindowAttributes, do_not_propagate_mask, 80);
BW_OFFSET(XSetWindowAttributes, override_redirect, 88);
BW_OFFSET(XSetWindowAttributes, colormap, 96);
BW_OFFSET(XSetWindowAttributes, cursor, 104);
BW_SIZE(XSetWindowAttributes, 112);
BW_OFFSET(XWindowAttributes, x, 0);
BW_OFFSET(XWindowAttributes, width, 8);
BW_OFFSET(XWindowAttributes, border_width, 16);
BW_OFFSET(XWindowAttributes, depth, 20);
BW_OFFSET(XWindowAttributes, visual, 24);
BW_OFFSET(XWindowAttributes, root, 32);
BW_OFFSET(XWindowAttributes, class, 40);
BW_OFFSET(XWindowAttributes, bit_gravity, 44);
BW_OFFSET(XWindowAttributes, win_gravity, 48);
BW_OFFSET(XWindowAttributes, backing_store, 52);
BW_OFFSET(XWindowAttributes, backing_planes, 56);
BW_OFFSET(XWindowAttributes, backing_pixel, 64);
BW_OFFSET(XWindowAttributes, save_under, 72);
BW_OFFSET(XWindowAttributes, colormap, 80);
BW_OFFSET(XWindowAttributes, map_installed, 88);
BW_OFFSET(XWindowAttributes, map_state, 92);
BW_OFFSET(XWindowAttributes, all_event_masks, 96);
BW_OFFSET(XWindowAttributes, your_event_mask, 104);
BW_OFFSET(XWindowAttributes, do_not_propagate_mask, 112);
BW_OFFSET(XWindowAttributes, override_redirect, 120);
BW_OFFSET(XWindowAttributes, screen, 128);
BW_SIZE(XWindowAttributes, 136);
BW_OFFSET(XWindowChanges, x, 0);
BW_OFFSET(XWindowChanges, border_width, 16);
BW_OFFSET(XWindowChanges, sibling, 24);
BW_OFFSET(XWindowChanges, stack_mode, 32);
BW_SIZE(XWindowChanges, 40);

/* Hints and properties */
BW_OFFSET(XSizeHints, flags, 0);
BW_OFFSET(XSizeHints, x, 8);
BW_OFFSET(XSizeHints, min_width, 24);
BW_OFFSET(XSizeHints, min_aspect, 48);
BW_OFFSET(XSizeHints, base_width, 64);
BW_OFFSET(XSizeHints, win_gravity, 72);
BW_SIZE(XSizeHints, 80);
BW_OFFSET(XWMHints, flags, 0);
BW_OFFSET(XWMHints, input, 8);
BW_OFFSET(XWMHints, initial_state, 12);
BW_OFFSET(XWMHints, icon_pixmap, 16);
BW_OFFSET(XWMHints, icon_window, 24);
BW_OFFSET(XWMHints, icon_x, 32);
BW_OFFSET(XWMHints, icon_y, 36);
BW_OFFSET(XWMHints, icon_mask, 40);
BW_OFFSET(XWMHints, window_group, 48);
BW_SIZE(XWMHints, 56);
BW_OFFSET(XTextProperty, value, 0);
BW_OFFSET(XTextProperty, encoding, 8);
BW_OFFSET(XTextProperty, format, 16);
BW_OFFSET(XTextProperty, nitems, 24);
BW_SIZE(XTextProperty, 32);
BW_OFFSET(XClassHint, res_name, 0);
BW_OFFSET(XClassHint, res_class, 8);

/* Colors, GCs, images, keymaps */
BW_OFFSET(XColor, pixel, 0);
BW_OFFSET(XColor, red, 8);
BW_OFFSET(XColor, green, 10);
BW_OFFSET(XColor, blue, 12);
BW_OFFSET(XColor, flags, 14);
BW_SIZE(XColor, 16);
BW_OFFSET(XGCValues, function, 0);
BW_OFFSET(XGCValues, plane_mask, 8);
BW_OFFSET(XGCValues, foreground, 16);
BW_OFFSET(XGCValues, background, 24);
BW_OFFSET(XGCValues, line_width, 32);
BW_OFFSET(XGCValues, arc_mode, 56);
BW_OFFSET(XGCValues, tile, 64);
BW_OFFSET(XGCValues, stipple, 72);
BW_OFFSET(XGCValues, ts_x_origin, 80);
BW_OFFSET(XGCValues, font, 88);
BW_OFFSET(XGCValues, subwindow_mode, 96);
BW_OFFSET(XGCValues, graphics_exposures, 100);
BW_OFFSET(XGCValues, clip_x_origin, 104);
BW_OFFSET(XGCValues, clip_mask, 112);
BW_OFFSET(XGCValues, dash_offset, 120);
BW_OFFSET(XGCValues, dashes, 124);
BW_SIZE(XGCValues, 128);
BW_OFFSET(XImage, width, 0);
BW_OFFSET(XImage, data, 16);
BW_OFFSET(XImage, byte_order, 24);
BW_OFFSET(XImage, depth, 40);
BW_OFFSET(XImage, bytes_per_line, 44);
BW_OFFSET(XImage, bits_per_pixel, 48);
BW_OFFSET(XImage, red_mask, 56);
BW_OFFSET(XImage, obdata, 80);
BW_OFFSET(XImage, f, 88);
BW_SIZE(XImage, 136);
BW_OFFSET(XModifierKeymap, max_keypermod, 0);
BW_OFFSET(XModifierKeymap, modifiermap, 8);
BW_SIZE(XModifierKeymap, 16);
BW_SIZE(XRectangle, 8);
BW_SIZE(XPixmapFormatValues, 12);

/* Events */
BW_SIZE(XEvent, 192);
BW_OFFSET(XAnyEvent, serial, 8);
BW_OFFSET(XAnyEvent, send_event, 16);
BW_OFFSET(XAnyEvent, display, 24);
BW_OFFSET(XAnyEvent, window, 32);
BW_OFFSET(XKeyEvent, root, 40);
BW_OFFSET(XKeyEvent, subwindow, 48);
BW_OFFSET(XKeyEvent, time, 56);
BW_OFFSET(XKeyEvent, x, 64);
BW_OFFSET(XKeyEvent, x_root, 72);
BW_OFFSET(XKeyEvent, state, 80);
BW_OFFSET(XKeyEvent, keycode, 84);
BW_OFFSET(XKeyEvent, same_screen, 88);
BW_OFFSET(XButtonEvent, button, 84);
BW_OFFSET(XMotionEvent, is_hint, 84);
BW_OFFSET(XCrossingEvent, mode, 80);
BW_OFFSET(XCrossingEvent, detail, 84);
BW_OFFSET(XCrossingEvent, focus, 92);
BW_OFFSET(XCrossingEvent, state, 96);
BW_OFFSET(XFocusChangeEvent, mode, 40);
BW_OFFSET(XKeymapEvent, key_vector, 40);
BW_OFFSET(XExposeEvent, x, 40);
BW_OFFSET(XExposeEvent, count, 56);
BW_OFFSET(XGraphicsExposeEvent, major_code, 60);
BW_OFFSET(XNoExposeEvent, major_code, 40);
BW_OFFSET(XVisibilityEvent, state, 40);
BW_OFFSET(XCreateWindowEvent, window, 40);
BW_OFFSET(XCreateWindowEvent, x, 48);
BW_OFFSET(XCreateWindowEvent, border_width, 64);
BW_OFFSET(XCreateWindowEvent, override_redirect, 68);
BW_OFFSET(XDestroyWindowEvent, window, 40);
BW_OFFSET(XUnmapEvent, from_configure, 48);
BW_OFFSET(XMapEvent, override_redirect, 48);
BW_OFFSET(XReparentEvent, parent, 48);
BW_OFFSET(XReparentEvent, x, 56);
BW_OFFSET(XReparentEvent, override_redirect, 64);
BW_OFFSET(XConfigureEvent, window, 40);
BW_OFFSET(XConfigureEvent, x, 48);
BW_OFFSET(XConfigureEvent, border_width, 64);
BW_OFFSET(XConfigureEvent, above, 72);
BW_OFFSET(XConfigureEvent, override_redirect, 80);
BW_OFFSET(XConfigureRequestEvent, above, 72);
BW_OFFSET(XConfigureRequestEvent, detail, 80);
BW_OFFSET(XConfigureRequestEvent, value_mask, 88);
BW_OFFSET(XGravityEvent, x, 48);
BW_OFFSET(XResizeRequestEvent, width, 40);
BW_OFFSET(XCirculateEvent, place, 48);
BW_OFFSET(XPropertyEvent, atom, 40);
BW_OFFSET(XPropertyEvent, time, 48);
BW_OFFSET(XPropertyEvent, state, 56);
BW_OFFSET(XSelectionClearEvent, selection, 40);
BW_OFFSET(XSelectionRequestEvent, requestor, 40);
BW_OFFSET(XSelectionRequestEvent, time, 72);
BW_OFFSET(XSelectionEvent, requestor, 32);
BW_OFFSET(XSelectionEvent, time, 64);
BW_OFFSET(XColormapEvent, colormap, 40);
BW_OFFSET(XColormapEvent, state, 52);
BW_OFFSET(XClientMessageEvent, message_type, 40);
BW_OFFSET(XClientMessageEvent, format, 48);
BW_OFFSET(XClientMessageEvent, data, 56);
BW_OFFSET(XMappingEvent, request, 40);
BW_OFFSET(XMappingEvent, count, 48);
BW_OFFSET(XGenericEvent, extension, 32);
BW_OFFSET(XGenericEvent, evtype, 36);
BW_OFFSET(XGenericEventCookie, cookie, 40);
BW_OFFSET(XGenericEventCookie, data, 48);

/* Xinerama, for when the bridge offers screens. */
BW_OFFSET(XineramaScreenInfo, screen_number, 0);
BW_OFFSET(XineramaScreenInfo, x_org, 4);
BW_OFFSET(XineramaScreenInfo, width, 8);
BW_SIZE(XineramaScreenInfo, 12);

/* Xrandr. tools/x11-64/xrandr.c declares these structures itself, because
 * the builder needs no libxrandr-dev to produce the library -- but winex11.so
 * was compiled against the real header, so the two must agree byte for byte.
 * When the builder does have the header, the numbers the shim asserts against
 * its own definitions are asserted here against the real ones. */
#if defined(__has_include)
#if __has_include(<X11/extensions/Xrandr.h>)
#include <X11/extensions/Xrandr.h>
#define BW_HAVE_XRANDR_HEADER 1
#endif
#endif

#ifdef BW_HAVE_XRANDR_HEADER
BW_OFFSET(XRRScreenSize, width, 0);
BW_OFFSET(XRRScreenSize, height, 4);
BW_OFFSET(XRRScreenSize, mwidth, 8);
BW_OFFSET(XRRScreenSize, mheight, 12);
BW_SIZE(XRRScreenSize, 16);

BW_OFFSET(XRRModeInfo, id, 0);
BW_OFFSET(XRRModeInfo, width, 8);
BW_OFFSET(XRRModeInfo, height, 12);
BW_OFFSET(XRRModeInfo, dotClock, 16);
BW_OFFSET(XRRModeInfo, hSyncStart, 24);
BW_OFFSET(XRRModeInfo, hTotal, 32);
BW_OFFSET(XRRModeInfo, vSyncStart, 40);
BW_OFFSET(XRRModeInfo, vTotal, 48);
BW_OFFSET(XRRModeInfo, name, 56);
BW_OFFSET(XRRModeInfo, nameLength, 64);
BW_OFFSET(XRRModeInfo, modeFlags, 72);
BW_SIZE(XRRModeInfo, 80);

BW_OFFSET(XRRScreenResources, timestamp, 0);
BW_OFFSET(XRRScreenResources, configTimestamp, 8);
BW_OFFSET(XRRScreenResources, ncrtc, 16);
BW_OFFSET(XRRScreenResources, crtcs, 24);
BW_OFFSET(XRRScreenResources, noutput, 32);
BW_OFFSET(XRRScreenResources, outputs, 40);
BW_OFFSET(XRRScreenResources, nmode, 48);
BW_OFFSET(XRRScreenResources, modes, 56);
BW_SIZE(XRRScreenResources, 64);

BW_OFFSET(XRROutputInfo, timestamp, 0);
BW_OFFSET(XRROutputInfo, crtc, 8);
BW_OFFSET(XRROutputInfo, name, 16);
BW_OFFSET(XRROutputInfo, nameLen, 24);
BW_OFFSET(XRROutputInfo, mm_width, 32);
BW_OFFSET(XRROutputInfo, mm_height, 40);
BW_OFFSET(XRROutputInfo, connection, 48);
BW_OFFSET(XRROutputInfo, subpixel_order, 50);
BW_OFFSET(XRROutputInfo, ncrtc, 52);
BW_OFFSET(XRROutputInfo, crtcs, 56);
BW_OFFSET(XRROutputInfo, nclone, 64);
BW_OFFSET(XRROutputInfo, clones, 72);
BW_OFFSET(XRROutputInfo, nmode, 80);
BW_OFFSET(XRROutputInfo, npreferred, 84);
BW_OFFSET(XRROutputInfo, modes, 88);
BW_SIZE(XRROutputInfo, 96);

BW_OFFSET(XRRCrtcInfo, timestamp, 0);
BW_OFFSET(XRRCrtcInfo, x, 8);
BW_OFFSET(XRRCrtcInfo, y, 12);
BW_OFFSET(XRRCrtcInfo, width, 16);
BW_OFFSET(XRRCrtcInfo, height, 20);
BW_OFFSET(XRRCrtcInfo, mode, 24);
BW_OFFSET(XRRCrtcInfo, rotation, 32);
BW_OFFSET(XRRCrtcInfo, noutput, 36);
BW_OFFSET(XRRCrtcInfo, outputs, 40);
BW_OFFSET(XRRCrtcInfo, rotations, 48);
BW_OFFSET(XRRCrtcInfo, npossible, 52);
BW_OFFSET(XRRCrtcInfo, possible, 56);
BW_SIZE(XRRCrtcInfo, 64);

BW_OFFSET(XRRProviderResources, timestamp, 0);
BW_OFFSET(XRRProviderResources, nproviders, 8);
BW_OFFSET(XRRProviderResources, providers, 16);
BW_SIZE(XRRProviderResources, 24);

/* Wine's get_frequency divides the dot clock by hTotal*vTotal, so the flags
 * that double or halve it must be the values the shim leaves clear. */
_Static_assert(RR_Interlace == 0x0010, "RR_Interlace");
_Static_assert(RR_DoubleScan == 0x0020, "RR_DoubleScan");
_Static_assert(RR_Rotate_0 == 1, "RR_Rotate_0");
_Static_assert(RR_Connected == 0, "RR_Connected");
_Static_assert(RRSetConfigSuccess == 0, "RRSetConfigSuccess");
_Static_assert(RRSetConfigFailed == 3, "RRSetConfigFailed");
#endif

/* The bridge ABI itself. */
_Static_assert(BOXEDWINE_X64_X11_OP_COUNT < 256, "the operation table stays small");
_Static_assert(BOXEDWINE_X64_X11_MAX_ARGS == 16, "zero through fifteen arguments");

/* The RandR record the host writes and the shim reads: one layout on both
 * sides, so every field is a uint32_t and nothing is padded. */
_Static_assert(sizeof(struct boxedwine_x64_x11_randr_mode) == 16, "randr mode record");
_Static_assert(sizeof(struct boxedwine_x64_x11_randr_state) == 48, "randr state record");
_Static_assert(offsetof(struct boxedwine_x64_x11_randr_state, modeCount) == 4, "randr modeCount");
_Static_assert(offsetof(struct boxedwine_x64_x11_randr_state, currentMode) == 20, "randr currentMode");

int boxedwine_x64_x11_layout_check_ok = 1;
