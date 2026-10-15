#import "TimelineViewController.h"

#include "sync.h"
#include "json.h"

#include <vector>

namespace {

NSString* ToNs(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

// Until the network layer exists on iOS, the screen shows a room built from a /sync-shaped sample,
// so text selection can be tried on long, mixed and multi-line messages
const char* SampleSync = R"({
  "next_batch": "demo",
  "rooms": {"join": {"!demo:localhost": {
    "state": {"events": [{"type": "m.room.name", "state_key": "", "sender": "@yuki:localhost", "content": {"name": "Огоньки"}}]},
    "timeline": {"events": [
      {"type": "m.room.message", "sender": "@alice:localhost", "event_id": "$1", "origin_server_ts": 1792000000000,
       "content": {"msgtype": "m.text", "body": "Привет! Попробуй выделить только кусок этого сообщения: например, одно слово или вторую строчку."}},
      {"type": "m.room.message", "sender": "@bob:localhost", "event_id": "$2", "origin_server_ts": 1792000060000,
       "content": {"msgtype": "m.text", "body": "Команда для копирования:\ncmake -S . -B build && cmake --build build\nА это уже обычный текст после неё."}},
      {"type": "m.room.message", "sender": "@yuki:localhost", "event_id": "$3", "origin_server_ts": 1792000120000,
       "content": {"msgtype": "m.text", "body": "Ссылка https://github.com/hoshidayori/hoshi и смайлик 🌠 в середине строки."}},
      {"type": "m.room.message", "sender": "@alice:localhost", "event_id": "$4", "origin_server_ts": 1792000180000,
       "content": {"msgtype": "m.text", "body": "Пик Орионид — в ночь с 21 на 22 октября. Лучше смотреть за пару часов до рассвета."}}
    ]}
  }}}
})";

} // namespace

@implementation TimelineViewController {
    std::vector<NHoshi::TMessage> _messages;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    NHoshi::TSyncState state;
    state.Apply(NHoshi::TJson::Parse(SampleSync));
    const NHoshi::TRoom& room = state.Rooms.at("!demo:localhost");
    _messages = room.Timeline;
    self.title = ToNs(room.Name);

    self.tableView.separatorStyle = UITableViewCellSeparatorStyleNone;
    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 80;
    self.tableView.allowsSelection = NO;  // a tap belongs to the text, not to the row
    [self.tableView registerClass:[UITableViewCell class] forCellReuseIdentifier:@"message"];
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
