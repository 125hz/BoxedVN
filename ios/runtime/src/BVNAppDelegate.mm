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
 *  The application delegate.
 *
 *  SDL's SDL_UIKitRunApp installs the class named by
 *  +[SDLUIKitDelegate getAppDelegateClassName].  SDL documents subclassing it
 *  and overriding that method in a category, which is exactly what happens
 *  here, so BoxedVN gets its own delegate without patching SDL.
 *
 *  The delegate owns one UIWindow for the SwiftUI library UI.  SDL creates a
 *  separate UIWindow of its own when a guest session starts.  Only one of the
 *  two is key and visible at a time, and BVNRuntime drives the swap through
 *  BVNFrontendShowLibrary / BVNFrontendHideLibrary.
 *
 *  The SwiftUI side is reached by name rather than by symbol, because the
 *  Swift application target links against this static library and not the
 *  other way round.  The frontend must declare:
 *
 *      @objc(BoxedVNFrontend) final class BoxedVNFrontend: NSObject {
 *          @objc static func makeRootViewController() -> UIViewController
 *      }
 *  ---------------------------------------------------------------------
 */

#import <UIKit/UIKit.h>

#include "BVNRuntime.h"

// Implemented in BVNLog.mm.  BVNGuestMain also calls this, but not until
// after -postFinishLaunch fires, which is *after* this delegate has already
// created the library window - see the first line of
// -application:didFinishLaunchingWithOptions: below.  Without starting the
// log here first, every diagnostic from window creation (including a failed
// BoxedVNFrontend lookup, the most likely cause of a black screen) lands only
// in the in-memory ring and is never written to a file anyone can read.  The
// function is idempotent, so BVNGuestMain's later call is a harmless no-op.
extern "C" bool BVNLogStartSessionFile(void);

// SDL does not install SDL_uikitappdelegate.h, so the interface BoxedVN
// subclasses is redeclared here.  It is pinned to the SDL2 version in
// scripts/dependencies.lock.sh.
@interface SDLUIKitDelegate : NSObject <UIApplicationDelegate>
+ (id)sharedAppDelegate;
+ (NSString*)getAppDelegateClassName;
- (void)hideLaunchScreen;
@property (nonatomic, strong) UIWindow* window;
@end

@interface BVNAppDelegate : SDLUIKitDelegate
@property (nonatomic, strong) UIWindow* libraryWindow;
@end

// SDL_UIKitRunApp resolves the delegate class with
// [SDLUIKitDelegate getAppDelegateClassName] - a message sent to the
// SDLUIKitDelegate class object BY NAME, not through a subclass reference.
// Objective-C class-method overrides only take effect for a message sent to
// the subclass (or an instance of it); a call site that names the base class
// explicitly, as this one does, always resolves against the base class's own
// method table no matter what subclasses exist. Overriding
// +getAppDelegateClassName inside @implementation BVNAppDelegate below is
// therefore invisible to this call site - without the category, SDL keeps
// instantiating its own plain SDLUIKitDelegate and BVNAppDelegate never runs.
//
// A category on SDLUIKitDelegate itself replaces its method table entry
// directly, which *does* affect every call site, including this one. This is
// exactly what SDL's own subclassing notice above +getAppDelegateClassName in
// SDL_uikitappdelegate.m asks for.
@interface SDLUIKitDelegate (BoxedVN)
@end

@implementation SDLUIKitDelegate (BoxedVN)
+ (NSString*)getAppDelegateClassName {
    return @"BVNAppDelegate";
}
@end

static __weak BVNAppDelegate* gAppDelegate = nil;

@implementation BVNAppDelegate

// SDL implements -window with a custom getter that walks its own video
// device's window list and returns nil when SDL video is not initialized -
// which is the entire time BoxedVN is idle showing the library UI, since
// SDL_Init(VIDEO) does not happen until a guest actually launches.
//
// In the legacy (non-scene) application lifecycle that BoxedVN uses - there
// is deliberately no UIApplicationSceneManifest, because SDL owns
// UIApplicationMain - UIApplicationDelegate.window is how UIKit identifies
// the application's main window. Handing it nil while a real, visible,
// key window exists is simply wrong, and it is exactly the kind of
// app-level presentation bookkeeping that out-of-process remote view
// controllers (UIDocumentPickerViewController, which SwiftUI's
// .fileImporter presents) depend on to wire up their remote view service.
// That fits an otherwise very strange symptom: the picker presents, renders
// real content, and highlights rows on tap, but the selection never
// completes - and only on a physical device, where the picker really is
// out-of-process, not in the Simulator.
//
// Falling back to SDL's own answer first keeps guest sessions behaving
// exactly as SDL expects; the library window is only reported when SDL has
// no window of its own.
- (UIWindow*)window {
    UIWindow* sdlWindow = [super window];
    if (sdlWindow != nil) {
        return sdlWindow;
    }
    return self.libraryWindow;
}

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    // First, unconditionally: everything below can fail in ways that leave
    // no other trace (a black screen is exactly that kind of failure), so
    // logging has to be live before any of it runs.
    BVNLogStartSessionFile();

    gAppDelegate = self;

    // Let SDL install its lifecycle observers and schedule -postFinishLaunch,
    // which is what eventually calls BVNGuestMain on the main thread.
    const BOOL result = [super application:application
             didFinishLaunchingWithOptions:launchOptions];

    [self createLibraryWindow];
    return result;
}

- (void)createLibraryWindow {
    Class frontend = NSClassFromString(@"BoxedVNFrontend");
    if (frontend == nil) {
        BVNLogWrite(BVNLogLevelError, "frontend",
                    "BoxedVNFrontend was not found. The Swift application "
                    "target must declare @objc(BoxedVNFrontend) with a static "
                    "makeRootViewController(); without it there is no UI to "
                    "show.");
        return;
    }

    SEL selector = NSSelectorFromString(@"makeRootViewController");
    if (![frontend respondsToSelector:selector]) {
        BVNLogWrite(BVNLogLevelError, "frontend",
                    "BoxedVNFrontend does not respond to "
                    "makeRootViewController.");
        return;
    }

    // -performSelector returns id; the frontend contract says it is a
    // UIViewController, and it is checked rather than trusted.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
    id controller = [frontend performSelector:selector];
#pragma clang diagnostic pop

    if (![controller isKindOfClass:UIViewController.class]) {
        BVNLogWrite(BVNLogLevelError, "frontend",
                    "BoxedVNFrontend.makeRootViewController() did not return a "
                    "UIViewController.");
        return;
    }

    UIWindowScene* scene = nil;
    for (UIScene* candidate in UIApplication.sharedApplication.connectedScenes) {
        if ([candidate isKindOfClass:UIWindowScene.class]) {
            scene = (UIWindowScene*)candidate;
            break;
        }
    }

    UIWindow* window = scene != nil
                           ? [[UIWindow alloc] initWithWindowScene:scene]
                           : [[UIWindow alloc]
                                 initWithFrame:UIScreen.mainScreen.bounds];
    window.rootViewController = (UIViewController*)controller;
    // Deliberately left at the standard UIWindowLevelNormal - a previous
    // version set this to UIWindowLevelNormal - 1 so a guest session's SDL
    // window (created later, at the default level) would visually cover the
    // library without an explicit hide, on the theory that the level offset
    // was a harmless z-order backstop alongside BVNFrontendHideLibrary's
    // explicit .hidden toggle below. In practice a sub-normal window level is
    // non-standard, and non-standard window levels are a known source of
    // UIKit misbehaving key-window/touch-routing decisions - specifically,
    // it lined up with a real report of UIDocumentPickerViewController (what
    // SwiftUI's .fileImporter presents) accepting taps on rows without
    // acting on them, reproducible only on a physical device (not the
    // Simulator) and only through this window. BVNFrontendHideLibrary
    // already explicitly hides this window before a guest starts (see
    // BVNRuntime.mm's runSession), so the level trick was never load-bearing
    // - removing it costs nothing and removes the most likely interference.
    [window makeKeyAndVisible];
    self.libraryWindow = window;

    BVNLogWrite(BVNLogLevelInfo, "frontend", "library window created");
    BVNRuntimeNotifyFrontendReady();
}

@end

extern "C" void BVNFrontendShowLibrary(void) {
    BVNAppDelegate* delegate = gAppDelegate;
    if (delegate.libraryWindow == nil) {
        return;
    }
    delegate.libraryWindow.hidden = NO;
    [delegate.libraryWindow makeKeyAndVisible];
}

extern "C" void BVNFrontendHideLibrary(void) {
    BVNAppDelegate* delegate = gAppDelegate;
    if (delegate.libraryWindow == nil) {
        return;
    }
    // Resigning key rather than hiding keeps the SwiftUI hierarchy alive, so
    // returning from a session does not lose the library's scroll position or
    // navigation state.
    delegate.libraryWindow.hidden = YES;
}
