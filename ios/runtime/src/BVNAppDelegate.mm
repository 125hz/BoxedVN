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
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <dispatch/dispatch.h>
#include <mutex>
#include <thread>

#include "BVNDXMTDisplay.h"
#include "BVNRuntime.h"
#import "BVNGuestOverlay.h"

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
@property (nonatomic, assign) BOOL guestOrientationLocked;
// The live view handed the screen to the guest for a landscape turn.
@property (nonatomic, assign) BOOL liveViewFullscreen;
- (void)createLibraryWindowForScene:(UIWindowScene*)scene;
- (UIWindow*)superWindowForGuest;
- (void)attachGuestPresentationToHost:(UIView*)host;
- (void)detachGuestPresentationFromHost;
- (void)logPresentationTree:(const char*)stage;
@end

@interface BVNSceneDelegate : NSObject <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow* window;
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
static NSString* const kBVNPreferredOrientationKey =
    @"BoxedVN.preferredOrientation";

static NSInteger BVNPreferredOrientationValue(void) {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    const NSInteger value = [defaults objectForKey:kBVNPreferredOrientationKey]
        ? [defaults integerForKey:kBVNPreferredOrientationKey] : 1;
    return MIN(2, MAX(0, value));
}

// Orientation lock. On (the default), the library and the guest stay in the
// preferred orientation above. Off, both follow the device the way iOS
// reports it, and a landscape turn with the live view on screen hands the
// guest the whole screen (see deviceOrientationDidChange:).
static NSString* const kBVNOrientationLockKey = @"BoxedVN.orientationLock";
static BOOL BVNOrientationLockEnabled(void) {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    return [defaults objectForKey:kBVNOrientationLockKey]
        ? [defaults boolForKey:kBVNOrientationLockKey] : YES;
}
static UIInterfaceOrientationMask BVNPreferredOrientationMask(void) {
    if (!BVNOrientationLockEnabled()) {
        return UIInterfaceOrientationMaskAllButUpsideDown;
    }
    switch (BVNPreferredOrientationValue()) {
        case 0: return UIInterfaceOrientationMaskPortrait;
        case 2: return UIInterfaceOrientationMaskLandscapeLeft;
        default: return UIInterfaceOrientationMaskLandscapeRight;
    }
}

static BOOL BVNOrientationMatchesPreference(UIInterfaceOrientation orientation) {
    if (!BVNOrientationLockEnabled()) {
        return YES;
    }
    switch (BVNPreferredOrientationValue()) {
        case 0: return orientation == UIInterfaceOrientationPortrait;
        case 2: return orientation == UIInterfaceOrientationLandscapeLeft;
        default: return orientation == UIInterfaceOrientationLandscapeRight;
    }
}
static NSMutableDictionary<NSValue*, UIView*>* gGuestVulkanSurfaceViews = nil;
// The live view host (see BVNGuestPresentationSetHostView below). Declared
// here because the class methods above the setter consult it.
static __weak UIView* gGuestPresentationHost = nil;
extern "C" void BVNGuestOverlayInstall(void);
static NSMutableDictionary<NSValue*, UIView*>* gGuestVulkanWaitingOverlays = nil;

// Defined at the bottom of this file, alongside the presentation geometry it
// describes. Declared here because the surface-teardown path above it needs it.
extern "C" void BVNForgetGuestPresentationAspect(void* surface);

// SDL 2.32.10 deliberately keeps its keyboard UITextField hidden and at a
// zero-sized frame. That worked on earlier iOS releases, but current iOS can
// accept -showKeyboard while refusing to make that hidden field first
// responder, producing no keyboard and no error. SDL is pinned, including the
// private ivar name, so use the Objective-C runtime to make the *same* field
// focusable rather than introducing a second text-input pipeline.
static UITextField* BVNSDLKeyboardTextField(UIViewController* controller) {
    if (controller == nil) {
        return nil;
    }
    Ivar textFieldIvar = class_getInstanceVariable(controller.class, "textField");
    if (textFieldIvar == nullptr) {
        return nil;
    }
    id candidate = object_getIvar(controller, textFieldIvar);
    return [candidate isKindOfClass:UITextField.class]
               ? (UITextField*)candidate
               : nil;
}

@implementation BVNAppDelegate

// SDL implements -window with a custom getter that walks its own video
// device's window list and returns nil when SDL video is not initialized -
// which is the entire time BoxedVN is idle showing the library UI, since
// SDL_Init(VIDEO) does not happen until a guest actually launches.
//
// UIApplicationDelegate.window remains part of UIKit's presentation
// bookkeeping even though BVNSceneDelegate owns the library UIWindow.
// Handing it nil while a real, visible, key window exists is simply wrong,
// and it is exactly the kind of
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

// Bypass the fallback in -window when native integration specifically needs
// SDL's UIWindow. Keeping this accessor here also avoids reaching into SDL's
// private video-device structures from the C++ frontend.
- (UIWindow*)superWindowForGuest {
    return [super window];
}

- (void)attachGuestPresentationToHost:(UIView*)host {
    NSAssert(NSThread.isMainThread, @"Guest presentation moves on main");
    UIWindow* guestWindow = [super window];
    UIView* root = guestWindow.rootViewController.view;
    if (host == nil || guestWindow == nil || root == nil) {
        return;
    }
    if (root.superview != host) {
        [root removeFromSuperview];
        root.frame = host.bounds;
        root.autoresizingMask = UIViewAutoresizingFlexibleWidth |
                                UIViewAutoresizingFlexibleHeight;
        [host insertSubview:root atIndex:0];
    }
    // Not hidden: the platform layer shows and raises SDL's window again on
    // the first frame (KNativeScreenSDL::showWindow), which un-hid the
    // emptied window and painted the whole screen black on the first live
    // view run. A transparent, non-interactive window below the library
    // window stays out of sight whatever SDL does to it, while SDL still
    // sees a visible window and keeps drawing.
    guestWindow.windowLevel = UIWindowLevelNormal - 1.0;
    guestWindow.alpha = 0.0;
    guestWindow.userInteractionEnabled = NO;
    guestWindow.hidden = NO;
    self.libraryWindow.windowLevel = UIWindowLevelNormal;
    self.libraryWindow.hidden = NO;
    [self.libraryWindow makeKeyAndVisible];
    BVNGuestOverlayInstall();
    BVNDXMTDisplayAttach();
    NSString* message = [NSString stringWithFormat:
        @"Guest presentation attached to the live view host (%.0fx%.0f pt).",
        host.bounds.size.width, host.bounds.size.height];
    BVNLogWrite(BVNLogLevelInfo, "frontend", message.UTF8String);
    [self logPresentationTree:"attach"];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        [self logPresentationTree:"attach+3s"];
    });
}

// One line per window of the scene plus the host: enough to tell from a
// device log which window is on top, whether it can be seen, and where the
// presentation view actually is.
- (void)logPresentationTree:(const char*)stage {
    UIWindowScene* scene = self.libraryWindow.windowScene;
    NSMutableString* text = [NSMutableString stringWithFormat:
        @"BOXEDVN_PRESENTATION_TREE stage=%s", stage];
    for (UIWindow* window in scene.windows) {
        [text appendFormat:@" | %@ level=%.0f hidden=%d alpha=%.2f key=%d frame=%.0fx%.0f root=%@ subviews=%lu",
            NSStringFromClass([window class]), (double)window.windowLevel,
            window.hidden, window.alpha, window.isKeyWindow,
            window.bounds.size.width, window.bounds.size.height,
            NSStringFromClass([window.rootViewController class]),
            (unsigned long)window.subviews.count];
    }
    UIView* host = gGuestPresentationHost;
    UIView* root = [super window].rootViewController.view;
    [text appendFormat:@" | host=%@ hostWindow=%@ hostFrame=%.0fx%.0f rootSuperview=%@ rootFrame=%.0fx%.0f rootInHost=%d",
        host ? NSStringFromClass([host class]) : @"nil",
        host.window ? NSStringFromClass([host.window class]) : @"nil",
        host.bounds.size.width, host.bounds.size.height,
        root.superview ? NSStringFromClass([root.superview class]) : @"nil",
        root.bounds.size.width, root.bounds.size.height,
        root != nil && root.superview == host];
    BVNLogWrite(BVNLogLevelInfo, "frontend", text.UTF8String);
}

- (void)detachGuestPresentationFromHost {
    NSAssert(NSThread.isMainThread, @"Guest presentation moves on main");
    UIWindow* guestWindow = [super window];
    UIView* root = guestWindow.rootViewController.view;
    if (guestWindow == nil || root == nil) {
        return;
    }
    if (root.superview != guestWindow) {
        [root removeFromSuperview];
        root.frame = guestWindow.bounds;
        root.autoresizingMask = UIViewAutoresizingFlexibleWidth |
                                UIViewAutoresizingFlexibleHeight;
        [guestWindow insertSubview:root atIndex:0];
    }
    guestWindow.windowLevel = UIWindowLevelNormal;
    guestWindow.alpha = 1.0;
    guestWindow.userInteractionEnabled = YES;
    guestWindow.hidden = NO;
    [guestWindow makeKeyAndVisible];
    BVNGuestOverlayInstall();
    BVNDXMTDisplayAttach();
    BVNLogWrite(BVNLogLevelInfo, "frontend",
                "Guest presentation returned to SDL's window.");
    [self logPresentationTree:"detach"];
}

// Landscape with the live view on screen means the guest gets the whole
// screen: the presentation goes back to SDL's window, which is the existing
// full-screen path, and comes back into the host when the device is upright
// again. Only the orientation-lock-off case can rotate; with the lock on the
// interface never turns.
- (void)deviceOrientationDidChange:(NSNotification*)notification {
    if (gGuestPresentationHost == nil || [super window] == nil) {
        return;
    }
    const UIInterfaceOrientation orientation =
        self.libraryWindow.windowScene.interfaceOrientation;
    const BOOL landscape = UIInterfaceOrientationIsLandscape(orientation);
    if (landscape && !self.liveViewFullscreen) {
        self.liveViewFullscreen = YES;
        BVNLogWrite(BVNLogLevelInfo, "frontend",
                    "Landscape: the live view takes the whole screen.");
        [self detachGuestPresentationFromHost];
    } else if (!landscape && self.liveViewFullscreen) {
        self.liveViewFullscreen = NO;
        BVNLogWrite(BVNLogLevelInfo, "frontend",
                    "Portrait: the guest returns to the live view.");
        [self attachGuestPresentationToHost:gGuestPresentationHost];
    }
}

- (void)guestWindowDidBecomeVisible:(NSNotification*)notification {
    // SDL shows its window from inside boxedmain. When a live view host is
    // registered the presentation belongs there, so the window is taken over
    // as soon as it appears rather than covering the library.
    UIWindow* shown = notification.object;
    if (gGuestPresentationHost == nil || shown == nil ||
        shown == self.libraryWindow ||
        (shown != [super window] &&
         ![NSStringFromClass([shown.rootViewController class])
             containsString:@"SDL"])) {
        return;
    }
    UIView* host = gGuestPresentationHost;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (gGuestPresentationHost == host && host != nil) {
            [self attachGuestPresentationToHost:host];
        }
    });
}

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    // First, unconditionally: everything below can fail in ways that leave
    // no other trace (a black screen is exactly that kind of failure), so
    // logging has to be live before any of it runs.
    BVNLogStartSessionFile();

    gAppDelegate = self;
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{
        kBVNPreferredOrientationKey: @1,
    }];

    [NSNotificationCenter.defaultCenter
        addObserver:self
           selector:@selector(guestKeyboardDidShow:)
               name:UIKeyboardDidShowNotification
             object:nil];
    [NSNotificationCenter.defaultCenter
        addObserver:self
           selector:@selector(guestKeyboardDidHide:)
               name:UIKeyboardDidHideNotification
             object:nil];
    [NSNotificationCenter.defaultCenter
        addObserver:self
           selector:@selector(guestWindowDidBecomeVisible:)
               name:UIWindowDidBecomeVisibleNotification
             object:nil];

    // Let SDL install its lifecycle observers and schedule -postFinishLaunch,
    // which is what eventually calls BVNGuestMain on the main thread.
    const BOOL result = [super application:application
             didFinishLaunchingWithOptions:launchOptions];

    return result;
}

- (UIInterfaceOrientationMask)application:(UIApplication*)application
    supportedInterfaceOrientationsForWindow:(UIWindow*)window {
    // A single persisted orientation owns the library and the guest. This
    // avoids changing UIWindow/Metal geometry during launch or play, which is
    // the transition that repeatedly invalidated UIKit's touch responder
    // chain on physical devices.
    return BVNPreferredOrientationMask();
}

- (void)createLibraryWindowForScene:(UIWindowScene*)scene {
    if (self.libraryWindow != nil) {
        return;
    }

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

    // Ask for the preferred orientation BEFORE the window is shown.
    //
    // Nothing used to apply it until a guest was launched, so the library
    // appeared in whatever orientation iOS chose from Info.plist - which lists
    // portrait, so a device held in portrait got a portrait window - and UIKit
    // then consulted this delegate, saw a landscape-only mask, and rotated a
    // window the user was already looking at. That is the visible spin at app
    // start: not a transient glitch but the app correcting itself in public.
    //
    // Requesting the geometry against the scene first means the correction
    // happens before anything is on screen. It is a request, not a guarantee -
    // UIKit may still refuse - so the error is logged rather than assumed
    // away, and the existing pre-guest confirmation is left in place.
    UIWindowSceneGeometryPreferencesIOS* preferences =
        [[UIWindowSceneGeometryPreferencesIOS alloc]
            initWithInterfaceOrientations:BVNPreferredOrientationMask()];
    [scene requestGeometryUpdateWithPreferences:preferences
                                   errorHandler:^(NSError* error) {
        NSString* message = [NSString stringWithFormat:
            @"UIKit did not apply the preferred orientation before the "
            @"library window was shown, so the app may visibly rotate: %@",
            error.localizedDescription];
        BVNLogWrite(BVNLogLevelWarning, "frontend", message.UTF8String);
    }];

    UIWindow* window = [[UIWindow alloc] initWithWindowScene:scene];
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
    // Show the window, but not its contents, until the orientation is the one
    // the user chose.
    //
    // Requesting the geometry above is not enough on its own: UIKit applies it
    // asynchronously and animates the change, so a scene that connected in
    // portrait rotates the library in front of the user for about a second.
    // The launch screen is a plain colour, so the only thing whose rotation is
    // visible is this window's contents - which means hiding them until the
    // orientation settles removes the symptom entirely, whatever UIKit's
    // timing turns out to be. That is a guarantee; winning a race with UIKit
    // is not.
    //
    // The wait is the same bounded poll the guest path already uses, so a
    // preference UIKit never applies costs three seconds and then shows the
    // library anyway rather than leaving a permanently blank screen.
    window.alpha = 0.0;
    [window makeKeyAndVisible];
    self.libraryWindow = window;

    __weak UIWindow* weakWindow = window;
    [self waitForPreferredOrientationWithAttempt:0 completion:^{
        UIWindow* settled = weakWindow;
        if (settled == nil) {
            return;
        }
        // A short fade rather than a hard cut: the window has been on screen
        // as a plain colour for a frame or two, and appearing abruptly reads
        // as a glitch of its own.
        [UIView animateWithDuration:0.12 animations:^{
            settled.alpha = 1.0;
        }];
    }];

    BVNLogWrite(BVNLogLevelInfo, "frontend",
                "scene-owned library window created");
    BVNRuntimeNotifyFrontendReady();
}

- (void)waitForPreferredOrientationWithAttempt:(NSUInteger)attempt
                                     completion:(dispatch_block_t)completion {
    UIWindowScene* scene = self.libraryWindow.windowScene;
    const CGSize size = self.libraryWindow.bounds.size;
    const BOOL wantsPortrait = BVNPreferredOrientationValue() == 0;
    const BOOL boundsMatch = wantsPortrait ? size.height >= size.width
                                           : size.width >= size.height;
    const BOOL sceneMatches = scene == nil ||
        BVNOrientationMatchesPreference(scene.interfaceOrientation);

    // Waiting for both values matters. interfaceOrientation changes before
    // the final layout pass on some iOS releases; creating SDL in that gap
    // gives its CAMetalLayer the portrait drawable which caused the stretched
    // first frame and subsequent rotation freezes reported on build 15.
    if (boundsMatch && sceneMatches) {
        dispatch_async(dispatch_get_main_queue(), ^{
            BVNLogWrite(BVNLogLevelInfo, "frontend",
                        "Preferred app geometry settled before SDL startup.");
            completion();
        });
        return;
    }

    if (attempt >= 180) {
        BVNLogWrite(BVNLogLevelWarning, "frontend",
                    "Preferred geometry did not settle within three seconds; "
                    "continuing so the launch request cannot remain stuck.");
        completion();
        return;
    }

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 (int64_t)(NSEC_PER_SEC / 60)),
                   dispatch_get_main_queue(), ^{
        [self waitForPreferredOrientationWithAttempt:attempt + 1
                                           completion:completion];
    });
}

- (void)prepareGuestPresentationWithCompletion:(dispatch_block_t)completion {
    NSAssert(NSThread.isMainThread, @"Guest presentation must be prepared on main");

    if (gGuestPresentationHost != nil) {
        // Embedded in the library page: no orientation lock and no wait for
        // the preferred geometry; the host's bounds are the geometry.
        completion();
        return;
    }
    UIWindowScene* scene = self.libraryWindow.windowScene;
    self.guestOrientationLocked = YES;
    [self.libraryWindow.rootViewController
        setNeedsUpdateOfSupportedInterfaceOrientations];

    if (scene == nil) {
        BVNLogWrite(BVNLogLevelWarning, "frontend",
                    "No UIWindowScene was available for the preferred guest "
                    "request; continuing with the current geometry.");
        completion();
        return;
    }

    BVNLogWrite(BVNLogLevelInfo, "frontend",
                "Confirming the locked app orientation before "
                "creating SDL's guest window.");
    UIWindowSceneGeometryPreferencesIOS* preferences =
        [[UIWindowSceneGeometryPreferencesIOS alloc]
            initWithInterfaceOrientations:BVNPreferredOrientationMask()];
    // The launch request originates in a SwiftUI button action. Let that
    // touch-up transaction finish before changing scene geometry; rotating
    // inside it can leave UIKit's window-level touch delivery permanently in
    // a cancelled/tracking state until the process restarts.
    dispatch_async(dispatch_get_main_queue(), ^{
        [scene requestGeometryUpdateWithPreferences:preferences
                                      errorHandler:^(NSError* error) {
            NSString* message = [NSString stringWithFormat:
                @"UIKit did not apply the preferred app geometry: %@",
                error.localizedDescription];
            BVNLogWrite(BVNLogLevelWarning, "frontend", message.UTF8String);
        }];
        [self waitForPreferredOrientationWithAttempt:0 completion:completion];
    });
}

- (void)finishGuestPresentation {
    NSAssert(NSThread.isMainThread, @"Guest presentation must finish on main");
    self.guestOrientationLocked = NO;
    [self.libraryWindow.rootViewController
        setNeedsUpdateOfSupportedInterfaceOrientations];
}

- (BOOL)setGuestKeyboardVisible:(BOOL)visible {
    NSAssert(NSThread.isMainThread, @"Guest keyboard must be changed on main");
    UIWindow* guestWindow = [super window];
    UIViewController* guestController = guestWindow.rootViewController;
    SEL action = NSSelectorFromString(visible ? @"showKeyboard" : @"hideKeyboard");
    if (guestController == nil || ![guestController respondsToSelector:action]) {
        BVNLogWrite(BVNLogLevelWarning, "input",
                    "SDL's UIKit keyboard controller was not available.");
        return NO;
    }

    UITextField* textField = BVNSDLKeyboardTextField(guestController);
    if (textField == nil) {
        BVNLogWrite(BVNLogLevelWarning, "input",
                    "SDL's UIKit keyboard text field was not found.");
        return NO;
    }

    if (visible) {
        // Current iOS accepted first-responder status for SDL's hidden/1x1,
        // nearly-transparent field without creating a keyboard scene. Give
        // the existing SDL field a normal control-sized, fully participating
        // view. Its colors remain clear so only BoxedVN's SDL keyboard button
        // is visible, but alpha is deliberately 1.0: UIKit may suppress input
        // presentation for an effectively invisible responder.
        textField.hidden = NO;
        textField.alpha = 1.0;
        textField.frame = CGRectMake(0.0,
                                     MAX(0.0, guestController.view.bounds.size.height - 44.0),
                                     44.0, 44.0);
        textField.backgroundColor = UIColor.clearColor;
        textField.textColor = UIColor.clearColor;
        textField.tintColor = UIColor.clearColor;
        textField.userInteractionEnabled = YES;
        textField.accessibilityElementsHidden = YES;
        if (textField.superview != guestController.view) {
            [textField removeFromSuperview];
            [guestController.view addSubview:textField];
        }
        if (gGuestPresentationHost == nil) {
            [guestWindow makeKeyAndVisible];
        }
        [guestController.view layoutIfNeeded];
    }

    // Keep SDL's controller bookkeeping/notifications intact, then verify the
    // real result instead of logging that a void method was merely invoked.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
    [guestController performSelector:action];
#pragma clang diagnostic pop

    if (visible && !textField.isFirstResponder) {
        [textField becomeFirstResponder];
    }
    if (visible) {
        [textField reloadInputViews];
    }
    if (!visible && textField.isFirstResponder) {
        [textField resignFirstResponder];
    }

    const BOOL succeeded = visible ? textField.isFirstResponder
                                   : !textField.isFirstResponder;
    NSString* diagnostic = [NSString stringWithFormat:
        @"SDL keyboard %@: keyWindow=%@ attached=%@ hidden=%@ "
         "firstResponder=%@",
        visible ? @"show" : @"hide",
        guestWindow.isKeyWindow ? @"yes" : @"no",
        textField.window != nil ? @"yes" : @"no",
        textField.hidden ? @"yes" : @"no",
        textField.isFirstResponder ? @"yes" : @"no"];
    BVNLogWrite(succeeded ? BVNLogLevelInfo : BVNLogLevelWarning,
                "input", diagnostic.UTF8String);
    return succeeded;
}

- (void)guestKeyboardDidShow:(NSNotification*)notification {
    if (self.guestOrientationLocked) {
        BVNLogWrite(BVNLogLevelInfo, "input",
                    "UIKit confirmed software keyboard did show.");
    }
}

- (void)guestKeyboardDidHide:(NSNotification*)notification {
    if (self.guestOrientationLocked) {
        BVNLogWrite(BVNLogLevelInfo, "input",
                    "UIKit confirmed software keyboard did hide.");
    }
}

@end

@implementation BVNSceneDelegate

- (void)scene:(UIScene*)scene
    willConnectToSession:(UISceneSession*)session
                 options:(UISceneConnectionOptions*)connectionOptions {
    if (![scene isKindOfClass:UIWindowScene.class]) {
        return;
    }

    BVNAppDelegate* delegate = gAppDelegate;
    [delegate createLibraryWindowForScene:(UIWindowScene*)scene];
    self.window = delegate.libraryWindow;
    BVNLogWrite(BVNLogLevelInfo, "frontend",
                "UIWindowScene connected to BoxedVN library");
}

@end

// ---------------------------------------------------------------------------
// Session display-rate preference
//
// Keep a live display-link preference while a guest session is active. Build
// Device present-path timing disproved adaptive-refresh starvation for a
// low-frame-rate GDI workload: the guest initiated only two presents in five
// seconds and both returned immediately. Its workaround therefore lives in the
// X11 input path; this link remains only as the session's smooth-display hint.
// Its callback deliberately does no work.
@interface BVNRefreshRateHold : NSObject
@property (nonatomic, strong) CADisplayLink* link;
@end

@implementation BVNRefreshRateHold
- (void)tick:(CADisplayLink*)link {
    (void)link;
}
@end

static BVNRefreshRateHold* gRefreshRateHold = nil;
static NSString* const kBVNFrameRateModeKey =
    @"BoxedVN.presentation.frameRateMode";
static std::atomic<int> gGuestFrameRateLimitHz{0};
static std::mutex gGuestFramePacerMutex;
static std::chrono::steady_clock::time_point gGuestNextFrameDeadline;

extern "C" int BVNGuestFrameRateMode(void) {
    return MAX(0, MIN(2, (int)[[NSUserDefaults standardUserDefaults]
        integerForKey:kBVNFrameRateModeKey]));
}

static void BVNResetGuestFramePacer(int mode) {
    gGuestFrameRateLimitHz.store(mode == 1 ? 60 : mode == 2 ? 120 : 0,
                                 std::memory_order_release);
    std::lock_guard<std::mutex> lock(gGuestFramePacerMutex);
    gGuestNextFrameDeadline = {};
}

extern "C" void BVNGuestFrameLimiterWait(void) {
    const int limit = gGuestFrameRateLimitHz.load(std::memory_order_acquire);
    if (limit <= 0) {
        return;
    }
    const auto interval = std::chrono::nanoseconds(1000000000ll / limit);
    std::lock_guard<std::mutex> lock(gGuestFramePacerMutex);
    const auto now = std::chrono::steady_clock::now();
    if (gGuestNextFrameDeadline.time_since_epoch().count() == 0 ||
        now > gGuestNextFrameDeadline + interval * 2) {
        gGuestNextFrameDeadline = now;
        return;
    }
    gGuestNextFrameDeadline += interval;
    if (gGuestNextFrameDeadline > now) {
        std::this_thread::sleep_until(gGuestNextFrameDeadline);
    }
}

static void BVNApplyGuestFrameRateMode(void) {
    if (gRefreshRateHold.link == nil) {
        return;
    }
    UIWindow* guestWindow = BVNGuestUIWindow();
    UIScreen* screen = guestWindow.screen ?: gAppDelegate.libraryWindow.screen;
    const NSInteger deviceMaximum = MAX(
        60, screen.maximumFramesPerSecond);
    const int mode = BVNGuestFrameRateMode();
    const NSInteger requested = mode == 1 ? 60
                                : mode == 2 ? 120
                                            : deviceMaximum;
    const NSInteger ceiling = MIN(requested, deviceMaximum);
    if (@available(iOS 15.0, *)) {
        if (mode == 0) {
            // Let ProMotion vary freely up to the panel limit while preferring
            // its fastest cadence. The OS may still lower this for power or
            // thermal reasons.
            gRefreshRateHold.link.preferredFrameRateRange =
                CAFrameRateRangeMake(30.0f, (float)deviceMaximum,
                                     (float)deviceMaximum);
        } else {
            gRefreshRateHold.link.preferredFrameRateRange =
                CAFrameRateRangeMake((float)ceiling, (float)ceiling,
                                     (float)ceiling);
        }
    } else {
        gRefreshRateHold.link.preferredFramesPerSecond =
            mode == 0 ? 0 : (NSInteger)ceiling;
    }
}

extern "C" void BVNGuestSetFrameRateMode(int mode) {
    NSCAssert(NSThread.isMainThread,
              @"Guest frame-rate mode must change on the main thread");
    mode = MAX(0, MIN(2, mode));
    [[NSUserDefaults standardUserDefaults]
        setInteger:mode forKey:kBVNFrameRateModeKey];
    BVNResetGuestFramePacer(mode);
    BVNApplyGuestFrameRateMode();
    NSString* message = [NSString stringWithFormat:
        @"Guest display cadence set to %@.",
        mode == 1 ? @"60 FPS" : mode == 2 ? @"120 FPS" : @"uncapped"];
    BVNLogWrite(BVNLogLevelInfo, "frontend", message.UTF8String);
}

static void BVNStartRefreshRateHold(void) {
    if (gRefreshRateHold != nil) {
        return;
    }
    gRefreshRateHold = [[BVNRefreshRateHold alloc] init];
    CADisplayLink* link =
        [CADisplayLink displayLinkWithTarget:gRefreshRateHold
                                    selector:@selector(tick:)];
    // Common modes, so it keeps firing while UIKit is tracking a touch. The
    // guest owns the main thread, but SDL's UIKit_PumpEvents cycles the main
    // run loop on every SDL_PollEvent, which is often enough to keep this
    // scheduled.
    [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    gRefreshRateHold.link = link;
    BVNResetGuestFramePacer(BVNGuestFrameRateMode());
    BVNApplyGuestFrameRateMode();
    const int mode = BVNGuestFrameRateMode();
    BVNLogWrite(BVNLogLevelInfo, "frontend",
                mode == 1 ? "Requesting a locked 60 Hz guest display cadence."
                : mode == 2
                    ? "Requesting a locked 120 Hz guest display cadence."
                    : "Requesting the fastest adaptive guest display cadence.");
}

static void BVNStopRefreshRateHold(void) {
    if (gRefreshRateHold == nil) {
        return;
    }
    [gRefreshRateHold.link invalidate];
    BVNResetGuestFramePacer(0);
    gRefreshRateHold.link = nil;
    gRefreshRateHold = nil;
}

extern "C" void BVNPrepareGuestPresentation(dispatch_block_t completion) {
    NSCAssert(NSThread.isMainThread, @"Guest startup must run on main");
    BVNStartRefreshRateHold();
    [gAppDelegate prepareGuestPresentationWithCompletion:completion];
}

extern "C" void BVNFinishGuestPresentation(void) {
    NSCAssert(NSThread.isMainThread, @"Guest cleanup must run on main");
    // SDL owns the actual guest window/view teardown. Drop any diagnostic
    // associations left by a forced Wine exit so they cannot retain UIKit
    // views or be mistaken for surfaces in the next session.
    BVNStopRefreshRateHold();
    BVNGuestOverlayRemove();
    [gGuestVulkanSurfaceViews removeAllObjects];
    [gGuestVulkanWaitingOverlays removeAllObjects];
    BVNForgetGuestPresentationAspect(nullptr);
    BVNDXMTDisplayFinish();
    [gAppDelegate finishGuestPresentation];
}

extern "C" UIWindow* BVNGuestUIWindow(void) {
    return [gAppDelegate superWindowForGuest];
}

// The live view host. While one is registered the guest's presentation
// lives inside it: SDL's root view becomes its bottom subview, the DXMT
// layer and the overlay attach above, and SDL's window hides so the library
// window keeps the screen. SDL still owns its window and view controller;
// only the view hierarchy moves, which is what its touch handling and layout
// size reporting key on.

extern "C" UIView* BVNGuestPresentationHostView(void) {
    return NSThread.isMainThread ? gGuestPresentationHost : nil;
}

extern "C" void BVNGuestPresentationSetHostView(void* pointer) {
    if (!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(), ^{
            BVNGuestPresentationSetHostView(pointer);
        });
        return;
    }
    UIView* host = (__bridge UIView*)pointer;
    gGuestPresentationHost = host;
    if (host != nil) {
        if (!gAppDelegate.liveViewFullscreen) {
            [gAppDelegate attachGuestPresentationToHost:host];
        }
    } else {
        gAppDelegate.liveViewFullscreen = NO;
        [gAppDelegate detachGuestPresentationFromHost];
    }
}

extern "C" const char* BVNGuestPreferredOrientationHint(void) {
    switch (BVNPreferredOrientationValue()) {
        case 0: return "Portrait";
        case 2: return "LandscapeLeft";
        default: return "LandscapeRight";
    }
}

extern "C" void BVNApplyPreferredOrientation(void) {
    if (!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(), ^{
            BVNApplyPreferredOrientation();
        });
        return;
    }
    BVNAppDelegate* delegate = gAppDelegate;
    if (delegate == nil) {
        return;
    }
    UIWindow* guestWindow = [delegate superWindowForGuest];
    [guestWindow.rootViewController
        setNeedsUpdateOfSupportedInterfaceOrientations];
    [delegate.libraryWindow.rootViewController
        setNeedsUpdateOfSupportedInterfaceOrientations];

    UIWindowScene* scene = guestWindow.windowScene
                               ?: delegate.libraryWindow.windowScene;
    if (scene != nil) {
        UIWindowSceneGeometryPreferencesIOS* preferences =
            [[UIWindowSceneGeometryPreferencesIOS alloc]
                initWithInterfaceOrientations:BVNPreferredOrientationMask()];
        [scene requestGeometryUpdateWithPreferences:preferences
                                       errorHandler:^(NSError* error) {
            NSString* message = [NSString stringWithFormat:
                @"UIKit did not apply the selected app orientation: %@",
                error.localizedDescription];
            BVNLogWrite(BVNLogLevelWarning, "frontend", message.UTF8String);
        }];
    }

    NSString* message = [NSString stringWithFormat:
        @"App orientation locked to %@.",
        BVNPreferredOrientationValue() == 0 ? @"portrait"
            : BVNPreferredOrientationValue() == 2 ? @"landscape flipped"
                                                   : @"landscape"];
    BVNLogWrite(BVNLogLevelInfo, "frontend", message.UTF8String);
}

extern "C" bool BVNGuestKeyboardSetVisible(bool visible) {
    if (!NSThread.isMainThread) {
        BVNLogWrite(BVNLogLevelWarning, "input",
                    "Ignored a guest keyboard request made off the main thread.");
        return false;
    }
    return [gAppDelegate setGuestKeyboardVisible:visible ? YES : NO] == YES;
}

extern "C" void BVNAttachGuestWindowToScene(void) {
    if (!NSThread.isMainThread) {
        BVNLogWrite(BVNLogLevelWarning, "frontend",
                    "Ignored off-main guest scene attachment request.");
        return;
    }

    BVNAppDelegate* delegate = gAppDelegate;
    UIWindow* guestWindow = [delegate superWindowForGuest];
    UIWindowScene* scene = delegate.libraryWindow.windowScene;
    if (guestWindow == nil || scene == nil) {
        BVNLogWrite(BVNLogLevelWarning, "frontend",
                    "Could not attach SDL guest window to the library scene.");
        return;
    }

    guestWindow.windowScene = scene;
    // The SDL renderer/Metal view may preserve aspect ratio inside this
    // hierarchy. Every ancestor behind it must therefore be black or a blue
    // UIKit/Boxedwine default leaks through as the letterbox colour.
    guestWindow.backgroundColor = UIColor.blackColor;
    guestWindow.rootViewController.view.backgroundColor = UIColor.blackColor;
    guestWindow.rootViewController.view.superview.backgroundColor =
        UIColor.blackColor;
    BVNDXMTDisplayAttach();
    BVNLogWrite(BVNLogLevelInfo, "frontend",
                "SDL guest window attached to the active UIWindowScene.");
    // With a live view host registered, the presentation belongs inside the
    // library page: SDL's view moves there now and the layer and overlay
    // follow through the attach. The window-visibility notification was the
    // only trigger before, and a device run showed it never matching SDL's
    // window: the overlay and the DXMT layer went into the host while SDL's
    // window covered the screen, so neither touches nor frames were seen.
    if (gGuestPresentationHost != nil) {
        [delegate attachGuestPresentationToHost:gGuestPresentationHost];
        return;
    }

    // SDL recreates its window when the guest switches from the software
    // renderer to Vulkan, and this runs for each one, so the overlay is
    // (re)attached to whichever window is current. Install is idempotent.
    BVNGuestOverlayInstall();
}

extern "C" void BVNRegisterGuestVulkanSurface(void* surface) {
    NSCAssert(NSThread.isMainThread,
              @"Vulkan presentation registration must run on main");
    if (surface == nullptr) {
        return;
    }

    BVNAppDelegate* delegate = gAppDelegate;
    UIWindow* guestWindow = [delegate superWindowForGuest];
    UIView* view = guestWindow.rootViewController.view;
    if (view == nil) {
        BVNLogWrite(BVNLogLevelWarning, "graphics",
                    "SDL created a Vulkan surface without an active UIKit "
                    "Metal view; surface teardown cannot restore the prior "
                    "guest presentation.");
        return;
    }

    if (gGuestVulkanSurfaceViews == nil) {
        gGuestVulkanSurfaceViews = [NSMutableDictionary dictionary];
    }
    NSValue* key = [NSValue valueWithPointer:surface];
    gGuestVulkanSurfaceViews[key] = view;

    // SDL installs the CAMetalLayer as soon as vkCreateSurface succeeds,
    // before the guest has submitted or presented anything. An untouched
    // CAMetalLayer is opaque black, which made a healthy game still building
    // its first frame indistinguishable from an emulator freeze. Keep the
    // requested black presentation backing and distinguish startup with a
    // native, non-interactive progress overlay. It is removed by the first
    // successful vkQueuePresentKHR.
    view.backgroundColor = UIColor.blackColor;
    view.layer.backgroundColor = UIColor.blackColor.CGColor;

    UIView* overlay = [[UIView alloc] initWithFrame:view.bounds];
    overlay.autoresizingMask = UIViewAutoresizingFlexibleWidth |
                               UIViewAutoresizingFlexibleHeight;
    overlay.userInteractionEnabled = NO;
    overlay.backgroundColor = UIColor.clearColor;

    UIActivityIndicatorView* spinner = [[UIActivityIndicatorView alloc]
        initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleLarge];
    spinner.color = UIColor.whiteColor;
    spinner.translatesAutoresizingMaskIntoConstraints = NO;
    [spinner startAnimating];

    UILabel* title = [[UILabel alloc] initWithFrame:CGRectZero];
    title.text = @"STARTING GAME";
    title.textColor = UIColor.whiteColor;
    title.font = [UIFont monospacedSystemFontOfSize:24.0
                                            weight:UIFontWeightSemibold];
    title.textAlignment = NSTextAlignmentCenter;
    title.translatesAutoresizingMaskIntoConstraints = NO;

    UILabel* detail = [[UILabel alloc] initWithFrame:CGRectZero];
    detail.text = @"WAITING FOR THE FIRST VIDEO FRAME";
    detail.textColor = [UIColor colorWithWhite:1.0 alpha:0.75];
    detail.font = [UIFont monospacedSystemFontOfSize:12.0
                                             weight:UIFontWeightRegular];
    detail.textAlignment = NSTextAlignmentCenter;
    detail.translatesAutoresizingMaskIntoConstraints = NO;

    [overlay addSubview:spinner];
    [overlay addSubview:title];
    [overlay addSubview:detail];
    [NSLayoutConstraint activateConstraints:@[
        [spinner.centerXAnchor constraintEqualToAnchor:overlay.centerXAnchor],
        [spinner.centerYAnchor constraintEqualToAnchor:overlay.centerYAnchor
                                              constant:-34.0],
        [title.topAnchor constraintEqualToAnchor:spinner.bottomAnchor
                                         constant:16.0],
        [title.centerXAnchor constraintEqualToAnchor:overlay.centerXAnchor],
        [detail.topAnchor constraintEqualToAnchor:title.bottomAnchor
                                          constant:8.0],
        [detail.centerXAnchor constraintEqualToAnchor:overlay.centerXAnchor],
    ]];
    [view addSubview:overlay];
    if (gGuestVulkanWaitingOverlays == nil) {
        gGuestVulkanWaitingOverlays = [NSMutableDictionary dictionary];
    }
    gGuestVulkanWaitingOverlays[key] = overlay;

    // SDL installs each new SDL_uikitmetalview by assigning it as the root
    // view controller's view, which appends it to the window and buries
    // anything already there. Re-assert the overlay's place on top; the call
    // is idempotent.
    BVNGuestOverlayInstall();

    NSString* message = [NSString stringWithFormat:
        @"Registered Vulkan surface %p with %@ at %.0fx%.0f; %lu UIKit "
         "surface view(s) active.",
        surface, NSStringFromClass(view.class), view.bounds.size.width,
        view.bounds.size.height,
        (unsigned long)gGuestVulkanSurfaceViews.count];
    BVNLogWrite(BVNLogLevelInfo, "graphics", message.UTF8String);
}

extern "C" void BVNGuestVulkanSurfaceDidPresent(void* surface) {
    NSCAssert(NSThread.isMainThread,
              @"Vulkan first-present UI transition must run on main");
    NSValue* key = [NSValue valueWithPointer:surface];
    UIView* overlay = gGuestVulkanWaitingOverlays[key];
    if (overlay == nil) {
        return;
    }
    [overlay removeFromSuperview];
    [gGuestVulkanWaitingOverlays removeObjectForKey:key];
    // Keep the presentation backing black after startup as well, so resize or
    // letterbox gaps never expose a host colour.
    UIView* surfaceView = gGuestVulkanSurfaceViews[key];
    surfaceView.backgroundColor = UIColor.blackColor;
    surfaceView.layer.backgroundColor = UIColor.blackColor.CGColor;
    NSString* message = [NSString stringWithFormat:
        @"Vulkan surface %p completed its first successful queue present; "
         "removed the native first-frame indicator.", surface];
    BVNLogWrite(BVNLogLevelInfo, "graphics", message.UTF8String);
}

extern "C" void BVNUnregisterGuestVulkanSurface(void* surface) {
    NSCAssert(NSThread.isMainThread,
              @"Vulkan presentation teardown must run on main");
    NSValue* key = [NSValue valueWithPointer:surface];
    UIView* view = gGuestVulkanSurfaceViews[key];
    if (view == nil) {
        BVNLogWrite(BVNLogLevelWarning, "graphics",
                    "A Vulkan surface was destroyed without a matching "
                    "UIKit Metal view.");
        return;
    }

    // SDL2's UIKit Vulkan backend pushes one SDL_uikitmetalview for each
    // SDL_Vulkan_CreateSurface call, but its public API has no corresponding
    // surface-destroy hook. Its private -setSDLWindow:nil path removes the
    // exact view and restores the next-oldest live view. SDL is version-pinned
    // in dependencies.lock.sh, and we verify the selector before invoking it.
    SEL selector = NSSelectorFromString(@"setSDLWindow:");
    if ([view respondsToSelector:selector]) {
        using SetSDLWindowFunction = void (*)(id, SEL, void*);
        SetSDLWindowFunction function =
            reinterpret_cast<SetSDLWindowFunction>([view methodForSelector:selector]);
        function(view, selector, nullptr);
    } else {
        [view removeFromSuperview];
        BVNLogWrite(BVNLogLevelWarning, "graphics",
                    "SDL's Vulkan Metal view no longer implements "
                    "setSDLWindow:; removed it without SDL stack repair.");
    }
    [gGuestVulkanSurfaceViews removeObjectForKey:key];
    UIView* overlay = gGuestVulkanWaitingOverlays[key];
    [overlay removeFromSuperview];
    [gGuestVulkanWaitingOverlays removeObjectForKey:key];
    BVNForgetGuestPresentationAspect(surface);

    NSString* message = [NSString stringWithFormat:
        @"Detached UIKit view for Vulkan surface %p; %lu surface view(s) "
         "remain.",
        surface, (unsigned long)gGuestVulkanSurfaceViews.count];
    BVNLogWrite(BVNLogLevelInfo, "graphics", message.UTF8String);
}

// Letterbox the guest picture instead of stretching it across the display.
//
// SDL sizes its Metal view to the whole window, so a 4:3 guest was scaled to
// fill a 2.2:1 screen: content spread edge to edge and ran under the Dynamic
// Island. Shrinking the view to an aspect-fit rectangle fixes the shape.
//
// It may also bear on something much more expensive, which was not obvious
// until the build-63 device log was counted: 17,879 presentation swapchains
// created in a 150-second session, every acquire and present returning
// VK_SUBOPTIMAL_KHR. MoltenVK raises that whenever the layer's natural
// drawable size (bounds * contentsScale, which SDL_uikitmetalview.layoutSubviews
// assigns) differs from the extent the swapchain was created at, and a
// full-window view (2622x1206) driving an 800x600 swapchain can never satisfy
// it. Whether shrinking the view ends the storm depends on DXVK adopting
// currentExtent, which is NOT verified here - see section 4 of
// docs/CONTINUING_WITHOUT_A_MAC.md and the rate report in
// KVulkdanSDLImpl::registerVulkanSwapchain, which is armed to answer it from
// the next device log.
//
// The fit is computed in pixels and snapped to a whole number of points either
// way: a fractional bounds width turns into a drawableSize that is one pixel
// off, and one pixel off is enough to keep VK_SUBOPTIMAL_KHR coming forever.
//
// KNativeScreenSDL computes the matching inverse for pointer events from the
// rectangle recorded here. The two must agree; if one changes the other has to
// change with it.
static CGRect gGuestPresentationContentRect = CGRectZero;
@interface BVNX11PatchView : UIView
- (BOOL)configureForPixelWidth:(uint32_t)width
                        height:(uint32_t)height;
- (BOOL)presentPixelData:(NSData*)pixelData;
@end

@implementation BVNX11PatchView

{
    id<MTLCommandQueue> _commandQueue;
    id<MTLTexture> _patchTexture;
}

+ (Class)layerClass {
    return CAMetalLayer.class;
}

- (BOOL)configureForPixelWidth:(uint32_t)width
                        height:(uint32_t)height {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil || width == 0 || height == 0) {
        return NO;
    }
    _commandQueue = [device newCommandQueue];
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                                  MTLPixelFormatBGRA8Unorm
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    // Metal does not require a usage flag for blit sources. Unknown lets the
    // driver infer this texture's replaceRegion + blit-copy access pattern.
    descriptor.usage = MTLTextureUsageUnknown;
    _patchTexture = [device newTextureWithDescriptor:descriptor];
    if (_commandQueue == nil || _patchTexture == nil) {
        return NO;
    }

    CAMetalLayer* layer = (CAMetalLayer*)self.layer;
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;
    layer.opaque = NO;
    layer.backgroundColor = UIColor.clearColor.CGColor;
    layer.contentsScale = 1.0;
    layer.drawableSize = CGSizeMake(width, height);
    layer.presentsWithTransaction = NO;
    return YES;
}

- (BOOL)presentPixelData:(NSData*)pixelData {
    if (_patchTexture == nil || pixelData == nil ||
        pixelData.length < _patchTexture.width * _patchTexture.height * 4) {
        return NO;
    }

    // Keep one shared Metal texture for the lifetime of the guest surface.
    // Core Animation's UIView backing-store path retained a small amount of
    // state for every forced partial draw/flush while SDL owned the main run
    // loop. At 20-45 GDI patches/sec that grew resident memory by
    // roughly 220 MB in three minutes, then presentation stopped near the
    // device limit. Uploading into this fixed allocation gives CA only its
    // normal bounded drawable pool, independent of the number of patches.
    const MTLRegion fullRegion = MTLRegionMake2D(
        0, 0, _patchTexture.width, _patchTexture.height);
    [_patchTexture replaceRegion:fullRegion
                     mipmapLevel:0
                       withBytes:pixelData.bytes
                     bytesPerRow:_patchTexture.width * 4];

    CAMetalLayer* layer = (CAMetalLayer*)self.layer;
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    if (drawable == nil || commandBuffer == nil) {
        return NO;
    }
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    [blit copyFromTexture:_patchTexture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(_patchTexture.width,
                                      _patchTexture.height, 1)
                toTexture:drawable.texture
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
    return YES;
}

@end

static BVNX11PatchView* gGuestX11PatchView = nil;
static NSMutableData* gGuestX11PatchPixels = nil;
static uint32_t gGuestX11PatchWidth = 0;
static uint32_t gGuestX11PatchHeight = 0;
static uint32_t gGuestX11ActiveWindow = 0;
static std::atomic<bool> gGuestX11PatchVisible{false};
static std::atomic<uint64_t> gGuestX11PatchCount{0};

extern "C" uint64_t BVNGuestX11PatchCount(void) {
    return gGuestX11PatchCount.load(std::memory_order_relaxed);
}

static void BVNRemoveGuestX11PatchView(void) {
    [gGuestX11PatchView removeFromSuperview];
    gGuestX11PatchView = nil;
    gGuestX11PatchPixels = nil;
    gGuestX11PatchWidth = 0;
    gGuestX11PatchHeight = 0;
    gGuestX11ActiveWindow = 0;
    gGuestX11PatchVisible.store(false, std::memory_order_release);
}

static void BVNSyncGuestX11PatchGeometry(UIView* presentation) {
    if (gGuestX11PatchView == nil || presentation == nil) {
        return;
    }
    UIView* container = presentation.superview;
    if (container == nil) {
        return;
    }
    if (gGuestX11PatchView.superview != container) {
        [gGuestX11PatchView removeFromSuperview];
        [container addSubview:gGuestX11PatchView];
    }
    gGuestX11PatchView.bounds = presentation.bounds;
    gGuestX11PatchView.center = presentation.center;
    gGuestX11PatchView.transform = presentation.transform;
    [container bringSubviewToFront:gGuestX11PatchView];
}

extern "C" void BVNGuestCompositeX11Patch(const uint8_t* pixels,
                                           uint32_t pitch,
                                           int32_t bitsPerPixel,
                                           uint32_t windowId,
                                           int32_t screenX,
                                           int32_t screenY,
                                           uint32_t windowWidth,
                                           uint32_t windowHeight,
                                           int32_t dirtyX,
                                           int32_t dirtyY,
                                           uint32_t dirtyWidth,
                                           uint32_t dirtyHeight,
                                           uint32_t activeWindowId,
                                           int32_t activeScreenX,
                                           int32_t activeScreenY,
                                           uint32_t activeWidth,
                                           uint32_t activeHeight,
                                           bool windowIsActiveAncestor) {
    NSCAssert(NSThread.isMainThread,
              @"X11 partial presentation must run on main");
    @autoreleasepool {
    UIView* presentation = BVNGuestPresentationView();
    if (presentation == nil || pixels == nullptr || bitsPerPixel != 32 ||
        pitch < windowWidth * 4 || dirtyWidth == 0 || dirtyHeight == 0 ||
        activeWindowId == 0 || activeWidth == 0 || activeHeight == 0) {
        return;
    }

    const uint32_t guestWidth = (uint32_t)lround(presentation.bounds.size.width);
    const uint32_t guestHeight =
        (uint32_t)lround(presentation.bounds.size.height);
    if (guestWidth == 0 || guestHeight == 0) {
        return;
    }
    if (gGuestX11PatchPixels == nil || gGuestX11PatchWidth != guestWidth ||
        gGuestX11PatchHeight != guestHeight) {
        BVNRemoveGuestX11PatchView();
        gGuestX11PatchWidth = guestWidth;
        gGuestX11PatchHeight = guestHeight;
        gGuestX11PatchPixels = [NSMutableData
            dataWithLength:(NSUInteger)guestWidth * guestHeight * 4];
        gGuestX11PatchView = [[BVNX11PatchView alloc]
            initWithFrame:presentation.frame];
        gGuestX11PatchView.backgroundColor = UIColor.clearColor;
        gGuestX11PatchView.opaque = NO;
        gGuestX11PatchView.userInteractionEnabled = NO;
        gGuestX11PatchView.contentScaleFactor = 1.0;
        if (![gGuestX11PatchView configureForPixelWidth:guestWidth
                                                 height:guestHeight]) {
            BVNLogWrite(BVNLogLevelError, "graphics",
                        "Could not create the bounded Metal X11 compositor.");
            BVNRemoveGuestX11PatchView();
            return;
        }
    }

    const bool activeWindowChanged =
        gGuestX11ActiveWindow != activeWindowId;
    if (activeWindowChanged) {
        gGuestX11ActiveWindow = activeWindowId;
        memset(gGuestX11PatchPixels.mutableBytes, 0,
               gGuestX11PatchPixels.length);
    }

    const int32_t sourceLeft = MAX(0, dirtyX);
    const int32_t sourceTop = MAX(0, dirtyY);
    const int32_t sourceRight = MIN((int32_t)windowWidth,
                                    dirtyX + (int32_t)dirtyWidth);
    const int32_t sourceBottom = MIN((int32_t)windowHeight,
                                     dirtyY + (int32_t)dirtyHeight);
    const int32_t absoluteLeft = screenX + sourceLeft;
    const int32_t absoluteTop = screenY + sourceTop;
    const int32_t absoluteRight = screenX + sourceRight;
    const int32_t absoluteBottom = screenY + sourceBottom;
    const int32_t activeRight = activeScreenX + (int32_t)activeWidth;
    const int32_t activeBottom = activeScreenY + (int32_t)activeHeight;

    // A decorated Wine parent surrounds the active Vulkan client. Its broad
    // frame invalidations include the title bar and unused backing pixels;
    // they are not guest presents. Build 79 promoted those invalidations and
    // visibly copied a 29-pixel title bar plus a black right-hand strip. Only
    // accept ancestor updates wholly inside the active client's root rect.
    if (windowIsActiveAncestor &&
        (absoluteLeft < activeScreenX || absoluteTop < activeScreenY ||
         absoluteRight > activeRight || absoluteBottom > activeBottom)) {
        return;
    }

    const int32_t clientLeft = MAX(activeScreenX, absoluteLeft) - activeScreenX;
    const int32_t clientTop = MAX(activeScreenY, absoluteTop) - activeScreenY;
    const int32_t clientRight = MIN(activeRight, absoluteRight) - activeScreenX;
    const int32_t clientBottom = MIN(activeBottom, absoluteBottom) - activeScreenY;
    if (clientRight <= clientLeft || clientBottom <= clientTop) {
        return;
    }

    // Dirty bounds describe a rectangle within the active client; they are
    // not a second framebuffer size. Build 80 treated a 960x720 dirty region
    // as a complete logical image and stretched it to 1280x720, visibly
    // enlarging the title screen and cutting off its right side. Map only from
    // the active client's live size to the presentation size. Usually that is
    // the required 1:1 transform.
    const bool truncatedFullHeightUpdate =
        windowIsActiveAncestor && clientLeft == 0 && clientTop == 0 &&
        clientBottom == (int32_t)activeHeight &&
        clientRight < (int32_t)activeWidth;
    if (truncatedFullHeightUpdate) {
        static uint64_t skippedTruncatedCount = 0;
        ++skippedTruncatedCount;
        if (skippedTruncatedCount <= 12 ||
            skippedTruncatedCount % 120 == 0) {
            NSString* message = [NSString stringWithFormat:
                @"Skipped incoherent X11 ancestor present #%llu: window "
                 "0x%x dirty client %d,%d %dx%d, active 0x%x %dx%d. The "
                 "dirty rectangle is not a framebuffer size and will not "
                 "be stretched over Vulkan.",
                (unsigned long long)skippedTruncatedCount, windowId,
                clientLeft, clientTop, clientRight - clientLeft,
                clientBottom - clientTop, activeWindowId, (int)activeWidth,
                (int)activeHeight];
            BVNLogWrite(BVNLogLevelInfo, "graphics", message.UTF8String);
        }
        return;
    }
    const int32_t clippedLeft = (int32_t)((int64_t)clientLeft * guestWidth /
                                           activeWidth);
    const int32_t clippedTop = (int32_t)((int64_t)clientTop * guestHeight /
                                          activeHeight);
    const int32_t clippedRight = (int32_t)(((int64_t)clientRight * guestWidth +
                                             activeWidth - 1) / activeWidth);
    const int32_t clippedBottom = (int32_t)(((int64_t)clientBottom * guestHeight +
                                              activeHeight - 1) / activeHeight);

    uint32_t* destination =
        reinterpret_cast<uint32_t*>(gGuestX11PatchPixels.mutableBytes);
    const int32_t clientOffsetX = activeScreenX - screenX;
    const int32_t clientOffsetY = activeScreenY - screenY;
    for (int32_t destinationY = clippedTop;
         destinationY < clippedBottom; ++destinationY) {
        const int32_t clientY = MIN(clientBottom - 1,
            (int32_t)((int64_t)destinationY * activeHeight / guestHeight));
        const uint32_t* sourceRow = reinterpret_cast<const uint32_t*>(
            pixels + (clientOffsetY + clientY) * pitch);
        uint32_t* destinationRow = destination +
            destinationY * guestWidth;
        for (int32_t destinationX = clippedLeft;
             destinationX < clippedRight; ++destinationX) {
            const int32_t clientX = MIN(clientRight - 1,
                (int32_t)((int64_t)destinationX * activeWidth / guestWidth));
            // Boxedwine's X11 visual is BGRX in little-endian memory. Core
            // Graphics consumes BGRA here, so make only the updated rectangle
            // opaque and leave the rest transparent over the Vulkan frame.
            destinationRow[destinationX] =
                sourceRow[clientOffsetX + clientX] | 0xff000000u;
        }
    }

    BVNSyncGuestX11PatchGeometry(presentation);
    gGuestX11PatchView.hidden = NO;
    gGuestX11PatchVisible.store(true, std::memory_order_release);

    if (![gGuestX11PatchView presentPixelData:gGuestX11PatchPixels]) {
        static bool loggedMetalPresentFailure = false;
        if (!loggedMetalPresentFailure) {
            loggedMetalPresentFailure = true;
            BVNLogWrite(BVNLogLevelWarning, "graphics",
                        "The bounded Metal X11 compositor could not obtain "
                        "a drawable; later patches will retry.");
        }
        return;
    }
    gGuestX11PatchCount.fetch_add(1, std::memory_order_relaxed);
    BVNGuestPerformanceFramePresented();

    static uint64_t patchCount = 0;
    ++patchCount;
    if (patchCount <= 12 || patchCount % 120 == 0) {
        const BVNMemoryReport memory = BVNMemoryProbe();
        NSString* message = [NSString stringWithFormat:
            @"Composited X11 client present #%llu: window 0x%x %dx%d at %d,%d, "
             "active 0x%x %dx%d at %d,%d, dirty %d,%d %dx%d, "
             "mapping %dx%d, published %d,%d %dx%d over guest %dx%d; "
             "process resident %.1f MB.",
            (unsigned long long)patchCount, windowId, (int)windowWidth,
            (int)windowHeight, screenX, screenY, activeWindowId,
            (int)activeWidth, (int)activeHeight, activeScreenX, activeScreenY,
            dirtyX, dirtyY, (int)dirtyWidth, (int)dirtyHeight,
            (int)activeWidth, (int)activeHeight, clippedLeft, clippedTop,
            clippedRight - clippedLeft, clippedBottom - clippedTop,
            (int)guestWidth, (int)guestHeight,
            (double)memory.processResidentBytes / (1024.0 * 1024.0)];
        BVNLogWrite(BVNLogLevelInfo, "graphics", message.UTF8String);
    }
    }
}

extern "C" bool BVNGuestTakeX11PatchClearRequest(void) {
    return gGuestX11PatchVisible.exchange(false, std::memory_order_acq_rel);
}

extern "C" void BVNGuestClearX11Patches(void) {
    NSCAssert(NSThread.isMainThread, @"X11 patch clearing must run on main");
    if (gGuestX11PatchPixels != nil) {
        memset(gGuestX11PatchPixels.mutableBytes, 0,
               gGuestX11PatchPixels.length);
    }
    gGuestX11PatchView.hidden = YES;
}
static bool gGuestPresentationIsLetterboxed = false;
// Remembered so a rotation - or any UIKit layout pass that resets the Metal
// view to the full window - can re-apply the same fit without the emulator
// having to create a new surface.
static void* gGuestPresentationSurface = nullptr;
static uint32_t gGuestPresentationGuestWidth = 0;
static uint32_t gGuestPresentationGuestHeight = 0;
// 0 = fit (letterbox), 1 = fill while preserving aspect (crop), 2 = stretch.
static int gGuestPresentationMode = 0;
static NSString* const kBVNFillCropPercentKey =
    @"BoxedVN.presentation.fillCropPercent";

// The natural drawable size of the presenting layer (bounds x contentsScale)
// as of the last fit. Cached rather than read live because swapchains are
// created on a guest thread while CALayer geometry belongs to the main one;
// it only changes where the fit is applied, so a snapshot there is accurate.
static std::atomic<int> gGuestNaturalDrawableWidth{0};
static std::atomic<int> gGuestNaturalDrawableHeight{0};

extern "C" int BVNGuestPreferredPresentationMode(void) {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    if ([defaults objectForKey:@"BoxedVN.presentation.mode"] != nil) {
        return MAX(0, MIN(2,
            (int)[defaults integerForKey:@"BoxedVN.presentation.mode"]));
    }
    // Preserve the old two-state preference when upgrading.
    return [defaults boolForKey:@"BoxedVN.presentation.stretch"] ? 2 : 0;
}

extern "C" int BVNGuestFillCropPercent(void) {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    if ([defaults objectForKey:kBVNFillCropPercentKey] == nil) {
        return 5;
    }
    return MAX(0, MIN(25,
        (int)[defaults integerForKey:kBVNFillCropPercentKey]));
}

// The geometry the current fit was computed for, measured on the Metal view's
// superview. The watcher below compares against these rather than trusting a
// UIKit layout callback to arrive.
static CGRect gLastFittedBounds = CGRectNull;
static UIEdgeInsets gLastFittedInsets = UIEdgeInsetsZero;

// Reports the rectangle the guest picture is actually being shown in. Returns
// false while it fills the window, so the pointer transform can never assume a
// letterbox that did not take effect - which is exactly what broke tapping in
// build 62.
extern "C" bool BVNGuestPresentationContentRect(int* x, int* y, int* w,
                                                int* h) {
    if (!gGuestPresentationIsLetterboxed) {
        return false;
    }
    if (x) { *x = (int)gGuestPresentationContentRect.origin.x; }
    if (y) { *y = (int)gGuestPresentationContentRect.origin.y; }
    if (w) { *w = (int)gGuestPresentationContentRect.size.width; }
    if (h) { *h = (int)gGuestPresentationContentRect.size.height; }
    return true;
}

extern "C" void BVNApplyGuestPresentationAspect(void* surface,
                                                uint32_t guestWidth,
                                                uint32_t guestHeight,
                                                int presentationMode) {
    // Deliberately NOT dispatch_async(dispatch_get_main_queue()) when called
    // off-main. That is what build 63 did, and the device log proves the
    // blocks never ran: both of them were still queued when the session ended
    // 2.5 minutes later and only executed after "Boxedwine shutdown", by which
    // point their surfaces had been unregistered. While boxedmain owns the
    // main thread the main dispatch queue is not drained - SDL's pump services
    // UIKit events, not GCD. Callers must already be on the main thread, which
    // on this code path means inside a DISPATCH_MAIN_THREAD_BLOCK; that
    // mechanism is an SDL user event and demonstrably does run.
    if (!NSThread.isMainThread) {
        BVNLogWrite(BVNLogLevelWarning, "graphics",
                    "Presentation aspect refused off the main thread. The "
                    "main dispatch queue is not serviced during a guest "
                    "session; use DISPATCH_MAIN_THREAD_BLOCK instead.");
        return;
    }
    if (surface == nullptr || guestWidth == 0 || guestHeight == 0) {
        BVNLogWrite(BVNLogLevelWarning, "graphics",
                    "Presentation aspect skipped: no surface or zero guest "
                    "size.");
        return;
    }
    NSValue* key = [NSValue valueWithPointer:surface];
    UIView* view = gGuestVulkanSurfaceViews[key];
    if (view == nil) {
        BVNLogWrite(BVNLogLevelWarning, "graphics",
                    "Presentation aspect skipped: no registered view for this "
                    "surface.");
        return;
    }
    UIView* container = view.superview;
    if (container == nil) {
        BVNLogWrite(BVNLogLevelWarning, "graphics",
                    "Presentation aspect skipped: the Metal view has no "
                    "superview to measure against.");
        return;
    }
    // Measure against the Metal view's superview - on iOS 26/27 a
    // UIDropShadowView, UIKit's own hosting wrapper - and NOT the UIWindow.
    //
    // Build 66 switched to the window on the theory that an intermediate view
    // UIKit owns should not be trusted. The device log says the opposite: the
    // window reported PORTRAIT safe-area insets (l0 r0 t62 b34) while its
    // bounds were landscape 874x402, and the drop-shadow view reported the
    // correct landscape ones (l62 r62 t0 b20) in the same session. SDL's
    // UIWindow is created with the legacy -initWithFrame: path and only
    // attached to the scene afterwards, so its own safe area lags; the view
    // hierarchy UIKit actually laid out does not.
    UIWindow* window = view.window;

    gGuestPresentationSurface = surface;
    gGuestPresentationGuestWidth = guestWidth;
    gGuestPresentationGuestHeight = guestHeight;
    presentationMode = MAX(0, MIN(2, presentationMode));
    gGuestPresentationMode = presentationMode;

    // Horizontal safe-area insets only. In landscape the Dynamic Island is an
    // opaque cut-out at one end, so content must stay clear of it; the bottom
    // inset is the translucent home indicator, and giving up 5% of the picture
    // height to a line that is drawn over it anyway is the worse trade.
    //
    // A horizontal inset while the container is taller than it is wide is
    // discarded. On build 64 the portrait fit came out 278pt wide instead of
    // 402 because UIKit was still reporting the LANDSCAPE insets (62pt each
    // side) during the rotation's layout pass - the log's "at 62.0" is exactly
    // insets.left. iPhone portrait never has a horizontal safe inset, so a
    // non-zero one here is stale by definition. -safeAreaInsetsDidChange
    // re-fits as well, but only after the wrong frame has already been shown.
    UIEdgeInsets insets = container.safeAreaInsets;
    const CGRect containerBounds = container.bounds;
    const bool containerIsPortrait =
        containerBounds.size.height > containerBounds.size.width;
    if (containerIsPortrait && (insets.left > 0.0 || insets.right > 0.0)) {
        insets.left = 0.0;
        insets.right = 0.0;
    }
    const CGRect available = UIEdgeInsetsInsetRect(
        containerBounds, UIEdgeInsetsMake(0.0, insets.left, 0.0, insets.right));
    if (available.size.width <= 0.0 || available.size.height <= 0.0) {
        NSString* message = [NSString stringWithFormat:
            @"Presentation aspect skipped: nothing to fit into (%.0fx%.0f, "
             "insets l%.0f r%.0f).",
            containerBounds.size.width, containerBounds.size.height,
            insets.left, insets.right];
        BVNLogWrite(BVNLogLevelWarning, "graphics", message.UTF8String);
        return;
    }
    gLastFittedBounds = containerBounds;
    gLastFittedInsets = container.safeAreaInsets;

    // The letterbox is applied as bounds + transform, NOT by resizing the view.
    //
    // Resizing it (build 64) broke two things at once, and both were the same
    // mistake: SDL_uikitviewcontroller.viewDidLayoutSubviews reports the SDL
    // window size as `self.view.bounds.size`, and SDL_uikitmetalview derives
    // the CAMetalLayer's drawableSize from bounds x contentsScale. So shrinking
    // the view to 536x402 told SDL its window was 536x402 - after which SDL
    // delivered touches in content-rect coordinates and the pointer transform
    // subtracted the letterbox offset a second time (a tap at the picture's
    // left edge mapped to guest x = -252, which is why the left third of the
    // screen went dead) - and it left the natural drawable at 1608x1206 while
    // DXVK kept creating 800x600 swapchains, which is the VK_SUBOPTIMAL_KHR
    // storm: 8,244 rebuilds, 113 per second, measured on build 64.
    //
    // Setting bounds to the GUEST resolution with contentsScale 1.0 and doing
    // the scaling with an affine transform fixes both by construction:
    //
    //   * bounds x contentsScale is exactly 800x600, an integer, with no
    //     rounding to get wrong - so the natural drawable finally equals the
    //     extent DXVK asks for.
    //   * SDL's window becomes 800x600, i.e. the guest resolution, so
    //     -[UITouch locationInView:] (which is transform-aware) hands SDL
    //     coordinates that are already guest pixels. The pointer transform
    //     becomes the identity rather than something that has to be kept in
    //     agreement with the presenter.
    //
    // Core Animation scales the 800x600 drawable up to the display rect, which
    // is what was already happening - the swapchain was never bigger than the
    // guest resolution.
    CGFloat scaleX = available.size.width / (CGFloat)guestWidth;
    CGFloat scaleY = available.size.height / (CGFloat)guestHeight;
    if (presentationMode == 0) {
        scaleX = scaleY = MIN(scaleX, scaleY);
    } else if (presentationMode == 1) {
        // Preserve the guest's shape while covering the available display.
        // This fixes games that enter a 4:3 fullscreen surface but render a
        // widescreen picture inside it: aspect-fit preserves both sets of
        // bars, while stretch distorts the picture. Fill crops only the outer
        // surface and keeps circles, fonts and cursor geometry undistorted.
        const CGFloat fitScale = MIN(scaleX, scaleY);
        const CGFloat coverScale = MAX(scaleX, scaleY);
        // A strict cover can remove a large part of a 4:3 guest surface on a
        // wide phone. Limit how much is removed from each opposing edge using
        // the player's persisted percentage. The default 5% keeps the prior
        // 90%-visible behaviour; increasing it trades the slim bars for zoom.
        const CGFloat cropPerEdge =
            (CGFloat)BVNGuestFillCropPercent() / 100.0;
        const CGFloat visibleFraction = 1.0 - cropPerEdge * 2.0;
        const CGFloat cropSafeScale = fitScale / visibleFraction;
        scaleX = scaleY = MIN(coverScale, cropSafeScale);
    }
    const CGRect target = CGRectMake(
        available.origin.x +
            (available.size.width - (CGFloat)guestWidth * scaleX) / 2.0,
        available.origin.y +
            (available.size.height - (CGFloat)guestHeight * scaleY) / 2.0,
        (CGFloat)guestWidth * scaleX, (CGFloat)guestHeight * scaleY);

    view.autoresizingMask = UIViewAutoresizingNone;
    view.translatesAutoresizingMaskIntoConstraints = YES;
    view.transform = CGAffineTransformIdentity;
    view.bounds = CGRectMake(0.0, 0.0, (CGFloat)guestWidth,
                             (CGFloat)guestHeight);
    view.layer.contentsScale = 1.0;
    view.transform = CGAffineTransformMakeScale(scaleX, scaleY);
    // `target` and -center are both in the superview's coordinate space.
    view.center = CGPointMake(CGRectGetMidX(target), CGRectGetMidY(target));
    // Letterbox bars read as part of the device, not as Wine's desktop.
    container.backgroundColor = UIColor.blackColor;
    window.backgroundColor = UIColor.blackColor;

    // Assign the drawable size directly rather than forcing a layout pass to
    // make SDL's -updateDrawableSize compute it. Build 65 called
    // -layoutIfNeeded here, and this function runs from the overlay's own
    // -layoutSubviews: that walks back up to the window and re-enters the
    // layout of a sibling subtree from inside a layout pass, which UIKit does
    // not support. After the first rotation the whole subtree stopped being
    // laid out - the picture kept its portrait geometry when the device went
    // back to landscape (no re-fit was even logged), and touches stopped
    // arriving because the Metal view was sitting outside its ancestor's
    // stale bounds. The value assigned here is exactly what
    // -updateDrawableSize would compute, bounds x contentsScale, with
    // contentsScale pinned to 1.
    CGSize drawable = CGSizeZero;
    if ([view.layer isKindOfClass:CAMetalLayer.class]) {
        CAMetalLayer* metalLayer = (CAMetalLayer*)view.layer;
        metalLayer.drawableSize = CGSizeMake((CGFloat)guestWidth,
                                             (CGFloat)guestHeight);
        drawable = metalLayer.drawableSize;
        // Three is the maximum Metal allows and the most frames that can be in
        // flight before a present has to wait for the compositor. MoltenVK
        // derives this from the swapchain's image count, which wined3d asks
        // for as two; with only two drawables a single late compositor frame
        // stalls the guest, matching the observed low-frame-rate GDI symptom.
        // See BVNStartRefreshRateHold above.
        metalLayer.maximumDrawableCount = 3;
    }

    // Flush the layout this frame change implies, so SDL's
    // -viewDidLayoutSubviews runs now and its window size becomes the view's
    // bounds - which is what makes touches arrive as guest pixels - rather
    // than at some unpredictable later point.
    //
    // This is only safe because the callers are the surface-creation path and
    // the poll in KNativeInputSDL::processEvents, neither of which is inside a
    // UIKit layout pass. Build 65 called it from the overlay's own
    // -layoutSubviews, which re-entered the layout of a sibling subtree and
    // wedged it; the overlay no longer re-fits at all for exactly that reason.
    [view layoutIfNeeded];
    BVNSyncGuestX11PatchGeometry(view);

    // Keep the full transformed frame, including a negative origin in aspect
    // fill mode. The container clips it visually; retaining the full frame in
    // diagnostics makes that crop explicit and never participates in UIKit's
    // transform-aware input conversion.
    gGuestPresentationContentRect = target;
    gGuestPresentationIsLetterboxed =
        !CGRectEqualToRect(target, containerBounds);
    gGuestNaturalDrawableWidth.store((int)lround(view.bounds.size.width),
                                     std::memory_order_relaxed);
    gGuestNaturalDrawableHeight.store((int)lround(view.bounds.size.height),
                                      std::memory_order_relaxed);
    NSString* message = [NSString stringWithFormat:
        @"Guest presentation %@: guest %ux%u shown as %.1fx%.1f at %.1f,%.1f "
         "of %.0fx%.0f (%@, insets l%.0f r%.0f t%.0f b%.0f); view bounds "
         "%.0fx%.0f, drawable %.0fx%.0f, contentsScale %.2f. SDL will now "
         "report its window as the view's bounds, so pointer coordinates are "
         "guest pixels.",
        presentationMode == 2 ? @"stretched to fill"
            : presentationMode == 1 ? @"aspect-filled" : @"aspect-fitted",
        guestWidth, guestHeight, target.size.width, target.size.height,
        target.origin.x, target.origin.y, containerBounds.size.width,
        containerBounds.size.height, NSStringFromClass(container.class),
        insets.left, insets.right, insets.top, insets.bottom,
        view.bounds.size.width, view.bounds.size.height,
        drawable.width, drawable.height, view.layer.contentsScale];
    BVNLogWrite(BVNLogLevelInfo, "graphics", message.UTF8String);
}

// Reports the layer's NATURAL drawable size - bounds x contentsScale, which
// MoltenVK compares against the extent a swapchain was created at. While those
// two disagree, every acquire and present returns VK_SUBOPTIMAL_KHR and DXVK
// rebuilds the swapchain forever; that was the 8,244-rebuild storm on build 64.
//
// Cached rather than read live, because swapchains are created on a guest
// thread while CALayer geometry belongs to the main one.
// The Metal view the guest presents through, so the overlay can convert a
// touch into the coordinate space SDL and Boxedwine both use.
extern "C" UIView* BVNGuestPresentationView(void) {
    if (!NSThread.isMainThread || gGuestPresentationSurface == nullptr) {
        return nil;
    }
    return gGuestVulkanSurfaceViews[
        [NSValue valueWithPointer:gGuestPresentationSurface]];
}

extern "C" void BVNGuestPresentationNaturalDrawableSize(int* width,
                                                        int* height) {
    if (width) {
        *width = gGuestNaturalDrawableWidth.load(std::memory_order_relaxed);
    }
    if (height) {
        *height = gGuestNaturalDrawableHeight.load(std::memory_order_relaxed);
    }
}

// Re-fits the guest picture when the window has changed shape, and reports
// whether it did.
//
// This exists because a UIKit layout callback is not a reliable signal here.
// On build 65 the overlay's -layoutSubviews fired for landscape -> portrait and
// then never again: rotating back to landscape produced no re-fit at all (the
// log has no further "Guest presentation" line), so the picture kept its
// portrait geometry and ended up off-centre and partly off-screen. Polling the
// window's own bounds and safe-area insets from Boxedwine's main loop cannot be
// missed that way - that loop is the one thing guaranteed to keep running.
//
// Cheap by construction: two struct comparisons, and it only does work when
// the answer has actually changed.
extern "C" bool BVNSyncGuestPresentationGeometry(void) {
    if (!NSThread.isMainThread || gGuestPresentationSurface == nullptr) {
        return false;
    }
    NSValue* key = [NSValue valueWithPointer:gGuestPresentationSurface];
    UIView* view = gGuestVulkanSurfaceViews[key];
    UIView* container = view.superview;
    if (view == nil || container == nil || view.window == nil) {
        return false;
    }
    if (CGRectEqualToRect(container.bounds, gLastFittedBounds) &&
        UIEdgeInsetsEqualToEdgeInsets(container.safeAreaInsets,
                                      gLastFittedInsets)) {
        return false;
    }
    // The overlay must be on the window that is actually on screen. UIKit hosts
    // this app inside a UIDropShadowView, and a rotation there is not a simple
    // bounds change - if it ever hands the guest a different window, an overlay
    // left behind on the old one goes silently dead, which is exactly what
    // "in portrait I could not even press the menu button" describes. Cheap to
    // assert, and a no-op when nothing moved.
    BVNGuestOverlayInstall();

    BVNLogWrite(BVNLogLevelInfo, "graphics",
                "Window geometry changed; re-fitting the guest picture.");
    BVNApplyGuestPresentationAspect(gGuestPresentationSurface,
                                    gGuestPresentationGuestWidth,
                                    gGuestPresentationGuestHeight,
                                    gGuestPresentationMode);
    BVNGuestOverlayGeometryDidChange();
    return true;
}

extern "C" int BVNGuestPresentationMode(void) {
    return gGuestPresentationMode;
}

extern "C" void BVNGuestSetPresentationMode(int mode) {
    if (!NSThread.isMainThread) {
        return;
    }
    mode = MAX(0, MIN(2, mode));
    [[NSUserDefaults standardUserDefaults]
        setInteger:mode forKey:@"BoxedVN.presentation.mode"];
    gGuestPresentationMode = mode;
    if (gGuestPresentationSurface != nullptr) {
        BVNApplyGuestPresentationAspect(gGuestPresentationSurface,
                                        gGuestPresentationGuestWidth,
                                        gGuestPresentationGuestHeight,
                                        mode);
        BVNGuestPresentationGeometryChanged();
    }
}

extern "C" void BVNGuestSetFillCropPercent(int percent) {
    if (!NSThread.isMainThread) {
        return;
    }
    percent = MAX(0, MIN(25, percent));
    [[NSUserDefaults standardUserDefaults]
        setInteger:percent forKey:kBVNFillCropPercentKey];
    if (gGuestPresentationSurface != nullptr &&
        gGuestPresentationMode == 1) {
        BVNApplyGuestPresentationAspect(gGuestPresentationSurface,
                                        gGuestPresentationGuestWidth,
                                        gGuestPresentationGuestHeight,
                                        gGuestPresentationMode);
        BVNGuestPresentationGeometryChanged();
    }
    NSString* message = [NSString stringWithFormat:
        @"Fill-aspect crop limit set to %d%% per edge.", percent];
    BVNLogWrite(BVNLogLevelInfo, "graphics", message.UTF8String);
}

// Forgets the recorded rectangle, so a stale letterbox from a surface that no
// longer exists can never be handed to the next one's pointer transform. Pass
// a surface to forget only that one; pass nullptr at session teardown.
extern "C" void BVNForgetGuestPresentationAspect(void* surface) {
    if (surface != nullptr && surface != gGuestPresentationSurface) {
        return;
    }
    gGuestPresentationSurface = nullptr;
    gGuestPresentationGuestWidth = 0;
    gGuestPresentationGuestHeight = 0;
    gGuestPresentationMode = 0;
    gLastFittedBounds = CGRectNull;
    gLastFittedInsets = UIEdgeInsetsZero;
    gGuestPresentationContentRect = CGRectZero;
    gGuestPresentationIsLetterboxed = false;
    gGuestNaturalDrawableWidth.store(0, std::memory_order_relaxed);
    gGuestNaturalDrawableHeight.store(0, std::memory_order_relaxed);
    BVNRemoveGuestX11PatchView();
}

extern "C" void* BVNCreateOffscreenMetalLayer(uint32_t width,
                                                uint32_t height) {
    NSCAssert(NSThread.isMainThread,
              @"Offscreen Metal layers must be created on main");
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.bounds = CGRectMake(0.0, 0.0, MAX(1u, width), MAX(1u, height));
    layer.drawableSize = CGSizeMake(MAX(1u, width), MAX(1u, height));
    layer.contentsScale = 1.0;
    layer.opaque = YES;

    NSString* message = [NSString stringWithFormat:
        @"Created unattached %ux%u CAMetalLayer for a Vulkan capability "
         "probe; the visible X11 compositor is unchanged.",
        width, height];
    BVNLogWrite(BVNLogLevelInfo, "graphics", message.UTF8String);
    return (__bridge_retained void*)layer;
}

extern "C" void BVNDestroyOffscreenMetalLayer(void* pointer) {
    if (pointer == nullptr) {
        return;
    }
    if (!NSThread.isMainThread) {
        // Forced guest shutdown can release KVulkan after SDL's event loop
        // has stopped. Do not synchronously dispatch into that stopped loop;
        // ownership is independent of C++ and can be returned on main later.
        dispatch_async(dispatch_get_main_queue(), ^{
            CFRelease((CFTypeRef)pointer);
        });
        return;
    }
    CFRelease((CFTypeRef)pointer);
}

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
    if (gGuestPresentationHost != nil) {
        // The live view keeps the library on screen around the guest.
        return;
    }
    // Resigning key rather than hiding keeps the SwiftUI hierarchy alive, so
    // returning from a session does not lose the library's scroll position or
    // navigation state.
    delegate.libraryWindow.hidden = YES;
}
