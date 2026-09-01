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

// x86-64 Xlib structure layouts, as the guest sees them.
//
// The existing X server code models every structure with the IA-32 Xlib
// layout (4-byte pointers and longs). A 64-bit Wine reads Display, Screen,
// Visual and XEvent fields directly through Xlib's macros, so the x86-64
// bridge writes those structures in the exact SysV x86-64 layout instead.
// Nothing here casts the 32-bit structures to 64-bit ones or truncates a
// guest pointer: every field is placed by offset.
//
// The numbers are asserted twice: here, against the sizes the SysV ABI
// mandates, and in the guest shim (tools/x11-64/layout_check.c), against the
// exact X11 headers it is compiled with. Both must agree before a build ships.
//
// This header has no BoxedWine dependencies so it can be unit tested on any
// host.

#ifndef __X11_LAYOUT64_H__
#define __X11_LAYOUT64_H__

#include <cstdint>
#include <cstring>

namespace x11layout64 {

// ---- struct _XDisplay (public part, Xlib.h) ---------------------------
namespace Display {
    constexpr uint32_t ext_data = 0;             // XExtData*
    constexpr uint32_t private1 = 8;             // struct _XPrivate*
    constexpr uint32_t fd = 16;                  // int
    constexpr uint32_t private2 = 20;            // int
    constexpr uint32_t proto_major_version = 24; // int
    constexpr uint32_t proto_minor_version = 28; // int
    constexpr uint32_t vendor = 32;              // char*
    constexpr uint32_t private3 = 40;            // XID
    constexpr uint32_t private4 = 48;            // XID
    constexpr uint32_t private5 = 56;            // XID
    constexpr uint32_t private6 = 64;            // int
    constexpr uint32_t resource_alloc = 72;      // XID (*)(struct _XDisplay*)
    constexpr uint32_t byte_order = 80;          // int
    constexpr uint32_t bitmap_unit = 84;         // int
    constexpr uint32_t bitmap_pad = 88;          // int
    constexpr uint32_t bitmap_bit_order = 92;    // int
    constexpr uint32_t nformats = 96;            // int
    constexpr uint32_t pixmap_format = 104;      // ScreenFormat*
    constexpr uint32_t private8 = 112;           // int
    constexpr uint32_t release = 116;            // int
    constexpr uint32_t private9 = 120;           // struct _XPrivate*
    constexpr uint32_t private10 = 128;          // struct _XPrivate*
    constexpr uint32_t qlen = 136;               // int
    constexpr uint32_t last_request_read = 144;  // unsigned long
    constexpr uint32_t request = 152;            // unsigned long
    constexpr uint32_t private11 = 160;          // XPointer
    constexpr uint32_t private12 = 168;          // XPointer
    constexpr uint32_t private13 = 176;          // XPointer
    constexpr uint32_t private14 = 184;          // XPointer
    constexpr uint32_t max_request_size = 192;   // unsigned
    constexpr uint32_t db = 200;                 // struct _XrmHashBucketRec*
    constexpr uint32_t private15 = 208;          // int (*)(struct _XDisplay*)
    constexpr uint32_t display_name = 216;       // char*
    constexpr uint32_t default_screen = 224;     // int
    constexpr uint32_t nscreens = 228;           // int
    constexpr uint32_t screens = 232;            // Screen*
    constexpr uint32_t motion_buffer = 240;      // unsigned long
    constexpr uint32_t private16 = 248;          // unsigned long
    constexpr uint32_t min_keycode = 256;        // int
    constexpr uint32_t max_keycode = 260;        // int
    constexpr uint32_t private17 = 264;          // XPointer
    constexpr uint32_t private18 = 272;          // XPointer
    constexpr uint32_t private19 = 280;          // int
    constexpr uint32_t xdefaults = 288;          // char*
    constexpr uint32_t size = 296;
}

// ---- Screen (Xlib.h) -----------------------------------------------------
namespace Screen {
    constexpr uint32_t ext_data = 0;        // XExtData*
    constexpr uint32_t display = 8;         // struct _XDisplay*
    constexpr uint32_t root = 16;           // Window
    constexpr uint32_t width = 24;          // int
    constexpr uint32_t height = 28;         // int
    constexpr uint32_t mwidth = 32;         // int
    constexpr uint32_t mheight = 36;        // int
    constexpr uint32_t ndepths = 40;        // int
    constexpr uint32_t depths = 48;         // Depth*
    constexpr uint32_t root_depth = 56;     // int
    constexpr uint32_t root_visual = 64;    // Visual*
    constexpr uint32_t default_gc = 72;     // GC
    constexpr uint32_t cmap = 80;           // Colormap
    constexpr uint32_t white_pixel = 88;    // unsigned long
    constexpr uint32_t black_pixel = 96;    // unsigned long
    constexpr uint32_t max_maps = 104;      // int
    constexpr uint32_t min_maps = 108;      // int
    constexpr uint32_t backing_store = 112; // int
    constexpr uint32_t save_unders = 116;   // Bool
    constexpr uint32_t root_input_mask = 120; // long
    constexpr uint32_t size = 128;
}

// ---- Depth (Xlib.h) ------------------------------------------------------
namespace Depth {
    constexpr uint32_t depth = 0;    // int
    constexpr uint32_t nvisuals = 4; // int
    constexpr uint32_t visuals = 8;  // Visual*
    constexpr uint32_t size = 16;
}

// ---- Visual (Xlib.h) -----------------------------------------------------
namespace Visual {
    constexpr uint32_t ext_data = 0;      // XExtData*
    constexpr uint32_t visualid = 8;      // VisualID (unsigned long)
    constexpr uint32_t c_class = 16;      // int
    constexpr uint32_t red_mask = 24;     // unsigned long
    constexpr uint32_t green_mask = 32;   // unsigned long
    constexpr uint32_t blue_mask = 40;    // unsigned long
    constexpr uint32_t bits_per_rgb = 48; // int
    constexpr uint32_t map_entries = 52;  // int
    constexpr uint32_t size = 56;
}

// ---- ScreenFormat (Xlib.h) ----------------------------------------------
namespace ScreenFormat {
    constexpr uint32_t ext_data = 0;        // XExtData*
    constexpr uint32_t depth = 8;           // int
    constexpr uint32_t bits_per_pixel = 12; // int
    constexpr uint32_t scanline_pad = 16;   // int
    constexpr uint32_t size = 24;
}

// ---- XVisualInfo (Xutil.h) -----------------------------------------------
namespace XVisualInfo {
    constexpr uint32_t visual = 0;         // Visual*
    constexpr uint32_t visualid = 8;       // VisualID
    constexpr uint32_t screen = 16;        // int
    constexpr uint32_t depth = 20;         // int
    constexpr uint32_t c_class = 24;       // int
    constexpr uint32_t red_mask = 32;      // unsigned long
    constexpr uint32_t green_mask = 40;    // unsigned long
    constexpr uint32_t blue_mask = 48;     // unsigned long
    constexpr uint32_t colormap_size = 56; // int
    constexpr uint32_t bits_per_rgb = 60;  // int
    constexpr uint32_t size = 64;
}

// ---- XSetWindowAttributes (Xlib.h) ---------------------------------------
namespace XSetWindowAttributes {
    constexpr uint32_t background_pixmap = 0;      // Pixmap
    constexpr uint32_t background_pixel = 8;       // unsigned long
    constexpr uint32_t border_pixmap = 16;         // Pixmap
    constexpr uint32_t border_pixel = 24;          // unsigned long
    constexpr uint32_t bit_gravity = 32;           // int
    constexpr uint32_t win_gravity = 36;           // int
    constexpr uint32_t backing_store = 40;         // int
    constexpr uint32_t backing_planes = 48;        // unsigned long
    constexpr uint32_t backing_pixel = 56;         // unsigned long
    constexpr uint32_t save_under = 64;            // Bool
    constexpr uint32_t event_mask = 72;            // long
    constexpr uint32_t do_not_propagate_mask = 80; // long
    constexpr uint32_t override_redirect = 88;     // Bool
    constexpr uint32_t colormap = 96;              // Colormap
    constexpr uint32_t cursor = 104;               // Cursor
    constexpr uint32_t size = 112;
}

// ---- XWindowAttributes (Xlib.h) ------------------------------------------
namespace XWindowAttributes {
    constexpr uint32_t x = 0;                       // int
    constexpr uint32_t y = 4;                       // int
    constexpr uint32_t width = 8;                   // int
    constexpr uint32_t height = 12;                 // int
    constexpr uint32_t border_width = 16;           // int
    constexpr uint32_t depth = 20;                  // int
    constexpr uint32_t visual = 24;                 // Visual*
    constexpr uint32_t root = 32;                   // Window
    constexpr uint32_t c_class = 40;                // int
    constexpr uint32_t bit_gravity = 44;            // int
    constexpr uint32_t win_gravity = 48;            // int
    constexpr uint32_t backing_store = 52;          // int
    constexpr uint32_t backing_planes = 56;         // unsigned long
    constexpr uint32_t backing_pixel = 64;          // unsigned long
    constexpr uint32_t save_under = 72;             // Bool
    constexpr uint32_t colormap = 80;               // Colormap
    constexpr uint32_t map_installed = 88;          // Bool
    constexpr uint32_t map_state = 92;              // int
    constexpr uint32_t all_event_masks = 96;        // long
    constexpr uint32_t your_event_mask = 104;       // long
    constexpr uint32_t do_not_propagate_mask = 112; // long
    constexpr uint32_t override_redirect = 120;     // Bool
    constexpr uint32_t screen = 128;                // Screen*
    constexpr uint32_t size = 136;
}

// ---- XWindowChanges (Xlib.h) ---------------------------------------------
namespace XWindowChanges {
    constexpr uint32_t x = 0;             // int
    constexpr uint32_t y = 4;             // int
    constexpr uint32_t width = 8;         // int
    constexpr uint32_t height = 12;       // int
    constexpr uint32_t border_width = 16; // int
    constexpr uint32_t sibling = 24;      // Window
    constexpr uint32_t stack_mode = 32;   // int
    constexpr uint32_t size = 40;
}

// ---- XSizeHints (Xutil.h) ------------------------------------------------
namespace XSizeHints {
    constexpr uint32_t flags = 0;         // long
    constexpr uint32_t x = 8;             // int
    constexpr uint32_t y = 12;
    constexpr uint32_t width = 16;
    constexpr uint32_t height = 20;
    constexpr uint32_t min_width = 24;
    constexpr uint32_t min_height = 28;
    constexpr uint32_t max_width = 32;
    constexpr uint32_t max_height = 36;
    constexpr uint32_t width_inc = 40;
    constexpr uint32_t height_inc = 44;
    constexpr uint32_t min_aspect_x = 48;
    constexpr uint32_t min_aspect_y = 52;
    constexpr uint32_t max_aspect_x = 56;
    constexpr uint32_t max_aspect_y = 60;
    constexpr uint32_t base_width = 64;
    constexpr uint32_t base_height = 68;
    constexpr uint32_t win_gravity = 72;
    constexpr uint32_t size = 80;
}

// ---- XWMHints (Xutil.h) --------------------------------------------------
namespace XWMHints {
    constexpr uint32_t flags = 0;          // long
    constexpr uint32_t input = 8;          // Bool
    constexpr uint32_t initial_state = 12; // int
    constexpr uint32_t icon_pixmap = 16;   // Pixmap
    constexpr uint32_t icon_window = 24;   // Window
    constexpr uint32_t icon_x = 32;        // int
    constexpr uint32_t icon_y = 36;        // int
    constexpr uint32_t icon_mask = 40;     // Pixmap
    constexpr uint32_t window_group = 48;  // XID
    constexpr uint32_t size = 56;
}

// ---- XTextProperty (Xutil.h) ---------------------------------------------
namespace XTextProperty {
    constexpr uint32_t value = 0;     // unsigned char*
    constexpr uint32_t encoding = 8;  // Atom
    constexpr uint32_t format = 16;   // int
    constexpr uint32_t nitems = 24;   // unsigned long
    constexpr uint32_t size = 32;
}

// ---- XColor (Xlib.h) -----------------------------------------------------
namespace XColor {
    constexpr uint32_t pixel = 0;  // unsigned long
    constexpr uint32_t red = 8;    // unsigned short
    constexpr uint32_t green = 10;
    constexpr uint32_t blue = 12;
    constexpr uint32_t flags = 14; // char
    constexpr uint32_t size = 16;
}

// ---- XGCValues (Xlib.h) --------------------------------------------------
namespace XGCValues {
    constexpr uint32_t function = 0;            // int
    constexpr uint32_t plane_mask = 8;          // unsigned long
    constexpr uint32_t foreground = 16;         // unsigned long
    constexpr uint32_t background = 24;         // unsigned long
    constexpr uint32_t line_width = 32;         // int
    constexpr uint32_t line_style = 36;
    constexpr uint32_t cap_style = 40;
    constexpr uint32_t join_style = 44;
    constexpr uint32_t fill_style = 48;
    constexpr uint32_t fill_rule = 52;
    constexpr uint32_t arc_mode = 56;
    constexpr uint32_t tile = 64;               // Pixmap
    constexpr uint32_t stipple = 72;            // Pixmap
    constexpr uint32_t ts_x_origin = 80;        // int
    constexpr uint32_t ts_y_origin = 84;
    constexpr uint32_t font = 88;               // Font
    constexpr uint32_t subwindow_mode = 96;     // int
    constexpr uint32_t graphics_exposures = 100; // Bool
    constexpr uint32_t clip_x_origin = 104;
    constexpr uint32_t clip_y_origin = 108;
    constexpr uint32_t clip_mask = 112;         // Pixmap
    constexpr uint32_t dash_offset = 120;       // int
    constexpr uint32_t dashes = 124;            // char
    constexpr uint32_t size = 128;
}

// ---- XImage (Xlib.h) -----------------------------------------------------
namespace XImage {
    constexpr uint32_t width = 0;
    constexpr uint32_t height = 4;
    constexpr uint32_t xoffset = 8;
    constexpr uint32_t format = 12;
    constexpr uint32_t data = 16;             // char*
    constexpr uint32_t byte_order = 24;
    constexpr uint32_t bitmap_unit = 28;
    constexpr uint32_t bitmap_bit_order = 32;
    constexpr uint32_t bitmap_pad = 36;
    constexpr uint32_t depth = 40;
    constexpr uint32_t bytes_per_line = 44;
    constexpr uint32_t bits_per_pixel = 48;
    constexpr uint32_t red_mask = 56;         // unsigned long
    constexpr uint32_t green_mask = 64;
    constexpr uint32_t blue_mask = 72;
    constexpr uint32_t obdata = 80;           // XPointer
    constexpr uint32_t f_create_image = 88;   // six function pointers
    constexpr uint32_t size = 136;
}

// ---- XModifierKeymap (Xlib.h) --------------------------------------------
namespace XModifierKeymap {
    constexpr uint32_t max_keypermod = 0; // int
    constexpr uint32_t modifiermap = 8;   // KeyCode*
    constexpr uint32_t size = 16;
}

// ---- XRectangle / XPoint (identical on both ABIs) -----------------------
namespace XRectangle {
    constexpr uint32_t x = 0;      // short
    constexpr uint32_t y = 2;
    constexpr uint32_t width = 4;  // unsigned short
    constexpr uint32_t height = 6;
    constexpr uint32_t size = 8;
}

// ---- XEvent (Xlib.h) -----------------------------------------------------
//
// Every event starts with the XAnyEvent prefix. The union is 24 longs.
namespace XEvent {
    constexpr uint32_t size = 192;

    namespace any {
        constexpr uint32_t type = 0;        // int
        constexpr uint32_t serial = 8;      // unsigned long
        constexpr uint32_t send_event = 16; // Bool
        constexpr uint32_t display = 24;    // Display*
        constexpr uint32_t window = 32;     // Window
    }
    namespace key {                         // XKeyEvent / XButtonEvent / XMotionEvent
        constexpr uint32_t root = 40;       // Window
        constexpr uint32_t subwindow = 48;  // Window
        constexpr uint32_t time = 56;       // Time
        constexpr uint32_t x = 64;
        constexpr uint32_t y = 68;
        constexpr uint32_t x_root = 72;
        constexpr uint32_t y_root = 76;
        constexpr uint32_t state = 80;      // unsigned int
        constexpr uint32_t keycode = 84;    // unsigned int (button for XButtonEvent)
        constexpr uint32_t is_hint = 84;    // char (XMotionEvent)
        constexpr uint32_t same_screen = 88; // Bool
    }
    namespace crossing {
        constexpr uint32_t mode = 80;
        constexpr uint32_t detail = 84;
        constexpr uint32_t same_screen = 88;
        constexpr uint32_t focus = 92;
        constexpr uint32_t state = 96;      // unsigned int
    }
    namespace focus {
        constexpr uint32_t mode = 40;
        constexpr uint32_t detail = 44;
    }
    namespace keymap {
        constexpr uint32_t key_vector = 40; // char[32]
    }
    namespace expose {
        constexpr uint32_t x = 40;
        constexpr uint32_t y = 44;
        constexpr uint32_t width = 48;
        constexpr uint32_t height = 52;
        constexpr uint32_t count = 56;
        constexpr uint32_t major_code = 60; // XGraphicsExposeEvent
        constexpr uint32_t minor_code = 64;
    }
    namespace noexpose {
        constexpr uint32_t major_code = 40;
        constexpr uint32_t minor_code = 44;
    }
    namespace visibility {
        constexpr uint32_t state = 40;
    }
    namespace createwindow {
        constexpr uint32_t parent = 32;
        constexpr uint32_t window = 40;
        constexpr uint32_t x = 48;
        constexpr uint32_t y = 52;
        constexpr uint32_t width = 56;
        constexpr uint32_t height = 60;
        constexpr uint32_t border_width = 64;
        constexpr uint32_t override_redirect = 68;
    }
    namespace destroywindow {                // also XUnmapEvent / XMapEvent
        constexpr uint32_t event = 32;
        constexpr uint32_t window = 40;
        constexpr uint32_t from_configure = 48;   // XUnmapEvent
        constexpr uint32_t override_redirect = 48; // XMapEvent
    }
    namespace maprequest {                   // also XCirculateRequestEvent
        constexpr uint32_t parent = 32;
        constexpr uint32_t window = 40;
        constexpr uint32_t place = 48;
    }
    namespace reparent {
        constexpr uint32_t event = 32;
        constexpr uint32_t window = 40;
        constexpr uint32_t parent = 48;
        constexpr uint32_t x = 56;
        constexpr uint32_t y = 60;
        constexpr uint32_t override_redirect = 64;
    }
    namespace configure {
        constexpr uint32_t event = 32;
        constexpr uint32_t window = 40;
        constexpr uint32_t x = 48;
        constexpr uint32_t y = 52;
        constexpr uint32_t width = 56;
        constexpr uint32_t height = 60;
        constexpr uint32_t border_width = 64;
        constexpr uint32_t above = 72;
        constexpr uint32_t override_redirect = 80;
    }
    namespace configurerequest {
        constexpr uint32_t parent = 32;
        constexpr uint32_t window = 40;
        constexpr uint32_t x = 48;
        constexpr uint32_t y = 52;
        constexpr uint32_t width = 56;
        constexpr uint32_t height = 60;
        constexpr uint32_t border_width = 64;
        constexpr uint32_t above = 72;
        constexpr uint32_t detail = 80;
        constexpr uint32_t value_mask = 88; // unsigned long
    }
    namespace gravity {
        constexpr uint32_t event = 32;
        constexpr uint32_t window = 40;
        constexpr uint32_t x = 48;
        constexpr uint32_t y = 52;
    }
    namespace resizerequest {
        constexpr uint32_t width = 40;
        constexpr uint32_t height = 44;
    }
    namespace circulate {
        constexpr uint32_t event = 32;
        constexpr uint32_t window = 40;
        constexpr uint32_t place = 48;
    }
    namespace property {
        constexpr uint32_t atom = 40;
        constexpr uint32_t time = 48;
        constexpr uint32_t state = 56;
    }
    namespace selectionclear {
        constexpr uint32_t selection = 40;
        constexpr uint32_t time = 48;
    }
    namespace selectionrequest {
        constexpr uint32_t owner = 32;
        constexpr uint32_t requestor = 40;
        constexpr uint32_t selection = 48;
        constexpr uint32_t target = 56;
        constexpr uint32_t property = 64;
        constexpr uint32_t time = 72;
    }
    namespace selection {
        constexpr uint32_t requestor = 32;
        constexpr uint32_t selection = 40;
        constexpr uint32_t target = 48;
        constexpr uint32_t property = 56;
        constexpr uint32_t time = 64;
    }
    namespace colormap {
        constexpr uint32_t colormap = 40;
        constexpr uint32_t c_new = 48;
        constexpr uint32_t state = 52;
    }
    namespace client {
        constexpr uint32_t message_type = 40;
        constexpr uint32_t format = 48;
        constexpr uint32_t data = 56;      // union { char b[20]; short s[10]; long l[5]; }
    }
    namespace mapping {
        constexpr uint32_t request = 40;
        constexpr uint32_t first_keycode = 44;
        constexpr uint32_t count = 48;
    }
    namespace generic {                     // XGenericEvent / XGenericEventCookie
        constexpr uint32_t extension = 32;
        constexpr uint32_t evtype = 36;
        constexpr uint32_t cookie = 40;     // unsigned int
        constexpr uint32_t data = 48;       // void*
    }
}

// ---- XineramaScreenInfo (Xinerama.h) ------------------------------------
namespace XineramaScreenInfo {
    constexpr uint32_t screen_number = 0; // int
    constexpr uint32_t x_org = 4;         // short
    constexpr uint32_t y_org = 6;
    constexpr uint32_t width = 8;
    constexpr uint32_t height = 10;
    constexpr uint32_t size = 12;
}

// ---- XPixmapFormatValues (Xlib.h; identical on both ABIs) ---------------
namespace XPixmapFormatValues {
    constexpr uint32_t depth = 0;
    constexpr uint32_t bits_per_pixel = 4;
    constexpr uint32_t scanline_pad = 8;
    constexpr uint32_t size = 12;
}

// ---- Byte-buffer field helpers ------------------------------------------
//
// Structures are assembled in a host byte buffer and copied to the guest in
// one validated write, or read from the guest in one validated read and then
// decoded from the buffer. Little-endian, unaligned-safe.

inline void put32(uint8_t* buffer, uint32_t offset, uint32_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

inline void put64(uint8_t* buffer, uint32_t offset, uint64_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

inline void put16(uint8_t* buffer, uint32_t offset, uint16_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

inline uint32_t get32(const uint8_t* buffer, uint32_t offset) {
    uint32_t value;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

inline uint64_t get64(const uint8_t* buffer, uint32_t offset) {
    uint64_t value;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

inline uint16_t get16(const uint8_t* buffer, uint32_t offset) {
    uint16_t value;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

// The structures Wine reads through Xlib macros must keep their ABI sizes.
static_assert(Display::size == 296, "x86-64 Display is 296 bytes");
static_assert(Screen::size == 128, "x86-64 Screen is 128 bytes");
static_assert(Depth::size == 16, "x86-64 Depth is 16 bytes");
static_assert(Visual::size == 56, "x86-64 Visual is 56 bytes");
static_assert(XVisualInfo::size == 64, "x86-64 XVisualInfo is 64 bytes");
static_assert(XSetWindowAttributes::size == 112, "x86-64 XSetWindowAttributes is 112 bytes");
static_assert(XWindowAttributes::size == 136, "x86-64 XWindowAttributes is 136 bytes");
static_assert(XEvent::size == 192, "x86-64 XEvent is 24 longs");
static_assert(XSizeHints::size == 80, "x86-64 XSizeHints is 80 bytes");
static_assert(XWMHints::size == 56, "x86-64 XWMHints is 56 bytes");
static_assert(XGCValues::size == 128, "x86-64 XGCValues is 128 bytes");
static_assert(XImage::size == 136, "x86-64 XImage is 136 bytes");

} // namespace x11layout64

#endif
