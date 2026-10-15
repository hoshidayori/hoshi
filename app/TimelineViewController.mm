#import "TimelineViewController.h"
#import "Session.h"

#include <vector>

namespace {

NSString* ToNs(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

} // namespace

@interface TimelineViewController () <UITableViewDataSource, UITableViewDelegate, UITextFieldDelegate>
@end

@implementation TimelineViewController {
    HSSession* _session;
    std::string _roomId;
    std::vector<NHoshi::TMessage> _messages;
    UITableView* _table;
    UITextField* _input;
    BOOL _hasOlder;
    BOOL _loadingOlder;
}

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

    _input = [[UITextField alloc] init];
    _input.placeholder = @"Message";
    _input.borderStyle = UITextBorderStyleRoundedRect;
    _input.returnKeyType = UIReturnKeySend;
    _input.delegate = self;
    _input.translatesAutoresizingMaskIntoConstraints = NO;

    [self.view addSubview:_table];
    [self.view addSubview:_input];
    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [_table.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [_table.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [_table.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [_input.topAnchor constraintEqualToAnchor:_table.bottomAnchor constant:8],
        [_input.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:12],
        [_input.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-12],
        [_input.bottomAnchor constraintEqualToAnchor:self.view.keyboardLayoutGuide.topAnchor constant:-8],
    ]];

    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(stateChanged)
                                               name:HSSessionStateDidChange object:_session];
    [self stateChanged];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
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
    self.title = room.Name.empty() ? ToNs(_roomId) : ToNs(room.Name);
    _hasOlder = !room.HistoryComplete;

    const bool first = _messages.empty();
    const bool atBottom = [self isNearBottom];
    // Older messages went in front if the message we showed first is now further down
    size_t prepended = 0;
    if (!first) {
        for (size_t i = 0; i < room.Timeline.size(); ++i) {
            if (room.Timeline[i].EventId == _messages.front().EventId) {
                prepended = i;
                break;
            }
        }
    }
    const CGFloat heightBefore = _table.contentSize.height;
    const CGFloat offsetBefore = _table.contentOffset.y;
    _messages = room.Timeline;
    [_table reloadData];

    if (first) {
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
    [self loadOlderIfNeeded];
}

- (BOOL)textFieldShouldReturn:(UITextField*)textField {
    NSString* text = [textField.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (text.length == 0) {
        return NO;
    }
    textField.text = @"";
    [_session sendText:text toRoom:_roomId completion:^(NSString* error) {
        if (error) {
            textField.text = text;  // keep what the user wrote so nothing is lost
            UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Not sent" message:error
                                                                    preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:alert animated:YES completion:nil];
        }
    }];
    return NO;
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

        [cell.contentView addSubview:sender];
        [cell.contentView addSubview:body];
        UILayoutGuide* margins = cell.contentView.layoutMarginsGuide;
        [NSLayoutConstraint activateConstraints:@[
            [sender.topAnchor constraintEqualToAnchor:margins.topAnchor],
            [sender.leadingAnchor constraintEqualToAnchor:margins.leadingAnchor constant:10],
            [sender.trailingAnchor constraintEqualToAnchor:margins.trailingAnchor],
            [body.topAnchor constraintEqualToAnchor:sender.bottomAnchor constant:2],
            [body.leadingAnchor constraintEqualToAnchor:margins.leadingAnchor],
            [body.trailingAnchor constraintLessThanOrEqualToAnchor:margins.trailingAnchor],
            [body.bottomAnchor constraintEqualToAnchor:margins.bottomAnchor],
        ]];
    }
    sender.text = ToNs(message.Sender);
    body.text = ToNs(message.Body);
    return cell;
}

@end
