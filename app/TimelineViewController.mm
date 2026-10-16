#import "TimelineViewController.h"
#import "Session.h"
#import "TimelineText.h"

#include <algorithm>
#include <functional>
#include <string_view>
#include <unordered_map>
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

// Whether the message has the key KeyOf gives it, without building the key
bool HasKey(const NHoshi::TMessage& message, const std::string& key) {
    if (key.size() < 2) {
        return false;
    }
    const std::string_view id = std::string_view(key).substr(2);
    return key[0] == 't' ? message.TxnId == id : message.TxnId.empty() && message.EventId == id;
}

// The window starts with this many of the newest messages, more than a screen of short ones, and grows or
// shrinks by WindowStep messages as the screen comes near its edges
constexpr size_t InitialWindow = 30;
constexpr NSInteger WindowStep = 10;

// Everything the text of a message depends on besides its key; strings as hashes, so that comparing the
// states of thousands of messages allocates nothing
struct TItemState {
    bool ShowSender = false;
    bool ShowTime = false;
    int SendState = 0;
    bool WaitingForKey = false;
    std::int64_t Minute = 0;
    size_t Body = 0;
    size_t Sender = 0;  // the shown name, when ShowSender
    size_t Avatar = 0;  // the avatar's mxc URI, when ShowSender and there is one
    bool HasImage = false;
    bool Loading = false;

    bool operator==(const TItemState&) const = default;
};

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
    std::vector<TItemState> _itemStates;  // what each of _items was built from
    // The window: _items are the room's messages from its first to its last key. _windowStart is the index of
    // the first one among all of the room's, _windowTotal their number.
    std::string _windowFirstKey;
    std::string _windowLastKey;
    BOOL _windowAtEnd;
    size_t _windowStart;
    size_t _windowTotal;
    // Asked by adjustWindow for the next update: messages to add or drop at each end
    NSInteger _growTop;
    NSInteger _growBottom;
    NSInteger _trimTop;
    NSInteger _trimBottom;
    BOOL _resetToEnd;  // the next update shows the newest messages
    BOOL _updating;    // inside stateChanged: its own scrolling must not start another update
    HSSenderMeta* _leadSender;  // the avatar of the series the window starts in, when its start is outside
    // The avatar of a series whose start, with its own avatar, scrolled off the top: a letter, or a picture over it
    UILabel* _sticky;
    UIImageView* _stickyImage;
    HSSenderMeta* _stickySender;  // what _sticky shows now
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
    double _textLayoutMs;    // within an update: TextKit's layout, and the text view's own layout after it
    double _viewLayoutMs;
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
    if (_windowAtEnd) {
        [self scrollToBottomAnimated:YES];
        return;
    }
    // The newest messages are not in the window: the window jumps there
    _resetToEnd = YES;
    [self stateChanged];
}

#pragma mark - Building the text

// The avatar next to a name: the member's picture when it is here, else the first letter of the name without
// the "@" of a bare user id, in the person's color
- (HSSenderMeta*)avatarOfSender:(const std::string&)userId room:(const NHoshi::TRoom&)room {
    const auto name = room.MemberNames.find(userId);
    NSString* sender = ToNs(name == room.MemberNames.end() ? userId : name->second);
    HSSenderMeta* avatar = [[HSSenderMeta alloc] init];
    NSString* letters = [sender hasPrefix:@"@"] && sender.length > 1 ? [sender substringFromIndex:1] : sender;
    avatar.initial = letters.length > 0
        ? [letters substringWithRange:[letters rangeOfComposedCharacterSequenceAtIndex:0]].localizedUppercaseString : @"?";
    avatar.color = SenderColor(userId);
    if (const auto url = room.MemberAvatars.find(userId); url != room.MemberAvatars.end()) {
        avatar.image = [_session avatarForMxc:url->second];
        avatar.loading = !avatar.image && [_session avatarIsLoading:url->second];
    }
    return avatar;
}

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
        HSSenderMeta* avatar = [self avatarOfSender:message.Sender room:room];
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

// Puts the new items into the text. The old and the new messages usually share a run in the same order: the
// window moved or grew, or messages came at an end. Then only the edges and the changed messages of the run
// are edited, so a selection the user is making survives. Returns how an old character offset maps to the new
// text; an offset in a part that went away maps to the nearest end of the shared run.
- (std::function<NSUInteger(NSUInteger)>)applyItems:(NSArray<NSAttributedString*>*)items keys:(const std::vector<std::string>&)keys {
    NSTextStorage* storage = _text.textStorage;
    // The shared run: old[from, from + count) == new[at, at + count)
    size_t from = 0;
    size_t at = 0;
    size_t count = 0;
    for (; from < _keys.size(); ++from) {
        const auto found = std::find(keys.begin(), keys.end(), _keys[from]);
        if (found != keys.end()) {
            at = static_cast<size_t>(found - keys.begin());
            break;
        }
    }
    while (from + count < _keys.size() && at + count < keys.size() && _keys[from + count] == keys[at + count]) {
        ++count;
    }
    if (count == 0) {
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
        NSUInteger Location;  // in the old text, from the start of the run
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
    offsets.push_back(offset);  // the end of the old text
    const NSUInteger runStart = offsets[from];
    const NSUInteger runEnd = offsets[from + count];

    [storage beginEditing];
    // From the end backwards, so the offsets not yet visited stay valid: the old messages after the run go,
    // the new ones after it come, the run's changed messages are replaced, then the same before the run
    NSMutableAttributedString* tail = [[NSMutableAttributedString alloc] init];
    for (size_t i = at + count; i < keys.size(); ++i) {
        [tail appendAttributedString:items[i]];
    }
    [storage replaceCharactersInRange:NSMakeRange(runEnd, storage.length - runEnd) withAttributedString:tail];
    for (size_t i = count; i-- > 0;) {
        NSAttributedString* was = _items[from + i];
        NSAttributedString* now = items[at + i];
        if (was != now && ![was isEqualToAttributedString:now]) {
            [storage replaceCharactersInRange:NSMakeRange(offsets[from + i], was.length) withAttributedString:now];
            edits.push_back({offsets[from + i] - runStart, was.length, now.length});
        }
    }
    NSMutableAttributedString* head = [[NSMutableAttributedString alloc] init];
    for (size_t i = 0; i < at; ++i) {
        [head appendAttributedString:items[i]];
    }
    [storage replaceCharactersInRange:NSMakeRange(0, runStart) withAttributedString:head];
    [storage endEditing];

    _keys = keys;
    _items = items;
    const NSUInteger headLength = head.length;
    NSUInteger runLength = runEnd - runStart;
    for (const auto& edit : edits) {
        runLength += edit.NewLength - edit.OldLength;
    }
    return [edits, headLength, runStart, runEnd, runLength](NSUInteger old) {
        if (old <= runStart) {
            return headLength;
        }
        if (old >= runEnd) {
            return headLength + runLength;
        }
        const NSUInteger inRun = old - runStart;
        NSInteger moved = static_cast<NSInteger>(inRun + headLength);
        for (const auto& edit : edits) {
            if (edit.Location + edit.OldLength <= inRun) {
                moved += static_cast<NSInteger>(edit.NewLength) - static_cast<NSInteger>(edit.OldLength);
            } else if (edit.Location < inRun) {
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
    const CFAbsoluteTime began = CFAbsoluteTimeGetCurrent();
    if (start && end) {
        [_text.textLayoutManager ensureLayoutForRange:[[NSTextRange alloc] initWithLocation:start endLocation:end]];
    }
    const CFAbsoluteTime laidOut = CFAbsoluteTimeGetCurrent();
    [_text layoutIfNeeded];
    _textLayoutMs += (laidOut - began) * 1000;
    _viewLayoutMs += (CFAbsoluteTimeGetCurrent() - laidOut) * 1000;
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
    const BOOL show = !_keys.empty() && (![self isNearBottom] || !_windowAtEnd);
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
    _updating = YES;
    const NHoshi::TRoom& room = it->second;
    self.title = ToNs(room.DisplayName(_session.userId.UTF8String));
    _hasOlder = !room.HistoryComplete;

    // Messages still being sent from this device come after everything the server has confirmed. Pointers,
    // not copies: a long room has thousands.
    std::vector<const NHoshi::TMessage*> messages;
    messages.reserve(room.Timeline.size() + room.Pending.size());
    for (const auto& message : room.Timeline) {
        messages.push_back(&message);
    }
    for (const auto& message : room.Pending) {
        messages.push_back(&message);
    }
    // The window: the messages in the text, a run of the room's messages around what is on screen. It follows
    // its first and last message across updates, then grows or shrinks as scrolling asked.
    const size_t total = messages.size();
    auto indexOf = [&](const std::string& key, size_t hint) -> size_t {
        // Usually where it was, or moved by a page of history that came in front: look there first
        for (size_t i = hint; i < std::min(total, hint + 200); ++i) {
            if (HasKey(*messages[i], key)) {
                return i;
            }
        }
        for (size_t i = total; i-- > 0;) {
            if (HasKey(*messages[i], key)) {
                return i;
            }
        }
        return total;
    };
    size_t start = total;
    size_t end = total;
    if (!_keys.empty() && !_resetToEnd) {
        start = indexOf(_windowFirstKey, _windowStart);
        end = _windowAtEnd ? total : std::min(total, indexOf(_windowLastKey, start) + 1);
    }
    if (start >= total || end <= start) {
        // First fill, a jump to the newest messages, or the window's messages are gone: the end of the room
        start = total > InitialWindow ? total - InitialWindow : 0;
        end = total;
    } else {
        start = std::min(start - std::min<size_t>(start, static_cast<size_t>(_growTop)) + static_cast<size_t>(_trimTop), end - 1);
        end = std::max(std::min(total, end + static_cast<size_t>(_growBottom)) - static_cast<size_t>(_trimBottom), start + 1);
    }
    _growTop = _growBottom = _trimTop = _trimBottom = 0;
    const bool jumpedToEnd = _resetToEnd;
    _resetToEnd = NO;
    _windowStart = start;

    std::vector<std::string> keys;
    keys.reserve(end - start);
    for (size_t i = start; i < end; ++i) {
        keys.push_back(KeyOf(*messages[i]));
    }
    // An unchanged message reuses its text from the last update. Unchanged means equal in everything its text
    // depends on, compared without building anything.
    std::unordered_map<std::string, size_t> previous;
    for (size_t i = 0; i < _keys.size(); ++i) {
        previous.emplace(_keys[i], i);
    }
    std::vector<TItemState> states;
    states.reserve(end - start);
    NSMutableArray<NSAttributedString*>* items = [NSMutableArray arrayWithCapacity:end - start];
    for (size_t i = start; i < end; ++i) {
        const NHoshi::TMessage& message = *messages[i];
        TItemState state;
        state.ShowSender = i == 0 || messages[i - 1]->Sender != message.Sender;
        // The time only where it changes, or a new series starts; a column of the same minute says nothing
        state.ShowTime = state.ShowSender || messages[i - 1]->OriginServerTs / 60000 != message.OriginServerTs / 60000;
        state.SendState = static_cast<int>(message.SendState);
        state.WaitingForKey = message.IsWaitingForKey();
        state.Minute = message.OriginServerTs / 60000;
        state.Body = std::hash<std::string>{}(message.Body);
        if (state.ShowSender) {
            const auto name = room.MemberNames.find(message.Sender);
            const auto avatar = room.MemberAvatars.find(message.Sender);
            state.Sender = std::hash<std::string>{}(name == room.MemberNames.end() ? message.Sender : name->second);
            if (avatar != room.MemberAvatars.end()) {
                state.Avatar = std::hash<std::string>{}(avatar->second);
                state.HasImage = [_session avatarForMxc:avatar->second] != nil;
                state.Loading = !state.HasImage && [_session avatarIsLoading:avatar->second];
            }
        }
        NSAttributedString* item = nil;
        if (const auto was = previous.find(keys[i - start]); was != previous.end() && _itemStates[was->second] == state) {
            item = _items[was->second];
        } else {
            item = [self itemForMessage:message showSender:state.ShowSender showTime:state.ShowTime room:room];
        }
        states.push_back(state);
        [items addObject:item];
    }
    _itemStates = std::move(states);

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

    const CFAbsoluteTime anchored = CFAbsoluteTimeGetCurrent();
    const bool wasAtEnd = _windowAtEnd;
    const auto moved = [self applyItems:items keys:keys];
    const CFAbsoluteTime applied = CFAbsoluteTimeGetCurrent();
    _windowFirstKey = keys.empty() ? std::string() : keys.front();
    _windowLastKey = keys.empty() ? std::string() : keys.back();
    _windowAtEnd = end == total;
    // The series the window starts in, when its start with the name and avatar is outside the window
    _leadSender = nil;
    if (start > 0 && start < total && messages[start - 1]->Sender == messages[start]->Sender) {
        _leadSender = [self avatarOfSender:messages[start]->Sender room:room];
    }
    _textLayoutMs = 0;
    _viewLayoutMs = 0;
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

    if (first || _sentSinceUpdate || jumpedToEnd) {
        _sentSinceUpdate = NO;
        [self scrollToBottomAnimated:NO];
        if (first) {
            [self selectFromLaunchArguments];
        }
    } else if (atBottom && wasAtEnd) {
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
    const CFAbsoluteTime scrolled = CFAbsoluteTimeGetCurrent();
    [self loadOlderIfNeeded];
    [self updateToBottomButton];
    [self updateStickyAvatar];
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"HSStressHistory"]) {
        const double rest = (scrolled - applied) * 1000 - _textLayoutMs - _viewLayoutMs;
        _stressSplit = [NSString stringWithFormat:@"build %.1f, anchor %.1f, edit %.1f, TextKit layout %.1f, view layout %.1f, "
            "offsets and scroll %.1f, sticky avatar %.1f",
            (built - started) * 1000, (anchored - built) * 1000, (applied - anchored) * 1000, _textLayoutMs, _viewLayoutMs, rest,
            (CFAbsoluteTimeGetCurrent() - scrolled) * 1000];
        [self stressHistoryAfterUpdateFrom:started anchor:first ? NSNotFound : moved(anchor)];
    }
    _windowTotal = total;
    _updating = NO;
    [self adjustWindow];
}

// Keeps the window about half a screen beyond each edge of the screen: grows it where the margin got thinner,
// and drops messages more than three quarters of a screen away, unless a selection may reach them. Runs an
// update when it changed the window, which comes back here; the depth stops a loop between the two.
- (void)adjustWindow {
    static int depth = 0;
    if (depth > 8 || _keys.empty()) {
        return;
    }
    const CGFloat screen = _text.bounds.size.height - _text.adjustedContentInset.top - _text.adjustedContentInset.bottom;
    const CGFloat visibleTop = _text.contentOffset.y + _text.adjustedContentInset.top;
    const CGFloat visibleBottom = visibleTop + screen;
    if (_windowStart > 0 && visibleTop < screen / 2) {
        _growTop = WindowStep;
    }
    if (!_windowAtEnd && _text.contentSize.height - visibleBottom < screen / 2) {
        _growBottom = WindowStep;
    }
    // A selection the user is making may still be dragged anywhere in the window: nothing goes then
    if (_text.selectedRange.length == 0 && _items.count > 1) {
        const CGFloat keepAbove = visibleTop - screen * 3 / 4;
        const CGFloat keepBelow = visibleBottom + screen * 3 / 4;
        // One lookup per edge: the message at that height, and all beyond it go
        size_t trimTop = 0;
        if (keepAbove > 0) {
            trimTop = [self itemAtY:keepAbove];
        }
        size_t trimBottom = 0;
        if (keepBelow < _text.contentSize.height) {
            trimBottom = _items.count - 1 - std::max([self itemAtY:keepBelow], trimTop);
        }
        _trimTop = _growTop ? 0 : static_cast<NSInteger>(trimTop);
        _trimBottom = _growBottom ? 0 : static_cast<NSInteger>(trimBottom);
    }
    if (_growTop || _growBottom || _trimTop || _trimBottom) {
        ++depth;
        [self stateChanged];
        --depth;
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
    [report appendFormat:@"messages %zu, window %zu from %zu, update %.1f ms (%@), top line stayed %@, more %d\n", _windowTotal,
        _keys.size(), _windowStart, ms, _stressSplit, stayed, _hasOlder];
    NSURL* documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    [report writeToURL:[documents URLByAppendingPathComponent:@"history.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    if (_hasOlder || _windowStart > 0) {
        // Like a user who scrolled up to read: a little below the very top, so the anchor has something to hold
        __weak TimelineViewController* weakSelf = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 10), dispatch_get_main_queue(), ^{
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
    // Older messages already here come in by growing the window; the server is asked only at the room's start
    if (_loadingOlder || !_hasOlder || _windowStart > 0) {
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
    if (!_updating) {
        [self adjustWindow];  // an update moves the offset itself, and adjusts the window at its end
    }
}

- (nullable HSSenderMeta*)senderOfItem:(size_t)index {
    NSAttributedString* item = _items[index];
    return item.length > 0 ? [item attribute:HSSenderMetaAttribute atIndex:0 effectiveRange:nullptr] : nil;
}

// The index in _items of the message at a height in content coordinates
- (size_t)itemAtY:(CGFloat)y {
    NSTextLayoutFragment* fragment = [_text.textLayoutManager textLayoutFragmentForPosition:
        CGPointMake(HSBubbleSide * 2, MAX(y - _text.textContainerInset.top, 0))];
    if (!fragment || _itemOffsets.empty()) {
        return 0;
    }
    const NSUInteger offset = [self offsetOfLocation:fragment.rangeInElement.location];
    const auto after = std::upper_bound(_itemOffsets.begin(), _itemOffsets.end(), offset);
    return after == _itemOffsets.begin() ? 0 : static_cast<size_t>(after - _itemOffsets.begin()) - 1;
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
    if (_items.count == 0 || _itemStates.size() != _items.count) {
        _sticky.hidden = YES;
        return;
    }
    // Where series start is known from the items' states; only the positions need TextKit
    size_t start = [self itemAtY:top];
    while (start > 0 && !_itemStates[start].ShowSender) {
        --start;
    }
    HSSenderMeta* sender = _itemStates[start].ShowSender ? [self senderOfItem:start] : nil;
    // The series began before the window: its name is not in the text at all
    const bool outside = !sender && start == 0;
    if (outside) {
        sender = _leadSender;
    }
    size_t next = start + 1;
    while (next < _items.count && !_itemStates[next].ShowSender) {
        ++next;
    }
    // The name line with its own avatar is still on screen: no second one
    const CGFloat nameBottom = outside ? -CGFLOAT_MAX : [self topOfItem:start] + 10 + HSBubblePadY + HSAvatarSize;
    if (!sender || nameBottom > top) {
        _sticky.hidden = YES;
        return;
    }
    const CGFloat seriesBottom = next < _items.count ? [self topOfItem:next] + 10 : _text.contentSize.height;
    const CGFloat y = MIN(top + 6, seriesBottom - HSAvatarSize);
    // Only what changed: this runs on every scroll step, and each property set is work for the render server
    if (_stickySender != sender) {
        _stickySender = sender;
        _sticky.text = sender.loading ? nil : sender.initial;
        _sticky.backgroundColor = sender.loading ? UIColor.tertiarySystemFillColor : sender.color;
        _stickyImage.image = sender.image;
        // A ring of the background color, so it lies cleanly over the times it passes
        _sticky.layer.borderWidth = 2;
        _sticky.layer.borderColor = [UIColor.systemBackgroundColor resolvedColorWithTraitCollection:self.traitCollection].CGColor;
    }
    const CGRect frame = CGRectMake(HSBubbleSide, y, HSAvatarSize, HSAvatarSize);
    if (!CGRectEqualToRect(_sticky.frame, frame)) {
        _sticky.frame = frame;
    }
    if (_sticky.hidden) {
        _sticky.hidden = NO;
        [_text bringSubviewToFront:_sticky];
    }
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
