// Editable landscape controls. Included by the UIKit guest overlay.
// Positions are normalized to the safe area so layouts survive rotation.
@interface BVNTouchLayout : UIView <UIPickerViewDataSource, UIPickerViewDelegate>
@property(nonatomic, strong) NSMutableArray<NSMutableDictionary*>* items;
@property(nonatomic, strong) NSMutableArray<UIButton*>* buttons;
@property(nonatomic, strong) NSMutableSet<NSNumber*>* held;
@property(nonatomic, copy) NSArray<NSString*>* keys;
@property(nonatomic, strong) UIView* editor;
@property(nonatomic, strong) UIPickerView* picker;
@property(nonatomic, strong) UISlider* sizeSlider;
@property(nonatomic, strong) UISlider* opacitySlider;
@property(nonatomic, strong) UILabel* selectionLabel;
@property(nonatomic, strong) UIStackView* toolbar;
@property(nonatomic, strong) UIButton* displayButton;
@property(nonatomic, strong) UIButton* menuButton;
@property(nonatomic) CGPoint menuPosition;
@property(nonatomic) BOOL menuOpen;
@property(nonatomic) BOOL menuDimmed;
@property(nonatomic, copy) void (^keyboardRequested)(void);
@property(nonatomic) NSInteger selection;
@property(nonatomic) BOOL editing;
@property(nonatomic, copy) void (^opacityChanged)(CGFloat);
- (void)editLayout;
- (void)releaseControls;
@end

@implementation BVNTouchLayout
- (instancetype)initWithFrame:(CGRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.multipleTouchEnabled = YES;
    self.items = [NSMutableArray array]; self.buttons = [NSMutableArray array];
    self.held = [NSMutableSet set]; self.selection = -1;
    NSMutableArray* keys = [NSMutableArray arrayWithArray:@[@"Mouse left", @"Mouse right", @"Mouse middle",
        @"Return", @"Space", @"Escape", @"Tab", @"Up", @"Down", @"Left", @"Right",
        @"Left Shift", @"Left Ctrl", @"Left Alt", @"Backspace"]];
    for (char c = 'A'; c <= 'Z'; ++c) [keys addObject:[NSString stringWithFormat:@"%c", c]];
    for (int n = 0; n <= 9; ++n) [keys addObject:[NSString stringWithFormat:@"%d", n]];
    for (int n = 1; n <= 12; ++n) [keys addObject:[NSString stringWithFormat:@"F%d", n]];
    self.keys = keys;
    self.toolbar = [[UIStackView alloc] init];
    self.toolbar.axis = UILayoutConstraintAxisHorizontal;
    self.toolbar.distribution = UIStackViewDistributionFillEqually;
    self.toolbar.spacing = 8;
    self.toolbar.backgroundColor = [UIColor colorWithWhite:0.06 alpha:0.85];
    self.toolbar.layer.cornerRadius = 12;
    NSArray* titles = @[@"Keyboard", @"Controls", @"Fit"];
    SEL actions[] = {@selector(showKeyboard), @selector(editLayout), @selector(changeDisplay)};
    for (NSUInteger i = 0; i < titles.count; ++i) {
        UIButton* button = [UIButton buttonWithType:UIButtonTypeSystem];
        [button setTitle:titles[i] forState:UIControlStateNormal];
        button.titleLabel.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightSemibold];
        [button addTarget:self action:actions[i] forControlEvents:UIControlEventTouchUpInside];
        [self.toolbar addArrangedSubview:button];
        if (i == 2) self.displayButton = button;
    }
    [self addSubview:self.toolbar];
    self.menuButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.menuButton setImage:[UIImage systemImageNamed:@"line.3.horizontal"] forState:UIControlStateNormal];
    self.menuButton.accessibilityLabel = @"Fullscreen menu";
    self.menuButton.accessibilityHint = @"Drag to reposition";
    self.menuButton.backgroundColor = [UIColor colorWithWhite:0.06 alpha:0.85];
    self.menuButton.layer.cornerRadius = 22;
    [self.menuButton addTarget:self action:@selector(wakeMenu) forControlEvents:UIControlEventTouchDown];
    [self.menuButton addTarget:self action:@selector(toggleMenu) forControlEvents:UIControlEventTouchUpInside];
    [self.menuButton addGestureRecognizer:[[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(dragMenu:)]];
    [self addSubview:self.menuButton];
    NSArray* menuPosition = [NSUserDefaults.standardUserDefaults arrayForKey:@"BoxedVN.fullscreenMenu.position"];
    self.menuPosition = menuPosition.count == 2 && [menuPosition[0] isKindOfClass:NSNumber.class] &&
        [menuPosition[1] isKindOfClass:NSNumber.class]
        ? CGPointMake(MAX(0, MIN(1, [menuPosition[0] doubleValue])), MAX(0, MIN(1, [menuPosition[1] doubleValue])))
        : CGPointMake(1, 0.5);
    id saved = [NSUserDefaults.standardUserDefaults arrayForKey:@"BoxedVN.touchLayout.v1"];
    for (id item in saved) {
        if (![item isKindOfClass:NSDictionary.class] || ![keys containsObject:item[@"key"]]) continue;
        NSMutableDictionary* copy = [item mutableCopy];
        for (NSString* field in @[@"x", @"y", @"size"])
            if (![copy[field] isKindOfClass:NSNumber.class]) copy[field] = @0.5;
        [self.items addObject:copy];
        if (self.items.count == 32) break;
    }
    [self rebuild];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(releaseControls)
        name:UIApplicationWillResignActiveNotification object:nil];
    return self;
}
- (void)dealloc { [NSObject cancelPreviousPerformRequestsWithTarget:self]; [self releaseControls]; [NSNotificationCenter.defaultCenter removeObserver:self]; }
- (UIView*)hitTest:(CGPoint)p withEvent:(UIEvent*)event {
    UIView* hit = [super hitTest:p withEvent:event];
    return hit == self ? nil : hit;
}
- (void)setHidden:(BOOL)hidden {
    if (hidden) {
        [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(dimMenu) object:nil];
        [self releaseControls]; self.editing = NO; self.editor.hidden = YES; self.menuOpen = NO;
    }
    [super setHidden:hidden];
    if (!hidden) [self wakeMenu];
}
- (void)wakeMenu {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(dimMenu) object:nil];
    self.menuDimmed = NO;
    [self.menuButton.layer removeAllAnimations];
    self.menuButton.alpha = self.controlOpacity;
    if (!self.hidden && !self.menuOpen && !self.editing)
        [self performSelector:@selector(dimMenu) withObject:nil afterDelay:4
                      inModes:@[NSRunLoopCommonModes]];
}
- (void)dimMenu {
    if (self.menuOpen || self.editing || self.hidden) return;
    self.menuDimmed = YES;
    [UIView animateWithDuration:0.3 animations:^{ self.menuButton.alpha = self.controlOpacity * 0.4; }];
}
- (void)toggleMenu {
    self.menuOpen = !self.menuOpen;
    [self wakeMenu]; [self setNeedsLayout];
}
- (void)dragMenu:(UIPanGestureRecognizer*)gesture {
    CGRect area = UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets);
    CGPoint delta = [gesture translationInView:self]; [gesture setTranslation:CGPointZero inView:self];
    self.menuPosition = CGPointMake(MAX(0, MIN(1, self.menuPosition.x + delta.x/MAX(1, area.size.width-52))),
                                   MAX(0, MIN(1, self.menuPosition.y + delta.y/MAX(1, area.size.height-52))));
    [self wakeMenu]; [self setNeedsLayout];
    if (gesture.state == UIGestureRecognizerStateEnded || gesture.state == UIGestureRecognizerStateCancelled)
        [NSUserDefaults.standardUserDefaults setObject:@[@(self.menuPosition.x), @(self.menuPosition.y)]
                                                forKey:@"BoxedVN.fullscreenMenu.position"];
}
- (CGFloat)controlOpacity {
    id value = [NSUserDefaults.standardUserDefaults objectForKey:@"BoxedVN.controls.opacity"];
    return value ? MAX(0.1, MIN(1.0, [value doubleValue])) : 0.7;
}
- (void)save { [NSUserDefaults.standardUserDefaults setObject:self.items forKey:@"BoxedVN.touchLayout.v1"]; }
- (void)showKeyboard {
    self.menuOpen = NO; [self wakeMenu]; [self setNeedsLayout];
    if (self.keyboardRequested) self.keyboardRequested();
}
- (void)changeDisplay {
    BVNGuestSetPresentationMode((BVNGuestPresentationMode() + 1) % 3);
    [self setNeedsLayout];
}
- (void)send:(NSInteger)index down:(BOOL)down {
    if (index < 0 || index >= (NSInteger)self.items.count) return;
    NSString* key = self.items[index][@"key"];
    if ([key hasPrefix:@"Mouse "]) {
        int button = [key isEqual:@"Mouse left"] ? 0 : [key isEqual:@"Mouse right"] ? 1 : 2;
        BVNGuestControlsSendRelativeButton(down ? 1 : 2, button);
    } else BVNGuestControlsSetKeyNamed(key.UTF8String, down);
}
- (void)releaseControls {
    for (NSNumber* index in self.held.allObjects) [self send:index.integerValue down:NO];
    [self.held removeAllObjects];
}
- (void)down:(UIButton*)button {
    if (self.editing) { self.selection = button.tag; [self refreshEditor]; return; }
    // Multiple buttons may map to the same key: release only its last holder.
    NSString* key = self.items[button.tag][@"key"];
    BOOL alreadyHeld = NO;
    for (NSNumber* index in self.held) if ([self.items[index.integerValue][@"key"] isEqual:key]) alreadyHeld = YES;
    [self.held addObject:@(button.tag)];
    if (!alreadyHeld) [self send:button.tag down:YES];
}
- (void)up:(UIButton*)button {
    if (![self.held containsObject:@(button.tag)]) return;
    [self.held removeObject:@(button.tag)];
    NSString* key = self.items[button.tag][@"key"];
    for (NSNumber* index in self.held) if ([self.items[index.integerValue][@"key"] isEqual:key]) return;
    [self send:button.tag down:NO];
}
- (void)rebuild {
    [self releaseControls];
    for (UIView* button in self.buttons) [button removeFromSuperview];
    [self.buttons removeAllObjects];
    for (NSUInteger i = 0; i < self.items.count; ++i) {
        UIButton* button = [UIButton buttonWithType:UIButtonTypeCustom];
        button.tag = i; button.exclusiveTouch = NO;
        [button setTitle:self.items[i][@"key"] forState:UIControlStateNormal];
        button.titleLabel.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightSemibold];
        button.titleLabel.adjustsFontSizeToFitWidth = YES;
        button.backgroundColor = [UIColor colorWithWhite:0.08 alpha:0.85];
        button.layer.cornerRadius = 12; button.layer.borderWidth = 1;
        button.layer.borderColor = UIColor.systemBlueColor.CGColor;
        [button addTarget:self action:@selector(down:) forControlEvents:UIControlEventTouchDown];
        [button addTarget:self action:@selector(up:) forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel];
        UIPanGestureRecognizer* drag = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(drag:)];
        drag.enabled = self.editing; [button addGestureRecognizer:drag];
        [self addSubview:button]; [self.buttons addObject:button];
    }
    if (self.editor) [self bringSubviewToFront:self.editor];
    [self bringSubviewToFront:self.toolbar];
    [self bringSubviewToFront:self.menuButton];
    [self setNeedsLayout];
}
- (void)layoutSubviews {
    [super layoutSubviews];
    CGRect area = UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets);
    self.menuButton.frame = CGRectMake(area.origin.x+4+self.menuPosition.x*MAX(0, area.size.width-52),
                                      area.origin.y+4+self.menuPosition.y*MAX(0, area.size.height-52), 44, 44);
    self.menuButton.hidden = self.editing;
    self.menuButton.alpha = self.controlOpacity * (self.menuDimmed ? 0.4 : 1);
    CGFloat menuWidth = MIN(276, area.size.width-8);
    CGFloat menuX = self.menuPosition.x < 0.5 ? CGRectGetMaxX(self.menuButton.frame)+4
                                            : CGRectGetMinX(self.menuButton.frame)-menuWidth-4;
    menuX = MAX(area.origin.x+4, MIN(menuX, CGRectGetMaxX(area)-menuWidth-4));
    self.toolbar.frame = CGRectMake(menuX, self.menuButton.frame.origin.y, menuWidth, 44);
    self.toolbar.hidden = self.editing || !self.menuOpen;
    self.toolbar.alpha = self.controlOpacity;
    [self.displayButton setTitle:@[@"Fit", @"Fill", @"Stretch"][BVNGuestPresentationMode()]
                       forState:UIControlStateNormal];
    for (UIButton* button in self.buttons) {
        NSDictionary* item = self.items[button.tag];
        CGFloat size = MAX(44, MIN(140, [item[@"size"] doubleValue]));
        size = MIN(size, MIN(area.size.width, area.size.height));
        CGFloat x = MAX(0, MIN(1, [item[@"x"] doubleValue]));
        CGFloat y = MAX(0, MIN(1, [item[@"y"] doubleValue]));
        button.frame = CGRectMake(area.origin.x + x * MAX(0, area.size.width-size),
                                 area.origin.y + y * MAX(0, area.size.height-size), size, size);
        button.alpha = self.editing ? 1 : self.controlOpacity;
        button.layer.borderColor = (self.editing && button.tag == self.selection
            ? UIColor.systemOrangeColor : UIColor.systemBlueColor).CGColor;
    }
    self.editor.frame = CGRectMake(CGRectGetMidX(area)-150, area.origin.y+4, 300, MIN(232, area.size.height-8));
}
- (void)drag:(UIPanGestureRecognizer*)gesture {
    if (!self.editing) return;
    UIButton* button = (UIButton*)gesture.view;
    self.selection = button.tag;
    CGRect area = UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets);
    CGPoint delta = [gesture translationInView:self]; [gesture setTranslation:CGPointZero inView:self];
    NSMutableDictionary* item = self.items[button.tag];
    item[@"x"] = @(MAX(0, MIN(1, [item[@"x"] doubleValue] + delta.x/MAX(1,area.size.width-button.bounds.size.width))));
    item[@"y"] = @(MAX(0, MIN(1, [item[@"y"] doubleValue] + delta.y/MAX(1,area.size.height-button.bounds.size.height))));
    [self refreshEditor]; [self setNeedsLayout];
    if (gesture.state == UIGestureRecognizerStateEnded || gesture.state == UIGestureRecognizerStateCancelled) [self save];
}
- (UIButton*)editorButton:(NSString*)title action:(SEL)action frame:(CGRect)frame {
    UIButton* button = [UIButton buttonWithType:UIButtonTypeSystem];
    [button setTitle:title forState:UIControlStateNormal]; button.frame = frame;
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    [self.editor addSubview:button]; return button;
}
- (void)editLayout {
    [self releaseControls]; self.editing = YES; self.menuOpen = NO;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(dimMenu) object:nil];
    if (!self.editor) {
        self.editor = [[UIView alloc] init]; self.editor.backgroundColor = [UIColor colorWithWhite:0.12 alpha:0.98];
        self.editor.layer.cornerRadius = 12; [self addSubview:self.editor];
        self.picker = [[UIPickerView alloc] initWithFrame:CGRectMake(0,0,190,90)];
        self.picker.dataSource = self; self.picker.delegate = self; [self.editor addSubview:self.picker];
        [self editorButton:@"Add" action:@selector(addControl) frame:CGRectMake(195,4,95,38)];
        [self editorButton:@"Done" action:@selector(doneEditing) frame:CGRectMake(195,44,95,38)];
        self.selectionLabel = [[UILabel alloc] initWithFrame:CGRectMake(10,90,190,24)];
        self.selectionLabel.font = [UIFont systemFontOfSize:12]; self.selectionLabel.textColor = UIColor.whiteColor;
        [self.editor addSubview:self.selectionLabel];
        [self editorButton:@"Delete" action:@selector(deleteControl) frame:CGRectMake(202,87,88,30)];
        for (int i=0;i<2;++i) {
            UILabel* label = [[UILabel alloc] initWithFrame:CGRectMake(10,120+i*42,85,30)];
            label.text = i ? @"All opacity" : @"Size"; label.font = [UIFont systemFontOfSize:12]; label.textColor = UIColor.whiteColor;
            [self.editor addSubview:label];
            UISlider* slider = [[UISlider alloc] initWithFrame:CGRectMake(95,120+i*42,190,30)];
            slider.minimumValue = i ? 0.1 : 44; slider.maximumValue = i ? 1 : 140;
            [slider addTarget:self action:@selector(sliderChanged:) forControlEvents:UIControlEventValueChanged];
            [self.editor addSubview:slider];
            if (i) self.opacitySlider=slider; else self.sizeSlider=slider;
        }
        UILabel* hint = [[UILabel alloc] initWithFrame:CGRectMake(10,201,280,24)];
        hint.text = @"Tap a control to select it. Drag to move."; hint.textColor = UIColor.lightGrayColor;
        hint.font = [UIFont systemFontOfSize:12]; [self.editor addSubview:hint];
    }
    self.editor.hidden = NO; [self rebuild]; [self refreshEditor];
}
- (void)refreshEditor {
    BOOL selected = self.selection >= 0 && self.selection < (NSInteger)self.items.count;
    self.selectionLabel.text = selected ? self.items[self.selection][@"key"] : @"Choose a key, then Add";
    self.sizeSlider.enabled = selected;
    self.sizeSlider.value = selected ? [self.items[self.selection][@"size"] floatValue] : 64;
    self.opacitySlider.value = self.controlOpacity;
}
- (void)addControl {
    if (self.items.count >= 32) return;
    [self.items addObject:[@{@"key":self.keys[[self.picker selectedRowInComponent:0]],
        @"x":@0.8,@"y":@0.75,@"size":@64} mutableCopy]];
    self.selection = self.items.count-1; [self rebuild]; [self refreshEditor]; [self save];
}
- (void)deleteControl {
    if (self.selection < 0 || self.selection >= (NSInteger)self.items.count) return;
    [self releaseControls]; [self.items removeObjectAtIndex:self.selection]; self.selection=-1;
    [self rebuild]; [self refreshEditor]; [self save];
}
- (void)sliderChanged:(UISlider*)slider {
    if (slider == self.opacitySlider) {
        [NSUserDefaults.standardUserDefaults setDouble:slider.value forKey:@"BoxedVN.controls.opacity"];
        if (self.opacityChanged) self.opacityChanged(slider.value);
    } else if (self.selection >= 0 && self.selection < (NSInteger)self.items.count) {
        self.items[self.selection][@"size"] = @(slider.value); [self save];
    }
    [self setNeedsLayout];
}
- (void)doneEditing { self.editing=NO; self.editor.hidden=YES; [self rebuild]; [self save]; [self wakeMenu]; }
- (NSInteger)numberOfComponentsInPickerView:(UIPickerView*)picker { return 1; }
- (NSInteger)pickerView:(UIPickerView*)picker numberOfRowsInComponent:(NSInteger)component { return self.keys.count; }
- (NSString*)pickerView:(UIPickerView*)picker titleForRow:(NSInteger)row forComponent:(NSInteger)component { return self.keys[row]; }
@end
