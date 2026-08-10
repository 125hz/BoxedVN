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
#import <QuartzCore/CAMetalLayer.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <atomic>
#include <cmath>
#include <dispatch/dispatch.h>

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
// Set from the in-game overlay's rotation control. Only meaningful while
// guestOrientationLocked is YES, i.e. while a guest session is running.
@property (nonatomic, assign) BOOL guestRotationUnlocked;
@property (nonatomic, assign) UIInterfaceOrientation orientationBeforeGuest;
- (void)createLibraryWindowForScene:(UIWindowScene*)scene;
- (UIWindow*)superWindowForGuest;
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
static NSMutableDictionary<NSValue*, UIView*>* gGuestVulkanSurfaceViews = nil;
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

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    // First, unconditionally: everything below can fail in ways that leave
    // no other trace (a black screen is exactly that kind of failure), so
    // logging has to be live before any of it runs.
    BVNLogStartSessionFile();

    gAppDelegate = self;

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

    // Let SDL install its lifecycle observers and schedule -postFinishLaunch,
    // which is what eventually calls BVNGuestMain on the main thread.
    const BOOL result = [super application:application
             didFinishLaunchingWithOptions:launchOptions];

    return result;
}

- (UIInterfaceOrientationMask)application:(UIApplication*)application
    supportedInterfaceOrientationsForWindow:(UIWindow*)window {
    // Guest sessions start landscape-locked, and stay that way unless the
    // player unlocks rotation from the in-game overlay. The lock exists
    // because a live orientation change while boxedmain owns the main thread
    // makes UIKit replace SDL's Metal drawable and first responder in the
    // middle of the emulator's event loop; on build 15 that presented as a
    // frozen guest and a permanently unresponsive keyboard button. What is
    // different now is that the presenter re-fits the picture and the pointer
    // transform on every layout pass (BVNGuestOverlayView.layoutSubviews), so
    // a new drawable is expected rather than fatal - but it is still opt-in,
    // and locked remains the default. The SwiftUI library is freely rotatable
    // before and after a guest.
    if (self.guestOrientationLocked) {
        return self.guestRotationUnlocked
                   ? UIInterfaceOrientationMaskAllButUpsideDown
                   : UIInterfaceOrientationMaskLandscape;
    }
    return UIInterfaceOrientationMaskAllButUpsideDown;
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
    [window makeKeyAndVisible];
    self.libraryWindow = window;

    BVNLogWrite(BVNLogLevelInfo, "frontend",
                "scene-owned library window created");
    BVNRuntimeNotifyFrontendReady();
}

- (void)waitForLandscapeWithAttempt:(NSUInteger)attempt
                         completion:(dispatch_block_t)completion {
    UIWindowScene* scene = self.libraryWindow.windowScene;
    const CGSize size = self.libraryWindow.bounds.size;
    const BOOL boundsAreLandscape = size.width > size.height;
    const BOOL sceneIsLandscape =
        scene == nil || UIInterfaceOrientationIsLandscape(scene.interfaceOrientation);

    // Waiting for both values matters. interfaceOrientation changes before
    // the final layout pass on some iOS releases; creating SDL in that gap
    // gives its CAMetalLayer the portrait drawable which caused the stretched
    // first frame and subsequent rotation freezes reported on build 15.
    if (boundsAreLandscape && sceneIsLandscape) {
        dispatch_async(dispatch_get_main_queue(), ^{
            BVNLogWrite(BVNLogLevelInfo, "frontend",
                        "Landscape guest geometry settled before SDL startup.");
            completion();
        });
        return;
    }

    if (attempt >= 180) {
        BVNLogWrite(BVNLogLevelWarning, "frontend",
                    "Landscape geometry did not settle within three seconds; "
                    "continuing so the launch request cannot remain stuck.");
        completion();
        return;
    }

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 (int64_t)(NSEC_PER_SEC / 60)),
                   dispatch_get_main_queue(), ^{
        [self waitForLandscapeWithAttempt:attempt + 1 completion:completion];
    });
}

- (void)prepareGuestPresentationWithCompletion:(dispatch_block_t)completion {
    NSAssert(NSThread.isMainThread, @"Guest presentation must be prepared on main");

    UIWindowScene* scene = self.libraryWindow.windowScene;
    self.orientationBeforeGuest = scene.interfaceOrientation;
    if (self.orientationBeforeGuest == UIInterfaceOrientationUnknown) {
        self.orientationBeforeGuest =
            self.libraryWindow.bounds.size.width > self.libraryWindow.bounds.size.height
                ? UIInterfaceOrientationLandscapeRight
                : UIInterfaceOrientationPortrait;
    }
    self.guestOrientationLocked = YES;
    self.guestRotationUnlocked = NO;
    [self.libraryWindow.rootViewController
        setNeedsUpdateOfSupportedInterfaceOrientations];

    if (scene == nil) {
        BVNLogWrite(BVNLogLevelWarning, "frontend",
                    "No UIWindowScene was available for the landscape guest "
                    "request; continuing with the current geometry.");
        completion();
        return;
    }

    BVNLogWrite(BVNLogLevelInfo, "frontend",
                "Requesting landscape and waiting for UIKit to finish before "
                "creating SDL's guest window.");
    UIWindowSceneGeometryPreferencesIOS* preferences =
        [[UIWindowSceneGeometryPreferencesIOS alloc]
            initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
    [scene requestGeometryUpdateWithPreferences:preferences
                                  errorHandler:^(NSError* error) {
        NSString* message = [NSString stringWithFormat:
            @"UIKit did not apply the preferred landscape guest geometry: %@",
            error.localizedDescription];
        BVNLogWrite(BVNLogLevelWarning, "frontend", message.UTF8String);
    }];
    [self waitForLandscapeWithAttempt:0 completion:completion];
}

- (void)finishGuestPresentation {
    NSAssert(NSThread.isMainThread, @"Guest presentation must finish on main");
    self.guestOrientationLocked = NO;
    self.guestRotationUnlocked = NO;
    [self.libraryWindow.rootViewController
        setNeedsUpdateOfSupportedInterfaceOrientations];

    UIWindowScene* scene = self.libraryWindow.windowScene;
    UIInterfaceOrientationMask restoreMask = UIInterfaceOrientationMaskPortrait;
    if (UIInterfaceOrientationIsLandscape(self.orientationBeforeGuest)) {
        restoreMask = self.orientationBeforeGuest == UIInterfaceOrientationLandscapeLeft
                          ? UIInterfaceOrientationMaskLandscapeLeft
                          : UIInterfaceOrientationMaskLandscapeRight;
    }
    if (scene != nil) {
        UIWindowSceneGeometryPreferencesIOS* preferences =
            [[UIWindowSceneGeometryPreferencesIOS alloc]
                initWithInterfaceOrientations:restoreMask];
        [scene requestGeometryUpdateWithPreferences:preferences
                                      errorHandler:^(NSError* error) {
            NSString* message = [NSString stringWithFormat:
                @"UIKit did not restore the library orientation: %@",
                error.localizedDescription];
            BVNLogWrite(BVNLogLevelWarning, "frontend", message.UTF8String);
        }];
    }
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
        [guestWindow makeKeyAndVisible];
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

extern "C" void BVNPrepareGuestPresentation(dispatch_block_t completion) {
    NSCAssert(NSThread.isMainThread, @"Guest startup must run on main");
    [gAppDelegate prepareGuestPresentationWithCompletion:completion];
}

extern "C" void BVNFinishGuestPresentation(void) {
    NSCAssert(NSThread.isMainThread, @"Guest cleanup must run on main");
    // SDL owns the actual guest window/view teardown. Drop any diagnostic
    // associations left by a forced Wine exit so they cannot retain UIKit
    // views or be mistaken for surfaces in the next session.
    BVNGuestOverlayRemove();
    [gGuestVulkanSurfaceViews removeAllObjects];
    [gGuestVulkanWaitingOverlays removeAllObjects];
    BVNForgetGuestPresentationAspect(nullptr);
    [gAppDelegate finishGuestPresentation];
}

// Implemented in platform/sdl/knativescreenSDL.cpp. SDL asks its own view
// controller for the orientation mask too, and answers landscape-only for a
// window that is wider than it is tall unless SDL_HINT_ORIENTATIONS says
// otherwise. UIKit intersects that with the app delegate's mask, so both have
// to be changed together or nothing happens.
extern "C" void BVNGuestControlsSetRotationHint(bool allowPortrait);

extern "C" UIWindow* BVNGuestUIWindow(void) {
    return [gAppDelegate superWindowForGuest];
}

extern "C" bool BVNGuestRotationIsUnlocked(void) {
    return gAppDelegate.guestRotationUnlocked == YES;
}

extern "C" void BVNGuestSetRotationUnlocked(bool unlocked) {
    if (!NSThread.isMainThread) {
        BVNLogWrite(BVNLogLevelWarning, "frontend",
                    "Ignored an off-main guest rotation request.");
        return;
    }
    BVNAppDelegate* delegate = gAppDelegate;
    if (delegate == nil) {
        return;
    }
    delegate.guestRotationUnlocked = unlocked ? YES : NO;
    BVNGuestControlsSetRotationHint(unlocked);

    UIWindow* guestWindow = [delegate superWindowForGuest];
    [guestWindow.rootViewController
        setNeedsUpdateOfSupportedInterfaceOrientations];
    [delegate.libraryWindow.rootViewController
        setNeedsUpdateOfSupportedInterfaceOrientations];

    // Re-locking has to actively put the device back into landscape; UIKit
    // does not rotate on its own just because portrait stopped being allowed
    // while the device is still held that way.
    UIWindowScene* scene = guestWindow.windowScene
                               ?: delegate.libraryWindow.windowScene;
    if (!unlocked && scene != nil) {
        UIWindowSceneGeometryPreferencesIOS* preferences =
            [[UIWindowSceneGeometryPreferencesIOS alloc]
                initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
        [scene requestGeometryUpdateWithPreferences:preferences
                                       errorHandler:^(NSError* error) {
            NSString* message = [NSString stringWithFormat:
                @"UIKit did not return the guest to landscape: %@",
                error.localizedDescription];
            BVNLogWrite(BVNLogLevelWarning, "frontend", message.UTF8String);
        }];
    }

    BVNLogWrite(BVNLogLevelInfo, "frontend",
                unlocked ? "Guest rotation unlocked by the player."
                         : "Guest rotation re-locked to landscape.");
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
    BVNLogWrite(BVNLogLevelInfo, "frontend",
                "SDL guest window attached to the active UIWindowScene.");

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
    // its first frame indistinguishable from an emulator freeze. Give it the
    // Wine desktop colour and a native, non-interactive progress overlay.
    // The overlay is removed by the first successful vkQueuePresentKHR.
    const CGFloat red = 59.0 / 255.0;
    const CGFloat green = 112.0 / 255.0;
    const CGFloat blue = 164.0 / 255.0;
    UIColor* wineBlue = [UIColor colorWithRed:red green:green blue:blue alpha:1.0];
    view.backgroundColor = wineBlue;
    view.layer.backgroundColor = wineBlue.CGColor;

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
    // The Wine-blue fill set at registration existed to distinguish "still
    // building the first frame" from a freeze. That question is now answered,
    // and blue showing through anywhere behind real content reads as a bug.
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
static bool gGuestPresentationIsLetterboxed = false;
// Remembered so a rotation - or any UIKit layout pass that resets the Metal
// view to the full window - can re-apply the same fit without the emulator
// having to create a new surface.
static void* gGuestPresentationSurface = nullptr;
static uint32_t gGuestPresentationGuestWidth = 0;
static uint32_t gGuestPresentationGuestHeight = 0;
static bool gGuestPresentationStretch = false;

// The natural drawable size of the presenting layer (bounds x contentsScale)
// as of the last fit. Cached rather than read live because swapchains are
// created on a guest thread while CALayer geometry belongs to the main one;
// it only changes where the fit is applied, so a snapshot there is accurate.
static std::atomic<int> gGuestNaturalDrawableWidth{0};
static std::atomic<int> gGuestNaturalDrawableHeight{0};

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
                                                bool stretchToFill) {
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
    gGuestPresentationStretch = stretchToFill;

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
    if (!stretchToFill) {
        scaleX = scaleY = MIN(scaleX, scaleY);
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
        stretchToFill ? @"stretched to fill" : @"aspect-fitted",
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
    BVNLogWrite(BVNLogLevelInfo, "graphics",
                "Window geometry changed; re-fitting the guest picture.");
    BVNApplyGuestPresentationAspect(gGuestPresentationSurface,
                                    gGuestPresentationGuestWidth,
                                    gGuestPresentationGuestHeight,
                                    gGuestPresentationStretch);
    return true;
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
    gGuestPresentationStretch = false;
    gLastFittedBounds = CGRectNull;
    gLastFittedInsets = UIEdgeInsetsZero;
    gGuestPresentationContentRect = CGRectZero;
    gGuestPresentationIsLetterboxed = false;
    gGuestNaturalDrawableWidth.store(0, std::memory_order_relaxed);
    gGuestNaturalDrawableHeight.store(0, std::memory_order_relaxed);
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
    // Resigning key rather than hiding keeps the SwiftUI hierarchy alive, so
    // returning from a session does not lose the library's scroll position or
    // navigation state.
    delegate.libraryWindow.hidden = YES;
}
