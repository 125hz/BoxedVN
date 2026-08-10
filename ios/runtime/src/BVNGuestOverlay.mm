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
 *  The in-game overlay.
 *
 *  Why UIKit and not SDL.  There was already an on-canvas keyboard drawn by
 *  KNativeScreenSDL, but it only existed while SDL owned a renderer.  A guest
 *  that presents through Vulkan - which is every Direct3D game, the whole
 *  point of this port - has no SDL renderer at all, so that keyboard was
 *  invisible for the entire session it mattered in.  UIKit views sit above
 *  SDL's Metal view regardless of how the guest draws.
 *
 *  UIKit does run during a guest session, even though boxedmain owns the main
 *  thread: SDL's UIKit_PumpEvents runs the main run loop from inside
 *  SDL_PollEvent, which is how touches reach the guest at all, and how the
 *  native "STARTING GAME" spinner animates.  What does NOT run is the main
 *  dispatch queue - see BVNApplyGuestPresentationAspect for the device log
 *  that proves it.  Nothing here may use dispatch_async(main).
 *
 *  Touch routing.  BVNGuestOverlayView passes every touch that does not land
 *  on one of its own controls straight through to SDL's view, so a visual
 *  novel - which is nothing but taps - plays exactly as before.  That is also
 *  why the menu is a button and not a gesture: a three-finger tap or an edge
 *  swipe would fight the game for input.
 *
 *  Layout.  Frames are set by hand in -layoutSubviews rather than by Auto
 *  Layout, so the geometry is deterministic and so that the one place UIKit
 *  tells us the window changed shape is also the one place that re-fits the
 *  guest picture and the pointer transform to it.
 *  ---------------------------------------------------------------------
 */

#import "BVNGuestOverlay.h"

#include "BVNRuntime.h"

// ---------------------------------------------------------------------------
// The key table
//
// Names are SDL's own (SDL_scancode_names in SDL_keyboard.c) and are resolved
// through SDL at install time; see BVNGuestOverlay.h.
// ---------------------------------------------------------------------------

typedef NS_ENUM(NSInteger, BVNOverlayKeyKind) {
    // Press and release with the tap.
    BVNOverlayKeyKindNormal = 0,
    // Latching. Tapping it holds the key DOWN until it is tapped again.
    //
    // Deliberately a hold and not a one-shot prefix. The previous SDL keyboard
    // applied Shift to exactly the next keystroke, which is right for typing
    // and useless for playing: visual novels skip read text while Ctrl is
    // *held*, and Song of Saya is no exception. A latch that really holds the
    // key covers both - Ctrl+S still works, because Ctrl is genuinely down
    // when S is pressed.
    BVNOverlayKeyKindModifier = 1,
    // Closes the keyboard.
    BVNOverlayKeyKindHide = 2,
};

@interface BVNOverlayKey : NSObject
@property (nonatomic, copy) NSString* label;
@property (nonatomic, copy) NSString* scancodeName;
@property (nonatomic, assign) uint32_t scancode;
@property (nonatomic, assign) BVNOverlayKeyKind kind;
@property (nonatomic, assign) CGFloat weight;
@property (nonatomic, weak) UIButton* button;
@end

@implementation BVNOverlayKey
@end

static BVNOverlayKey* BVNKey(NSString* label, NSString* scancodeName,
                             BVNOverlayKeyKind kind, CGFloat weight) {
    BVNOverlayKey* key = [[BVNOverlayKey alloc] init];
    key.label = label;
    key.scancodeName = scancodeName;
    key.kind = kind;
    key.weight = weight;
    key.scancode = scancodeName.length
                       ? BVNGuestControlsScancodeForName(scancodeName.UTF8String)
                       : 0;
    return key;
}

static NSArray<NSArray<BVNOverlayKey*>*>* BVNKeyboardRows(void) {
    // Function keys earn their row: F5/F9 are quick save and quick load in
    // essentially every Windows visual novel, and there is no other way to
    // reach them from a touchscreen.
    return @[
        @[
            BVNKey(@"F1", @"F1", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"F2", @"F2", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"F3", @"F3", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"F4", @"F4", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"F5", @"F5", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"F6", @"F6", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"F7", @"F7", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"F8", @"F8", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"F9", @"F9", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"F10", @"F10", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"F11", @"F11", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"F12", @"F12", BVNOverlayKeyKindNormal, 1),
        ],
        @[
            BVNKey(@"esc", @"Escape", BVNOverlayKeyKindNormal, 1.4),
            BVNKey(@"1", @"1", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"2", @"2", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"3", @"3", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"4", @"4", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"5", @"5", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"6", @"6", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"7", @"7", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"8", @"8", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"9", @"9", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"0", @"0", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"-", @"-", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"=", @"=", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"del", @"Backspace", BVNOverlayKeyKindNormal, 1.6),
        ],
        @[
            BVNKey(@"tab", @"Tab", BVNOverlayKeyKindNormal, 1.4),
            BVNKey(@"Q", @"Q", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"W", @"W", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"E", @"E", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"R", @"R", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"T", @"T", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"Y", @"Y", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"U", @"U", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"I", @"I", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"O", @"O", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"P", @"P", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"[", @"[", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"]", @"]", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"\\", @"\\", BVNOverlayKeyKindNormal, 1.6),
        ],
        @[
            BVNKey(@"A", @"A", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"S", @"S", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"D", @"D", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"F", @"F", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"G", @"G", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"H", @"H", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"J", @"J", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"K", @"K", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"L", @"L", BVNOverlayKeyKindNormal, 1),
            BVNKey(@";", @";", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"'", @"'", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"enter", @"Return", BVNOverlayKeyKindNormal, 2.2),
        ],
        @[
            BVNKey(@"shift", @"Left Shift", BVNOverlayKeyKindModifier, 1.9),
            BVNKey(@"Z", @"Z", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"X", @"X", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"C", @"C", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"V", @"V", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"B", @"B", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"N", @"N", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"M", @"M", BVNOverlayKeyKindNormal, 1),
            BVNKey(@",", @",", BVNOverlayKeyKindNormal, 1),
            BVNKey(@".", @".", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"/", @"/", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"↑", @"Up", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"pgup", @"PageUp", BVNOverlayKeyKindNormal, 1.3),
        ],
        @[
            BVNKey(@"ctrl", @"Left Ctrl", BVNOverlayKeyKindModifier, 1.6),
            BVNKey(@"alt", @"Left Alt", BVNOverlayKeyKindModifier, 1.4),
            BVNKey(@"space", @"Space", BVNOverlayKeyKindNormal, 5),
            BVNKey(@"←", @"Left", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"↓", @"Down", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"→", @"Right", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"pgdn", @"PageDown", BVNOverlayKeyKindNormal, 1.3),
            BVNKey(@"close", nil, BVNOverlayKeyKindHide, 1.6),
        ],
    ];
}

// ---------------------------------------------------------------------------

static const CGFloat kBVNOverlayMargin = 10.0;
static const CGFloat kBVNMenuButtonSize = 46.0;
static const CGFloat kBVNMenuRowHeight = 50.0;
static const CGFloat kBVNMenuWidth = 268.0;
static const CGFloat kBVNKeyGap = 4.0;

@interface BVNGuestOverlayView : UIView

@property (nonatomic, strong) UIButton* menuButton;
@property (nonatomic, strong) UIView* scrim;
@property (nonatomic, strong) UIView* menuPanel;
@property (nonatomic, strong) UIButton* keyboardItem;
@property (nonatomic, strong) UIButton* rotationItem;
@property (nonatomic, strong) UIButton* quitItem;
@property (nonatomic, strong) UILabel* quitPrompt;
@property (nonatomic, strong) UIButton* quitConfirmItem;
@property (nonatomic, strong) UIButton* quitCancelItem;
@property (nonatomic, strong) UIView* keyboardPanel;
@property (nonatomic, strong) NSArray<NSArray<BVNOverlayKey*>*>* keyRows;
@property (nonatomic, strong) NSMutableSet<NSNumber*>* heldScancodes;
@property (nonatomic, assign) BOOL menuOpen;
@property (nonatomic, assign) BOOL confirmingQuit;
@property (nonatomic, assign) BOOL layingOut;
// The finger currently acting as the guest's mouse. A second finger belongs to
// the two-finger right-click gesture, not to a second cursor.
@property (nonatomic, weak) UITouch* guestTouch;
// Where the player has dragged the menu button to, as a fraction of the safe
// area, so it stays put across a rotation. Negative means "not moved yet".
@property (nonatomic, assign) CGPoint menuButtonFraction;

// A latched Ctrl that outlives the keyboard would leave the guest convinced a
// key is held down with no way to release it.
- (void)releaseHeldKeys;

@end

static BVNGuestOverlayView* gOverlay = nil;

@implementation BVNGuestOverlayView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self == nil) {
        return nil;
    }
    self.backgroundColor = UIColor.clearColor;
    self.opaque = NO;
    self.heldScancodes = [NSMutableSet set];
    self.keyRows = BVNKeyboardRows();
    [self buildMenu];
    [self buildKeyboard];
    [self reportUnresolvedKeys];
    return self;
}

// A key SDL does not recognise would otherwise be a button that silently does
// nothing, discovered only by a player pressing it mid-game.
- (void)reportUnresolvedKeys {
    NSMutableArray<NSString*>* unresolved = [NSMutableArray array];
    for (NSArray<BVNOverlayKey*>* row in self.keyRows) {
        for (BVNOverlayKey* key in row) {
            if (key.kind != BVNOverlayKeyKindHide && key.scancode == 0) {
                [unresolved addObject:key.scancodeName ?: key.label];
            }
        }
    }
    if (unresolved.count == 0) {
        BVNLogWrite(BVNLogLevelInfo, "input",
                    "Guest overlay keyboard installed; SDL resolved every key.");
        return;
    }
    NSString* message = [NSString stringWithFormat:
        @"Guest overlay keyboard has %lu key(s) SDL does not know and that "
         "will do nothing: %@.",
        (unsigned long)unresolved.count,
        [unresolved componentsJoinedByString:@", "]];
    BVNLogWrite(BVNLogLevelWarning, "input", message.UTF8String);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

- (UIButton*)makePanelItemWithTitle:(NSString*)title
                             action:(SEL)action
                        destructive:(BOOL)destructive {
    // UIButtonTypeCustom, not UIButtonTypeSystem. On iOS 15+ a system button
    // carries a UIButtonConfiguration, and once it does, a later
    // -setTitle:forState: is not reflected until the configuration is
    // refreshed. On build 64 that made the two rows whose titles change at
    // runtime - "Show/Hide keyboard" and "Rotation: locked/free" - render as
    // blank rows, while "Quit to library", whose title is only ever set here,
    // looked fine. A custom button has no configuration and no such rule.
    UIButton* button = [UIButton buttonWithType:UIButtonTypeCustom];
    [button setTitle:title forState:UIControlStateNormal];
    [button setTitleColor:(destructive ? [UIColor colorWithRed:1.0
                                                          green:0.42
                                                           blue:0.38
                                                          alpha:1.0]
                                       : UIColor.whiteColor)
                 forState:UIControlStateNormal];
    button.titleLabel.font = [UIFont systemFontOfSize:17.0
                                               weight:UIFontWeightMedium];
    // Left-aligned inside a frame that is already inset by the layout pass,
    // rather than contentEdgeInsets, which iOS 15 deprecated.
    button.contentHorizontalAlignment =
        UIControlContentHorizontalAlignmentLeft;
    [button addTarget:self
               action:action
     forControlEvents:UIControlEventTouchUpInside];
    return button;
}

- (void)buildMenu {
    UIButton* menu = [UIButton buttonWithType:UIButtonTypeSystem];
    UIImageSymbolConfiguration* symbol = [UIImageSymbolConfiguration
        configurationWithPointSize:20.0 weight:UIImageSymbolWeightSemibold];
    [menu setImage:[UIImage systemImageNamed:@"line.3.horizontal"
                           withConfiguration:symbol]
          forState:UIControlStateNormal];
    menu.tintColor = UIColor.whiteColor;
    menu.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.45];
    menu.layer.cornerRadius = kBVNMenuButtonSize / 2.0;
    menu.layer.borderWidth = 1.0;
    menu.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.28].CGColor;
    // Faint until touched: it lives on top of the game for the whole session,
    // and a visual novel is mostly full-frame artwork.
    menu.alpha = 0.4;
    menu.accessibilityLabel = @"BoxedVN menu";
    [menu addTarget:self
             action:@selector(toggleMenu)
   forControlEvents:UIControlEventTouchUpInside];
    // Drag it anywhere. A visual novel's own buttons move around the screen
    // between scenes, so a fixed corner is eventually going to sit on top of
    // something the player needs.
    UIPanGestureRecognizer* drag = [[UIPanGestureRecognizer alloc]
        initWithTarget:self action:@selector(dragMenuButton:)];
    [menu addGestureRecognizer:drag];
    [self addSubview:menu];
    self.menuButton = menu;
    // Negative means "never dragged": the layout then uses the default corner.
    self.menuButtonFraction = CGPointMake(-1.0, -1.0);

    // Two-finger tap = right click. A gesture recogniser on the overlay sees
    // the touch before SDL's view does, but only claims it once two fingers
    // land, so single-finger tapping - which is the whole of a visual novel -
    // is untouched.
    UITapGestureRecognizer* rightClick = [[UITapGestureRecognizer alloc]
        initWithTarget:self action:@selector(handleTwoFingerTap:)];
    rightClick.numberOfTouchesRequired = 2;
    rightClick.numberOfTapsRequired = 1;
    // Do not let it swallow the first finger from the game while it waits to
    // see whether a second one arrives.
    rightClick.cancelsTouchesInView = NO;
    rightClick.delaysTouchesBegan = NO;
    rightClick.delaysTouchesEnded = NO;
    [self addGestureRecognizer:rightClick];

    UIView* scrim = [[UIView alloc] initWithFrame:self.bounds];
    scrim.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.25];
    scrim.hidden = YES;
    UITapGestureRecognizer* dismiss = [[UITapGestureRecognizer alloc]
        initWithTarget:self action:@selector(closeMenu)];
    [scrim addGestureRecognizer:dismiss];
    [self addSubview:scrim];
    self.scrim = scrim;

    UIView* panel = [[UIView alloc] initWithFrame:CGRectZero];
    panel.backgroundColor = [UIColor colorWithWhite:0.07 alpha:0.96];
    panel.layer.cornerRadius = 16.0;
    panel.layer.borderWidth = 1.0;
    panel.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.16].CGColor;
    panel.clipsToBounds = YES;
    panel.hidden = YES;
    [self addSubview:panel];
    self.menuPanel = panel;

    self.keyboardItem = [self makePanelItemWithTitle:@"Show keyboard"
                                              action:@selector(toggleKeyboard)
                                         destructive:NO];
    self.rotationItem = [self makePanelItemWithTitle:@"Rotation: locked"
                                              action:@selector(toggleRotation)
                                         destructive:NO];
    self.quitItem = [self makePanelItemWithTitle:@"Quit to library"
                                          action:@selector(askToQuit)
                                     destructive:YES];
    [panel addSubview:self.keyboardItem];
    [panel addSubview:self.rotationItem];
    [panel addSubview:self.quitItem];

    // Confirmation is drawn inside this panel rather than presented as a
    // UIAlertController. Presenting a view controller would hand UIKit a modal
    // transition to drive while the emulator owns the main thread and the run
    // loop is only pumped in microsecond slices; an in-panel confirmation needs
    // nothing but the layout pass that is already happening.
    UILabel* prompt = [[UILabel alloc] initWithFrame:CGRectZero];
    prompt.text = @"Quit? Unsaved progress is lost.";
    prompt.textColor = [UIColor colorWithWhite:1.0 alpha:0.85];
    prompt.font = [UIFont systemFontOfSize:14.0];
    prompt.numberOfLines = 2;
    prompt.hidden = YES;
    [panel addSubview:prompt];
    self.quitPrompt = prompt;

    self.quitCancelItem = [self makePanelItemWithTitle:@"Keep playing"
                                                action:@selector(cancelQuit)
                                           destructive:NO];
    self.quitConfirmItem = [self makePanelItemWithTitle:@"Quit now"
                                                 action:@selector(confirmQuit)
                                            destructive:YES];
    self.quitCancelItem.hidden = YES;
    self.quitConfirmItem.hidden = YES;
    [panel addSubview:self.quitCancelItem];
    [panel addSubview:self.quitConfirmItem];
}

- (void)buildKeyboard {
    UIView* panel = [[UIView alloc] initWithFrame:CGRectZero];
    panel.backgroundColor = [UIColor colorWithWhite:0.04 alpha:0.94];
    panel.hidden = YES;
    [self addSubview:panel];
    self.keyboardPanel = panel;

    for (NSArray<BVNOverlayKey*>* row in self.keyRows) {
        for (BVNOverlayKey* key in row) {
            UIButton* button = [UIButton buttonWithType:UIButtonTypeCustom];
            [button setTitle:key.label forState:UIControlStateNormal];
            [button setTitleColor:UIColor.whiteColor
                         forState:UIControlStateNormal];
            button.backgroundColor = [UIColor colorWithWhite:0.19 alpha:1.0];
            button.layer.cornerRadius = 6.0;
            button.titleLabel.adjustsFontSizeToFitWidth = YES;
            button.titleLabel.minimumScaleFactor = 0.6;
            if (key.kind == BVNOverlayKeyKindNormal) {
                // Down on touch-down and up on release, so holding a key
                // really holds it - a visual novel advances text by holding
                // Enter or Space as often as by tapping. Release is claimed
                // for the outside and cancelled cases too, or a finger that
                // slides off the key would leave it stuck down in the guest.
                [button addTarget:self
                           action:@selector(keyTouchDown:)
                 forControlEvents:UIControlEventTouchDown];
                [button addTarget:self
                           action:@selector(keyTouchUp:)
                 forControlEvents:UIControlEventTouchUpInside |
                                  UIControlEventTouchUpOutside |
                                  UIControlEventTouchCancel];
            } else {
                // Latching keys and "close" act only on a deliberate tap that
                // ends on the key, so sliding a finger away cancels.
                [button addTarget:self
                           action:@selector(keyTouchUp:)
                 forControlEvents:UIControlEventTouchUpInside];
            }
            [panel addSubview:button];
            key.button = button;
        }
    }
}

// ---------------------------------------------------------------------------
// Touch routing: anything that is not one of our controls belongs to the game.
// ---------------------------------------------------------------------------

- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
    UIView* hit = [super hitTest:point withEvent:event];
    if (hit != self) {
        return hit;  // one of the overlay's own controls
    }

    // Not a control, so it belongs to the guest - and the overlay delivers it
    // itself rather than letting UIKit route it down to SDL's view.
    //
    // The build-67 log is why. In portrait it shows this method passing a tap
    // at 284,536 straight through, squarely inside the picture, and SDL
    // receiving nothing at all. Whatever UIKit does with a transformed view
    // inside its own hosting hierarchy, three builds have now failed to
    // predict it. -[UITouch locationInView:] resolves that same transform for
    // us, so taking delivery here removes the guesswork entirely.
    //
    // Without a presenting view - the software-rendered Wine desktop, where
    // SDL owns a renderer and its own logical-size mapping - pass the touch
    // down as before.
    return BVNGuestPresentationView() != nil ? self : nil;
}

// ---------------------------------------------------------------------------
// Guest pointer input
// ---------------------------------------------------------------------------

- (UITouch*)guestTouchIn:(NSSet<UITouch*>*)touches {
    for (UITouch* touch in touches) {
        if (touch == self.guestTouch) {
            return touch;
        }
    }
    return nil;
}

- (BOOL)sendGuestPointer:(UITouch*)touch phase:(int)phase {
    UIView* presentation = BVNGuestPresentationView();
    if (presentation == nil) {
        return NO;
    }
    const CGPoint point = [touch locationInView:presentation];
    BVNGuestControlsSendPointer((int)lround(point.x), (int)lround(point.y),
                                phase);
    return YES;
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (self.guestTouch != nil) {
        // A second finger is for the right-click gesture, not a second cursor.
        return;
    }
    UITouch* touch = touches.anyObject;
    if (touch == nil || ![self sendGuestPointer:touch phase:1]) {
        [super touchesBegan:touches withEvent:event];
        return;
    }
    self.guestTouch = touch;
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = [self guestTouchIn:touches];
    if (touch != nil) {
        [self sendGuestPointer:touch phase:0];
    }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = [self guestTouchIn:touches];
    if (touch == nil) {
        return;
    }
    [self sendGuestPointer:touch phase:2];
    self.guestTouch = nil;
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = [self guestTouchIn:touches];
    if (touch == nil) {
        return;
    }
    // Release the button even on a cancel, or the guest is left with it held.
    [self sendGuestPointer:touch phase:2];
    self.guestTouch = nil;
}

// UIKit reports the safe area separately from, and sometimes later than, the
// bounds change that accompanies a rotation. Build 64 fitted the guest picture
// during a portrait layout pass that was still reporting the landscape insets,
// producing a 278pt-wide picture in a 402pt-wide window. Re-fit when the insets
// settle.
- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    [self setNeedsLayout];
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

- (void)layoutSubviews {
    [super layoutSubviews];
    if (self.layingOut) {
        return;
    }
    self.layingOut = YES;

    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGRect bounds = self.bounds;

    self.scrim.frame = bounds;

    self.menuButton.bounds = CGRectMake(0.0, 0.0, kBVNMenuButtonSize,
                                        kBVNMenuButtonSize);
    const CGRect travel = [self menuButtonTravel];
    if (self.menuButtonFraction.x < 0.0) {
        self.menuButton.center = CGPointMake(CGRectGetMinX(travel),
                                             CGRectGetMinY(travel));
    } else {
        self.menuButton.center = CGPointMake(
            CGRectGetMinX(travel) + travel.size.width * self.menuButtonFraction.x,
            CGRectGetMinY(travel) + travel.size.height * self.menuButtonFraction.y);
    }

    [self layoutMenuPanelWithSafeArea:safe];
    [self layoutKeyboardPanelWithSafeArea:safe];

    // Deliberately does NOT re-fit the guest picture. That belongs to the poll
    // in KNativeInputSDL::processEvents, which runs outside any UIKit layout
    // pass. Re-fitting from here means running the presenter - which flushes
    // the Metal view's layout - from inside this layout pass, on a view in a
    // sibling subtree. Build 65 did that and UIKit stopped laying the subtree
    // out at all after the first rotation.
    self.layingOut = NO;
}

- (void)layoutMenuPanelWithSafeArea:(UIEdgeInsets)safe {
    const CGFloat width = MIN(kBVNMenuWidth,
                              self.bounds.size.width - safe.left - safe.right -
                                  kBVNOverlayMargin * 2.0);

    const CGFloat inset = 16.0;
    CGFloat cursor = 0.0;
    if (self.confirmingQuit) {
        self.quitPrompt.frame = CGRectMake(inset, 12.0, width - inset * 2.0,
                                           40.0);
        cursor = 60.0;
        for (UIButton* item in @[self.quitCancelItem, self.quitConfirmItem]) {
            item.frame = CGRectMake(inset, cursor, width - inset * 2.0,
                                    kBVNMenuRowHeight);
            cursor += kBVNMenuRowHeight;
        }
    } else {
        for (UIButton* item in @[self.keyboardItem, self.rotationItem,
                                 self.quitItem]) {
            item.frame = CGRectMake(inset, cursor, width - inset * 2.0,
                                    kBVNMenuRowHeight);
            cursor += kBVNMenuRowHeight;
        }
    }
    // Follow the menu button, then clamp inside the safe area. The button is
    // draggable, so the panel has to be able to open above it or to its left
    // rather than always down-and-right off the screen - and it must clear the
    // Dynamic Island, which it was not doing when the insets were read from
    // the wrong view.
    CGFloat x = CGRectGetMinX(self.menuButton.frame);
    CGFloat y = CGRectGetMaxY(self.menuButton.frame) + 8.0;
    const CGFloat maxX = self.bounds.size.width - safe.right -
                         kBVNOverlayMargin - width;
    const CGFloat maxY = self.bounds.size.height - safe.bottom -
                         kBVNOverlayMargin - cursor;
    if (y > maxY) {
        // Not enough room below: open upwards from the button instead.
        y = CGRectGetMinY(self.menuButton.frame) - 8.0 - cursor;
    }
    x = MIN(MAX(x, safe.left + kBVNOverlayMargin), MAX(maxX, safe.left));
    y = MIN(MAX(y, safe.top + kBVNOverlayMargin), MAX(maxY, safe.top));
    self.menuPanel.frame = CGRectMake(x, y, width, cursor);
}

- (void)layoutKeyboardPanelWithSafeArea:(UIEdgeInsets)safe {
    const CGFloat width = self.bounds.size.width;
    const NSUInteger rowCount = self.keyRows.count;
    if (rowCount == 0 || width <= 0.0) {
        return;
    }

    // Six rows have to fit a 402pt-tall landscape phone without eating the
    // whole screen - at this fraction they take about 63% of it, and the
    // keyboard is summoned deliberately and dismissed again - while still
    // being tappable on a 402pt-wide portrait one, where the clamp takes over.
    const CGFloat usableHeight = self.bounds.size.height - safe.top;
    CGFloat rowHeight = floor(usableHeight * 0.085);
    rowHeight = MAX(28.0, MIN(46.0, rowHeight));

    const CGFloat contentHeight =
        rowCount * rowHeight + (rowCount - 1) * kBVNKeyGap;
    const CGFloat panelHeight = contentHeight + kBVNKeyGap * 2.0 + safe.bottom;
    self.keyboardPanel.frame = CGRectMake(0.0,
                                          self.bounds.size.height - panelHeight,
                                          width, panelHeight);

    const CGFloat left = safe.left + kBVNKeyGap;
    const CGFloat right = width - safe.right - kBVNKeyGap;
    CGFloat rowY = kBVNKeyGap;
    for (NSArray<BVNOverlayKey*>* row in self.keyRows) {
        CGFloat totalWeight = 0.0;
        for (BVNOverlayKey* key in row) {
            totalWeight += key.weight;
        }
        const CGFloat usableWidth =
            (right - left) - ((CGFloat)row.count - 1.0) * kBVNKeyGap;
        CGFloat consumed = 0.0;
        CGFloat keyX = left;
        for (NSUInteger index = 0; index < row.count; ++index) {
            BVNOverlayKey* key = row[index];
            consumed += key.weight;
            const CGFloat nextX = left +
                usableWidth * (consumed / totalWeight) +
                (CGFloat)index * kBVNKeyGap;
            key.button.frame = CGRectMake(keyX, rowY, nextX - keyX, rowHeight);
            key.button.titleLabel.font =
                [UIFont systemFontOfSize:MIN(17.0, rowHeight * 0.42)
                                  weight:UIFontWeightMedium];
            keyX = nextX + kBVNKeyGap;
        }
        rowY += rowHeight + kBVNKeyGap;
    }
}

// The draggable area the menu button's centre is allowed to occupy, in the
// overlay's coordinates: the safe area, inset by half the button so it can
// never be dragged half off the screen.
- (CGRect)menuButtonTravel {
    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGFloat half = kBVNMenuButtonSize / 2.0;
    CGRect travel = UIEdgeInsetsInsetRect(self.bounds, safe);
    travel = CGRectInset(travel, half + kBVNOverlayMargin,
                         half + kBVNOverlayMargin);
    if (travel.size.width < 0.0 || travel.size.height < 0.0) {
        travel = self.bounds;
    }
    return travel;
}

- (void)dragMenuButton:(UIPanGestureRecognizer*)recognizer {
    const CGPoint translation = [recognizer translationInView:self];
    [recognizer setTranslation:CGPointZero inView:self];

    const CGRect travel = [self menuButtonTravel];
    CGPoint centre = self.menuButton.center;
    centre.x = MIN(MAX(centre.x + translation.x, CGRectGetMinX(travel)),
                   CGRectGetMaxX(travel));
    centre.y = MIN(MAX(centre.y + translation.y, CGRectGetMinY(travel)),
                   CGRectGetMaxY(travel));
    self.menuButton.center = centre;

    if (recognizer.state == UIGestureRecognizerStateEnded ||
        recognizer.state == UIGestureRecognizerStateCancelled) {
        // Store it as a fraction of the travel rectangle, so a rotation keeps
        // it in the same relative place instead of clamping it to an edge.
        self.menuButtonFraction = CGPointMake(
            travel.size.width > 0.0
                ? (centre.x - CGRectGetMinX(travel)) / travel.size.width : 0.0,
            travel.size.height > 0.0
                ? (centre.y - CGRectGetMinY(travel)) / travel.size.height : 0.0);
        [self setNeedsLayout];
    }
}

- (void)handleTwoFingerTap:(UITapGestureRecognizer*)recognizer {
    if (self.menuOpen || !self.keyboardPanel.hidden) {
        // The overlay's own UI is up; a two-finger tap there is not aimed at
        // the game.
        return;
    }
    UIView* presentation = BVNGuestPresentationView();
    if (presentation == nil) {
        return;
    }
    // Midpoint of the two fingers, in the presenting view's bounds - which are
    // the guest resolution, and which -locationInView: resolves through the
    // view's scale transform for us.
    const CGPoint point = [recognizer locationInView:presentation];
    if (!CGRectContainsPoint(presentation.bounds, point)) {
        return;
    }
    BVNGuestControlsSendRightClick((int)lround(point.x), (int)lround(point.y));
}

// ---------------------------------------------------------------------------
// Menu actions
// ---------------------------------------------------------------------------

- (void)toggleMenu {
    self.menuOpen = !self.menuOpen;
    if (!self.menuOpen) {
        self.confirmingQuit = NO;
    }
    [self applyMenuState];
}

- (void)closeMenu {
    self.menuOpen = NO;
    self.confirmingQuit = NO;
    [self applyMenuState];
}

- (void)applyMenuState {
    self.menuButton.alpha = self.menuOpen ? 1.0 : 0.4;
    self.scrim.hidden = !self.menuOpen;
    self.menuPanel.hidden = !self.menuOpen;

    const BOOL confirming = self.confirmingQuit;
    self.keyboardItem.hidden = confirming;
    self.rotationItem.hidden = confirming;
    self.quitItem.hidden = confirming;
    self.quitPrompt.hidden = !confirming;
    self.quitCancelItem.hidden = !confirming;
    self.quitConfirmItem.hidden = !confirming;

    [self.keyboardItem setTitle:(self.keyboardPanel.hidden ? @"Show keyboard"
                                                           : @"Hide keyboard")
                       forState:UIControlStateNormal];
    [self.rotationItem setTitle:(BVNGuestRotationIsUnlocked()
                                     ? @"Rotation: free"
                                     : @"Rotation: locked")
                       forState:UIControlStateNormal];
    [self setNeedsLayout];
}

- (void)toggleKeyboard {
    const BOOL show = self.keyboardPanel.hidden;
    if (!show) {
        [self releaseHeldKeys];
    }
    self.keyboardPanel.hidden = !show;
    [self closeMenu];
    BVNLogWrite(BVNLogLevelInfo, "input",
                show ? "Guest overlay keyboard shown."
                     : "Guest overlay keyboard hidden.");
}

- (void)toggleRotation {
    BVNGuestSetRotationUnlocked(!BVNGuestRotationIsUnlocked());
    [self applyMenuState];
}

- (void)askToQuit {
    self.confirmingQuit = YES;
    [self applyMenuState];
}

- (void)cancelQuit {
    self.confirmingQuit = NO;
    [self applyMenuState];
}

- (void)confirmQuit {
    [self releaseHeldKeys];
    [self closeMenu];
    BVNLogWrite(BVNLogLevelInfo, "runtime",
                "Player asked to quit the guest from the in-game overlay.");
    if (!BVNRuntimeRequestShutdown()) {
        BVNLogWrite(BVNLogLevelWarning, "runtime",
                    "Quit was requested with no session running.");
    }
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

- (BVNOverlayKey*)keyForButton:(UIButton*)button {
    for (NSArray<BVNOverlayKey*>* row in self.keyRows) {
        for (BVNOverlayKey* key in row) {
            if (key.button == button) {
                return key;
            }
        }
    }
    return nil;
}

- (void)applyHeldAppearance:(BVNOverlayKey*)key held:(BOOL)held {
    key.button.backgroundColor =
        held ? [UIColor colorWithRed:0.16 green:0.52 blue:0.86 alpha:1.0]
             : [UIColor colorWithWhite:0.19 alpha:1.0];
}

- (void)keyTouchDown:(UIButton*)sender {
    BVNOverlayKey* key = [self keyForButton:sender];
    if (key == nil) {
        return;
    }
    if (key.kind != BVNOverlayKeyKindNormal || key.scancode == 0) {
        return;
    }
    sender.backgroundColor = [UIColor colorWithWhite:0.36 alpha:1.0];
    BVNGuestControlsSendKey(key.scancode, true);
}

- (void)keyTouchUp:(UIButton*)sender {
    BVNOverlayKey* key = [self keyForButton:sender];
    if (key == nil) {
        return;
    }
    if (key.kind == BVNOverlayKeyKindHide) {
        [self toggleKeyboard];
        return;
    }
    if (key.scancode == 0) {
        return;
    }
    NSNumber* boxed = @(key.scancode);
    if (key.kind == BVNOverlayKeyKindModifier) {
        const BOOL wasHeld = [self.heldScancodes containsObject:boxed];
        if (wasHeld) {
            [self.heldScancodes removeObject:boxed];
            BVNGuestControlsSendKey(key.scancode, false);
        } else {
            [self.heldScancodes addObject:boxed];
            BVNGuestControlsSendKey(key.scancode, true);
        }
        [self applyHeldAppearance:key held:!wasHeld];
        return;
    }
    sender.backgroundColor = [UIColor colorWithWhite:0.19 alpha:1.0];
    BVNGuestControlsSendKey(key.scancode, false);
}

- (void)releaseHeldKeys {
    for (NSArray<BVNOverlayKey*>* row in self.keyRows) {
        for (BVNOverlayKey* key in row) {
            if (key.scancode != 0 &&
                [self.heldScancodes containsObject:@(key.scancode)]) {
                BVNGuestControlsSendKey(key.scancode, false);
                [self applyHeldAppearance:key held:NO];
            }
        }
    }
    [self.heldScancodes removeAllObjects];
}

@end

// ---------------------------------------------------------------------------
// C entry points
// ---------------------------------------------------------------------------

extern "C" void BVNGuestOverlayInstall(void) {
    if (!NSThread.isMainThread) {
        BVNLogWrite(BVNLogLevelWarning, "frontend",
                    "Ignored an off-main guest overlay install request.");
        return;
    }
    UIWindow* window = BVNGuestUIWindow();
    if (window == nil) {
        BVNLogWrite(BVNLogLevelWarning, "frontend",
                    "Guest overlay not installed: SDL has no guest window "
                    "yet.");
        return;
    }
    if (gOverlay == nil) {
        gOverlay = [[BVNGuestOverlayView alloc] initWithFrame:window.bounds];
    }
    if (gOverlay.superview == window) {
        [window bringSubviewToFront:gOverlay];
        return;
    }

    // A direct subview of the UIWindow, not of SDL's view. Adding it to SDL's
    // view would put it inside the letterboxed Metal view and clip it to the
    // picture; the overlay has to be able to use the black bars.
    //
    // Pinned with constraints rather than an autoresizing mask. A direct
    // window subview is not laid out by a view controller, so nothing
    // guarantees an autoresized frame survives an orientation change intact -
    // and if the overlay's frame is stale, its controls are drawn in one place
    // and hit-tested in another, which is indistinguishable from "the menu
    // does not respond".
    [gOverlay removeFromSuperview];
    gOverlay.frame = window.bounds;
    gOverlay.translatesAutoresizingMaskIntoConstraints = NO;
    [window addSubview:gOverlay];
    [NSLayoutConstraint activateConstraints:@[
        [gOverlay.leadingAnchor constraintEqualToAnchor:window.leadingAnchor],
        [gOverlay.trailingAnchor constraintEqualToAnchor:window.trailingAnchor],
        [gOverlay.topAnchor constraintEqualToAnchor:window.topAnchor],
        [gOverlay.bottomAnchor constraintEqualToAnchor:window.bottomAnchor],
    ]];
    [gOverlay setNeedsLayout];

    BVNLogWrite(BVNLogLevelInfo, "frontend",
                "Guest overlay attached to SDL's guest window.");
}

extern "C" void BVNGuestOverlayRemove(void) {
    if (!NSThread.isMainThread || gOverlay == nil) {
        return;
    }
    [gOverlay releaseHeldKeys];
    [gOverlay removeFromSuperview];
    gOverlay = nil;
    BVNLogWrite(BVNLogLevelInfo, "frontend", "Guest overlay removed.");
}
