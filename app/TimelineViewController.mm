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

- (NSAttributedString*)itemForMessage:(const NHoshi::TMessage&)message showSender:(BOOL)showSender room:(const NHoshi::TRoom&)room {
    NSMutableAttributedString* item = [[NSMutableAttributedString alloc] init];
    const CGFloat indent = HSBubbleSide + HSBubblePadX;

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
        [item appendAttributedString:[[NSAttributedString alloc] initWithString:[sender stringByAppendingString:@"\n"] attributes:@{
            NSFontAttributeName: font,
            NSForegroundColorAttributeName: SenderColor(message.Sender),
            NSParagraphStyleAttributeName: style,
        }]];
    }

    NSMutableParagraphStyle* style = [[NSMutableParagraphStyle alloc] init];
    style.firstLineHeadIndent = indent;
    style.headIndent = indent;
    style.tailIndent = -(HSBubbleSide + HSBubblePadX + HSTimeColumn);
    style.paragraphSpacingBefore = showSender ? 0 : 2 * HSBubblePadY + 4;

    HSMessageMeta* meta = [[HSMessageMeta alloc] init];
    meta.time = message.OriginServerTs ? TimeOf(message.OriginServerTs) : @"";
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

// Exact positions need the whole text laid out; TextKit 2 otherwise estimates the parts not yet seen
- (void)layOutAll {
    [_text.textLayoutManager ensureLayoutForRange:_text.textLayoutManager.documentRange];
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
    [self layOutAll];
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
    const NHoshi::TRoom& room = it->second;
    self.title = ToNs(room.DisplayName(_session.userId.UTF8String));
    _hasOlder = !room.HistoryComplete;

    // Messages still being sent from this device come after everything the server has confirmed
    std::vector<NHoshi::TMessage> messages = room.Timeline;
    messages.insert(messages.end(), room.Pending.begin(), room.Pending.end());
    std::vector<std::string> keys;
    NSMutableArray<NSAttributedString*>* items = [NSMutableArray arrayWithCapacity:messages.size()];
    for (size_t i = 0; i < messages.size(); ++i) {
        const bool showSender = i == 0 || messages[i - 1].Sender != messages[i].Sender;
        keys.push_back(KeyOf(messages[i]));
        [items addObject:[self itemForMessage:messages[i] showSender:showSender room:room]];
    }

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
        // Older messages went in front, or a message changed above: keep the one being read where it was
        [self layOutAll];
        NSTextLayoutFragment* fragment = [_text.textLayoutManager textLayoutFragmentForLocation:location];
        if (fragment) {
            _text.contentOffset = CGPointMake(0, fragment.layoutFragmentFrame.origin.y + _text.textContainerInset.top - anchorShift);
        }
    }
    [self loadOlderIfNeeded];
    [self updateToBottomButton];
}

// For screenshots from sim-run.sh: "-HSSelectLast N" selects the last N characters of the timeline, so a
// selection across messages can be seen without touching the screen
- (void)selectFromLaunchArguments {
    const NSInteger count = [NSUserDefaults.standardUserDefaults integerForKey:@"HSSelectLast"];
    const NSUInteger length = _text.textStorage.length;
    if (count <= 0 || length == 0) {
        return;
    }
    const NSUInteger selected = std::min<NSUInteger>(static_cast<NSUInteger>(count), length);
    [_text becomeFirstResponder];
    _text.selectedRange = NSMakeRange(length - selected, selected);
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
