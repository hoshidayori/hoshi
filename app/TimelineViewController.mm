#import "TimelineViewController.h"
#import "Session.h"
#import "TimelineText.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace {

NSString* ToNs(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

// The same person always gets the same color for their name
UIColor* SenderColor(const std::string& userId) {
    static NSArray<UIColor*>* palette = @[
        UIColor.systemBlueColor, UIColor.systemPinkColor, UIColor.systemGreenColor, UIColor.systemOrangeColor,
        UIColor.systemPurpleColor, UIColor.systemTealColor, UIColor.systemIndigoColor, UIColor.systemBrownColor,
    ];
    return palette[std::hash<std::string>{}(userId) % palette.count];
}

NSString* TimeOf(std::int64_t originServerTs) {
    static NSDateFormatter* formatter = [] {
        NSDateFormatter* f = [[NSDateFormatter alloc] init];
        f.dateStyle = NSDateFormatterNoStyle;
        f.timeStyle = NSDateFormatterShortStyle;
        return f;
    }();
    return [formatter stringFromDate:[NSDate dateWithTimeIntervalSince1970:static_cast<double>(originServerTs) / 1000]];
}

// What identifies a message across updates: the transaction id for messages from this device, so the local
// echo and the event that comes back from the server are the same message
std::string KeyOf(const NHoshi::TMessage& message) {
    return message.TxnId.empty() ? "e:" + message.EventId : "t:" + message.TxnId;
}

} // namespace

@interface TimelineViewController () <UITextViewDelegate, UIGestureRecognizerDelegate>
@end

@implementation TimelineViewController {
    HSSession* _session;
    std::string _roomId;
    // The timeline is one text; _keys and _items are its messages in order, each item the text of one message
    HSTimelineTextView* _text;
    std::vector<std::string> _keys;
    NSArray<NSAttributedString*>* _items;
    std::vector<NSUInteger> _itemOffsets;  // where each item starts in the text
    NSDictionary<NSString*, NSAttributedString*>* _itemCache;  // the last update's items by what they depend on
    // The avatar of a series whose start, with its own avatar, scrolled off the top: a letter, or a picture over it
    UILabel* _sticky;
    UIImageView* _stickyImage;
    // Composer: Return inserts a line break, the button sends; it grows up to MaxInputLines, then scrolls
    UIView* _composer;
    UITextView* _input;
    UILabel* _placeholder;
    UIButton* _send;
    NSLayoutConstraint* _inputHeight;
    UIButton* _toBottom;
    BOOL _hasOlder;
    BOOL _loadingOlder;
    BOOL _sentSinceUpdate;
    NSString* _stressSplit;  // -HSStressHistory: where the last update's time went
}

static const CGFloat MaxInputLines = 6;

- (instancetype)initWithSession:(HSSession*)session roomId:(const std::string&)roomId {
    if ((self = [super initWithNibName:nil bundle:nil])) {
        _session = session;
        _roomId = roomId;
        _items = @[];
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;

    _text = [[HSTimelineTextView alloc] init];
    _text.delegate = self;
    _text.alwaysBounceVertical = YES;
    _text.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
    _text.translatesAutoresizingMaskIntoConstraints = NO;
    UITapGestureRecognizer* tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(textTapped:)];
    tap.cancelsTouchesInView = NO;
    tap.delegate = self;
    [_text addGestureRecognizer:tap];

    _sticky = [[UILabel alloc] initWithFrame:CGRectMake(HSBubbleSide, 0, HSAvatarSize, HSAvatarSize)];
    _sticky.layer.cornerRadius = HSAvatarSize / 2;
    _sticky.clipsToBounds = YES;
    _sticky.textAlignment = NSTextAlignmentCenter;
    _sticky.textColor = UIColor.whiteColor;
    _sticky.font = [UIFont systemFontOfSize:HSAvatarSize * 0.45 weight:UIFontWeightSemibold];
    _sticky.hidden = YES;
    _sticky.isAccessibilityElement = NO;  // the name in the text already says who it is
    _stickyImage = [[UIImageView alloc] initWithFrame:_sticky.bounds];
    _stickyImage.contentMode = UIViewContentModeScaleAspectFill;
    _stickyImage.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [_sticky addSubview:_stickyImage];
    [_text addSubview:_sticky];

    _composer = [[UIView alloc] init];
    _composer.translatesAutoresizingMaskIntoConstraints = NO;

    _input = [[UITextView alloc] init];
    _input.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _input.adjustsFontForContentSizeCategory = YES;
    _input.backgroundColor = UIColor.secondarySystemBackgroundColor;
    _input.layer.cornerRadius = 18;
    _input.textContainerInset = UIEdgeInsetsMake(8, 10, 8, 10);
    _input.scrollEnabled = NO;
    _input.delegate = self;
    _input.translatesAutoresizingMaskIntoConstraints = NO;

    _placeholder = [[UILabel alloc] init];
    _placeholder.text = @"Message";
    _placeholder.font = _input.font;
    _placeholder.textColor = UIColor.placeholderTextColor;
    _placeholder.translatesAutoresizingMaskIntoConstraints = NO;
    [_input addSubview:_placeholder];

    _send = [UIButton buttonWithType:UIButtonTypeSystem];
    [_send setImage:[UIImage systemImageNamed:@"arrow.up.circle.fill"
                            withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:30]]
           forState:UIControlStateNormal];
    _send.accessibilityLabel = @"Send";
    _send.enabled = NO;
    [_send addTarget:self action:@selector(sendTapped) forControlEvents:UIControlEventTouchUpInside];
    _send.translatesAutoresizingMaskIntoConstraints = NO;
    [_send setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];

    _toBottom = [UIButton buttonWithType:UIButtonTypeSystem];
    UIButtonConfiguration* toBottom = UIButtonConfiguration.filledButtonConfiguration;
    toBottom.image = [UIImage systemImageNamed:@"chevron.down"];
    toBottom.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    toBottom.baseBackgroundColor = UIColor.secondarySystemBackgroundColor;
    toBottom.baseForegroundColor = UIColor.labelColor;
    _toBottom.configuration = toBottom;
    _toBottom.accessibilityLabel = @"Jump to the latest message";
    _toBottom.layer.shadowColor = UIColor.blackColor.CGColor;
    _toBottom.layer.shadowOpacity = 0.15;
    _toBottom.layer.shadowRadius = 4;
    _toBottom.layer.shadowOffset = CGSizeMake(0, 1);
    _toBottom.hidden = YES;
    [_toBottom addTarget:self action:@selector(toBottomTapped) forControlEvents:UIControlEventTouchUpInside];
    _toBottom.translatesAutoresizingMaskIntoConstraints = NO;

    [self.view addSubview:_text];
    [self.view addSubview:_composer];
    [self.view addSubview:_toBottom];
    [_composer addSubview:_input];
    [_composer addSubview:_send];
    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    _inputHeight = [_input.heightAnchor constraintEqualToConstant:[self inputHeightForLines:1]];
    [NSLayoutConstraint activateConstraints:@[
        [_text.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [_text.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [_text.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [_composer.topAnchor constraintEqualToAnchor:_text.bottomAnchor],
        [_composer.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [_composer.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [_composer.bottomAnchor constraintEqualToAnchor:self.view.keyboardLayoutGuide.topAnchor],

        [_input.topAnchor constraintEqualToAnchor:_composer.topAnchor constant:8],
        [_input.bottomAnchor constraintEqualToAnchor:_composer.bottomAnchor constant:-8],
        [_input.leadingAnchor constraintEqualToAnchor:_composer.leadingAnchor constant:12],
        _inputHeight,
        [_send.leadingAnchor constraintEqualToAnchor:_input.trailingAnchor constant:6],
        [_send.trailingAnchor constraintEqualToAnchor:_composer.trailingAnchor constant:-10],
        [_send.bottomAnchor constraintEqualToAnchor:_input.bottomAnchor],
        [_send.heightAnchor constraintEqualToConstant:[self inputHeightForLines:1]],

        [_placeholder.leadingAnchor constraintEqualToAnchor:_input.leadingAnchor constant:15],
        [_placeholder.centerYAnchor constraintEqualToAnchor:_input.topAnchor constant:[self inputHeightForLines:1] / 2],

        [_toBottom.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
        [_toBottom.bottomAnchor constraintEqualToAnchor:_composer.topAnchor constant:-12],
        [_toBottom.widthAnchor constraintEqualToConstant:40],
        [_toBottom.heightAnchor constraintEqualToConstant:40],
    ]];

    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(stateChanged)
                                               name:HSSessionStateDidChange object:_session];
    // A picture that arrived replaces the letter in place, like any other change of a message
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(stateChanged)
                                               name:HSSessionAvatarsDidChange object:_session];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    // The first fill needs the final width, or the lines are laid out again and the scroll position is off
    if (_keys.empty()) {
        [self stateChanged];
    }
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (CGFloat)inputHeightForLines:(CGFloat)lines {
    return ceil(_input.font.lineHeight * lines + _input.textContainerInset.top + _input.textContainerInset.bottom);
}

- (void)textViewDidChange:(UITextView*)textView {
    if (textView != _input) {
        return;
    }
    _placeholder.hidden = textView.text.length > 0;
    NSString* trimmed = [textView.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    _send.enabled = trimmed.length > 0;

    // Grow with the text up to MaxInputLines, then let the text view scroll
    const CGFloat maxHeight = [self inputHeightForLines:MaxInputLines];
    const CGFloat fitting = [textView sizeThatFits:CGSizeMake(textView.bounds.size.width, CGFLOAT_MAX)].height;
    const CGFloat height = MAX([self inputHeightForLines:1], MIN(fitting, maxHeight));
    textView.scrollEnabled = fitting > maxHeight;
    if (_inputHeight.constant != height) {
        _inputHeight.constant = height;
        [self.view layoutIfNeeded];
    }
}

- (void)sendTapped {
    NSString* text = [_input.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (text.length == 0) {
        return;
    }
    _input.text = @"";
    [self textViewDidChange:_input];
    _sentSinceUpdate = YES;  // the local echo arrives with the next state change; scroll to it then
    // The message appears at once as pending; a failure keeps it in the timeline with a retry
    [_session sendText:text toRoom:_roomId];
}

- (void)toBottomTapped {
    [self scrollToBottomAnimated:YES];
}

#pragma mark - Building the text

- (NSAttributedString*)itemForMessage:(const NHoshi::TMessage&)message showSender:(BOOL)showSender
                                 showTime:(BOOL)showTime room:(const NHoshi::TRoom&)room {
    NSMutableAttributedString* item = [[NSMutableAttributedString alloc] init];
    const CGFloat indent = HSBubbleSide + HSAvatarColumn + HSBubblePadX;

    // The name only above the first of several messages in a row from the same person, on screen and in a copy
    if (showSender) {
        NSMutableParagraphStyle* style = [[NSMutableParagraphStyle alloc] init];
        style.firstLineHeadIndent = indent;
        style.headIndent = indent;
        style.paragraphSpacingBefore = 10 + HSBubblePadY;
        style.paragraphSpacing = HSBubblePadY + 2;
        const auto name = room.MemberNames.find(message.Sender);
        NSString* sender = ToNs(name == room.MemberNames.end() ? message.Sender : name->second);
        UIFont* font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
        font = [UIFont fontWithDescriptor:[font.fontDescriptor fontDescriptorWithSymbolicTraits:UIFontDescriptorTraitBold] size:0];
        // The avatar next to the name: for now the first letter of the name, without the "@" of a bare user id
        HSSenderMeta* avatar = [[HSSenderMeta alloc] init];
        NSString* letters = [sender hasPrefix:@"@"] && sender.length > 1 ? [sender substringFromIndex:1] : sender;
        avatar.initial = letters.length > 0
            ? [letters substringWithRange:[letters rangeOfComposedCharacterSequenceAtIndex:0]].localizedUppercaseString : @"?";
        avatar.color = SenderColor(message.Sender);
        if (const auto url = room.MemberAvatars.find(message.Sender); url != room.MemberAvatars.end()) {
            avatar.image = [_session avatarForMxc:url->second];
        }
        [item appendAttributedString:[[NSAttributedString alloc] initWithString:[sender stringByAppendingString:@"\n"] attributes:@{
            NSFontAttributeName: font,
            NSForegroundColorAttributeName: SenderColor(message.Sender),
            NSParagraphStyleAttributeName: style,
            HSSenderMetaAttribute: avatar,
        }]];
    }

    NSMutableParagraphStyle* style = [[NSMutableParagraphStyle alloc] init];
    style.firstLineHeadIndent = indent;
    style.headIndent = indent;
    style.tailIndent = -(HSBubbleSide + HSBubblePadX);
    style.paragraphSpacingBefore = showSender ? 0 : 2 * HSBubblePadY + 4;

    HSMessageMeta* meta = [[HSMessageMeta alloc] init];
    meta.time = showTime && message.OriginServerTs ? TimeOf(message.OriginServerTs) : @"";
    meta.txnId = ToNs(message.TxnId);
    meta.waitingForKey = message.IsWaitingForKey();
    meta.status = message.SendState == NHoshi::TMessage::ESendState::Sending ? HSMessageStatusSending
        : message.SendState == NHoshi::TMessage::ESendState::Failed ? HSMessageStatusFailed
        : HSMessageStatusSent;

    UIFont* font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    UIColor* color = UIColor.labelColor;
    NSString* body;
    if (meta.waitingForKey) {
        // The room key has not arrived (yet): the sender's device may still be sharing it, or never will
        body = @"Waiting for the key to decrypt this message";
        font = [UIFont italicSystemFontOfSize:font.pointSize];
        color = UIColor.secondaryLabelColor;
    } else {
        // One paragraph per message, so its bubble does not break at its own line breaks
        body = [[[ToNs(message.Body) stringByReplacingOccurrencesOfString:@"\r\n" withString:@" "]
            stringByReplacingOccurrencesOfString:@"\n" withString:@" "]
            stringByReplacingOccurrencesOfString:@"\r" withString:@" "];
        if (meta.status != HSMessageStatusSent) {
            color = UIColor.secondaryLabelColor;
        }
    }
    [item appendAttributedString:[[NSAttributedString alloc] initWithString:[body stringByAppendingString:@"\n"] attributes:@{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: color,
        NSParagraphStyleAttributeName: style,
        HSMessageMetaAttribute: meta,
    }]];
    return item;
}

// Puts the new items into the text. When the old messages are still there in the same order, only what
// changed is edited: older messages go in front, newer ones at the end, so a selection the user is making
// survives a new message. Returns how an old character offset maps to the new text.
- (std::function<NSUInteger(NSUInteger)>)applyItems:(NSArray<NSAttributedString*>*)items keys:(const std::vector<std::string>&)keys {
    NSTextStorage* storage = _text.textStorage;
    size_t prepended = 0;
    bool incremental = !_keys.empty() && keys.size() >= _keys.size();
    if (incremental) {
        const auto first = std::find(keys.begin(), keys.end(), _keys.front());
        prepended = static_cast<size_t>(first - keys.begin());
        incremental = first != keys.end() && prepended + _keys.size() <= keys.size() &&
            std::equal(_keys.begin(), _keys.end(), first);
    }
    if (!incremental) {
        NSMutableAttributedString* all = [[NSMutableAttributedString alloc] init];
        for (NSAttributedString* item in items) {
            [all appendAttributedString:item];
        }
        [storage setAttributedString:all];
        _keys = keys;
        _items = items;
        return [](NSUInteger) { return NSUInteger(0); };
    }

    struct TEdit {
        NSUInteger Location;  // in the old text
        NSUInteger OldLength;
        NSUInteger NewLength;
    };
    std::vector<TEdit> edits;
    std::vector<NSUInteger> offsets;
    NSUInteger offset = 0;
    for (NSAttributedString* item in _items) {
        offsets.push_back(offset);
        offset += item.length;
    }

    [storage beginEditing];
    NSMutableAttributedString* tail = [[NSMutableAttributedString alloc] init];
    for (size_t i = prepended + _keys.size(); i < keys.size(); ++i) {
        [tail appendAttributedString:items[i]];
    }
    [storage appendAttributedString:tail];
    // Backwards, so the offsets of the items not yet visited stay valid
    for (size_t i = _keys.size(); i-- > 0;) {
        NSAttributedString* now = items[prepended + i];
        if (![_items[i] isEqualToAttributedString:now]) {
            [storage replaceCharactersInRange:NSMakeRange(offsets[i], _items[i].length) withAttributedString:now];
            edits.push_back({offsets[i], _items[i].length, now.length});
        }
    }
    NSMutableAttributedString* head = [[NSMutableAttributedString alloc] init];
    for (size_t i = 0; i < prepended; ++i) {
        [head appendAttributedString:items[i]];
    }
    [storage insertAttributedString:head atIndex:0];
    [storage endEditing];

    _keys = keys;
    _items = items;
    const NSUInteger headLength = head.length;
    return [edits, headLength](NSUInteger old) {
        NSInteger moved = static_cast<NSInteger>(old + headLength);
        for (const auto& edit : edits) {
            if (edit.Location + edit.OldLength <= old) {
                moved += static_cast<NSInteger>(edit.NewLength) - static_cast<NSInteger>(edit.OldLength);
            } else if (edit.Location < old) {
                moved = static_cast<NSInteger>(edit.Location + headLength);  // inside a changed message: its start
            }
        }
        return static_cast<NSUInteger>(std::max<NSInteger>(moved, 0));
    };
}

#pragma mark - Positions

- (NSTextContentManager*)content {
    return _text.textLayoutManager.textContentManager;
}

- (NSUInteger)offsetOfLocation:(id<NSTextLocation>)location {
    return static_cast<NSUInteger>([[self content] offsetFromLocation:[self content].documentRange.location toLocation:location]);
}

- (nullable id<NSTextLocation>)locationOfOffset:(NSUInteger)offset {
    return [[self content] locationFromLocation:[self content].documentRange.location withOffset:static_cast<NSInteger>(offset)];
}

// Exact positions need the text above them laid out; TextKit 2 otherwise estimates the parts not yet seen.
// Laying out all of a long room took most of an update, so callers ask only for the part they measure.
- (void)layOutAll {
    [self layOutFrom:0 to:_text.textStorage.length];
}

- (void)layOutFrom:(NSUInteger)from to:(NSUInteger)to {
    const NSUInteger length = _text.textStorage.length;
    id<NSTextLocation> start = [self locationOfOffset:std::min(from, length)];
    id<NSTextLocation> end = [self locationOfOffset:std::min(std::max(from, to), length)];
    if (start && end) {
        [_text.textLayoutManager ensureLayoutForRange:[[NSTextRange alloc] initWithLocation:start endLocation:end]];
    }
    [_text layoutIfNeeded];
    // A few messages sit at the bottom, next to the composer, as in other messengers
    const CGFloat visible = _text.bounds.size.height - _text.safeAreaInsets.top - _text.safeAreaInsets.bottom;
    const CGFloat free = MAX(0, visible - _text.contentSize.height);
    if (_text.contentInset.top != free) {
        _text.contentInset = UIEdgeInsetsMake(free, 0, 0, 0);
    }
}

- (BOOL)isNearBottom {
    const CGFloat visibleBottom = _text.contentOffset.y + _text.bounds.size.height - _text.adjustedContentInset.bottom;
    return _text.contentSize.height - visibleBottom < 80;
}

- (void)scrollToBottomAnimated:(BOOL)animated {
    // The last messages, enough for a screen of them; what is above keeps its earlier layout or estimate
    const size_t tail = _itemOffsets.size() > 40 ? _itemOffsets.size() - 40 : 0;
    [self layOutFrom:_itemOffsets.empty() ? 0 : _itemOffsets[tail] to:_text.textStorage.length];
    const CGFloat bottom = _text.contentSize.height - _text.bounds.size.height + _text.adjustedContentInset.bottom;
    [_text setContentOffset:CGPointMake(0, MAX(-_text.adjustedContentInset.top, bottom)) animated:animated];
}

- (void)updateToBottomButton {
    const BOOL show = !_keys.empty() && ![self isNearBottom];
    if (_toBottom.hidden == !show) {
        return;
    }
    if (show) {
        _toBottom.alpha = 0;
        _toBottom.hidden = NO;
    }
    [UIView animateWithDuration:0.2 animations:^{
        self->_toBottom.alpha = show ? 1 : 0;
    } completion:^(BOOL) {
        self->_toBottom.hidden = !show;
    }];
}

#pragma mark - Updates

- (void)stateChanged {
    const auto& rooms = _session.state.Rooms;
    const auto it = rooms.find(_roomId);
    if (it == rooms.end() || !self.view.window) {
        return;
    }
    const CFAbsoluteTime started = CFAbsoluteTimeGetCurrent();
    const NHoshi::TRoom& room = it->second;
    self.title = ToNs(room.DisplayName(_session.userId.UTF8String));
    _hasOlder = !room.HistoryComplete;

    // Messages still being sent from this device come after everything the server has confirmed
    std::vector<NHoshi::TMessage> messages = room.Timeline;
    messages.insert(messages.end(), room.Pending.begin(), room.Pending.end());
    std::vector<std::string> keys;
    NSMutableArray<NSAttributedString*>* items = [NSMutableArray arrayWithCapacity:messages.size()];
    // An unchanged message reuses its text from the last update: building all of them each time took a
    // noticeable part of an update in a long room
    NSMutableDictionary<NSString*, NSAttributedString*>* cache = [NSMutableDictionary dictionaryWithCapacity:messages.size()];
    for (size_t i = 0; i < messages.size(); ++i) {
        const NHoshi::TMessage& message = messages[i];
        const bool showSender = i == 0 || messages[i - 1].Sender != message.Sender;
        // The time only where it changes, or a new series starts; a column of the same minute says nothing
        const bool showTime = showSender || messages[i - 1].OriginServerTs / 60000 != message.OriginServerTs / 60000;
        keys.push_back(KeyOf(message));
        // Everything the text of the message depends on
        std::string signature = keys.back() + "|" + std::to_string(showSender) + std::to_string(showTime) +
            std::to_string(static_cast<int>(message.SendState)) + std::to_string(message.IsWaitingForKey()) + "|" +
            std::to_string(message.OriginServerTs / 60000) + "|" + message.Body;
        if (showSender) {
            const auto name = room.MemberNames.find(message.Sender);
            const auto avatar = room.MemberAvatars.find(message.Sender);
            signature += "|" + (name == room.MemberNames.end() ? std::string() : name->second);
            if (avatar != room.MemberAvatars.end()) {
                signature += "|" + avatar->second + ([_session avatarForMxc:avatar->second] ? "+" : "-");
            }
        }
        NSString* cacheKey = [[NSString alloc] initWithBytes:signature.data() length:signature.size() encoding:NSUTF8StringEncoding]
            ?: @(keys.back().c_str());
        NSAttributedString* item = _itemCache[cacheKey] ?: [self itemForMessage:message showSender:showSender showTime:showTime room:room];
        cache[cacheKey] = item;
        [items addObject:item];
    }
    _itemCache = cache;

    const CFAbsoluteTime built = CFAbsoluteTimeGetCurrent();
    const bool first = _keys.empty();
    const bool atBottom = [self isNearBottom];
    // What the user is reading: the message at the top of the screen and how far below the top edge it is
    NSUInteger anchor = 0;
    CGFloat anchorShift = 0;
    if (!first) {
        const CGFloat top = _text.contentOffset.y + _text.adjustedContentInset.top - _text.textContainerInset.top;
        NSTextLayoutFragment* fragment = [_text.textLayoutManager textLayoutFragmentForPosition:CGPointMake(HSBubbleSide * 2, MAX(top, 0))];
        if (fragment) {
            anchor = [self offsetOfLocation:fragment.rangeInElement.location];
            anchorShift = fragment.layoutFragmentFrame.origin.y + _text.textContainerInset.top - _text.contentOffset.y;
        }
    }
    const NSRange selection = _text.selectedRange;

    const auto moved = [self applyItems:items keys:keys];
    const CFAbsoluteTime applied = CFAbsoluteTimeGetCurrent();
    _itemOffsets.clear();
    NSUInteger itemOffset = 0;
    for (NSAttributedString* item in _items) {
        _itemOffsets.push_back(itemOffset);
        itemOffset += item.length;
    }

    // A selection the user is making stays on the same text
    const NSUInteger length = _text.textStorage.length;
    if (!first && selection.length > 0 && selection.location != NSNotFound) {
        const NSUInteger start = std::min(moved(selection.location), length);
        const NSUInteger end = std::min(moved(selection.location + selection.length), length);
        if (end > start) {
            _text.selectedRange = NSMakeRange(start, end - start);
        }
    }

    if (first || _sentSinceUpdate) {
        _sentSinceUpdate = NO;
        [self scrollToBottomAnimated:NO];
        if (first) {
            [self selectFromLaunchArguments];
        }
    } else if (atBottom) {
        // New messages scroll into view only for someone already at the bottom, not for someone reading history
        [self scrollToBottomAnimated:YES];
    } else if (id<NSTextLocation> location = [self locationOfOffset:std::min(moved(anchor), length)]) {
        // Older messages went in front, or a message changed above: keep the one being read where it was.
        // Its position needs everything above it, and a screen below for what comes into view.
        [self layOutFrom:0 to:moved(anchor) + 4000];
        NSTextLayoutFragment* fragment = [_text.textLayoutManager textLayoutFragmentForLocation:location];
        if (fragment) {
            _text.contentOffset = CGPointMake(0, fragment.layoutFragmentFrame.origin.y + _text.textContainerInset.top - anchorShift);
        }
    }
    [self loadOlderIfNeeded];
    [self updateToBottomButton];
    [self updateStickyAvatar];
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"HSStressHistory"]) {
        _stressSplit = [NSString stringWithFormat:@"build %.1f, edit %.1f, layout and scroll %.1f",
            (built - started) * 1000, (applied - built) * 1000, (CFAbsoluteTimeGetCurrent() - applied) * 1000];
        [self stressHistoryAfterUpdateFrom:started anchor:first ? NSNotFound : moved(anchor)];
    }
}

// "-HSStressHistory YES": keeps scrolling to the top until the whole history is loaded, and writes to
// Documents/history.txt for every update the message count, the time it took, and whether the line that was
// at the top of the screen stayed there
- (void)stressHistoryAfterUpdateFrom:(CFAbsoluteTime)started anchor:(NSUInteger)anchor {
    static NSMutableString* report = [NSMutableString string];
    const double ms = (CFAbsoluteTimeGetCurrent() - started) * 1000;
    NSString* stayed = @"-";
    if (anchor != NSNotFound) {
        const CGFloat top = _text.contentOffset.y + _text.adjustedContentInset.top - _text.textContainerInset.top;
        NSTextLayoutFragment* fragment = [_text.textLayoutManager textLayoutFragmentForPosition:CGPointMake(HSBubbleSide * 2, MAX(top, 0))];
        const NSUInteger now = fragment ? [self offsetOfLocation:fragment.rangeInElement.location] : NSNotFound;
        stayed = now == anchor ? @"yes" : [NSString stringWithFormat:@"NO (%lu -> %lu)", static_cast<unsigned long>(anchor),
                                                                      static_cast<unsigned long>(now)];
    }
    [report appendFormat:@"messages %zu, update %.1f ms (%@), top line stayed %@, more %d\n", _keys.size(), ms, _stressSplit, stayed,
        _hasOlder];
    NSURL* documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    [report writeToURL:[documents URLByAppendingPathComponent:@"history.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    if (_hasOlder) {
        // Like a user who scrolled up to read: a little below the very top, so the anchor has something to hold
        __weak TimelineViewController* weakSelf = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
            TimelineViewController* strongSelf = weakSelf;
            if (strongSelf) {
                strongSelf->_text.contentOffset = CGPointMake(0, 100 - strongSelf->_text.adjustedContentInset.top);
                [strongSelf loadOlderIfNeeded];
            }
        });
    }
}

// For screenshots from sim-run.sh: "-HSSelectLast N" selects the last N characters of the timeline, so a
// selection across messages can be seen without touching the screen
- (void)selectFromLaunchArguments {
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"HSHitTest"]) {
        [self writeHitTest];
    }
    const NSInteger count = [NSUserDefaults.standardUserDefaults integerForKey:@"HSSelectLast"];
    const NSUInteger length = _text.textStorage.length;
    if (count <= 0 || length == 0) {
        return;
    }
    const NSUInteger selected = std::min<NSUInteger>(static_cast<NSUInteger>(count), length);
    [_text becomeFirstResponder];
    _text.selectedRange = NSMakeRange(length - selected, selected);
}

// "-HSHitTest YES": for the middle of every line, which character a finger there would reach, against the
// line's own characters; written to Documents/hittest.txt. A finger must land in the line it touches.
- (void)writeHitTest {
    [self layOutAll];
    NSMutableString* report = [NSMutableString string];
    [_text.textLayoutManager enumerateTextLayoutFragmentsFromLocation:_text.textLayoutManager.documentRange.location
        options:NSTextLayoutFragmentEnumerationOptionsEnsuresLayout usingBlock:^BOOL(NSTextLayoutFragment* fragment) {
            const NSUInteger start = [self offsetOfLocation:fragment.rangeInElement.location];
            for (NSTextLineFragment* line in fragment.textLineFragments) {
                const CGRect bounds = line.typographicBounds;
                const CGPoint point = CGPointMake(
                    fragment.layoutFragmentFrame.origin.x + CGRectGetMidX(bounds) + self->_text.textContainerInset.left,
                    fragment.layoutFragmentFrame.origin.y + CGRectGetMidY(bounds) + self->_text.textContainerInset.top);
                UITextPosition* hit = [self->_text closestPositionToPoint:point];
                const NSInteger offset = [self->_text offsetFromPosition:self->_text.beginningOfDocument toPosition:hit];
                const NSRange expected = NSMakeRange(start + static_cast<NSUInteger>(line.characterRange.location),
                                                     static_cast<NSUInteger>(line.characterRange.length));
                // The way TextKit 2's own selection gestures find a place: in text container coordinates
                const CGPoint inContainer = CGPointMake(point.x - self->_text.textContainerInset.left, point.y - self->_text.textContainerInset.top);
                NSTextLayoutManager* tlm = self->_text.textLayoutManager;
                NSArray<NSTextSelection*>* selections = [tlm.textSelectionNavigation textSelectionsInteractingAtPoint:inContainer
                    inContainerAtLocation:tlm.documentRange.location anchors:@[] modifiers:0 selecting:YES bounds:CGRectInfinite];
                NSInteger navigation = -1;
                if (id<NSTextLocation> location = selections.firstObject.textRanges.firstObject.location) {
                    navigation = static_cast<NSInteger>([self offsetOfLocation:location]);
                }
                NSInteger lineStart = -1;
                if (NSTextRange* range = [tlm lineFragmentRangeForPoint:inContainer inContainerAtLocation:tlm.documentRange.location]) {
                    lineStart = static_cast<NSInteger>([self offsetOfLocation:range.location]);
                }
                auto inLine = [&](NSInteger at) {
                    return at >= static_cast<NSInteger>(expected.location) && at <= static_cast<NSInteger>(NSMaxRange(expected));
                };
                const BOOL ok = inLine(offset) && inLine(navigation) && inLine(lineStart);
                [report appendFormat:@"%@ y=%.1f expected %lu..%lu closest %ld navigation %ld line %ld \"%@\"\n", ok ? @"ok  " : @"MISS",
                    point.y, static_cast<unsigned long>(expected.location), static_cast<unsigned long>(NSMaxRange(expected)),
                    static_cast<long>(offset), static_cast<long>(navigation), static_cast<long>(lineStart),
                    [line.attributedString.string substringWithRange:line.characterRange]];
                // The caret the handles hang on: it should cover the line, not the space above the paragraph
                const CGFloat lineTop = fragment.layoutFragmentFrame.origin.y + bounds.origin.y + self->_text.textContainerInset.top;
                for (NSUInteger at : {expected.location, NSMaxRange(expected) > expected.location ? NSMaxRange(expected) - 1 : expected.location}) {
                    UITextPosition* position = [self->_text positionFromPosition:self->_text.beginningOfDocument offset:static_cast<NSInteger>(at)];
                    const CGRect caret = position ? [self->_text caretRectForPosition:position] : CGRectNull;
                    [report appendFormat:@"     caret at %lu: y=%.1f h=%.1f; line y=%.1f h=%.1f\n", static_cast<unsigned long>(at),
                        caret.origin.y, caret.size.height, lineTop, bounds.size.height];
                }
                NSArray<UITextSelectionRect*>* rects = [self->_text selectionRectsForRange:[self->_text textRangeFromPosition:
                    [self->_text positionFromPosition:self->_text.beginningOfDocument offset:static_cast<NSInteger>(expected.location)]
                    toPosition:[self->_text positionFromPosition:self->_text.beginningOfDocument offset:static_cast<NSInteger>(NSMaxRange(expected))]]];
                for (UITextSelectionRect* rect in rects) {
                    [report appendFormat:@"     selection rect y=%.1f h=%.1f start=%d end=%d\n", rect.rect.origin.y, rect.rect.size.height,
                        rect.containsStart, rect.containsEnd];
                }
            }
            return YES;
        }];
    NSURL* documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    [report writeToURL:[documents URLByAppendingPathComponent:@"hittest.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

- (void)loadOlderIfNeeded {
    if (_loadingOlder || !_hasOlder) {
        return;
    }
    // Near the top, or the loaded messages do not even fill the screen
    const BOOL nearTop = _text.contentOffset.y + _text.adjustedContentInset.top < 300;
    if (!nearTop) {
        return;
    }
    _loadingOlder = YES;
    __weak TimelineViewController* weakSelf = self;
    [_session loadOlderInRoom:_roomId completion:^(BOOL more) {
        TimelineViewController* strongSelf = weakSelf;
        if (strongSelf) {
            strongSelf->_loadingOlder = NO;
            strongSelf->_hasOlder = more;
        }
    }];
}

- (void)scrollViewDidScroll:(UIScrollView*)scrollView {
    if (scrollView != _text) {
        return;  // the composer's text view scrolls too once it is full
    }
    [self loadOlderIfNeeded];
    [self updateToBottomButton];
    [self updateStickyAvatar];
}

- (nullable HSSenderMeta*)senderOfItem:(size_t)index {
    NSAttributedString* item = _items[index];
    return item.length > 0 ? [item attribute:HSSenderMetaAttribute atIndex:0 effectiveRange:nullptr] : nil;
}

// Top of the item's first paragraph, in the text view's content coordinates
- (CGFloat)topOfItem:(size_t)index {
    id<NSTextLocation> location = [self locationOfOffset:_itemOffsets[index]];
    NSTextLayoutFragment* fragment = location ? [_text.textLayoutManager textLayoutFragmentForLocation:location] : nil;
    return fragment ? fragment.layoutFragmentFrame.origin.y + _text.textContainerInset.top : 0;
}

// When the start of a series is above the screen, its avatar waits at the top edge while the series' messages
// are still visible, and the next series pushes it up and away
- (void)updateStickyAvatar {
    const CGFloat top = _text.contentOffset.y + _text.adjustedContentInset.top;
    NSTextLayoutFragment* fragment = _items.count == 0 ? nil
        : [_text.textLayoutManager textLayoutFragmentForPosition:CGPointMake(HSBubbleSide * 2, MAX(top - _text.textContainerInset.top, 0))];
    if (!fragment) {
        _sticky.hidden = YES;
        return;
    }
    const NSUInteger offset = [self offsetOfLocation:fragment.rangeInElement.location];
    const auto after = std::upper_bound(_itemOffsets.begin(), _itemOffsets.end(), offset);
    size_t start = after == _itemOffsets.begin() ? 0 : static_cast<size_t>(after - _itemOffsets.begin()) - 1;
    while (start > 0 && ![self senderOfItem:start]) {
        --start;
    }
    HSSenderMeta* sender = [self senderOfItem:start];
    size_t next = start + 1;
    while (next < _items.count && ![self senderOfItem:next]) {
        ++next;
    }
    // The name line with its own avatar is still on screen: no second one
    const CGFloat nameBottom = [self topOfItem:start] + 10 + HSBubblePadY + HSAvatarSize;
    if (!sender || nameBottom > top) {
        _sticky.hidden = YES;
        return;
    }
    const CGFloat seriesBottom = next < _items.count ? [self topOfItem:next] + 10 : _text.contentSize.height;
    const CGFloat y = MIN(top + 6, seriesBottom - HSAvatarSize);
    _sticky.text = sender.initial;
    _sticky.backgroundColor = sender.color;
    _stickyImage.image = sender.image;
    // A ring of the background color, so it lies cleanly over the times it passes
    _sticky.layer.borderWidth = 2;
    _sticky.layer.borderColor = [UIColor.systemBackgroundColor resolvedColorWithTraitCollection:self.traitCollection].CGColor;
    _sticky.frame = CGRectMake(HSBubbleSide, y, HSAvatarSize, HSAvatarSize);
    _sticky.hidden = NO;
    [_text bringSubviewToFront:_sticky];
}

#pragma mark - Retry

- (BOOL)gestureRecognizer:(UIGestureRecognizer*)gestureRecognizer
    shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer*)other {
    return YES;  // the text view's own selection gestures keep working
}

// A tap on the red mark next to a message that was not sent sends it again
- (void)textTapped:(UITapGestureRecognizer*)tap {
    NSString* txnId = [_text failedTxnIdAtPoint:[tap locationInView:_text]];
    if (txnId.length > 0) {
        [_session retryMessage:txnId.UTF8String inRoom:_roomId];
    }
}

@end
