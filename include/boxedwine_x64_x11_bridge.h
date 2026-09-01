/*
 * BoxedWine x86-64 guest X11 bridge ABI.
 *
 * C-compatible and free of BoxedWine types: it is included by the 64-bit
 * guest libX11 shim (tools/x11-64) and by the native dispatcher
 * (source/x11/x11bridge64.cpp).
 *
 * The 32-bit guest libraries reach BoxedWine's X server with `int 0x9b` and
 * stack arguments. CPU64 decodes opcode 0x9b as x87 FWAIT, and the old
 * convention has no room for 64-bit pointers, so the x86-64 shim uses a
 * private syscall number instead, the same mechanism the DXMT unix-call
 * bridge already relies on:
 *
 *   RAX = BOXEDWINE_X64_HOSTCALL_X11_BRIDGE
 *   RDI = operation index (BOXEDWINE_X64_X11_OP_*)
 *   RSI = guest pointer to an array of uint64_t arguments (may be 0 when
 *         RDX is 0)
 *   RDX = argument count, 0..BOXEDWINE_X64_X11_MAX_ARGS
 *   RAX = signed 64-bit result on return
 *
 * The argument array is IN/OUT: an operation that produces a variable-size
 * result reports the number of bytes it needs by writing the capacity slot
 * back, and returns BOXEDWINE_X64_X11_E_BUFFER. Every pointer the host reads
 * or writes through is validated against the guest page table first; the
 * host never dereferences a guest address directly.
 *
 * Every guest-visible structure (Display, Screen, Visual, XEvent, ...) is
 * written in the exact x86-64 Xlib layout. The guest shim asserts those
 * layouts against the system headers it is compiled with; the host asserts
 * the same numbers in source/x11/x11layout64.h.
 */
#ifndef BOXEDWINE_X64_X11_BRIDGE_H
#define BOXEDWINE_X64_X11_BRIDGE_H

#include <stdint.h>

/* Above the Linux x86-64 syscall table, beside the DXMT unix-call number. */
#define BOXEDWINE_X64_HOSTCALL_X11_BRIDGE 0x7fff0002ULL

/* Zero through fifteen arguments. */
#define BOXEDWINE_X64_X11_MAX_ARGS 16U

/* Results below zero never collide with an Xlib Status, count, or XID. */
#define BOXEDWINE_X64_X11_E_BADOP   (-1)  /* operation index outside the table */
#define BOXEDWINE_X64_X11_E_BUFFER  (-2)  /* capacity slot rewritten with the size needed */
#define BOXEDWINE_X64_X11_E_FAULT   (-3)  /* a guest pointer was not readable/writable */
#define BOXEDWINE_X64_X11_E_ARGS    (-4)  /* argument count outside 0..16 or too few */
#define BOXEDWINE_X64_X11_E_UNIMPL  (-5)  /* known operation, not implemented yet */
#define BOXEDWINE_X64_X11_E_DISPLAY (-6)  /* the Display pointer names no open display */

/* Bytes XOpenDisplay hands the host to lay out Display, Screen, Depth[],
 * Visual[], ScreenFormat[] and the vendor string. */
#define BOXEDWINE_X64_X11_DISPLAY_ARENA_BYTES 16384U

/* Private tail of the guest Display: the host's display id, placed after the
 * last public Xlib field (xdefaults at 288, so the public part is 296 bytes). */
#define BOXEDWINE_X64_X11_DISPLAY_ID_OFFSET 296U
#define BOXEDWINE_X64_X11_DISPLAY_PRIVATE_BYTES 320U

/* Operation table. The string is what diagnostics print. Indices are the
 * position in this list; append, never reorder. */
#define BOXEDWINE_X64_X11_OPS(X) \
    X(INIT_THREADS, "init-threads") \
    X(OPEN_DISPLAY, "open-display") \
    X(CLOSE_DISPLAY, "close-display") \
    X(GRAB_SERVER, "grab-server") \
    X(UNGRAB_SERVER, "ungrab-server") \
    X(SYNC, "sync") \
    X(FLUSH, "flush") \
    X(INTERN_ATOM, "intern-atom") \
    X(INTERN_ATOMS, "intern-atoms") \
    X(GET_ATOM_NAME, "get-atom-name") \
    X(CREATE_WINDOW, "create-window") \
    X(DESTROY_WINDOW, "destroy-window") \
    X(MAP_WINDOW, "map-window") \
    X(UNMAP_WINDOW, "unmap-window") \
    X(SELECT_INPUT, "select-input") \
    X(MOVE_RESIZE_WINDOW, "move-resize-window") \
    X(CONFIGURE_WINDOW, "configure-window") \
    X(CHANGE_WINDOW_ATTRIBUTES, "change-window-attributes") \
    X(GET_WINDOW_ATTRIBUTES, "get-window-attributes") \
    X(REPARENT_WINDOW, "reparent-window") \
    X(TRANSLATE_COORDINATES, "translate-coordinates") \
    X(GET_GEOMETRY, "get-geometry") \
    X(QUERY_TREE, "query-tree") \
    X(WITHDRAW_WINDOW, "withdraw-window") \
    X(ICONIFY_WINDOW, "iconify-window") \
    X(SET_INPUT_FOCUS, "set-input-focus") \
    X(GET_INPUT_FOCUS, "get-input-focus") \
    X(SET_TRANSIENT_FOR_HINT, "set-transient-for-hint") \
    X(CLEAR_AREA, "clear-area") \
    X(CHANGE_PROPERTY, "change-property") \
    X(GET_WINDOW_PROPERTY, "get-window-property") \
    X(DELETE_PROPERTY, "delete-property") \
    X(SET_WM_HINTS, "set-wm-hints") \
    X(GET_WM_HINTS, "get-wm-hints") \
    X(SET_WM_NORMAL_HINTS, "set-wm-normal-hints") \
    X(GET_WM_NORMAL_HINTS, "get-wm-normal-hints") \
    X(SET_TEXT_PROPERTY, "set-text-property") \
    X(LOCK_EVENTS, "lock-events") \
    X(GET_EVENT, "get-event") \
    X(REMOVE_EVENT, "remove-event") \
    X(UNLOCK_EVENTS, "unlock-events") \
    X(CHECK_TYPED_WINDOW_EVENT, "check-typed-window-event") \
    X(SEND_EVENT, "send-event") \
    X(PUT_BACK_EVENT, "put-back-event") \
    X(GRAB_POINTER, "grab-pointer") \
    X(UNGRAB_POINTER, "ungrab-pointer") \
    X(WARP_POINTER, "warp-pointer") \
    X(QUERY_POINTER, "query-pointer") \
    X(CREATE_COLORMAP, "create-colormap") \
    X(FREE_COLORMAP, "free-colormap") \
    X(ALLOC_COLOR, "alloc-color") \
    X(ALLOC_COLOR_CELLS, "alloc-color-cells") \
    X(FREE_COLORS, "free-colors") \
    X(QUERY_COLOR, "query-color") \
    X(QUERY_COLORS, "query-colors") \
    X(STORE_COLOR, "store-color") \
    X(CREATE_GC, "create-gc") \
    X(FREE_GC, "free-gc") \
    X(CHANGE_GC, "change-gc") \
    X(SET_GC_VALUE, "set-gc-value") \
    X(SET_CLIP_RECTANGLES, "set-clip-rectangles") \
    X(COPY_AREA, "copy-area") \
    X(DRAW_LINE, "draw-line") \
    X(DRAW_RECTANGLE, "draw-rectangle") \
    X(FILL_RECTANGLE, "fill-rectangle") \
    X(PUT_IMAGE, "put-image") \
    X(GET_IMAGE, "get-image") \
    X(CREATE_PIXMAP, "create-pixmap") \
    X(CREATE_BITMAP_FROM_DATA, "create-bitmap-from-data") \
    X(FREE_PIXMAP, "free-pixmap") \
    X(CREATE_FONT_CURSOR, "create-font-cursor") \
    X(CREATE_PIXMAP_CURSOR, "create-pixmap-cursor") \
    X(DEFINE_CURSOR, "define-cursor") \
    X(FREE_CURSOR, "free-cursor") \
    X(GET_KEYBOARD_MAPPING, "get-keyboard-mapping") \
    X(GET_MODIFIER_MAPPING, "get-modifier-mapping") \
    X(KEYSYM_TO_KEYCODE, "keysym-to-keycode") \
    X(KEYCODE_TO_KEYSYM, "keycode-to-keysym") \
    X(KEYSYM_TO_STRING, "keysym-to-string") \
    X(LOOKUP_STRING, "lookup-string") \
    X(KB_TRANSLATE_KEYSYM, "kb-translate-keysym") \
    X(LOOKUP_KEYSYM, "lookup-keysym") \
    X(QUERY_EXTENSION, "query-extension") \
    X(SET_SELECTION_OWNER, "set-selection-owner") \
    X(GET_SELECTION_OWNER, "get-selection-owner") \
    X(CONVERT_SELECTION, "convert-selection") \
    X(REPORT_UNIMPLEMENTED, "report-unimplemented") \
    X(TRACE, "trace")

enum boxedwine_x64_x11_op {
#define BOXEDWINE_X64_X11_OP_ENUM(name, text) BOXEDWINE_X64_X11_OP_##name,
    BOXEDWINE_X64_X11_OPS(BOXEDWINE_X64_X11_OP_ENUM)
#undef BOXEDWINE_X64_X11_OP_ENUM
    BOXEDWINE_X64_X11_OP_COUNT
};

/* Selector for SET_GC_VALUE: args = { display, gc, selector, value }. */
#define BOXEDWINE_X64_X11_GC_FUNCTION 0
#define BOXEDWINE_X64_X11_GC_BACKGROUND 1
#define BOXEDWINE_X64_X11_GC_FOREGROUND 2
#define BOXEDWINE_X64_X11_GC_SUBWINDOW_MODE 3
#define BOXEDWINE_X64_X11_GC_GRAPHICS_EXPOSURES 4
#define BOXEDWINE_X64_X11_GC_CLIP_MASK 5
#define BOXEDWINE_X64_X11_GC_FILL_STYLE 6
#define BOXEDWINE_X64_X11_GC_ARC_MODE 7

#if defined(__x86_64__) && !defined(BOXEDWINE_X64_X11_BRIDGE_HOST)
/* Guest side. Independent of the guest libc: FEX executes the x86-64
 * SYSCALL instruction and BoxedWine consumes the reserved number. */
static inline int64_t boxedwine_x64_x11_call(uint64_t op, uint64_t *args,
                                             uint64_t count)
{
    uint64_t number = BOXEDWINE_X64_HOSTCALL_X11_BRIDGE;
    uint64_t argsAddress = (uint64_t)(uintptr_t)args;
    __asm__ volatile("syscall"
                     : "+a"(number)
                     : "D"(op), "S"(argsAddress), "d"(count)
                     : "rcx", "r11", "memory");
    return (int64_t)number;
}
#endif

#endif /* BOXEDWINE_X64_X11_BRIDGE_H */
