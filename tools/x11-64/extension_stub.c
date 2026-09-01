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
 * Deterministic stand-ins for the X extension libraries Wine's driver
 * dlopens by name (libXrender, libXrandr, libXinerama, libXi, libXcursor,
 * libXfixes, libXcomposite, libXxf86vm).
 *
 * The distro copies of these libraries cannot work here: they speak the X
 * wire protocol through libX11 internals the BoxedWine shim does not have.
 * Leaving them to fail at dlopen would also work, but silently and with a
 * message that names the wrong cause. Each stub library instead exports
 * exactly the entry points winex11.so looks up (measured from its string
 * table) and answers every "is this extension here?" question with no, so
 * Wine takes its core-protocol paths. Anything else that is reached reports
 * itself once through the bridge and returns zero.
 *
 * Compiled once per library with -DBW_STUB_LIBRARY=<name>; see
 * scripts/build-boxedwine-x64-x11.sh.
 */
#include <stdint.h>

#include "boxedwine_x64_x11_bridge.h"

#define BW_EXPORT __attribute__((visibility("default")))

static void bw_report_once(const char *name, int *reported)
{
    if (!*reported) {
        uint64_t args[1] = { (uint64_t)(uintptr_t)name };
        *reported = 1;
        boxedwine_x64_x11_call(BOXEDWINE_X64_X11_OP_REPORT_UNIMPLEMENTED, args, 1);
    }
}

/* A function reached only when its extension was wrongly assumed present. */
#define BW_STUB(name) \
    BW_EXPORT long name(void) \
    { \
        static int reported; \
        bw_report_once(#name, &reported); \
        return 0; \
    }

/* A query the driver makes before using an extension: answer "absent". */
#define BW_ABSENT(name, value) \
    BW_EXPORT long name(void) \
    { \
        return (value); \
    }

#if BW_STUB_LIBRARY == 1 /* libXrender.so.1 */
BW_ABSENT(XRenderQueryExtension, 0)
BW_ABSENT(XRenderQueryVersion, 0)
BW_STUB(XRenderAddGlyphs)
BW_STUB(XRenderChangePicture)
BW_STUB(XRenderComposite)
BW_STUB(XRenderCompositeText16)
BW_STUB(XRenderCreateGlyphSet)
BW_STUB(XRenderCreateLinearGradient)
BW_STUB(XRenderCreatePicture)
BW_STUB(XRenderFillRectangle)
BW_STUB(XRenderFindFormat)
BW_STUB(XRenderFindVisualFormat)
BW_STUB(XRenderFreeGlyphSet)
BW_STUB(XRenderFreePicture)
BW_STUB(XRenderSetPictureClipRectangles)
BW_STUB(XRenderSetPictureTransform)
#elif BW_STUB_LIBRARY == 2 /* libXrandr.so.2 */
BW_ABSENT(XRRQueryExtension, 0)
BW_ABSENT(XRRQueryVersion, 0)
BW_STUB(XRRConfigCurrentConfiguration)
BW_STUB(XRRConfigCurrentRate)
BW_STUB(XRRFreeCrtcInfo)
BW_STUB(XRRFreeOutputInfo)
BW_STUB(XRRFreeProviderInfo)
BW_STUB(XRRFreeProviderResources)
BW_STUB(XRRFreeScreenConfigInfo)
BW_STUB(XRRFreeScreenResources)
BW_STUB(XRRGetCrtcInfo)
BW_STUB(XRRGetOutputInfo)
BW_STUB(XRRGetOutputPrimary)
BW_STUB(XRRGetOutputProperty)
BW_STUB(XRRGetProviderInfo)
BW_STUB(XRRGetProviderResources)
BW_STUB(XRRGetScreenInfo)
BW_STUB(XRRGetScreenResources)
BW_STUB(XRRGetScreenResourcesCurrent)
BW_STUB(XRRGetScreenSizeRange)
BW_STUB(XRRRates)
BW_STUB(XRRSelectInput)
BW_STUB(XRRSetCrtcConfig)
BW_STUB(XRRSetScreenConfig)
BW_STUB(XRRSetScreenConfigAndRate)
BW_STUB(XRRSetScreenSize)
BW_STUB(XRRSizes)
#elif BW_STUB_LIBRARY == 3 /* libXinerama.so.1 */
BW_ABSENT(XineramaQueryExtension, 0)
BW_ABSENT(XineramaQueryVersion, 0)
BW_ABSENT(XineramaIsActive, 0)
BW_STUB(XineramaQueryScreens)
#elif BW_STUB_LIBRARY == 4 /* libXi.so.6 */
/* XIQueryVersion returns a Status; anything but Success disables XInput2. */
BW_ABSENT(XIQueryVersion, 1)
BW_STUB(XIFreeDeviceInfo)
BW_STUB(XIGetClientPointer)
BW_STUB(XIQueryDevice)
BW_STUB(XISelectEvents)
#elif BW_STUB_LIBRARY == 5 /* libXcursor.so.1 */
BW_STUB(XcursorImageCreate)
BW_STUB(XcursorImageDestroy)
BW_STUB(XcursorImageLoadCursor)
BW_STUB(XcursorImagesCreate)
BW_STUB(XcursorImagesDestroy)
BW_STUB(XcursorImagesLoadCursor)
BW_STUB(XcursorLibraryLoadCursor)
BW_ABSENT(XcursorSupportsARGB, 0)
BW_ABSENT(XcursorGetDefaultSize, 32)
#elif BW_STUB_LIBRARY == 6 /* libXfixes.so.3 */
BW_ABSENT(XFixesQueryExtension, 0)
BW_ABSENT(XFixesQueryVersion, 0)
BW_STUB(XFixesSelectSelectionInput)
#elif BW_STUB_LIBRARY == 7 /* libXcomposite.so.1 */
BW_ABSENT(XCompositeQueryExtension, 0)
BW_ABSENT(XCompositeQueryVersion, 0)
BW_ABSENT(XCompositeVersion, 0)
BW_STUB(XCompositeCreateRegionFromBorderClip)
BW_STUB(XCompositeNameWindowPixmap)
BW_STUB(XCompositeRedirectSubwindows)
BW_STUB(XCompositeRedirectWindow)
BW_STUB(XCompositeUnredirectSubwindows)
BW_STUB(XCompositeUnredirectWindow)
#elif BW_STUB_LIBRARY == 8 /* libXxf86vm.so.1 */
BW_ABSENT(XF86VidModeQueryExtension, 0)
BW_ABSENT(XF86VidModeQueryVersion, 0)
BW_STUB(XF86VidModeGetAllModeLines)
BW_STUB(XF86VidModeGetGamma)
BW_STUB(XF86VidModeGetGammaRamp)
BW_STUB(XF86VidModeGetGammaRampSize)
BW_STUB(XF86VidModeGetModeLine)
BW_STUB(XF86VidModeLockModeSwitch)
BW_STUB(XF86VidModeSetGamma)
BW_STUB(XF86VidModeSetGammaRamp)
BW_STUB(XF86VidModeSetViewPort)
BW_STUB(XF86VidModeSwitchToMode)
#else
#error "BW_STUB_LIBRARY must select one of the extension libraries"
#endif
