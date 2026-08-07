/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 *
 *  ---------------------------------------------------------------------
 *  The process entry point.
 *
 *  BoxedVN does not use SwiftUI's @main.  SDL's UIKit backend expects to own
 *  UIApplicationMain: SDL_UIKitRunApp installs SDLUIKitDelegate, and that
 *  delegate calls the supplied main function from -postFinishLaunch on the
 *  main thread, then relies on SDL_PumpEvents to service the run loop from
 *  inside it (see SDL_uikitappdelegate.m and SDL_uikitevents.m).
 *
 *  Fighting that arrangement is how SDL ports on iOS break, so BoxedVN adopts
 *  it and hosts SwiftUI inside it instead: BVNAppDelegate subclasses
 *  SDLUIKitDelegate and puts a UIHostingController on screen, and
 *  BVNGuestMain services the run loop whenever no guest is running.
 *  ---------------------------------------------------------------------
 */

#import <UIKit/UIKit.h>

#include <SDL.h>
#include <SDL_main.h>

// SDL_main.h defines `main` to `SDL_main` so that SDL's own shim can be the
// real entry point.  BoxedVN provides the entry point itself, so the macro is
// removed here - the same thing SDL's src/main/uikit/SDL_uikit_main.c does.
#ifdef main
#undef main
#endif

#include "BVNRuntime.h"

int main(int argc, char* argv[]) {
    @autoreleasepool {
        // SDL_UIKitRunApp calls UIApplicationMain with the delegate class
        // named by +[SDLUIKitDelegate getAppDelegateClassName], which
        // BVNAppDelegate overrides in a category.
        return SDL_UIKitRunApp(argc, argv, BVNGuestMain);
    }
}
