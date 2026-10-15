#import "TimelineViewController.h"
#import "Session.h"

#include <vector>

namespace {

NSString* ToNs(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

} // namespace

@interface TimelineViewController () <UITableViewDataSource, UITableViewDelegate, UITextViewDelegate>
@end

@implementation TimelineViewController {
    HSSession* _session;
    std::string _roomId;
    std::vector<NHoshi::TMessage> _messages;
    UITableView* _table;
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
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;

    _table = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
    _table.dataSource = self;
    _table.delegate = self;
    _table.separatorStyle = UITableViewCellSeparatorStyleNone;
    _table.rowHeight = UITableViewAutomaticDimension;
    _table.estimatedRowHeight = 80;
    _table.allowsSelection = NO;  // a tap belongs to the text, not to the row
    _table.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
    [_table registerClass:[UITableViewCell class] forCellReuseIdentifier:@"message"];
    _table.translatesAutoresizingMaskIntoConstraints = NO;

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

    [self.view addSubview:_table];
    [self.view addSubview:_composer];
    [self.view addSubview:_toBottom];
    [_composer addSubview:_input];
    [_composer addSubview:_send];
    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    _inputHeight = [_input.heightAnchor constraintEqualToConstant:[self inputHeightForLines:1]];
    [NSLayoutConstraint activateConstraints:@[
        [_table.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [_table.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [_table.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [_composer.topAnchor constraintEqualToAnchor:_table.bottomAnchor],
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
    [self stateChanged];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (CGFloat)inputHeightForLines:(CGFloat)lines {
    return ceil(_input.font.lineHeight * lines + _input.textContainerInset.top + _input.textContainerInset.bottom);
}

- (void)textViewDidChange:(UITextView*)textView {
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

- (void)updateToBottomButton {
    const BOOL show = !_messages.empty() && ![self isNearBottom];
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

- (BOOL)isNearBottom {
    const CGFloat visibleBottom = _table.contentOffset.y + _table.bounds.size.height - _table.adjustedContentInset.bottom;
    return _table.contentSize.height - visibleBottom < 80;
}

- (void)scrollToBottomAnimated:(BOOL)animated {
    if (!_messages.empty()) {
        [_table scrollToRowAtIndexPath:[NSIndexPath indexPathForRow:static_cast<NSInteger>(_messages.size()) - 1 inSection:0]
                      atScrollPosition:UITableViewScrollPositionBottom animated:animated];
    }
}

- (void)stateChanged {
    const auto& rooms = _session.state.Rooms;
    const auto it = rooms.find(_roomId);
    if (it == rooms.end()) {
        return;
    }
    const NHoshi::TRoom& room = it->second;
    self.title = ToNs(room.DisplayName(_session.userId.UTF8String));
    _hasOlder = !room.HistoryComplete;

    const bool first = _messages.empty();
    const bool atBottom = [self isNearBottom];
    // Older messages went in front if the message we showed first is now further down
    size_t prepended = 0;
    if (!first && !_messages.front().EventId.empty()) {
        for (size_t i = 0; i < room.Timeline.size(); ++i) {
            if (room.Timeline[i].EventId == _messages.front().EventId) {
                prepended = i;
                break;
            }
        }
    }
    const CGFloat heightBefore = _table.contentSize.height;
    const CGFloat offsetBefore = _table.contentOffset.y;
    // Messages still being sent from this device come after everything the server has confirmed
    _messages = room.Timeline;
    _messages.insert(_messages.end(), room.Pending.begin(), room.Pending.end());
    [_table reloadData];

    if (first || _sentSinceUpdate) {
        _sentSinceUpdate = NO;
        [_table layoutIfNeeded];
        [self scrollToBottomAnimated:NO];
    } else if (prepended > 0) {
        // Keep the message the user is reading at the same place on the screen
        [_table layoutIfNeeded];
        _table.contentOffset = CGPointMake(0, offsetBefore + _table.contentSize.height - heightBefore);
    } else if (atBottom) {
        // New messages scroll into view only for someone already at the bottom, not for someone reading history
        [self scrollToBottomAnimated:YES];
    }
    [self loadOlderIfNeeded];
    [self updateToBottomButton];
}

- (void)loadOlderIfNeeded {
    if (_loadingOlder || !_hasOlder) {
        return;
    }
    // Near the top, or the loaded messages do not even fill the screen
    const BOOL nearTop = _table.contentOffset.y + _table.adjustedContentInset.top < 300;
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
    if (scrollView != _table) {
        return;  // the composer's text view scrolls too once it is full
    }
    [self loadOlderIfNeeded];
    [self updateToBottomButton];
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
    return static_cast<NSInteger>(_messages.size());
}

- (UITableViewCell*)tableView:(UITableView*)tableView cellForRowAtIndexPath:(NSIndexPath*)indexPath {
    UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:@"message" forIndexPath:indexPath];
    const auto& message = _messages[static_cast<size_t>(indexPath.row)];

    UILabel* sender = [cell.contentView viewWithTag:1];
    UITextView* body = [cell.contentView viewWithTag:2];
    if (!body) {
        sender = [[UILabel alloc] init];
        sender.tag = 1;
        sender.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
        sender.textColor = UIColor.secondaryLabelColor;
        sender.translatesAutoresizingMaskIntoConstraints = NO;

        body = [[UITextView alloc] init];
        body.tag = 2;
        body.editable = NO;
        body.selectable = YES;
        body.scrollEnabled = NO;  // grows to its text so the row height follows
        body.dataDetectorTypes = UIDataDetectorTypeLink;
        body.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
        body.adjustsFontForContentSizeCategory = YES;
        body.backgroundColor = UIColor.secondarySystemBackgroundColor;
        body.layer.cornerRadius = 12;
        body.textContainerInset = UIEdgeInsetsMake(8, 10, 8, 10);
        body.translatesAutoresizingMaskIntoConstraints = NO;

        // "Sending…" or "Not sent — tap to retry" under a message from this device; empty, it takes no space
        UILabel* status = [[UILabel alloc] init];
        status.tag = 3;
        status.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption2];
        status.userInteractionEnabled = YES;
        [status addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(statusTapped:)]];
        status.translatesAutoresizingMaskIntoConstraints = NO;

        [cell.contentView addSubview:sender];
        [cell.contentView addSubview:body];
        [cell.contentView addSubview:status];
        UILayoutGuide* margins = cell.contentView.layoutMarginsGuide;
        [NSLayoutConstraint activateConstraints:@[
            [sender.topAnchor constraintEqualToAnchor:margins.topAnchor],
            [sender.leadingAnchor constraintEqualToAnchor:margins.leadingAnchor constant:10],
            [sender.trailingAnchor constraintEqualToAnchor:margins.trailingAnchor],
            [body.topAnchor constraintEqualToAnchor:sender.bottomAnchor constant:2],
            [body.leadingAnchor constraintEqualToAnchor:margins.leadingAnchor],
            [body.trailingAnchor constraintLessThanOrEqualToAnchor:margins.trailingAnchor],
            [status.topAnchor constraintEqualToAnchor:body.bottomAnchor constant:2],
            [status.leadingAnchor constraintEqualToAnchor:margins.leadingAnchor constant:10],
            [status.trailingAnchor constraintLessThanOrEqualToAnchor:margins.trailingAnchor],
            [status.bottomAnchor constraintEqualToAnchor:margins.bottomAnchor],
        ]];
    }
    UILabel* status = [cell.contentView viewWithTag:3];
    sender.text = ToNs(message.Sender);
    if (message.IsWaitingForKey()) {
        // The room key has not arrived (yet): the sender's device may still be sharing it, or never will
        body.text = @"🔒 Waiting for the key to decrypt this message";
        body.textColor = UIColor.secondaryLabelColor;
        body.font = [UIFont italicSystemFontOfSize:[UIFont preferredFontForTextStyle:UIFontTextStyleBody].pointSize];
    } else {
        body.text = ToNs(message.Body);
        body.textColor = UIColor.labelColor;
        body.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    }
    switch (message.SendState) {
        case NHoshi::TMessage::ESendState::Sent:
            body.alpha = 1;
            status.text = nil;
            break;
        case NHoshi::TMessage::ESendState::Sending:
            body.alpha = 0.5;
            status.text = @"Sending…";
            status.textColor = UIColor.secondaryLabelColor;
            break;
        case NHoshi::TMessage::ESendState::Failed:
            body.alpha = 0.5;
            status.text = @"Not sent — tap to retry";
            status.textColor = UIColor.systemRedColor;
            break;
    }
    return cell;
}

- (void)statusTapped:(UITapGestureRecognizer*)tap {
    UIView* view = tap.view;
    while (view && ![view isKindOfClass:[UITableViewCell class]]) {
        view = view.superview;
    }
    NSIndexPath* indexPath = view ? [_table indexPathForCell:(UITableViewCell*)view] : nil;
    if (!indexPath || indexPath.row >= static_cast<NSInteger>(_messages.size())) {
        return;
    }
    const auto& message = _messages[static_cast<size_t>(indexPath.row)];
    if (message.SendState == NHoshi::TMessage::ESendState::Failed) {
        [_session retryMessage:message.TxnId inRoom:_roomId];
    }
}

@end
