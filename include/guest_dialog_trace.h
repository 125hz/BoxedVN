/*
 * BoxedWine - what a guest dialog says, in a device log.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * A 64-bit program that refuses to start paints its reason into a small
 * window and waits on a condition variable forever. Every fact about that
 * window already reaches the log -- that it was created, its geometry, that
 * it was mapped, that it was presented -- except the one worth reading.
 *
 * The X protocol does not carry the reason either. Wine rasterises text
 * in-process with FreeType and hands the server finished pixels through
 * XPutImage; source/x11 implements no XDrawString, XDrawText or
 * XDrawImageString at all, because nothing has ever called one. So there is
 * no string in the server to log for the body of a message box.
 *
 * What the server does see is the window's name. winex11.drv sets WM_NAME
 * and _NET_WM_NAME from the window caption and WM_CLASS from the window
 * class, through XChangeProperty and XSetTextProperty, and a dialog's
 * caption is who is complaining. That is the cheap and reliable half, and
 * it is what this bounds and prints, together with a line naming the window
 * itself when it is mapped. The other half -- the body text -- comes from
 * Wine's own `msgbox` channel, which its user32 message box writes the text
 * to and which the 64-bit launch now turns on (ios/app/Sources/AppModel.swift).
 * The two land in the same capture, seconds apart, and neither depends on
 * the other being there.
 *
 * Bounded the way include/dll_search_trace.h is bounded, and for the same
 * reason: a window whose title is set on a timer would otherwise write the
 * same line until the log is useless. The budget is whole-session rather
 * than per-process because the interesting dialogs are few and they come
 * from two processes at most -- the program and the desktop.
 */

#ifndef __GUEST_DIALOG_TRACE_H__
#define __GUEST_DIALOG_TRACE_H__

/* Lines the whole session may spend on window text. A desktop session sets
 * a name on the desktop window, one on the program's main window and one on
 * each dialog; 64 covers that many times over and is a readable block in a
 * device log rather than its bulk. */
#define K_GUEST_DIALOG_TRACE_BUDGET 64

/* Characters of one property that reach a line. A caption is a few dozen;
 * WM_CLASS is two short names. Long enough that a truncation is a real
 * surprise, short enough that a hostile property cannot own the log. */
#define K_GUEST_DIALOG_TEXT_MAX 240

/* A window at most this big is a candidate for "small transient thing the
 * user is meant to read", even when it names no transient parent. Wine's
 * message boxes and its own error dialogs are far under this; a program's
 * main window at the emulated 800x600 is not. */
#define K_GUEST_DIALOG_MAX_WIDTH 640
#define K_GUEST_DIALOG_MAX_HEIGHT 480

#if defined(__cplusplus)
#include <atomic>
#include <cstddef>
#include <cstring>

namespace boxedvn {

/* The properties whose value is text a person wrote. Compared by atom NAME,
 * not by number: WM_NAME and WM_CLASS are predefined atoms with fixed ids,
 * but _NET_WM_NAME is interned at run time and its id differs between
 * sessions, so a numeric list would quietly stop matching the one property
 * modern Wine actually sets. */
inline bool guestDialogPropertyIsName(const char* atomName) {
    if (atomName == nullptr) {
        return false;
    }
    static const char* const names[] = {
        "WM_NAME",
        "WM_ICON_NAME",
        "WM_CLASS",
        "WM_WINDOW_ROLE",
        "_NET_WM_NAME",
        "_NET_WM_ICON_NAME",
    };
    for (const char* name : names) {
        if (std::strcmp(atomName, name) == 0) {
            return true;
        }
    }
    return false;
}

/* Whether a window that is being mapped is worth a line of its own.
 *
 * An override-redirect window is a menu, a tooltip or a drag icon: the
 * server sees dozens of them in a session and none of them is a dialog.
 * A window that names a transient parent is one whatever its size -- that
 * is what WM_TRANSIENT_FOR means -- and anything else has to be small. */
inline bool guestDialogWindowIsInteresting(unsigned int width,
                                           unsigned int height,
                                           bool hasTransientFor,
                                           bool overrideRedirect) {
    if (overrideRedirect) {
        return false;
    }
    if (width == 0 || height == 0) {
        return false;
    }
    if (hasTransientFor) {
        return true;
    }
    return width <= K_GUEST_DIALOG_MAX_WIDTH &&
           height <= K_GUEST_DIALOG_MAX_HEIGHT;
}

/* Copy a text property into a printable, bounded, single-line C string.
 *
 * Property bytes are whatever the guest wrote: a caption is Latin-1 or
 * UTF-8, WM_CLASS is two NUL-separated names, and a program is free to put
 * a newline or a quote in any of them. Everything outside printable ASCII
 * becomes a dot so one byte cannot end the line early, and an apostrophe
 * becomes a dot so it cannot close the quoted field and forge another. A
 * NUL becomes '|' -- it is a separator inside WM_CLASS, not a terminator --
 * and trailing separators are dropped, because a property is usually stored
 * with its terminating NUL included.
 *
 * Returns the number of characters written, not counting the terminator.
 * `out` is always NUL-terminated when outSize is non-zero. */
inline size_t guestDialogSanitizeText(const unsigned char* bytes,
                                      size_t length,
                                      char* out,
                                      size_t outSize) {
    if (out == nullptr || outSize == 0) {
        return 0;
    }
    size_t written = 0;
    const size_t limit = outSize - 1;
    if (bytes != nullptr) {
        for (size_t i = 0; i < length && written < limit; i++) {
            const unsigned char c = bytes[i];
            if (c == 0) {
                out[written++] = '|';
            } else if (c >= 0x20 && c < 0x7f && c != 0x27 /* apostrophe */) {
                out[written++] = (char)c;
            } else {
                out[written++] = '.';
            }
        }
    }
    while (written > 0 && out[written - 1] == '|') {
        written--;
    }
    out[written] = 0;
    return written;
}

/* What to do with one candidate line. The counter stops at the budget so a
 * program that sets a title in a loop cannot wrap it back around. */
enum GuestDialogTraceSlot {
    K_GUEST_DIALOG_TRACE_PRINT = 0,   /* print the line */
    K_GUEST_DIALOG_TRACE_LAST = 1,    /* print that the budget is spent */
    K_GUEST_DIALOG_TRACE_QUIET = 2,   /* say nothing */
};

inline GuestDialogTraceSlot guestDialogTraceSlot() {
    static std::atomic<unsigned int> spent{0};
    unsigned int taken = spent.load(std::memory_order_relaxed);
    while (taken <= K_GUEST_DIALOG_TRACE_BUDGET) {
        if (spent.compare_exchange_weak(taken, taken + 1,
                                        std::memory_order_relaxed)) {
            return taken < K_GUEST_DIALOG_TRACE_BUDGET
                       ? K_GUEST_DIALOG_TRACE_PRINT
                       : K_GUEST_DIALOG_TRACE_LAST;
        }
    }
    return K_GUEST_DIALOG_TRACE_QUIET;
}

}  // namespace boxedvn

#endif /* __cplusplus */

#endif /* __GUEST_DIALOG_TRACE_H__ */
