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

#include <dispatch/dispatch.h>

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
@property (nonatomic, assign) BOOL guestOrientationLocked;
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
    // Wine/Boxedwine sessions are deliberately landscape-only. Allowing a
    // live portrait/landscape transition while boxedmain owns the main
    // thread makes UIKit replace SDL's Metal drawable and first responder in
    // the middle of the emulator's event loop. On device that presented as a
    // frozen guest and a permanently unresponsive software-keyboard button.
    // The SwiftUI library remains freely rotatable before and after a guest.
    if (self.guestOrientationLocked) {
        return UIInterfaceOrientationMaskLandscape;
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
    [gGuestVulkanSurfaceViews removeAllObjects];
    [gGuestVulkanWaitingOverlays removeAllObjects];
    [gAppDelegate finishGuestPresentation];
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

    NSString* message = [NSString stringWithFormat:
        @"Detached UIKit view for Vulkan surface %p; %lu surface view(s) "
         "remain.",
        surface, (unsigned long)gGuestVulkanSurfaceViews.count];
    BVNLogWrite(BVNLogLevelInfo, "graphics", message.UTF8String);
}

// Letterbox the guest picture instead of stretching it across the display.
//
// MoltenVK creates the swapchain images at the guest resolution (the device log
// shows "Created 2 swapchain images with size (800, 600)"), and SDL sizes the
// Metal view to the whole window, so those images are scaled to fill a 2.2:1
// screen - which is why 4:3 content spread edge to edge and ran under the
// Dynamic Island.
//
// Shrinking the layer's frame to an aspect-fit rectangle while leaving
// drawableSize at the guest resolution fixes it without touching DXVK or the
// swapchain: Core Animation scales the same drawable into a correctly
// proportioned rect. The safe-area insets keep it clear of the Dynamic Island.
//
// KNativeScreenSDL computes the matching inverse for pointer events. The two
// must agree; if one changes the other has to change with it.
extern "C" void BVNApplyGuestPresentationAspect(void* surface,
                                                uint32_t guestWidth,
                                                uint32_t guestHeight,
                                                bool stretchToFill) {
    if (!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(), ^{
            BVNApplyGuestPresentationAspect(surface, guestWidth, guestHeight,
                                            stretchToFill);
        });
        return;
    }
    if (surface == nullptr || guestWidth == 0 || guestHeight == 0) {
        return;
    }
    NSValue* key = [NSValue valueWithPointer:surface];
    UIView* view = gGuestVulkanSurfaceViews[key];
    if (view == nil || view.superview == nil) {
        return;
    }

    const CGRect available = UIEdgeInsetsInsetRect(view.superview.bounds,
                                                  view.superview.safeAreaInsets);
    if (available.size.width <= 0.0 || available.size.height <= 0.0) {
        return;
    }

    CGRect target = available;
    if (!stretchToFill) {
        const CGFloat scale = MIN(available.size.width / (CGFloat)guestWidth,
                                  available.size.height / (CGFloat)guestHeight);
        const CGFloat width = (CGFloat)guestWidth * scale;
        const CGFloat height = (CGFloat)guestHeight * scale;
        target = CGRectMake(available.origin.x +
                                (available.size.width - width) / 2.0,
                            available.origin.y +
                                (available.size.height - height) / 2.0,
                            width, height);
    }

    view.frame = target;

    NSString* message = [NSString stringWithFormat:
        @"Guest presentation %@: guest %ux%u into %.0fx%.0f at %.0f,%.0f "
         "(safe area %.0fx%.0f).",
        stretchToFill ? @"stretched to fill" : @"aspect-fitted",
        guestWidth, guestHeight, target.size.width, target.size.height,
        target.origin.x, target.origin.y, available.size.width,
        available.size.height];
    BVNLogWrite(BVNLogLevelInfo, "graphics", message.UTF8String);
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
