#import "RoomsViewController.h"
#import "AppRouter.h"
#import "Session.h"
#import "TimelineViewController.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

NSString* ToNs(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

} // namespace

@implementation RoomsViewController {
    HSSession* _session;
    std::vector<std::string> _roomIds;
}

- (instancetype)initWithSession:(HSSession*)session {
    if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
        _session = session;
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Rooms";
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Sign out" style:UIBarButtonItemStylePlain
                                                                             target:self action:@selector(signOut)];
    [self.tableView registerClass:[UITableViewCell class] forCellReuseIdentifier:@"room"];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(stateChanged)
                                               name:HSSessionStateDidChange object:_session];
    [self stateChanged];
}

- (void)signOut {
    UIAlertController* confirm = [UIAlertController alertControllerWithTitle:@"Sign out?"
        message:@"This device will forget the account and stop receiving messages." preferredStyle:UIAlertControllerStyleAlert];
    [confirm addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [confirm addAction:[UIAlertAction actionWithTitle:@"Sign out" style:UIAlertActionStyleDestructive handler:^(UIAlertAction*) {
        [HSAppRouter.shared signOut];
    }]];
    [self presentViewController:confirm animated:YES completion:nil];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)stateChanged {
    _roomIds.clear();
    for (const auto& [id, room] : _session.state.Rooms) {
        _roomIds.push_back(id);
    }
    // The room with the newest message first, as in other messengers; rooms without messages go last
    const auto& rooms = _session.state.Rooms;
    auto lastActivity = [&rooms](const std::string& id) -> std::int64_t {
        const auto& timeline = rooms.at(id).Timeline;
        return timeline.empty() ? 0 : timeline.back().OriginServerTs;
    };
    std::stable_sort(_roomIds.begin(), _roomIds.end(), [&](const std::string& a, const std::string& b) {
        return lastActivity(a) > lastActivity(b);
    });
    [self.tableView reloadData];
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
    return static_cast<NSInteger>(_roomIds.size());
}

- (UITableViewCell*)tableView:(UITableView*)tableView cellForRowAtIndexPath:(NSIndexPath*)indexPath {
    UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:@"room" forIndexPath:indexPath];
    const auto& room = _session.state.Rooms.at(_roomIds[static_cast<size_t>(indexPath.row)]);
    UIListContentConfiguration* content = UIListContentConfiguration.subtitleCellConfiguration;
    content.text = room.Name.empty() ? ToNs(room.Id) : ToNs(room.Name);
    if (!room.Timeline.empty()) {
        const auto& last = room.Timeline.back();
        content.secondaryText = last.IsWaitingForKey() ? @"🔒 Encrypted message" : ToNs(last.Body);
    }
    if (room.Encrypted) {
        content.image = [UIImage systemImageNamed:@"lock.fill"];
        content.imageProperties.tintColor = UIColor.secondaryLabelColor;
    }
    content.secondaryTextProperties.numberOfLines = 1;
    cell.contentConfiguration = content;
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    TimelineViewController* timeline = [[TimelineViewController alloc] initWithSession:_session
                                                                                roomId:_roomIds[static_cast<size_t>(indexPath.row)]];
    [self.navigationController pushViewController:timeline animated:YES];
}

@end
