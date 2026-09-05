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

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

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
        ],
        @[
            BVNKey(@"ctrl", @"Left Ctrl", BVNOverlayKeyKindModifier, 1.6),
            BVNKey(@"alt", @"Left Alt", BVNOverlayKeyKindModifier, 1.4),
            BVNKey(@"space", @"Space", BVNOverlayKeyKindNormal, 5),
            BVNKey(@"close", nil, BVNOverlayKeyKindHide, 1.6),
        ],
        @[
            BVNKey(@"pgup", @"PageUp", BVNOverlayKeyKindNormal, 1.4),
            BVNKey(@"←", @"Left", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"↑", @"Up", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"↓", @"Down", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"→", @"Right", BVNOverlayKeyKindNormal, 1),
            BVNKey(@"pgdn", @"PageDown", BVNOverlayKeyKindNormal, 1.4),
        ],
    ];
}

// ---------------------------------------------------------------------------

static const CGFloat kBVNOverlayMargin = 10.0;
static const CGFloat kBVNMenuButtonSize = 46.0;
static const CGFloat kBVNMenuRowHeight = 46.0;
static const CGFloat kBVNMenuWidth = 268.0;
static const CGFloat kBVNKeyGap = 4.0;
// A row below this is not a key any more, it is a target a fingertip misses;
// above the upper bound the keyboard rows start eating a landscape screen.
static const CGFloat kBVNKeyRowMinimumHeight = 28.0;
static const CGFloat kBVNKeyRowMaximumHeight = 40.0;
// What a container has to measure before the keyboard is drawn *inside* it
// rather than as a sheet over the page. The rows at the minimum row height
// plus the gaps come to just under 200pt, so a container has to be well past
// that or the keyboard is the whole of it and the guest picture is gone. The
// live view's portrait host on a phone is about 350x260pt and fails this; the
// rotated full-screen landscape host (~390pt tall) and an iPad's live view
// pass it.
static const CGFloat kBVNKeyboardInlineMinimumHeight = 320.0;
static const CGFloat kBVNKeyboardInlineMinimumWidth = 340.0;
static NSString* const kBVNTrackpadModeKey = @"BoxedVN.pointer.trackpad";
static NSString* const kBVNPointerModeKey = @"BoxedVN.pointer.mode";
static NSString* const kBVNPointerSensitivityKey =
    @"BoxedVN.pointer.sensitivity";
static NSString* const kBVNPointerOpacityKey = @"BoxedVN.pointer.opacity";
static NSString* const kBVNPointerSizeKey = @"BoxedVN.pointer.size";
static NSString* const kBVNPointerOutlineOpacityKey =
    @"BoxedVN.pointer.outlineOpacity";
static NSString* const kBVNPointerShadowOpacityKey =
    @"BoxedVN.pointer.shadowOpacity";
static NSString* const kBVNPointerThicknessKey = @"BoxedVN.pointer.thickness";
static NSString* const kBVNPointerOuterCircleKey =
    @"BoxedVN.pointer.outerCircle";
static NSString* const kBVNPointerInnerCircleKey =
    @"BoxedVN.pointer.innerCircle";

// Trackpad motion is scaled by ONE number, never by a per-axis pair: the
// horizontal and the vertical guest-pixel delta are multiplied by the same
// factor, so a diagonal finger stroke stays diagonal whatever letterboxing or
// rotation sits between the overlay and the guest picture. 1.0 is
// finger-for-pixel, which is what the cursor is for - a Wine title-bar button
// has to be reachable, and anything faster than the finger overshoots it.
// Both writers (the in-guest panel and the live view's long-press popover)
// clamp to these bounds, so a stale or hand-edited default cannot leave the
// cursor unusable.
static const double kBVNPointerSensitivityMinimum = 0.25;
static const double kBVNPointerSensitivityMaximum = 3.0;
static const double kBVNPointerSensitivityDefault = 1.0;
// The remaining ranges the two panels share, so the popover's sliders and the
// in-guest sliders cannot disagree about what a value means.
static const double kBVNPointerSizeMinimum = 12.0;
static const double kBVNPointerSizeMaximum = 64.0;
static const double kBVNPointerThicknessMinimum = 0.5;
static const double kBVNPointerThicknessMaximum = 6.0;
static const double kBVNPointerOpacityMinimum = 0.1;
static const double kBVNPointerOpacityMaximum = 1.0;

static double BVNPointerClamp(double value, double minimum, double maximum) {
    if (!(value > minimum)) {
        // Also catches NaN, which a corrupted default can produce.
        return minimum;
    }
    return value > maximum ? maximum : value;
}

// Reads a pointer default that -registerDefaults may not have supplied yet:
// the live view can ask for these before any overlay exists, i.e. before a
// guest has started. The fallbacks are the same values registerDefaults uses.
static double BVNPointerDefaultDouble(NSString* key, double fallback,
                                      double minimum, double maximum) {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    if ([defaults objectForKey:key] == nil) {
        return fallback;
    }
    return BVNPointerClamp([defaults doubleForKey:key], minimum, maximum);
}

// The one reader every motion path goes through. A default written before the
// range narrowed - the shipped default used to be 1.4 - is clamped here rather
// than rewritten, so nothing has to migrate the store.
static double BVNPointerSensitivity(void) {
    return BVNPointerDefaultDouble(kBVNPointerSensitivityKey,
                                   kBVNPointerSensitivityDefault,
                                   kBVNPointerSensitivityMinimum,
                                   kBVNPointerSensitivityMaximum);
}

static NSString* const kBVNPerformanceEnabledKey =
    @"BoxedVN.performance.enabled";
static NSString* const kBVNPerformanceFPSKey = @"BoxedVN.performance.fps";
static NSString* const kBVNPerformanceRAMKey = @"BoxedVN.performance.ram";
static NSString* const kBVNPerformanceFrameTimeKey =
    @"BoxedVN.performance.frameTime";
static NSString* const kBVNPerformanceBatteryKey =
    @"BoxedVN.performance.battery";

@interface BVNGuestOverlayView : UIView

// Called from the live-view control bar's pointer toggle (extern C below).
- (void)setPointerMode:(NSInteger)mode;

@property (nonatomic, strong) UIButton* menuButton;
@property (nonatomic, strong) UIView* scrim;
@property (nonatomic, strong) UIView* menuPanel;
@property (nonatomic, strong) UIButton* keyboardItem;
@property (nonatomic, strong) UIButton* pointerItem;
@property (nonatomic, strong) UIButton* pointerSettingsItem;
@property (nonatomic, strong) UIButton* displayItem;
@property (nonatomic, strong) UIButton* frameRateItem;
@property (nonatomic, strong) UIButton* performanceItem;
@property (nonatomic, strong) UIButton* performanceSettingsItem;
@property (nonatomic, strong) UIButton* quitItem;
@property (nonatomic, strong) UILabel* quitPrompt;
@property (nonatomic, strong) UIButton* quitConfirmItem;
@property (nonatomic, strong) UIButton* quitCancelItem;
@property (nonatomic, strong) UIView* keyboardPanel;
@property (nonatomic, strong) NSArray<NSArray<BVNOverlayKey*>*>* keyRows;
@property (nonatomic, strong) NSMutableSet<NSNumber*>* heldScancodes;
@property (nonatomic, assign) BOOL menuOpen;
@property (nonatomic, assign) BOOL confirmingQuit;
@property (nonatomic, assign) BOOL pointerSettingsOpen;
@property (nonatomic, assign) BOOL performanceSettingsOpen;
@property (nonatomic, assign) BOOL layingOut;
// The finger currently acting as the guest's mouse. A second finger belongs to
// the two-finger right-click gesture, not to a second cursor.
@property (nonatomic, weak) UITouch* guestTouch;
// Where the player has dragged the menu button to, as a fraction of the safe
// area, so it stays put across a rotation. Negative means "not moved yet".
@property (nonatomic, assign) CGPoint menuButtonFraction;

// Pointer mode. Direct is the default and must stay the default: a visual
// novel is a full-screen tap target, and making the player drive a virtual
// cursor to advance dialogue would be strictly worse. Trackpad exists for the
// small Windows controls - a Wine title bar button, a file-manager row - that
// a fingertip cannot hit accurately.
@property (nonatomic, assign) BOOL trackpadMode;
@property (nonatomic, assign) BOOL guestCursorMode;
@property (nonatomic, assign) BOOL wineCursorOnlyMode;
@property (nonatomic, strong) UIView* cursorView;
@property (nonatomic, strong) UIImageView* guestCursorView;
@property (nonatomic, assign) CGPoint guestCursorHotspot;
@property (nonatomic, assign) CGSize guestCursorPixelSize;
@property (nonatomic, assign) BOOL guestCursorVisible;
@property (nonatomic, assign) BOOL guestCursorUsesFallback;
@property (nonatomic, assign) uint64_t appliedPointerRevision;
@property (nonatomic, assign) uint64_t appliedGuestCursorRevision;
@property (nonatomic, strong) UIView* cursorHaloView;
@property (nonatomic, strong) UIView* cursorDotView;
@property (nonatomic, strong) UIView* pointerSettingsPanel;
@property (nonatomic, strong) UIButton* pointerSettingsBackItem;
@property (nonatomic, strong) NSArray<UILabel*>* pointerSettingLabels;
@property (nonatomic, strong) UISlider* pointerOpacitySlider;
@property (nonatomic, strong) UISlider* pointerSensitivitySlider;
@property (nonatomic, strong) UISlider* pointerSizeSlider;
@property (nonatomic, strong) UISlider* pointerOutlineSlider;
@property (nonatomic, strong) UISlider* pointerShadowSlider;
@property (nonatomic, strong) UISlider* pointerThicknessSlider;
@property (nonatomic, strong) UISwitch* pointerOuterSwitch;
@property (nonatomic, strong) UISwitch* pointerInnerSwitch;

@property (nonatomic, strong) UIView* performanceView;
@property (nonatomic, strong) UILabel* performanceLabel;
@property (nonatomic, strong) NSTimer* performanceTimer;
@property (nonatomic, strong) NSTimer* pointerMotionTimer;
@property (nonatomic) CFTimeInterval lastPointerMotionTime;
@property (nonatomic, assign) uint64_t performanceLastFrameCount;
@property (nonatomic, assign) NSTimeInterval performanceLastSampleTime;
@property (nonatomic, assign) CGPoint performanceFraction;
@property (nonatomic, strong) UIView* performanceSettingsPanel;
@property (nonatomic, strong) UIButton* performanceSettingsBackItem;
@property (nonatomic, strong) NSArray<UILabel*>* performanceSettingLabels;
@property (nonatomic, strong) UISwitch* performanceFPSSwitch;
@property (nonatomic, strong) UISwitch* performanceRAMSwitch;
@property (nonatomic, strong) UISwitch* performanceFrameTimeSwitch;
@property (nonatomic, strong) UISwitch* performanceBatterySwitch;
// The cursor's position in the presenting view's bounds, i.e. guest pixels.
@property (nonatomic, assign) CGPoint cursorGuestPoint;
// A direct-touch press and release must use one stable guest transform. The
// presenting UIView can be replaced or re-letterboxed while the finger is
// down (notably while Vulkan recreates a surface), and remapping the same
// UITouch at release then produces a click whose two halves are hundreds of
// pixels apart. Keep the last guest point actually delivered until release.
@property (nonatomic, assign) CGPoint directLastGuestPoint;
@property (nonatomic, assign) BOOL directButtonHeld;
@property (nonatomic, assign) CGPoint trackpadTouchStart;
@property (nonatomic, assign) BOOL trackpadTouchMoved;
@property (nonatomic, assign) BOOL trackpadHasMotionBaseline;
@property (nonatomic, assign) BOOL trackpadButtonHeld;
@property (nonatomic, assign) CGPoint trackpadButtonGuestPoint;
@property (nonatomic, strong) NSTimer* trackpadHoldTimer;
@property (nonatomic, strong) NSTimer* trackpadTapReleaseTimer;
// Fired once, at the moment a stationary hold turns into a held left button -
// the instant a drag becomes possible and the only instant the finger has no
// other way of knowing it. Kept alive and -prepare'd while the hold timer is
// armed, because a generator built at the moment of the impact plays late.
@property (nonatomic, strong) UIImpactFeedbackGenerator* trackpadHoldHaptic;
// Where the finger was on the previous -touchesMoved, in the presenting
// view's coordinates. UIKit's own -previousLocationInView: cannot be used for
// this: UITouch objects are recycled between gestures, and on the first move
// of a new gesture it can still report the last point of the *previous* one -
// which is the reported "let go, wait a second, nudge the finger, and the
// cursor jumps once before behaving" bug. Tracking the point ourselves makes
// the first delta of every gesture exactly zero.
@property (nonatomic, assign) CGPoint trackpadLastPoint;

// The startup notice shown while Wine boots.
@property (nonatomic, strong) UIView* startupNotice;
@property (nonatomic, strong) UILabel* startupStateBadge;
@property (nonatomic, strong) UILabel* startupProgress;
@property (nonatomic, strong) UILabel* startupMetrics;
@property (nonatomic, strong) UILabel* startupLog;
@property (nonatomic, assign) uint64_t appliedStartupLogGeneration;
@property (nonatomic, assign) NSTimeInterval startupLastActivityTime;
@property (nonatomic, copy) NSString* startupActivitySignature;
@property (nonatomic, assign) NSUInteger startupTranslationCount;
@property (nonatomic, copy) NSString* startupLastRIP;

// A latched Ctrl that outlives the keyboard would leave the guest convinced a
// key is held down with no way to release it.
- (void)releaseHeldKeys;
- (void)cancelTrackpadGesture;

// The on-screen keyboard. -setKeyboardVisible: is the one place its state
// changes: it moves the panel to whichever container can show it, publishes
// gLiveKeyboardVisible and writes the witness.
- (void)setKeyboardVisible:(BOOL)visible;
- (void)updateKeyboardPlacement;
- (void)dismissKeyboardForTeardown;
- (void)reportKeyboardVisibility;

// Re-reads every pointer default into the in-guest panel's own controls and
// applies it to the drawn cursor. Called when the live view's popover writes
// the store from outside this panel.
- (void)syncPointerSettingsControls;

@end

static BVNGuestOverlayView* gOverlay = nil;
static std::atomic<double> gLivePerformanceFps {0.0};
static std::atomic<double> gLivePerformanceFrameMs {0.0};
std::atomic<bool> gLiveKeyboardVisible {false};
extern "C" UIView* BVNGuestPresentationHostView(void);
static std::atomic<uint64_t> gPerformancePresentedFrames{0};
static std::atomic<uint64_t> gPerformanceLastUpdateNanoseconds{0};

namespace {

struct BVNGuestCursorBitmap {
    int width = 0;
    int height = 0;
    int hotX = 0;
    int hotY = 0;
    std::vector<uint8_t> pixels;
};

std::atomic<int> gGuestPointerX{0};
std::atomic<int> gGuestPointerY{0};
std::atomic<uint64_t> gGuestPointerRevision{0};
std::atomic<uint32_t> gSelectedGuestCursorId{0};
std::atomic<int> gSelectedGuestCursorShape{68};
std::atomic<bool> gSelectedGuestCursorVisible{true};
std::atomic<uint64_t> gGuestCursorRevision{0};
std::mutex gGuestCursorMutex;
std::unordered_map<uint32_t, BVNGuestCursorBitmap> gGuestCursorBitmaps;

}  // namespace

static NSString* BVNStartupTokenAfter(NSString* line, NSString* marker) {
    const NSRange markerRange = [line rangeOfString:marker];
    if (markerRange.location == NSNotFound) {
        return nil;
    }
    const NSUInteger start = NSMaxRange(markerRange);
    NSRange end = [line rangeOfCharacterFromSet:
        NSCharacterSet.whitespaceAndNewlineCharacterSet
                                             options:0
                                               range:NSMakeRange(
                                                   start,
                                                   line.length - start)];
    const NSUInteger length = end.location == NSNotFound
        ? line.length - start
        : end.location - start;
    return [line substringWithRange:NSMakeRange(start, length)];
}

static NSString* BVNStartupMessageAfter(NSString* line, NSString* marker) {
    const NSRange markerRange = [line rangeOfString:marker];
    if (markerRange.location == NSNotFound) {
        return nil;
    }
    NSString* message = [line substringFromIndex:NSMaxRange(markerRange)];
    message = [message stringByTrimmingCharactersInSet:
        NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (message.length > 110) {
        message = [[message substringToIndex:109] stringByAppendingString:@"…"];
    }
    return message;
}

extern "C" void BVNGuestPointerPositionChanged(int x, int y) {
    const int previousX = gGuestPointerX.exchange(x, std::memory_order_relaxed);
    const int previousY = gGuestPointerY.exchange(y, std::memory_order_relaxed);
    if (previousX == x && previousY == y &&
        gGuestPointerRevision.load(std::memory_order_relaxed) != 0) return;
    gGuestPointerRevision.fetch_add(1, std::memory_order_release);
    if (NSThread.isMainThread) {
        BVNGuestOverlayApplyPendingState();
    }
}

extern "C" void BVNGuestCursorDefine(uint32_t id,
                                      const uint8_t* bgraPixels,
                                      int width, int height,
                                      int hotX, int hotY) {
    if (bgraPixels == nullptr || width <= 0 || height <= 0 ||
        width > 512 || height > 512) {
        return;
    }
    BVNGuestCursorBitmap bitmap;
    bitmap.width = width;
    bitmap.height = height;
    bitmap.hotX = hotX;
    bitmap.hotY = hotY;
    bitmap.pixels.assign(bgraPixels,
                         bgraPixels + (size_t)width * height * 4);
    {
        std::lock_guard<std::mutex> lock(gGuestCursorMutex);
        gGuestCursorBitmaps[id] = std::move(bitmap);
    }
    gGuestCursorRevision.fetch_add(1, std::memory_order_release);
}

extern "C" void BVNGuestCursorSelect(uint32_t id, int shape, bool visible) {
    const auto previousId = gSelectedGuestCursorId.exchange(id, std::memory_order_relaxed);
    const auto previousShape = gSelectedGuestCursorShape.exchange(shape, std::memory_order_relaxed);
    const auto previousVisible = gSelectedGuestCursorVisible.exchange(visible, std::memory_order_relaxed);
    // Wine repeats SetCursor during pointer motion. Re-selecting the same
    // cursor must not rebuild NSData/CGImage/UIImage on every mouse sample.
    // Defining new bitmap contents still publishes its own revision above.
    if (previousId == id && previousShape == shape && previousVisible == visible &&
        gGuestCursorRevision.load(std::memory_order_relaxed) != 0) return;
    gGuestCursorRevision.fetch_add(1, std::memory_order_release);
    if (NSThread.isMainThread) {
        BVNGuestOverlayApplyPendingState();
    }
}

@implementation BVNGuestOverlayView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self == nil) {
        return nil;
    }
    self.backgroundColor = UIColor.clearColor;
    self.opaque = NO;
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{
        kBVNPointerOpacityKey: @1.0,
        kBVNPointerSensitivityKey: @(kBVNPointerSensitivityDefault),
        kBVNPointerSizeKey: @22.0,
        kBVNPointerOutlineOpacityKey: @1.0,
        kBVNPointerShadowOpacityKey: @0.85,
        kBVNPointerThicknessKey: @2.0,
        kBVNPointerOuterCircleKey: @YES,
        kBVNPointerInnerCircleKey: @YES,
        kBVNPerformanceEnabledKey: @NO,
        kBVNPerformanceFPSKey: @YES,
        kBVNPerformanceRAMKey: @YES,
        kBVNPerformanceFrameTimeKey: @YES,
        kBVNPerformanceBatteryKey: @YES,
    }];
    self.heldScancodes = [NSMutableSet set];
    self.keyRows = BVNKeyboardRows();
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    if ([defaults objectForKey:kBVNPointerModeKey] != nil) {
        const NSInteger pointerMode = MAX(
            0, MIN(3, [defaults integerForKey:kBVNPointerModeKey]));
        self.trackpadMode = pointerMode != 0;
        self.guestCursorMode = self.trackpadMode;
        self.wineCursorOnlyMode = NO;
    } else {
        // Preserve the old direct/trackpad choice when upgrading.
        self.trackpadMode = [defaults boolForKey:kBVNTrackpadModeKey];
        self.guestCursorMode = self.trackpadMode;
        self.wineCursorOnlyMode = NO;
    }
    [self buildMenu];
    [self buildKeyboard];
    [self buildCursor];
    [self buildPointerSettings];
    [self buildPerformanceOverlay];
    [self buildPerformanceSettings];
    [self buildStartupNotice];
    [self reportUnresolvedKeys];
    return self;
}

- (void)buildCursor {
    // A ring rather than an arrow: it reads at any size, needs no asset, and
    // stays visible over both the bright and dark artwork a visual novel uses.
    //
    // A white ring on white artwork still disappeared - a menu screen or a
    // fade to white is exactly where a trackpad cursor is needed most. It now
    // carries a dark drop shadow, and a dark ring under the white one, so the
    // silhouette survives on any background without tinting the cursor itself.
    UIView* cursor = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 22, 22)];
    cursor.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.22];
    cursor.layer.cornerRadius = 11.0;
    cursor.layer.borderWidth = 2.0;
    cursor.layer.borderColor = UIColor.whiteColor.CGColor;
    cursor.layer.shadowColor = UIColor.blackColor.CGColor;
    cursor.layer.shadowOpacity = 0.85f;
    cursor.layer.shadowRadius = 3.0;
    cursor.layer.shadowOffset = CGSizeMake(0.0, 1.0);
    cursor.userInteractionEnabled = NO;
    cursor.hidden = YES;

    // Drawn outside the white ring, so on a bright background the cursor is a
    // dark outline and on a dark one it is invisible behind the white.
    UIView* halo = [[UIView alloc] initWithFrame:CGRectMake(-2, -2, 26, 26)];
    halo.backgroundColor = UIColor.clearColor;
    halo.layer.cornerRadius = 13.0;
    halo.layer.borderWidth = 1.5;
    halo.layer.borderColor = [UIColor colorWithWhite:0.0 alpha:0.55].CGColor;
    halo.userInteractionEnabled = NO;
    [cursor addSubview:halo];

    UIView* dot = [[UIView alloc] initWithFrame:CGRectMake(9, 9, 4, 4)];
    dot.backgroundColor = UIColor.whiteColor;
    dot.layer.cornerRadius = 2.0;
    dot.layer.shadowColor = UIColor.blackColor.CGColor;
    dot.layer.shadowOpacity = 0.9f;
    dot.layer.shadowRadius = 1.5;
    dot.layer.shadowOffset = CGSizeZero;
    [cursor addSubview:dot];

    [self addSubview:cursor];
    self.cursorView = cursor;
    self.cursorHaloView = halo;
    self.cursorDotView = dot;

    UIImageView* guestCursor = [[UIImageView alloc] initWithFrame:CGRectZero];
    guestCursor.contentMode = UIViewContentModeScaleToFill;
    guestCursor.userInteractionEnabled = NO;
    // Wine cursor themes are allowed to contain a white-only bitmap. That is
    // usable on a desktop with a contrasting wallpaper but disappears on the
    // white menus common to visual novels. A tight, zero-offset shadow follows
    // the bitmap alpha and supplies the black edge of the familiar Windows
    // cursor without replacing Wine's shape or hotspot.
    guestCursor.layer.shadowColor = UIColor.blackColor.CGColor;
    guestCursor.layer.shadowOpacity = 1.0f;
    guestCursor.layer.shadowRadius = 2.0f;
    guestCursor.layer.shadowOffset = CGSizeZero;
    guestCursor.layer.masksToBounds = NO;
    guestCursor.hidden = YES;
    [self addSubview:guestCursor];
    self.guestCursorView = guestCursor;
    [self applyPointerAppearance];
}

- (UISlider*)makePointerSliderFrom:(float)minimum
                                to:(float)maximum
                               key:(NSString*)key {
    UISlider* slider = [[UISlider alloc] initWithFrame:CGRectZero];
    slider.minimumValue = minimum;
    slider.maximumValue = maximum;
    slider.value = [[NSUserDefaults standardUserDefaults] floatForKey:key];
    [slider addTarget:self action:@selector(pointerSettingChanged:)
       forControlEvents:UIControlEventValueChanged];
    return slider;
}

- (void)buildPointerSettings {
    UIView* panel = [[UIView alloc] initWithFrame:CGRectZero];
    panel.backgroundColor = [UIColor colorWithWhite:0.07 alpha:0.98];
    panel.layer.cornerRadius = 16.0;
    panel.layer.borderWidth = 1.0;
    panel.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.16].CGColor;
    panel.hidden = YES;
    [self addSubview:panel];
    self.pointerSettingsPanel = panel;

    NSArray<NSString*>* titles = @[
        @"Sensitivity", @"Opacity", @"Size", @"Outline opacity", @"Shadow opacity",
        @"Cursor thickness", @"Outer circle", @"Inner circle",
    ];
    NSMutableArray<UILabel*>* labels = [NSMutableArray array];
    for (NSString* title in titles) {
        UILabel* label = [[UILabel alloc] initWithFrame:CGRectZero];
        label.text = title;
        label.textColor = UIColor.whiteColor;
        label.font = [UIFont systemFontOfSize:14.0];
        [panel addSubview:label];
        [labels addObject:label];
    }
    self.pointerSettingLabels = labels;

    self.pointerSensitivitySlider = [self
        makePointerSliderFrom:(float)kBVNPointerSensitivityMinimum
                           to:(float)kBVNPointerSensitivityMaximum
                          key:kBVNPointerSensitivityKey];
    self.pointerOpacitySlider = [self
        makePointerSliderFrom:(float)kBVNPointerOpacityMinimum
                           to:(float)kBVNPointerOpacityMaximum
                          key:kBVNPointerOpacityKey];
    self.pointerSizeSlider = [self
        makePointerSliderFrom:(float)kBVNPointerSizeMinimum
                           to:(float)kBVNPointerSizeMaximum
                          key:kBVNPointerSizeKey];
    self.pointerOutlineSlider = [self makePointerSliderFrom:0.0 to:1.0
                                                        key:kBVNPointerOutlineOpacityKey];
    self.pointerShadowSlider = [self makePointerSliderFrom:0.0 to:1.0
                                                       key:kBVNPointerShadowOpacityKey];
    self.pointerThicknessSlider = [self
        makePointerSliderFrom:(float)kBVNPointerThicknessMinimum
                           to:(float)kBVNPointerThicknessMaximum
                          key:kBVNPointerThicknessKey];
    for (UISlider* slider in @[self.pointerSensitivitySlider,
                              self.pointerOpacitySlider,
                              self.pointerSizeSlider,
                              self.pointerOutlineSlider,
                              self.pointerShadowSlider,
                              self.pointerThicknessSlider]) {
        [panel addSubview:slider];
    }

    self.pointerOuterSwitch = [[UISwitch alloc] initWithFrame:CGRectZero];
    self.pointerOuterSwitch.on = [[NSUserDefaults standardUserDefaults]
        boolForKey:kBVNPointerOuterCircleKey];
    [self.pointerOuterSwitch addTarget:self
                                action:@selector(pointerSettingChanged:)
                      forControlEvents:UIControlEventValueChanged];
    [panel addSubview:self.pointerOuterSwitch];

    self.pointerInnerSwitch = [[UISwitch alloc] initWithFrame:CGRectZero];
    self.pointerInnerSwitch.on = [[NSUserDefaults standardUserDefaults]
        boolForKey:kBVNPointerInnerCircleKey];
    [self.pointerInnerSwitch addTarget:self
                                action:@selector(pointerSettingChanged:)
                      forControlEvents:UIControlEventValueChanged];
    [panel addSubview:self.pointerInnerSwitch];

    self.pointerSettingsBackItem = [self makePanelItemWithTitle:@"Back"
                                                         action:@selector(closePointerSettings)
                                                    destructive:NO];
    [panel addSubview:self.pointerSettingsBackItem];
}

- (void)applyPointerAppearance {
    if (self.cursorView == nil) {
        return;
    }
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    const CGFloat size = [defaults doubleForKey:kBVNPointerSizeKey];
    const CGFloat thickness = [defaults doubleForKey:kBVNPointerThicknessKey];
    self.cursorView.bounds = CGRectMake(0.0, 0.0, size, size);
    self.cursorView.layer.cornerRadius = size / 2.0;
    self.cursorView.layer.borderWidth = thickness;
    self.cursorView.layer.borderColor =
        [UIColor colorWithWhite:1.0
                          alpha:[defaults doubleForKey:kBVNPointerOutlineOpacityKey]].CGColor;
    self.cursorView.layer.shadowOpacity =
        (float)[defaults doubleForKey:kBVNPointerShadowOpacityKey];
    self.cursorView.alpha = [defaults doubleForKey:kBVNPointerOpacityKey];

    // Wine cursor bitmaps keep their native dimensions and hotspot. When Wine
    // only supplies a cursor shape, BoxedVN draws the fallback and applies the
    // same size, outline and shadow controls as the trackpad ring.
    self.guestCursorView.layer.shadowOpacity = self.guestCursorUsesFallback
        ? (float)[defaults doubleForKey:kBVNPointerShadowOpacityKey]
        : 1.0f;
    self.guestCursorView.layer.shadowRadius = self.guestCursorUsesFallback
        ? MAX(0.5, thickness * 0.75) : 2.0;

    const CGFloat haloInset = MAX(1.0, thickness);
    self.cursorHaloView.frame = CGRectMake(-haloInset, -haloInset,
                                           size + haloInset * 2.0,
                                           size + haloInset * 2.0);
    self.cursorHaloView.layer.cornerRadius =
        (size + haloInset * 2.0) / 2.0;
    self.cursorHaloView.layer.borderWidth = MAX(1.0, thickness * 0.75);
    self.cursorHaloView.hidden =
        ![defaults boolForKey:kBVNPointerOuterCircleKey];

    const CGFloat dotSize = MAX(3.0, size * 0.18);
    self.cursorDotView.frame = CGRectMake((size - dotSize) / 2.0,
                                          (size - dotSize) / 2.0,
                                          dotSize, dotSize);
    self.cursorDotView.layer.cornerRadius = dotSize / 2.0;
    self.cursorDotView.hidden =
        ![defaults boolForKey:kBVNPointerInnerCircleKey];

    // Fallback pixels bake the outline into the image, so regenerate them
    // when the appearance sliders move. A real Wine bitmap is left untouched.
    if (self.guestCursorView != nil && self.guestCursorUsesFallback) {
        self.appliedGuestCursorRevision = UINT64_MAX;
        [self applyGuestCursorState];
    }
}

- (void)pointerSettingChanged:(UIControl*)sender {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    if (sender == self.pointerSensitivitySlider) {
        [defaults setFloat:self.pointerSensitivitySlider.value
                    forKey:kBVNPointerSensitivityKey];
    } else if (sender == self.pointerOpacitySlider) {
        [defaults setFloat:self.pointerOpacitySlider.value
                    forKey:kBVNPointerOpacityKey];
    } else if (sender == self.pointerSizeSlider) {
        [defaults setFloat:self.pointerSizeSlider.value
                    forKey:kBVNPointerSizeKey];
    } else if (sender == self.pointerOutlineSlider) {
        [defaults setFloat:self.pointerOutlineSlider.value
                    forKey:kBVNPointerOutlineOpacityKey];
    } else if (sender == self.pointerShadowSlider) {
        [defaults setFloat:self.pointerShadowSlider.value
                    forKey:kBVNPointerShadowOpacityKey];
    } else if (sender == self.pointerThicknessSlider) {
        [defaults setFloat:self.pointerThicknessSlider.value
                    forKey:kBVNPointerThicknessKey];
    } else if (sender == self.pointerOuterSwitch) {
        [defaults setBool:self.pointerOuterSwitch.on
                   forKey:kBVNPointerOuterCircleKey];
    } else if (sender == self.pointerInnerSwitch) {
        [defaults setBool:self.pointerInnerSwitch.on
                   forKey:kBVNPointerInnerCircleKey];
    }
    [self applyPointerAppearance];
    [self positionCursor];
}

// The live view's popover writes the same defaults these controls do, so the
// in-guest panel has to be re-read after an outside write or it would show -
// and on the next drag, re-write - the values from before it.
- (void)syncPointerSettingsControls {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    self.pointerSensitivitySlider.value = (float)BVNPointerSensitivity();
    self.pointerOpacitySlider.value =
        [defaults floatForKey:kBVNPointerOpacityKey];
    self.pointerSizeSlider.value = [defaults floatForKey:kBVNPointerSizeKey];
    self.pointerOutlineSlider.value =
        [defaults floatForKey:kBVNPointerOutlineOpacityKey];
    self.pointerShadowSlider.value =
        [defaults floatForKey:kBVNPointerShadowOpacityKey];
    self.pointerThicknessSlider.value =
        [defaults floatForKey:kBVNPointerThicknessKey];
    self.pointerOuterSwitch.on = [defaults boolForKey:kBVNPointerOuterCircleKey];
    self.pointerInnerSwitch.on = [defaults boolForKey:kBVNPointerInnerCircleKey];
    [self applyPointerAppearance];
    [self positionCursor];
}

- (void)buildPerformanceOverlay {
    UIView* view = [[UIView alloc] initWithFrame:CGRectZero];
    view.backgroundColor = [UIColor colorWithWhite:0.03 alpha:0.78];
    view.layer.cornerRadius = 10.0;
    view.layer.borderWidth = 1.0;
    view.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.22].CGColor;
    view.hidden = ![[NSUserDefaults standardUserDefaults]
        boolForKey:kBVNPerformanceEnabledKey];

    UILabel* label = [[UILabel alloc] initWithFrame:CGRectZero];
    label.textColor = UIColor.whiteColor;
    label.font = [UIFont monospacedDigitSystemFontOfSize:12.0
                                                  weight:UIFontWeightMedium];
    label.numberOfLines = 0;
    label.userInteractionEnabled = NO;
    [view addSubview:label];

    UIPanGestureRecognizer* drag = [[UIPanGestureRecognizer alloc]
        initWithTarget:self action:@selector(dragPerformanceOverlay:)];
    [view addGestureRecognizer:drag];
    [self addSubview:view];
    self.performanceView = view;
    self.performanceLabel = label;
    self.performanceFraction = CGPointMake(1.0, 0.0);
    self.performanceLastFrameCount =
        gPerformancePresentedFrames.load(std::memory_order_relaxed);
    self.performanceLastSampleTime = CACurrentMediaTime();

    self.performanceTimer = [NSTimer timerWithTimeInterval:0.5
                                                     target:self
                                                   selector:@selector(updatePerformanceOverlay:)
                                                   userInfo:nil
                                                    repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:self.performanceTimer
                              forMode:NSRunLoopCommonModes];
    UIDevice.currentDevice.batteryMonitoringEnabled = YES;
    [self updatePerformanceOverlay:self.performanceTimer];
}

- (UISwitch*)makePerformanceSwitchForKey:(NSString*)key {
    UISwitch* control = [[UISwitch alloc] initWithFrame:CGRectZero];
    control.on = [[NSUserDefaults standardUserDefaults] boolForKey:key];
    [control addTarget:self action:@selector(performanceSettingChanged:)
       forControlEvents:UIControlEventValueChanged];
    return control;
}

- (void)buildPerformanceSettings {
    UIView* panel = [[UIView alloc] initWithFrame:CGRectZero];
    panel.backgroundColor = [UIColor colorWithWhite:0.07 alpha:0.98];
    panel.layer.cornerRadius = 16.0;
    panel.layer.borderWidth = 1.0;
    panel.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.16].CGColor;
    panel.hidden = YES;
    [self addSubview:panel];
    self.performanceSettingsPanel = panel;

    NSArray<NSString*>* titles = @[@"FPS", @"RAM used / total",
                                    @"Frame-time", @"Battery"];
    NSMutableArray<UILabel*>* labels = [NSMutableArray array];
    for (NSString* title in titles) {
        UILabel* label = [[UILabel alloc] initWithFrame:CGRectZero];
        label.text = title;
        label.textColor = UIColor.whiteColor;
        label.font = [UIFont systemFontOfSize:15.0];
        [panel addSubview:label];
        [labels addObject:label];
    }
    self.performanceSettingLabels = labels;
    self.performanceFPSSwitch = [self makePerformanceSwitchForKey:kBVNPerformanceFPSKey];
    self.performanceRAMSwitch = [self makePerformanceSwitchForKey:kBVNPerformanceRAMKey];
    self.performanceFrameTimeSwitch =
        [self makePerformanceSwitchForKey:kBVNPerformanceFrameTimeKey];
    self.performanceBatterySwitch =
        [self makePerformanceSwitchForKey:kBVNPerformanceBatteryKey];
    for (UISwitch* control in @[self.performanceFPSSwitch,
                                self.performanceRAMSwitch,
                                self.performanceFrameTimeSwitch,
                                self.performanceBatterySwitch]) {
        [panel addSubview:control];
    }
    self.performanceSettingsBackItem =
        [self makePanelItemWithTitle:@"Back"
                              action:@selector(closePerformanceSettings)
                         destructive:NO];
    [panel addSubview:self.performanceSettingsBackItem];
}

- (void)updatePerformanceOverlay:(NSTimer*)timer {
    @autoreleasepool {
    (void)timer;
    const NSTimeInterval now = CACurrentMediaTime();
    const uint64_t frames =
        gPerformancePresentedFrames.load(std::memory_order_relaxed);
    const NSTimeInterval elapsed = MAX(0.001, now - self.performanceLastSampleTime);
    const double fps = (double)(frames - self.performanceLastFrameCount) / elapsed;
    self.performanceLastFrameCount = frames;
    self.performanceLastSampleTime = now;
    // Published for the SwiftUI live-view readout, which shows these instead
    // of the in-guest panel while a host is registered.
    gLivePerformanceFps.store(fps, std::memory_order_relaxed);
    gLivePerformanceFrameMs.store(fps > 0.01 ? 1000.0 / fps : 0.0,
                                  std::memory_order_relaxed);

    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    NSMutableArray<NSString*>* lines = [NSMutableArray array];
    if ([defaults boolForKey:kBVNPerformanceFPSKey]) {
        [lines addObject:[NSString stringWithFormat:@"FPS       %5.1f", fps]];
    }
    if ([defaults boolForKey:kBVNPerformanceRAMKey]) {
        const BVNMemoryReport memory = BVNMemoryProbe();
        const uint64_t total = MIN(memory.physicalMemoryBytes,
                                   memory.processResidentBytes +
                                       memory.availableBytes);
        NSByteCountFormatter* formatter = [[NSByteCountFormatter alloc] init];
        formatter.countStyle = NSByteCountFormatterCountStyleMemory;
        formatter.allowedUnits = NSByteCountFormatterUseMB |
                                 NSByteCountFormatterUseGB;
        [lines addObject:[NSString stringWithFormat:@"RAM       %@ / %@",
            [formatter stringFromByteCount:(long long)memory.processResidentBytes],
            [formatter stringFromByteCount:(long long)total]]];
    }
    if ([defaults boolForKey:kBVNPerformanceFrameTimeKey]) {
        [lines addObject:[NSString stringWithFormat:@"Frame     %5.1f ms",
            fps > 0.01 ? 1000.0 / fps : 0.0]];
    }
    if ([defaults boolForKey:kBVNPerformanceBatteryKey]) {
        const float level = UIDevice.currentDevice.batteryLevel;
        [lines addObject:level >= 0.0f
            ? [NSString stringWithFormat:@"%.0f%%", level * 100.0f]
            : @"--"];
    }
    self.performanceLabel.text = [lines componentsJoinedByString:@"\n"];
    [self setNeedsLayout];
    }
}

- (void)performanceSettingChanged:(UISwitch*)sender {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    if (sender == self.performanceFPSSwitch) {
        [defaults setBool:sender.on forKey:kBVNPerformanceFPSKey];
    } else if (sender == self.performanceRAMSwitch) {
        [defaults setBool:sender.on forKey:kBVNPerformanceRAMKey];
    } else if (sender == self.performanceFrameTimeSwitch) {
        [defaults setBool:sender.on forKey:kBVNPerformanceFrameTimeKey];
    } else if (sender == self.performanceBatterySwitch) {
        [defaults setBool:sender.on forKey:kBVNPerformanceBatteryKey];
    }
    [self updatePerformanceOverlay:self.performanceTimer];
}

- (void)buildStartupNotice {
    UIView* notice = [[UIView alloc] initWithFrame:self.bounds];
    notice.backgroundColor = UIColor.systemBackgroundColor;
    notice.userInteractionEnabled = NO;
    notice.hidden = YES;

    UILabel* title = [[UILabel alloc] initWithFrame:CGRectZero];
    title.text = @"Starting Wine";
    title.textColor = UIColor.labelColor;
    title.font = [[UIFontMetrics metricsForTextStyle:UIFontTextStyleTitle1]
        scaledFontForFont:[UIFont systemFontOfSize:24.0
                                             weight:UIFontWeightBold]];
    title.adjustsFontForContentSizeCategory = YES;
    title.textAlignment = NSTextAlignmentLeft;

    UILabel* body = [[UILabel alloc] initWithFrame:CGRectZero];
    body.text = @"Keep BoxedVN open. Translation and guest startup activity "
                 "will appear here as it happens.";
    body.textColor = UIColor.secondaryLabelColor;
    body.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    body.adjustsFontForContentSizeCategory = YES;
    body.textAlignment = NSTextAlignmentLeft;
    body.numberOfLines = 0;

    UILabel* badge = [[UILabel alloc] initWithFrame:CGRectZero];
    badge.text = @"PREPARING RUNTIME";
    badge.textColor = UIColor.systemBlueColor;
    badge.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    badge.adjustsFontForContentSizeCategory = YES;

    UILabel* progress = [[UILabel alloc] initWithFrame:CGRectZero];
    progress.text = @"Preparing executable memory";
    progress.textColor = UIColor.labelColor;
    progress.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    progress.adjustsFontForContentSizeCategory = YES;
    progress.numberOfLines = 2;

    UILabel* metrics = [[UILabel alloc] initWithFrame:CGRectZero];
    metrics.text = @"Waiting for guest translation";
    metrics.textColor = UIColor.secondaryLabelColor;
    metrics.font = [UIFont monospacedDigitSystemFontOfSize:12.0
                                                    weight:UIFontWeightRegular];
    metrics.adjustsFontForContentSizeCategory = YES;
    metrics.numberOfLines = 2;

    UIView* activityCard = [[UIView alloc] initWithFrame:CGRectZero];
    activityCard.backgroundColor = UIColor.secondarySystemBackgroundColor;
    activityCard.layer.cornerRadius = 14.0;
    activityCard.layer.cornerCurve = kCACornerCurveContinuous;

    UILabel* activityTitle = [[UILabel alloc] initWithFrame:CGRectZero];
    activityTitle.text = @"LIVE STARTUP ACTIVITY";
    activityTitle.textColor = UIColor.secondaryLabelColor;
    activityTitle.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    activityTitle.adjustsFontForContentSizeCategory = YES;
    activityTitle.translatesAutoresizingMaskIntoConstraints = NO;

    UILabel* activityLog = [[UILabel alloc] initWithFrame:CGRectZero];
    activityLog.text = @"Waiting for the guest runtime…";
    activityLog.textColor = UIColor.labelColor;
    activityLog.font = [UIFont monospacedSystemFontOfSize:11.5
                                                   weight:UIFontWeightRegular];
    activityLog.adjustsFontForContentSizeCategory = YES;
    activityLog.numberOfLines = 7;
    activityLog.lineBreakMode = NSLineBreakByTruncatingMiddle;
    activityLog.translatesAutoresizingMaskIntoConstraints = NO;

    [activityCard addSubview:activityTitle];
    [activityCard addSubview:activityLog];
    [NSLayoutConstraint activateConstraints:@[
        [activityTitle.topAnchor constraintEqualToAnchor:activityCard.topAnchor
                                                 constant:14.0],
        [activityTitle.leadingAnchor constraintEqualToAnchor:activityCard.leadingAnchor
                                                     constant:16.0],
        [activityTitle.trailingAnchor constraintEqualToAnchor:activityCard.trailingAnchor
                                                      constant:-16.0],
        [activityLog.topAnchor constraintEqualToAnchor:activityTitle.bottomAnchor
                                               constant:10.0],
        [activityLog.leadingAnchor constraintEqualToAnchor:activityCard.leadingAnchor
                                                   constant:16.0],
        [activityLog.trailingAnchor constraintEqualToAnchor:activityCard.trailingAnchor
                                                    constant:-16.0],
        [activityLog.bottomAnchor constraintEqualToAnchor:activityCard.bottomAnchor
                                                  constant:-14.0],
    ]];

    UILabel* footer = [[UILabel alloc] initWithFrame:CGRectZero];
    footer.text = @"If activity stops, the idle timer makes the pause visible. "
                  "The complete session log is still saved for export.";
    footer.textColor = UIColor.tertiaryLabelColor;
    footer.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    footer.adjustsFontForContentSizeCategory = YES;
    footer.numberOfLines = 0;

    // Only the title and the current action are shown in the live view. The
    // body, badge, metrics, activity card and footer are built but not added,
    // so their text setters elsewhere stay valid while the notice stays small.
    (void)body;
    (void)badge;
    (void)metrics;
    (void)activityCard;
    (void)footer;
    UIStackView* content = [[UIStackView alloc] initWithArrangedSubviews:@[
        title, progress
    ]];
    content.axis = UILayoutConstraintAxisVertical;
    content.alignment = UIStackViewAlignmentFill;
    content.spacing = 8.0;
    content.translatesAutoresizingMaskIntoConstraints = NO;

    [notice addSubview:content];
    UILayoutGuide* readable = notice.readableContentGuide;
    NSLayoutConstraint* centered =
        [content.centerYAnchor constraintEqualToAnchor:notice.safeAreaLayoutGuide.centerYAnchor];
    centered.priority = UILayoutPriorityDefaultHigh;
    NSLayoutConstraint* readableWidth =
        [content.widthAnchor constraintEqualToAnchor:readable.widthAnchor];
    readableWidth.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [content.centerXAnchor constraintEqualToAnchor:readable.centerXAnchor],
        [content.leadingAnchor constraintGreaterThanOrEqualToAnchor:readable.leadingAnchor],
        [content.trailingAnchor constraintLessThanOrEqualToAnchor:readable.trailingAnchor],
        [content.widthAnchor constraintLessThanOrEqualToConstant:560.0],
        [content.topAnchor constraintGreaterThanOrEqualToAnchor:notice.safeAreaLayoutGuide.topAnchor
                                                        constant:18.0],
        [content.bottomAnchor constraintLessThanOrEqualToAnchor:notice.safeAreaLayoutGuide.bottomAnchor
                                                         constant:-18.0],
        readableWidth,
        centered,
    ]];

    [self addSubview:notice];
    self.startupNotice = notice;
    self.startupStateBadge = badge;
    self.startupProgress = progress;
    self.startupMetrics = metrics;
    self.startupLog = activityLog;
}

- (void)refreshStartupActivity {
    const NSTimeInterval now = CACurrentMediaTime();
    const uint64_t generation = BVNLogGeneration();
    if (generation != self.appliedStartupLogGeneration) {
        self.appliedStartupLogGeneration = generation;

        constexpr size_t kStartupLogTailBytes = 96 * 1024;
        std::vector<char> buffer(kStartupLogTailBytes);
        const size_t copied = BVNLogCopyRecent(buffer.data(), buffer.size());
        NSString* tail = [[NSString alloc]
            initWithBytes:buffer.data()
                   length:copied
                 encoding:NSUTF8StringEncoding];
        if (tail == nil) {
            tail = @"";
        }

        NSMutableArray<NSString*>* activity = [NSMutableArray array];
        NSString* stage = @"Preparing the 64-bit guest runtime";
        // No longer fed: the per-block translator trace it counted has been
        // retired. Kept so the property it publishes keeps its type and its
        // meaning of "nothing translated yet" rather than going stale.
        NSUInteger translationCount = 0;
        NSString* lastRIP = nil;

        for (NSString* line in [tail componentsSeparatedByCharactersInSet:
                                    NSCharacterSet.newlineCharacterSet]) {
            NSString* entry = nil;
            // The translator used to report every block it compiled, and this
            // read the running count off those lines. That trace was two
            // thousand lines of a device log and is gone; what is left is the
            // one transition that matters, where the loader hands control to
            // Windows code.
            if ([line containsString:@"BOXEDWINE_FEX64_IRET_POST"]) {
                NSString* rip = BVNStartupTokenAfter(line, @"rip=");
                if (rip.length > 0) {
                    lastRIP = rip;
                }
                stage = @"Entering Windows code";
                entry = [NSString stringWithFormat:@"Windows entry reached  %@",
                                                    rip ?: @"unknown RIP"];
            } else if ([line containsString:@"BOXEDWINE_X64_BUILTIN_PREFLIGHT"]) {
                NSString* open = BVNStartupTokenAfter(line, @"open=");
                stage = @"Checking the Windows system modules";
                entry = [NSString stringWithFormat:@"system module check  %@",
                                                    open ?: @"?"];
            } else if ([line containsString:@"BOXEDWINE_X64_DLL_SEARCH"]) {
                NSString* op = BVNStartupTokenAfter(line, @"op=");
                stage = @"Loading the Windows system modules";
                entry = [NSString stringWithFormat:@"module search  %@",
                                                    op ?: @"?"];
            } else if ([line containsString:@"BOXEDWINE_FEX64_SYSCALL_ENTER"]) {
                NSString* number = BVNStartupTokenAfter(line, @"nr=");
                NSString* rip = BVNStartupTokenAfter(line, @"rip=");
                stage = @"Running the 64-bit guest loader";
                entry = [NSString stringWithFormat:@"guest syscall %@  %@",
                                                    number ?: @"?",
                                                    rip ?: @""];
            } else if ([line containsString:@"BOXEDWINE_FEX64_START loaded"]) {
                NSString* rip = BVNStartupTokenAfter(line, @"rip=");
                stage = @"Loading the 64-bit guest runtime";
                entry = [NSString stringWithFormat:@"loader mapped  %@",
                                                    rip ?: @""];
            } else if ([line containsString:@"BOXEDWINE_FEX64_START scheduled"]) {
                entry = @"translator scheduled";
            } else if ([line containsString:@"BOXEDWINE_FEX64_SCHED enter"]) {
                entry = @"guest thread entered translator";
            } else if ([line containsString:@"BOXEDWINE_FEX64_LIVE_ENTER"]) {
                NSString* rip = BVNStartupTokenAfter(line, @"rip=");
                entry = [NSString stringWithFormat:@"guest execution began  %@",
                                                    rip ?: @""];
            } else if ([line containsString:@"BOXEDWINE_FEX64_SAMPLE poll="]) {
                NSString* rip = BVNStartupTokenAfter(line, @"guest_rip=");
                NSString* target = BVNStartupTokenAfter(line, @"entry_rip=");
                if (rip.length > 0) {
                    lastRIP = rip;
                }
                stage = @"Running translated 64-bit code";
                entry = [NSString stringWithFormat:@"execution sample  %@  entry %@",
                                                    rip ?: @"unknown RIP",
                                                    target ?: @"unknown"];
            } else if ([line containsString:@"BOXEDWINE_X64_EXEC_REMAP"]) {
                stage = @"Replacing the 64-bit guest process";
                entry = @"exec address space rebuilt";
            } else if ([line containsString:@"BOXEDWINE_X64_EXEC pid="]) {
                stage = @"Starting the 64-bit guest process";
                entry = @"guest process entered exec";
            } else if ([line containsString:@"BOXEDWINE_X64_FORK"]) {
                NSString* child = BVNStartupTokenAfter(line, @"child=");
                stage = @"Creating a 64-bit guest process";
                entry = [NSString stringWithFormat:@"guest process forked  child %@",
                                                    child ?: @"?"];
            } else if ([line containsString:@"BOXEDWINE_DXMT_CALL"]) {
                NSString* name = BVNStartupTokenAfter(line, @"name=");
                NSString* index = BVNStartupTokenAfter(line, @"index=");
                stage = @"Calling the graphics translation layer";
                entry = [NSString stringWithFormat:@"graphics call %@  index %@",
                                                    name ?: @"other",
                                                    index ?: @"?"];
            } else if ([line containsString:@"BOXEDWINE_DXMT_RETURN"]) {
                NSString* status = BVNStartupTokenAfter(line, @"status=");
                stage = @"Running the graphics translation layer";
                entry = [NSString stringWithFormat:@"graphics call returned %@",
                                                    status ?: @"?"];
            } else if ([line containsString:@"] dxmt: "]) {
                stage = @"Preparing the graphics translation layer";
                entry = BVNStartupMessageAfter(line, @"] dxmt: ");
            } else if ([line containsString:@"] jit: "]) {
                entry = BVNStartupMessageAfter(line, @"] jit: ");
            } else if ([line hasPrefix:@"Launching \""]) {
                stage = @"Launching Wine";
                entry = @"Wine process launch requested";
            } else if ([line hasPrefix:@"wine:"] ||
                       [line containsString:@":err:"] ||
                       [line containsString:@":warn:"]) {
                stage = @"Wine is starting";
                entry = line;
                if (entry.length > 110) {
                    entry = [[entry substringToIndex:109]
                        stringByAppendingString:@"…"];
                }
            }

            if (entry.length == 0 || [entry isEqualToString:activity.lastObject]) {
                continue;
            }
            [activity addObject:entry];
            if (activity.count > 7) {
                [activity removeObjectAtIndex:0];
            }
        }

        NSString* visibleLog = activity.count > 0
            ? [activity componentsJoinedByString:@"\n"]
            : @"Waiting for the guest runtime…";
        NSString* signature = [NSString stringWithFormat:@"%@|%lu|%@",
            visibleLog, (unsigned long)translationCount, lastRIP ?: @""];
        if (![signature isEqualToString:self.startupActivitySignature]) {
            self.startupActivitySignature = signature;
            self.startupLastActivityTime = now;
        }
        self.startupTranslationCount = translationCount;
        self.startupLastRIP = lastRIP;
        self.startupProgress.text = stage;
        self.startupLog.text = visibleLog;
    }

    const NSTimeInterval idle = self.startupLastActivityTime > 0.0
        ? MAX(0.0, now - self.startupLastActivityTime)
        : 0.0;
    if (idle >= 3.0) {
        self.startupStateBadge.text = @"NO NEW GUEST ACTIVITY";
        self.startupStateBadge.textColor = UIColor.systemOrangeColor;
    } else if (self.startupTranslationCount > 0) {
        self.startupStateBadge.text = @"TRANSLATOR ACTIVE";
        self.startupStateBadge.textColor = UIColor.systemGreenColor;
    } else {
        self.startupStateBadge.text = @"PREPARING RUNTIME";
        self.startupStateBadge.textColor = UIColor.systemBlueColor;
    }

    NSMutableArray<NSString*>* metrics = [NSMutableArray array];
    if (self.startupTranslationCount > 0) {
        [metrics addObject:[NSString stringWithFormat:@"%lu translations",
            (unsigned long)self.startupTranslationCount]];
    }
    if (self.startupLastRIP.length > 0) {
        [metrics addObject:[NSString stringWithFormat:@"last RIP %@",
                                                     self.startupLastRIP]];
    }
    if (idle >= 1.0) {
        [metrics addObject:[NSString stringWithFormat:@"idle %.0fs", idle]];
    }
    self.startupMetrics.text = metrics.count > 0
        ? [metrics componentsJoinedByString:@"  ·  "]
        : @"Waiting for guest translation";
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
    // runtime - such as "Show/Hide keyboard" and pointer/performance state - render as
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
    self.pointerItem = [self makePanelItemWithTitle:@"Pointer: direct tap"
                                             action:@selector(togglePointerMode)
                                        destructive:NO];
    self.pointerSettingsItem = [self makePanelItemWithTitle:@"Pointer settings"
                                                     action:@selector(openPointerSettings)
                                                destructive:NO];
    self.displayItem = [self makePanelItemWithTitle:@"Display: fit"
                                             action:@selector(toggleDisplayMode)
                                        destructive:NO];
    self.frameRateItem = [self makePanelItemWithTitle:@"Frame rate: uncapped"
                                                action:@selector(toggleFrameRateMode)
                                           destructive:NO];
    self.performanceItem = [self makePanelItemWithTitle:@"Performance overlay: off"
                                                  action:@selector(togglePerformanceOverlay)
                                             destructive:NO];
    self.performanceSettingsItem = [self makePanelItemWithTitle:@"Performance settings"
                                                          action:@selector(openPerformanceSettings)
                                                     destructive:NO];
    self.quitItem = [self makePanelItemWithTitle:@"Quit to library"
                                          action:@selector(askToQuit)
                                     destructive:YES];
    [panel addSubview:self.keyboardItem];
    [panel addSubview:self.pointerItem];
    [panel addSubview:self.pointerSettingsItem];
    [panel addSubview:self.displayItem];
    [panel addSubview:self.frameRateItem];
    [panel addSubview:self.performanceItem];
    [panel addSubview:self.performanceSettingsItem];
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
            button.titleLabel.minimumScaleFactor = 0.75;
            button.contentEdgeInsets = UIEdgeInsetsMake(2, 2, 2, 2);
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
    UIView* presentation = BVNGuestPresentationView();
    int softwareWidth = 0;
    int softwareHeight = 0;
    const BOOL softwareGuest = presentation == nil &&
        BVNGuestControlsScreenSize(&softwareWidth, &softwareHeight);
    float softwareX = 0.0f;
    float softwareY = 0.0f;
    const BOOL insideSoftwarePicture = softwareGuest &&
        BVNGuestControlsMapSoftwarePoint((float)point.x, (float)point.y,
                                         &softwareX, &softwareY) &&
        softwareX >= 0.0f && softwareY >= 0.0f &&
        softwareX < softwareWidth && softwareY < softwareHeight;

    // A tap on the letterbox is not a tap on the game.
    //
    // A 4:3 guest on this phone is shown 536pt wide inside an 874pt window, so
    // the black bars are 338pt - 39% of the screen - and until now every one of
    // those taps was claimed, converted to a guest coordinate outside the
    // screen, and clamped by KNativeInputSDL::checkMousePos onto the picture's
    // edge. A finger resting near the bezel therefore clicked the left or right
    // edge of the guest, which in a visual novel is a menu strip or a
    // text-advance target. That is the "it selects things I did not tap".
    //
    // Trackpad mode is the exception and must stay one: there the whole screen
    // is the trackpad surface and the finger's absolute position is not where
    // the click goes.
    const BOOL insidePicture =
        presentation != nil &&
        CGRectContainsPoint(presentation.frame,
                            [self convertPoint:point
                                         toView:presentation.superview]);
    const BOOL claim = (softwareGuest &&
                        (self.trackpadMode || insideSoftwarePicture)) ||
        (presentation != nil && (self.trackpadMode || insidePicture));

    // Report what happened, at most once a second. Build 68 removed this line
    // when the ownership change went in and immediately needed it back: the
    // device report was that in portrait even the menu button stopped
    // responding, which cannot be explained by anything in this method and
    // means the overlay is not being hit-tested at all. Naming what UIKit
    // asked and what was answered is the only way to tell those apart.
    static NSTimeInterval lastReport = 0.0;
    const NSTimeInterval now = CACurrentMediaTime();
    if (now - lastReport >= 1.0) {
        lastReport = now;
        NSString* message = [NSString stringWithFormat:
            @"Overlay hit test at %.0f,%.0f in bounds %.0fx%.0f (window %@, "
             "menu button %.0f,%.0f %.0fx%.0f, presenting view %@ frame "
             "%.0f,%.0f %.0fx%.0f) -> %@",
            point.x, point.y, self.bounds.size.width, self.bounds.size.height,
            self.window == nil ? @"DETACHED" : @"attached",
            self.menuButton.frame.origin.x, self.menuButton.frame.origin.y,
            self.menuButton.frame.size.width, self.menuButton.frame.size.height,
            presentation == nil ? @"none" : NSStringFromClass(presentation.class),
            presentation.frame.origin.x, presentation.frame.origin.y,
            presentation.frame.size.width, presentation.frame.size.height,
            softwareGuest ? (claim ? @"claimed for software guest"
                                   : @"ignored: outside software picture")
                          : presentation == nil ? @"passed down to SDL"
                                : (claim ? @"claimed for the guest"
                                         : @"ignored: outside the picture")];
        BVNLogWrite(BVNLogLevelInfo, "input", message.UTF8String);
    }
    return claim ? self : nil;
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

- (BOOL)guestSizeWidth:(CGFloat*)width height:(CGFloat*)height {
    UIView* presentation = BVNGuestPresentationView();
    if (presentation != nil) {
        if (width) { *width = presentation.bounds.size.width; }
        if (height) { *height = presentation.bounds.size.height; }
        return presentation.bounds.size.width > 0.0 &&
               presentation.bounds.size.height > 0.0;
    }
    int guestWidth = 0;
    int guestHeight = 0;
    if (!BVNGuestControlsScreenSize(&guestWidth, &guestHeight)) {
        return NO;
    }
    if (width) { *width = guestWidth; }
    if (height) { *height = guestHeight; }
    return YES;
}

- (CGPoint)guestPointForTouch:(UITouch*)touch {
    UIView* presentation = BVNGuestPresentationView();
    if (presentation != nil) {
        return [touch locationInView:presentation];
    }
    CGFloat guestWidth = 0.0;
    CGFloat guestHeight = 0.0;
    if (![self guestSizeWidth:&guestWidth height:&guestHeight] ||
        self.bounds.size.width <= 0.0 || self.bounds.size.height <= 0.0) {
        return CGPointZero;
    }
    const CGPoint local = [touch locationInView:self];
    float mappedX = 0.0f;
    float mappedY = 0.0f;
    if (BVNGuestControlsMapSoftwarePoint((float)local.x, (float)local.y,
                                         &mappedX, &mappedY)) {
        return CGPointMake(mappedX, mappedY);
    }
    return CGPointMake(local.x * guestWidth / self.bounds.size.width,
                       local.y * guestHeight / self.bounds.size.height);
}

- (BOOL)sendGuestPointer:(UITouch*)touch phase:(int)phase {
    CGFloat guestWidth = 0.0;
    CGFloat guestHeight = 0.0;
    if (![self guestSizeWidth:&guestWidth height:&guestHeight]) {
        return NO;
    }
    const CGPoint point = [self guestPointForTouch:touch];
    if (point.x < 0.0 || point.y < 0.0 ||
        point.x >= guestWidth || point.y >= guestHeight) {
        return NO;
    }
    BVNGuestControlsSendPointer((int)lround(point.x), (int)lround(point.y),
                                phase);
    if (phase != 2) {
        self.directLastGuestPoint = point;
    }
    return YES;
}

// A light impact, not a selection or a notification: the gesture being
// reported is a button going down under the finger, and the heavier styles
// read as an error beside a game's own audio.
- (void)playTrackpadHoldFeedback {
    if (self.trackpadHoldHaptic == nil) {
        self.trackpadHoldHaptic = [[UIImpactFeedbackGenerator alloc]
            initWithStyle:UIImpactFeedbackStyleLight];
    }
    [self.trackpadHoldHaptic impactOccurred];
    // Re-arm for the next hold: the engine idles again after an impact.
    [self.trackpadHoldHaptic prepare];
    [self reportTrackpadHoldEngaged];
}

// One line per engagement, budgeted. A report of "the drag never started"
// then reads as either a run with no line - the hold threshold was never
// reached - or a line the guest did not act on, which is a different defect.
- (void)reportTrackpadHoldEngaged {
    static int budget = 24;
    if (budget <= 0) {
        return;
    }
    --budget;
    NSString* line = [NSString stringWithFormat:
        @"BOXEDVN_POINTER_HOLD engaged=1 guest=%.0f,%.0f",
        self.cursorGuestPoint.x, self.cursorGuestPoint.y];
    BVNLogWrite(BVNLogLevelInfo, "input", line.UTF8String);
}

- (void)trackpadHoldTimerFired:(NSTimer*)timer {
    if (timer != self.trackpadHoldTimer) {
        return;
    }
    self.trackpadHoldTimer = nil;
    if (!self.trackpadMode || self.guestTouch == nil ||
        self.trackpadTouchMoved || self.trackpadButtonHeld) {
        return;
    }
    // Only here. A tap, a move, the two-finger right-click and every release
    // are silent: this is the one transition the finger cannot see, because
    // the cursor does not move and the guest may not redraw, and the next
    // movement will drag rather than hover.
    [self playTrackpadHoldFeedback];
    BVNGuestControlsSendPointer((int)lround(self.cursorGuestPoint.x),
                                (int)lround(self.cursorGuestPoint.y), 1);
    self.trackpadButtonGuestPoint = self.cursorGuestPoint;
    self.trackpadButtonHeld = YES;
}

- (void)trackpadTapReleaseTimerFired:(NSTimer*)timer {
    if (timer != self.trackpadTapReleaseTimer) {
        return;
    }
    self.trackpadTapReleaseTimer = nil;
    if (!self.trackpadButtonHeld) {
        return;
    }
    BVNGuestControlsSendPointer(
        (int)lround(self.trackpadButtonGuestPoint.x),
        (int)lround(self.trackpadButtonGuestPoint.y), 2);
    self.trackpadButtonHeld = NO;
}

- (void)cancelTrackpadHoldTimer {
    [self.trackpadHoldTimer invalidate];
    self.trackpadHoldTimer = nil;
}

- (void)cancelTrackpadTapReleaseTimer {
    [self.trackpadTapReleaseTimer invalidate];
    self.trackpadTapReleaseTimer = nil;
}

// Keep UIKit's local cursor responsive, but feed at most 60 motion samples/s
// into Wine's message pump. Button edges flush the latest point immediately.
- (void)flushPointerMotion {
    [self.pointerMotionTimer invalidate];
    self.pointerMotionTimer = nil;
    self.lastPointerMotionTime = CACurrentMediaTime();
    BVNGuestControlsSendPointer((int)lround(self.cursorGuestPoint.x),
                                (int)lround(self.cursorGuestPoint.y), 0);
}

- (void)queuePointerMotion {
    if (self.pointerMotionTimer != nil) return;
    const double delay = 1.0 / 60.0 - (CACurrentMediaTime() - self.lastPointerMotionTime);
    if (delay <= 0) {
        [self flushPointerMotion];
        return;
    }
    __weak BVNGuestOverlayView* weakSelf = self;
    self.pointerMotionTimer = [NSTimer timerWithTimeInterval:delay repeats:NO
        block:^(NSTimer* timer) { [weakSelf flushPointerMotion]; }];
    [[NSRunLoop mainRunLoop] addTimer:self.pointerMotionTimer forMode:NSRunLoopCommonModes];
}

- (void)cancelTrackpadGesture {
    if (self.pointerMotionTimer != nil) [self flushPointerMotion];
    [self cancelTrackpadHoldTimer];
    [self cancelTrackpadTapReleaseTimer];
    if (self.trackpadButtonHeld) {
        BVNGuestControlsSendPointer(
            (int)lround(self.trackpadButtonGuestPoint.x),
            (int)lround(self.trackpadButtonGuestPoint.y), 2);
    }
    self.trackpadButtonHeld = NO;
    self.trackpadHasMotionBaseline = NO;
    self.guestTouch = nil;
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (self.guestTouch != nil) {
        // A second finger is for the right-click gesture, not a second cursor.
        return;
    }
    UITouch* touch = touches.anyObject;
    CGFloat guestWidth = 0.0;
    CGFloat guestHeight = 0.0;
    if (touch == nil || ![self guestSizeWidth:&guestWidth height:&guestHeight]) {
        [super touchesBegan:touches withEvent:event];
        return;
    }
    if (self.trackpadMode && self.trackpadTapReleaseTimer != nil) {
        // Finish the preceding tap before accepting another gesture; never
        // overwrite a held injected button with the next touch's state.
        [self cancelTrackpadGesture];
    }
    self.guestTouch = touch;

    if (self.trackpadMode) {
        // A stationary hold presses the left button after a short threshold;
        // movement after that drags, while an ordinary quick tap still clicks
        // on release.
        self.trackpadTouchStart = [touch locationInView:self];
        self.trackpadTouchMoved = NO;
        self.trackpadLastPoint = [touch locationInView:self];
        self.trackpadHasMotionBaseline = NO;
        self.trackpadButtonHeld = NO;
        [self cancelTrackpadHoldTimer];
        // Warm the Taptic engine now, while the 0.35s threshold runs: an
        // unprepared generator can take longer than the impact is worth.
        if (self.trackpadHoldHaptic == nil) {
            self.trackpadHoldHaptic = [[UIImpactFeedbackGenerator alloc]
                initWithStyle:UIImpactFeedbackStyleLight];
        }
        [self.trackpadHoldHaptic prepare];
        self.trackpadHoldTimer = [NSTimer
            timerWithTimeInterval:0.35
                           target:self
                         selector:@selector(trackpadHoldTimerFired:)
                         userInfo:nil
                          repeats:NO];
        [[NSRunLoop mainRunLoop] addTimer:self.trackpadHoldTimer
                                  forMode:NSRunLoopCommonModes];
        return;
    }
    self.cursorGuestPoint = [self guestPointForTouch:touch];
    [self positionCursor];
    self.directButtonHeld = [self sendGuestPointer:touch phase:1];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = [self guestTouchIn:touches];
    if (touch == nil) {
        return;
    }
    if (!self.trackpadMode) {
        self.cursorGuestPoint = [self guestPointForTouch:touch];
        [self positionCursor];
        [self sendGuestPointer:touch phase:0];
        return;
    }

    CGFloat guestWidth = 0.0;
    CGFloat guestHeight = 0.0;
    if (![self guestSizeWidth:&guestWidth height:&guestHeight]) { return; }
    // Measure on the stable overlay, then convert the delta into guest pixels.
    // This works for both the transformed Metal view and SDL's software path.
    const CGPoint now = [touch locationInView:self];
    if (!self.trackpadHasMotionBaseline) {
        self.trackpadHasMotionBaseline = YES;
        // Establish a new baseline for every gesture. UIKit can report a tiny
        // first displacement when the finger is lifted and put down again;
        // treating that sample as motion is the visible cursor "jump". Once
        // a deliberate hold already pressed the button, keep the delta so a
        // title-bar drag begins with the first movement.
        if (!self.trackpadButtonHeld) {
            self.trackpadLastPoint = now;
            const CGFloat totalX = now.x - self.trackpadTouchStart.x;
            const CGFloat totalY = now.y - self.trackpadTouchStart.y;
            if (hypot(totalX, totalY) > 4.0) {
                self.trackpadTouchMoved = YES;
                [self cancelTrackpadHoldTimer];
            }
            return;
        }
    }
    const CGPoint before = self.trackpadLastPoint;
    self.trackpadLastPoint = now;
    CGFloat dx = 0.0;
    CGFloat dy = 0.0;
    UIView* presentation = BVNGuestPresentationView();
    if (presentation != nil) {
        // Convert both samples through UIKit's actual presentation transform.
        // Scaling the overlay's X/Y independently by the guest dimensions
        // made portrait trackpad motion about four times faster horizontally
        // than vertically because the landscape guest occupies only part of
        // the portrait window. Point conversion accounts for letterboxing,
        // rotation, and scale while one sensitivity value remains isotropic.
        const CGPoint guestBefore = [self convertPoint:before
                                                toView:presentation];
        const CGPoint guestNow = [self convertPoint:now
                                             toView:presentation];
        dx = guestNow.x - guestBefore.x;
        dy = guestNow.y - guestBefore.y;
    } else {
        dx = (now.x - before.x) * guestWidth /
             MAX(1.0, self.bounds.size.width);
        dy = (now.y - before.y) * guestHeight /
             MAX(1.0, self.bounds.size.height);
        float beforeX = 0.0f;
        float beforeY = 0.0f;
        float nowX = 0.0f;
        float nowY = 0.0f;
        if (BVNGuestControlsMapSoftwarePoint((float)before.x, (float)before.y,
                                             &beforeX, &beforeY) &&
            BVNGuestControlsMapSoftwarePoint((float)now.x, (float)now.y,
                                             &nowX, &nowY)) {
            dx = nowX - beforeX;
            dy = nowY - beforeY;
        }
    }
    const CGFloat totalX = now.x - self.trackpadTouchStart.x;
    const CGFloat totalY = now.y - self.trackpadTouchStart.y;
    if (hypot(totalX, totalY) > 4.0) {
        self.trackpadTouchMoved = YES;
        if (!self.trackpadButtonHeld) {
            [self cancelTrackpadHoldTimer];
        }
    }
    // One scale, both axes: dx and dy are multiplied by the identical factor,
    // which is what keeps the cursor's motion 1:1 in aspect with the finger's.
    const CGFloat sensitivity = BVNPointerSensitivity();
    [self moveCursorBy:CGPointMake(dx * sensitivity,
                                   dy * sensitivity)];
    [self queuePointerMotion];
    if (self.trackpadButtonHeld) {
        self.trackpadButtonGuestPoint = self.cursorGuestPoint;
    }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = [self guestTouchIn:touches];
    if (touch == nil) {
        return;
    }
    self.guestTouch = nil;
    if (self.pointerMotionTimer != nil) [self flushPointerMotion];

    if (!self.trackpadMode) {
        if (self.directButtonHeld) {
            BVNGuestControlsSendPointer(
                (int)lround(self.directLastGuestPoint.x),
                (int)lround(self.directLastGuestPoint.y), 2);
        }
        self.directButtonHeld = NO;
        return;
    }
    [self cancelTrackpadHoldTimer];
    const int x = (int)lround(self.cursorGuestPoint.x);
    const int y = (int)lround(self.cursorGuestPoint.y);
    if (self.trackpadButtonHeld) {
        BVNGuestControlsSendPointer(x, y, 2);
        self.trackpadButtonHeld = NO;
    } else if (!self.trackpadTouchMoved) {
        // A touchpad tap is recognized only at finger-up, but sending down and
        // up back-to-back makes the injected button mask disappear before a
        // polling Windows engine gets scheduled. Hold it for one short input
        // quantum while keeping both halves at the same guest pixel.
        BVNGuestControlsSendPointer(x, y, 1);
        self.trackpadButtonGuestPoint = CGPointMake(x, y);
        self.trackpadButtonHeld = YES;
        [self cancelTrackpadTapReleaseTimer];
        self.trackpadTapReleaseTimer = [NSTimer
            timerWithTimeInterval:0.06
                           target:self
                         selector:@selector(trackpadTapReleaseTimerFired:)
                         userInfo:nil
                          repeats:NO];
        [[NSRunLoop mainRunLoop] addTimer:self.trackpadTapReleaseTimer
                                  forMode:NSRunLoopCommonModes];
    }
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = [self guestTouchIn:touches];
    if (touch == nil) {
        return;
    }
    if (!self.trackpadMode) {
        // Release the button even on a cancel, or the guest is left with it
        // held.
        if (self.directButtonHeld) {
            BVNGuestControlsSendPointer(
                (int)lround(self.directLastGuestPoint.x),
                (int)lround(self.directLastGuestPoint.y), 2);
        }
        self.directButtonHeld = NO;
    } else {
        [self cancelTrackpadGesture];
    }
    self.guestTouch = nil;
}

// ---------------------------------------------------------------------------
// The virtual cursor
// ---------------------------------------------------------------------------

- (void)moveCursorBy:(CGPoint)delta {
    CGFloat guestWidth = 0.0;
    CGFloat guestHeight = 0.0;
    if (![self guestSizeWidth:&guestWidth height:&guestHeight]) {
        return;
    }
    CGPoint point = self.cursorGuestPoint;
    point.x = MIN(MAX(point.x + delta.x, 0.0), guestWidth - 1.0);
    point.y = MIN(MAX(point.y + delta.y, 0.0), guestHeight - 1.0);
    self.cursorGuestPoint = point;
    [self positionCursor];
}

- (UIImage*)outlinedGuestCursorSymbolNamed:(NSString*)name {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    const CGFloat outlineOpacity =
        [defaults doubleForKey:kBVNPointerOutlineOpacityKey];
    const CGFloat thickness = MAX(
        0.5, [defaults doubleForKey:kBVNPointerThicknessKey] * 0.5);
    UIImageSymbolConfiguration* configuration =
        [UIImageSymbolConfiguration configurationWithPointSize:20.0
                                                        weight:UIImageSymbolWeightBold];
    UIImage* source = [UIImage systemImageNamed:name
                               withConfiguration:configuration];
    if (source == nil) {
        return nil;
    }
    UIImage* black = [source imageWithTintColor:
        [UIColor colorWithWhite:0.0 alpha:outlineOpacity]
                                  renderingMode:UIImageRenderingModeAlwaysOriginal];
    UIImage* white = [source imageWithTintColor:UIColor.whiteColor
                                  renderingMode:UIImageRenderingModeAlwaysOriginal];
    const CGSize size = CGSizeMake(30.0, 30.0);
    UIGraphicsImageRenderer* renderer = [[UIGraphicsImageRenderer alloc]
        initWithSize:size];
    return [renderer imageWithActions:^(UIGraphicsImageRendererContext* ctx) {
        const CGRect base = CGRectMake((size.width - source.size.width) / 2.0,
                                       (size.height - source.size.height) / 2.0,
                                       source.size.width, source.size.height);
        const CGPoint offsets[] = {
            {-thickness, 0.0}, {thickness, 0.0},
            {0.0, -thickness}, {0.0, thickness},
            {-thickness * 0.7, -thickness * 0.7},
            {thickness * 0.7, -thickness * 0.7},
            {-thickness * 0.7, thickness * 0.7},
            {thickness * 0.7, thickness * 0.7},
        };
        for (const CGPoint offset : offsets) {
            [black drawInRect:CGRectOffset(base, offset.x, offset.y)];
        }
        [white drawInRect:base];
    }];
}

- (UIImage*)fallbackGuestCursorImageForShape:(int)shape {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    const CGFloat outlineOpacity =
        [defaults doubleForKey:kBVNPointerOutlineOpacityKey];
    const CGFloat outlineWidth = MAX(
        0.5, [defaults doubleForKey:kBVNPointerThicknessKey]);
    // Draw the default arrow ourselves. SF Symbols' cursorarrow is a solid
    // white glyph with no Windows-style black outline, so it vanishes on a
    // white game screen even though its hotspot is correct.
    if (shape == 0 || shape == 22 || shape == 68 || shape == 132) {
        UIGraphicsImageRenderer* renderer = [[UIGraphicsImageRenderer alloc]
            initWithSize:CGSizeMake(24.0, 30.0)];
        return [renderer imageWithActions:^(UIGraphicsImageRendererContext* ctx) {
            UIBezierPath* arrow = [UIBezierPath bezierPath];
            [arrow moveToPoint:CGPointMake(2.0, 1.0)];
            [arrow addLineToPoint:CGPointMake(2.0, 24.0)];
            [arrow addLineToPoint:CGPointMake(8.0, 18.0)];
            [arrow addLineToPoint:CGPointMake(13.0, 28.0)];
            [arrow addLineToPoint:CGPointMake(18.0, 25.0)];
            [arrow addLineToPoint:CGPointMake(13.0, 16.0)];
            [arrow addLineToPoint:CGPointMake(22.0, 15.0)];
            [arrow closePath];
            arrow.lineJoinStyle = kCGLineJoinRound;
            arrow.lineWidth = outlineWidth;
            [UIColor.whiteColor setFill];
            [arrow fill];
            [[UIColor colorWithWhite:0.0 alpha:outlineOpacity] setStroke];
            [arrow stroke];
        }];
    }
    NSString* symbolName = @"cursorarrow";
    if (shape == 152) {
        symbolName = @"character.cursor.ibeam";
    } else if (shape == 130) {
        symbolName = @"plus";
    } else if (shape == 150 || shape == 1000) {
        symbolName = @"hourglass";
    } else if (shape == 108) {
        symbolName = @"arrow.left.and.right";
    } else if (shape == 116) {
        symbolName = @"arrow.up.and.down";
    } else if (shape == 60) {
        symbolName = @"hand.point.up.left.fill";
    } else if (shape == 88) {
        symbolName = @"nosign";
    }
    UIImage* symbol = [self outlinedGuestCursorSymbolNamed:symbolName];
    if (symbol != nil) {
        return symbol;
    }

    // Old iOS versions do not have cursorarrow. Draw the familiar X11 arrow
    // rather than falling back to BoxedVN's ring in "Wine cursor" mode.
    UIGraphicsImageRenderer* renderer = [[UIGraphicsImageRenderer alloc]
        initWithSize:CGSizeMake(24.0, 30.0)];
    return [renderer imageWithActions:^(UIGraphicsImageRendererContext* ctx) {
        UIBezierPath* arrow = [UIBezierPath bezierPath];
        [arrow moveToPoint:CGPointMake(2.0, 1.0)];
        [arrow addLineToPoint:CGPointMake(2.0, 24.0)];
        [arrow addLineToPoint:CGPointMake(8.0, 18.0)];
        [arrow addLineToPoint:CGPointMake(13.0, 28.0)];
        [arrow addLineToPoint:CGPointMake(18.0, 25.0)];
        [arrow addLineToPoint:CGPointMake(13.0, 16.0)];
        [arrow addLineToPoint:CGPointMake(22.0, 15.0)];
        [arrow closePath];
        arrow.lineWidth = outlineWidth;
        [UIColor.whiteColor setFill];
        [arrow fill];
        [[UIColor colorWithWhite:0.0 alpha:outlineOpacity] setStroke];
        [arrow stroke];
    }];
}

- (void)applyGuestCursorState {
    const uint64_t revision =
        gGuestCursorRevision.load(std::memory_order_acquire);
    if (revision == self.appliedGuestCursorRevision) {
        return;
    }
    self.appliedGuestCursorRevision = revision;
    const uint32_t cursorId =
        gSelectedGuestCursorId.load(std::memory_order_relaxed);
    BVNGuestCursorBitmap bitmap;
    bool hasBitmap = false;
    {
        std::lock_guard<std::mutex> lock(gGuestCursorMutex);
        auto found = gGuestCursorBitmaps.find(cursorId);
        if (found != gGuestCursorBitmaps.end()) {
            bitmap = found->second;
            hasBitmap = true;
        }
    }

    UIImage* image = nil;
    self.guestCursorUsesFallback = NO;
    if (hasBitmap) {
        NSData* data = [NSData dataWithBytes:bitmap.pixels.data()
                                      length:bitmap.pixels.size()];
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(
            (__bridge CFDataRef)data);
        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        CGImageRef cgImage = CGImageCreate(
            bitmap.width, bitmap.height, 8, 32, bitmap.width * 4,
            colorSpace,
            kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst,
            provider, nullptr, false, kCGRenderingIntentDefault);
        if (cgImage != nullptr) {
            image = [UIImage imageWithCGImage:cgImage scale:1.0
                                  orientation:UIImageOrientationUp];
            CGImageRelease(cgImage);
        }
        CGColorSpaceRelease(colorSpace);
        CGDataProviderRelease(provider);
        self.guestCursorHotspot = CGPointMake(bitmap.hotX, bitmap.hotY);
        self.guestCursorPixelSize = CGSizeMake(bitmap.width, bitmap.height);
    }
    if (image == nil) {
        if (!self.wineCursorOnlyMode) {
            self.guestCursorUsesFallback = YES;
            const int shape =
                gSelectedGuestCursorShape.load(std::memory_order_relaxed);
            image = [self fallbackGuestCursorImageForShape:shape];
            if (shape == 108 || shape == 116 || shape == 130 ||
                shape == 150 || shape == 152 || shape == 88 ||
                shape == 1000) {
                self.guestCursorHotspot = CGPointMake(image.size.width / 2.0,
                                                      image.size.height / 2.0);
            } else if (shape == 60) {
                self.guestCursorHotspot = CGPointMake(8.0, 3.0);
            } else {
                self.guestCursorHotspot = CGPointMake(2.0, 2.0);
            }
            self.guestCursorPixelSize = image.size;
        } else {
            self.guestCursorHotspot = CGPointZero;
            self.guestCursorPixelSize = CGSizeZero;
        }
    }
    self.guestCursorView.image = image;
    self.guestCursorVisible =
        gSelectedGuestCursorVisible.load(std::memory_order_relaxed);
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    const CGFloat thickness =
        [defaults doubleForKey:kBVNPointerThicknessKey];
    self.guestCursorView.layer.shadowOpacity = self.guestCursorUsesFallback
        ? (float)[defaults doubleForKey:kBVNPointerShadowOpacityKey]
        : 1.0f;
    self.guestCursorView.layer.shadowRadius = self.guestCursorUsesFallback
        ? MAX(0.5, thickness * 0.75) : 2.0;
}

- (CGPoint)overlayPointForGuestPoint:(CGPoint)point {
    UIView* presentation = BVNGuestPresentationView();
    if (presentation != nil) {
        return [presentation convertPoint:point toView:self];
    }
    float windowX = 0.0f;
    float windowY = 0.0f;
    if (BVNGuestControlsMapSoftwarePointToWindow(
            (float)point.x, (float)point.y, &windowX, &windowY)) {
        return CGPointMake(windowX, windowY);
    }
    CGFloat guestWidth = 1.0;
    CGFloat guestHeight = 1.0;
    [self guestSizeWidth:&guestWidth height:&guestHeight];
    return CGPointMake(point.x * self.bounds.size.width / MAX(1.0, guestWidth),
                       point.y * self.bounds.size.height / MAX(1.0, guestHeight));
}

- (void)positionCursor {
    CGFloat guestWidth = 0.0;
    CGFloat guestHeight = 0.0;
    // Direct-tap mode shows no cursor at all, in the live view host as well.
    // Showing the guest's bitmap there put a second, hotspot-shifted copy of
    // the pointer next to the one Wine already draws, which on the desktop
    // read as a black rectangle beside the cursor. The cursor belongs to
    // trackpad mode, where the finger is not on the pointer.
    if (!self.trackpadMode || !self.startupNotice.hidden ||
        ![self guestSizeWidth:&guestWidth height:&guestHeight]) {
        self.cursorView.hidden = YES;
        self.guestCursorView.hidden = YES;
        return;
    }
    if (self.appliedPointerRevision == 0 &&
        CGPointEqualToPoint(self.cursorGuestPoint, CGPointZero)) {
        self.cursorGuestPoint = CGPointMake(guestWidth / 2.0,
                                            guestHeight / 2.0);
    }
    const CGPoint cursorPoint =
        [self overlayPointForGuestPoint:self.cursorGuestPoint];
    self.cursorView.hidden = self.guestCursorMode || !self.trackpadMode;
    self.cursorView.center = cursorPoint;

    [self applyGuestCursorState];
    // The fallback-assisted mode stays visible when a game hides Wine's
    // cursor so a touch-only trackpad never loses position feedback. The
    // Wine-only mode is deliberately literal: no bitmap and a guest hide
    // request both produce no cursor.
    self.guestCursorView.hidden = !self.guestCursorMode ||
                                  self.guestCursorView.image == nil ||
        (self.wineCursorOnlyMode && !self.guestCursorVisible);
    if (!self.guestCursorView.hidden) {
        NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
        const CGFloat guestScale = self.guestCursorUsesFallback
            ? MAX(0.5, [defaults doubleForKey:kBVNPointerSizeKey] / 22.0)
            : 1.0;
        // Anchored on the cursor point the ring already uses, with one scale
        // taken from the picture's own mapping. Mapping the bitmap's two
        // corners independently, as this did, let a corner that falls outside
        // the picture take a different branch of overlayPointForGuestPoint
        // from the other one: the frame then stretched away from the pointer
        // and the cursor bitmap was drawn as a bar beside it.
        const CGPoint pictureOrigin =
            [self overlayPointForGuestPoint:CGPointZero];
        const CGPoint pictureFar = [self overlayPointForGuestPoint:
            CGPointMake(guestWidth, guestHeight)];
        CGFloat scaleX = fabs(pictureFar.x - pictureOrigin.x) /
                         MAX(1.0, guestWidth);
        CGFloat scaleY = fabs(pictureFar.y - pictureOrigin.y) /
                         MAX(1.0, guestHeight);
        if (scaleX <= 0.0 || scaleY <= 0.0) {
            scaleX = 1.0;
            scaleY = 1.0;
        }
        const CGSize size = CGSizeMake(
            MAX(1.0, self.guestCursorPixelSize.width * guestScale * scaleX),
            MAX(1.0, self.guestCursorPixelSize.height * guestScale * scaleY));
        self.guestCursorView.frame = CGRectMake(
            cursorPoint.x - self.guestCursorHotspot.x * guestScale * scaleX,
            cursorPoint.y - self.guestCursorHotspot.y * guestScale * scaleY,
            size.width, size.height);
        [self bringSubviewToFront:self.guestCursorView];
    }
    [self bringSubviewToFront:self.cursorView];
}

- (void)setPointerMode:(NSInteger)mode {
    mode = MAX(0, MIN(1, mode));
    const BOOL enabled = mode != 0;
    const BOOL guestCursor = enabled;
    const BOOL wineCursorOnly = NO;
    if (self.trackpadMode == enabled &&
        self.guestCursorMode == guestCursor &&
        self.wineCursorOnlyMode == wineCursorOnly) {
        return;
    }
    [self cancelTrackpadGesture];
    self.trackpadMode = enabled;
    self.guestCursorMode = guestCursor;
    self.wineCursorOnlyMode = wineCursorOnly;
    // The selected guest cursor may not have changed, but changing between
    // fallback-assisted and Wine-only presentation changes what the same
    // revision must display.
    self.appliedGuestCursorRevision = UINT64_MAX;
    if (enabled) {
        // Start in the middle of the picture rather than at 0,0, which on a
        // Wine desktop is behind the menu button.
        CGFloat guestWidth = 0.0;
        CGFloat guestHeight = 0.0;
        if ([self guestSizeWidth:&guestWidth height:&guestHeight]) {
            self.cursorGuestPoint = CGPointMake(guestWidth / 2.0,
                                                guestHeight / 2.0);
        }
    }
    [[NSUserDefaults standardUserDefaults] setBool:enabled
                                            forKey:kBVNTrackpadModeKey];
    [[NSUserDefaults standardUserDefaults] setInteger:mode
                                               forKey:kBVNPointerModeKey];
    [self positionCursor];
    BVNLogWrite(BVNLogLevelInfo, "input",
                enabled
                    ? "Pointer mode: trackpad with Wine cursor and fallback."
                    : "Pointer mode: direct tap.");
}

// ---------------------------------------------------------------------------
// Layout
//
// These four methods went missing in build 70: a block replacement that was
// meant to rewrite the touch handlers spanned them as well, and nothing
// noticed because the overlay still receives a frame from its constraints and
// still hit-tests. Its *subviews* were simply never positioned, so every
// control kept the zero frame it was created with - which is exactly the
// reported symptom, a menu button that is gone in games and on the desktop
// alike while touches still reach the guest.
// ---------------------------------------------------------------------------

- (void)layoutSubviews {
    [super layoutSubviews];
    if (self.layingOut) {
        return;
    }
    self.layingOut = YES;
    // The live view hosts its own controls and readout in the page, so the
    // in-guest menu button and performance panel are hidden there. They come
    // back for the full-screen presentation.
    const BOOL hosted = BVNGuestPresentationHostView() != nil;
    self.menuButton.hidden = hosted;
    self.performanceView.hidden = hosted;

    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGRect bounds = self.bounds;

    self.scrim.frame = bounds;
    self.startupNotice.frame = bounds;

    self.menuButton.bounds = CGRectMake(0.0, 0.0, kBVNMenuButtonSize,
                                        kBVNMenuButtonSize);
    const CGRect travel = [self menuButtonTravel];
    if (self.menuButtonFraction.x < 0.0) {
        self.menuButton.center = [self defaultMenuButtonCentre];
    } else {
        self.menuButton.center = CGPointMake(
            CGRectGetMinX(travel) + travel.size.width * self.menuButtonFraction.x,
            CGRectGetMinY(travel) + travel.size.height * self.menuButtonFraction.y);
    }

    [self layoutMenuPanelWithSafeArea:safe];
    [self layoutKeyboardPanelWithSafeArea:safe];
    [self layoutPointerSettingsPanelWithSafeArea:safe];
    [self layoutPerformanceSettingsPanelWithSafeArea:safe];
    [self layoutPerformanceOverlayWithSafeArea:safe];
    [self positionCursor];

    // Deliberately does NOT re-fit the guest picture. That belongs to the poll
    // in KNativeInputSDL::processEvents, which runs outside any UIKit layout
    // pass. Running the presenter from here means flushing the Metal view's
    // layout from inside this one, on a view in a sibling subtree; build 65
    // did that and UIKit stopped laying the subtree out at all after the first
    // rotation.
    self.layingOut = NO;
}

// UIKit reports the safe area separately from, and sometimes later than, the
// bounds change that accompanies a rotation. Build 64 fitted the guest picture
// during a portrait layout pass that was still reporting the landscape insets.
- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    [self setNeedsLayout];
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
        for (UIButton* item in @[self.keyboardItem, self.pointerItem,
                                 self.pointerSettingsItem, self.displayItem,
                                 self.frameRateItem,
                                 self.performanceItem,
                                 self.performanceSettingsItem,
                                 self.quitItem]) {
            item.frame = CGRectMake(inset, cursor, width - inset * 2.0,
                                    kBVNMenuRowHeight);
            cursor += kBVNMenuRowHeight;
        }
    }

    // Follow the menu button, then clamp inside the safe area. The button is
    // draggable, so the panel has to be able to open above it rather than
    // always down-and-right off the screen - and it must clear the Dynamic
    // Island.
    CGFloat x = CGRectGetMinX(self.menuButton.frame);
    CGFloat y = CGRectGetMaxY(self.menuButton.frame) + 8.0;
    const CGFloat maxX = self.bounds.size.width - safe.right -
                         kBVNOverlayMargin - width;
    const CGFloat maxY = self.bounds.size.height - safe.bottom -
                         kBVNOverlayMargin - cursor;
    if (y > maxY) {
        y = CGRectGetMinY(self.menuButton.frame) - 8.0 - cursor;
    }
    x = MIN(MAX(x, safe.left + kBVNOverlayMargin), MAX(maxX, safe.left));
    y = MIN(MAX(y, safe.top + kBVNOverlayMargin), MAX(maxY, safe.top));
    self.menuPanel.frame = CGRectMake(x, y, width, cursor);
}

// Where the on-screen keyboard belongs.
//
// Inside the overlay whenever the overlay is big enough to carry it: the
// full-screen session, the rotated landscape live-view host, an iPad's live
// view. The live view's portrait host on a phone is not - it is about
// 350x260pt, and six tappable rows are 200 of those points, so the keyboard
// there would *be* the live view. In that case the panel is anchored to the
// library window instead and comes up as a bottom sheet over the page, at the
// full width of the screen and covering none of the guest picture.
//
// It is the same panel, the same buttons and the same key routing either way;
// only the parent differs, and -layoutKeyboardPanelWithSafeArea: lays it out
// in whichever parent it currently has.
- (UIView*)keyboardContainer {
    const CGSize size = self.bounds.size;
    if (size.width <= 0.0 || size.height <= 0.0) {
        // No geometry yet - the overlay has been created but its constraints
        // have not resolved. Nothing to decide on, and moving the panel now
        // would only be undone by the first real layout pass.
        UIView* current = self.keyboardPanel.superview;
        if (current != nil) {
            return current;
        }
        return self;
    }
    if (size.height >= kBVNKeyboardInlineMinimumHeight &&
        size.width >= kBVNKeyboardInlineMinimumWidth) {
        return self;
    }
    // The host's window, not the overlay's: they are the same library window
    // today, but the host is what the page registered and is the thing that
    // is guaranteed to be in the window hierarchy.
    UIView* host = BVNGuestPresentationHostView();
    UIWindow* window = host.window;
    if (window == nil) {
        window = self.window;
    }
    if (window != nil) {
        return window;
    }
    // Not in a window yet - during construction, or after the overlay has
    // been detached. Keep it where it was built.
    return self;
}

- (void)updateKeyboardPlacement {
    if (self.keyboardPanel == nil) {
        return;
    }
    UIView* container = [self keyboardContainer];
    if (container == nil) {
        container = self;
    }
    if (self.keyboardPanel.superview != container) {
        [self.keyboardPanel removeFromSuperview];
        [container addSubview:self.keyboardPanel];
    }
    // A sheet has to stay above the page, and above the rotated full-screen
    // host if one is still parked on the same window. Only when it is not
    // already there: this runs from every layout pass, and an unconditional
    // reorder dirties the window's layout on each one.
    if (container != self && !self.keyboardPanel.hidden &&
        container.subviews.lastObject != self.keyboardPanel) {
        [container bringSubviewToFront:self.keyboardPanel];
    }
}

- (void)layoutKeyboardPanelWithSafeArea:(UIEdgeInsets)safe {
    [self updateKeyboardPlacement];
    UIView* container = self.keyboardPanel.superview;
    const NSUInteger rowCount = self.keyRows.count;
    if (container == nil || rowCount == 0) {
        return;
    }
    // As a sheet the panel is laid out in the window's coordinates and against
    // the window's safe area. The overlay's own insets are all zero there - it
    // is a small view in the middle of a page - so using them would put the
    // bottom row under the home indicator.
    const BOOL sheet = container != self;
    const CGRect area = container.bounds;
    const UIEdgeInsets insets = sheet ? container.safeAreaInsets : safe;
    const CGFloat width = area.size.width;
    if (width <= 0.0) {
        return;
    }

    // Six rows have to fit a 402pt-tall landscape phone without eating the
    // whole screen - at this fraction they take about 63% of it, and the
    // keyboard is summoned deliberately and dismissed again - while still
    // being tappable on a 402pt-wide portrait one, where the clamp takes over.
    const CGFloat usableHeight = area.size.height - insets.top;
    CGFloat rowHeight = floor(usableHeight * 0.085);
    rowHeight = MAX(kBVNKeyRowMinimumHeight,
                    MIN(kBVNKeyRowMaximumHeight, rowHeight));

    const CGFloat contentHeight =
        rowCount * rowHeight + (rowCount - 1) * kBVNKeyGap;
    const CGFloat panelHeight = contentHeight + kBVNKeyGap * 2.0 + insets.bottom;
    self.keyboardPanel.frame = CGRectMake(0.0,
                                          area.size.height - panelHeight,
                                          width, panelHeight);
    // Rounded at the top only when it is a sheet, so it reads as something
    // that came up over the page rather than as a slab of the page itself.
    self.keyboardPanel.layer.cornerRadius = sheet ? 16.0 : 0.0;
    self.keyboardPanel.layer.maskedCorners =
        kCALayerMinXMinYCorner | kCALayerMaxXMinYCorner;

    const CGFloat left = insets.left + 8.0;
    const CGFloat right = width - insets.right - 8.0;
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
                [UIFont systemFontOfSize:MIN(14.0, MIN(rowHeight * 0.38, (nextX - keyX) * 0.55))
                                  weight:UIFontWeightMedium];
            keyX = nextX + kBVNKeyGap;
        }
        rowY += rowHeight + kBVNKeyGap;
    }
}

// The rectangle the menu button's centre may occupy: the safe area, inset by
// half the button so it can never be dragged half off the screen.
// Where the menu button's centre is allowed to go.
//
// This used to be the safe area inset by the button's radius plus a margin,
// which in landscape stops it roughly where the 4:3 picture starts - the
// button could not be parked on the black bar or against the bezel. The travel
// is now the whole overlay, inset only by the radius so the button always
// stays fully on screen. The safe area only decides where it *starts*: see
// -defaultMenuButtonCentre.
- (CGRect)menuButtonTravel {
    const CGFloat half = kBVNMenuButtonSize / 2.0;
    CGRect travel = CGRectInset(self.bounds, half, half);
    if (travel.size.width < 0.0 || travel.size.height < 0.0) {
        travel = self.bounds;
    }
    return travel;
}

// The resting place before the player has ever dragged it: inside the safe
// area, clear of the Dynamic Island and the home indicator.
- (CGPoint)defaultMenuButtonCentre {
    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGRect travel = [self menuButtonTravel];
    const CGFloat half = kBVNMenuButtonSize / 2.0;
    CGPoint centre = CGPointMake(safe.left + kBVNOverlayMargin + half,
                                 safe.top + kBVNOverlayMargin + half);
    centre.x = MIN(MAX(centre.x, CGRectGetMinX(travel)), CGRectGetMaxX(travel));
    centre.y = MIN(MAX(centre.y, CGRectGetMinY(travel)), CGRectGetMaxY(travel));
    return centre;
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
    CGFloat guestWidth = 0.0;
    CGFloat guestHeight = 0.0;
    if (![self guestSizeWidth:&guestWidth height:&guestHeight]) {
        return;
    }
    // Midpoint of the two fingers, in the presenting view's bounds - which are
    // the guest resolution, and which -locationInView: resolves through the
    // view's scale transform for us.
    CGPoint point = self.trackpadMode
        ? self.cursorGuestPoint
        : presentation != nil
            ? [recognizer locationInView:presentation]
            : CGPointMake([recognizer locationInView:self].x * guestWidth /
                              MAX(1.0, self.bounds.size.width),
                          [recognizer locationInView:self].y * guestHeight /
                              MAX(1.0, self.bounds.size.height));
    if (point.x < 0.0 || point.y < 0.0 ||
        point.x >= guestWidth || point.y >= guestHeight) {
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
        self.pointerSettingsOpen = NO;
        self.performanceSettingsOpen = NO;
    }
    [self applyMenuState];
    BVNLogWrite(BVNLogLevelInfo, "input",
                self.menuOpen ? "Guest overlay menu opened."
                              : "Guest overlay menu closed.");
}

- (void)layoutPointerSettingsPanelWithSafeArea:(UIEdgeInsets)safe {
    const CGFloat availableWidth = self.bounds.size.width - safe.left -
                                   safe.right - kBVNOverlayMargin * 2.0;
    const CGFloat availableHeight = self.bounds.size.height - safe.top -
                                    safe.bottom - kBVNOverlayMargin * 2.0;
    const CGFloat width = MIN(540.0, MAX(280.0, availableWidth));
    const CGFloat height = MIN(350.0, MAX(280.0, availableHeight));
    const CGFloat x = safe.left + (availableWidth - width) / 2.0 +
                      kBVNOverlayMargin;
    const CGFloat y = safe.top + (availableHeight - height) / 2.0 +
                      kBVNOverlayMargin;
    self.pointerSettingsPanel.frame = CGRectMake(x, y, width, height);

    const CGFloat inset = 16.0;
    const CGFloat backHeight = 38.0;
    self.pointerSettingsBackItem.frame =
        CGRectMake(inset, 4.0, width - inset * 2.0, backHeight);
    const CGFloat rowTop = backHeight + 4.0;
    const CGFloat rowHeight = (height - rowTop - 8.0) /
                              self.pointerSettingLabels.count;
    const CGFloat labelWidth = MIN(160.0, width * 0.42);
    NSArray<UIControl*>* controls = @[
        self.pointerSensitivitySlider, self.pointerOpacitySlider,
        self.pointerSizeSlider,
        self.pointerOutlineSlider, self.pointerShadowSlider,
        self.pointerThicknessSlider, self.pointerOuterSwitch,
        self.pointerInnerSwitch,
    ];
    for (NSUInteger index = 0; index < controls.count; ++index) {
        const CGFloat rowY = rowTop + rowHeight * index;
        self.pointerSettingLabels[index].frame =
            CGRectMake(inset, rowY, labelWidth, rowHeight);
        UIControl* control = controls[index];
        if ([control isKindOfClass:UISwitch.class]) {
            control.center = CGPointMake(width - inset -
                                         control.bounds.size.width / 2.0,
                                         rowY + rowHeight / 2.0);
        } else {
            control.frame = CGRectMake(inset + labelWidth + 8.0,
                                       rowY,
                                       width - inset * 2.0 - labelWidth - 8.0,
                                       rowHeight);
        }
    }
}

- (void)layoutPerformanceSettingsPanelWithSafeArea:(UIEdgeInsets)safe {
    const CGFloat availableWidth = self.bounds.size.width - safe.left -
                                   safe.right - kBVNOverlayMargin * 2.0;
    const CGFloat availableHeight = self.bounds.size.height - safe.top -
                                    safe.bottom - kBVNOverlayMargin * 2.0;
    const CGFloat width = MIN(440.0, MAX(280.0, availableWidth));
    const CGFloat height = MIN(270.0, MAX(230.0, availableHeight));
    const CGFloat x = safe.left + (availableWidth - width) / 2.0 +
                      kBVNOverlayMargin;
    const CGFloat y = safe.top + (availableHeight - height) / 2.0 +
                      kBVNOverlayMargin;
    self.performanceSettingsPanel.frame = CGRectMake(x, y, width, height);
    const CGFloat inset = 16.0;
    self.performanceSettingsBackItem.frame =
        CGRectMake(inset, 4.0, width - inset * 2.0, 38.0);
    const CGFloat rowTop = 44.0;
    const CGFloat rowHeight = (height - rowTop - 8.0) /
                              self.performanceSettingLabels.count;
    NSArray<UISwitch*>* controls = @[self.performanceFPSSwitch,
                                     self.performanceRAMSwitch,
                                     self.performanceFrameTimeSwitch,
                                     self.performanceBatterySwitch];
    for (NSUInteger index = 0; index < controls.count; ++index) {
        const CGFloat rowY = rowTop + rowHeight * index;
        self.performanceSettingLabels[index].frame =
            CGRectMake(inset, rowY, width - 100.0, rowHeight);
        UISwitch* control = controls[index];
        control.center = CGPointMake(width - inset - control.bounds.size.width / 2.0,
                                     rowY + rowHeight / 2.0);
    }
}

- (void)layoutPerformanceOverlayWithSafeArea:(UIEdgeInsets)safe {
    const CGSize text = [self.performanceLabel
        sizeThatFits:CGSizeMake(260.0, CGFLOAT_MAX)];
    const CGSize size = CGSizeMake(MAX(156.0, ceil(text.width) + 20.0),
                                   MAX(38.0, ceil(text.height) + 16.0));
    self.performanceView.bounds = CGRectMake(0.0, 0.0, size.width, size.height);
    self.performanceLabel.frame = CGRectInset(self.performanceView.bounds,
                                               10.0, 8.0);
    const CGFloat minX = safe.left + kBVNOverlayMargin + size.width / 2.0;
    const CGFloat maxX = self.bounds.size.width - safe.right -
                         kBVNOverlayMargin - size.width / 2.0;
    const CGFloat minY = safe.top + kBVNOverlayMargin + size.height / 2.0;
    const CGFloat maxY = self.bounds.size.height - safe.bottom -
                         kBVNOverlayMargin - size.height / 2.0;
    self.performanceView.center = CGPointMake(
        minX + MAX(0.0, maxX - minX) * self.performanceFraction.x,
        minY + MAX(0.0, maxY - minY) * self.performanceFraction.y);
}

- (void)dragPerformanceOverlay:(UIPanGestureRecognizer*)recognizer {
    const CGPoint translation = [recognizer translationInView:self];
    [recognizer setTranslation:CGPointZero inView:self];
    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGFloat halfWidth = self.performanceView.bounds.size.width / 2.0;
    const CGFloat halfHeight = self.performanceView.bounds.size.height / 2.0;
    const CGFloat minX = safe.left + kBVNOverlayMargin + halfWidth;
    const CGFloat maxX = self.bounds.size.width - safe.right -
                         kBVNOverlayMargin - halfWidth;
    const CGFloat minY = safe.top + kBVNOverlayMargin + halfHeight;
    const CGFloat maxY = self.bounds.size.height - safe.bottom -
                         kBVNOverlayMargin - halfHeight;
    CGPoint center = self.performanceView.center;
    center.x = MIN(MAX(center.x + translation.x, minX), MAX(minX, maxX));
    center.y = MIN(MAX(center.y + translation.y, minY), MAX(minY, maxY));
    self.performanceView.center = center;
    if (recognizer.state == UIGestureRecognizerStateEnded ||
        recognizer.state == UIGestureRecognizerStateCancelled) {
        self.performanceFraction = CGPointMake(
            maxX > minX ? (center.x - minX) / (maxX - minX) : 0.0,
            maxY > minY ? (center.y - minY) / (maxY - minY) : 0.0);
    }
}

- (void)closeMenu {
    self.menuOpen = NO;
    self.confirmingQuit = NO;
    self.pointerSettingsOpen = NO;
    self.performanceSettingsOpen = NO;
    [self applyMenuState];
}

- (void)applyMenuState {
    self.menuButton.alpha = self.menuOpen ? 1.0 : 0.4;
    self.scrim.hidden = !self.menuOpen;
    self.menuPanel.hidden = !self.menuOpen || self.pointerSettingsOpen;
    self.menuPanel.hidden = self.menuPanel.hidden || self.performanceSettingsOpen;
    self.pointerSettingsPanel.hidden = !self.menuOpen ||
                                       !self.pointerSettingsOpen;
    self.performanceSettingsPanel.hidden = !self.menuOpen ||
                                           !self.performanceSettingsOpen;

    const BOOL confirming = self.confirmingQuit;
    self.keyboardItem.hidden = confirming;
    self.pointerItem.hidden = confirming;
    self.pointerSettingsItem.hidden = confirming;
    self.displayItem.hidden = confirming;
    self.frameRateItem.hidden = confirming;
    self.performanceItem.hidden = confirming;
    self.performanceSettingsItem.hidden = confirming;
    self.quitItem.hidden = confirming;
    self.quitPrompt.hidden = !confirming;
    self.quitCancelItem.hidden = !confirming;
    self.quitConfirmItem.hidden = !confirming;

    [self.keyboardItem setTitle:(self.keyboardPanel.hidden ? @"Show keyboard"
                                                           : @"Hide keyboard")
                       forState:UIControlStateNormal];
    NSString* pointerTitle = @"Pointer: direct tap";
    if (self.trackpadMode) {
        pointerTitle = @"Pointer: Wine cursor + fallback";
    }
    [self.pointerItem setTitle:pointerTitle
                      forState:UIControlStateNormal];
    const int displayMode = BVNGuestPresentationMode();
    [self.displayItem setTitle:(displayMode == 2
                                    ? @"Display: stretch"
                                    : displayMode == 1
                                    ? @"Display: fill aspect"
                                    : @"Display: fit aspect")
                      forState:UIControlStateNormal];
    const int frameRateMode = BVNGuestFrameRateMode();
    [self.frameRateItem setTitle:(frameRateMode == 1
                                      ? @"Frame rate: 60 FPS"
                                      : frameRateMode == 2
                                      ? @"Frame rate: 120 FPS"
                                      : @"Frame rate: uncapped")
                         forState:UIControlStateNormal];
    const BOOL performanceEnabled = [[NSUserDefaults standardUserDefaults]
        boolForKey:kBVNPerformanceEnabledKey];
    [self.performanceItem setTitle:(performanceEnabled
                                        ? @"Performance overlay: on"
                                        : @"Performance overlay: off")
                         forState:UIControlStateNormal];
    if (self.menuOpen) {
        [self bringSubviewToFront:self.scrim];
        [self bringSubviewToFront:self.menuPanel];
        [self bringSubviewToFront:self.pointerSettingsPanel];
        [self bringSubviewToFront:self.performanceSettingsPanel];
    }
    [self bringSubviewToFront:self.menuButton];
    [self setNeedsLayout];
}

- (void)toggleKeyboard {
    [self setKeyboardVisible:self.keyboardPanel.hidden];
    [self closeMenu];
}

// The single place the keyboard's visibility changes, whichever control asked
// for it: the in-guest menu row, the keyboard's own "close" key, or the live
// view's control bar through BVNGuestOverlayToggleKeyboard.
- (void)setKeyboardVisible:(BOOL)visible {
    if (!visible) {
        // A latched Ctrl must not survive the keyboard that latched it.
        [self releaseHeldKeys];
    }
    [self updateKeyboardPlacement];
    self.keyboardPanel.hidden = !visible;
    if (visible) {
        [self.keyboardPanel.superview bringSubviewToFront:self.keyboardPanel];
    }
    // The panel is framed by -layoutSubviews wherever it lives, so a sheet
    // that has just been parented to the window still needs this pass before
    // it can be seen; without it the first toggle shows a zero-sized view.
    [self setNeedsLayout];
    [self layoutIfNeeded];
    gLiveKeyboardVisible.store(visible ? true : false,
                               std::memory_order_relaxed);
    [self reportKeyboardVisibility];
}

// Called when the overlay itself is going away. Hiding is not enough: a sheet
// lives on the library window, which outlives both the overlay and the guest,
// and an abandoned one would sit over the page for the rest of the app's life.
- (void)dismissKeyboardForTeardown {
    [self releaseHeldKeys];
    self.keyboardPanel.hidden = YES;
    if (self.keyboardPanel.superview != self) {
        [self.keyboardPanel removeFromSuperview];
        [self addSubview:self.keyboardPanel];
    }
    gLiveKeyboardVisible.store(false, std::memory_order_relaxed);
    [self reportKeyboardVisibility];
}

// One line per change, budgeted: which container the keyboard went into and
// the rectangle it occupies there. A toggle that changes nothing visible is
// then a line with an empty or off-screen frame rather than a silence.
- (void)reportKeyboardVisibility {
    static int budget = 32;
    if (budget <= 0) {
        return;
    }
    --budget;
    UIView* container = self.keyboardPanel.superview;
    const CGRect frame = self.keyboardPanel.frame;
    NSString* host = container == nil
        ? @"none"
        : container == self
            ? @"overlay"
            : [container isKindOfClass:UIWindow.class] ? @"window" : @"other";
    NSString* line = [NSString stringWithFormat:
        @"BOXEDVN_GUEST_KEYBOARD visible=%d host=%@ frame=%.0f,%.0f,%.0fx%.0f",
        self.keyboardPanel.hidden ? 0 : 1, host,
        frame.origin.x, frame.origin.y,
        frame.size.width, frame.size.height];
    BVNLogWrite(BVNLogLevelInfo, "input", line.UTF8String);
}

- (void)togglePointerMode {
    [self setPointerMode:self.trackpadMode ? 0 : 1];
    [self applyMenuState];
}

- (void)toggleFrameRateMode {
    BVNGuestSetFrameRateMode((BVNGuestFrameRateMode() + 1) % 3);
    [self applyMenuState];
}

- (void)openPointerSettings {
    self.pointerSettingsOpen = YES;
    [self applyMenuState];
}

- (void)openPerformanceSettings {
    self.performanceSettingsOpen = YES;
    [self applyMenuState];
}

- (void)closePerformanceSettings {
    self.performanceSettingsOpen = NO;
    [self applyMenuState];
}

- (void)togglePerformanceOverlay {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    const BOOL enabled = ![defaults boolForKey:kBVNPerformanceEnabledKey];
    [defaults setBool:enabled forKey:kBVNPerformanceEnabledKey];
    self.performanceView.hidden = !enabled;
    self.performanceLastFrameCount =
        gPerformancePresentedFrames.load(std::memory_order_relaxed);
    self.performanceLastSampleTime = CACurrentMediaTime();
    [self updatePerformanceOverlay:self.performanceTimer];
    [self applyMenuState];
}

- (void)closePointerSettings {
    self.pointerSettingsOpen = NO;
    [self applyMenuState];
}

- (void)toggleDisplayMode {
    const int mode = (BVNGuestPresentationMode() + 1) % 3;
    BVNGuestSetPresentationMode(mode);
    [self applyMenuState];
    BVNLogWrite(BVNLogLevelInfo, "graphics", mode == 2
                    ? "Guest display mode: stretch."
                    : mode == 1
                    ? "Guest display mode: fill aspect."
                    : "Guest display mode: fit aspect.");
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
    [self cancelTrackpadGesture];
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

extern "C" UIView* BVNGuestPresentationHostView(void);

extern "C" void BVNGuestOverlayInstall(void) {
    if (!NSThread.isMainThread) {
        BVNLogWrite(BVNLogLevelWarning, "frontend",
                    "Ignored an off-main guest overlay install request.");
        return;
    }
    UIWindow* window = BVNGuestUIWindow();
    // The live view host, when the container page has registered one,
    // replaces the window as the overlay's container: same z-order role,
    // same pinned edges, only the parent differs.
    UIView* container = BVNGuestPresentationHostView() ?: (UIView*)window;
    if (window == nil) {
        BVNLogWrite(BVNLogLevelWarning, "frontend",
                    "Guest overlay not installed: SDL has no guest window "
                    "yet.");
        return;
    }
    if (gOverlay == nil) {
        gOverlay = [[BVNGuestOverlayView alloc] initWithFrame:container.bounds];
    }
    if (gOverlay.superview == container) {
        [container bringSubviewToFront:gOverlay];
        // The container can be the same object and still have changed size -
        // a rotation of the page, say - which is what decides whether the
        // keyboard belongs inside the overlay or over the page.
        [gOverlay updateKeyboardPlacement];
        return;
    }
    if (gOverlay.superview != nil) {
        BVNLogWrite(BVNLogLevelWarning, "frontend",
                    "The guest overlay was attached to a window that is no "
                    "longer SDL's; moving it. A stale attachment means no "
                    "control on it responds, including the menu button.");
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
    gOverlay.frame = container.bounds;
    gOverlay.translatesAutoresizingMaskIntoConstraints = NO;
    [container addSubview:gOverlay];
    [NSLayoutConstraint activateConstraints:@[
        [gOverlay.leadingAnchor constraintEqualToAnchor:container.leadingAnchor],
        [gOverlay.trailingAnchor constraintEqualToAnchor:container.trailingAnchor],
        [gOverlay.topAnchor constraintEqualToAnchor:container.topAnchor],
        [gOverlay.bottomAnchor constraintEqualToAnchor:container.bottomAnchor],
    ]];
    [gOverlay setNeedsLayout];

    BVNLogWrite(BVNLogLevelInfo, "frontend",
                "Guest overlay attached to SDL's guest window.");
}

// The Wine startup notice.
//
// Requested from either thread and applied on the main one. Build 70 required
// the main thread and dropped the request otherwise, which meant the notice
// was never hidden: the call that hides it lives in
// KNativeScreenSDL::putBitsOnWnd, which runs on the X server's thread.
static std::atomic<int> gStartupNoticeWanted{0};   // -1 hide, 0 unset, 1 show
// Written by the JIT allocator's thread. This is intentionally kept separate
// from translated units: an allocation can hold thousands of translations.
static std::atomic<size_t> gStartupNoticeAllocations{0};
extern "C" void BVNGuestOverlayApplyPendingState(void);

extern "C" void BVNGuestStartupNoticeSetVisible(bool visible) {
    gStartupNoticeWanted.store(visible ? 1 : -1, std::memory_order_relaxed);
    if (NSThread.isMainThread) {
        BVNGuestOverlayApplyPendingState();
    }
}

// Applies anything requested off the main thread, and re-asserts the overlay's
// place on top of SDL's views. Called from KNativeInputSDL::processEvents.
//
// Being on top is not something to assume. SDL adds its own view to the window
// *after* BVNAttachGuestWindowToScene installs the overlay, so on the
// software-rendered path - the Wine desktop, and the blue screen every game
// boots through - the overlay was buried from the moment it was created. That
// is why the menu button and the startup text were both invisible there while
// working fine in a Vulkan game, where registering the surface happened to
// re-front it.
extern "C" void BVNGuestOverlayApplyPendingState(void) {
    if (!NSThread.isMainThread || gOverlay == nil) {
        return;
    }
    const int wanted = gStartupNoticeWanted.exchange(0,
                                                     std::memory_order_relaxed);
    if (wanted != 0) {
        const BOOL visible = wanted > 0;
        gOverlay.startupNotice.hidden = !visible;
        if (visible) {
            gOverlay.appliedStartupLogGeneration = UINT64_MAX;
            gOverlay.startupLastActivityTime = CACurrentMediaTime();
            gOverlay.startupActivitySignature = nil;
            gOverlay.startupTranslationCount = 0;
            gOverlay.startupLastRIP = nil;
            [gOverlay refreshStartupActivity];
            [gOverlay bringSubviewToFront:gOverlay.startupNotice];
        }
    }
    if (wanted != 0) {
        // Trackpad mode may already be active from UserDefaults. Keep its
        // cursor behind the opaque startup page and reveal it only after Wine
        // has produced the desktop/game presentation.
        [gOverlay positionCursor];
    }
    const uint64_t pointerRevision =
        gGuestPointerRevision.load(std::memory_order_acquire);
    if (pointerRevision != gOverlay.appliedPointerRevision) {
        gOverlay.appliedPointerRevision = pointerRevision;
        gOverlay.cursorGuestPoint = CGPointMake(
            gGuestPointerX.load(std::memory_order_relaxed),
            gGuestPointerY.load(std::memory_order_relaxed));
        [gOverlay positionCursor];
    }
    const uint64_t cursorRevision =
        gGuestCursorRevision.load(std::memory_order_acquire);
    if (cursorRevision != gOverlay.appliedGuestCursorRevision) {
        [gOverlay applyGuestCursorState];
        [gOverlay positionCursor];
    }
    if (!gOverlay.startupNotice.hidden) {
        [gOverlay refreshStartupActivity];
    }
    UIView* parent = gOverlay.superview;
    if (parent != nil && parent.subviews.lastObject != gOverlay) {
        [parent bringSubviewToFront:gOverlay];
    }
    // A keyboard sheet lives on the library window, outside the overlay's
    // subtree, so being on top has to be re-asserted for it separately -
    // anything the page presents lands on the same window.
    UIView* keyboardParent = gOverlay.keyboardPanel.superview;
    if (!gOverlay.keyboardPanel.hidden && keyboardParent != nil &&
        keyboardParent != gOverlay &&
        keyboardParent.subviews.lastObject != gOverlay.keyboardPanel) {
        [keyboardParent bringSubviewToFront:gOverlay.keyboardPanel];
    }
}

extern "C" uint64_t mythic_get_present_count(void) __attribute__((weak));

extern "C" uint64_t BVNGuestPresentedFrameCount(void) {
    // DXMT's counter counts every guest present, including frames its
    // mailbox mode drops; that is the rate the readout should show.
    if (mythic_get_present_count != nullptr) {
        const uint64_t guestPresents = mythic_get_present_count();
        if (guestPresents != 0) {
            return guestPresents;
        }
    }
    return gPerformancePresentedFrames.load(std::memory_order_relaxed);
}

extern "C" void BVNGuestPerformanceSnapshot(double* framesPerSecond,
                                           double* frameMilliseconds) {
    if (framesPerSecond) {
        *framesPerSecond = gLivePerformanceFps.load(std::memory_order_relaxed);
    }
    if (frameMilliseconds) {
        *frameMilliseconds =
            gLivePerformanceFrameMs.load(std::memory_order_relaxed);
    }
}

extern "C" void BVNGuestControlsSetKeyNamed(const char* name, bool down) {
    if (name) {
        BVNGuestControlsSendKey(BVNGuestControlsScancodeForName(name), down);
    }
}

extern "C" void BVNGuestControlsTapKeyNamed(const char* name) {
    if (name == NULL) {
        return;
    }
    const uint32_t scancode = BVNGuestControlsScancodeForName(name);
    if (scancode == 0) {
        return;
    }
    BVNGuestControlsSendKey(scancode, true);
    // Give a polling game a real press interval, rather than down/up in the
    // same main-loop iteration. The overlay keyboard already holds on touch.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        BVNGuestControlsSendKey(scancode, false);
    });
}

extern "C" int BVNGuestControlsPointerMode(void) {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    const NSInteger mode =
        [defaults objectForKey:@"BoxedVN.pointer.mode"]
            ? [defaults integerForKey:@"BoxedVN.pointer.mode"] : 0;
    return mode != 0 ? 1 : 0;
}

extern "C" void BVNGuestControlsSetPointerMode(int mode) {
    if (!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(), ^{
            BVNGuestControlsSetPointerMode(mode);
        });
        return;
    }
    const NSInteger clamped = mode != 0 ? 1 : 0;
    [[NSUserDefaults standardUserDefaults] setInteger:clamped
                                               forKey:@"BoxedVN.pointer.mode"];
    if (gOverlay != nil) {
        [gOverlay setPointerMode:clamped];
    }
}

// ---------------------------------------------------------------------------
// Mouse settings, shared by the in-guest pointer panel and the live view's
// long-press popover
//
// Both write the same NSUserDefaults keys, so there is one store and no
// synchronisation problem: the getter reads it, the setter clamps into it and
// then makes the running overlay show the result at once. Neither side owns a
// copy of the values.
// ---------------------------------------------------------------------------

extern "C" BVNPointerSettings BVNGuestPointerSettingsGet(void) {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    BVNPointerSettings settings;
    settings.size = (float)BVNPointerDefaultDouble(kBVNPointerSizeKey, 22.0,
                                                   kBVNPointerSizeMinimum,
                                                   kBVNPointerSizeMaximum);
    settings.thickness = (float)BVNPointerDefaultDouble(
        kBVNPointerThicknessKey, 2.0, kBVNPointerThicknessMinimum,
        kBVNPointerThicknessMaximum);
    settings.opacity = (float)BVNPointerDefaultDouble(
        kBVNPointerOpacityKey, 1.0, kBVNPointerOpacityMinimum,
        kBVNPointerOpacityMaximum);
    settings.sensitivity = (float)BVNPointerSensitivity();
    settings.outline = [defaults objectForKey:kBVNPointerOuterCircleKey] == nil
        ? true
        : [defaults boolForKey:kBVNPointerOuterCircleKey] != NO;
    return settings;
}

extern "C" void BVNGuestPointerSettingsSet(BVNPointerSettings settings) {
    if (!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(), ^{
            BVNGuestPointerSettingsSet(settings);
        });
        return;
    }
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    [defaults setDouble:BVNPointerClamp(settings.size, kBVNPointerSizeMinimum,
                                        kBVNPointerSizeMaximum)
                 forKey:kBVNPointerSizeKey];
    [defaults setDouble:BVNPointerClamp(settings.thickness,
                                        kBVNPointerThicknessMinimum,
                                        kBVNPointerThicknessMaximum)
                 forKey:kBVNPointerThicknessKey];
    [defaults setDouble:BVNPointerClamp(settings.opacity,
                                        kBVNPointerOpacityMinimum,
                                        kBVNPointerOpacityMaximum)
                 forKey:kBVNPointerOpacityKey];
    [defaults setDouble:BVNPointerClamp(settings.sensitivity,
                                        kBVNPointerSensitivityMinimum,
                                        kBVNPointerSensitivityMaximum)
                 forKey:kBVNPointerSensitivityKey];
    [defaults setBool:settings.outline ? YES : NO
               forKey:kBVNPointerOuterCircleKey];
    if (gOverlay != nil) {
        [gOverlay syncPointerSettingsControls];
    }
}

extern "C" void BVNGuestPerformanceFramePresented(void) {
    // Vulkan full frames and Wine's X11/GDI partial updates can arrive from
    // different threads for the same display interval. Count the union of
    // visible updates rather than Vulkan alone, but coalesce near-simultaneous
    // notifications so the overlay cannot double-count a mixed present.
    const uint64_t now = (uint64_t)(CACurrentMediaTime() * 1000000000.0);
    uint64_t previous =
        gPerformanceLastUpdateNanoseconds.load(std::memory_order_relaxed);
    while (previous == 0 || now - previous >= 8000000ull) {
        if (gPerformanceLastUpdateNanoseconds.compare_exchange_weak(
                previous, now, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            gPerformancePresentedFrames.fetch_add(1,
                                                  std::memory_order_relaxed);
            return;
        }
    }
}

extern "C" void BVNGuestOverlayGeometryDidChange(void) {
    if (!NSThread.isMainThread || gOverlay == nil) {
        return;
    }
    // UIKit can retain the pre-rotation touch-delivery chain even after every
    // recognizer/control is manually cancelled. Rebuild this lightweight view
    // so the post-rotation UIWindow gets a fresh responder and hit-test chain.
    // Pointer style/mode are UserDefaults-backed; preserve the movable menu
    // position and startup notice explicitly.
    BVNGuestOverlayView* oldOverlay = gOverlay;
    const CGPoint menuFraction = oldOverlay.menuButtonFraction;
    const CGPoint performanceFraction = oldOverlay.performanceFraction;
    const BOOL startupVisible = !oldOverlay.startupNotice.hidden;
    const BOOL keyboardVisible = !oldOverlay.keyboardPanel.hidden;
    [oldOverlay.performanceTimer invalidate];
    oldOverlay.performanceTimer = nil;
    // Takes the keyboard off the library window if it was a sheet there; the
    // rebuilt overlay puts its own back up below.
    [oldOverlay dismissKeyboardForTeardown];
    [oldOverlay releaseHeldKeys];
    [oldOverlay removeFromSuperview];
    gOverlay = nil;
    BVNGuestOverlayInstall();
    if (gOverlay != nil) {
        gOverlay.menuButtonFraction = menuFraction;
        gOverlay.performanceFraction = performanceFraction;
        gOverlay.startupNotice.hidden = !startupVisible;
        if (startupVisible) {
            gOverlay.appliedStartupLogGeneration = UINT64_MAX;
            gOverlay.startupLastActivityTime = CACurrentMediaTime();
            [gOverlay refreshStartupActivity];
        }
        [gOverlay setNeedsLayout];
        // A rotation is exactly when the keyboard is most likely to change
        // container, so it is re-shown through the same path a button press
        // takes rather than by restoring the old frame.
        if (keyboardVisible) {
            [gOverlay setKeyboardVisible:YES];
        }
    }
    BVNLogWrite(BVNLogLevelInfo, "input",
                "Rebuilt guest overlay after geometry change; the new scene "
                "has a fresh touch responder chain.");
}

// Called from the JIT's allocator, which is not the main thread. Recorded here
// and rendered by BVNGuestOverlayApplyPendingState.
extern "C" void BVNGuestStartupNoticeSetProgress(size_t allocationCount) {
    gStartupNoticeAllocations.store(allocationCount,
                                    std::memory_order_relaxed);
}

extern "C" void BVNGuestOverlayRemove(void) {
    if (!NSThread.isMainThread || gOverlay == nil) {
        return;
    }
    [gOverlay.performanceTimer invalidate];
    gOverlay.performanceTimer = nil;
    [gOverlay dismissKeyboardForTeardown];
    [gOverlay releaseHeldKeys];
    [gOverlay removeFromSuperview];
    gOverlay = nil;
    BVNLogWrite(BVNLogLevelInfo, "frontend", "Guest overlay removed.");
}

// ---------------------------------------------------------------------------
// The live view's keyboard button
//
// The control bar drives the overlay's own drawn keyboard, not SDL's system
// one: see BVNGuestControlsToggleKeyboard in BVNAppDelegate.mm for why the
// system keyboard cannot appear while the guest is hosted in the page.
//
// Returns false when there is no overlay to show, which is the same thing as
// "no guest is running"; the caller then has nothing to toggle.
// ---------------------------------------------------------------------------

extern "C" bool BVNGuestOverlayToggleKeyboard(void) {
    if (!NSThread.isMainThread || gOverlay == nil) {
        return false;
    }
    [gOverlay setKeyboardVisible:gOverlay.keyboardPanel.hidden];
    [gOverlay closeMenu];
    return true;
}

extern "C" void BVNGuestOverlayDismissKeyboard(void) {
    if (!NSThread.isMainThread || gOverlay == nil) {
        return;
    }
    [gOverlay dismissKeyboardForTeardown];
}
