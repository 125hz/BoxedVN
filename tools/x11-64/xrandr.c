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
 * The x86-64 guest libXrandr.so.2 for BoxedWine.
 *
 * Wine 9.0's winex11.drv picks its display-settings handler by what XRandR
 * answers (dlls/winex11.drv/xrandr.c). With no XRandR it takes `nores`,
 * whose mode list is a single entry -- the desktop size, at screen_bpp, at
 * 60 Hz. A program that enumerates display modes through EnumDisplaySettings
 * or IDXGIOutput::GetDisplayModeList then finds one mode and nothing to
 * switch to, and many refuse to start.
 *
 * So this is not a stub. It reports RandR 1.4, one connected output
 * ("BoxedVN") on one CRTC, and the mode list the host owns: the launch
 * resolution, the standard sizes, each at 60 Hz and at the panel's own rate.
 * Wine's xrandr14_get_modes walks XRRGetScreenResourcesCurrent,
 * XRRGetOutputInfo and XRRGetCrtcInfo to build that list, and
 * XRRSetCrtcConfig to switch, which becomes a root-window resize on the host
 * -- the same thing the IA-32 lane's XRRSetScreenConfigAndRate does.
 *
 * Every structure here is guest memory this library owns, laid out exactly
 * as the real libXrandr lays it out for this ABI, because winex11.so was
 * compiled against the real <X11/extensions/Xrandr.h>. The offsets are
 * asserted below, and asserted again against the real header in
 * layout_check.c when the builder has libxrandr-dev.
 *
 * What crosses to the host is only policy: which modes exist, which one is
 * current, and what a switch does. See BOXEDWINE_X64_X11_OP_RANDR_GET_STATE
 * and _RANDR_SET_MODE in include/boxedwine_x64_x11_bridge.h.
 */
#include <X11/X.h>
#include <X11/Xlib.h>

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boxedwine_x64_x11_bridge.h"

#if !defined(__x86_64__)
#error "tools/x11-64 is the x86-64 guest shim"
#endif

#define BW_EXPORT __attribute__((visibility("default")))

#define A(x) ((uint64_t)(x))
#define P(x) ((uint64_t)(uintptr_t)(x))

/* ---- The Xrandr ABI -------------------------------------------------------
 *
 * Declared here rather than included: the builder needs no libxrandr-dev to
 * produce the library, and a header that disagreed with what winex11.so was
 * compiled against would be a silent memory-layout fault rather than a build
 * failure. The static assertions below pin every field an x86-64 caller
 * reads.
 */
typedef XID RROutput;
typedef XID RRCrtc;
typedef XID RRMode;
typedef XID RRProvider;
typedef unsigned short Rotation;
typedef unsigned short SizeID;
typedef unsigned short SubpixelOrder;
typedef unsigned short Connection;
typedef unsigned long XRRModeFlags;

#define RR_Rotate_0 1
#define RR_Rotate_90 2
#define RR_Rotate_180 4
#define RR_Rotate_270 8

#define RR_Connected 0
#define RR_Disconnected 1
#define RR_UnknownConnection 2

#define RRSetConfigSuccess 0
#define RRSetConfigInvalidConfigTime 1
#define RRSetConfigInvalidTime 2
#define RRSetConfigFailed 3

typedef struct {
    int width, height;
    int mwidth, mheight;
} XRRScreenSize;

typedef struct _XRRModeInfo {
    RRMode id;
    unsigned int width;
    unsigned int height;
    unsigned long dotClock;
    unsigned int hSyncStart;
    unsigned int hSyncEnd;
    unsigned int hTotal;
    unsigned int hSkew;
    unsigned int vSyncStart;
    unsigned int vSyncEnd;
    unsigned int vTotal;
    char *name;
    unsigned int nameLength;
    XRRModeFlags modeFlags;
} XRRModeInfo;

typedef struct _XRRScreenResources {
    Time timestamp;
    Time configTimestamp;
    int ncrtc;
    RRCrtc *crtcs;
    int noutput;
    RROutput *outputs;
    int nmode;
    XRRModeInfo *modes;
} XRRScreenResources;

typedef struct _XRROutputInfo {
    Time timestamp;
    RRCrtc crtc;
    char *name;
    int nameLen;
    unsigned long mm_width;
    unsigned long mm_height;
    Connection connection;
    SubpixelOrder subpixel_order;
    int ncrtc;
    RRCrtc *crtcs;
    int nclone;
    RROutput *clones;
    int nmode;
    int npreferred;
    RRMode *modes;
} XRROutputInfo;

typedef struct _XRRCrtcInfo {
    Time timestamp;
    int x, y;
    unsigned int width, height;
    RRMode mode;
    Rotation rotation;
    int noutput;
    RROutput *outputs;
    Rotation rotations;
    int npossible;
    RROutput *possible;
} XRRCrtcInfo;

typedef struct _XRRProviderResources {
    Time timestamp;
    int nproviders;
    RRProvider *providers;
} XRRProviderResources;

typedef struct _XRRProviderInfo {
    unsigned int capabilities;
    int ncrtcs;
    RRCrtc *crtcs;
    int noutputs;
    RROutput *outputs;
    char *name;
    int nassociatedproviders;
    RRProvider *associated_providers;
    unsigned int *associated_capability;
    int nameLen;
} XRRProviderInfo;

/* XRRScreenConfiguration is opaque in the real header; winex11 only ever
 * passes the pointer back. */
typedef struct _XRRScreenConfiguration {
    int sizeIndex;
    short rate;
    Rotation rotation;
} XRRScreenConfiguration;

#define BW_OFFSET(type, field, expected) \
    _Static_assert(offsetof(type, field) == (expected), \
                   #type "." #field " must sit at " #expected)
#define BW_SIZE(type, expected) \
    _Static_assert(sizeof(type) == (expected), #type " must be " #expected " bytes")

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

BW_SIZE(struct boxedwine_x64_x11_randr_mode, 16);
BW_SIZE(struct boxedwine_x64_x11_randr_state, 48);

/* ---- Identities the driver sees ------------------------------------------- */

/* One output on one CRTC. The ids are arbitrary but must be non-zero: Wine
 * reads a zero crtc as "detached" and a zero mode as "disabled". */
#define BW_RANDR_OUTPUT ((RROutput)0x1001)
#define BW_RANDR_CRTC ((RRCrtc)0x1101)
#define BW_RANDR_MODE_BASE ((RRMode)0x2000)

#define BW_RANDR_OUTPUT_NAME "BoxedVN"

/* Wine registers its RandR event handlers at event_base + subcode, and its
 * handler table has 128 entries, so the base has to be a small number above
 * the core events (LASTEvent is 36). The 32-bit lane's XRAND_Base is 10000,
 * which only ever reaches a 32-bit Xlib that does not index a table with it.
 * Nothing here ever delivers a RandR event; the base exists so registration
 * lands somewhere legal. */
#define BW_RANDR_EVENT_BASE 64
#define BW_RANDR_ERROR_BASE 152

/* What XRRQueryVersion reports. Wine takes its RandR 1.4 handler only for
 * 1.4 or newer (xrandr.c: `ret >= 4 && (major > 1 || (major == 1 && minor >=
 * 4))`); anything less leaves it on the 1.0 handler, whose mode list comes
 * from XRRSizes and XRRRates below. */
#define BW_RANDR_MAJOR 1
#define BW_RANDR_MINOR 4

/* ---- Host state ------------------------------------------------------------ */

struct bw_randr_state {
    struct boxedwine_x64_x11_randr_state header;
    struct boxedwine_x64_x11_randr_mode modes[BOXEDWINE_X64_X11_RANDR_MAX_MODES];
};

/* One call, one fixed buffer: the payload is bounded by
 * BOXEDWINE_X64_X11_RANDR_MAX_MODES, so there is no size negotiation. */
static int bw_randr_fetch(Display *dpy, struct bw_randr_state *out)
{
    unsigned char blob[sizeof(struct boxedwine_x64_x11_randr_state) +
                       BOXEDWINE_X64_X11_RANDR_MAX_MODES *
                           sizeof(struct boxedwine_x64_x11_randr_mode)];
    uint64_t args[3];
    int64_t result;
    uint32_t count;

    if (!dpy) {
        return 0;
    }
    memset(blob, 0, sizeof(blob));
    args[0] = P(dpy);
    args[1] = P(blob);
    args[2] = sizeof(blob);
    result = boxedwine_x64_x11_call(BOXEDWINE_X64_X11_OP_RANDR_GET_STATE, args, 3);
    if (result < 0) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    memcpy(&out->header, blob, sizeof(out->header));
    if (out->header.version != BOXEDWINE_X64_X11_RANDR_STATE_VERSION) {
        return 0;
    }
    count = out->header.modeCount;
    if (count > BOXEDWINE_X64_X11_RANDR_MAX_MODES) {
        count = BOXEDWINE_X64_X11_RANDR_MAX_MODES;
        out->header.modeCount = count;
    }
    if (count) {
        memcpy(out->modes, blob + sizeof(out->header),
               (size_t)count * sizeof(out->modes[0]));
    }
    return count != 0;
}

static Status bw_randr_set(Display *dpy, uint32_t width, uint32_t height, uint32_t rate)
{
    uint64_t args[4];
    int64_t result;

    args[0] = P(dpy);
    args[1] = A(width);
    args[2] = A(height);
    args[3] = A(rate);
    result = boxedwine_x64_x11_call(BOXEDWINE_X64_X11_OP_RANDR_SET_MODE, args, 4);
    if (result == RRSetConfigSuccess) {
        return RRSetConfigSuccess;
    }
    return RRSetConfigFailed;
}

/* Timestamps have to move for a caller that compares them; nothing here
 * validates one. */
static Time bw_randr_stamp(void)
{
    static unsigned long counter;
    return (Time)(++counter);
}

/* ---- Extension presence ---------------------------------------------------- */

BW_EXPORT Bool XRRQueryExtension(Display *dpy, int *event_base_return, int *error_base_return)
{
    struct bw_randr_state state;

    /* A host that does not serve the RandR operations reports the extension
     * absent, so Wine falls back to its `nores` handler rather than working
     * from an empty resource list. */
    if (!bw_randr_fetch(dpy, &state)) {
        return False;
    }
    if (event_base_return) {
        *event_base_return = BW_RANDR_EVENT_BASE;
    }
    if (error_base_return) {
        *error_base_return = BW_RANDR_ERROR_BASE;
    }
    return True;
}

BW_EXPORT Status XRRQueryVersion(Display *dpy, int *major_version_return,
                                 int *minor_version_return)
{
    struct bw_randr_state state;

    if (!bw_randr_fetch(dpy, &state)) {
        return 0;
    }
    if (major_version_return) {
        *major_version_return = BW_RANDR_MAJOR;
    }
    if (minor_version_return) {
        *minor_version_return = BW_RANDR_MINOR;
    }
    return 1;
}

BW_EXPORT void XRRSelectInput(Display *dpy, Window window, int mask)
{
    /* Nothing here changes a mode behind Wine's back, so no RandR event is
     * ever delivered and there is nothing to select. */
    (void)dpy;
    (void)window;
    (void)mask;
}

/* ---- RandR 1.2+ resources -------------------------------------------------- */

/* Enough for "16384x16384\0". */
#define BW_MODE_NAME_BYTES 16

static XRRScreenResources *bw_build_resources(const struct bw_randr_state *state)
{
    const uint32_t count = state->header.modeCount;
    size_t offset = 0;
    size_t crtcsOffset, outputsOffset, modesOffset, namesOffset, total;
    unsigned char *block;
    XRRScreenResources *resources;
    XRRModeInfo *modes;
    uint32_t i;

    offset += sizeof(XRRScreenResources);
    crtcsOffset = offset;
    offset += sizeof(RRCrtc);
    outputsOffset = offset;
    offset += sizeof(RROutput);
    modesOffset = offset;
    offset += (size_t)count * sizeof(XRRModeInfo);
    namesOffset = offset;
    offset += (size_t)count * BW_MODE_NAME_BYTES;
    total = offset;

    block = (unsigned char *)calloc(1, total);
    if (!block) {
        return NULL;
    }
    resources = (XRRScreenResources *)block;
    resources->timestamp = bw_randr_stamp();
    resources->configTimestamp = resources->timestamp;
    resources->ncrtc = 1;
    resources->crtcs = (RRCrtc *)(block + crtcsOffset);
    resources->crtcs[0] = BW_RANDR_CRTC;
    resources->noutput = 1;
    resources->outputs = (RROutput *)(block + outputsOffset);
    resources->outputs[0] = BW_RANDR_OUTPUT;
    resources->nmode = (int)count;
    resources->modes = (XRRModeInfo *)(block + modesOffset);

    modes = resources->modes;
    for (i = 0; i < count; i++) {
        const uint32_t width = state->modes[i].width;
        const uint32_t height = state->modes[i].height;
        const uint32_t rate = state->modes[i].rate;
        char *name = (char *)(block + namesOffset + (size_t)i * BW_MODE_NAME_BYTES);
        int length;

        modes[i].id = BW_RANDR_MODE_BASE + i;
        modes[i].width = width;
        modes[i].height = height;
        /* Wine derives the refresh rate from the timings:
         *   dots = hTotal * vTotal; frequency = (dotClock + dots/2) / dots
         * With no blanking the division is exact, so the rate a caller reads
         * is the rate the host named. */
        modes[i].hSyncStart = width;
        modes[i].hSyncEnd = width;
        modes[i].hTotal = width;
        modes[i].hSkew = 0;
        modes[i].vSyncStart = height;
        modes[i].vSyncEnd = height;
        modes[i].vTotal = height;
        modes[i].dotClock = (unsigned long)width * (unsigned long)height * (unsigned long)rate;
        modes[i].modeFlags = 0;
        modes[i].name = name;
        length = snprintf(name, BW_MODE_NAME_BYTES, "%ux%u", width, height);
        modes[i].nameLength = length > 0 && length < BW_MODE_NAME_BYTES
                                  ? (unsigned int)length
                                  : (unsigned int)strlen(name);
    }
    return resources;
}

static XRRScreenResources *bw_get_resources(Display *dpy)
{
    struct bw_randr_state state;

    if (!bw_randr_fetch(dpy, &state)) {
        return NULL;
    }
    return bw_build_resources(&state);
}

BW_EXPORT XRRScreenResources *XRRGetScreenResources(Display *dpy, Window window)
{
    (void)window;
    return bw_get_resources(dpy);
}

BW_EXPORT XRRScreenResources *XRRGetScreenResourcesCurrent(Display *dpy, Window window)
{
    (void)window;
    return bw_get_resources(dpy);
}

BW_EXPORT void XRRFreeScreenResources(XRRScreenResources *resources)
{
    free(resources);
}

BW_EXPORT XRROutputInfo *XRRGetOutputInfo(Display *dpy, XRRScreenResources *resources,
                                          RROutput output)
{
    struct bw_randr_state state;
    size_t offset = 0;
    size_t crtcsOffset, modesOffset, nameOffset, total;
    unsigned char *block;
    XRROutputInfo *info;
    uint32_t count, i;

    (void)resources;
    if (output != BW_RANDR_OUTPUT || !bw_randr_fetch(dpy, &state)) {
        return NULL;
    }
    count = state.header.modeCount;

    offset += sizeof(XRROutputInfo);
    crtcsOffset = offset;
    offset += sizeof(RRCrtc);
    modesOffset = offset;
    offset += (size_t)count * sizeof(RRMode);
    nameOffset = offset;
    offset += sizeof(BW_RANDR_OUTPUT_NAME);
    total = offset;

    block = (unsigned char *)calloc(1, total);
    if (!block) {
        return NULL;
    }
    info = (XRROutputInfo *)block;
    info->timestamp = bw_randr_stamp();
    info->crtc = BW_RANDR_CRTC;
    info->name = (char *)(block + nameOffset);
    memcpy(info->name, BW_RANDR_OUTPUT_NAME, sizeof(BW_RANDR_OUTPUT_NAME));
    info->nameLen = (int)(sizeof(BW_RANDR_OUTPUT_NAME) - 1);
    info->mm_width = state.header.mmWidth;
    info->mm_height = state.header.mmHeight;
    info->connection = RR_Connected;
    info->subpixel_order = 0; /* SubPixelUnknown */
    info->ncrtc = 1;
    info->crtcs = (RRCrtc *)(block + crtcsOffset);
    info->crtcs[0] = BW_RANDR_CRTC;
    info->nclone = 0;
    info->clones = NULL;
    info->nmode = (int)count;
    info->npreferred = count ? 1 : 0;
    info->modes = (RRMode *)(block + modesOffset);
    for (i = 0; i < count; i++) {
        info->modes[i] = BW_RANDR_MODE_BASE + i;
    }
    return info;
}

BW_EXPORT void XRRFreeOutputInfo(XRROutputInfo *outputInfo)
{
    free(outputInfo);
}

BW_EXPORT XRRCrtcInfo *XRRGetCrtcInfo(Display *dpy, XRRScreenResources *resources, RRCrtc crtc)
{
    struct bw_randr_state state;
    size_t offset = 0;
    size_t outputsOffset, possibleOffset, total;
    unsigned char *block;
    XRRCrtcInfo *info;

    (void)resources;
    if (crtc != BW_RANDR_CRTC || !bw_randr_fetch(dpy, &state)) {
        return NULL;
    }

    offset += sizeof(XRRCrtcInfo);
    outputsOffset = offset;
    offset += sizeof(RROutput);
    possibleOffset = offset;
    offset += sizeof(RROutput);
    total = offset;

    block = (unsigned char *)calloc(1, total);
    if (!block) {
        return NULL;
    }
    info = (XRRCrtcInfo *)block;
    info->timestamp = bw_randr_stamp();
    info->x = 0;
    info->y = 0;
    info->width = state.header.currentWidth;
    info->height = state.header.currentHeight;
    info->mode = state.header.currentMode == BOXEDWINE_X64_X11_RANDR_NO_MODE
                     ? None
                     : BW_RANDR_MODE_BASE + state.header.currentMode;
    /* One orientation only: the host cannot rotate the root window, and a
     * rotations mask with more bits makes Wine report every mode again per
     * orientation. */
    info->rotation = RR_Rotate_0;
    info->rotations = RR_Rotate_0;
    info->noutput = 1;
    info->outputs = (RROutput *)(block + outputsOffset);
    info->outputs[0] = BW_RANDR_OUTPUT;
    info->npossible = 1;
    info->possible = (RROutput *)(block + possibleOffset);
    info->possible[0] = BW_RANDR_OUTPUT;
    return info;
}

BW_EXPORT void XRRFreeCrtcInfo(XRRCrtcInfo *crtcInfo)
{
    free(crtcInfo);
}

BW_EXPORT RROutput XRRGetOutputPrimary(Display *dpy, Window window)
{
    (void)dpy;
    (void)window;
    return BW_RANDR_OUTPUT;
}

BW_EXPORT Status XRRGetScreenSizeRange(Display *dpy, Window window, int *minWidth,
                                       int *minHeight, int *maxWidth, int *maxHeight)
{
    struct bw_randr_state state;

    if (!bw_randr_fetch(dpy, &state)) {
        return 0;
    }
    if (minWidth) {
        *minWidth = (int)state.header.minWidth;
    }
    if (minHeight) {
        *minHeight = (int)state.header.minHeight;
    }
    if (maxWidth) {
        *maxWidth = (int)state.header.maxWidth;
    }
    if (maxHeight) {
        *maxHeight = (int)state.header.maxHeight;
    }
    (void)window;
    return 1;
}

BW_EXPORT void XRRSetScreenSize(Display *dpy, Window window, int width, int height,
                                int mmWidth, int mmHeight)
{
    /* Wine calls this between the two XRRSetCrtcConfig calls of a mode
     * change, to make room for the CRTC before enabling it. The root window
     * here is resized by the switch itself, so there is nothing to reserve. */
    (void)dpy;
    (void)window;
    (void)width;
    (void)height;
    (void)mmWidth;
    (void)mmHeight;
}

BW_EXPORT Status XRRSetCrtcConfig(Display *dpy, XRRScreenResources *resources, RRCrtc crtc,
                                  Time timestamp, int x, int y, RRMode mode, Rotation rotation,
                                  RROutput *outputs, int noutputs)
{
    struct bw_randr_state state;
    uint32_t index;

    (void)resources;
    (void)timestamp;
    (void)outputs;
    (void)noutputs;
    if (crtc != BW_RANDR_CRTC) {
        return RRSetConfigFailed;
    }
    /* Wine disables the CRTC before shrinking the screen and re-enables it
     * with the new mode. The disable half is accepted and does nothing: this
     * output is the device panel and cannot be detached. */
    if (mode == None) {
        return RRSetConfigSuccess;
    }
    /* One output, always at the origin. A caller that asks for a positioned
     * CRTC is describing a second monitor this lane does not have; the mode
     * is still worth honouring, and the next XRRGetCrtcInfo reports the
     * origin it actually got. */
    (void)x;
    (void)y;
    if (rotation && rotation != RR_Rotate_0) {
        return RRSetConfigFailed;
    }
    if (!bw_randr_fetch(dpy, &state)) {
        return RRSetConfigFailed;
    }
    if (mode < BW_RANDR_MODE_BASE) {
        return RRSetConfigFailed;
    }
    index = (uint32_t)(mode - BW_RANDR_MODE_BASE);
    if (index >= state.header.modeCount) {
        return RRSetConfigFailed;
    }
    return bw_randr_set(dpy, state.modes[index].width, state.modes[index].height,
                        state.modes[index].rate);
}

/* ---- Properties and providers ---------------------------------------------- */

BW_EXPORT int XRRGetOutputProperty(Display *dpy, RROutput output, Atom property, long offset,
                                   long length, Bool _delete, Bool pending, Atom req_type,
                                   Atom *actual_type, int *actual_format, unsigned long *nitems,
                                   unsigned long *bytes_after, unsigned char **prop)
{
    /* No EDID and no other output property. Wine warns once per output and
     * carries on with no monitor name, which is what it does on any driver
     * that publishes none. */
    (void)dpy;
    (void)output;
    (void)property;
    (void)offset;
    (void)length;
    (void)_delete;
    (void)pending;
    (void)req_type;
    if (actual_type) {
        *actual_type = None;
    }
    if (actual_format) {
        *actual_format = 0;
    }
    if (nitems) {
        *nitems = 0;
    }
    if (bytes_after) {
        *bytes_after = 0;
    }
    if (prop) {
        *prop = NULL;
    }
    return BadRequest;
}

BW_EXPORT XRRProviderResources *XRRGetProviderResources(Display *dpy, Window window)
{
    XRRProviderResources *resources;

    /* Must not be NULL: Wine's xrandr14_get_gpus fails the whole handler on a
     * NULL here, but takes a documented fallback -- one made-up adapter, its
     * outputs read from the screen resources -- when the list is empty. */
    (void)dpy;
    (void)window;
    resources = (XRRProviderResources *)calloc(1, sizeof(*resources));
    if (!resources) {
        return NULL;
    }
    resources->timestamp = bw_randr_stamp();
    resources->nproviders = 0;
    resources->providers = NULL;
    return resources;
}

BW_EXPORT void XRRFreeProviderResources(XRRProviderResources *resources)
{
    free(resources);
}

BW_EXPORT XRRProviderInfo *XRRGetProviderInfo(Display *dpy, XRRScreenResources *resources,
                                              RRProvider provider)
{
    /* Unreachable while XRRGetProviderResources reports none. */
    (void)dpy;
    (void)resources;
    (void)provider;
    return NULL;
}

BW_EXPORT void XRRFreeProviderInfo(XRRProviderInfo *provider)
{
    free(provider);
}

/* ---- RandR 1.0 -------------------------------------------------------------
 *
 * Wine registers its 1.0 settings handler before deciding whether the 1.4 one
 * can replace it, and keeps it if anything about 1.4 disappoints. It reports
 * the same modes, addressed by size index and rate rather than by mode id.
 *
 * XRRSizes and XRRRates return arrays the caller does not free, so they are
 * built once. The host holds the mode list still for the life of the process
 * for exactly this reason.
 */

static pthread_mutex_t bw_sizes_lock = PTHREAD_MUTEX_INITIALIZER;
static XRRScreenSize bw_sizes[BOXEDWINE_X64_X11_RANDR_MAX_MODES];
static short bw_rates[BOXEDWINE_X64_X11_RANDR_MAX_MODES];
static int bw_size_count;
static int bw_rate_count;
static int bw_sizes_ready;

static int bw_build_sizes(Display *dpy)
{
    struct bw_randr_state state;
    uint32_t i;
    int j;

    pthread_mutex_lock(&bw_sizes_lock);
    if (bw_sizes_ready) {
        pthread_mutex_unlock(&bw_sizes_lock);
        return bw_size_count;
    }
    if (!bw_randr_fetch(dpy, &state)) {
        pthread_mutex_unlock(&bw_sizes_lock);
        return 0;
    }
    for (i = 0; i < state.header.modeCount; i++) {
        int known = 0;
        for (j = 0; j < bw_size_count; j++) {
            if (bw_sizes[j].width == (int)state.modes[i].width &&
                bw_sizes[j].height == (int)state.modes[i].height) {
                known = 1;
                break;
            }
        }
        if (!known && bw_size_count < (int)BOXEDWINE_X64_X11_RANDR_MAX_MODES) {
            bw_sizes[bw_size_count].width = (int)state.modes[i].width;
            bw_sizes[bw_size_count].height = (int)state.modes[i].height;
            bw_sizes[bw_size_count].mwidth = (int)(state.modes[i].width * 265 / 1000);
            bw_sizes[bw_size_count].mheight = (int)(state.modes[i].height * 265 / 1000);
            bw_size_count++;
        }
        known = 0;
        for (j = 0; j < bw_rate_count; j++) {
            if (bw_rates[j] == (short)state.modes[i].rate) {
                known = 1;
                break;
            }
        }
        if (!known && bw_rate_count < (int)BOXEDWINE_X64_X11_RANDR_MAX_MODES) {
            bw_rates[bw_rate_count++] = (short)state.modes[i].rate;
        }
    }
    bw_sizes_ready = bw_size_count > 0;
    pthread_mutex_unlock(&bw_sizes_lock);
    return bw_size_count;
}

BW_EXPORT XRRScreenSize *XRRSizes(Display *dpy, int screen, int *nsizes)
{
    const int count = bw_build_sizes(dpy);

    (void)screen;
    if (nsizes) {
        *nsizes = count;
    }
    return count ? bw_sizes : NULL;
}

BW_EXPORT short *XRRRates(Display *dpy, int screen, int sizeID, int *nrates)
{
    const int count = bw_build_sizes(dpy);

    (void)screen;
    if (sizeID < 0 || sizeID >= count) {
        if (nrates) {
            *nrates = 0;
        }
        return NULL;
    }
    /* Every size is offered at every rate. */
    if (nrates) {
        *nrates = bw_rate_count;
    }
    return bw_rate_count ? bw_rates : NULL;
}

BW_EXPORT XRRScreenConfiguration *XRRGetScreenInfo(Display *dpy, Window window)
{
    struct bw_randr_state state;
    XRRScreenConfiguration *config;
    int count, i;

    (void)window;
    if (!bw_randr_fetch(dpy, &state)) {
        return NULL;
    }
    count = bw_build_sizes(dpy);
    config = (XRRScreenConfiguration *)calloc(1, sizeof(*config));
    if (!config) {
        return NULL;
    }
    config->sizeIndex = 0;
    config->rate = (short)state.header.currentRate;
    config->rotation = RR_Rotate_0;
    for (i = 0; i < count; i++) {
        if (bw_sizes[i].width == (int)state.header.currentWidth &&
            bw_sizes[i].height == (int)state.header.currentHeight) {
            config->sizeIndex = i;
            break;
        }
    }
    return config;
}

BW_EXPORT void XRRFreeScreenConfigInfo(XRRScreenConfiguration *config)
{
    free(config);
}

BW_EXPORT SizeID XRRConfigCurrentConfiguration(XRRScreenConfiguration *config, Rotation *rotation)
{
    if (rotation) {
        *rotation = RR_Rotate_0;
    }
    return config ? (SizeID)config->sizeIndex : (SizeID)0;
}

BW_EXPORT short XRRConfigCurrentRate(XRRScreenConfiguration *config)
{
    return config ? config->rate : 0;
}

static Status bw_set_by_size_index(Display *dpy, int sizeIndex, short rate)
{
    const int count = bw_build_sizes(dpy);

    if (sizeIndex < 0 || sizeIndex >= count) {
        return RRSetConfigFailed;
    }
    return bw_randr_set(dpy, (uint32_t)bw_sizes[sizeIndex].width,
                        (uint32_t)bw_sizes[sizeIndex].height,
                        rate > 0 ? (uint32_t)rate : 0);
}

BW_EXPORT Status XRRSetScreenConfig(Display *dpy, XRRScreenConfiguration *config, Drawable draw,
                                    int size_index, Rotation rotation, Time timestamp)
{
    (void)draw;
    (void)rotation;
    (void)timestamp;
    return bw_set_by_size_index(dpy, size_index, config ? config->rate : 0);
}

BW_EXPORT Status XRRSetScreenConfigAndRate(Display *dpy, XRRScreenConfiguration *config,
                                           Drawable draw, int size_index, Rotation rotation,
                                           short rate, Time timestamp)
{
    (void)config;
    (void)draw;
    (void)rotation;
    (void)timestamp;
    return bw_set_by_size_index(dpy, size_index, rate);
}
